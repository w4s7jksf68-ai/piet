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

namespace {

// `double` asks for double storage: it keeps the numbers as read, where the
// default stores a non-integer column as float (delimited::typeColumn).
// Under `set type double` the default is double, and `nodouble` asks for the
// float columns (insheet's help). `clear` is read in
// the only sense it has here: an `insheet` always replaces what is in memory.
// `case` is read (it suppresses the lower-casing pass).
const piet::eval::OptVocab& kVocabInsheet = piet::eval::registerOptVocab(
    {"insheet"}, {{"clear", "names", "nonames", "comma", "tab", "delimiter", "delim", "case",
                   "double", "nodouble"},
                  {}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace file_command_detail;

// ===========================================================================
// insheet
// ===========================================================================
//
// insheet reads through the one native delimited module (delimited_io.cpp) in
// its own mode: the delimiter from the first line (tab, else comma, else the
// line is one field), trimmed fields with the untrimmed width, Stata's naming
// rule (lower case unless `case`, illegal characters removed, the cell as the
// label) and the storage types import delimited uses (w2ar log:3526-3690).

ojson runInsheet(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabInsheet.known,
                                            kVocabInsheet.unimplemented)) {
    return ojson(*err);
  }
  if (hasFlag(parsed.opts, "double") && hasFlag(parsed.opts, "nodouble"))
    return ioSyntax("options double and nodouble may not be combined");

  std::vector<std::string> varlist;
  std::string pathArg;
  if (!splitUsing(quoteAwareWords(preCommaArgs(parsed.raw)), &varlist, &pathArg)) {
    return ioSyntax("insheet: missing the file. Usage: insheet [varlist] using \"file\" [, clear "
                    "names nonames comma tab delimiter(D) case double]");
  }
  for (const auto& v : varlist) {
    if (!strutil::isValidStataIdentifier(v)) return ioSyntax(v + ": invalid variable name");
  }

  // insheet reads into an EMPTY dataset: without `clear` over data in memory
  // it is r(18) and the data stay (w2ar dx_ins_noclear, log:3678); with
  // `clear` the data go first, so a missing file leaves k=0 (dx_ins_missing
  // 601 k=0, log:3690).
  const bool clear = hasFlag(parsed.opts, "clear");
  if (!clear && session.hasData()) {
    Datastore* ds = nullptr;
    try {
      ds = session.datastore();
    } catch (const std::exception&) {
      ds = nullptr;
    }
    if (ds != nullptr && ds->nCols() > 0) {
      return ioErrorRc("you must start with an empty dataset", 18);
    }
  }
  if (clear && session.hasData()) {
    ojson cleared = runClear("", session);
    if (isErrorBlock(cleared)) return cleared;
  }

  const std::string resolved = resolveUserPath(session, pathArg);
  if (!pathExists(resolved)) return ioErrorRc("file " + typedUserPath(pathArg) + " not found", 601);
  std::string text;
  {
    std::ifstream f(cecon::to_native_path(resolved), std::ios::binary);
    if (!f) return ioError("insheet: cannot read " + pathArg, "data_io");
    std::ostringstream ss;
    ss << f.rdbuf();
    text = ss.str();
  }

  // -- the delimiter: delimiter()/comma/tab, else the first line's own -- tab
  // when it holds one, else comma when it holds one, else none (the whole
  // line is one variable: dx_ins_028_names, log:3630).
  char delim = 0;
  {
    std::string derr;
    if (!readDelimiterOpt(parsed.opts, &delim, &derr)) return ioError(derr, "unsupported_option");
  }
  if (hasFlag(parsed.opts, "comma")) delim = ',';
  if (hasFlag(parsed.opts, "tab")) delim = '\t';
  if (delim == 0) {
    const size_t eol = text.find('\n');
    const std::string head = text.substr(0, eol == std::string::npos ? text.size() : eol);
    if (head.find('\t') != std::string::npos) delim = '\t';
    else if (head.find(',') != std::string::npos) delim = ',';
  }

  const delimited::InsheetRows rows = delimited::parseInsheet(text, delim);
  if (rows.fields.empty()) {
    // An empty file: Stata answers r(612) (dx_ins_empty, log:3668).
    return ioErrorRc("file " + pathArg + " is empty -- unexpected end of file", 612);
  }

  // -- the header: `names`/`nonames`, a varlist (implies nonames), else
  // Stata's automatic rule: a first row whose every field is a number is data.
  const bool wantNames = hasFlag(parsed.opts, "names");
  const bool wantNoNames = hasFlag(parsed.opts, "nonames") || !varlist.empty();
  if (wantNames && wantNoNames && varlist.empty()) {
    return ioSyntax("insheet: `names` and `nonames` cannot both be given");
  }
  bool header = true;
  if (wantNoNames) {
    header = false;
  } else if (!wantNames) {
    bool allNumeric = true;
    for (const auto& f : rows.fields[0]) {
      double d = 0.0;
      if (f.empty() || !delimited::parseNumber(f, &d)) {
        allNumeric = false;
        break;
      }
    }
    header = !allNumeric;
  }

  const size_t first = header ? 1 : 0;
  // A data row with MORE fields than the names row widens the dataset, as
  // import delimited does: the extra columns are v<column>, missing on the
  // shorter rows (BUG-414: `a,b,c` over `4,5` and `6,7,8,9,10` reads a b c v4
  // v5, 3 obs, StataBE insheet rc 0). Where a header name already holds
  // v<column>, insheet takes the lowest free v# (`v3,b` over `3,4,5` reads v3 b
  // v1) and import delimited refuses (delimited::Fallback).
  size_t k = header ? rows.fields[0].size() : 0;
  for (size_t r = first; r < rows.fields.size(); ++r) k = std::max(k, rows.fields[r].size());
  std::vector<std::string> headerCells(k);
  if (header) {
    for (size_t c = 0; c < rows.fields[0].size() && c < k; ++c) headerCells[c] = rows.fields[0][c];
  }
  delimited::NameResult nr = delimited::legalNames(
      headerCells, hasFlag(parsed.opts, "case") ? delimited::Case::kPreserve : delimited::Case::kLower,
      delimited::Fallback::kLowestFree);
  if (!header) {
    for (auto& l : nr.labels) l.clear();
  }
  if (varlist.size() > k) return ioErrorRc("too many variables specified", 103);
  for (size_t i = 0; i < varlist.size(); ++i) nr.names[i] = varlist[i];

  delimited::Table table;
  table.names = nr.names;
  table.labels = nr.labels;
  std::vector<size_t> widthFloor(k, 0);
  for (size_t r = first; r < rows.fields.size(); ++r) {
    std::vector<std::string> row(k);
    for (size_t c = 0; c < rows.fields[r].size() && c < k; ++c) {
      row[c] = rows.fields[r][c];
      widthFloor[c] = std::max(widthFloor[c], rows.widths[r][c]);
    }
    table.rows.push_back(std::move(row));
  }
  // Float columns unless double storage is asked for: by `double`, or by the
  // c(type) default `set type double` sets with no `nodouble` to undo it
  // (insheet's help; w2k_binscatter.log:1477 BS_077_data_o1 reads
  // float(1.6209999)).
  const bool asDouble =
      !hasFlag(parsed.opts, "nodouble") &&
      (hasFlag(parsed.opts, "double") || session.defaultStorageType != "float");
  table.types.resize(k);
  for (size_t c = 0; c < k; ++c) {
    std::vector<const std::string*> cells;
    cells.reserve(table.rows.size());
    for (const auto& row : table.rows) cells.push_back(&row[c]);
    table.types[c] = delimited::typeColumn(cells, false, false, asDouble, widthFloor[c],
                                           /*precisionToDouble=*/false);
  }
  if (auto err = delimited::stageAndAdopt(session, table, "insheet", resolved)) return *err;
  return mutationResult(session, "insheet");
}

}  // namespace piet::handlers
