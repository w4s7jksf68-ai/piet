// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers.h"

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

#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/bcal.h"  // AER scope-2 B2-3: business calendars
#include "piet/stata_format.h"  // piet::fmt::renderTd -- the describe table date column
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // expandVarlist -- the ONE varlist wildcard/range expander
#include "piet/mutation_result.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

#include "support.h"
#include "options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace utility_detail;

// ===========================================================================
// bcal -- Stata business calendars ([D] bcal)
// ===========================================================================
//
// AER scope-2 B2-3, the blocker of paper 066. The calendar reader, the
// create induction and the index arithmetic live in piet/bcal.{h,cpp}; this
// handler is the command surface ([D] bcal; measured StataBE 19.5 behaviour).
//
// NAMED DIVERGENCE (D2/D3/D4): Stata searches BASE;SITE;.;PERSONAL;PLUS for a
// .stbcal and `bcal dir` walks the same path. PIET models no sysdir, so the
// search directory is the session's cwd alone: `bofd("simple", ...)` -- the one
// calendar StataBE ships, in BASE -- is missing here unless the file is in the
// working directory, `bcal dir` lists the cwd, and `personal` is refused by
// name rather than writing somewhere PIET cannot then find.



// The fit-sample filter that keeps each surviving row's position in the working
// frame (inrange_filter.cpp; e(sample)'s channel).
InRangeFilterResult applyIfAndInFilterKeepingIndex(SessionState&, const std::string&,
                                                   const std::optional<std::string>&,
                                                   const std::optional<std::string>&,
                                                   std::string*);

namespace {

using BcalOpts = std::vector<std::pair<std::string, eval::OptValue>>;

// PIET models no adopath: a calendar lives in the session's working directory or
// nowhere, and a calendar's "./name.stbcal" is relative to it.
std::string calendarFileIn(const std::string& home, const std::string& name) {
  return (std::filesystem::path(home) / (name + ".stbcal")).u8string();
}

// An option typed with no argument where one is required.
bool typedBare(const BcalOpts& opts, const std::string& key) {
  return hasFlag(opts, key) && !optStr(opts, key).has_value();
}

// A numeric column read as %9.0fc: right-aligned, with thousands commas.
std::string paddedThousands(std::int64_t count) {
  std::string digits = std::to_string(count < 0 ? -count : count);
  for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3)
    digits.insert(static_cast<size_t>(at), ",");
  if (count < 0) digits.insert(digits.begin(), '-');
  return digits.size() >= 9 ? digits : std::string(9 - digits.size(), ' ') + digits;
}

std::string padded9(std::int64_t value) {
  char text[32];
  std::snprintf(text, sizeof text, "%9lld", static_cast<long long>(value));
  return text;
}

std::string padded11OneDecimal(double value) {
  char text[64];
  std::snprintf(text, sizeof text, "%11.1f", value);
  return text;
}

// The two per-year figures, as the table prints them and r() posts them: r() takes
// the decimal the printed text shows, so a binary tie (182.625) goes to even.
double postedOneDecimal(double value) {
  char text[64];
  std::snprintf(text, sizeof text, "%.1f", value);
  return std::strtod(text, nullptr);
}

struct CalendarTally {
  std::int64_t included = 0, omitted = 0;
  double includedPerYear = 0, omittedPerYear = 0;
};

CalendarTally tallyCalendar(const bcal::BusinessCalendar& cal) {
  const double span = static_cast<double>(cal.hi - cal.lo + 1);
  CalendarTally tally;
  tally.included = cal.maxTb - cal.minTb + 1;
  tally.omitted = cal.hi - cal.lo + 1 - tally.included;
  tally.includedPerYear = static_cast<double>(tally.included) / span * 365.25;
  tally.omittedPerYear = static_cast<double>(tally.omitted) / span * 365.25;
  return tally;
}

// The block `bcal describe` prints and `bcal create` opens with. Numbers end at a
// fixed column; the unit word starts at column 37.
std::string calendarTable(const bcal::BusinessCalendar& cal) {
  const CalendarTally tally = tallyCalendar(cal);
  const auto tdText = [](std::int64_t day) {
    return piet::fmt::renderTd(static_cast<double>(day)).value_or(std::string("."));
  };
  const auto unitRow = [](const std::string& caption, const std::string& numbers,
                          const std::string& unit) {
    std::string row = caption + numbers;
    if (row.size() < 36) row.append(36 - row.size(), ' ');
    return row + unit + "\n";
  };
  const std::string gutter(13, ' ');
  const std::string tbUnits = "in %tb" + cal.name + " units";
  std::string table = "\n  Business calendar " + cal.name + " (format %tb" + cal.name + "):\n\n";
  table += "    Purpose: " + cal.purpose + "\n\n";
  table += "      Range: " + tdText(cal.lo) + "  " + tdText(cal.hi) + "\n";
  table += unitRow(gutter, padded9(cal.lo) + "  " + padded9(cal.hi), "in %td units");
  table += unitRow(gutter, padded9(cal.minTb) + "  " + padded9(cal.maxTb), tbUnits);
  table += "\n     Center: " + tdText(cal.center) + "\n";
  table += unitRow(gutter, padded9(cal.center), "in %td units");
  table += unitRow(gutter, padded9(0), tbUnits);
  table += "\n";
  table += unitRow("    Omitted: ", paddedThousands(tally.omitted), "days");
  table += unitRow(gutter, padded11OneDecimal(tally.omittedPerYear), "approx. days/year");
  table += "\n";
  table += unitRow("   Included: ", paddedThousands(tally.included), "days");
  table += unitRow(gutter, padded11OneDecimal(tally.includedPerYear), "approx. days/year");
  return table;
}

// r() belongs to the command as a whole: what an earlier one left does not survive
// a bcal verb, whether it succeeds or refuses (measured: r(omitted) reads missing
// after a load, r(undefined) empty after check's r(459)).
void forgetEarlierR(SessionState& session) {
  session.eval.rValues.clear();
  session.eval.rStrings.clear();
}

void postCalendarFacts(SessionState& session, const bcal::BusinessCalendar& cal,
                       const std::string& fileAsShown, bool withTally) {
  forgetEarlierR(session);
  auto& numbers = session.eval.rValues;
  auto& texts = session.eval.rStrings;
  texts.set("fn", fileAsShown);
  texts.set("name", cal.name);
  texts.set("purpose", cal.purpose);
  numbers.set("min_date_td", static_cast<double>(cal.lo));
  numbers.set("max_date_td", static_cast<double>(cal.hi));
  numbers.set("ctr_date_td", static_cast<double>(cal.center));
  numbers.set("min_date_tb", static_cast<double>(cal.minTb));
  numbers.set("max_date_tb", static_cast<double>(cal.maxTb));
  std::vector<std::string> posted = {"fn",          "name",        "purpose",
                                     "min_date_td", "max_date_td", "ctr_date_td",
                                     "min_date_tb", "max_date_tb"};
  if (!withTally) {
    eval::declareRPosted(session.eval, "bcal", std::move(posted));
    return;
  }
  const CalendarTally tally = tallyCalendar(cal);
  numbers.set("omitted", static_cast<double>(tally.omitted));
  numbers.set("included", static_cast<double>(tally.included));
  numbers.set("omitted_year", postedOneDecimal(tally.omittedPerYear));
  numbers.set("included_year", postedOneDecimal(tally.includedPerYear));
  posted.insert(posted.end(), {"omitted", "included", "omitted_year", "included_year"});
  eval::declareRPosted(session.eval, "bcal", std::move(posted));
}

// --- bcal describe / bcal load ---------------------------------------------
// Both go to the file every time, unlike the expression surfaces, which answer
// from the cache: a cached calendar whose file is gone is r(601) here (measured).
ojson bcalReadVerb(const eval::ParsedCommand& parsed, SessionState& session, const std::string& home,
                   bool isLoad) {
  if (!parsed.opts.empty()) return utilErrorRc("options not allowed", 101);
  if (parsed.varlist.size() > 2) return utilErrorRc("varlist not allowed", 101);
  if (parsed.varlist.size() < 2) {
    return utilErrorRc(std::string("syntax error\n    Syntax is bcal ") +
                           (isLoad ? "load" : "describe") +
                           " calname|%tbfmt.\n    Nothing was found where a calendar name or "
                           "%tbfmt was expected.",
                       198);
  }
  const std::string typed = dequote(parsed.varlist[1]);
  const std::string name = bcal::calNameFromFormat(typed);
  if (!bcal::isValidCalName(name))
    return utilErrorRc(typed + " is an invalid calendar name or %fmt", 198);

  const std::string fileName = name + ".stbcal";
  const std::string shownFile = "./" + fileName;
  const std::string diskFile = calendarFileIn(home, name);
  std::ifstream source(cecon::to_native_path(diskFile), std::ios::binary);
  if (!source) return utilErrorRc("file " + fileName + " not found", 601);

  std::string listing = "loading " + shownFile + " ...\n\n";
  if (isLoad) {
    std::string line;
    for (int number = 1; std::getline(source, line); ++number) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      char prefix[32];
      std::snprintf(prefix, sizeof prefix, "%6d. ", number);
      listing += prefix + line + "\n";
    }
  }
  source.close();

  bcal::StbcalError readerError;
  std::shared_ptr<const bcal::BusinessCalendar> cal = bcal::loadFile(name, diskFile, &readerError);
  if (cal == nullptr) {
    ojson refusal = utilErrorRc(readerError.message, readerError.rc);
    if (!isLoad) return refusal;
    ojson blocks = ojson::array();
    blocks.push_back(displayBlock(listing));
    blocks.push_back(std::move(refusal));
    return blocks;
  }
  postCalendarFacts(session, *cal, shownFile, /*withTally=*/!isLoad);
  return displayBlock(isLoad ? listing + "\n(calendar loaded successfully)\n" : calendarTable(*cal));
}

// --- bcal dir --------------------------------------------------------------
ojson bcalDirVerb(const eval::ParsedCommand& parsed, SessionState& session, const std::string& home) {
  if (findOpt(parsed.opts, "testmode") != nullptr)
    return ojson(eval::unsupportedOption("testmode", "bcal dir has no test hook here"));
  if (auto refusal = eval::rejectUnknownOptions(parsed.opts, {})) return ojson(*refusal);
  if (parsed.varlist.size() > 2) return utilErrorRc("varlist not allowed", 101);
  const std::string pattern = parsed.varlist.size() == 2 ? parsed.varlist[1] : std::string("*");
  if (!pattern.empty() && pattern.front() == '%')
    return utilErrorRc(pattern + " is not a calendar name pattern", 198);

  // No name rule here: an eleven-character stem is still listed (measured).
  std::vector<std::string> found;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(std::filesystem::path(cecon::to_native_path(home)), ec), end;
       !ec && it != end; it.increment(ec)) {
    std::error_code typeError;
    if (!it->is_regular_file(typeError) || it->path().extension() != ".stbcal") continue;
    const std::string stem = it->path().stem().u8string();
    if (strutil::globMatch(pattern, stem)) found.push_back(stem);
  }
  std::sort(found.begin(), found.end());

  if (found.empty()) {
    forgetEarlierR(session);
    session.eval.rStrings.set("calendars", "");
    eval::declareRPosted(session.eval, "bcal", {"calendars"});
    return displayBlock("no calendar files matching " + pattern + " found\n    No business calendars matching " +
                        pattern + " were found, which is to say, no files matching " + pattern +
                        ".stbcal were found in any of the directories along the adopath.\n");
  }
  std::string text = "  " + std::to_string(found.size()) + " calendar file" +
                     (found.size() == 1 ? " found:\n" : "s found:\n");
  forgetEarlierR(session);
  std::string reverseNames;  // r(calendars) runs opposite to the listing (measured)
  std::vector<std::string> posted = {"calendars"};
  for (const std::string& stem : found) {
    const std::string shown = "./" + stem + ".stbcal";
    text += std::string(stem.size() < 20 ? 20 - stem.size() : 0, ' ') + stem + ":  " + shown + "\n";
    reverseNames = reverseNames.empty() ? stem : stem + " " + reverseNames;
    session.eval.rStrings.set("fn_" + stem, shown);
    posted.push_back("fn_" + stem);
  }
  session.eval.rStrings.set("calendars", reverseNames);
  eval::declareRPosted(session.eval, "bcal", std::move(posted));
  return displayBlock(text);
}

// --- bcal check ------------------------------------------------------------
ojson bcalCheckVerb(const eval::ParsedCommand& parsed, SessionState& session, const std::string& home) {
  if (auto refusal = eval::rejectUnknownOptions(parsed.opts, {"rc0"})) return ojson(*refusal);
  Datastore* ds = session.hasData() ? session.datastore() : nullptr;

  std::set<std::string> asked;
  const bool narrowed = parsed.varlist.size() > 1;
  if (narrowed) {
    const std::vector<std::string> typed(parsed.varlist.begin() + 1, parsed.varlist.end());
    if (ds == nullptr) return utilErrorRc("variable " + typed.front() + " not found", 111);
    loops::VarlistExpandResult expanded = loops::expandVarlist(joinWith(typed, " "), session);
    if (expanded.isError) return expanded.error;
    if (auto refusal = loops::checkKnownVarlist(expanded.tokens, session)) return *refusal;
    asked.insert(expanded.tokens.begin(), expanded.tokens.end());
  }

  // Calendar name -> the variables using it, names ascending, variables in dataset
  // order. A varlist names the real variables; StataBE 19.5 itself reports the
  // k-th variable of the dataset there, which is not copied.
  std::map<std::string, std::vector<std::string>> users;
  if (ds != nullptr) {
    for (const VarInfo& info : ds->formatCard(ds->names())) {
      if (narrowed && asked.count(info.name) == 0) continue;
      const std::string fmt = session.displayFormat(info);
      if (fmt.empty() || fmt.front() != '%') continue;
      const std::string name = bcal::calNameFromFormat(fmt);
      if (!name.empty()) users[name].push_back(info.name);
    }
  }
  if (users.empty()) {
    forgetEarlierR(session);
    eval::declareRPosted(session.eval, "bcal", {});
    return displayBlock("(no variables use %tb formats)\n");
  }

  std::vector<std::string> defined, undefined, withErrors;
  std::string report;
  std::error_code missing;
  for (const auto& [name, vars] : users) {
    const std::string tag = "%tb" + name;
    const char* status = "defined, ";
    if (bcal::isValidCalName(name) && bcal::lookup(name) != nullptr) {
      defined.push_back(name);
    } else if (bcal::isValidCalName(name) &&
               std::filesystem::exists(cecon::to_native_path(calendarFileIn(home, name)), missing)) {
      status = "defined, has errors, ";
      withErrors.push_back(name);
    } else {
      status = "undefined, ";
      undefined.push_back(name);
    }
    report += "\n" + std::string(tag.size() < 26 ? 26 - tag.size() : 0, ' ') + tag + ":  " + status +
              (vars.size() == 1 ? "used by variable\n" : "used by variables\n");
    report += std::string(29, ' ') + joinWith(vars, " ") + "\n";
  }

  const bool anyTrouble = !undefined.empty() || !withErrors.empty();
  const std::string troubleLine = "undefined or defined-with-errors %tb formats";
  if (anyTrouble && !hasFlag(parsed.opts, "rc0")) {
    // The help's rule: r() is not stored on this path; runBcal empties it.
    ojson blocks = ojson::array();
    blocks.push_back(displayBlock(report));
    blocks.push_back(utilErrorRc(troubleLine, 459));
    return blocks;
  }
  if (anyTrouble) report += "\n" + troubleLine + "\n";

  forgetEarlierR(session);
  std::vector<std::string> posted;
  const auto postText = [&](const std::string& rName, const std::string& value) {
    session.eval.rStrings.set(rName, value);
    posted.push_back(rName);
  };
  if (!defined.empty()) postText("defined", joinWith(defined, " "));
  if (!undefined.empty()) postText("undefined", joinWith(undefined, " "));
  if (!withErrors.empty()) postText("error", joinWith(withErrors, " "));
  for (const auto& [name, vars] : users) postText("varlist_" + name, joinWith(vars, " "));
  eval::declareRPosted(session.eval, "bcal", std::move(posted));
  return displayBlock(report);
}

// --- bcal create -----------------------------------------------------------
std::string creationStampLine() {
  static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const std::time_t now = std::time(nullptr);
  const std::tm* local = std::localtime(&now);
  char line[64];
  std::snprintf(line, sizeof line, "* Created/replaced on %2d %s %d", local->tm_mday,
                kMonths[local->tm_mon], local->tm_year + 1900);
  return line;
}

// The file token, split as typed: `sub/`, `./` or "" for the path, then NAME.
struct CalendarTarget {
  std::string typedPath, name, suffix;
  std::string shownFile() const { return typedPath + name + ".stbcal"; }
};

std::optional<ojson> splitCalendarTarget(const std::string& token, const SessionState& session,
                                         CalendarTarget* target) {
  const size_t slash = token.find_last_of("/\\");
  const std::string fileWord = slash == std::string::npos ? token : token.substr(slash + 1);
  if (slash != std::string::npos) {
    target->typedPath = token.substr(0, slash + 1);
    const std::string folder = slash == 0 ? token.substr(0, 1) : token.substr(0, slash);
    std::error_code ec;
    if (!std::filesystem::is_directory(cecon::to_native_path(resolveUserPath(session, folder)), ec))
      return utilErrorRc("path " + folder + " does not exist", 601);
  }
  const size_t dot = fileWord.find_last_of('.');
  target->name = dot == std::string::npos ? fileWord : fileWord.substr(0, dot);
  target->suffix = dot == std::string::npos ? std::string() : fileWord.substr(dot);
  if (!strutil::isValidStataIdentifier(target->name))
    return utilErrorRc(target->name + " is not a valid calendar name", 198);
  if (target->name.size() > 10) {
    return utilErrorRc(target->name + " too long; calendar names are limited to 10 characters",
                       198);
  }
  if (!target->suffix.empty() && target->suffix != ".stbcal" && target->suffix != ".")
    return utilErrorRc("extension " + target->suffix + " in calendar file not allowed", 198);
  return std::nullopt;
}

std::optional<ojson> readMaxgapOption(const BcalOpts& opts, bcal::InduceOpts* into) {
  const std::optional<std::string> typed = optStr(opts, "maxgap");
  if (!typed.has_value() && !typedBare(opts, "maxgap")) return std::nullopt;
  const std::string text = typed.value_or(std::string());
  char* stop = nullptr;
  const long long value = std::strtoll(text.c_str(), &stop, 10);
  if (text.empty() || *stop != '\0') return utilErrorRc("option maxgap() incorrectly specified", 198);
  if (value < 1) return utilErrorRc("maxgap() should be at least 1", 198);
  into->maxgap = value;
  return std::nullopt;
}

std::optional<ojson> readDateformatOption(const BcalOpts& opts, bcal::InduceOpts* into) {
  into->dateformat = "ymd";
  const std::optional<std::string> typed = optStr(opts, "dateformat");
  if (!typed.has_value()) return std::nullopt;
  const std::string order = lowerOf(*typed);
  static const std::set<std::string> kOrders = {"ymd", "ydm", "myd", "mdy", "dym", "dmy"};
  if (kOrders.count(order) == 0) {
    return utilErrorRc(*typed + " found where dateformat expected\n"
                                "    dateformat options: ymd | ydm | myd | mdy | dym | dmy",
                       198);
  }
  into->dateformat = order;
  return std::nullopt;
}

std::optional<ojson> readPurposeOption(const BcalOpts& opts, bcal::InduceOpts* into) {
  const std::optional<std::string> typed = optStr(opts, "purpose");
  if (!typed.has_value()) return std::nullopt;
  std::string words = *typed;
  words.erase(std::remove(words.begin(), words.end(), '"'), words.end());
  if (words.size() > 63) return utilErrorRc("purpose() is limited to 63 characters", 198);
  into->purpose = words;
  return std::nullopt;
}

// range() and centerdate() read under the dateformat chosen just before them.
std::optional<ojson> readDateOptions(const BcalOpts& opts, bcal::InduceOpts* into) {
  const std::string& order = into->dateformat;
  if (const std::optional<std::string> typed = optStr(opts, "range")) {
    const std::vector<std::string> ends = splitWs(*typed);
    if (ends.size() != 2) return utilErrorRc("two date arguments expected in range()", 198);
    const auto readEnd = [&](const std::string& word, bool* have, std::int64_t* day) {
      if (word == ".") return true;
      const std::optional<std::int64_t> parsed = bcal::readWholeDate(word, order);
      if (!parsed.has_value()) return false;
      *have = true;
      *day = *parsed;
      return true;
    };
    if (!readEnd(ends[0], &into->haveRangeLo, &into->rangeLo))
      return utilErrorRc(ends[0] + " not in " + order + " format", 198);
    if (!readEnd(ends[1], &into->haveRangeHi, &into->rangeHi))
      return utilErrorRc(ends[1] + " not in " + order + " format", 198);
    if (into->haveRangeLo && into->haveRangeHi && into->rangeHi <= into->rangeLo)
      return utilErrorRc("second date should be after first date in range()", 198);
  }
  if (const std::optional<std::string> typed = optStr(opts, "centerdate")) {
    const std::optional<std::int64_t> parsed = bcal::readWholeDate(*typed, order);
    if (!parsed.has_value()) return utilErrorRc(*typed + " not in " + order + " format", 198);
    into->haveCenter = true;
    into->center = *parsed;
  }
  return std::nullopt;
}

// excludemissing(varlist [, any]): the rows it takes out of the sample.
struct ExclusionRule {
  std::vector<std::string> vars;
  bool whenAny = false;
};

std::optional<ojson> readExclusionOption(const BcalOpts& opts, SessionState& session,
                                         ExclusionRule* rule) {
  const std::optional<std::string> typed = optStr(opts, "excludemissing");
  if (!typed.has_value()) return std::nullopt;
  const size_t comma = typed->find(',');
  if (comma != std::string::npos) {
    for (const std::string& word : splitWs(typed->substr(comma + 1))) {
      if (word != "any") return utilErrorRc("option " + word + " not allowed", 198);
      rule->whenAny = true;
    }
  }
  loops::VarlistExpandResult expanded =
      loops::expandVarlist(trimOf(typed->substr(0, comma)), session);
  if (expanded.isError) return ojson(expanded.error);
  if (auto refusal = loops::checkKnownVarlist(expanded.tokens, session)) return refusal;
  if (expanded.tokens.empty()) return utilErrorRc("varlist required", 100);
  rule->vars = std::move(expanded.tokens);
  return std::nullopt;
}

// The from() values in the sample, each with its row in the working frame. Stata's
// missing counts for strings too: a blank cell is missing here.
struct CreationSample {
  std::vector<double> dates;
  std::vector<std::int64_t> frameRows;
};

CreationSample drawSample(const Datastore& source, const std::string& fromVar,
                          const ExclusionRule& rule, const double* framePositions) {
  std::vector<ColView> guards;
  for (const std::string& var : rule.vars) guards.push_back(viewOf(source, var));
  const auto isBlank = [](const ColView& view, std::int64_t row) {
    return cellMissing(view, row) ||
           (view.isFactor && (*view.levels)[static_cast<size_t>(view.codes[row])].empty());
  };
  const double* from = source.col(fromVar).f64();
  CreationSample sample;
  for (std::int64_t row = 0; row < source.nRows(); ++row) {
    if (!guards.empty()) {
      const auto test = [&](const ColView& view) { return isBlank(view, row); };
      const bool out = rule.whenAny ? std::any_of(guards.begin(), guards.end(), test)
                                    : std::all_of(guards.begin(), guards.end(), test);
      if (out) continue;
    }
    sample.dates.push_back(from[row]);
    sample.frameRows.push_back(framePositions == nullptr
                                   ? row
                                   : static_cast<std::int64_t>(framePositions[row]) - 1);
  }
  return sample;
}

ojson bcalCreateVerb(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto refusal = eval::rejectUnknownOptions(
          parsed.opts, {"from", "generate", "personal", "excludemissing", "replace", "purpose",
                        "dateformat", "range", "centerdate", "maxgap"}))
    return ojson(*refusal);
  if (parsed.varlist.size() < 2) return utilErrorRc("name of business calendar file expected", 198);
  if (parsed.varlist.size() > 2) return utilSyntax("invalid syntax");

  CalendarTarget target;
  if (auto refusal = splitCalendarTarget(dequote(parsed.varlist[1]), session, &target)) return *refusal;
  if (hasFlag(parsed.opts, "personal")) {
    return ojson(eval::unsupportedOption(
        "personal", "this build has no PERSONAL directory; give the calendar a path instead"));
  }
  const std::optional<std::string> fromText = optStr(parsed.opts, "from");
  if (!fromText.has_value() || fromText->empty()) return utilErrorRc("option from() required", 198);
  if (!session.hasData()) return utilErrorRc("no dataset in use", 3);
  Datastore* head = session.datastore();
  if (head == nullptr) return ojson(eval::noDataLoaded());

  loops::VarlistExpandResult fromVars = loops::expandVarlist(*fromText, session);
  if (fromVars.isError) return ojson(fromVars.error);
  if (fromVars.tokens.size() > 1) return utilErrorRc("too many variables specified", 103);
  if (auto refusal = loops::checkKnownVarlist(fromVars.tokens, session)) return *refusal;
  if (fromVars.tokens.empty()) return utilErrorRc("option from() required", 198);
  const std::string fromVar = fromVars.tokens.front();
  if (head->dtype(fromVar) != "f64") return utilErrorRc(fromVar + " has non-numeric values", 198);

  // generate() is judged here, before anything can be written. A storage-type word
  // as the name is still taken as a name (open BUG-1874).
  std::optional<std::string> newVar;
  if (typedBare(parsed.opts, "generate")) return utilSyntax("option generate() incorrectly specified");
  if (const std::optional<std::string> typed = optStr(parsed.opts, "generate")) {
    const std::vector<std::string> names = splitWs(*typed);
    if (names.size() > 1) return utilErrorRc("too many variables specified", 103);
    if (names.empty()) return utilSyntax("option generate() incorrectly specified");
    const std::string& name = names.front();
    if (head->has(name)) return utilErrorRc("variable " + name + " already defined", 110);
    if (strutil::isReservedVarName(name)) return utilErrorRc(name + " invalid varname", 198);
    if (!strutil::isValidStataIdentifier(name)) return utilErrorRc(name + " invalid name", 198);
    newVar = name;
  }

  bcal::InduceOpts induce;
  induce.calName = target.name;
  induce.todayLine = creationStampLine();
  if (auto refusal = readMaxgapOption(parsed.opts, &induce)) return *refusal;
  if (auto refusal = readDateformatOption(parsed.opts, &induce)) return *refusal;
  if (auto refusal = readPurposeOption(parsed.opts, &induce)) return *refusal;
  if (auto refusal = readDateOptions(parsed.opts, &induce)) return *refusal;
  ExclusionRule exclusion;
  if (auto refusal = readExclusionOption(parsed.opts, session, &exclusion)) return *refusal;

  // The option readers above may have touched the session; take the head afresh.
  head = session.datastore();
  if (head == nullptr) return ojson(eval::noDataLoaded());
  const std::int64_t rowsInFrame = head->nRows();
  CreationSample sample;
  const bool qualified = (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
                         (parsed.inRange.has_value() && !parsed.inRange->empty());
  try {
    if (qualified) {
      std::string positionVar;
      const InRangeFilterResult kept = applyIfAndInFilterKeepingIndex(
          session, *session.currentHbin(), parsed.ifExpr, parsed.inRange, &positionVar);
      if (!kept.ok) return inRangeRejectError(kept, parsed.inRange.value_or(std::string()), "bcal create");
      session.noteProjectionScratch(kept.dataPath);
      const Datastore filtered(kept.dataPath);
      sample = drawSample(filtered, fromVar, exclusion, filtered.col(positionVar).f64());
    } else {
      sample = drawSample(*head, fromVar, exclusion, nullptr);
    }
  } catch (const std::exception& e) {
    return utilError("bcal create: could not read the data -- " + std::string(e.what()), "data_io");
  }
  if (sample.dates.empty()) return utilErrorRc("no observations found for calendar", 2000);

  // An existing file loses to everything checked so far and beats a gap failure,
  // so the gap's answer waits until that test has been made (measured).
  const bcal::InduceResult made = bcal::induceFromDates(sample.dates, induce);
  const bool gapFailure = !made.ok && made.message.rfind("gap in business calendar", 0) == 0;
  if (!made.ok && !gapFailure) return utilErrorRc(made.message, made.rc);

  const std::string shownFile = target.shownFile();
  const std::string diskFile = resolveUserPath(session, shownFile);
  std::error_code existsError;
  const bool alreadyThere = std::filesystem::exists(cecon::to_native_path(diskFile), existsError);
  if (alreadyThere && !hasFlag(parsed.opts, "replace"))
    return utilErrorRc("file " + shownFile + " already exists; use replace to overwrite", 602);
  if (gapFailure) {
    return utilErrorRc(made.message + (alreadyThere ? "\n    " + shownFile + " left unchanged" : ""),
                       made.rc);
  }

  // What was just induced has to read back before it may touch the disk: a center
  // or range start on an omitted weekday and a single distinct date all end here.
  bcal::ParseResult reread = bcal::parseStbcal(made.text, target.name);
  if (!reread.ok) {
    return utilErrorRc("bcal describe " + target.name + " failed" +
                           (alreadyThere ? "; " + shownFile + " left unchanged" : ""),
                       197);
  }
  {
    std::ofstream out(cecon::to_native_path(diskFile), std::ios::binary | std::ios::trunc);
    out << made.text;
    out.flush();
    if (!out) return utilError("could not write " + shownFile + " (disk full or I/O error)", "data_io");
  }
  reread.cal.fn = diskFile;
  auto calendar = std::make_shared<const bcal::BusinessCalendar>(std::move(reread.cal));
  bcal::install(target.name, calendar);

  std::string text = calendarTable(*calendar);
  text += "\n  Notes:\n\n    Business calendar file " + shownFile + " saved.\n";
  if (!target.typedPath.empty()) {
    std::error_code sameError;
    const std::string folder = resolveUserPath(session, target.typedPath);
    if (!std::filesystem::equivalent(cecon::to_native_path(folder),
                                     cecon::to_native_path(bcal::searchDir()), sameError) &&
        !sameError) {
      text += "\n    " + target.name + ".stbcal not in a Stata system directory.\n";
    }
  }
  if (newVar.has_value()) {
    text += "\n    Variable " + *newVar + " created; it contains business dates in %tb" +
            target.name + " format.\n";
  }
  if (made.leadNote) text += "\n    First date in range() occurs before first business date in dataset.\n";
  if (made.trailNote) text += "\n    Second date in range() occurs after last business date in dataset.\n";

  ojson blocks = ojson::array();
  blocks.push_back(displayBlock(text));
  if (newVar.has_value()) {
    // One value per observation; rows outside the sample stay missing.
    std::vector<std::string> cells(static_cast<size_t>(rowsInFrame));
    for (size_t i = 0; i < sample.dates.size(); ++i) {
      cells[static_cast<size_t>(sample.frameRows[i])] = numCell(bcal::bofd(*calendar, sample.dates[i]));
    }
    std::string written;
    if (auto refusal = writeColumn(session, *session.currentHbin(), *newVar, cells, "f64", true, &written))
      return *refusal;
    session.load(written);
    session.varMeta[*newVar].format = "%tb" + target.name;
    session.userFormattedVars.insert(*newVar);
    ojson mutation = mutationResult(session, "gen", {*newVar});
    if (mutation.is_array()) {
      for (const auto& block : mutation) blocks.push_back(block);
    } else if (!mutation.is_null()) {
      blocks.push_back(std::move(mutation));
    }
  }
  postCalendarFacts(session, *calendar, shownFile, /*withTally=*/true);
  return blocks;
}

// A verb token against Stata's minimum abbreviations: `ch[eck]`, `d[escribe]`, and
// dir, load and create only in full.
std::string resolveBcalVerb(const std::string& typed) {
  const std::string word = lowerOf(typed);
  const auto abbreviates = [&](const std::string& full, size_t atLeast) {
    return word.size() >= atLeast && word.size() <= full.size() &&
           full.compare(0, word.size(), word) == 0;
  };
  if (word == "dir" || word == "load" || word == "create") return word;
  if (abbreviates("check", 2)) return "check";
  if (abbreviates("describe", 1)) return "describe";
  return std::string();
}

}  // namespace

ojson runBcal(const eval::ParsedCommand& parsed, SessionState& session) {
  (void)kVocabBcal;  // the dispatcher's abbreviation net reads it by registration
  if (parsed.varlist.empty()) return utilSyntax("invalid syntax");
  const std::string verb = resolveBcalVerb(parsed.varlist[0]);
  if (verb.empty()) return utilErrorRc(parsed.varlist[0] + " invalid bcal subcommand", 198);

  // Point lookup() at the same directory these verbs read.
  const std::string home = bcal::searchDirFor(session.cwd.value_or(std::string()));
  bcal::setSearchDir(home);

  ojson answer;
  if (verb == "create") {
    answer = bcalCreateVerb(parsed, session);
  } else if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    answer = utilErrorRc("if not allowed", 101);
  } else if (verb == "describe" || verb == "load") {
    answer = bcalReadVerb(parsed, session, home, /*isLoad=*/verb == "load");
  } else if (verb == "dir") {
    answer = bcalDirVerb(parsed, session, home);
  } else {
    answer = bcalCheckVerb(parsed, session, home);
  }
  if (firstError(answer) != nullptr) {
    forgetEarlierR(session);
    eval::declareRPosted(session.eval, "bcal", {});
  }
  return answer;
}


}  // namespace piet::handlers
