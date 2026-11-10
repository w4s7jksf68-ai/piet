// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "session_support.h"
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
}  // namespace piet::dispatch_detail
