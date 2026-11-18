// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/io.h"
#include "piet/dispatcher.h"  // dispatch(): cumul marks its if/in sample with generate

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <regex>
#include <ctime>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data/frame_internal.h"  // canonical numeric storage rounding for recast
#include "data/missing.h"         // cumul's by() groups: each missing code is its own
#include "core/rng.h"         // Philox4x32-10, the engine's own counter-based RNG
#include "core/rng_mt64.h"    // Stata's session stream: bsample's fast path
#include "estimation/bsample_draws.h"    // the fast path's draw, shared with the bootstrap prefix
#include "core/rng_normal.h"  // uniform01 / normal_icdf (AS241) -- drawnorm
#include "io/win_compat.h"    // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"  // piet::fmtStataDefaultG — Stata's %9.0g default
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"  // pythonFloatRepr -- round-trippable CSV numerics
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/storage_type.h"  // FU-944: input's c(type) and float store
#include "piet/strutil.h"
#include "estimation/estimation_internal.h"  // esampleHeadMask / esampleRewriteHeadMask
#include "engine/scratch_columns.h"          // cumul's if/in restriction and weight columns
#include "dataset_shape_support.h"
#include "dataset_shape_options.h"

namespace piet::handlers {

using namespace dataset_shape_detail;

// ===========================================================================
// input  (the block fold + the handler)
// ===========================================================================

namespace {

// The byte that joins an `input` block's rows into one line. 0x1F (US) does not
// appear in Stata source, which is exactly why `program`'s own folding could
// not use a printable sentinel either.
constexpr char kInputSep = '\x1F';
// Appended to a folded block that ran off the end of the file without `end`.
constexpr char kInputUnterminated = '\x1E';

const std::regex& reInputHead() {
  static const std::regex re(R"(^input\b(.*)$)", std::regex::icase);
  return re;
}

const std::regex& reInputEnd() {
  static const std::regex re(R"(^end\s*$)", std::regex::icase);
  return re;
}

// A `program [define] NAME` header. captureInputBlocks runs BEFORE
// program::captureProgams (it must -- see doexec.cpp), so it is the one that
// would otherwise consume a program body's `end` if that body contained a line
// starting with `input`. This is deliberately a LOOSE match rather than a copy
// of program.cpp's own header regex: its only job is "do not fold inside here",
// and for that, over-matching is safe (an `input` block is never nested in
// something this matches by accident) while under-matching is not.
const std::regex& reProgramHeadForSkip() {
  static const std::regex re(R"(^program\b(?!\s+(?:drop|dir|list)\b))", std::regex::icase);
  return re;
}

// One free-format Stata data token: a quoted string, or a run of non-space.
// Returns false at end of input.
bool nextDataToken(const std::string& s, size_t* p, std::string* tok, bool* wasQuoted) {
  while (*p < s.size() && std::isspace(static_cast<unsigned char>(s[*p]))) ++(*p);
  if (*p >= s.size()) return false;
  *wasQuoted = false;
  if (s[*p] == '"') {
    *wasQuoted = true;
    ++(*p);
    std::string out;
    while (*p < s.size() && s[*p] != '"') out += s[(*p)++];
    if (*p < s.size()) ++(*p);  // closing quote
    *tok = out;
    return true;
  }
  std::string out;
  while (*p < s.size() && !std::isspace(static_cast<unsigned char>(s[*p]))) out += s[(*p)++];
  *tok = out;
  return true;
}

bool isStorageTypeWord(const std::string& w, bool* isStr) {
  const std::string t = strutil::lower(w);
  if (t == "byte" || t == "int" || t == "long" || t == "float" || t == "double") {
    *isStr = false;
    return true;
  }
  static const std::regex reStr(R"(^str([0-9]+)$)");
  if (std::regex_match(t, reStr)) {
    *isStr = true;
    return true;
  }
  return false;
}

}  // namespace

// Fold `input ... end` into ONE line, exactly the way `program ... end` is
// folded a line earlier in doexec.cpp. `input`'s body is DATA, not statements:
// letting the execution loop see its rows would dispatch each one as a command,
// and the tokenizer would split them on their own commas.
//
// The line-number offset this introduces is the same class `program` blocks
// already have, and is documented rather than corrected: a later error in the
// same do-file reports the line of the FOLDED block, not of the row inside it.
//
// KNOWN LIMIT, same as `program`'s own capture pass: only a line that BEGINS
// with `input` is folded, so `capture noisily input x y` ... `end` is not a
// block this pass recognises and its rows would reach the dispatcher as
// commands. Matching prefixed forms belongs in one place for both constructs,
// not separately here.
//
// IDEMPOTENT: the pass runs once per executeDoSource, and a brace body's lines
// are run through executeDoSource again, so a folded line must pass through.
std::vector<std::string> captureInputBlocks(const std::vector<std::string>& lines) {
  std::vector<std::string> out;
  size_t i = 0;
  const size_t n = lines.size();
  while (i < n) {
    const std::string stripped = strutil::trim(lines[i]);
    // Pass a `program ... end` region through verbatim. Without this the two
    // folds race for the same terminator: whichever runs first wins, and the
    // other one silently swallows the rest of the file.
    if (std::regex_search(stripped, reProgramHeadForSkip())) {
      out.push_back(lines[i]);
      ++i;
      while (i < n) {
        const std::string pb = strutil::trim(lines[i]);
        out.push_back(lines[i]);
        ++i;
        if (std::regex_match(pb, reInputEnd())) break;  // same `end` shape
      }
      continue;
    }
    std::smatch m;
    // A line that already carries the fold bytes IS a folded block: a brace
    // body (`capture noisily { input ... end }`, a loop, an `if`) was folded
    // with the whole file, and runBody hands its lines back through
    // executeDoSource, which runs this pass again. Re-folding it opened a
    // second block that hunted for an `end` the first fold had consumed, ate
    // the rest of the body as data, and answered "block not terminated".
    const bool alreadyFolded = stripped.find(kInputSep) != std::string::npos ||
                               stripped.find(kInputUnterminated) != std::string::npos;
    if (alreadyFolded || !std::regex_search(stripped, m, reInputHead())) {
      out.push_back(lines[i]);
      ++i;
      continue;
    }
    // `input` with the varlist (and any option tail) on the head line.
    std::string folded = "input" + m.str(1);
    ++i;
    bool terminated = false;
    while (i < n) {
      const std::string body = strutil::trim(lines[i]);
      ++i;
      if (std::regex_match(body, reInputEnd())) {
        terminated = true;
        break;
      }
      if (body.empty()) continue;  // blank lines inside a data block carry nothing
      folded += kInputSep;
      folded += body;
    }
    // A block closed by `end` with no data line in it still IS a block: one
    // empty row part marks it, so `input` / `end` reaches runInput as a block.
    if (terminated && folded.find(kInputSep) == std::string::npos) folded += kInputSep;
    if (!terminated) folded += kInputUnterminated;
    out.push_back(folded);
  }
  return out;
}

ojson runInput(const std::string& rawCmd, SessionState& session) {
  const size_t sep = rawCmd.find(kInputSep);
  if (sep == std::string::npos) {
    // A bare `input` typed with no block: the fold never fired, so there is no
    // data to read. Say that, rather than succeeding and doing nothing.
    return rsSyntax("input: requires a do-file block terminated by `end` — interactive input is "
                    "not available in this build.");
  }
  std::string body = rawCmd;
  bool unterminated = false;
  if (!body.empty() && body.back() == kInputUnterminated) {
    unterminated = true;
    body.pop_back();
  }

  // Split the fold back into the head line and the data rows.
  std::vector<std::string> parts;
  {
    std::string cur;
    for (char c : body) {
      if (c == kInputSep) {
        parts.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    parts.push_back(cur);
  }
  if (unterminated) return rsSyntax("input: block not terminated by `end`");

  std::string head = parts[0];
  {
    static const std::regex reHead(R"(^\s*input\b\s*(.*)$)", std::regex::icase);
    std::smatch m;
    if (std::regex_search(head, m, reHead)) head = m.str(1);
  }
  // The option tail, if any. `input` reads none of them, so the whole tail goes
  // through the reject net -- `automatic`/`label` come back as "valid Stata
  // option, not yet implemented" rather than as unknown keys.
  std::string varlistText = head;
  bool labelsFromText = false;  // `automatic` / `label`
  {
    const long long comma = eval::findUnquotedComma(head);
    if (comma >= 0) {
      varlistText = head.substr(0, static_cast<size_t>(comma));
      std::vector<std::pair<std::string, eval::OptValue>> opts =
          eval::parseOpts(head.substr(static_cast<size_t>(comma) + 1));
      eval::canonicalizeForCommand(opts, "input");
      if (auto err = eval::rejectUnknownOptions(opts, kVocabInput.known, kVocabInput.unimplemented))
        return ojson(*err);
      for (const auto& kv : opts)
        if (kv.first == "automatic" || kv.first == "label") labelsFromText = true;
    }
  }

  // varlist, with `str#`/`byte`/... type words binding to the NEXT name only
  // (`input double y x1 x2` types y alone: w2e_float_default.log:2149
  // input_double_types `double|float`), or to every name of a parenthesised
  // list right after the word ([U] 11.4.2 "Lists of new variables": `var1
  // int(var2 var3)` stores both as ints; the truth bank's `input double(x w)`
  // cells are rc 0 in StataBE 19.5).
  std::vector<std::string> names;
  std::vector<char> declaredStr;
  std::vector<std::string> declaredNumType;  // "" = untyped (c(type) applies)
  std::vector<std::string> declaredStrType;  // the `str#` word as declared, "" otherwise
  {
    // The parentheses are their own tokens, whether or not blanks surround them.
    std::string spaced;
    for (char c : varlistText) {
      if (c == '(' || c == ')') {
        spaced += ' ';
        spaced += c;
        spaced += ' ';
      } else {
        spaced += c;
      }
    }
    bool pendingStr = false;
    bool pendingType = false;
    std::string pendingWord;
    bool inGroup = false;  // inside `type(` ... `)`: the type stays bound
    for (const auto& w : splitWs(spaced)) {
      if (w == "(") {
        if (!pendingType || inGroup) return rsSyntax("input: invalid varlist: unexpected `('");
        inGroup = true;
        continue;
      }
      if (w == ")") {
        if (!inGroup) return rsSyntax("input: invalid varlist: unexpected `)'");
        inGroup = false;
        pendingType = false;
        pendingStr = false;
        pendingWord.clear();
        continue;
      }
      bool isStr = false;
      if (isStorageTypeWord(w, &isStr)) {
        if (inGroup) return rsSyntax("input: invalid varlist: a storage type inside `" +
                                     pendingWord + "(...)'");
        pendingType = true;
        pendingStr = isStr;
        pendingWord = strutil::lower(w);
        continue;
      }
      if (!strutil::isValidStataIdentifier(w))
        return rsSyntax("input: invalid variable name `" + w + "' — " +
                        strutil::stataIdentifierRuleText());
      names.push_back(w);
      declaredStr.push_back(pendingType && pendingStr ? 1 : 0);
      declaredNumType.push_back(pendingType && !pendingStr ? pendingWord : std::string());
      declaredStrType.push_back(pendingType && pendingStr ? pendingWord : std::string());
      if (inGroup) continue;
      pendingType = false;
      pendingStr = false;
      pendingWord.clear();
    }
    if (inGroup) return rsSyntax("input: invalid varlist: `(' without its `)'");
    if (pendingType)
      return rsSyntax("input: a storage type must be followed by a variable name");
  }

  Datastore* ds = nullptr;
  if (session.hasData()) {
    try {
      ds = session.datastore();
    } catch (const std::exception& e) {
      return rsError(std::string("input: ") + e.what(), "data_io");
    }
  }

  // A name typed twice, or one the dataset already has, is r(110) "variable x
  // already defined" (StataBE 19.5, bank 61fe92aaa9219b1f `input a a`,
  // 0b64cc4d66434da2 `input price mpg` over the auto data, 4dc0887f243f65a5 a
  // second `input a b`): a varlist names NEW variables; a bare `input` is what
  // appends to the existing ones.
  {
    std::set<std::string> seen;
    for (const auto& nm : names) {
      if (!seen.insert(nm).second || (ds != nullptr && ds->has(nm)))
        return rsErrorRc("variable " + nm + " already defined", 110);
    }
  }

  if (ds != nullptr) {
    // With data in memory this build APPENDS to the existing variables, in the
    // existing order. A varlist of new names is refused rather than silently
    // creating a second, differently shaped dataset.
    if (!names.empty())
      return rsError("input: with data in memory this build appends to the existing variables "
                     "only (type `input` alone; new variables are not added by `input " +
                     joinWith(names, " ") + "`).");
    names = ds->names();
    declaredStr.assign(names.size(), 0);
    declaredNumType.assign(names.size(), std::string());
    for (size_t c = 0; c < names.size(); ++c)
      declaredStr[c] = (ds->dtype(names[c]) == "factor") ? 1 : 0;
  } else if (names.empty()) {
    return rsErrorRc("nothing to input", 104);
  }

  const size_t k = names.size();
  if (k == 0) return rsSyntax("input: no variables to read into");

  // One data line is one observation (StataBE 19.5, bank cells 6bdb2488fb3f59c7,
  // 5a1ec235a5b2164b, d27edf4866751bd1, b6a6a8818c981866): its values are read
  // free format, a value past the last variable is ignored, and a line that
  // cannot be read -- too few values, or text for a numeric variable -- is
  // reported (`'x' cannot be read as a number`, the rest of the line is not
  // looked at) and its observation is not stored; the lines after it still are.
  std::vector<std::string> notes;
  std::vector<std::vector<std::string>> goodRows;
  std::vector<std::vector<char>> goodQuoted;
  for (size_t r = 1; r < parts.size(); ++r) {
    std::vector<std::string> lineTokens;
    std::vector<char> lineQuoted;
    size_t p = 0;
    std::string tok;
    bool wasQuoted = false;
    while (nextDataToken(parts[r], &p, &tok, &wasQuoted)) {
      lineTokens.push_back(tok);
      lineQuoted.push_back(wasQuoted ? 1 : 0);
    }
    if (lineTokens.empty()) continue;  // the empty-block marker, or a blank line
    bool readable = true;
    for (size_t c = 0; c < k && readable; ++c) {
      const bool isText = declaredStr[c] != 0;
      if (c >= lineTokens.size()) {
        if (!isText) {
          notes.push_back("'' cannot be read as a number");
          readable = false;
        }
        continue;
      }
      if (isText || (!lineQuoted[c] && (lineTokens[c] == "." || lineTokens[c].empty()))) continue;
      const std::string& cell = lineTokens[c];
      if (cell.size() == 2 && cell[0] == '.' && cell[1] >= 'a' && cell[1] <= 'z') continue;
      double v = 0.0;
      if (!parseStrictDouble(cell, &v)) {
        if (labelsFromText)
          return rsError("input: `" + cell + "' is text in the numeric variable " + names[c] +
                         "; building value labels from it (options automatic and label) is not "
                         "implemented in this build");
        notes.push_back("'" + cell + "' cannot be read as a number");
        readable = false;
      }
    }
    if (!readable) continue;
    lineTokens.resize(k);
    lineQuoted.resize(k, 0);
    goodRows.push_back(std::move(lineTokens));
    goodQuoted.push_back(std::move(lineQuoted));
  }
  const size_t nNew = goodRows.size();
  std::vector<std::string> tokens;
  std::vector<char> quoted;
  tokens.reserve(nNew * k);
  quoted.reserve(nNew * k);
  for (size_t r = 0; r < nNew; ++r)
    for (size_t c = 0; c < k; ++c) {
      tokens.push_back(goodRows[r][c]);
      quoted.push_back(goodQuoted[r][c]);
    }
  // Notes go out ahead of the result; nothing read means nothing is created.
  auto withNotes = [&notes](ojson result) -> ojson {
    if (notes.empty()) return result;
    if (result.is_array() && result.empty()) result = ojson();
    std::string text;
    for (const auto& n : notes) text += (text.empty() ? "" : "\n") + n;
    ojson note;
    note["type"] = "display";
    note["text"] = text;
    if (result.is_null()) return note;
    return ojson::array({note, result});
  };
  if (nNew == 0 && ds == nullptr) return withNotes(silent());

  // FU-944: the storage type each numeric column takes, and so the value it
  // holds. Untyped is c(type) (float by default: w2e_float_default.log:2120
  // input_default_types `+1.19999a0000000X+000|float|float` for 1.1;
  // w2g_ebmat.log:141 e15_data_sum is the float data's sum), a declared type is
  // its own, and rows appended to data in memory take the existing column's
  // type. A float column stores float(v); every other type keeps the value read
  // (an integer type's non-integer input is not captured; nothing is invented).
  std::vector<std::string> storeType(k);
  for (size_t c = 0; c < k; ++c) {
    if (declaredStr[c]) continue;
    storeType[c] = ds != nullptr ? storage::resolveReplaceType(session, names[c])
                                 : storage::resolveCreateType(session, declaredNumType[c]);
  }

  CsvTable t;
  for (size_t c = 0; c < k; ++c) {
    t.names.push_back(names[c]);
    t.isFactor.push_back(declaredStr[c]);
  }
  t.rows.reserve(nNew);
  // Extended missing values `.a`..`.z` typed into a numeric column (StataBE
  // 19.5 stores them as such: truth bank M2 `n6miss` cells, `. 1` / `.a 2`).
  // The staging CSV has no spelling for them, so the cell is staged as `.` and
  // set afterwards (setExtendedMissing below). Per column: (row, letter).
  std::vector<std::vector<std::pair<size_t, char>>> extMissing(k);
  for (size_t r = 0; r < nNew; ++r) {
    std::vector<std::string> row(k);
    for (size_t c = 0; c < k; ++c) {
      const std::string& tok = tokens[r * k + c];
      const bool wasQuoted = quoted[r * k + c] != 0;
      if (!wasQuoted && (tok == "." || tok.empty())) continue;  // missing stays missing
      if (!wasQuoted && !t.isFactor[c] && tok.size() == 2 && tok[0] == '.' && tok[1] >= 'a' &&
          tok[1] <= 'z') {
        extMissing[c].emplace_back(r, tok[1]);
        continue;
      }
      if (!t.isFactor[c]) {
        double v = 0.0;
        if (!parseStrictDouble(tok, &v))
          return rsSyntax("input: `" + tok + "' is not a number, and " + names[c] +
                          " is a numeric variable (declare it `str#` to read text)");
        if (storeType[c] == "float") v = static_cast<double>(static_cast<float>(v));
        row[c] = numCell(v);
        continue;
      }
      row[c] = tok;
    }
    t.rows.push_back(std::move(row));
  }
  // Each column holding them gets a helper column in the staging data: the
  // letter's code (1 = .a ... 26 = .z) on its rows, empty elsewhere. After the
  // load one `replace ... if helper == code` per distinct letter writes them --
  // no expression grows with the number of rows (a ~1,000-row cond() chain was
  // "expression nested too deeply") -- and the helpers are dropped.
  std::vector<std::string> xmHelper(k);
  {
    int seq = 0;
    for (size_t c = 0; c < k; ++c) {
      if (extMissing[c].empty()) continue;
      std::string h;
      do {
        h = "__piet_inxm" + std::to_string(++seq);
      } while (std::find(names.begin(), names.end(), h) != names.end() ||
               (ds != nullptr && ds->has(h)));
      xmHelper[c] = h;
      t.names.push_back(h);
      t.isFactor.push_back(0);
      std::vector<std::string> codes(nNew);
      for (const auto& [r, letter] : extMissing[c])
        codes[r] = numCell(static_cast<double>(letter - 'a' + 1));
      for (size_t r = 0; r < nNew; ++r) t.rows[r].push_back(codes[r]);
    }
  }

  std::string newPath;
  if (auto err = writeAndConvert(session, t, "input", &newPath)) return *err;

  // The extended missing values, written by the expression engine (which reads
  // `.a`..`.z` literals) once the rows are in place, from the helper columns.
  auto setExtendedMissing = [&]() -> std::optional<ojson> {
    std::string helpers;
    for (size_t c = 0; c < k; ++c) {
      if (xmHelper[c].empty()) continue;
      std::vector<char> letters;
      for (const auto& rl : extMissing[c])
        if (std::find(letters.begin(), letters.end(), rl.second) == letters.end())
          letters.push_back(rl.second);
      std::sort(letters.begin(), letters.end());
      for (char letter : letters) {
        const ojson res = dispatch("quietly replace " + names[c] + " = ." + std::string(1, letter) +
                                       " if " + xmHelper[c] + " == " +
                                       std::to_string(letter - 'a' + 1),
                                   session);
        if (res.is_array()) {
          for (const auto& b : res)
            if (b.is_object() && b.value("type", std::string()) == "error") return b;
        }
      }
      helpers += " " + xmHelper[c];
    }
    if (helpers.empty()) return std::nullopt;
    const ojson res = dispatch("quietly drop" + helpers, session);
    if (res.is_array()) {
      for (const auto& b : res)
        if (b.is_object() && b.value("type", std::string()) == "error") return b;
    }
    return std::nullopt;
  };

  if (ds == nullptr) {
    session.load(newPath);
    if (auto bad = assertRowCount(session, "input", static_cast<int64_t>(nNew))) return *bad;
    // The types `: type`/describe and the .dta writer read, the way gen records
    // them (dispatcher.cpp, BUG-295): an untyped float column on the implicit
    // plane, a declaration (or a non-float c(type)) on varMeta.
    for (size_t c = 0; c < k; ++c) {
      // A declared `str#` is the column's storage type (`: type` answers it:
      // w2ar dx_inp_079_str `input str10 date`, str10 %10s, log:4625).
      if (declaredStr[c] && c < declaredStrType.size() && !declaredStrType[c].empty()) {
        session.varMeta[names[c]].stataType = declaredStrType[c];
        continue;
      }
      if (declaredStr[c] || storeType[c].empty()) continue;
      if (declaredNumType[c].empty() && storeType[c] == "float") {
        session.varMeta[names[c]].stataType.reset();
        session.implicitFloatVars.insert(names[c]);
      } else {
        session.implicitFloatVars.erase(names[c]);
        session.varMeta[names[c]].stataType = storeType[c];
      }
    }
    if (auto bad = setExtendedMissing()) return *bad;
    // NO newVars list here: this is a whole new dataset, not columns ADDED to
    // an existing one, and `newVars` is what steers seedNMissForPureAdd's
    // carry-forward — feeding it every column of a frame that has no
    // predecessor asks it to carry counts from a frame that does not exist.
    return withNotes(mutationResult(session, "input"));
  }
  const int64_t before = ds->nRows();
  const std::string out = nextPath(session.workdir(), "input_append");
  if (auto err = runVerb({"append", "--data", *session.currentHbin(), "--using", newPath, "--out",
                          out}))
    return *err;
  session.load(out);
  if (auto bad = assertRowCount(session, "input", before + static_cast<int64_t>(nNew))) return *bad;
  if (auto bad = setExtendedMissing()) return *bad;
  return withNotes(mutationResult(session, "input"));
}

}  // namespace piet::handlers
