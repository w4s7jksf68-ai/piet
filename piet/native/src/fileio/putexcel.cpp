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

// putexcel's vocabulary, in three parts.
//
//   * the ones this handler READS: set's mode/sheet, the matrix `names` flag,
//     the range `merge` flag, and `open`, which is a no-op here because this
//     build has no separate open/close phase (every assignment rewrites the
//     workbook in full).
//   * the FORMATTING options, which are the plan's deliberate exception to the
//     known-means-read rule: they are accepted, ignored, and announced once per
//     `set` with the exact note decision 6 specifies. They do not change any
//     VALUE this build writes -- only how a spreadsheet would paint it -- and
//     the announcement is what keeps that honest.
//   * the ones that WOULD change a value or the document's structure, which are
//     refused by name.
const piet::eval::OptVocab& kVocabPutexcel = piet::eval::registerOptVocab(
    {"putexcel"},
    {{"replace", "modify", "sheet", "names", "merge", "open",
      // formatting -- accepted, ignored, announced (decision 6).
      "nformat", "bold", "italic", "underline", "font", "hcenter", "vcenter", "border",
      "fpattern", "txtwrap", "shrinkfit", "locked", "hidden", "txtindent", "left", "right",
      "top", "bottom"},
     {"formula", "asdate", "asdatetime", "colwise", "rowwise", "returnset", "overwritefmt",
      "dateformat", "keepcellformat"}});

// The formatting subset, named once so the note fires on exactly the options
// the note describes.
const std::set<std::string>& putexcelFormatOpts() {
  static const std::set<std::string> kSet = {
      "nformat", "bold", "italic",  "underline", "font",  "hcenter", "vcenter", "border",
      "fpattern", "txtwrap", "shrinkfit", "locked", "hidden", "txtindent", "left", "right",
      "top", "bottom"};
  return kSet;
}

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace file_command_detail;

// ===========================================================================
// putexcel
// ===========================================================================
//
// Decision 6. `putexcel` is the only STATEFUL command in this bundle: `set`
// names the workbook, each `A1 = ...` updates one cell of an accumulated map,
// and the whole workbook is rewritten from that map on every assignment. The
// map lives in `SessionState::putexcel` because it must survive between
// statements; the shape is the one session_state.h documents, plus a
// `spelling` field holding the filename AS THE USER TYPED IT, because every
// message and echo must name that and not the resolved path (defect class 4).
namespace {

// ===========================================================================
// A1 addresses
// ===========================================================================

struct CellAddr {
  int row = 0;
  int col = 0;
};

// "bc12" -> {row 12, col 55}. Case-insensitive (Stata's addresses are).
// Returns false on anything that is not <letters><digits>.
bool parseAddr(const std::string& s, CellAddr* out) {
  size_t i = 0;
  long long col = 0;
  while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) {
    col = col * 26 + (std::toupper(static_cast<unsigned char>(s[i])) - 'A' + 1);
    if (col > 16384) return false;  // past XFD, the last column a workbook has
    ++i;
  }
  if (i == 0 || i == s.size()) return false;
  long long row = 0;
  for (size_t j = i; j < s.size(); ++j) {
    if (!std::isdigit(static_cast<unsigned char>(s[j]))) return false;
    row = row * 10 + (s[j] - '0');
    if (row > 1048576) return false;  // past the last row a workbook has
  }
  if (row < 1) return false;
  out->row = static_cast<int>(row);
  out->col = static_cast<int>(col);
  return true;
}

std::string colRefOf(int col1) {  // 1 -> "A"
  std::string s;
  int n = col1;
  while (n > 0) {
    const int rem = (n - 1) % 26;
    s.insert(s.begin(), static_cast<char>('A' + rem));
    n = (n - 1) / 26;
  }
  return s;
}

std::string addrOf(int row, int col) { return colRefOf(col) + std::to_string(row); }

// The state, unpacked. `ok == false` means no `putexcel set` has run.
struct PutexcelState {
  bool ok = false;
  std::string path;      // resolved -- what the writer opens
  std::string spelling;  // as typed -- what messages say
  std::string sheet = "Sheet1";
  bool noteShown = false;
};

PutexcelState readPutexcelState(SessionState& session) {
  PutexcelState st;
  const ojson& s = session.putexcel;
  if (!s.is_object() || !s.contains("path")) return st;
  st.ok = true;
  st.path = s.value("path", std::string());
  st.spelling = s.value("spelling", std::string());
  st.sheet = s.value("sheet", std::string("Sheet1"));
  st.noteShown = s.value("noteShown", false);
  return st;
}

// The accumulated cells, as the writer wants them. `cells` is an object keyed
// by upper-case A1 address; each value is {"n": <number>} or {"s": "<text>"}.
std::vector<xlsx::CellOut> cellsOf(const ojson& state) {
  std::vector<xlsx::CellOut> out;
  if (!state.is_object() || !state.contains("cells") || !state["cells"].is_object()) return out;
  for (auto it = state["cells"].begin(); it != state["cells"].end(); ++it) {
    CellAddr a;
    if (!parseAddr(it.key(), &a)) continue;
    xlsx::CellOut c;
    c.row = a.row;
    c.col = a.col;
    const ojson& v = it.value();
    if (v.is_object() && v.contains("n") && v["n"].is_number()) {
      c.isNumber = true;
      c.number = v["n"].get<double>();
    } else if (v.is_object() && v.contains("s") && v["s"].is_string()) {
      c.text = v["s"].get<std::string>();
    } else {
      continue;
    }
    out.push_back(c);
  }
  return out;
}

std::vector<std::string> mergesOf(const ojson& state) {
  std::vector<std::string> out;
  if (!state.is_object() || !state.contains("merges") || !state["merges"].is_array()) return out;
  for (const auto& m : state["merges"]) {
    if (m.is_string()) out.push_back(m.get<std::string>());
  }
  return out;
}

// Rewrite the workbook from the accumulated map. nullopt = written.
std::optional<ojson> rewriteWorkbook(SessionState& session, const PutexcelState& st) {
  try {
    xlsx::writeCells(st.path, st.sheet, cellsOf(session.putexcel), mergesOf(session.putexcel));
  } catch (const std::exception& e) {
    // The writer's message names the RESOLVED path; re-word it around the
    // spelling the user typed.
    std::string why = e.what();
    const size_t at = why.find(st.path);
    if (at != std::string::npos) {
      why = why.substr(0, at) + st.spelling + why.substr(at + st.path.size());
    }
    return ioError("putexcel: " + why, "data_io");
  }
  return std::nullopt;
}

// How many worksheet parts a .xlsx names.
//
// The reader in xlsx.h hands back the FIRST worksheet and says nothing about
// how many there are, and this bundle's manifest does not include xlsx.h, so
// there is no sheet-inventory call to make. What CAN be done without one is
// count the worksheet part names, which a ZIP stores UNCOMPRESSED in its own
// headers whatever it does with the parts themselves. That is enough to answer
// the only question that matters here -- "would rewriting this file throw away
// sheets the user still has?" -- and it errs toward refusing, never toward
// silently dropping a sheet. Returns 0 when the file cannot be read at all.
// The byte-scan that used to live here is GONE, and its removal is the point:
// it searched the raw file for "xl/worksheets/" and stopped at the first quote,
// '<' or control byte -- but a STORED zip places the next record's "PK"
// signature immediately after the filename, so it read `sheet1.xmlPK` as a
// second distinct sheet. Every workbook this build writes therefore looked
// multi-sheet, and `putexcel set ..., modify` refused all of them. Replaced by
// `xlsx::worksheetCount`, which uses the same ZIP reader `xlsx::read` does.

// Scan one `putexcel` VALUE starting at `i`, leaving `i` just past it.
// Returns false when the text at `i` is not a value this grammar accepts.
bool scanValue(const std::string& s, size_t& i, std::string* out, char* kind) {
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  if (i >= s.size()) return false;
  if (s[i] == '"') {
    std::string tok;
    ++i;
    while (i < s.size() && s[i] != '"') {
      tok.push_back(s[i]);
      ++i;
    }
    if (i >= s.size()) return false;  // unterminated
    ++i;
    *out = tok;
    *kind = 's';
    return true;
  }
  // `matrix(NAME)` and `(expr)` both end at the matching close paren.
  const std::string rest = lowerOf(s.substr(i, 7));
  const bool isMatrix = rest.compare(0, 7, "matrix(") == 0;
  const size_t open = isMatrix ? i + 6 : i;
  if (s[open] == '(') {
    int depth = 0;
    size_t j = open;
    for (; j < s.size(); ++j) {
      if (s[j] == '(') ++depth;
      else if (s[j] == ')') {
        --depth;
        if (depth == 0) break;
      }
    }
    if (j >= s.size()) return false;  // unbalanced
    *out = s.substr(open + 1, j - open - 1);
    *kind = isMatrix ? 'm' : 'e';
    i = j + 1;
    return true;
  }
  std::string tok;
  while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) {
    tok.push_back(s[i]);
    ++i;
  }
  if (tok.empty()) return false;
  *out = tok;
  *kind = 'n';
  return true;
}

}  // namespace

ojson runPutexcel(const std::string& rawCmd, SessionState& session) {
  // The RAW line, because the tokenizer splits on `=` and on the comma inside
  // `matrix(M), names`. So the option tail has to be found and parsed here too
  // -- and then run through the SAME reject net every other handler uses, so a
  // typo is caught rather than dropped (defect class 3).
  std::string statement = rawCmd;
  std::vector<std::pair<std::string, eval::OptValue>> opts;
  {
    const long long comma = eval::findUnquotedComma(statement);
    if (comma >= 0) {
      const std::string tail = statement.substr(static_cast<size_t>(comma) + 1);
      statement = statement.substr(0, static_cast<size_t>(comma));
      if (!trimOf(tail).empty()) {
        try {
          opts = eval::parseOpts(tail);
        } catch (const eval::TokenizerError& te) {
          return ojson(te.block);
        }
      }
    }
  }
  // The dispatcher canonicalizes `parsed.opts`; this handler never sees that
  // vector, so it runs the SAME resolution against the SAME registered
  // vocabulary before its own net -- otherwise an abbreviation the net accepts
  // would still reach the `k == "replace"` comparisons below unresolved.
  eval::canonicalizeForCommand(opts, "putexcel");
  if (auto err = eval::rejectUnknownOptions(opts, kVocabPutexcel.known,
                                            kVocabPutexcel.unimplemented)) {
    return ojson(*err);
  }

  statement = dropCommandWord(statement);
  if (statement.empty()) {
    return ioSyntax("putexcel: nothing to do. Usage: putexcel set \"file\" [, replace|modify "
                    "sheet(S)] | putexcel A1 = <value> ... | putexcel A1:B2, merge | putexcel "
                    "clear|describe|save");
  }

  // The formatting note, at most once per `set` (decision 6). Fired here so it
  // covers a formatting option given ON the `set` line as well as on a cell
  // assignment.
  bool sawFormatOpt = false;
  for (const auto& [k, v] : opts) {
    (void)v;
    if (putexcelFormatOpts().count(k) != 0) {
      sawFormatOpt = true;
      break;
    }
  }

  const std::vector<std::string> words = quoteAwareWords(statement);
  const std::string head = words.empty() ? std::string() : lowerOf(words[0]);
  ojson out = ojson::array();

  // ---- putexcel set FILE [, replace|modify sheet(S)] ---------------------
  if (head == "set") {
    if (words.size() < 2) return ioSyntax("putexcel set: missing the workbook filename");
    if (words.size() > 2) {
      return ioSyntax("putexcel set: expected one filename, got `" + words[1] + " " + words[2] +
                      "`");
    }
    const std::string spelling = words[1];
    const std::string resolved = resolveUserPath(session, spelling);
    const bool replace = hasFlag(opts, "replace");
    const bool modify = hasFlag(opts, "modify");
    if (replace && modify) {
      return ioSyntax("putexcel set: `replace` and `modify` cannot both be given");
    }
    const bool exists = pathExists(resolved);
    // Stata's own default for an existing file is `modify`.
    const bool wantModify = modify || (exists && !replace);
    std::string sheet = optStr(opts, "sheet").value_or(std::string("Sheet1"));
    if (sheet.empty()) sheet = "Sheet1";

    ojson state = ojson::object();
    state["path"] = resolved;
    state["spelling"] = spelling;
    state["sheet"] = sheet;
    state["mode"] = wantModify ? "modify" : "replace";
    state["noteShown"] = false;
    state["cells"] = ojson::object();
    state["merges"] = ojson::array();

    if (wantModify && exists) {
      if (xlsx::worksheetCount(resolved) > 1) {
        return ioRefusal("putexcel", "single_sheet_only",
                         "putexcel: this build writes single-sheet workbooks, and " + spelling +
                             " has more than one worksheet — rewriting it would discard the "
                             "others. Write to a new file instead.");
      }
      if (optStr(opts, "sheet").has_value()) {
        // The reader available here cannot say what the existing sheet is
        // CALLED, so honouring `sheet()` would mean renaming whatever is there
        // and hoping it was the right one.
        return ioRefusal("putexcel", "single_sheet_only",
                         "putexcel: this build writes single-sheet workbooks, so `sheet()` cannot "
                         "be combined with modifying an existing workbook — drop `sheet()`, or "
                         "write to a new file with `replace`.");
      }
      xlsx::Sheet loaded;
      try {
        loaded = xlsx::read(resolved, false);
      } catch (const std::exception&) {
        return ioError("putexcel: " + spelling + " is not a workbook this build can read");
      }
      for (size_t r = 0; r < loaded.rows.size(); ++r) {
        for (size_t c = 0; c < loaded.rows[r].size(); ++c) {
          const xlsx::Cell& cell = loaded.rows[r][c];
          if (cell.blank) continue;
          ojson v = ojson::object();
          if (cell.isNumber)
            v["n"] = cell.number;
          else
            v["s"] = cell.text;
          state["cells"][addrOf(static_cast<int>(r) + 1, static_cast<int>(c) + 1)] = std::move(v);
        }
      }
      out.push_back(displayBlock(
          "(putexcel: " + spelling +
          " was read for its VALUES; any formatting it carried is not preserved by this build)"));
    }

    session.putexcel = std::move(state);
    if (sawFormatOpt) {
      session.putexcel["noteShown"] = true;
      out.push_back(displayBlock("(putexcel: cell formatting options are accepted and ignored in "
                                 "this build — values only)"));
    }
    return out;
  }

  // ---- putexcel clear ----------------------------------------------------
  if (head == "clear") {
    session.putexcel = ojson::object();
    return silent();
  }

  PutexcelState st = readPutexcelState(session);
  if (!st.ok) {
    return ioError("putexcel set must be called first — name the workbook with `putexcel set "
                   "\"file\"` before " +
                   std::string(head == "describe" ? "describing it" : "writing a cell") + ".");
  }

  // ---- putexcel describe -------------------------------------------------
  if (head == "describe") {
    out.push_back(displayBlock("putexcel: workbook " + st.spelling));
    out.push_back(displayBlock("putexcel: sheet     " + st.sheet));
    out.push_back(displayBlock(
        "putexcel: cells     " +
        std::to_string(session.putexcel.contains("cells") ? session.putexcel["cells"].size() : 0)));
    return out;
  }

  // ---- putexcel save / open ---------------------------------------------
  if (head == "save" || head == "open") {
    // Not a no-op that hides something: the workbook on disk is already current
    // (every assignment rewrites it in full), so there is nothing left to do
    // and saying so is the honest answer.
    return displayBlock("(putexcel: " + st.spelling +
                        " is rewritten after every assignment, so `" + head +
                        "` has nothing left to do)");
  }

  if (sawFormatOpt && !st.noteShown) {
    session.putexcel["noteShown"] = true;
    out.push_back(displayBlock("(putexcel: cell formatting options are accepted and ignored in "
                               "this build — values only)"));
  }

  // ---- putexcel A1:B2 [, merge] -----------------------------------------
  if (words.size() == 1 && statement.find('=') == std::string::npos) {
    const std::string range = words[0];
    const size_t colon = range.find(':');
    if (colon == std::string::npos) {
      return ioSyntax("putexcel: `" + range +
                      "` — expected `A1 = <value>`, a range like `A1:B2, merge`, or one of "
                      "set/save/clear/describe");
    }
    CellAddr a, b;
    if (!parseAddr(range.substr(0, colon), &a) || !parseAddr(range.substr(colon + 1), &b)) {
      return ioSyntax("putexcel: `" + range + "` is not a cell range");
    }
    if (b.row < a.row || b.col < a.col) {
      return ioSyntax("putexcel: `" + range +
                      "` runs backwards — name the top-left cell first");
    }
    if (!hasFlag(opts, "merge")) {
      return ioSyntax("putexcel: `" + range +
                      "` names a range but says nothing to do with it — add `, merge`");
    }
    const std::string canon = addrOf(a.row, a.col) + ":" + addrOf(b.row, b.col);
    bool already = false;
    for (const auto& m : session.putexcel["merges"]) {
      if (m.is_string() && m.get<std::string>() == canon) already = true;
    }
    if (!already) session.putexcel["merges"].push_back(canon);
    if (auto err = rewriteWorkbook(session, st)) return *err;
    return out;
  }

  // ---- putexcel A1 = <value> [B1 = <value> ...] --------------------------
  {
    size_t i = 0;
    size_t assignments = 0;
    auto setCell = [&](int row, int col, bool isNumber, double num, const std::string& text) {
      ojson v = ojson::object();
      if (isNumber)
        v["n"] = num;
      else
        v["s"] = text;
      session.putexcel["cells"][addrOf(row, col)] = std::move(v);
    };
    while (true) {
      while (i < statement.size() && std::isspace(static_cast<unsigned char>(statement[i]))) ++i;
      if (i >= statement.size()) break;
      size_t j = i;
      while (j < statement.size() && !std::isspace(static_cast<unsigned char>(statement[j])) &&
             statement[j] != '=') {
        ++j;
      }
      const std::string addrTok = statement.substr(i, j - i);
      CellAddr at;
      if (!parseAddr(addrTok, &at)) {
        return ioSyntax("putexcel: `" + addrTok +
                        "` is not a cell address (expected something like `A1`)");
      }
      i = j;
      while (i < statement.size() && std::isspace(static_cast<unsigned char>(statement[i]))) ++i;
      if (i >= statement.size() || statement[i] != '=') {
        return ioSyntax("putexcel: expected `=` after " + addrTok);
      }
      ++i;
      std::string value;
      char kind = 0;
      if (!scanValue(statement, i, &value, &kind)) {
        return ioSyntax("putexcel: " + addrTok +
                        " = <value> — expected a quoted string, a number, `(expression)` or "
                        "`matrix(NAME)`");
      }
      ++assignments;
      if (kind == 's') {
        setCell(at.row, at.col, false, 0.0, value);
      } else if (kind == 'n') {
        double d = 0.0;
        if (!parseStrictDouble(value, &d)) {
          return ioSyntax("putexcel: `" + value +
                          "` is neither a number nor a quoted string — quote it to write it as "
                          "text");
        }
        setCell(at.row, at.col, true, d, std::string());
      } else if (kind == 'e') {
        std::optional<double> d = evalScalarExpr(value, session);
        if (!d.has_value()) {
          return ioError("putexcel: (" + value + ") does not resolve to a number");
        }
        setCell(at.row, at.col, true, *d, std::string());
      } else {  // 'm' -- matrix(NAME)
        const std::string mname = trimOf(value);
        const eval::EvalMatrix* m = session.eval.findMatrix(mname);
        if (m == nullptr) return ioErrorRc("putexcel: matrix " + mname + " not found", 111);
        const bool names = hasFlag(opts, "names");
        int r0 = at.row;
        if (names) {
          // Column names across the top, row names down the left -- so the
          // block starts one row and one column in from the address given.
          for (size_t c = 0; c < m->colnames.size(); ++c) {
            setCell(r0, at.col + 1 + static_cast<int>(c), false, 0.0, m->colnames[c]);
          }
          ++r0;
        }
        for (size_t r = 0; r < m->data.size(); ++r) {
          const int rowNo = r0 + static_cast<int>(r);
          int c0 = at.col;
          if (names) {
            setCell(rowNo, c0, false, 0.0,
                    r < m->rownames.size() ? m->rownames[r] : std::string());
            ++c0;
          }
          for (size_t c = 0; c < m->data[r].size(); ++c) {
            const double v = m->data[r][c];
            // Missing is `.`, never 0.
            if (std::isnan(v))
              setCell(rowNo, c0 + static_cast<int>(c), false, 0.0, ".");
            else
              setCell(rowNo, c0 + static_cast<int>(c), true, v, std::string());
          }
        }
      }
    }
    if (assignments == 0) {
      return ioSyntax("putexcel: `" + statement +
                      "` — expected `A1 = <value>`, a range like `A1:B2, merge`, or one of "
                      "set/save/clear/describe");
    }
  }

  if (auto err = rewriteWorkbook(session, st)) return *err;
  return out;
}

}  // namespace piet::handlers
