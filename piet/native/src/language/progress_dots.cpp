// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
//   _dots     display blocks; the mark's "no newline" is the display block's
//             `continue` flag, which doexec already joins (`display ... _continue`).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "piet/cclass.h"             // substituteCRefs: the dispatcher's own pre-pass for a definition line
#include "piet/datastore.h"
#include "piet/dispatcher.h"         // markout: a dispatched `quietly replace`
#include "piet/display_hardening.h"  // fmtStataDefaultG: what `display <number>` prints
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"        // hasMacroRefs, tryCaptureDefinition
#include "piet/eval/pyvalue.h"       // pythonFloatRepr: a round-trippable number for $S_3/$S_6/$S_7
#include "piet/eval/session.h"       // replaceRClass
#include "piet/eval/tokenizer.h"
#include "piet/handlers/session_commands.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"       // renderValue/padToField: the one %w.pf renderer
#include "piet/strutil.h"
#include "command_reply_support.h"

namespace {

// _dots' `syntax` line: title() reps() NODOTS DOTS() MAXDOTS(). All five read.
// KNOWN (review d1 D6/R10): Stata's lowercase/all-caps spellings admit no
// abbreviation, so `_dots 0, ti(T)` and `require estout, inst` are r(198)
// there, while the registry's prefix rule accepts them here.
const piet::eval::OptVocab& kVocabDots = piet::eval::registerOptVocab(
    {"_dots"}, {{"title", "reps", "nodots", "dots", "maxdots"}, {}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace command_reply_detail;

namespace {

std::string dequoteOpt(std::string s) {
  s = strutil::trim(s);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
  if (s.size() >= 4 && s.compare(0, 2, "`\"") == 0 && s.compare(s.size() - 2, 2, "\"'") == 0)
    return s.substr(2, s.size() - 4);
  return s;
}

}  // namespace

// ===========================================================================
// _dots
// ===========================================================================
//
// Implements _dots per _dots's ado (it has no help file); identified against
// StataBE probes (w2ax_t1_official.log dots_header, dots_loop, dots_x; envp2
// dots_*). `_dots 0, title(T) reps(#)` prints the title line (`T (#)` with the
// count comma-grouped, `T` alone without reps) and the 50-column ruler (not
// under `nodots`); `_dots i rc` prints one mark with no newline -- `.` for
// rc 0/text/result/input, `x` for 1/error, `e` 2, `n` 3, `s` -1, `?` anything
// else -- and every 50*dots()th mark closes its line with ` %5.0fc i`;
// `_dots i` alone ends a line that is not at a 50-mark boundary, and a bare
// `_dots` ends the line. A mark is a display block with `continue`, which
// doexec joins onto the next display (the `display ... _continue` mechanism).
nlohmann::ordered_json runDots(const eval::ParsedCommand& parsed, SessionState& session) {
  (void)session;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabDots.known, kVocabDots.unimplemented))
    return ojson(*err);
  // reps(numlist integer max=1), dots()/maxdots() (numlist >0 integer max=1).
  auto intOpt = [&](const std::string& key, bool positive, long long* out) -> std::optional<ojson> {
    const std::optional<std::string> s = optStr(parsed.opts, key);
    if (!s.has_value()) return std::nullopt;
    const std::optional<double> v = parseNumber(*s);
    if (!v.has_value()) return envError(key + "() invalid -- invalid numlist", 121);
    if (std::floor(*v) != *v)
      return envError(key + "() invalid -- invalid number, noninteger value", 126);  // envp2 dots_badreps
    if (positive && *v <= 0)
      return envError(key + "() invalid -- invalid numlist has elements outside of allowed range",
                      125);
    *out = static_cast<long long>(*v);
    return std::nullopt;
  };
  long long reps = -1, dots = 1, maxdots = 0;
  if (auto e = intOpt("reps", false, &reps)) return *e;
  if (auto e = intOpt("dots", true, &dots)) return *e;
  if (auto e = intOpt("maxdots", true, &maxdots)) return *e;
  const bool haveMaxdots = optStr(parsed.opts, "maxdots").has_value();

  const std::vector<std::string> toks = splitWs(parsed.argText);
  const std::string i = toks.size() > 0 ? toks[0] : std::string();
  const std::string rc = toks.size() > 1 ? toks[1] : std::string();

  if (i == "0") {
    ojson out = ojson::array();
    const std::string title = dequoteOpt(optStr(parsed.opts, "title").value_or(std::string()));
    if (!title.empty()) {
      out.push_back(displayBlock(reps > 0 ? title + " (" +
                                                strutil::trim(fixedFmt(static_cast<double>(reps), 26, 0, true)) +
                                                ")"
                                          : title));
    }
    if (!hasOpt(parsed.opts, "nodots"))
      out.push_back(displayBlock("----+--- 1 ---+--- 2 ---+--- 3 ---+--- 4 ---+--- 5 "));
    return out;
  }
  if (i.empty()) return displayBlock("");  // envp2 dots_bare: a newline
  const std::optional<double> iv = parseNumber(i);
  if (!iv.has_value()) return envError(i + " not found", 111);  // envp2 dots_abc
  auto modOf = [](double x, double y) { return x - y * std::floor(x / y); };
  const double per = 50.0 * static_cast<double>(dots);
  if (rc.empty()) {
    // envp2 dots_noarg (`_dots 3`: a newline) and dots_50noarg (`_dots 50`: none).
    if (modOf(*iv, per) != 0) return displayBlock("");
    return ojson::array();
  }
  if (modOf(*iv, static_cast<double>(dots)) != 0) return ojson::array();
  std::string mark;
  if (rc == "0" || rc == "text" || rc == "result" || rc == "input") {
    mark = ".";
  } else if (rc == "-1") {
    mark = "s";
  } else if (rc == "1" || rc == "error") {
    mark = "x";
  } else if (rc == "2") {
    mark = "e";
  } else if (rc == "3") {
    mark = "n";
  } else {
    mark = "?";
  }
  if (modOf(*iv, per) == 0) {
    // envp2 dots_50: `.    50` and the line ends. maxdots() sizes the field
    // to its own comma-grouped width (_dots's CommaFormat).
    int width = 5;
    if (haveMaxdots)
      width = static_cast<int>(strutil::trim(fixedFmt(static_cast<double>(maxdots), 26, 0, true)).size());
    return displayBlock(mark + " " + fixedFmt(*iv, width, 0, true));
  }
  ojson b = displayBlock(mark);
  b["continue"] = true;
  return b;
}

}  // namespace piet::handlers
