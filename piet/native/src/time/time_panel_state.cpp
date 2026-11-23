// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Time panel state.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/eval/result_order.h"  // orderRResults: tsset/xtset list r() as Stata does
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // BUG-540: stataMissingWithKind — `.a`-`.z` identity in matrix literals
#include "piet/eval/macros.h"
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/program.h"  // BUG-922: program::restorePreservesDeeperThan is defined here
#include "estimation/statistics_command_support.h"  // numericColumn / abbrevCp (tsreport)
#include "piet/dispatcher.h"  // tsreport: the if/in sample marked by generate/replace
#include "piet/serset.h"  // BUG-1461: serset::storageType, signed into `datasignature`
#include "piet/strmutate.h"  // BUG-532: needsStringCount, the string-condition test under by:
#include "piet/stata_format.h"  // eb-order T-P1: `matrix list ..., format()`
#include "piet/strutil.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path / env_utf8 (BUG-050/051)
#include "data/missing.h"   // Stata `.` / `.a`-`.z` kinds + stata_cmp (shared with the engine)
#include "core/stata_arith.h"  // `^` with StataBE's bits (frame.cpp's)

#if defined(__APPLE__)
#include <sys/clonefile.h>  // Q1 (gate ③): APFS copy-on-write snapshot for preserve
#endif  // BUG-1469: the vocabularies below sat inside it, undeclared off macOS

#include "engine/command_support.h"
#include "estimation/estimation_options.h"
#include "piet/handlers/observations.h"
#include "piet/handlers/time_panel.h"
#include "piet/handlers/variables.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace command_detail {}
using namespace command_detail;
using namespace session_vocab;

namespace command_detail {

// The display format a variable carries now: the one resolver
// (SessionState::displayFormat: the session's own, the file's, else the
// declared type's default).
std::string tsCurrentFormat(SessionState& session, Datastore* ds, const std::string& var) {
  if (ds != nullptr && ds->has(var)) {
    const std::vector<VarInfo> card = ds->formatCard(std::vector<std::string>{var});
    if (!card.empty()) return session.displayFormat(card.front());
  }
  VarInfo bare;
  bare.name = var;
  return session.displayFormat(bare);
}

// The panel variable as one comparable key per row: its value, or a string
// column's level code (bare `xtset` re-displays a panel-only declaration whose
// variable has since become string, see runXtsetPanelOnly); missing is NaN
// either way.
std::vector<double> tsPanelKey(Datastore* ds, const std::string& pvar) {
  const int64_t n = ds->nRows();
  std::vector<double> key(static_cast<size_t>(n));
  const cecon::Column& c = ds->col(pvar);
  if (ds->dtype(pvar) == "f64") {
    const double* v = c.f64();
    for (int64_t i = 0; i < n; ++i) key[static_cast<size_t>(i)] = v[i];
  } else {
    const int32_t* codes = c.codes();
    for (int64_t i = 0; i < n; ++i)
      key[static_cast<size_t>(i)] = codes[i] < 0 ? std::nan("") : static_cast<double>(codes[i]);
  }
  return key;
}

// BUG-1090: the values of the numeric `vars` in the order `sort vars` leaves the
// rows, worked out from the UNSORTED columns. The declaration paths used to read
// them back from the sorted frame, which on a resident head writes the whole
// frame to disk to look at one or two columns. This is exact only when every
// value is an ordinary number: the engine's stable ascending sort and this one
// then give the same sequence of values, since rows that tie carry identical
// values. A missing value (whose kinds the engine orders among themselves) or a
// negative zero gives nullopt, and the caller reads the sorted head as before.
std::optional<std::vector<std::vector<double>>> sortedPlainKeys(
    Datastore* ds, const std::vector<std::string>& vars) {
  const int64_t n = ds->nRows();
  std::vector<const double*> cols;
  for (const auto& v : vars) {
    if (ds->dtype(v) != "f64") return std::nullopt;
    const double* c = ds->col(v).f64();
    for (int64_t i = 0; i < n; ++i)
      if (std::isnan(c[i]) || (c[i] == 0.0 && std::signbit(c[i]))) return std::nullopt;
    cols.push_back(c);
  }
  std::vector<int64_t> order(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) order[static_cast<size_t>(i)] = i;
  std::stable_sort(order.begin(), order.end(), [&](int64_t a, int64_t b) {
    for (const double* c : cols)
      if (c[a] != c[b]) return c[a] < c[b];
    return false;
  });
  std::vector<std::vector<double>> out;
  for (const double* c : cols) {
    std::vector<double> sorted(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) sorted[static_cast<size_t>(i)] = c[order[static_cast<size_t>(i)]];
    out.push_back(std::move(sorted));
  }
  return out;
}

// What a format's time scale is called. Absent letters (%tC among them) have
// neither a period name nor a counted step: their delta is milliseconds.
struct TsScale {
  char letter;
  const char* periodName;  // "" = the scale has no period option of its own
  const char* stepWord;    // nullptr = delta counts milliseconds
};
constexpr TsScale kTsScales[] = {
    {'g', "generic", "unit"},   {'d', "daily", "day"},          {'b', "", "day"},
    {'w', "weekly", "week"},    {'m', "monthly", "month"},      {'q', "quarterly", "quarter"},
    {'h', "halfyearly", "halfyear"}, {'y', "yearly", "year"},   {'c', "clocktime", nullptr},
};

const TsScale* tsFindScale(char letter) {
  for (const TsScale& row : kTsScales)
    if (row.letter == letter) return &row;
  return nullptr;
}

// Read literally: %-ty and %-td count as generic, only %t? and the old %d name
// a scale.
char tsScaleLetter(const std::string& fmt) {
  if (fmt.size() > 2 && fmt[1] == 't') return fmt[2];
  return fmt.size() > 1 && fmt[1] == 'd' ? 'd' : 'g';
}

// "a yearly period", or "a period" when the scale has no name.
std::string tsPeriodPhrase(char letter) {
  const TsScale* scale = tsFindScale(letter);
  const std::string name = scale == nullptr ? std::string() : scale->periodName;
  return name.empty() ? "a period" : "a " + name + " period";
}

// Stata's default real-to-string, %9.0g without the blanks.
std::string tsPlainNumber(double v) { return strutil::trim(eval::applyStataFmt(v, "%9.0g")); }

std::string tsCounted(double count, const char* word, bool plural) {
  return tsPlainNumber(count) + " " + word + (plural ? "s" : "");
}

// A span of milliseconds as weeks, days, hours and minutes taken greedily, with
// whatever is left over written as (possibly fractional) seconds.
std::string tsClockSpan(double millis) {
  static constexpr struct {
    double size;
    const char* word;
  } kBlocks[] = {{604800000.0, "week"}, {86400000.0, "day"}, {3600000.0, "hour"}, {60000.0, "minute"}};
  std::vector<std::string> parts;
  double left = millis;
  for (const auto& block : kBlocks) {
    const double whole = std::floor(left / block.size);
    if (whole > 0) {
      parts.push_back(tsCounted(whole, block.word, whole > 1));
      left -= whole * block.size;
    }
  }
  if (left > 0) parts.push_back(tsCounted(left / 1000.0, "second", left != 1000.0));
  std::string text;
  for (const auto& part : parts) text += (text.empty() ? "" : " ") + part;
  return text;
}

// A first or last time as the report and r(tmins)/r(tmaxs) show it. The clocktime
// period option, alone among the ways to reach %tc, shows milliseconds as well
// (Stata displays its edges in %tcDDmonCCYY_HH:MM:SS.sss there); r(tsfmt) stays %tc.
std::string tsTimeText(double t, const std::string& fmt, bool showMillis) {
  if (std::isnan(t)) return ".";
  // The old-style bare %d shows what %td shows; the shared formatter knows only the latter.
  std::string text = strutil::trim(eval::applyStataFmt(t, fmt == "%d" ? std::string("%td") : fmt));
  if (showMillis) {
    char field[8];
    std::snprintf(field, sizeof field, ".%03d", static_cast<int>(t - std::floor(t / 1000.0) * 1000.0));
    text += field;
  }
  return text;
}

// The r() face of a value that may not exist.
double tsPostable(double v) { return std::isnan(v) ? eval::kStataMissingSentinel : v; }

// Smallest and largest nonmissing value, or (NaN, NaN). With `gate`, only rows
// whose gate value is nonmissing count.
std::pair<double, double> tsSpan(const double* values, const double* gate, int64_t n) {
  double low = std::nan(""), high = std::nan("");
  for (int64_t row = 0; row < n; ++row) {
    if (std::isnan(values[row]) || (gate != nullptr && std::isnan(gate[row]))) continue;
    if (std::isnan(low) || values[row] < low) low = values[row];
    if (std::isnan(high) || values[row] > high) high = values[row];
  }
  return {low, high};
}

// Visits each block of rows sharing a key in key-sorted data: every missing key
// falls in one block, and no key column at all means one block of everything.
template <typename Visit>
void tsForEachBlock(const double* keys, int64_t n, Visit&& visit) {
  int64_t start = 0;
  for (int64_t row = 1; row <= n; ++row) {
    const bool continues =
        row < n && (keys == nullptr || keys[row] == keys[row - 1] ||
                    (std::isnan(keys[row]) && std::isnan(keys[row - 1])));
    if (continues) continue;
    visit(start, row);
    start = row;
  }
}

bool tsBlocksEqualSized(const double* keys, int64_t n) {
  int64_t size = -1;
  bool equal = true;
  tsForEachBlock(keys, n, [&](int64_t from, int64_t to) {
    if (size >= 0 && to - from != size) equal = false;
    size = to - from;
  });
  return equal;
}

// [XT] xtset's balance words, stated as sets of rows rather than per panel: any
// missing time leaves at best "weakly" (when every time is missing) and otherwise
// "unbalanced"; panels of unequal size are "unbalanced"; and "strongly" needs each
// time value on as many rows as there are panels with a nonmissing id, so rows
// with a missing id count against it (ids 1 2 . at times 1 2 each are "weakly").
const char* tsBalanceWord(const double* ids, const double* times, int64_t n) {
  const int64_t blankTimes = std::count_if(times, times + n, [](double t) { return std::isnan(t); });
  if (blankTimes > 0) return blankTimes == n ? "weakly balanced" : "unbalanced";
  if (!tsBlocksEqualSized(ids, n)) return "unbalanced";
  int64_t realPanels = 0;
  tsForEachBlock(ids, n, [&](int64_t from, int64_t) {
    if (!std::isnan(ids[from])) ++realPanels;
  });
  std::vector<double> ordered(times, times + n);
  std::sort(ordered.begin(), ordered.end());
  bool eachOnEveryPanel = true;
  tsForEachBlock(ordered.data(), n, [&](int64_t from, int64_t to) {
    if (to - from != realPanels) eachOnEveryPanel = false;
  });
  return eachOnEveryPanel ? "strongly balanced" : "weakly balanced";
}

// Consecutive nonmissing times within each panel, compared with delta. Exact on
// doubles: the times are whole numbers by the time this runs.
struct TsStepTally {
  int64_t offBeat = 0;  // steps that are not exactly delta
  bool repeated = false;
  bool belowDelta = false;
};

TsStepTally tsTallySteps(const double* ids, const double* times, int64_t n, double delta) {
  TsStepTally tally;
  tsForEachBlock(ids, n, [&](int64_t from, int64_t to) {
    double previous = std::nan("");
    for (int64_t row = from; row < to; ++row) {
      if (std::isnan(times[row])) continue;
      if (!std::isnan(previous)) {
        const double step = times[row] - previous;
        tally.repeated = tally.repeated || step == 0;
        tally.belowDelta = tally.belowDelta || step < delta;
        if (step != delta) ++tally.offBeat;
      }
      previous = times[row];
    }
  });
  return tally;
}

// The named variable must exist and be numeric. Reachable through a re-display
// whose variable has since been dropped or made string.
std::optional<ojson> tsUnusableKey(Datastore* ds, const std::string& var) {
  if (!ds->has(var)) return engineErrorBlockRc("variable " + var + " not found", 111);
  if (ds->dtype(var) != "f64")
    return engineErrorBlockRc(
        "string variables not allowed in varlist;\n" + var + " is a string variable", 109);
  return std::nullopt;
}

std::optional<ojson> tsBadFormatOption(const TsDeclare& d) {
  if (d.format.empty()) return std::nullopt;
  if (!d.period.empty())
    return engineErrorBlockRc("may not specify both format() and " + d.period, 198);
  if (isValidDisplayFormat(d.format)) return std::nullopt;
  if (d.format.front() == '%') return engineErrorBlockRc("invalid %format", 120);
  return engineErrorBlockRc(d.format + " not found", 111);
}

// tsset.ado's delta(): terms `# [unit]` joined by blanks or `+`, each unit worded for the time
// variable's scale (%tc in milliseconds; days or weeks for %td; one word for the other
// scales). Returns the delta in the variable's own steps.
std::optional<ojson> tsParseDeltaText(const std::string& text, char unit, double* out) {
  auto generic = [&]() {
    return engineErrorBlockRc("invalid argument in delta(): " + text, 198);
  };
  auto wrongUnit = [&]() {
    return engineErrorBlockRc(std::string("option delta(") + text + ") invalid for %t" + unit +
                                  " time variable",
                              198);
  };
  std::vector<std::string> t;
  {
    std::string cur;
    auto flush = [&]() {
      if (!cur.empty()) t.push_back(cur);
      cur.clear();
    };
    for (char c : text) {
      if (c == ' ' || c == '\t') flush();
      else if (c == '+' || c == '(' || c == ')') {
        flush();
        t.emplace_back(1, c);
      } else cur += c;
    }
    flush();
  }
  if (t.empty()) {
    *out = 1.0;
    return std::nullopt;
  }
  double d = 0.0;
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i] == "+") continue;
    if (t[i] == "(" || t[i] == ")") return generic();
    char* end = nullptr;
    double toadd = std::strtod(t[i].c_str(), &end);
    if (end == t[i].c_str() || *end != '\0' || std::isnan(toadd)) return generic();
    int cas = 0;
    const std::string s = i + 1 < t.size() ? t[i + 1] : std::string();
    if (s == "seconds" || s == "secs" || s == "second" || s == "sec") cas = 1;
    else if (s == "minutes" || s == "mins" || s == "minute" || s == "min") cas = 2;
    else if (s == "hours" || s == "hour") cas = 3;
    else if (s == "days" || s == "day") cas = 4;
    else if (s == "weeks" || s == "week") cas = 5;
    else if (s == "months" || s == "month") cas = 6;
    else if (s == "quarters" || s == "quarter") cas = 7;
    else if (s == "halfyears" || s == "halfyear") cas = 8;
    else if (s == "years" || s == "year") cas = 9;
    else if (!s.empty()) return generic();
    if (cas != 0) {
      if (unit == 'c' || unit == 'C') {
        static const double kMult[] = {0, 1000.0, 60000.0, 3600000.0, 86400000.0, 604800000.0};
        if (cas > 5) return wrongUnit();
        toadd *= kMult[cas];
      } else if (unit == 'd') {
        // only `days` is valid here: a week count is scaled by 7 and then refused all
        // the same, so `delta(1 week)` on a %td variable is the unit error
        if (cas != 4) return wrongUnit();
      } else if ((unit == 'w' && cas != 5) || (unit == 'm' && cas != 6) ||
                 (unit == 'q' && cas != 7) || (unit == 'h' && cas != 8) ||
                 (unit == 'y' && cas != 9)) {
        return wrongUnit();
      } else if (unit != 'w' && unit != 'm' && unit != 'q' && unit != 'h' && unit != 'y') {
        return wrongUnit();
      }
      ++i;
    }
    d += toadd;
  }
  *out = d;
  return std::nullopt;
}

std::optional<ojson> tsBadDelta(double delta) {
  std::string complaint;
  if (delta == 0) complaint = "zero";
  else if (delta < 0) complaint = "a negative number";
  else if (delta != std::floor(delta)) complaint = "a noninteger value";
  if (complaint.empty()) return std::nullopt;
  return engineErrorBlockRc("delta(" + tsPlainNumber(delta) + ") evaluates to " + complaint, 198);
}

// The display format a declaration leaves on the time variable.
std::string tsChooseFormat(const TsDeclare& d, const std::string& held) {
  if (!d.format.empty()) return d.format;
  if (d.period.empty()) return held;
  const char wanted = d.period.front();
  // clocktime always resets to plain %tc, even from a detailed %tc mask.
  if (wanted != 'c' && tsScaleLetter(held) == wanted) return held;
  return std::string("%t") + wanted;
}

// r(unit1) is the chosen format's third character; r(unit) is the period typed, or
// else what that character spells. Only the names listed here are posted: a %9.0g
// time variable has r(unit1) "." and no r(unit). (Named difference from StataBE,
// which leaves stray text in r(unit) for a third character of 0 or 1.)
std::string tsUnitWord(const std::string& period, const std::string& fmt) {
  if (!period.empty()) return period;
  if (fmt.size() < 3) return "generic";
  const TsScale* scale = tsFindScale(fmt[2]);
  return scale == nullptr ? std::string() : scale->periodName;
}

ojson tsEchoBlock(const std::string& text) {
  ojson block;
  block["type"] = "echo";
  block["text"] = text;
  return block;
}

void tsPostScalar(SessionState& session, std::vector<std::string>& posted, const char* name,
                  double value) {
  session.eval.rValues.set(name, value);
  posted.emplace_back(name);
}

void tsPostText(SessionState& session, std::vector<std::string>& posted, const char* name,
                const std::string& value) {
  session.eval.rStrings.set(name, value);
  posted.emplace_back(name);
}

}  // namespace

// Exported (handlers.h) for xtdescribe's `Delta(t) = ...` line as well.
std::string tsDeltaText(double delta, const std::string& fmt) {
  const TsScale* scale = tsFindScale(tsScaleLetter(fmt));
  if (scale == nullptr || scale->stepWord == nullptr) return tsClockSpan(delta);
  return tsCounted(delta, scale->stepWord, delta > 1);
}

ojson runTsDeclare(const TsDeclare& d, SessionState& session, const std::string& rCmd) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();

  std::vector<std::string> sortKeys;
  if (d.panelvar.has_value()) sortKeys.push_back(*d.panelvar);
  sortKeys.push_back(d.timevar);
  for (const std::string& key : sortKeys)
    if (auto refusal = tsUnusableKey(ds, key)) return *refusal;
  if (auto refusal = tsBadFormatOption(d)) return *refusal;
  {
    const double* raw = ds->col(d.timevar).f64();
    const bool allWhole = std::all_of(raw, raw + ds->nRows(), [](double t) {
      return std::isnan(t) || t == std::floor(t);
    });
    if (!allWhole) return engineErrorBlockRc("time variable must contain only integer values", 451);
  }

  // From here on Stata's side effects survive a later refusal: the sort and the
  // format stay, only the declaration itself waits for every check to pass.
  const std::string heldFormat = tsCurrentFormat(session, ds, d.timevar);
  // BUG-1090: work the key columns out before sorting, so that plain data never
  // has the sorted frame materialised just to read two columns of it. `ds` is not
  // used again past the sort, which may replace the head.
  std::optional<std::vector<std::vector<double>>> keyColumns = sortedPlainKeys(ds, sortKeys);
  ojson sortAnswer = runSort(sortKeys, session);
  if (sortAnswer.is_object() && sortAnswer.value("type", std::string()) == "error") return sortAnswer;
  if (!keyColumns.has_value()) {
    Datastore* sortedHead = session.datastore();
    if (sortedHead == nullptr) return eval::noDataLoaded();
    keyColumns.emplace();
    for (const std::string& key : sortKeys) keyColumns->push_back(tsPanelKey(sortedHead, key));
  }
  const double* ids = d.panelvar.has_value() ? keyColumns->front().data() : nullptr;
  const double* times = keyColumns->back().data();
  const int64_t rows = static_cast<int64_t>(keyColumns->back().size());

  const std::string chosenFormat = tsChooseFormat(d, heldFormat);
  std::string warning;
  if (chosenFormat != heldFormat) {
    session.varMeta[d.timevar].format = chosenFormat;
    session.userFormattedVars.insert(d.timevar);
    const char before = tsScaleLetter(heldFormat), after = tsScaleLetter(chosenFormat);
    if (before != after && before != 'g') {
      warning = "warning: Variable " + d.timevar + " had been formatted " + heldFormat + " (" +
                tsPeriodPhrase(before) + "), and you asked for " + tsPeriodPhrase(after) +
                ". Are you sure that is what you want? Format has been changed. " + d.timevar +
                " is now formatted " + chosenFormat + ".\n";
    }
  }

  double delta = d.delta.value_or(1.0);
  if (!d.deltaText.empty()) {
    if (auto refusal = tsParseDeltaText(d.deltaText, tsScaleLetter(chosenFormat), &delta))
      return *refusal;
  }
  if (auto refusal = tsBadDelta(delta)) return *refusal;
  const TsStepTally steps = tsTallySteps(ids, times, rows, delta);
  if (steps.repeated) {
    return engineErrorBlockRc(ids != nullptr ? "repeated time values within panel"
                                             : "repeated time values in sample",
                              451);
  }
  if (steps.belowDelta && !d.force)
    return engineErrorBlockRc("time values with period less than delta() found", 451);

  session.panelvar = d.panelvar;
  session.timevar = d.timevar;
  session.delta = d.deltaText.empty() ? d.delta : std::optional<double>(delta);
  session.writeTsChars();

  // Rows with a missing panel id count for gaps, repeats and balance, not edges.
  const auto [firstTime, lastTime] = tsSpan(times, ids, rows);
  const bool clockOption = d.period == "clocktime";
  const std::string firstText = tsTimeText(firstTime, chosenFormat, clockOption);
  const std::string lastText = tsTimeText(lastTime, chosenFormat, clockOption);
  const std::string stepPhrase = tsDeltaText(delta, chosenFormat);
  const std::string balanceWord = ids != nullptr ? tsBalanceWord(ids, times, rows) : "";

  std::vector<std::string> posted;
  if (d.noQuery) {
    // noquery: tsset.ado runs no Query, so only the delta, the format and the names come back.
    tsPostScalar(session, posted, "tdelta", delta);
    tsPostText(session, posted, "timevar", d.timevar);
    tsPostText(session, posted, "tsfmt", chosenFormat);
    if (chosenFormat.size() > 2) tsPostText(session, posted, "unit1", chosenFormat.substr(2, 1));
    const std::string unitWord0 = tsUnitWord(d.period, chosenFormat);
    if (!unitWord0.empty()) tsPostText(session, posted, "unit", unitWord0);
    if (ids != nullptr) tsPostText(session, posted, "panelvar", *d.panelvar);
    eval::orderRResults(session.eval, {"tdelta", "panelvar", "timevar", "unit", "unit1", "tsfmt"});
    eval::declareRPosted(session.eval, rCmd, std::move(posted));
    return ojson::array();
  }
  tsPostScalar(session, posted, "tdelta", delta);
  tsPostScalar(session, posted, "tmin", tsPostable(firstTime));
  tsPostScalar(session, posted, "tmax", tsPostable(lastTime));
  tsPostScalar(session, posted, "gaps", steps.offBeat > 0 ? 1.0 : 0.0);
  tsPostText(session, posted, "timevar", d.timevar);
  tsPostText(session, posted, "tsfmt", chosenFormat);
  tsPostText(session, posted, "tdeltas", stepPhrase);
  tsPostText(session, posted, "tmins", firstText);
  tsPostText(session, posted, "tmaxs", lastText);
  if (chosenFormat.size() > 2) tsPostText(session, posted, "unit1", chosenFormat.substr(2, 1));
  const std::string unitWord = tsUnitWord(d.period, chosenFormat);
  if (!unitWord.empty()) tsPostText(session, posted, "unit", unitWord);
  if (ids != nullptr) {
    const auto [lowId, highId] = tsSpan(ids, nullptr, rows);
    tsPostScalar(session, posted, "imin", tsPostable(lowId));
    tsPostScalar(session, posted, "imax", tsPostable(highId));
    tsPostText(session, posted, "panelvar", *d.panelvar);
    tsPostText(session, posted, "balanced", balanceWord);
  }
  // Stata lists them `tdelta tmax tmin gaps imax imin` and `panelvar timevar
  // unit unit1 tsfmt tdeltas tmaxs tmins balanced` (vs-stata truth bank).
  eval::orderRResults(session.eval, {"tdelta", "tmax", "tmin", "gaps", "imax", "imin",
                                     "panelvar", "timevar", "unit", "unit1", "tsfmt", "tdeltas",
                                     "tmaxs", "tmins", "balanced"});
  eval::declareRPosted(session.eval, rCmd, std::move(posted));

  // The labels align on the colon; the panel label can widen the column past
  // "Time variable", the time-only form never looks at it.
  const std::string panelCaption = d.panelLabel + " variable";
  const size_t column = ids != nullptr ? std::max<size_t>(panelCaption.size(), 13) : 13;
  const auto reportLine = [&](const std::string& caption, const std::string& body) {
    return std::string(static_cast<size_t>(d.indent) + column - caption.size(), ' ') + caption +
           ": " + body;
  };
  std::string report = warning;
  if (ids != nullptr)
    report += reportLine(panelCaption,
                         (d.panelAlias.empty() ? *d.panelvar : d.panelAlias) + " (" + balanceWord +
                             ")") +
              "\n";
  std::string timeBody = (d.timeAlias.empty() ? d.timevar : d.timeAlias) + ", " + firstText +
                         " to " + lastText;
  if (steps.offBeat == 1) timeBody += ", but with a gap";
  else if (steps.offBeat > 1) timeBody += ", but with gaps";
  report += reportLine("Time variable", timeBody) + "\n" + reportLine("Delta", stepPhrase);
  return tsEchoBlock(report);
}

// Unlike a declaration with a time variable, this leaves the row order and the
// sort marker alone, as Stata does: `by id:` straight after `xtset id` is "not
// sorted" there as well.
ojson runXtsetPanelOnly(const std::string& ivar, SessionState& session, bool declare,
                        const TsDeclare& look) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  if (!ds->has(ivar)) return engineErrorBlockRc("variable " + ivar + " not found", 111);
  const bool numericPanel = ds->dtype(ivar) == "f64";
  if (declare && !numericPanel)
    return engineErrorBlockRc(
        "string variables not allowed in varlist;\n" + ivar + " is a string variable", 109);

  // Balance here only asks whether the panels are the same size, missing ids
  // being one more panel. A string variable's level codes group it by text.
  std::vector<double> keys = tsPanelKey(ds, ivar);
  const auto realEnd =
      std::partition(keys.begin(), keys.end(), [](double k) { return !std::isnan(k); });
  std::sort(keys.begin(), realEnd);
  const int64_t rows = static_cast<int64_t>(keys.size());
  const std::string balanceWord = tsBlocksEqualSized(keys.data(), rows) ? "balanced" : "unbalanced";

  if (declare) {
    session.panelvar = ivar;
    session.timevar.reset();
    session.delta.reset();
    session.writeTsChars();
  }

  const bool anyId = realEnd != keys.begin();
  const double lowId = numericPanel && anyId ? keys.front() : std::nan("");
  const double highId = numericPanel && anyId ? *std::prev(realEnd) : std::nan("");
  std::vector<std::string> posted;
  tsPostText(session, posted, "panelvar", ivar);
  tsPostText(session, posted, "balanced", balanceWord);
  tsPostScalar(session, posted, "imin", tsPostable(lowId));
  tsPostScalar(session, posted, "imax", tsPostable(highId));
  eval::orderRResults(session.eval, {"imax", "imin", "panelvar", "balanced"});
  eval::declareRPosted(session.eval, "xtset", std::move(posted));

  return tsEchoBlock(std::string(static_cast<size_t>(look.indent), ' ') + look.panelLabel +
                     " variable: " + ivar + " (" + balanceWord + ")");
}

ojson runTsRedisplay(SessionState& session, const std::string& rCmd, const TsDeclare& given,
                     bool anyOption) {
  const bool panelLive = session.panelvar.has_value() && !session.panelvar->empty();
  if (!session.timevar.has_value() || session.timevar->empty()) {
    if (rCmd != "xtset") return engineErrorBlockRc("time variable not set, use tsset varname ...", 111);
    if (!panelLive) return engineErrorBlockRc("panel variable not set; use xtset varname ...", 459);
    return runXtsetPanelOnly(*session.panelvar, session, false, given);
  }
  TsDeclare again = given;
  again.timevar = *session.timevar;
  again.panelvar = panelLive ? session.panelvar : std::nullopt;
  // Typing an option replaces the whole time setting, delta included; typing none
  // restates the live one, with r(unit) then read from the variable's format.
  if (!anyOption) {
    again.delta = session.delta;
    again.format = tsCurrentFormat(session, session.datastore(), again.timevar);
  }
  return runTsDeclare(again, session, rCmd);
}

ojson runXtset(const std::string& ivar, const std::optional<std::string>& tvar, SessionState& session,
               std::optional<double> delta) {
  if (!tvar.has_value() || tvar->empty()) return runXtsetPanelOnly(ivar, session, true);
  TsDeclare both;
  both.panelvar = ivar;
  both.timevar = *tvar;
  both.delta = delta;
  return runTsDeclare(both, session, "xtset");
}

// ---------------------------------------------------------------------------
// drop / keep (varlist form)



// `tsreport [if] [in] [, detail panel nopanel]` ([TS] tsreport): the sample's
// first and last periods, its observation count and the gaps in it -- an
// observation whose next period (t + delta, in its panel) is not in the
// sample. Without `panel` a panel's end counts as a gap ("includes panel
// changes") except at the very end; with it, only gaps within a panel count.
// detail lists them (observation numbers before and after, the panel, the
// first and last missing period, the missing count) and posts r(table)
// (vs-stata bank d05ab7915bd5c213, a8e77c4da53be74b, 5a2ccb8dc685388d,
// e751e767c32ef34a, b572ef742d19de1b, ff4a3ed7d95fcc22). The varlist forms
// (Gap summary report, casewise) are refused by name.
ojson runTsreport(const eval::ParsedCommand& parsed, SessionState& session) {
  auto rc = [](const std::string& m, int r) {
    return ojson(eval::errorBlock("syntax_error", m, std::nullopt, std::nullopt, std::nullopt,
                                  "r(" + std::to_string(r) + ")"));
  };
  if (auto e = eval::rejectUnknownOptions(parsed.opts, {"detail", "casewise", "nopanel", "panel"},
                                          {}))
    return ojson(*e);
  bool detail = false, casewise = false, nopanel = false, panelOpt = false;
  for (const auto& [k, v] : parsed.opts) {
    (void)v;
    if (k == "detail") detail = true;
    if (k == "casewise") casewise = true;
    if (k == "nopanel") nopanel = true;
    if (k == "panel") panelOpt = true;
  }
  if (panelOpt && nopanel) return rc("cannot specify both panel and nopanel", 198);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (!session.timevar.has_value() || session.timevar->empty())
    return rc("time variable not set, use tsset varname ...", 111);
  const bool hasPanel = session.panelvar.has_value() && !session.panelvar->empty();
  if ((panelOpt || nopanel) && !hasPanel) return rc("must specify panelvar; use tsset or xtset", 321);
  if (!parsed.varlist.empty()) {
    if (casewise && parsed.varlist.size() < 2)
      return rc("casewise can only be specified with two or more variables", 198);
    return ojson(eval::unsupportedCommand("tsreport", std::nullopt, std::nullopt, "piet_gap",
                                          eval::UnsupportedCommandKind::kNotImplemented));
  }
  if (casewise) return rc("casewise can only be specified with two or more variables", 198);
  Datastore* ds = session.datastore();
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const std::string tvar = *session.timevar;
  const std::string pvar = hasPanel ? *session.panelvar : std::string();
  const double delta = session.delta.value_or(1.0);
  std::string tsfmt = tsCurrentFormat(session, ds, tvar);
  if (tsfmt.empty()) tsfmt = "%9.0g";
  // The if/in sample (row mask over the data in memory).
  const int64_t nr = ds->nRows();
  std::vector<char> inq(static_cast<size_t>(nr), 1);
  if ((parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
      (parsed.inRange.has_value() && !parsed.inRange->empty())) {
    // Mark the rows through a scratch variable so `if`/`in` follow Stata's rules.
    const std::string mk = "__tsreport_m";
    std::string q;
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) q += " if " + *parsed.ifExpr;
    if (parsed.inRange.has_value() && !parsed.inRange->empty()) q += " in " + *parsed.inRange;
    for (const std::string& line :
         {"quietly generate byte " + mk + " = 0", "quietly replace " + mk + " = 1" + q}) {
      ojson r = dispatch(line, session);
      for (const auto& b : r)
        if (b.is_object() && b.value("type", std::string()) == "error") {
          (void)dispatch("capture drop " + mk, session);
          return b;
        }
    }
    ds = session.datastore();
    std::vector<double> mv;
    std::string err;
    if (ds == nullptr || !statcmd_detail::numericColumn(*ds, mk, &mv, &err)) {
      (void)dispatch("capture drop " + mk, session);
      return engineErrorBlockRc("tsreport: could not read the sample", 198);
    }
    for (size_t i = 0; i < inq.size() && i < mv.size(); ++i) inq[i] = mv[i] == 1 ? 1 : 0;
    (void)dispatch("quietly drop " + mk, session);
    ds = session.datastore();
  }
  std::vector<double> t, pv;
  std::string cerr;
  if (!statcmd_detail::numericColumn(*ds, tvar, &t, &cerr))
    return engineErrorBlockRc("tsreport: time variable", 198);
  if (hasPanel && !statcmd_detail::numericColumn(*ds, pvar, &pv, &cerr))
    return engineErrorBlockRc("tsreport: panel variable", 198);
  const size_t n = t.size();
  std::vector<char> use(n, 0);
  for (size_t i = 0; i < n; ++i)
    use[i] = inq[i] && !std::isnan(t[i]) && (!hasPanel || !std::isnan(pv[i]));
  // Order by panel and time (the tsset order), original row number kept.
  std::vector<size_t> ord(n);
  for (size_t i = 0; i < n; ++i) ord[i] = i;
  auto keyLess = [&](size_t a, size_t b) {
    if (hasPanel) {
      const double pa = pv[a], pb = pv[b];
      if (std::isnan(pa) != std::isnan(pb)) return !std::isnan(pa);
      if (pa != pb && !(std::isnan(pa) && std::isnan(pb))) return pa < pb;
    }
    const double ta = t[a], tb = t[b];
    if (std::isnan(ta) != std::isnan(tb)) return !std::isnan(ta);
    if (ta != tb && !(std::isnan(ta) && std::isnan(tb))) return ta < tb;
    return a < b;
  };
  std::stable_sort(ord.begin(), ord.end(), keyLess);
  double tmin = std::numeric_limits<double>::quiet_NaN(), tmax = tmin, ptop = tmin;
  double tobs = 0;
  for (size_t i = 0; i < n; ++i) {
    if (!use[i]) continue;
    tobs += 1;
    if (std::isnan(tmin) || t[i] < tmin) tmin = t[i];
    if (std::isnan(tmax) || t[i] > tmax) tmax = t[i];
    if (hasPanel && (std::isnan(ptop) || pv[i] > ptop)) ptop = pv[i];
  }
  // F.touse: the sample flag of the same panel's observation at t + delta.
  std::map<std::pair<double, double>, char> at;
  for (size_t i = 0; i < n; ++i)
    if (!std::isnan(t[i]) && (!hasPanel || !std::isnan(pv[i])))
      at[{hasPanel ? pv[i] : 0.0, t[i]}] = use[i];
  std::vector<char> gap(n, 0);
  for (size_t k = 0; k < n; ++k) {
    const size_t i = ord[k];
    if (!use[i]) continue;
    auto it = at.find({hasPanel ? pv[i] : 0.0, t[i] + delta});
    const bool fUse = it != at.end() && it->second == 1;
    if (fUse) continue;
    if (!panelOpt && hasPanel) {
      gap[i] = !(t[i] == tmax && pv[i] == ptop);
    } else {
      if (k + 1 >= n) continue;
      const size_t j = ord[k + 1];
      const bool pcheck = !hasPanel || pv[j] == pv[i];
      gap[i] = pcheck && !std::isnan(t[j]) && t[i] < tmax;
    }
  }
  double ngaps = 0;
  for (char g : gap) ngaps += g;
  eval::replaceRClass(session.eval);
  std::vector<std::string> posted;
  session.eval.rValues.set("end", tmax);
  session.eval.rValues.set("start", tmin);
  session.eval.rValues.set("N_obs", tobs);
  session.eval.rValues.set("N_gaps", ngaps);
  session.eval.rStrings.set("tsfmt", tsfmt);
  session.eval.rStrings.set("timevar", tvar);
  posted = {"end", "start", "N_obs", "N_gaps", "tsfmt", "timevar"};
  if (hasPanel) {
    session.eval.rStrings.set("panelvar", pvar);
    posted.push_back("panelvar");
  }
  // The display.
  auto fmtT = [&](double v) { return strutil::trim(eval::applyStataFmt(v, tsfmt)); };
  auto fmtWidth = [&](const std::string& f) -> size_t {
    static const std::map<std::string, size_t> kDef = {
        {"%td", 9}, {"%tc", 18}, {"%tC", 18}, {"%tw", 7}, {"%tm", 7},
        {"%tq", 6}, {"%th", 6},  {"%ty", 4},  {"%tb", 9}};
    auto it = kDef.find(f);
    if (it != kDef.end()) return it->second;
    size_t i = 1;
    if (i < f.size() && f[i] == '-') ++i;
    const size_t w = static_cast<size_t>(std::atoi(f.c_str() + i));
    if (w > 0) return w;
    return std::max(fmtT(tmin).size(), fmtT(tmax).size());
  };
  const size_t fw = fmtWidth(tsfmt);
  const bool left = tsfmt.size() > 1 && tsfmt[1] == '-';
  size_t digits = std::max<size_t>(4, strutil::trim(eval::applyStataFmt(tobs, "%12.0f")).size() + 2);
  auto num = [&](double v) {
    if (fw > digits)
      return strutil::trim(eval::applyStataFmt(v, "%" + std::string(left ? "-" : "") +
                                                      std::to_string(fw) + ".0fc"));
    return strutil::trim(eval::applyStataFmt(v, "%12.0f"));
  };
  std::vector<std::string> vals = {fmtT(tmin), fmtT(tmax), num(tobs), num(ngaps)};
  size_t len = 0;
  for (const auto& v : vals) len = std::max(len, cecon::ident::codePointCount(v));
  auto padL = [](const std::string& s, size_t w) {
    const size_t c = cecon::ident::codePointCount(s);
    return c >= w ? s : std::string(w - c, ' ') + s;
  };
  std::ostringstream out;
  out << "\n";
  if (hasPanel) out << "Panel variable: " << pvar << "\nTime variable:  " << tvar << "\n";
  else out << "Time variable: " << tvar << "\n";
  out << std::string(18 + len, '-') << "\n";
  out << "Starting period = " << padL(vals[0], len) << "\n";
  out << "Ending period   = " << padL(vals[1], len) << "\n";
  out << "Number of obs   = " << padL(vals[2], len) << "\n";
  out << "Number of gaps  = " << padL(vals[3], len);
  if (!panelOpt && hasPanel) out << " (includes panel changes)";
  if (detail && ngaps > 0) {
    // The gap rows in (sample, panel, time, original order).
    std::vector<size_t> o2(n);
    for (size_t i = 0; i < n; ++i) o2[i] = i;
    std::stable_sort(o2.begin(), o2.end(), [&](size_t a, size_t b) {
      if (use[a] != use[b]) return use[a] < use[b];
      return keyLess(a, b);
    });
    const double nan = std::numeric_limits<double>::quiet_NaN();
    struct GapRow {
      double before, after, panel, s, e, obs;
    };
    std::vector<GapRow> rowsG;
    for (size_t k = 0; k < n; ++k) {
      const size_t i = o2[k];
      if (!gap[i]) continue;
      GapRow g{static_cast<double>(i + 1), nan, hasPanel ? pv[i] : nan, t[i] + delta, nan, nan};
      if (k + 1 < n) {
        const size_t j = o2[k + 1];
        const bool pcheck = !hasPanel || pv[j] == pv[i];
        if (pcheck && !std::isnan(t[j])) {
          g.e = t[j] - delta;
          g.after = static_cast<double>(j + 1);
        }
      }
      g.obs = std::isnan(g.e) ? nan : (g.e - g.s + delta) / delta;
      rowsG.push_back(g);
    }
    auto i0 = [](double v) { return strutil::trim(eval::applyStataFmt(v, "%30.0f")); };
    size_t gds = 13;
    for (const auto& g : rowsG)
      gds = std::max(gds, i0(g.before).size() + 2 + (std::isnan(g.after) ? 1 : i0(g.after).size()) + 1);
    const size_t ffw = std::max<size_t>(fw + 2, 12);
    auto cell = [&](const std::string& v, size_t w, bool lastCol) {
      return lastCol ? padL(v, w) : padL(v, w - 1) + " ";
    };
    std::string pfmt = "%9.0g";
    if (hasPanel) {
      auto it = session.varMeta.find(pvar);
      if (it != session.varMeta.end() && it->second.format.has_value()) pfmt = *it->second.format;
    }
    const size_t pw = std::max<size_t>(fmtWidth(pfmt) + 2, 12);
    size_t total = gds + 1 + (hasPanel ? pw : 0) + ffw * 2 + 12;
    out << "\n\nGap report\n" << std::string(total, '-') << "\n";
    std::string hdr = (gds < 12 ? std::string(gds > 4 ? gds - 4 : 0, ' ') + "Obs"
                                : std::string(gds - 13, ' ') + "Observations");
    hdr += std::string(gds > cecon::ident::codePointCount(hdr) ? gds - cecon::ident::codePointCount(hdr) : 0, ' ');
    hdr += "|";
    if (hasPanel) hdr += cell(statcmd_detail::abbrevCp(pvar, 9), pw, false);
    hdr += cell("Start", ffw, false) + cell("End", ffw, false) + cell("N. obs", 12, true);
    out << hdr << "\n" << std::string(gds, '-') << "+" << std::string(total - gds - 1, '-') << "\n";
    std::vector<std::string> rowNames;
    for (const auto& g : rowsG) {
      std::string lab, rname;
      if (std::isnan(g.after) || g.after == g.before) {
        lab = i0(g.before);
        rname = lab;
      } else {
        lab = i0(g.before) + "\xe2\x80\x93" + i0(g.after);
        rname = i0(g.before) + "--" + i0(g.after);
      }
      rowNames.push_back(rname);
      std::string line = padL(lab + " ", gds) + "|";
      if (hasPanel) line += cell(strutil::trim(eval::applyStataFmt(g.panel, pfmt)), pw, false);
      line += cell(fmtT(g.s), ffw, false);
      line += cell(std::isnan(g.e) ? "." : fmtT(g.e), ffw, false);
      line += cell(std::isnan(g.obs) ? "." : strutil::trim(eval::applyStataFmt(g.obs, "%9.0fc")), 12,
                   true);
      out << line << "\n";
    }
    out << std::string(total, '-');
    eval::EvalMatrix M;
    M.rownames = rowNames;
    M.colnames = hasPanel ? std::vector<std::string>{"ObsBefore", "ObsAfter", pvar, "StartTime",
                                                     "EndTime", "Obs"}
                          : std::vector<std::string>{"ObsBefore", "ObsAfter", "StartTime",
                                                     "EndTime", "Obs"};
    for (const auto& g : rowsG) {
      if (hasPanel) M.data.push_back({g.before, g.after, g.panel, g.s, g.e, g.obs});
      else M.data.push_back({g.before, g.after, g.s, g.e, g.obs});
    }
    session.eval.rMatrices.emplace_back("table", M);
    posted.push_back("table");
  }
  eval::declareRPosted(session.eval, "tsreport", std::move(posted));
  ojson b;
  b["type"] = "display";
  b["text"] = out.str();
  return b;
}

}  // namespace piet::handlers
