// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// stata_settings.cpp -- the Stata settings PIET keeps as values: what `set NAME
// ARG` stores and what c(NAME) answers for them (declared in piet/cclass.h).
//
// Two lists. Settings that only change what Stata DISPLAYS or how it manages its
// environment (linesize, pagesize, fvwrap, fvwrapon, showemptycells,
// showomitted, scheme, tracedepth, linegap; adosize, niceness, reventries,
// maxdb, dtascomplevel, timeout1/2, min_memory, max_memory, segmentsize) are
// kept as values. Settings that change NUMBERS or DATA (maxiter, emptycells)
// accept only their default until the computation honours them (onlyDefault).
//
// PIET accepts the first list and does not act on it (a note says so), but
// Stata reports each one back through c(): `set linesize 100` then `display
// c(linesize)` prints 100, and a fresh session prints the edition's default
// (StataBE 19.5, the vs-stata truth bank of 2026-11-10). The value is checked
// as Stata checks it -- a number out of range or a word Stata does not take is
// refused with Stata's code -- and kept under the `set` marker
// (kSetMarkerPrefix + the full setting name), the store the dispatcher's
// generic `set` arm already writes. A setting with no measured default
// answers only once it has been set; before that c(NAME) keeps its refusal by
// name (cClassRefusalMessage), never a guessed value.
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "piet/cclass.h"

namespace piet {

namespace {

// Format: a numeric display format, or nothing (`set cformat` alone restores
// the edition's empty default). Fixed: Stata takes the word and keeps its own
// value (`set virtual on` leaves c(virtual) off).
enum class Kind { Integer, Memory, OnOff, Word, Format, Fixed };

struct Setting {
  Kind kind;
  // Integer: the inclusive range Stata takes, and the refusal it words.
  long long lo = 0, hi = 0;
  const char* rangeMessage = nullptr;
  // Word: the words Stata takes ("" = any single name, e.g. a scheme).
  std::vector<std::string> words;
  // The edition's value before any `set`; nullptr = unmeasured (c() refuses
  // until the setting is set).
  const char* initial = nullptr;
  // The shortest abbreviation Stata accepts for the setting word (0 = the full
  // name only).
  size_t minAbbrev = 0;
  // A setting that changes NUMBERS or DATA in Stata and that PIET does not
  // honour: only its default is accepted, any other value is refused with this
  // message (r(198)) rather than kept for c() and ignored by the computation.
  const char* onlyDefault = nullptr;
  // OnOff / Word: `set NAME` with no word is taken and keeps "" (`set lstretch`,
  // `set showbaselevels`: set_defaults resets them so).
  bool blankAllowed = false;
  // Word: an abbreviation of a listed word means the word (`set dp com`).
  bool wordPrefix = false;
  // Word: kept in lowercase (`set locale_functions en_US` reads back en_us).
  bool lowerValue = false;
  // Word: a word not in `words` names something Stata looks up and does not
  // find, r(601) "<what> WORD not found".
  const char* notFound = nullptr;
};

// Defaults and ranges: StataBE 19.5 as the truth bank measured them where it
// did (display c(linesize) 80, c(pagesize) 26, c(maxiter) 300, c(reventries)
// 5000; `set linesize 10` "linesize must be between 20 and 255", `set maxiter`
// "maxiter must be between 0 and 16000", `set adosize 2` "adosize must be
// between 10 and 10000", `set niceness 11` "niceness out of range"), else the
// shipped help files ([R] set iter, [D] memory, [R] db, [P] trace, [R] set
// showbaselevels, [D] set dtascomplevel, [U] reventries, [P] sysdir for
// adosize 1000, [R] set_timeout for timeout1 30 / timeout2 180).
const std::map<std::string, Setting>& settingTable() {
  static const std::map<std::string, Setting> kTable = {
      {"linesize", {Kind::Integer, 20, 255, "linesize must be between 20 and 255", {}, "80", 2}},
      {"pagesize", {Kind::Integer, 1, 10000, nullptr, {}, "26", 0}},
      {"maxiter",
       {Kind::Integer, 0, 16000, "maxiter must be between 0 and 16000", {}, "300", 0,
        "set maxiter: only the default, 300, is implemented in this version of PIET -- its "
        "iterative estimators do not read this setting, so another value is refused rather "
        "than kept and ignored (use the estimator's own iterate() option where it has one)"}},
      {"adosize", {Kind::Integer, 10, 10000, "adosize must be between 10 and 10000", {}, "1000", 0}},
      {"niceness",
       {Kind::Integer, 0, 10, "niceness out of range\n    niceness must be between 0 and 10", {},
        "5", 0}},
      {"reventries",
       {Kind::Integer, 5, 32000, "reventries must be between 5 and 32000", {}, "5000", 0}},
      {"maxdb", {Kind::Integer, 5, 1000, "maxdb must be between 5 and 1000", {}, "50", 0}},
      {"tracedepth", {Kind::Integer, 1, 32000, nullptr, {}, "32000", 6}},
      {"dtascomplevel",
       {Kind::Integer, 0, 9, "dtascomplevel must be between 0 and 9", {}, "1", 0}},
      {"fvwrap", {Kind::Integer, 0, 1000000, nullptr, {}, "1", 0}},
      {"linegap", {Kind::Integer, 0, 1000000, nullptr, {}, nullptr, 0}},
      {"timeout1", {Kind::Integer, 0, 1000000, nullptr, {}, "30", 0}},
      {"timeout2", {Kind::Integer, 0, 1000000, nullptr, {}, "180", 0}},
      {"min_memory", {Kind::Memory, 0, 0, nullptr, {}, "0", 0}},
      {"max_memory", {Kind::Memory, 0, 0, nullptr, {}, ".", 0}},
      {"segmentsize", {Kind::Memory, 0, 0, nullptr, {}, "33554432", 0}},
      {"emptycells",
       {Kind::Word, 0, 0, nullptr, {"keep", "drop"}, "keep", 0,
        "set emptycells drop is a valid Stata setting that this version of PIET does not yet "
        "implement -- estimation keeps the empty cells of an interaction in e(b), so it is "
        "refused rather than kept and ignored"}},
      {"fvwrapon", {Kind::Word, 0, 0, nullptr, {"word", "width"}, "word", 0}},
      {"scheme", {Kind::Word, 0, 0, nullptr, {}, "stcolor", 0}},
      {"showemptycells", {Kind::OnOff, 0, 0, nullptr, {}, nullptr, 0}},
      {"showomitted", {Kind::OnOff, 0, 0, nullptr, {}, nullptr, 0}},
      // Output, trace, Mata, network and interface settings Stata reports back
      // through c() (vs-stata bank, StataBE 19.5: `display "`c(NAME)'"` in a
      // fresh session, and after `set NAME word`); a default the bank did not
      // print is the factory value `set_defaults` restores, else unmeasured.
      // A style setting takes a collect style that exists: PIET knows only the
      // factory one, and another name is r(601) at the `set` (`set table_style
      // classic` is "collect style classic not found", 36d84d7e21127fbb).
      {"dp", {Kind::Word, 0, 0, nullptr, {"comma", "period"}, "period", 0, nullptr, false, true}},
      {"showbaselevels",
       {Kind::Word, 0, 0, nullptr, {"on", "off", "all"}, "", 0, nullptr, true}},
      {"cformat", {Kind::Format, 0, 0, nullptr, {}, "", 0}},
      {"pformat", {Kind::Format, 0, 0, nullptr, {}, "", 0}},
      {"sformat", {Kind::Format, 0, 0, nullptr, {}, "", 0}},
      {"coeftabresults", {Kind::OnOff, 0, 0, nullptr, {}, "on", 0}},
      {"dots", {Kind::OnOff, 0, 0, nullptr, {}, nullptr, 0}},
      {"fvlabel", {Kind::OnOff, 0, 0, nullptr, {}, "on", 0}},
      {"logtype",
       {Kind::Word, 0, 0, nullptr, {"text", "smcl"}, "smcl", 0, nullptr, false, true}},
      {"lstretch", {Kind::OnOff, 0, 0, nullptr, {}, nullptr, 0, nullptr, true}},
      {"matafavor", {Kind::Word, 0, 0, nullptr, {"space", "speed"}, "space", 0}},
      {"matastrict", {Kind::OnOff, 0, 0, nullptr, {}, "off", 0}},
      {"matalnum", {Kind::OnOff, 0, 0, nullptr, {}, "off", 0}},
      {"mataoptimize", {Kind::OnOff, 0, 0, nullptr, {}, "on", 0}},
      {"traceexpand", {Kind::OnOff, 0, 0, nullptr, {}, "on", 0}},
      {"tracesep", {Kind::OnOff, 0, 0, nullptr, {}, "on", 0}},
      {"traceindent", {Kind::OnOff, 0, 0, nullptr, {}, "on", 0}},
      {"tracenumber", {Kind::OnOff, 0, 0, nullptr, {}, "off", 0}},
      {"tracehilite", {Kind::Word, 0, 0, nullptr, {}, "", 0, nullptr, true}},
      {"virtual", {Kind::Fixed, 0, 0, nullptr, {"on", "off"}, "off", 0}},
      {"buildfvinfo", {Kind::OnOff, 0, 0, nullptr, {}, nullptr, 0}},
      {"checksum", {Kind::OnOff, 0, 0, nullptr, {}, "off", 0}},
      {"collect_double", {Kind::OnOff, 0, 0, nullptr, {}, "on", 0}},
      {"collect_label", {Kind::Word, 0, 0, nullptr, {"default"}, "default", 0}},
      {"collect_style",
       {Kind::Word, 0, 0, nullptr, {"default"}, "default", 0, nullptr, false, false, false,
        "collect style"}},
      {"collect_warn", {Kind::OnOff, 0, 0, nullptr, {}, "on", 0}},
      {"table_style",
       {Kind::Word, 0, 0, nullptr, {"table"}, "table", 0, nullptr, false, false, false,
        "collect style"}},
      {"etable_style",
       {Kind::Word, 0, 0, nullptr, {"etable"}, "etable", 0, nullptr, false, false, false,
        "collect style"}},
      {"dtable_style",
       {Kind::Word, 0, 0, nullptr, {"dtable"}, "dtable", 0, nullptr, false, false, false,
        "collect style"}},
      // [TABLES] set tabulate_style: default `tabulate` (the shipped help);
      // another name is r(601) (vs-stata bank 844d0c28936cf378 `set
      // tabulate_style classic`).
      {"tabulate_style",
       {Kind::Word, 0, 0, nullptr, {"tabulate"}, "tabulate", 0, nullptr, false, false, false,
        "collect style"}},
      {"copycolor",
       {Kind::Word, 0, 0, nullptr, {"automatic", "asis", "gs1", "gs2", "gs3"}, "automatic", 0}},
      {"printcolor",
       {Kind::Word, 0, 0, nullptr, {"automatic", "asis", "gs1", "gs2", "gs3"}, "automatic", 0}},
      {"docx_hardbreak", {Kind::OnOff, 0, 0, nullptr, {}, "off", 0}},
      {"httpproxy", {Kind::OnOff, 0, 0, nullptr, {}, "off", 0}},
      {"playsnd", {Kind::OnOff, 0, 0, nullptr, {}, "off", 0}},
      {"revkeyboard", {Kind::OnOff, 0, 0, nullptr, {}, nullptr, 0}},
      {"smoothfonts", {Kind::OnOff, 0, 0, nullptr, {}, "on", 0}},
      {"sortmethod", {Kind::Word, 0, 0, nullptr, {}, "default", 0}},
      {"update_query", {Kind::OnOff, 0, 0, nullptr, {}, "off", 0}},
      {"locale_functions", {Kind::Word, 0, 0, nullptr, {}, nullptr, 0, nullptr, false, false, true}},
  };
  return kTable;
}

// The full setting name a typed word means, or "" when it names none of these.
std::string fullSettingName(const std::string& word) {
  for (const auto& [full, s] : settingTable()) {
    if (word == full) return full;
    if (s.minAbbrev > 0 && word.size() >= s.minAbbrev && word.size() < full.size() &&
        full.compare(0, word.size(), word) == 0)
      return full;
  }
  return std::string();
}

std::string lowerAscii(std::string v) {
  for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return v;
}

std::string markerKey(const std::string& full) { return std::string(kSetMarkerPrefix) + full; }

// `#[b|k|m|g]` in bytes (the [D] memory units; k = 1024), or `.` for no limit.
std::optional<std::string> memoryAmount(const std::string& arg) {
  if (arg == ".") return arg;
  static const std::regex re(R"(^(\d+)([bBkKmMgG]?)$)");
  std::smatch m;
  if (!std::regex_match(arg, m, re)) return std::nullopt;
  double v = std::strtod(m.str(1).c_str(), nullptr);
  const char u = m.str(2).empty() ? 'b' : static_cast<char>(std::tolower(m.str(2)[0]));
  if (u == 'k') v *= 1024.0;
  else if (u == 'm') v *= 1024.0 * 1024.0;
  else if (u == 'g') v *= 1024.0 * 1024.0 * 1024.0;
  if (!(v < 9.0e15)) return std::nullopt;
  return std::to_string(static_cast<long long>(v));
}

}  // namespace

std::optional<CValue> settingCValue(const std::string& name, SessionState& session) {
  // The edition's fixed limits (StataBE 19.5 truth bank: c(max_N_theory)
  // 2.147e+09, the r(N_max) every describe posts, +1.ffffff8c00000X+01e;
  // c(max_k_theory) 2048; c(max_width_theory) 1048576; c(min_matsize) 10 and
  // c(max_matsize) 800; c(namelenchar) 32).
  static const std::map<std::string, const char*> kFixed = {
      {"max_N_theory", "2147483619"}, {"max_k_theory", "2048"},
      {"max_width_theory", "1048576"}, {"min_matsize", "10"},
      {"max_matsize", "800"},         {"namelenchar", "32"},
  };
  if (auto it = kFixed.find(name); it != kFixed.end()) return CValue{false, it->second};
  // c(mode): "batch" when Stata runs a do-file in batch mode (the truth bank's
  // `display c(MP) " " c(SE) " " c(mode)` prints `0 0 batch`), "" in the
  // interactive window. PIET's headless `--do` run, the one with no window to
  // ask (SessionState::hasRequestPostEvent false), is that batch run.
  if (name == "mode") return CValue{true, session.hasRequestPostEvent() ? "" : "batch"};
  const auto& table = settingTable();
  const auto it = table.find(name);
  if (it == table.end()) return std::nullopt;
  const Setting& s = it->second;
  const bool isString = s.kind != Kind::Integer && s.kind != Kind::Memory;
  if (const std::string* v = session.eval.globals.find(markerKey(name))) {
    // A marker this table did not write (a `set` before the setting was kept
    // here) holds no value: refused by name, as before.
    if ((v->empty() && !s.blankAllowed && s.kind != Kind::Format) ||
        (!v->empty() && (*v)[0] == '\x01'))
      return std::nullopt;
    return CValue{isString, *v};
  }
  if (s.initial == nullptr) return std::nullopt;
  return CValue{isString, s.initial};
}

void forgetAbbreviatedSetting(const std::string& word, SessionState& session) {
  if (word.empty()) return;
  for (const auto& [full, s] : settingTable()) {
    (void)s;
    if (word.size() < full.size() && full.compare(0, word.size(), word) == 0)
      session.eval.globals.set(markerKey(full), std::string(1, '\x01'));
  }
}

bool isKeptStataSetting(const std::string& word) { return !fullSettingName(word).empty(); }

std::optional<SettingRefusal> storeStataSetting(const std::string& word,
                                                const std::vector<std::string>& args,
                                                SessionState& session, bool* handled) {
  *handled = false;
  const std::string full = fullSettingName(word);
  if (full.empty()) return std::nullopt;
  *handled = true;
  const Setting& s = settingTable().at(full);
  // A value this table cannot vouch for (a range or wording Stata's answer to
  // is unmeasured) is accepted, as every ignored setting is, but not kept: the
  // marker then says "set to something", and c() refuses by name.
  const auto unkept = [&]() -> std::optional<SettingRefusal> {
    session.eval.globals.set(markerKey(full), std::string(1, '\x01'));
    return std::nullopt;
  };
  const std::string arg = args.empty() ? std::string() : args.front();
  std::string value;
  switch (s.kind) {
    case Kind::Integer: {
      // The number as `set matsize` takes it (route_session.cpp's matsize arm,
      // measured): an integer, else `ARG found where number expected` (a word
      // ending in a point takes two blanks); a second word is r(101).
      if (args.size() > 1) return SettingRefusal{"varlist not allowed", "101"};
      static const std::regex reInteger(R"(^[+-]?\d+$)");
      if (!std::regex_match(arg, reInteger)) {
        return SettingRefusal{arg.empty() ? std::string("nothing found where number expected")
                                          : arg + (arg.back() == '.' ? "  " : " ") +
                                                "found where number expected",
                              "198"};
      }
      const long long n = std::strtoll(arg.c_str(), nullptr, 10);
      if (n < s.lo || n > s.hi) {
        if (s.rangeMessage == nullptr) return unkept();  // range unmeasured
        return SettingRefusal{s.rangeMessage, "198"};
      }
      value = std::to_string(n);
      break;
    }
    case Kind::Memory: {
      if (args.size() != 1) return unkept();
      const std::optional<std::string> bytes = memoryAmount(arg);
      if (!bytes) return unkept();  // Stata's wording for a bad amount is unmeasured
      value = *bytes;
      break;
    }
    case Kind::OnOff: {
      // As `set rmsg|trace|varabbrev` (route_session.cpp, StataBE 19.5).
      if (args.empty() && s.blankAllowed) break;
      if (args.empty()) return SettingRefusal{"on or off required", "100"};
      if (args.size() > 1) return SettingRefusal{"varlist not allowed", "101"};
      if (arg != "on" && arg != "off")
        return SettingRefusal{"'" + arg + "' found where on or off expected", "198"};
      value = arg;
      break;
    }
    case Kind::Word: {
      if (args.empty() && s.blankAllowed) break;
      if (args.size() != 1) return unkept();
      // A quoted word is the word (`set tracehilite "display"`).
      std::string word = arg;
      if (word.size() >= 2 && word.front() == '"' && word.back() == '"')
        word = word.substr(1, word.size() - 2);
      if (!s.words.empty()) {
        std::string hit;
        const std::string typed = s.wordPrefix ? lowerAscii(word) : word;
        for (const auto& w : s.words) {
          if (typed == w || (s.wordPrefix && !typed.empty() && typed.size() < w.size() &&
                             w.compare(0, typed.size(), typed) == 0))
            hit = w;
        }
        if (hit.empty() && s.notFound != nullptr)
          return SettingRefusal{std::string(s.notFound) + " " + word + " not found", "601"};
        if (hit.empty()) return unkept();  // Stata's wording here is unmeasured
        word = hit;
      }
      value = s.lowerValue ? lowerAscii(word) : word;
      break;
    }
    case Kind::Format: {
      // Nothing restores the default; a numeric format is kept as typed
      // (route_session.cpp refuses any other word, r(7), before this runs).
      if (args.empty()) break;
      if (args.size() != 1) return unkept();
      static const std::regex reFmt(R"(^%-?0?[0-9]+\.[0-9]+[fge]c?$)");
      if (!std::regex_match(arg, reFmt)) return unkept();
      value = arg;
      break;
    }
    case Kind::Fixed: {
      // Taken and ignored: c() keeps the edition's value.
      if (args.size() != 1) return unkept();
      bool ok = false;
      for (const auto& w : s.words) ok = ok || arg == w;
      if (!ok) return unkept();
      value = s.initial != nullptr ? s.initial : "";
      break;
    }
  }
  if (s.onlyDefault != nullptr && s.initial != nullptr && value != s.initial)
    return SettingRefusal{s.onlyDefault, "198"};
  session.eval.globals.set(markerKey(full), value);
  return std::nullopt;
}

}  // namespace piet
