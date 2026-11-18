// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// `dtaversion filename` -- the .dta file-format number of a file, read from its
// header, stored in r(version) and reported in one paragraph.
//
// What it does is [P]/[D] dtaversion and dtaversion.ado (StataNow 19.5), read
// for the order of the checks and the wording; the code is our own:
//   * the argument is a filename, quoted or not, taken as typed (no `.dta` is
//     added); a file that does not exist is r(601) "file NAME not found";
//   * a header `<stata_dta><header><release>NNN</release>` with NNN >= 116 is
//     format NNN; otherwise the first three bytes are format 103-116, byte order
//     1 or 2 and file type 1; otherwise format 102 is bytes 102 0 1 0; anything
//     else is r(610), "file not Stata .dta file or ...";
//   * r(version) is the format; the message names the file as typed and the
//     Stata release(s) that wrote that format.
// The message is SMCL `{p 2 3 2}`: a paragraph indented 2 on its first line and 3
// after, wrapped two columns short of c(linesize); a word longer than the line
// is broken, its rest continuing after "> " (as the bank records it for a long
// path, cell 38489fef43b7c876).

#include <cmath>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "language/command_reply_support.h"
#include "piet/cclass.h"
#include "piet/eval/errors.h"
#include "piet/eval/session.h"
#include "piet/handlers/io.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "piet/user_paths.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace command_reply_detail;

namespace {

// The format number, or nullopt when the header is none of the three shapes.
std::optional<int> dtaFormatOf(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::string head(64, '\0');
  in.read(&head[0], static_cast<std::streamsize>(head.size()));
  head.resize(static_cast<size_t>(in.gcount()));
  const std::string open = "<stata_dta><header><release>";
  if (head.compare(0, open.size(), open) == 0 && head.size() >= open.size() + 3) {
    const std::string digits = head.substr(open.size(), 3);
    bool numeric = true;
    for (char c : digits) numeric = numeric && c >= '0' && c <= '9';
    if (numeric) {
      const int format = std::stoi(digits);
      if (format >= 116 && head.compare(open.size() + 3, 10, "</release>") == 0) return format;
    }
  }
  if (head.size() >= 3) {
    const int format = static_cast<unsigned char>(head[0]);
    const int order = static_cast<unsigned char>(head[1]);
    const int type = static_cast<unsigned char>(head[2]);
    if (format >= 103 && format <= 116 && (order == 1 || order == 2) && type == 1) return format;
  }
  if (head.size() >= 4 && static_cast<unsigned char>(head[0]) == 102 && head[1] == 0 &&
      head[2] == 1 && head[3] == 0)
    return 102;
  return std::nullopt;
}

// "14, 15, ..., or 19": every release from the one that introduced the format
// to the running one (dtaversion.ado's makereleaselist).
std::string releaseList(int introduced, int current) {
  if (introduced >= current) return std::to_string(current);
  if (introduced == current - 1)
    return std::to_string(introduced) + " or " + std::to_string(current);
  std::string out = std::to_string(introduced);
  for (int r = introduced + 1; r < current; ++r) out += ", " + std::to_string(r);
  return out + ", or " + std::to_string(current);
}

std::string releaseText(int format, int current) {
  switch (format) {
    case 102: return "from Stata 1)";
    case 103: return "from Stata 2 or 3)";
    case 104: return "from Stata 4)";
    case 105: return "from Stata 5)";
    case 106:
    case 107: return "from a never-released interim format during the development of Stata 6)";
    case 108: return "from Stata 6)";
    case 109: return "from a never-released interim format during the development of Stata 7)";
    case 110:
    case 111: return "from Stata 7)";
    case 112:
    case 113: return "from Stata 8 or 9)";
    case 114: return "from Stata 10 or 11)";
    case 115: return "from Stata 12)";
    case 116: return "from a never-released interim format during the development of Stata 13)";
    case 117: return "from Stata 13)";
    case 118: return "from Stata " + releaseList(14, current) + ")";
    case 119: return "from Stata " + releaseList(15, current) + ")";
    case 120:
    case 121: return "from Stata " + releaseList(18, current) + ")";
    default: return "from a Stata after Stata " + std::to_string(current) + ")";
  }
}

// SMCL `{p first rest right}` over words, at c(linesize).
std::string paragraph(const std::string& text, size_t first, size_t rest, size_t width) {
  std::vector<std::string> words;
  for (size_t i = 0; i < text.size();) {
    while (i < text.size() && text[i] == ' ') ++i;
    size_t j = i;
    while (j < text.size() && text[j] != ' ') ++j;
    if (j > i) words.push_back(text.substr(i, j - i));
    i = j;
  }
  std::vector<std::string> lines;
  std::string line(first, ' ');
  bool hasWord = false;
  for (std::string w : words) {
    if (line.size() + (hasWord ? 1 : 0) + w.size() <= width) {
      if (hasWord) line += ' ';
      line += w;
      hasWord = true;
      continue;
    }
    if (hasWord) {
      lines.push_back(line);
      line.assign(rest, ' ');
    }
    while (line.size() + w.size() > width && line.size() < width) {
      const size_t take = width - line.size();
      line += w.substr(0, take);
      lines.push_back(line);
      w = w.substr(take);
      line = std::string(rest, ' ') + "> ";
    }
    line += w;
    hasWord = true;
  }
  lines.push_back(line);
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) out += (i ? "\n" : "") + lines[i];
  return out;
}

}  // namespace

ojson runDtaversion(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!parsed.opts.empty()) return envError("invalid syntax", 198);
  const std::string typed = strutil::trim(parsed.argText);
  if (typed.empty()) return envError("invalid file specification", 198);
  const std::string name = typedUserPath(typed);
  const std::string path = resolveUserPath(session, typed);
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode))
    return envError("file " + name + " not found", 601);
  const std::optional<int> format = dtaFormatOf(path);
  if (!format.has_value())
    return envError("file not Stata .dta file or ...\nFile " + name +
                        " is not a known .dta-file format. The file might be a horribly "
                        "corrupted .dta file, but the most likely explanation is simply "
                        "that the file is not a .dta file.",
                    610);

  int current = 19;
  if (auto v = resolveCValue("stata_version", session); v.has_value() && !v->isString) {
    try {
      current = static_cast<int>(std::floor(std::stod(v->text)));
    } catch (...) {
    }
  }
  size_t linesize = 80;
  if (auto v = resolveCValue("linesize", session); v.has_value() && !v->isString) {
    try {
      linesize = static_cast<size_t>(std::stod(v->text));
    } catch (...) {
    }
  }

  eval::replaceRClass(session.eval);
  session.eval.rValues.set("version", static_cast<double>(*format));
  eval::declareRPosted(session.eval, "dtaversion", {"version"});
  const std::string text = "(file \"" + name + "\" is .dta-format " + std::to_string(*format) +
                           " " + releaseText(*format, current);
  return displayBlock(paragraph(text, 2, 3, linesize > 4 ? linesize - 2 : linesize));
}

}  // namespace piet::handlers
