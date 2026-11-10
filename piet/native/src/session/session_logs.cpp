// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// SessionState: `log using/close/off/on`, log queries, and the per-frame transcript tee (text and SMCL writers).
#include "piet/session_state.h"
#ifndef _WIN32
#include <execinfo.h>
#include <dlfcn.h>
#endif
#include "io/win_compat.h"  // cecon::to_native_path (BUG-050 wide open)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdio>   // C4-L1 window trace (stderr fprintf, env-gated)
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>  // std::greater<> (BUG-381 census sort)
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>

#include "piet/bcal.h"  // bcal::clearCache (resetProcessCaches)
#include "piet/cclass.h"  // BUG-1006: resolveCValue (macroCValue)
#include "piet/counters.h"
#include "piet/eval/errors.h"  // eval::coarseRc (the r(N) of a logged error)
#include "piet/eval/macros.h"  // eScalarKey (eNameOrderFor)
#include "piet/display_hardening.h"  // BUG-718: readVarObservation (macroVarObs)
#include "piet/scratch_seq.h"  // BUG-389: scratchMintSeq (sweep throttle)
#include "piet/storage_type.h"  // stataDefaultFormat (displayFormat)

namespace piet {

namespace {
// ---------------------------------------------------------------------------
// SMCL log writer (`log using FILE.smcl`, or `log using FILE, smcl`).
//
// Implements the SMCL log format per the log help file. Everything byte-level
// below was identified against StataBE 19.5 captures of `log using x.smcl`
// (scratch-r150/t7-l5/stata/cap1..cap8), not guessed:
//
//  * A fresh file starts "{smcl}\n{txt}{sf}{ul off}{.-}\n" and four label
//    lines (name / log / log type / opened on); the label values are in {res}.
//    Appending to a file that already has content, and `log on`, start with
//    "{smcl}\n{com}{sf}{ul off}{txt}{.-}\n" instead (the closing marker of the
//    previous segment has no newline, so the two run together).
//  * Stata writes a mode tag ({txt} {res} {com} {err}) only where the mode
//    CHANGES. Each command is a blank line in {txt}, then "{com}. cmd"; a
//    display line is {res}; an error is {err} followed by
//    "{txt}{search r(N), local:r(N);}" unless the command was captured.
//  * Literal braces in an echoed command line are escaped one for one as
//    {c -(} and {c )-}. What `display` prints is NOT escaped: its text is SMCL
//    already (the user typed the directives), and Stata copies it through.
//    (Text any other command prints is plain; there the braces are escaped.)
//  * `log off` / `log close` write the command line, then a footer of the
//    same four labels (paused on / closed on), "{txt}{.-}\n{smcl}\n" and a
//    final "{txt}{sf}{ul off}" with no newline. `log on` writes the
//    "resumed on" header but not its own command line. A paused log is closed
//    silently. A log left open at the end of the run gets no footer.
//  * Under `quietly` / `capture` (without `noisily`) those banners and footers
//    are suppressed output: the file gets only "{smcl}\n{com}{sf}{ul off}"
//    (no newline) where each would stand. `cap log off X` / `cap log on X`,
//    the 073 spelling, is that case.
//
// What differs from StataBE, by design: PIET's result blocks are not Stata's
// rendered tables, so a non-display block is written as the same "key: value"
// dump the text log uses (in {txt} mode, braces escaped) rather than as the
// ado's SMCL table; the loop-body numbering ("  2. ...") and the "> " line
// continuations of an echoed command are not reproduced; and the console-side
// output of `log using` / `log off` (the "(file ... not found)" note and the
// printed header) is not part of PIET's blocks, so the unnamed log never
// contains another log's banner; and the OPENING banner is always the full one
// -- a `quietly log using` gets the short marker in StataBE (the same rule as
// above), but the banner is written when the log opens, before the command
// line that says how it was spelled has reached the tee. The depth rule of LogState::depth is the text
// log's own and is kept: a log opened under a prefix (`quietly log using ...`)
// belongs to the prefix's inner frame and records only what that depth carries.
// A frame that flattens a whole `do` file or loop is written when that frame
// ends; the `log off` / `log on` / `log close` lines inside it take effect where
// they stand in the frame.
constexpr char kModeTxt = 't';
constexpr char kModeRes = 'r';
constexpr char kModeCom = 'c';
constexpr char kModeErr = 'e';

const char* smclModeTag(char mode) {
  switch (mode) {
    case kModeTxt: return "{txt}";
    case kModeRes: return "{res}";
    case kModeCom: return "{com}";
    default: return "{err}";
  }
}

// Moves the stream to `mode`, writing the tag only on a change.
void smclSetMode(LogState& log, char mode) {
  if (log.smclMode == mode) return;
  log.stream << smclModeTag(mode);
  log.smclMode = mode;
}

// Literal braces as SMCL spells them, one for one.
std::string smclEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    if (c == '{') {
      out += "{c -(}";
    } else if (c == '}') {
      out += "{c )-}";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// "28 Oct 2026, 02:46:04": the day is blank-padded like c(current_date)
// (cclass.cpp), the clock zero-padded like c(current_time). The captures were
// taken on a two-digit day, so the blank padding of 1-9 is c(current_date)'s
// convention, not something a header capture showed.
std::string smclStamp() {
  static const char* const kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const std::time_t t = std::time(nullptr);
  std::tm tmv{};
#ifdef _WIN32
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  const int mon = (tmv.tm_mon >= 0 && tmv.tm_mon < 12) ? tmv.tm_mon : 0;
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%2d %s %04d, %02d:%02d:%02d", tmv.tm_mday, kMon[mon],
                tmv.tm_year + 1900, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  return std::string(buf);
}

// The name / log / log type lines. `nameLinePrefix` is what precedes "name:" --
// the header's line follows the {txt} that ends its first line, the footers'
// carries its own {txt}.
void smclLabelLines(LogState& log, const char* nameLinePrefix) {
  log.stream << nameLinePrefix << "name:  {res}"
             << (log.name.empty() ? std::string("<unnamed>") : smclEscape(log.name)) << "\n"
             << "       {txt}log:  {res}" << smclEscape(log.path) << "\n"
             << "  {txt}log type:  {res}smcl\n";
}

// Under `quietly` / `capture` (no `noisily`) the banner and footer text are
// output the command suppresses, and only the structural markers reach the
// file: "{smcl}\n{com}{sf}{ul off}", with no newline after it.
void smclWriteQuietMarker(LogState& log) {
  log.stream << "{smcl}\n{com}{sf}{ul off}";
  log.smclMode = kModeCom;
  log.stream.flush();
}

// Opening banner. `continuation` = the file already holds a segment, or this
// is the resume of a paused log.
void smclWriteBanner(LogState& log, bool continuation, bool resumed, bool quiet = false) {
  if (quiet) return smclWriteQuietMarker(log);
  log.stream << "{smcl}\n"
             << (continuation ? "{com}{sf}{ul off}{txt}{.-}\n" : "{txt}{sf}{ul off}{.-}\n");
  smclLabelLines(log, "      ");
  log.stream << (resumed ? "{txt}resumed on:  {res}" : " {txt}opened on:  {res}") << smclStamp()
             << "\n";
  log.smclMode = kModeRes;
  log.stream.flush();
}

// Closing footer of a segment; `what` is "paused" or "closed".
void smclWriteFooter(LogState& log, const char* what, bool quiet = false) {
  if (quiet) return smclWriteQuietMarker(log);
  smclLabelLines(log, "      {txt}");
  log.stream << " {txt}" << what << " on:  {res}" << smclStamp() << "\n"
             << "{txt}{.-}\n{smcl}\n{txt}{sf}{ul off}";
  log.smclMode = kModeTxt;
  log.stream.flush();
}

// Applies every request in `ops` at once, in order, to a log whose tee will
// not meet their command lines (the frame already passed, or none of the log's
// depth is coming). Returns true when the log was closed. A request that finds
// the log already in the asked state (a second `log off`) writes nothing, as
// Stata does.
bool smclDrainOps(LogState& log) {
  bool closed = false;
  for (const LogOp op : log.ops) {
    if (op == LogOp::Pause) {
      if (!log.paused) {
        smclWriteFooter(log, "paused");
        log.paused = true;
      }
    } else if (op == LogOp::Resume) {
      if (log.paused) {
        smclWriteBanner(log, /*continuation=*/true, /*resumed=*/true);
        log.paused = false;
      }
    } else {
      if (!log.paused) smclWriteFooter(log, "closed");  // a paused log closes silently
      closed = true;
      break;
    }
  }
  log.ops.clear();
  return closed;
}

// The state `log` will be in once its queued requests are applied.
bool smclWillBePaused(const LogState& log) {
  bool paused = log.paused;
  for (const LogOp op : log.ops) {
    if (op == LogOp::Pause) paused = true;
    if (op == LogOp::Resume) paused = false;
  }
  return paused;
}

// Is the echo text `echo` (".  cap log off Main_log") the command line of a
// queued `op` aimed at `log`? The prefixes capture/quietly/noisily (and their
// abbreviations) are skipped; a target that is a macro cannot be compared and
// is taken to match. The tee uses this to put the footer after the right line
// when a container frame (a `do` file, a loop) carries many commands. `quiet`
// reports the prefixes' verdict on output: capture and quietly silence it,
// a later noisily restores it.
struct LogCommandEcho {
  bool matches = false;
  bool quiet = false;
};

// An echo line (".  cap noi display ...") split into words, with the command
// prefixes consumed: `first` is the index of the command word, `quiet` the
// prefixes' verdict (capture and quietly silence output, a later noisily
// restores it). A trailing ';' (delimiter mode) is dropped from each word.
struct EchoWords {
  std::vector<std::string> tok;
  size_t first = 0;
  bool quiet = false;
};
EchoWords smclSplitEcho(const std::string& echo) {
  EchoWords out;
  std::istringstream words(echo.size() > 1 && echo[0] == '.' ? echo.substr(1) : echo);
  for (std::string w; words >> w;) {
    while (!w.empty() && w.back() == ';') w.pop_back();
    out.tok.push_back(w);
  }
  auto abbrevOf = [](std::string w, const std::string& full, size_t minLen) {
    while (!w.empty() && w.back() == ':') w.pop_back();
    for (char& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return w.size() >= minLen && w.size() <= full.size() && full.compare(0, w.size(), w) == 0;
  };
  for (; out.first < out.tok.size(); ++out.first) {
    const std::string& w = out.tok[out.first];
    if (abbrevOf(w, "capture", 3) || abbrevOf(w, "quietly", 3)) {
      out.quiet = true;
    } else if (abbrevOf(w, "noisily", 2)) {
      out.quiet = false;
    } else {
      break;
    }
  }
  return out;
}

LogCommandEcho smclParseLogCommand(const std::string& echo, const LogState& log, LogOp op) {
  const EchoWords e = smclSplitEcho(echo);
  LogCommandEcho out;
  out.quiet = e.quiet;
  const size_t i = e.first;
  if (i + 1 >= e.tok.size() || e.tok[i] != "log") return out;
  const char* sub = op == LogOp::Pause ? "off" : op == LogOp::Resume ? "on" : "close";
  if (e.tok[i + 1] != sub) return out;
  if (i + 2 >= e.tok.size()) {
    out.matches = log.name.empty();
  } else {
    const std::string& target = e.tok[i + 2];
    out.matches = target == "_all" || target.find_first_of("`$") != std::string::npos ||
                  target == log.name;
  }
  return out;
}

// Did this command line come from `display` (or an abbreviation, `di` up)? Its
// output text is what the user typed, SMCL directives and all; the text any
// other command prints is plain and has its braces escaped.
bool smclEchoIsDisplay(const std::string& echo) {
  if (echo.empty() || echo[0] != '.') return false;
  const EchoWords e = smclSplitEcho(echo);
  if (e.first >= e.tok.size()) return false;
  const std::string& w = e.tok[e.first];
  return w.size() >= 2 && std::string("display").compare(0, w.size(), w) == 0;
}
}  // namespace

bool SessionState::openLog(const std::string& resolvedPath, bool appendMode,
                           const std::string& name, int depth, bool smcl) {
  // Defensive only — environment/logging_commands.cpp's runLog already refuses reopening a log
  // under a name that is already open, matching real Stata, so in normal use
  // there is nothing to drop here. Dropping (rather than e.g. asserting) keeps
  // this a total function.
  closeLog(name);
  // A `log close` of the same file that is still waiting for its footer must
  // write it before this open can truncate or extend the file.
  for (auto it = endingLogs_.begin(); it != endingLogs_.end();) {
    if ((*it)->path == resolvedPath) {
      smclDrainOps(**it);
      it = endingLogs_.erase(it);
    } else {
      ++it;
    }
  }
  auto st = std::make_unique<LogState>();
  st->path = resolvedPath;
  st->name = name;
  st->depth = depth;
  st->smcl = smcl;
  bool continuation = false;
  if (smcl && appendMode) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(cecon::to_native_path(resolvedPath), ec);
    continuation = !ec && size > 0;
  }
  st->stream.open(cecon::to_native_path(resolvedPath),
                   std::ios::out | (appendMode ? std::ios::app : std::ios::trunc));
  if (!st->stream.is_open()) return false;
  if (smcl) {
    smclWriteBanner(*st, continuation, /*resumed=*/false);
    st->skipNextEcho = true;  // the `log using` line is not in the log
  }
  logs_.push_back(std::move(st));
  return true;
}

bool SessionState::closeLog(const std::string& name) {
  for (auto it = logs_.begin(); it != logs_.end(); ++it) {
    if ((*it)->name != name) continue;
    std::unique_ptr<LogState> log = std::move(*it);
    logs_.erase(it);
    if (!log->smcl) return true;  // ofstream's dtor flushes + closes
    log->ops.push_back(LogOp::Close);
    if (log->depth > dispatchDepth_) {
      // Opened deeper than the frame closing it: no later frame of that depth
      // will carry the command line, so the footer cannot wait for it.
      smclDrainOps(*log);
    } else {
      endingLogs_.push_back(std::move(log));
    }
    return true;
  }
  return false;
}

void SessionState::closeAllLogs() {
  for (const auto& n : logNames()) closeLog(n);
}

// session log queries. Method bodies retain their original ordering.
bool SessionState::logIsOpen(const std::string& name) const {
    for (const auto& l : logs_) {
      if (l->name == name) return true;
    }
    return false;
  }

std::vector<std::string> SessionState::logNames() const {
    std::vector<std::string> out;
    out.reserve(logs_.size());
    for (const auto& l : logs_) out.push_back(l->name);
    return out;
  }

bool SessionState::pauseLog(const std::string& name) {
  for (auto& l : logs_) {
    if (l->name != name) continue;
    if (!l->smcl) {
      l->paused = true;
    } else if (!smclWillBePaused(*l)) {
      // The footer follows the `log off` command line, which logTee() writes
      // after this handler has returned -- unless no later frame of this log's
      // depth is coming (opened deeper than the frame pausing it).
      l->ops.push_back(LogOp::Pause);
      if (l->depth > dispatchDepth_) smclDrainOps(*l);
    }
    return true;
  }
  return false;
}

bool SessionState::resumeLog(const std::string& name) {
  for (auto& l : logs_) {
    if (l->name != name) continue;
    if (!l->smcl) {
      l->paused = false;
    } else if (smclWillBePaused(*l)) {
      // The resumed header stands where the `log on` line would (the line
      // itself is not recorded: the log was off when it ran).
      l->ops.push_back(LogOp::Resume);
      if (l->depth > dispatchDepth_) smclDrainOps(*l);
    }
    return true;
  }
  return false;
}

namespace {
// BS-006 (BUG-028): generic plain-text rendering of one dispatch() result
// block, for logTee() below. Deliberately NOT a byte-exact Stata SMCL
// renderer (see environment/logging_commands.cpp's runLog doc comment for the scope call —
// the gate is "real, sensible content", not fixed-width ASCII tables).
// "echo" (the ". command" echo line) and "display" (every scalar/`display`/
// note-style single-line result) already carry their own ready-made human
// text in a "text" field, used verbatim by logTee(); "error" blocks carry
// theirs in "message". Every OTHER block type (estimation/tabulate/
// summarize/browse/graph/describe/levelsof/table_result/data_mutation/
// _control) has no pre-rendered plain-text form ANYWHERE in this codebase —
// the webview frontend renders those straight from JSON — so jsonAsText()
// recursively dumps the block's own fields as indented "key: value" text:
// real field values (actual coefficients, row/column names, ...), not a
// stub placeholder, even though it isn't laid out like Stata's own tables.
std::string jsonScalarToText(const nlohmann::ordered_json& v) {
  if (v.is_null()) return ".";
  if (v.is_string()) return v.get<std::string>();
  if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
  if (v.is_number_integer() || v.is_number_unsigned()) {
    return std::to_string(v.get<long long>());
  }
  if (v.is_number_float()) {
    std::ostringstream oss;
    oss << v.get<double>();
    return oss.str();
  }
  return v.dump();  // shouldn't happen for a scalar, but never throw
}

void jsonAsText(const nlohmann::ordered_json& v, std::ostream& out, const std::string& prefix) {
  if (v.is_object()) {
    for (auto it = v.begin(); it != v.end(); ++it) {
      if (it.key() == "type") continue;  // conveyed by logTee()'s own header line
      // F7 (B.3.0 "bounded transcripts"): never render a data_mutation block's
      // `varlist_after` array into the log. Stata logs nothing there, and this
      // single array was ~4.3 MB PER mutating verb — the dominant driver of the
      // incident run's 4.7 GB log. Skipping the KEY (rather than filtering a
      // copy) means the huge array is never even traversed/serialized here.
      // varlist_after appears only at a data_mutation block's top level, so a
      // blanket skip is safe (mirrors the "type" skip just above).
      if (it.key() == "varlist_after") continue;
      const auto& val = it.value();
      if (val.is_object() || val.is_array()) {
        out << prefix << it.key() << ":\n";
        jsonAsText(val, out, prefix + "    ");
      } else {
        out << prefix << it.key() << ": " << jsonScalarToText(val) << "\n";
      }
    }
  } else if (v.is_array()) {
    int idx = 0;
    for (const auto& elem : v) {
      if (elem.is_object() || elem.is_array()) {
        out << prefix << "[" << idx << "]\n";
        jsonAsText(elem, out, prefix + "    ");
      } else {
        out << prefix << "- " << jsonScalarToText(elem) << "\n";
      }
      ++idx;
    }
  } else {
    out << prefix << jsonScalarToText(v) << "\n";
  }
}

// A command line: a blank line in {txt}, then "{com}. cmd". PIET's prompt is
// ".  cmd" (two spaces); Stata's is ". cmd". An echo that does not start with
// "." is a note PIET printed (not a command) and stays {txt} output.
void smclWriteEcho(LogState& log, std::string t) {
  smclSetMode(log, kModeTxt);
  if (!t.empty() && t[0] == '.') {
    log.stream << "\n";
    if (t.compare(0, 3, ".  ") == 0) t.erase(1, 1);
    smclSetMode(log, kModeCom);
  }
  log.stream << smclEscape(t) << "\n";
}

// One non-echo block. `fromDisplay` = the command line before it was `display`.
void smclWriteBlock(LogState& log, const nlohmann::ordered_json& b, const std::string& type,
                    bool fromDisplay, const std::string& tabstatText, bool& consumedTabstat) {
  if (type == "display" && fromDisplay) {
    // The text is SMCL already -- it is what the user typed to `display`. If it
    // carries directives of its own it may have changed the mode, so forget it.
    const std::string text = b.value("text", std::string());
    smclSetMode(log, kModeRes);
    log.stream << text << "\n";
    if (text.find('{') != std::string::npos) log.smclMode = '?';
  } else if (type == "display") {
    // A line some other command printed (`type`, `macro list`, a note): plain
    // text, so a literal brace in it is escaped.
    smclSetMode(log, kModeRes);
    log.stream << smclEscape(b.value("text", std::string())) << "\n";
  } else if (type == "estimation" && b.value("estimator", std::string()) == "tabstat" &&
             !tabstatText.empty()) {
    smclSetMode(log, kModeTxt);
    log.stream << smclEscape(tabstatText);
    consumedTabstat = true;
  } else if (type == "error") {
    smclSetMode(log, kModeErr);
    log.stream << smclEscape(b.value("message", std::string("(error)"))) << "\n";
    if (!b.value("captured", false)) {
      // An error nothing captured ends the command with its return code, which
      // Stata links to the r() help; `capture noisily` prints none. "1" is the
      // generic bucket coarseRc() falls back to, not a code anything named.
      const std::string rc = eval::coarseRc(b);
      if (rc != "1") {
        smclSetMode(log, kModeTxt);
        log.stream << "{search r(" << rc << "), local:r(" << rc << ");}\n";
      }
    }
  } else {
    std::ostringstream dump;
    if (!type.empty()) dump << "[" << type << "]\n";
    jsonAsText(b, dump, "    ");
    smclSetMode(log, kModeTxt);
    log.stream << smclEscape(dump.str());
  }
}

// One dispatch frame's blocks into an SMCL log (the writer's layout is
// described above openLog). The quiet filters are the text path's own. A
// queued `log off` / `log on` / `log close` takes effect at the echo of its
// own command line -- wherever that falls in the frame, so a `do` file that
// pauses a log halfway pauses it halfway -- and at the frame's end if the
// line never appears. Returns true when the log was closed.
bool smclTeeFrame(LogState& log, const nlohmann::ordered_json& blocks, bool dropEcho,
                  bool dropOutput, const std::string& tabstatText, bool& consumedTabstat) {
  bool fromDisplay = false;  // is the command whose output this is `display`?
  for (const auto& b : blocks) {
    if (!b.is_object()) continue;
    const std::string type = b.value("type", std::string());
    if (type == "echo") fromDisplay = smclEchoIsDisplay(b.value("text", std::string()));
    if (type == "echo" && !log.ops.empty()) {
      const std::string t = b.value("text", std::string());
      const LogOp op = log.ops.front();
      const LogCommandEcho cmd = smclParseLogCommand(t, log, op);
      if (cmd.matches) {
        log.ops.erase(log.ops.begin());
        const bool quiet = cmd.quiet || dropOutput;
        if (op == LogOp::Resume) {
          if (log.paused) {  // the `log on` line itself is not recorded
            smclWriteBanner(log, /*continuation=*/true, /*resumed=*/true, quiet);
            log.paused = false;
          }
          continue;
        }
        if (!log.paused && !dropEcho) smclWriteEcho(log, t);
        if (!log.paused) smclWriteFooter(log, op == LogOp::Pause ? "paused" : "closed", quiet);
        if (op == LogOp::Close) return true;  // later blocks belong to someone else
        log.paused = true;
        continue;
      }
    }
    if (log.paused) continue;
    if (type == "echo" && log.skipNextEcho) {
      log.skipNextEcho = false;
      continue;
    }
    if (type == "echo" && dropEcho) continue;
    if (type != "echo" && type != "error" && dropOutput) continue;
    if (type == "echo") {
      smclWriteEcho(log, b.value("text", std::string()));
    } else {
      smclWriteBlock(log, b, type, fromDisplay, tabstatText, consumedTabstat);
    }
  }
  const bool closed = smclDrainOps(log);
  log.stream.flush();
  return closed;
}
}  // namespace

void SessionState::logTee(const nlohmann::ordered_json& blocks, int depth) {
  if ((logs_.empty() && endingLogs_.empty()) || !blocks.is_array()) {
    pendingTabstatLogText.clear();
    return;
  }
  // W:P8: every open, unpaused log whose depth matches this frame gets the
  // same transcript — Stata tees to all open logs, so a `name()`d log opened
  // mid-session records the commands that run while it is open without
  // stealing them from the outer log. See LogState::depth for why the frame
  // has to match rather than "any frame".
  // BUG-189: a quiet scope is invisible to the block filters at this point (they
  // run after dispatch returns), so the gate has to be asked here — see
  // SessionState::logEchoSuppressed for the measured Stata semantics. Errors are
  // never suppressed: `quietly` is output-only and does not swallow errors, and
  // a log that hides the error explaining why a run stopped is worse than a
  // noisy one.
  const bool dropEcho = logEchoSuppressed();
  const bool dropOutput = logOutputSuppressed();
  bool consumedTabstat = false;
  for (const auto& log : logs_) {
    // A paused SMCL log still reads the frame when a `log on` is waiting in it.
    if (log->paused && !(log->smcl && !log->ops.empty())) continue;
    if (log->depth != depth) continue;
    if (log->smcl) {
      smclTeeFrame(*log, blocks, dropEcho, dropOutput, pendingTabstatLogText, consumedTabstat);
      continue;
    }
    for (const auto& b : blocks) {
      if (!b.is_object()) continue;
      std::string type = b.value("type", std::string());
      if (type == "echo" && dropEcho) continue;
      if (type != "echo" && type != "error" && dropOutput) continue;
      if (type == "echo" || type == "display") {
        // A command echo stands after a blank line, as in Stata's own text
        // log (". cmd" lines are separated from the previous output by one;
        // the first line of a `quietly log using` log is that blank).
        const std::string t = b.value("text", std::string());
        if (type == "echo" && !t.empty() && t[0] == '.') log->stream << "\n";
        log->stream << t << "\n";
      } else if (type == "estimation" && b.value("estimator", std::string()) == "tabstat" &&
                 !pendingTabstatLogText.empty()) {
        log->stream << pendingTabstatLogText;
        consumedTabstat = true;
      } else if (type == "error") {
        log->stream << b.value("message", std::string("(error)")) << "\n";
        std::string code = b.value("code", std::string());
        if (!code.empty()) log->stream << "(" << code << ")\n";
      } else {
        if (!type.empty()) log->stream << "[" << type << "]\n";
        jsonAsText(b, log->stream, "    ");
      }
    }
    log->stream.flush();
  }
  // `log close`d SMCL logs: the frame at the log's own depth carries the
  // command line; the closed-on footer follows it and the file is released.
  for (auto it = endingLogs_.begin(); it != endingLogs_.end();) {
    if ((*it)->depth != depth) {
      ++it;
      continue;
    }
    smclTeeFrame(**it, blocks, dropEcho, dropOutput, pendingTabstatLogText, consumedTabstat);
    // The tee always finishes it: its request queue ends with the Close.
    it = endingLogs_.erase(it);
  }
  // Consumed by the log that wrote it; a tee at a frame no open log belongs to
  // (the dispatch wrapper's nested call) leaves it for the frame's own tee.
  if (consumedTabstat) pendingTabstatLogText.clear();
}

}  // namespace piet
