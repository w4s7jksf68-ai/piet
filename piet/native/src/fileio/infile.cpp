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
#include "fixed_column_import.h"

namespace {

// `automatic` asks infile to build value labels from the string values it
// reads, and `byvariable()` changes the record ORDER (all of v1, then all of
// v2). Both change what the dataset IS, so neither may be accepted unread.
// `using(data)` is the dictionary form's data file (runDictionaryRead).
const piet::eval::OptVocab& kVocabInfile =
    piet::eval::registerOptVocab({"infile"}, {{"clear", "using"}, {"automatic", "byvariable"}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace file_command_detail;

// ===========================================================================
// infile (free format)
// ===========================================================================
//
// Decision 3. Stata's free-format reader: values are separated by whitespace,
// commas or line breaks INTERCHANGEABLY, a double-quoted run is one value, `.`
// is missing, and a RECORD is however many values the varlist declares --
// source line breaks carry no meaning at all. A `str#` prefix declares the next
// variable a string; `_skip(#)` consumes # values without storing them.
namespace {

// ===========================================================================
// cobra plumbing
// ===========================================================================

std::string nextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);
}

CobraError errorFromRaw(const CobraRunResult& r) {
  std::string t = r.stderr_text;
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
  try {
    nlohmann::json payload = nlohmann::json::parse(t);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                        err.at("message").get<std::string>(), err.value("detail", std::string()));
    }
  } catch (const nlohmann::json::exception&) {
  }
  return CobraError("engine_error", t.empty() ? "cobra failed" : t);
}

// Run ONE cobra verb. nullopt = success; otherwise the error BLOCK to return.
// Never a bare parse of stdout_text and never a swallowed rc (IDIOMS).
std::optional<ojson> runVerb(const std::vector<std::string>& args) {
  CobraRunResult cr = runCobra(args);
  if (cr.ok) return std::nullopt;
  CobraError ce = errorFromRaw(cr);
  return ioError(cobraErrorMessage(ce), ce.code);
}

// ===========================================================================
// CSV transport (the ONE staging shape the three readers share)
// ===========================================================================

// One CSV field, quoted only when it must be -- the same convention
// delimited_export.cpp's export writer uses.
std::string csvField(const std::string& s, char delim) {
  bool needQuote = false;
  for (char c : s) {
    if (c == delim || c == '"' || c == '\n' || c == '\r') {
      needQuote = true;
      break;
    }
  }
  if (!needQuote) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"')
      out += "\"\"";
    else
      out += c;
  }
  out += "\"";
  return out;
}

// A whole table on its way to `cobra convert`. Factor columns are named
// explicitly rather than left to type inference, because a declared string
// column whose visible values happen to be all-numeric would otherwise come
// back f64 -- the declared type silently discarded.
struct CsvTable {
  std::vector<std::string> names;
  std::vector<char> isFactor;                  // parallel to `names`
  std::vector<std::vector<std::string>> rows;  // rows[r][c]; "" == missing
};

// Stage `t` as a CSV and convert it to an hbin. nullopt = success and *outPath
// holds the new frame; otherwise the error block to return. A torn file is
// removed rather than left where a later `load()` could mmap it (BUG-124).
std::optional<ojson> writeAndConvert(SessionState& session, const CsvTable& t,
                                     const std::string& prefix, std::string* outPath) {
  const std::string csvPath = nextPath(session.workdir(), prefix) + ".csv";
  registerScratchMint(csvPath);            // BUG-389
  session.noteProjectionScratch(csvPath);  // read once by `convert`, then dead
  {
    std::ofstream f(cecon::to_native_path(csvPath), std::ios::binary);
    if (!f) return ioError(prefix + ": cannot stage the converted rows", "data_io");
    for (size_t c = 0; c < t.names.size(); ++c) {
      if (c) f << ',';
      f << csvField(t.names[c], ',');
    }
    f << "\n";
    for (const auto& row : t.rows) {
      // BUG-1078: a row whose every cell is missing would be a BLANK line, and
      // cobra's CSV reader skips blank lines (BUG-697's pad rows, same reader),
      // so a one-variable `input` with a `""` or `.` row lost it and stopped on
      // the row-count assert. A quoted empty first field keeps the row; convert
      // reads it as missing for a factor and for an f64 column alike.
      bool allEmpty = true;
      for (size_t c = 0; c < t.names.size() && allEmpty; ++c) {
        if (c < row.size() && !row[c].empty()) allEmpty = false;
      }
      for (size_t c = 0; c < t.names.size(); ++c) {
        if (c) f << ',';
        if (c == 0 && allEmpty) {
          f << "\"\"";
        } else if (c < row.size()) {
          f << csvField(row[c], ',');
        }
      }
      f << "\n";
    }
    f.flush();
    if (!f) {
      cecon::native_unlink(csvPath);
      return ioError(prefix + ": failed staging the converted rows (disk full or I/O error)",
                     "data_io");
    }
  }
  std::string factorList;
  for (size_t c = 0; c < t.names.size(); ++c) {
    if (!t.isFactor[c]) continue;
    if (!factorList.empty()) factorList += ",";
    factorList += t.names[c];
  }
  const std::string hbin = nextPath(session.workdir(), prefix);
  std::vector<std::string> args = {"convert", "--data", csvPath, "--out", hbin, "--delim", ","};
  if (!factorList.empty()) {
    args.push_back("--factor");
    args.push_back(factorList);
  }
  if (auto err = runVerb(args)) {
    cecon::native_unlink(hbin);
    return err;
  }
  *outPath = hbin;
  return std::nullopt;
}

// Adopt `hbin` as the session's dataset, exactly the way `use` does: the
// resident chain is DROPPED (a load from disk never continues it), the display
// metadata plane is cleared (a staged CSV declares no labels), and the source
// the user named becomes c(filename). Unlike `use`, c(changed) is 1: the data
// read have not been saved (StataBE 19.5, BUG-1478: 1 after `infile a b using
// f.raw, clear`, as after import delimited).
void adoptLoadedFrame(SessionState& session, const std::string& hbin,
                      const std::string& sourceResolved) {
  session.load(hbin, SessionState::LoadIntent::FreshRead);
  session.clearMetadata();
  session.setCurrentFile(sourceResolved);
  session.datasetChanged = true;
  session.invalidateDatastore();
}

// One entry of an `infile` varlist: a variable, or a skip.
struct InfileSlot {
  std::string name;    // empty for a `_skip(#)` slot
  bool isString = false;
  int skip = 0;        // >0 only for a skip slot
  std::string declaredType;  // "str3", "int", ... -- recorded as metadata
};

// Every free-format value in `bytes`, in order. A quoted run is one value even
// when it contains a delimiter; `.` stays `.` (the caller decides it is
// missing, because a string column's `.` is the literal character).
std::vector<std::string> freeFormatValues(const std::string& bytes, std::vector<char>* wasQuoted) {
  std::vector<std::string> out;
  size_t i = 0;
  const size_t n = bytes.size();
  while (i < n) {
    const char c = bytes[i];
    if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == '"') {
      std::string tok;
      ++i;
      while (i < n && bytes[i] != '"') {
        tok.push_back(bytes[i]);
        ++i;
      }
      if (i < n) ++i;  // the closing quote
      out.push_back(tok);
      wasQuoted->push_back(1);
      continue;
    }
    std::string tok;
    while (i < n && bytes[i] != ',' && !std::isspace(static_cast<unsigned char>(bytes[i]))) {
      tok.push_back(bytes[i]);
      ++i;
    }
    out.push_back(tok);
    wasQuoted->push_back(0);
  }
  return out;
}

}  // namespace

ojson runInfile(const eval::ParsedCommand& parsedIn, SessionState& session) {
  const eval::ParsedCommand parsed = joinSpacedOptionValues(parsedIn);
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabInfile.known,
                                            kVocabInfile.unimplemented)) {
    return ojson(*err);
  }

  std::vector<std::string> words;
  std::string pathArg;
  if (!splitUsing(quoteAwareWords(preCommaArgs(parsed.raw)), &words, &pathArg)) {
    return ioSyntax("infile: missing the file. Usage: infile varlist using \"file\" [, clear]");
  }
  if (auto err = requireEmptyOrClear(parsed, session)) return *err;
  if (words.empty()) {
    // `infile using dict [, using(data)]` -- the dictionary form, 166 of the
    // corpus's 168 infile lines (w2ar dx_inf_010_dict log:3953, the dictionary
    // naming its data file dx_inf_037 log:4007).
    return runDictionaryRead("infile", pathArg, parsed, session);
  }

  // -- the varlist spec ----------------------------------------------------
  std::vector<InfileSlot> slots;
  {
    std::string pendingType;
    bool pendingIsString = false;
    for (const auto& w : words) {
      bool isStr = false;
      if (isTypeToken(w, &isStr)) {
        pendingType = lowerOf(w);
        pendingIsString = isStr;
        continue;
      }
      const std::string lw = lowerOf(w);
      if (lw.compare(0, 6, "_skip(") == 0 && !lw.empty() && lw.back() == ')') {
        long long k = 0;
        if (!parseStrictInt(w.substr(6, w.size() - 7), &k) || k < 1) {
          return ioSyntax("infile: `" + w + "` — _skip() takes a positive whole number of values");
        }
        InfileSlot s;
        s.skip = static_cast<int>(k);
        slots.push_back(s);
        pendingType.clear();
        pendingIsString = false;
        continue;
      }
      if (!strutil::isValidStataIdentifier(w)) {
        return ioSyntax("infile: invalid variable name `" + w + "' — " +
                        strutil::stataIdentifierRuleText());
      }
      InfileSlot s;
      s.name = w;
      s.isString = pendingIsString;
      s.declaredType = pendingType;
      slots.push_back(s);
      pendingType.clear();
      pendingIsString = false;
    }
  }
  size_t width = 0;
  std::vector<std::string> names;
  for (const auto& s : slots) {
    width += (s.skip > 0) ? static_cast<size_t>(s.skip) : 1u;
    if (s.skip == 0) names.push_back(s.name);
  }
  if (names.empty()) return ioSyntax("infile: no variables named");
  {
    std::set<std::string> seen;
    for (const auto& n : names) {
      if (!seen.insert(n).second) return ioSyntax("infile: variable " + n + " named twice");
    }
  }

  std::string resolved = resolveUserPath(session, pathArg);
  std::string shown = typedUserPath(pathArg);
  assumeSuffix(&resolved, &shown, ".raw");
  if (!pathExists(resolved)) return ioErrorRc("file " + shown + " not found", 601);
  auto bytes = readWholeFile(resolved);
  if (!bytes.has_value()) return ioError("infile: cannot read " + shown, "data_io");

  std::vector<char> quoted;
  const std::vector<std::string> values = freeFormatValues(*bytes, &quoted);

  CsvTable table;
  for (const auto& s : slots) {
    if (s.skip > 0) continue;
    table.names.push_back(s.name);
    table.isFactor.push_back(s.isString ? 1 : 0);
  }
  // The record loop. A trailing PARTIAL record is padded with missing, which is
  // what Stata does (it warns; PIET says so in a display line below).
  const size_t nRec = width == 0 ? 0 : (values.size() + width - 1) / width;
  bool padded = false;
  for (size_t r = 0; r < nRec; ++r) {
    std::vector<std::string> row;
    size_t v = r * width;
    for (const auto& s : slots) {
      if (s.skip > 0) {
        v += static_cast<size_t>(s.skip);
        continue;
      }
      if (v >= values.size()) {
        row.emplace_back();  // "" == missing
        padded = true;
        ++v;
        continue;
      }
      const std::string& tok = values[v];
      const bool wasQuoted = quoted[v] != 0;
      ++v;
      if (s.isString) {
        row.push_back(tok);
        continue;
      }
      if (!wasQuoted && tok == ".") {
        row.emplace_back();
        continue;
      }
      double d = 0.0;
      if (!parseStrictDouble(tok, &d)) {
        return ioError("infile: `" + tok + "` cannot be read as a number for variable " + s.name +
                       " (declare it a string with a `str#` prefix if it is text)");
      }
      row.push_back(eval::pythonFloatRepr(d));
    }
    table.rows.push_back(std::move(row));
  }

  std::string hbin;
  if (auto err = writeAndConvert(session, table, "infile", &hbin)) return *err;
  adoptLoadedFrame(session, hbin, resolved);

  // The DECLARED types are metadata this build records but does not enforce --
  // the frame is f64/factor throughout. Recording them is what makes a later
  // `save x.dta` write the type the user asked for (the --in-meta sidecar).
  for (const auto& s : slots) {
    if (s.skip > 0 || s.declaredType.empty()) continue;
    session.varMeta[s.name].stataType = s.declaredType;
  }

  ojson out = ojson::array();
  if (padded) {
    out.push_back(displayBlock("(infile: the last record was short — the missing values were "
                               "filled with `.`)"));
  }
  out.push_back(mutationResult(session, "infile"));
  return out;
}

}  // namespace piet::handlers
