// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "session_support.h"
#include "text_support.h"
#include <regex>
#include "options.h"
#include "handler_declarations.h"

namespace piet::dispatch_detail {


// BUG-1330: a version number as StataBE 19.5 prints it, in `version` and in its
// r(9) refusal: an integer with one decimal (15.0, 0.0, -1.0), anything else as
// a number without its leading zero (16.25, .5, .99).
std::string versionText(double v) {
  char buf[32];
  if (v == std::floor(v)) {
    std::snprintf(buf, sizeof(buf), "%.1f", v == 0.0 ? 0.0 : v);  // `version -0` is 0.0
    return buf;
  }
  std::string t = eval::fmtNumeric(v);
  if (t.rfind("0.", 0) == 0 || t.rfind("-0.", 0) == 0) t.erase(t.find('0'), 1);
  return t;
}


// BUG-1330: the number of `version #` / `version #:`, or the refusal StataBE 19.5
// gives. The part before the point, sign and leading zeros included, is one or
// two characters with a digit; at most two digits follow the point, and a bare
// trailing point only follows a one-character part. `05`, `+5`, `5.`, `1.55`,
// `01.50` and `+1.55` are numbers; `016`, `100`, `-10`, `+15`, `10.`, `+1.`,
// `1.555`, `19.500`, `.5`, `1e1` and `16.2.1` are r(198) invalid syntax. A
// number outside 1 to 19.5 is r(9): `version 0.5`, `99`, `00` and `-1` alike.
std::optional<ojson> parseVersionNumber(const std::string& tok, double* out) {
  static const std::regex reNum(R"(^(?:[+-]?\d|\d\d)(?:\.\d{1,2})?$|^\d\.$)");
  if (!std::regex_match(tok, reNum))
    return ojson(eval::errorBlock("syntax_error", "invalid syntax", std::nullopt, std::nullopt,
                                  std::nullopt, "r(198)"));
  const double v = std::strtod(tok.c_str(), nullptr);
  if (v < 1.0 || v > 19.5)
    return ojson(eval::errorBlock("engine_error",
                                  "this is version 19.5 of StataNow; it cannot run version " +
                                      versionText(v) + " programs",
                                  std::nullopt, std::nullopt, std::nullopt, "r(9)"));
  *out = v;
  return std::nullopt;
}


// BUG-1330: whether `version #` (or `version #:`) here sets c(userversion) too.
// StataBE 19.5 leaves it in a program body and in a loop body of the level now
// running (`forvalues`/`foreach`/`while`: 12|15 after `version 12` inside one,
// under `version 15`), and sets it at the top level, in an `if` block and in a
// do-file, also one run from a program or loop (CH2|12|12).
bool versionSetsUserVersion(SessionState& session) {
  return session.eval.locals.find(kProgramBodyLocal) == nullptr &&
         loops::loopBodyDepth() <= session.loopBodyFloor;
}


// BUG-1330: the setting a lowercased `set` word names, for those whose c() key
// PIET answers until they are set (cclass.cpp ignoredSettingCValue). StataBE 19.5
// takes `r` to `rmsg`, `tr` to `trace` and `mat` to `matsize`; scheme, varabbrev,
// fredkey, maxvar, segmentsize and max_memory only in full (`set sch` r(199)).
// Any other word is returned as it is.
std::string canonicalSetName(const std::string& word) {
  static const std::pair<std::string, size_t> kAbbrev[] = {
      {"rmsg", 1}, {"trace", 2}, {"matsize", 3}};
  for (const auto& [full, minLen] : kAbbrev)
    if (word.size() >= minLen && word.size() <= full.size() && full.compare(0, word.size(), word) == 0)
      return full;
  return word;
}

// The settings Stata's `set` knows ([R] set's list, the help files each one
// points to, and the settings StataBE 19.5 ran in the vs-stata truth bank of
// 2026-11-10), plus PIET's own `extcompat`. A typed word that is a prefix of
// any of them is left to the arms (abbreviations are per setting: `set lin`
// runs); a word that begins none is r(199) "-set X- not allowed; 'X' not
// recognized" (bank 26736b588799d6eb `set zznotasetting on`, 0a8c18ab3b8fa0a9
// `set noisily 1`).
namespace {
bool settingWordMatches(const std::string& w, bool full);
bool isStataSettingWord(const std::string& w) { return settingWordMatches(w, false); }
bool settingWordIsFull(const std::string& w) { return settingWordMatches(w, true); }
bool settingWordMatches(const std::string& w, bool exact) {
  static const char* const kSettings[] = {
      "adosize", "autotabgraphs", "buildfvinfo", "cformat", "charset", "checksum", "clevel",
      "coeftabresults", "collect_double", "collect_label", "collect_style", "collect_warn",
      "conren", "copycolor", "ctrlh", "dockable", "dockingguides", "docx_hardbreak",
      "docx_maxtable", "docx_paramode", "doeditbackup", "dots", "doublebuffer", "dp",
      "dtable_style", "dtascomplevel", "emptycells", "eolchar", "etable_style", "extcompat",
      "fastscroll", "floatwindows", "fredkey", "fvbase", "fvlabel", "fvtrack", "fvwrap",
      "fvwrapon", "graphics", "haverdir", "httpproxy", "httpproxyauth", "httpproxyhost",
      "httpproxyport", "httpproxypw", "httpproxyuser", "include_bitmap", "insertmode",
      "iterlog", "java_heapmax", "java_home", "kmp_blocktime", "lapack_mkl", "lapack_mkl_cnr",
      "lapack_openblas", "level", "linegap", "linesize", "locale_functions", "locale_ui",
      "locksplitters", "logmsg", "logtype", "lstretch", "matacache", "matafavor", "matalibs",
      "matalnum", "matamofirst", "mataoptimize", "matasolvetol", "matastrict", "matsize",
      "maxbezierpath", "maxdb", "maxiter", "max_memory", "max_preservemem", "maxvar", "memory",
      "min_memory", "more", "niceness", "notifyuser", "obs", "odbcdriver", "odbcmgr", "output",
      "pagesize", "pdf_maxtable", "persistfv", "persistvtopic", "pformat", "piccomments",
      "pinnable", "playsnd", "prefix", "printcolor", "processors", "python_exec",
      "python_userpath", "reshape_favor", "reventries", "revkeyboard", "rmsg", "rng",
      "rngstate", "rngstream", "scheme", "scrollbufsize", "searchdefault", "seed",
      "segmentsize", "sformat", "showbaselevels", "showemptycells", "showomitted",
      "smoothfonts", "smoothsize", "sortmethod", "sortrngstate", "sortseed", "svy_tab_comps",
      "table_style", "tabulate_comps", "tabulate_style", "taskbargroups", "timeout1",
      "timeout2", "trace", "tracedepth", "traceexpand", "tracehilite", "traceindent",
      "tracenumber", "tracesep", "type", "update_interval", "update_prompt", "update_query",
      "use_atsui_graph", "use_qd_text", "varabbrev", "varkeyboard", "varlabelpos", "virtual",
      "window"};
  for (const char* s : kSettings) {
    const std::string full(s);
    if (exact ? w == full : (w.size() <= full.size() && full.compare(0, w.size(), w) == 0))
      return true;
  }
  return false;
}

}  // namespace

bool isFullStataSettingWord(const std::string& w) {
  static const char* const kExtra[] = {"rmsg", "trace", "matsize"};
  for (const char* s : kExtra)
    if (w == s) return true;
  return isStataSettingWord(w) && !w.empty() && settingWordIsFull(w);
}

namespace {
bool isPrefixOfAny(const std::string& w, std::initializer_list<const char*> words) {
  if (w.empty()) return false;
  for (const char* s : words) {
    const std::string full(s);
    if (w.size() <= full.size() && full.compare(0, w.size(), w) == 0) return true;
  }
  return false;
}
}  // namespace

std::optional<std::pair<std::string, std::string>> checkedStataSetWord(
    const eval::ParsedCommand& parsed, bool hasData) {
  (void)hasData;  // only Stata/BE's `set maxvar` refusal read it (see below)
  using R = std::optional<std::pair<std::string, std::string>>;
  if (parsed.varlist.empty()) return std::nullopt;
  const std::string& typed = parsed.varlist[0];
  const std::string name = lower(typed);
  if (!isStataSettingWord(name))
    return R({"-set " + typed + "- not allowed; '" + typed + "' not recognized", "r(199)"});
  const std::string arg = parsed.varlist.size() > 1 ? parsed.varlist[1] : std::string();
  const bool oneArg = parsed.varlist.size() == 2;
  // The value checks, each a StataBE 19.5 refusal in the bank (the setting typed
  // in full; an abbreviation keeps the arms' answer).
  if (name == "cformat" || name == "pformat" || name == "sformat") {
    // A numeric display format (%9.3f, %8.2gc, ...) or nothing; `set cformat
    // zz` is r(7) "invalid numeric format" (c450558ca51e5270).
    static const std::regex reFmt(R"(^%-?0?[0-9]+\.[0-9]+[fge]c?$)");
    if (oneArg && !std::regex_match(arg, reFmt)) return R({"invalid numeric format", "r(7)"});
  } else if (name == "dp") {
    // comma or period; `set dp zz` r(198) "invalid syntax" (0c6beb4b6e4c1bcd).
    if (oneArg && !isPrefixOfAny(lower(arg), {"comma", "period"}))
      return R({"invalid syntax", "r(198)"});
  } else if (name == "logtype") {
    // text or smcl (7a3db216e53c18a3 `set logtype zz` r(198)).
    if (oneArg && !isPrefixOfAny(lower(arg), {"text", "smcl"}))
      return R({"syntax error:  the valid default logtypes are text and smcl", "r(198)"});
  } else if (name == "output") {
    // proc, inform or error; any other word is r(101) "<word> not allowed"
    // (803217bfd7438b99 `set output zz`, c13b1059a920c870 `set output proper`).
    if (oneArg && !isPrefixOfAny(lower(arg), {"proc", "inform", "error"}))
      return R({arg + " not allowed", "r(101)"});
  } else if (name == "showbaselevels") {
    // on, off or all (7d60d4719ad382b5 `set showbaselevels maybe` r(100)).
    if (oneArg && arg != "on" && arg != "off" && arg != "all")
      return R({"on, off, or all required", "r(100)"});
  } else if (name == "sortseed") {
    // An integer (c2e1ed90defa00ee `set sortseed abc` r(198) "invalid sortseed").
    static const std::regex reInt(R"(^[+-]?[0-9]+$)");
    if (oneArg && !std::regex_match(arg, reInt)) return R({"invalid sortseed", "r(198)"});
  }
  // `set maxvar #` is an accepted header line. Stata/BE refuses every one (r(198)
  // "maxvar not settable in this version of Stata"; r(4) with data in memory: bank
  // 5ccc22f778772dc5, 0dfc41893ad0f885, 5649416d00862867, c82dc0242371e27e), but it
  // is the usual first line of do-files written for Stata/SE and MP, which accept it
  // (tests/eval/piet2 test_131 `set maxvar 10000`). PIET follows Stata/SE and MP
  // where the editions differ (User ruling 2026-11-12).
  return std::nullopt;
}

}  // namespace piet::dispatch_detail
