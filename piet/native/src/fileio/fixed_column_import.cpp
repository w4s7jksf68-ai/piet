// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/io.h"
#include "piet/handlers/session_commands.h"
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
#include "fixed_column_import.h"

namespace {

const piet::eval::OptVocab& kVocabInfix =
    piet::eval::registerOptVocab({"infix"}, {{"clear", "using"}, {}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace file_command_detail;

// ===========================================================================
// infix / infile dictionaries (fixed columns)
// ===========================================================================
//
// One fixed-column reader behind `infix` (command-line and dictionary specs)
// and `infile`'s dictionary form. The rules are the oracle's (w2ar
// log:3755-4064):
//   * an untyped numeric is float, its values rounded to float (log:3772);
//   * a bare `str` takes its column range's width, and a declared `str#`
//     narrower than its range is widened to it (`str3 name 8-12` -> str5,
//     log:3801); the width is the range's, not the data's (`8-40` -> str33);
//   * a string field ends at a NUL byte and is trimmed of blanks, except an
//     infile `%#S` field, which keeps them (log:4011 `[b c  ]`);
//   * a blank, `.` or unreadable numeric field is missing; a range past the
//     end of a line reads "" / missing (log:3838);
//   * `if`/`in` keep the records they select (log:3826);
//   * the reader wants an empty dataset: without `clear` over data, r(18)
//     with the data kept; with `clear` the data go first, so a missing or
//     unreadable dictionary leaves k=0 (log:3876-3900, 4052-4064).
namespace file_command_detail {

bool isTypeToken(const std::string& tok, bool* isString) {
  const std::string t = lowerOf(tok);
  if (t == "byte" || t == "int" || t == "long" || t == "float" || t == "double") {
    *isString = false;
    return true;
  }
  if (t == "strl") {
    *isString = true;
    return true;
  }
  if (t.size() > 3 && t.compare(0, 3, "str") == 0) {
    for (size_t i = 3; i < t.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(t[i]))) return false;
    }
    *isString = true;
    return true;
  }
  return false;
}

}  // namespace file_command_detail

namespace file_command_detail {

// `using ("data")` with a blank before the parenthesis is the same option as
// `using("data")` in Stata (w2ar dx_inf_030_space rc 0, log:3968); the
// tokenizer hands it over as a bare `using` followed by a key `"data")`.
eval::ParsedCommand joinSpacedOptionValues(const eval::ParsedCommand& parsed) {
  eval::ParsedCommand out = parsed;
  out.opts.clear();
  const auto& o = parsed.opts;
  for (size_t i = 0; i < o.size(); ++i) {
    if (o[i].second.isBool && i + 1 < o.size()) {
      const std::string& nk = o[i + 1].first;
      if (nk.size() >= 2 && (nk[0] == '(' || nk[0] == '"') && nk.back() == ')') {
        std::string v = nk.substr(0, nk.size() - 1);
        if (!v.empty() && v[0] == '(') v.erase(0, 1);
        out.opts.emplace_back(o[i].first, eval::OptValue::ofString(v));
        ++i;
        continue;
      }
    }
    out.opts.push_back(o[i]);
  }
  return out;
}

}  // namespace file_command_detail

namespace {

struct FixSlot {
  std::string name;
  int from = 1;  // 1-based, inclusive
  int to = 1;
  bool isString = false;
  bool keepBlanks = false;  // infile %#S
  std::string declaredType;  // "" untyped numeric, "str" bare, "strN", or a numeric type
  std::string label;
};

// The storage type a slot's column takes.
std::string fixSlotType(const FixSlot& s) {
  if (!s.isString) return s.declaredType.empty() ? std::string("float") : s.declaredType;
  const int width = std::max(1, s.to - s.from + 1);
  if (lowerOf(s.declaredType) == "strl") return "strL";
  int declared = 0;
  if (s.declaredType.size() > 3) declared = std::atoi(s.declaredType.c_str() + 3);
  return "str" + std::to_string(std::max(width, declared));
}

}  // namespace

namespace file_command_detail {

// The empty-dataset rule both readers share; nullopt = go on.
std::optional<ojson> requireEmptyOrClear(const eval::ParsedCommand& parsed, SessionState& session) {
  const bool clear = hasFlag(parsed.opts, "clear");
  if (!session.hasData()) return std::nullopt;
  if (!clear) {
    Datastore* ds = nullptr;
    try {
      ds = session.datastore();
    } catch (const std::exception&) {
      ds = nullptr;
    }
    if (ds != nullptr && ds->nCols() > 0) {
      return ioErrorRc("you must start with an empty dataset", 18);
    }
    return std::nullopt;
  }
  ojson cleared = runClear("", session);
  if (isErrorBlock(cleared)) return cleared;
  return std::nullopt;
}

}  // namespace file_command_detail

namespace {

// Read `bytes` through `slots`, stage it, adopt it and apply if/in.
ojson readFixedAndAdopt(const std::string& verb, const std::string& bytes,
                        const std::vector<FixSlot>& slots, const std::string& source,
                        const eval::ParsedCommand& parsed, SessionState& session) {
  delimited::Table table;
  for (const auto& s : slots) {
    table.names.push_back(s.name);
    table.labels.push_back(s.label);
    delimited::ColumnType ct;
    ct.isString = s.isString;
    ct.stataType = fixSlotType(s);
    table.types.push_back(ct);
  }
  size_t start = 0;
  const size_t n = bytes.size();
  while (start < n) {
    size_t eol = bytes.find('\n', start);
    if (eol == std::string::npos) eol = n;
    std::string line = bytes.substr(start, eol - start);
    start = eol + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;  // a blank line is not a record
    if (line.find('\t') != std::string::npos) {
      // Columns are counted with tabs expanded to stops every 8 (w2ar
      // dx_fx_fb16_L1map `1_______5___` for "1<tab>5<tab>2.5", log:2512).
      std::string expanded;
      for (char ch : line) {
        if (ch == '\t') {
          do expanded += ' ';
          while (expanded.size() % 8 != 0);
        } else {
          expanded += ch;
        }
      }
      line = std::move(expanded);
    }
    std::vector<std::string> row;
    row.reserve(slots.size());
    for (const auto& s : slots) {
      std::string field;
      if (static_cast<size_t>(s.from) <= line.size()) {
        const size_t begin = static_cast<size_t>(s.from) - 1;
        const size_t end = std::min(static_cast<size_t>(s.to), line.size());
        field = line.substr(begin, end - begin);
      }
      if (s.isString) {
        const size_t nul = field.find('\0');
        if (nul != std::string::npos) field.resize(nul);
        if (!s.keepBlanks) {
          field = trimOf(field);
          // A string field that opens with a double quote loses that quote
          // (w2ar dx_outf_if_L1 `"b c"  "two"` reads `b c"  "two"`, log:4990;
          // a lone `"` reads "", dx_fx_fd17_L1map).
          if (!field.empty() && field.front() == '"') field.erase(0, 1);
        }
        row.push_back(field);
        continue;
      }
      field = trimOf(field);
      double d = 0.0;
      if (field.empty() || field == "." || !parseStrictDouble(field, &d)) {
        row.emplace_back();
      } else {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", d);
        row.push_back(buf);
      }
    }
    table.rows.push_back(std::move(row));
  }
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    // `in` names RECORDS to read: a range past the last record reads what
    // there is (an empty file `in 1` is N=0 at rc 0, w2ar dx_fx_fc15_L1map).
    const int64_t nRec = static_cast<int64_t>(table.rows.size());
    InRangeBounds rr = parseInRangeBounds(*parsed.inRange, nRec);
    if (!rr.ok && parsed.inRange->find_first_of("lL-") == std::string::npos) {
      rr = parseInRangeBounds(*parsed.inRange, std::numeric_limits<int32_t>::max());
    }
    if (!rr.ok) {
      InRangeFilterResult fr;
      fr.ok = false;
      fr.refusal = rr.refusal;
      return inRangeRejectError(fr, *parsed.inRange, verb);
    }
    const int64_t a = std::min<int64_t>(rr.a - 1, nRec);
    const int64_t b = std::min<int64_t>(rr.b, nRec);
    std::vector<std::vector<std::string>> kept(table.rows.begin() + static_cast<std::ptrdiff_t>(a),
                                               table.rows.begin() + static_cast<std::ptrdiff_t>(std::max(a, b)));
    table.rows = std::move(kept);
  }
  if (auto err = delimited::stageAndAdopt(session, table, verb, source)) return *err;
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, *session.currentHbin(), parsed.ifExpr, std::nullopt);
    } catch (const CobraError& ce) {
      return ioError(cobraErrorMessage(ce), ce.code);
    }
    if (fr.dataPath != *session.currentHbin()) {
      session.load(fr.dataPath);
      session.invalidateDatastore();
    }
  }
  return mutationResult(session, verb);
}

// One dictionary file: its optional `using` data file, its entries, and the
// data lines that follow `}` when it names no file.
struct Dictionary {
  std::string usingFile;
  std::vector<std::string> entries;  // the lines between { and }
  std::string trailingData;
  bool ok = false;
};

Dictionary readDictionary(const std::string& text) {
  Dictionary d;
  const size_t open = text.find('{');
  if (open == std::string::npos) return d;
  const size_t close = text.find('}', open);
  if (close == std::string::npos) return d;
  const std::string head = trimOf(text.substr(0, open));
  // [infix|infile] dictionary [using FILE]
  std::vector<std::string> hw = quoteAwareWords(head);
  size_t i = 0;
  if (i < hw.size() && (lowerOf(hw[i]) == "infix" || lowerOf(hw[i]) == "infile")) ++i;
  if (i >= hw.size() || lowerOf(hw[i]) != "dictionary") return d;
  ++i;
  if (i < hw.size()) {
    if (lowerOf(hw[i]) != "using" || i + 1 >= hw.size()) return d;
    d.usingFile = hw[i + 1];
    i += 2;
  }
  if (i != hw.size()) return d;
  const std::string body = text.substr(open + 1, close - open - 1);
  size_t p = 0;
  while (p <= body.size()) {
    size_t e = body.find('\n', p);
    if (e == std::string::npos) e = body.size();
    std::string line = trimOf(body.substr(p, e - p));
    if (!line.empty() && line.back() == '\r') line.pop_back();
    p = e + 1;
    if (line.empty() || line[0] == '*' || line.compare(0, 2, "//") == 0) continue;
    d.entries.push_back(line);
  }
  size_t after = close + 1;
  if (after < text.size() && text[after] == '\r') ++after;
  if (after < text.size() && text[after] == '\n') ++after;
  d.trailingData = after < text.size() ? text.substr(after) : std::string();
  d.ok = true;
  return d;
}

// `#[-#]` -> from/to.
bool parseColRange(const std::string& spec, int* from, int* to) {
  long long a = 0, b = 0;
  const size_t dash = spec.find('-');
  if (dash == std::string::npos) {
    if (!parseStrictInt(spec, &a) || a < 1) return false;
    b = a;
  } else if (!parseStrictInt(spec.substr(0, dash), &a) || !parseStrictInt(spec.substr(dash + 1), &b) ||
             a < 1 || b < a) {
    return false;
  }
  *from = static_cast<int>(a);
  *to = static_cast<int>(b);
  return true;
}

// infix's command-line / dictionary spec: `[type] name #[-#]` repeated.
std::optional<ojson> parseInfixSpec(const std::vector<std::string>& words, std::vector<FixSlot>* slots) {
  std::string pendingType;
  bool pendingIsString = false;
  std::string pendingName;
  for (const auto& w : words) {
    bool isStr = false;
    if (pendingName.empty() && (isTypeToken(w, &isStr) || lowerOf(w) == "str")) {
      if (lowerOf(w) == "str") isStr = true;
      pendingType = lowerOf(w);
      pendingIsString = isStr;
      continue;
    }
    if (!w.empty() && std::isdigit(static_cast<unsigned char>(w[0]))) {
      if (pendingName.empty()) {
        return ioSyntax("infix: column range `" + w + "` does not follow a variable name");
      }
      FixSlot s;
      if (!parseColRange(w, &s.from, &s.to)) {
        return ioSyntax("infix: `" + w + "` is not a column range (expected #-# with the first "
                        "column no greater than the last)");
      }
      s.name = pendingName;
      s.isString = pendingIsString;
      s.declaredType = pendingType;
      slots->push_back(s);
      pendingName.clear();
      pendingType.clear();
      pendingIsString = false;
      continue;
    }
    if (!pendingName.empty()) return ioSyntax("infix: variable " + pendingName + " has no column range");
    if (!strutil::isValidStataIdentifier(w)) {
      return ioSyntax("infix: invalid variable name `" + w + "' — " +
                      strutil::stataIdentifierRuleText());
    }
    pendingName = w;
  }
  if (!pendingName.empty()) return ioSyntax("infix: variable " + pendingName + " has no column range");
  if (slots->empty()) return ioSyntax("infix: no variables named");
  std::set<std::string> seen;
  for (const auto& s : *slots) {
    if (!seen.insert(s.name).second) return ioSyntax("infix: variable " + s.name + " named twice");
  }
  return std::nullopt;
}

// infile's dictionary entries: `[_column(#)] [type] name [%fmt] ["label"]`.
// The field starts at _column(#) (else where the previous one ended) and is
// as wide as the input format says (%4f, %5s, %5S).
std::optional<ojson> parseInfileDictionary(const std::vector<std::string>& entries,
                                           std::vector<FixSlot>* slots) {
  int nextCol = 1;
  for (const auto& line : entries) {
    std::vector<std::string> w = quoteAwareWords(line);
    size_t i = 0;
    FixSlot s;
    int col = nextCol;
    if (i < w.size() && lowerOf(w[i]).compare(0, 8, "_column(") == 0 && w[i].back() == ')') {
      long long c = 0;
      if (!parseStrictInt(w[i].substr(8, w[i].size() - 9), &c) || c < 1)
        return ioSyntax("infile: invalid dictionary entry `" + line + "`");
      col = static_cast<int>(c);
      ++i;
    }
    bool isStr = false;
    if (i < w.size() && isTypeToken(w[i], &isStr)) {
      s.declaredType = lowerOf(w[i]);
      s.isString = isStr;
      ++i;
    }
    if (i >= w.size() || !strutil::isValidStataIdentifier(w[i]))
      return ioSyntax("infile: invalid dictionary entry `" + line + "`");
    s.name = w[i++];
    int width = 0;
    if (i < w.size() && !w[i].empty() && w[i][0] == '%') {
      const std::string f = w[i++];
      size_t k = 1;
      while (k < f.size() && std::isdigit(static_cast<unsigned char>(f[k]))) ++k;
      width = std::atoi(f.substr(1, k - 1).c_str());
      // `%#S` keeps the field's blanks.
      if (k < f.size() && f[k] == 'S') s.keepBlanks = true;
    }
    if (i < w.size()) {
      const std::string lab = w[i++];
      if (lab.size() >= 2 && lab.front() == '"' && lab.back() == '"')
        s.label = lab.substr(1, lab.size() - 2);
      else
        return ioSyntax("infile: invalid dictionary entry `" + line + "`");
    }
    if (i != w.size()) return ioSyntax("infile: invalid dictionary entry `" + line + "`");
    if (width <= 0 && s.isString && s.declaredType.size() > 3)
      width = std::atoi(s.declaredType.c_str() + 3);
    if (width <= 0) return ioSyntax("infile: dictionary entry `" + line + "` has no field width");
    s.from = col;
    s.to = col + width - 1;
    nextCol = s.to + 1;
    if (s.isString) {
      // An infile string keeps its DECLARED width (str5 over %5s, log:3958).
      if (s.declaredType.size() <= 3) s.declaredType = "str" + std::to_string(width);
      s.to = s.from + width - 1;
    }
    slots->push_back(s);
  }
  if (slots->empty()) return ioSyntax("infile: the dictionary declares no variables");
  return std::nullopt;
}

}  // namespace

namespace file_command_detail {

// Reads the dictionary at `dictPath` and the data it points to (the using()
// option, the dictionary's own `using FILE`, or the lines after `}`).
ojson runDictionaryRead(const std::string& verb, const std::string& dictArg,
                        const eval::ParsedCommand& parsed, SessionState& session) {
  std::string dictPath = resolveUserPath(session, dictArg);
  std::string dictShown = typedUserPath(dictArg);
  assumeSuffix(&dictPath, &dictShown, ".dct");
  session.materializeVirtualSave(dictPath);
  if (!pathExists(dictPath)) return ioErrorRc("file " + dictShown + " not found", 601);
  auto dictBytes = readWholeFile(dictPath);
  if (!dictBytes.has_value()) return ioError(verb + ": cannot read " + dictShown, "data_io");
  const Dictionary d = readDictionary(*dictBytes);
  if (!d.ok) return ioSyntax(verb + ": " + dictShown + " is not a dictionary");
  std::vector<FixSlot> slots;
  if (verb == "infix") {
    std::vector<std::string> words;
    for (const auto& e : d.entries)
      for (const auto& w : quoteAwareWords(e)) words.push_back(w);
    if (auto err = parseInfixSpec(words, &slots)) return *err;
  } else {
    if (auto err = parseInfileDictionary(d.entries, &slots)) return *err;
  }
  std::string dataArg;
  if (auto u = optStrAny(parsed.opts, {"using"})) dataArg = *u;
  else if (!d.usingFile.empty()) dataArg = dequote(d.usingFile);
  std::string bytes;
  std::string source = dictPath;
  if (!dataArg.empty()) {
    std::string dataPath = resolveUserPath(session, dataArg);
    std::string dataShown = typedUserPath(dataArg);
    assumeSuffix(&dataPath, &dataShown, ".raw");
    session.materializeVirtualSave(dataPath);
    if (!pathExists(dataPath)) return ioErrorRc("file " + dataShown + " not found", 601);
    auto b = readWholeFile(dataPath);
    if (!b.has_value()) return ioError(verb + ": cannot read " + dataShown, "data_io");
    bytes = *b;
    source = dataPath;
  } else {
    bytes = d.trailingData;
  }
  return readFixedAndAdopt(verb, bytes, slots, source, parsed, session);
}

}  // namespace file_command_detail

ojson runInfix(const eval::ParsedCommand& parsedIn, SessionState& session) {
  const eval::ParsedCommand parsed = joinSpacedOptionValues(parsedIn);
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabInfix.known,
                                            kVocabInfix.unimplemented)) {
    return ojson(*err);
  }

  std::vector<std::string> words;
  std::string pathArg;
  if (!splitUsing(dropQualifierWords(quoteAwareWords(preCommaArgs(parsed.raw))), &words,
                  &pathArg)) {
    return ioSyntax("infix: missing the file. Usage: infix [type] var #-# [...] using \"file\" "
                    "[, clear]");
  }
  std::vector<FixSlot> slots;
  if (!words.empty()) {
    if (auto err = parseInfixSpec(words, &slots)) return *err;
  }
  if (auto err = requireEmptyOrClear(parsed, session)) return *err;
  if (words.empty()) {
    // `infix using dict [, using(data)]` -- the dictionary form (w2ar
    // dx_ifx_dict, log:3857).
    return runDictionaryRead("infix", pathArg, parsed, session);
  }
  std::string resolved = resolveUserPath(session, pathArg);
  std::string shown = typedUserPath(pathArg);
  assumeSuffix(&resolved, &shown, ".raw");
  session.materializeVirtualSave(resolved);  // BUG-125: a save_virtual'd tempfile becomes real
  if (!pathExists(resolved)) return ioErrorRc("file " + shown + " not found", 601);
  auto bytes = readWholeFile(resolved);
  if (!bytes.has_value()) return ioError("infix: cannot read " + shown, "data_io");
  return readFixedAndAdopt("infix", *bytes, slots, resolved, parsed, session);
}

}  // namespace piet::handlers
