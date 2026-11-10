// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Logging commands.

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
#include "piet/handlers/session_commands.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace command_detail {}
using namespace command_detail;
using namespace session_vocab;

ojson runLog(const std::string& raw, SessionState& session) {
  // Tokenize on whitespace to find the sub-keyword (varlist[0] equivalent) —
  // "log" itself is the command word, so the first token after it is the sub.
  // BUG-251/WS-Q: was `rawLower.find("log")` + `substr(pos + 3)` — the fifth
  // member of BUG-232's family, in the same file as two of the four the entry
  // named, and missed by its grep because that grep only searched for the
  // command words the arc was widening. `log` has no Stata abbreviation today
  // (measured), so this was latent, not live; it is repaired anyway because
  // "latent" here means "one registry edit from killing the run".
  //
  // No spelling literal and no npos arithmetic: the sub-keyword is whatever
  // follows the leading token, and the leading token is the command word by
  // construction (eval::tokenize sets pc.raw = strip(line)). Kept local rather
  // than sharing dispatcher.cpp's rawArgsAfterCmd() because that helper lives
  // in dispatcher's anonymous namespace; this carries no command-word
  // knowledge, so it is not a second copy of a spelling set.
  std::string after;
  {
    size_t b = raw.find_first_not_of(" \t");
    if (b != std::string::npos) {
      size_t e = raw.find_first_of(" \t", b);
      if (e != std::string::npos) after = trim(raw.substr(e));
    }
  }
  std::string afterLower = lower(after);
  size_t subEnd = afterLower.find_first_of(" \t");
  std::string sub = lower(subEnd == std::string::npos ? after : after.substr(0, subEnd));

  if (sub == "using") {
    static const std::regex reUsing(R"re(\busing\s+("([^"]+)"|'([^']+)'|(\S+)))re",
                                     std::regex::icase);
    std::smatch m;
    std::string path;
    if (std::regex_search(raw, m, reUsing)) {
      if (m[2].matched) {
        path = m[2].str();
      } else if (m[3].matched) {
        path = m[3].str();
      } else {
        path = m[4].str();
        // BS-006: the bare/unquoted alternative's `\S+` is greedy and a
        // comma is not whitespace, so `log using file.log, replace` (no
        // space before the comma — the common style) swallows the comma
        // into `path` too. Quoted paths (m[2]/m[3] above) never hit this —
        // they stop at the closing quote — so the strip is scoped to this
        // branch only; a bare path can never legitimately end in a comma.
        if (!path.empty() && path.back() == ',') path.pop_back();
      }
    }
    if (!path.empty()) {
      // resolve relative to session.cwd, matching shims.py's _resolve(session, path).
      namespace fs = std::filesystem;
      fs::path p(path);
      if (p.is_relative()) {
        std::string base = session.cwd.has_value() && !session.cwd->empty() ? *session.cwd
                            : fs::current_path().u8string();
        p = cecon::to_native_path(base) / p;
      }
      path = p.lexically_normal().u8string();
    }
    if (path.empty()) {
      return engineErrorBlock("log using: no filename given");
    }

    // BS-006: parse the post-comma option tail the SAME quote/paren-aware
    // way every other command does (BS-018's `do FILE, nostop` precedent —
    // dispatcher.cpp). The `using` regex above already extracts the
    // filename regardless of where the comma sits, so re-splitting `raw`
    // here for the options tail is safe.
    std::string optsTail;
    long long commaPos = eval::findUnquotedComma(raw);
    if (commaPos >= 0) optsTail = trim(raw.substr(static_cast<size_t>(commaPos) + 1));
    auto opts = eval::parseOpts(optsTail);
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kLogKnownOpts = kVocabLog.known;
    // BUG-262 — a PER-NET exception to the dispatcher's single resolution
    // point, and the reason there has to be one. That point canonicalizes
    // `parsed.opts`; this handler does not read `parsed.opts`. It re-parses its
    // own option list out of `raw` (it has to: `log using "f, g.log", replace`
    // needs the quote-aware split), so the vector below never passed through
    // the dispatcher and arrives with raw keys. Measured: `log using f.log,
    // repl` was accepted by the net and then dropped by the loop underneath,
    // so the log was NOT replaced. Same lesson as BUG-232 — "one resolution
    // point" is a claim about ONE VECTOR, and a second parse is a second
    // vector. Uses the registered vocabulary, so still no second copy of the
    // spellings.
    eval::canonicalizeOptionKeys(opts, kLogKnownOpts);
    if (auto err = eval::rejectUnknownOptions(opts, kLogKnownOpts)) {
      return ojson(*err);
    }
    bool wantReplace = false, wantAppend = false, wantText = false, wantSmcl = false;
    std::string logName;
    for (const auto& [k, v] : opts) {
      if (k == "replace") {
        wantReplace = true;
      } else if (k == "append") {
        wantAppend = true;
      } else if (k == "text") {
        wantText = true;
      } else if (k == "smcl") {
        wantSmcl = true;
      } else if (k == "name") {
        // W:P8: `name(X)`. Stata also accepts `name(X, replace)`; only the
        // name itself is meaningful here (the file-level replace/append
        // decision is the option pair above), so take the first token.
        logName = v.isBool ? std::string() : trim(v.strValue);
        size_t comma = logName.find(',');
        if (comma != std::string::npos) logName = trim(logName.substr(0, comma));
        if (logName.empty()) {
          return engineErrorBlock("log using: option `name()' requires a log name",
                                   "syntax_error");
        }
      }
    }
    if (wantReplace && wantAppend) {
      return engineErrorBlock("log using: `replace' and `append' may not be combined",
                               "syntax_error");
    }
    if (wantText && wantSmcl) {
      return engineErrorBlock("log using: `text' and `smcl' may not be combined",
                               "syntax_error");
    }

    // Format selection and the file's extension (see this function's
    // top-of-doc comment). The extension is the filename's own, never a dot in
    // a directory name.
    const size_t nameStart = path.find_last_of("/\\");
    const std::string fileName = nameStart == std::string::npos ? path : path.substr(nameStart + 1);
    const size_t dot = fileName.rfind('.');
    const bool hasExtension = dot != std::string::npos && dot > 0;
    bool useSmcl;
    if (wantSmcl) {
      useSmcl = true;
    } else if (wantText) {
      useSmcl = false;
    } else if (!hasExtension) {
      useSmcl = true;  // `set logtype` default (not settable in this build)
    } else {
      useSmcl = lower(fileName.substr(dot)) == ".smcl";
    }
    if (!hasExtension) path += useSmcl ? ".smcl" : ".log";

    // W:P8: the clash is per NAME, not per session — a named log may be opened
    // while the unnamed one is running (that is the whole point of `name()`,
    // and what the P8 target does), but reopening a name that is already
    // logging is Stata's own hard stop: r(604) "log file already open", for the
    // unnamed log and a named one alike (BUG-1463; StataBE 19.5, it was rc 1).
    if (session.logIsOpen(logName)) return engineErrorBlockRc("log file already open", 604);

    bool appendMode = wantAppend;
    if (!wantReplace && !wantAppend && cecon::native_exists(path)) {
      return engineErrorBlock(
          "log using: file already exists (specify `replace' or `append')");
    }
    // The `log using` command is itself running inside a dispatch frame, so
    // its own depth is the depth of the sibling commands this log must record.
    if (!session.openLog(path, appendMode, logName, session.dispatchDepth(), useSmcl)) {
      return engineErrorBlock("log using: could not open \"" + path + "\" for writing");
    }
    return ojson::array();  // Stata: silent when log opens successfully
  }

  // `close`/`off`/`on` take an optional log name (or `_all`) after the
  // sub-keyword: `log close p8named`, `log off _all`.
  std::string target;
  if (subEnd != std::string::npos) target = trim(after.substr(subEnd));
  const bool targetAll = lower(target) == "_all";
  if (targetAll) target.clear();

  if (sub == "close") {
    if (targetAll) {
      session.closeAllLogs();
      return ojson::array();
    }
    // Naming a log that is not open is Stata's r(111) "log NAME not found"
    // (live-verified). A bare `log close` with nothing open stays the silent
    // no-op it has always been.
    if (!session.closeLog(target) && !target.empty()) {
      return engineErrorBlock("log " + target + " not found");
    }
    return ojson::array();  // Stata: silent when log closes
  }

  if (sub == "off") {
    if (targetAll) {
      for (const auto& n : session.logNames()) session.pauseLog(n);
      return ojson::array();
    }
    if (!session.pauseLog(target) && !target.empty()) {
      return engineErrorBlock("log " + target + " not found");
    }
    return ojson::array();  // Stata: silent when log pauses
  }

  if (sub == "on") {
    if (targetAll) {
      for (const auto& n : session.logNames()) session.resumeLog(n);
      return ojson::array();
    }
    if (!session.resumeLog(target) && !target.empty()) {
      return engineErrorBlock("log " + target + " not found");
    }
    return ojson::array();  // Stata: silent when log resumes
  }

  // Unknown subcommand -- silent noop (unchanged from BUG-028's original shim).
  return ojson::array();
}

// ---------------------------------------------------------------------------
// U7/S9 offline shims: which / ssc / confirm / mkdir / erase — mirrors
// piet/backend/piet/shims.py (Python oracle) + router/commands.py's parse.
// All are no-data-needed (kNoDataCommands) so classroom do-files that guard
// package/file existence before `use` never halt. analysis.do's line-77
// package-presence loop (`capture which drdid` / `capture ssc install ...`)
// and its dozen `capture confirm variable/numeric/string ...` guards
// (throughout the depvar/control setup) exercise all five directly.


}  // namespace piet::handlers
