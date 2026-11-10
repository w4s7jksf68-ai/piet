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
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // BUG-540: stataMissingWithKind — `.a`-`.z` identity in matrix literals
#include "piet/eval/macros.h"
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/program.h"  // BUG-922: program::restorePreservesDeeperThan is defined here
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

  const double delta = d.delta.value_or(1.0);
  if (auto refusal = tsBadDelta(delta)) return *refusal;
  const TsStepTally steps = tsTallySteps(ids, times, rows, delta);
  if (steps.repeated) {
    return engineErrorBlockRc(ids != nullptr ? "repeated time values within panel"
                                             : "repeated time values in sample",
                              451);
  }
  if (steps.belowDelta)
    return engineErrorBlockRc("time values with period less than delta() found", 451);

  session.panelvar = d.panelvar;
  session.timevar = d.timevar;
  session.delta = d.delta;
  session.writeTsChars();

  // Rows with a missing panel id count for gaps, repeats and balance, not edges.
  const auto [firstTime, lastTime] = tsSpan(times, ids, rows);
  const bool clockOption = d.period == "clocktime";
  const std::string firstText = tsTimeText(firstTime, chosenFormat, clockOption);
  const std::string lastText = tsTimeText(lastTime, chosenFormat, clockOption);
  const std::string stepPhrase = tsDeltaText(delta, chosenFormat);
  const std::string balanceWord = ids != nullptr ? tsBalanceWord(ids, times, rows) : "";

  std::vector<std::string> posted;
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
  if (ids != nullptr) report += reportLine(panelCaption, *d.panelvar + " (" + balanceWord + ")") + "\n";
  std::string timeBody = d.timevar + ", " + firstText + " to " + lastText;
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


}  // namespace piet::handlers
