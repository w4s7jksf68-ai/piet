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
  const auto fromOpt = optStr(parsed.opts, "from");
  const auto toOpt = optStr(parsed.opts, "to");
  if (!fromOpt.has_value()) return ioSyntax("filefilter: from() is required");
  if (!toOpt.has_value()) return ioSyntax("filefilter: to() is required");
  const std::string from = unescapeFilter(*fromOpt);
  const std::string to = unescapeFilter(*toOpt);
  if (from.empty()) {
    return ioSyntax("filefilter: from() cannot be empty — there would be a match between every "
                    "pair of characters");
  }

  const std::string inSpelling = words[0];
  const std::string outSpelling = words[1];
  const std::string inPath = resolveUserPath(session, inSpelling);
  const std::string outPath = resolveUserPath(session, outSpelling);
  if (!pathExists(inPath)) {
    return ioErrorRc("file " + typedUserPath(inSpelling) + " not found", 601);
  }
  // "Is the destination also a source?" -- here it can be, and must not be.
  if (sameFile(inPath, outPath)) {
    return ioError("filefilter: the input and the output must be different files (" + inSpelling +
                   " names both) — filter into a new file and rename it afterwards.");
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
  while (true) {
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

  if (auto err = commitBytes("filefilter", outPath, outSpelling, result)) return *err;

  // r-class (defect class 5): the arm has no table block to capture from, so
  // the handler publishes r(occurrences) itself.
  session.eval.rValues.set("occurrences", static_cast<double>(occurrences));
  return displayBlock("(" + std::to_string(occurrences) + " occurrence" +
                      (occurrences == 1 ? "" : "s") + " replaced)");
}

}  // namespace piet::handlers
