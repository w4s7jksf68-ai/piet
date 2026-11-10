// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// `type FILE` and `dir` / `ls`: show a text file, list a directory.
#include "piet/handlers/session_commands.h"
#include "piet/user_paths.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "piet/bundle_paths.h"
#include "piet/eval/errors.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::to_native_path / env_utf8 (BUG-050/051)
#include "environment_command_support.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace environment_detail;

namespace {

// BUG-233 / FIX:OPTNET-ABBREV-SWEEP: a net that does not register its
// vocabulary here is a net the dispatcher's single resolution point cannot
// canonicalize for, which is precisely how an ACCEPTED abbreviation reaches a
// handler that then compares raw keys and drops it. Registered up front, read
// back by the reject calls below, so each spelling is still written once.
const eval::OptVocab& kVocabType =
    eval::registerOptVocab({"type"}, {{"showtabs", "starbang", "lines"}, {}});
// Both spellings register, because both are registry commands routed to
// runDir: a vocabulary filed under "dir" alone leaves `ls x, wid' with no
// canonicalization at the dispatcher's single resolution point, so the
// abbreviation is ACCEPTED by rejectUnknownOptions and then dropped by this
// handler's `k == "wide"' compare -- the exact BUG-233 shape.
const eval::OptVocab& kVocabDir = eval::registerOptVocab({"dir", "ls"}, {{"wide"}, {}});

}  // namespace

// ---------------------------------------------------------------------------
// type FILE [, showtabs starbang lines(#)]
//
// ONE display block, not one per line: the log tee renders blocks, and a
// thousand-block `type` of a log file would be a thousand tee'd records of one
// user action.
nlohmann::ordered_json runType(const std::string& raw, const std::vector<std::string>& varlist,
                               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                               SessionState& session) {
  namespace fs = std::filesystem;
  if (auto err = eval::rejectUnknownOptions(opts, kVocabType.known)) return ojson(*err);

  std::string arg = varlist.empty() ? argAfterCommand(raw) : dequote(varlist[0]);
  if (arg.empty()) return errBlock("type: filename required", "syntax_error");

  bool showtabs = false;
  bool starbang = false;
  long long maxLines = -1;
  for (const auto& [k, v] : opts) {
    if (k == "showtabs" && v.isBool) showtabs = v.boolValue;
    else if (k == "starbang" && v.isBool) starbang = v.boolValue;
    else if (k == "lines") {
      if (!parseLongLong(v.strValue, maxLines) || maxLines < 0) {
        return errBlock("type: lines() requires a non-negative integer", "syntax_error");
      }
    }
  }

  const std::string path = resolveUserPath(session, arg);
  const fs::path np = cecon::to_native_path(path);
  std::error_code ec;
  if (!fs::exists(np, ec) || ec) return errBlockRc("file " + typedUserPath(arg) + " not found", "r(601)");

  std::ifstream in(np, std::ios::binary);
  if (!in) return errBlockRc("file " + typedUserPath(arg) + " could not be opened", "r(603)");

  // 4 MiB + 1: reading one byte past the cap is what distinguishes "the file
  // ended" from "we stopped", without a second stat that could race the read.
  constexpr std::size_t kCap = 4u * 1024u * 1024u;
  std::string data(kCap + 1, '\0');
  in.read(&data[0], static_cast<std::streamsize>(kCap + 1));
  const std::size_t got = static_cast<std::size_t>(in.gcount());
  data.resize(got);
  const bool truncated = got > kCap;
  if (truncated) data.resize(kCap);

  // Binary sniff BEFORE any line work: dumping a .dta into the console is the
  // failure mode this guard exists for -- it emits control bytes that can
  // reconfigure the terminal, and the do-file learns nothing from it either way.
  const std::size_t sniff = data.size() < 1024 ? data.size() : 1024;
  const std::size_t nul = data.find('\0');
  if (nul != std::string::npos && nul < sniff) {
    std::error_code sec;
    const auto bytes = fs::file_size(np, sec);
    return displayBlock("(type: binary file -- " +
                        (sec ? std::string("?") : std::to_string(bytes)) + " bytes)");
  }

  std::vector<std::string> lines;
  {
    std::string cur;
    for (const char c : data) {
      if (c == '\n') {
        lines.push_back(cur);
        cur.clear();
      } else if (c != '\r') {
        cur.push_back(c);
      }
    }
    if (!cur.empty()) lines.push_back(cur);  // a file ending in \n has no extra blank line
  }

  // starbang first, lines(#) second: `lines(20)` on a starbang'd file means
  // "twenty of the lines you are SHOWING me", which is the only reading under
  // which combining the two options is useful at all.
  if (starbang) {
    std::vector<std::string> kept;
    for (const auto& l : lines) {
      if (l.rfind("*!", 0) == 0) kept.push_back(l);
    }
    lines.swap(kept);
  }
  if (maxLines >= 0 && static_cast<std::size_t>(maxLines) < lines.size()) {
    lines.resize(static_cast<std::size_t>(maxLines));
  }

  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) text += "\n";
    if (!showtabs) {
      text += lines[i];
      continue;
    }
    for (const char c : lines[i]) {
      if (c == '\t') text += "<T>";
      else text.push_back(c);
    }
  }
  if (truncated) {
    if (!text.empty()) text += "\n";
    text += "(type: output truncated at 4 MiB)";
  }
  return displayBlock(text);
}

// ---------------------------------------------------------------------------
// dir [PATH|GLOB] [, wide]
nlohmann::ordered_json runDir(const std::string& raw, const std::vector<std::string>& varlist,
                              const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                              SessionState& session) {
  namespace fs = std::filesystem;
  if (auto err = eval::rejectUnknownOptions(opts, kVocabDir.known)) return ojson(*err);

  bool wide = false;
  for (const auto& [k, v] : opts) {
    if (k == "wide" && v.isBool) wide = v.boolValue;
  }

  std::string arg = varlist.empty() ? argAfterCommand(raw) : dequote(varlist[0]);

  std::string dirPath;
  std::string pattern = "*";
  if (arg.empty()) {
    dirPath = sessionCwd(session);
  } else if (arg.find('*') != std::string::npos || arg.find('?') != std::string::npos) {
    const fs::path p = cecon::to_native_path(resolveUserPath(session, arg));
    dirPath = p.parent_path().u8string();
    pattern = p.filename().u8string();
    if (dirPath.empty()) dirPath = sessionCwd(session);
  } else {
    const std::string resolved = resolveUserPath(session, arg);
    const fs::path np = cecon::to_native_path(resolved);
    std::error_code ec;
    if (fs::is_directory(np, ec) && !ec) {
      dirPath = resolved;
    } else if (fs::exists(np, ec) && !ec) {
      // `dir report.tex` names one file. Listing its PARENT instead would be a
      // different, much larger answer to the question that was asked.
      dirPath = np.parent_path().u8string();
      pattern = np.filename().u8string();
    } else {
      return errBlockRc("directory " + resolved + " not found", "r(601)");
    }
  }

  std::error_code ec;
  const fs::path base = cecon::to_native_path(dirPath);
  if (!fs::is_directory(base, ec) || ec) {
    return errBlockRc("directory " + dirPath + " not found", "r(601)");
  }
  std::vector<std::pair<std::string, bool>> entries;  // name, isDir
  for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string nm = it->path().filename().u8string();
    if (!globMatch(pattern, nm)) continue;
    std::error_code dec;
    entries.emplace_back(nm, fs::is_directory(it->path(), dec) && !dec);
  }
  std::sort(entries.begin(), entries.end(),
            [](const std::pair<std::string, bool>& a, const std::pair<std::string, bool>& b) {
              return a.first < b.first;
            });

  std::string text;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i) text += "\n";
    text += entries[i].first;
    if (entries[i].second) text += "/";
  }

  if (!wide) return displayBlock(text);
  // `wide` is accepted so a corpus do-file does not halt on it, but accepting
  // it silently would make PIET's answer look like Stata's multi-column one.
  // The note says which layout the caller actually got -- appended to the SAME
  // block, because the dispatcher's dir/ls arm pushes this return value as one
  // element and a two-block array would arrive nested and render as nothing.
  if (!text.empty()) text += "\n";
  text += "(dir: option wide ignored -- one entry per line)";
  return displayBlock(text);
}

}  // namespace piet::handlers
