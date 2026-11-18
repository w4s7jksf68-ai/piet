// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/io.h"
#include "piet/user_paths.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data/missing.h"  // cecon::missing_kind -- insheet's float columns carry `.` only
#include "io/win_compat.h"  // cecon::to_native_path / native_exists / native_unlink
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/delimited_io.h"  // insheet: the one native delimited reader
#include "piet/display_hardening.h"  // evalScalarExpr -- `putexcel A1 = (expr)`
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // applyStataFmt -- outfile writes through display formats
#include "piet/eval/pyvalue.h"  // pythonFloatRepr -- round-trippable numerics
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"  // infix `in`: parseInRangeBounds
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"  // piet::fmt::general -- outfile's %10.0g
#include "piet/strutil.h"
#include "xlsx.h"
#include "file_command_support.h"

namespace {

// `ascii` asks for the byte-oriented interpretation this filter already uses.
// `unicode` asks for a UTF-16 one, which would change which bytes match, so it
// is refused rather than ignored.
const piet::eval::OptVocab& kVocabFilefilter = piet::eval::registerOptVocab(
    {"filefilter"}, {{"from", "to", "replace", "ascii"}, {"unicode"}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace file_command_detail;

// ===========================================================================
// filefilter
// ===========================================================================
//
// Decision 7. A whole-file search/replace with Stata's own escape vocabulary,
// publishing r(occurrences). The input and output must be DIFFERENT files: this
// command reads one and writes the other, and letting them be the same would
// mean truncating the source before it has been fully read.
namespace {

// Stata's `filefilter` escapes. The three-character tokens are tested first, so
// `\BS` never resolves as `\B` + `S`.
std::string unescapeFilter(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' || i + 1 >= s.size()) {
      out.push_back(s[i]);
      continue;
    }
    const std::string three = s.substr(i, 3);
    if (three == "\\BS") { out.push_back('\\'); i += 2; continue; }
    if (three == "\\LQ") { out.push_back('`');  i += 2; continue; }
    if (three == "\\RQ") { out.push_back('\''); i += 2; continue; }
    // \###d is a byte in decimal, \##h one in hexadecimal ([D] filefilter;
    // vs-stata bank f179fd039bb4bff0 `from(\032d)`, 1cba9efa7f58da05 `from(\20h)`).
    if (i + 4 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isdigit(static_cast<unsigned char>(s[i + 2])) &&
        std::isdigit(static_cast<unsigned char>(s[i + 3])) && s[i + 4] == 'd') {
      out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 3)) & 0xFF));
      i += 4;
      continue;
    }
    if (i + 3 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 2])) && s[i + 3] == 'h') {
      out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16) & 0xFF));
      i += 3;
      continue;
    }
    switch (s[i + 1]) {
      case 'Q': out.push_back('"'); ++i; continue;
      case '$': out.push_back('$'); ++i; continue;
      case 'n': out.push_back('\n'); ++i; continue;
      case 'r': out.push_back('\r'); ++i; continue;
      case 't': out.push_back('\t'); ++i; continue;
      case 'M': out.push_back('\r'); ++i; continue;
      case 'U': out.push_back('\n'); ++i; continue;
      case 'W': out += "\r\n"; ++i; continue;
      case '\\': out.push_back('\\'); ++i; continue;
      default: out.push_back(s[i]); continue;
    }
  }
  return out;
}

// Every backslash must begin one of the escapes above; anything else is
// StataBE's r(198) "Unresolved backslash sequence." (vs-stata bank
// 58ee22c60462b326 `from(\LF)`, f42df5983100aa28 `from(\040)`).
bool escapesResolve(const std::string& s) {
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\') continue;
    if (i + 1 >= s.size()) return false;
    const std::string three = s.substr(i, 3);
    if (three == "\\BS" || three == "\\LQ" || three == "\\RQ") { i += 2; continue; }
    if (i + 4 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isdigit(static_cast<unsigned char>(s[i + 2])) &&
        std::isdigit(static_cast<unsigned char>(s[i + 3])) && s[i + 4] == 'd') { i += 4; continue; }
    if (i + 3 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 2])) && s[i + 3] == 'h') { i += 3; continue; }
    if (std::string("Q$nrtMUW\\").find(s[i + 1]) != std::string::npos) { ++i; continue; }
    return false;
  }
  return true;
}

constexpr std::size_t kFilefilterCap = 256u * 1024u * 1024u;  // 256 MiB

}  // namespace

ojson runFilefilter(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabFilefilter.known,
                                            kVocabFilefilter.unimplemented)) {
    return ojson(*err);
  }

  const std::vector<std::string> words = quoteAwareWords(preCommaArgs(parsed.raw));
  if (words.size() != 2) {
    return ioSyntax("filefilter: expected two filenames. Usage: filefilter oldfile newfile, "
                    "from(\"...\") to(\"...\") [replace]");
  }
  // from() and to() are read from the line as typed: the option parser hands
  // back the text with its quotes already removed, and the quotes decide whether
  // a pattern may hold a blank.
  std::optional<std::string> fromOpt, toOpt;
  {
    const std::string rawLine = parsed.raw;
    bool inQuote = false;
    size_t comma = std::string::npos;
    for (size_t i = 0; i < rawLine.size(); ++i) {
      if (rawLine[i] == '"') inQuote = !inQuote;
      else if (rawLine[i] == ',' && !inQuote) { comma = i; break; }
    }
    const std::string tail = comma == std::string::npos ? std::string() : rawLine.substr(comma + 1);
    for (size_t i = 0; i < tail.size();) {
      if (std::isspace(static_cast<unsigned char>(tail[i]))) { ++i; continue; }
      size_t j = i;
      while (j < tail.size() && (std::isalnum(static_cast<unsigned char>(tail[j])) || tail[j] == '_')) ++j;
      const std::string name = tail.substr(i, j - i);
      if (j > i && j < tail.size() && tail[j] == '(') {
        size_t depth = 0, k = j;
        bool q = false;
        for (; k < tail.size(); ++k) {
          if (tail[k] == '"') q = !q;
          else if (!q && tail[k] == '(') ++depth;
          else if (!q && tail[k] == ')' && --depth == 0) break;
        }
        const std::string inner = tail.substr(j + 1, k > j ? k - j - 1 : 0);
        if (!name.empty() && std::string("from").compare(0, name.size(), name) == 0) fromOpt = inner;
        else if (!name.empty() && std::string("to").compare(0, name.size(), name) == 0) toOpt = inner;
        i = k + 1;
      } else {
        i = j > i ? j : i + 1;
      }
    }
  }
  // from() and to() are one word each unless quoted: `from(alpha beta)` is
  // r(198) "option from() incorrectly specified" (vs-stata bank
  // d9dfbb0686cc573f; `from("alpha beta")` runs, 259d62f0e174b5cc).
  std::string fromDecoded, toDecoded;
  for (auto* slot : {&fromOpt, &toOpt}) {
    if (!slot->has_value()) continue;
    const bool isFrom = slot == &fromOpt;
    const std::string nm = isFrom ? "from" : "to";
    std::string t = trimOf(**slot);
    const bool quoted = t.size() >= 2 && t.front() == '"' && t.back() == '"';
    if (quoted) t = t.substr(1, t.size() - 2);
    else if (t.find_first_of(" \t") != std::string::npos)
      return ioErrorRc("option " + nm + "() incorrectly specified", 198);
    if (!escapesResolve(t))
      return ioErrorRc("Unresolved backslash sequence.\nUse '\\BS' to represent a backslash "
                       "character.",
                       198);
    (isFrom ? fromDecoded : toDecoded) = t;
  }
  // No from(): nothing to look for, so the file is copied as it is (StataBE,
  // bank 09cfff54ecf6fec8). No to(): the matches are removed (26116b92884118b6).
  const bool fromEmpty = fromDecoded.empty();
  const std::string from = unescapeFilter(fromDecoded);
  const std::string to = unescapeFilter(toDecoded);

  const std::string inSpelling = words[0];
  const std::string outSpelling = words[1];
  const std::string inPath = resolveUserPath(session, inSpelling);
  const std::string outPath = resolveUserPath(session, outSpelling);
  if (!pathExists(inPath)) {
    return ioErrorRc("file " + typedUserPath(inSpelling) + " not found", 601);
  }
  // "Is the destination also a source?" -- here it can be, and must not be.
  if (sameFile(inPath, outPath)) {
    // StataBE: r(602) "IN and OUT must be different." (vs-stata bank
    // 8a948756a490ba0e).
    return ioErrorRc(inSpelling + " and " + outSpelling + " must be different.", 602);
  }
  if (auto err = refuseExisting(outPath, outSpelling, hasFlag(parsed.opts, "replace"))) {
    return *err;
  }
  if (auto err = refuseIfSessionHbin(session, "filefilter", outPath, outSpelling)) return *err;

  {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(cecon::to_native_path(inPath), ec);
    if (!ec && sz > kFilefilterCap) {
      return ioError("filefilter: " + inSpelling + " is " + std::to_string(sz) +
                     " bytes; this build filters files up to 268435456 bytes (it holds the whole "
                     "file in memory).");
    }
  }
  auto bytes = readWholeFile(inPath);
  if (!bytes.has_value()) return ioError("filefilter: cannot read " + inSpelling, "data_io");

  std::string result;
  result.reserve(bytes->size());
  long long occurrences = 0;
  size_t at = 0;
  while (!fromEmpty) {
    const size_t hit = bytes->find(from, at);
    if (hit == std::string::npos) {
      result.append(*bytes, at, std::string::npos);
      break;
    }
    result.append(*bytes, at, hit - at);
    result += to;
    at = hit + from.size();
    ++occurrences;
  }

  if (fromEmpty) result = *bytes;
  if (auto err = commitBytes("filefilter", outPath, outSpelling, result)) return *err;

  // r-class (defect class 5): the arm has no table block to capture from, so
  // the handler publishes r(occurrences) itself.
  eval::replaceRClass(session.eval);
  session.eval.rValues.set("occurrences", static_cast<double>(occurrences));
  session.eval.rValues.set("bytes_from", static_cast<double>(from.size()));
  session.eval.rValues.set("bytes_to", static_cast<double>(to.size()));
  eval::declareRPosted(session.eval, "filefilter", {"occurrences", "bytes_from", "bytes_to"});
  if (fromEmpty)
    return displayBlock("(from() option is empty, therefore whole operation is irrelevant;\n"
                        " input file will be copied to output file)");
  return displayBlock("(" + std::to_string(occurrences) + " occurrence" +
                      (occurrences == 1 ? "" : "s") + " replaced)");
}

}  // namespace piet::handlers
