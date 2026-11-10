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

// `wide` asks for one observation per output line, which is exactly and only
// what this writer does -- honoured by construction. `dictionary` writes a
// companion .dct and `fast` changes the buffering contract; neither is done.
const piet::eval::OptVocab& kVocabOutfile = piet::eval::registerOptVocab(
    {"outfile"}, {{"replace", "noquote", "comma", "nolabel", "wide"}, {"dictionary", "fast"}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace file_command_detail;

namespace {

// ===========================================================================
// Reading the loaded dataset (the two writers)
// ===========================================================================

// The value-label table attached to `name`, or nullptr. Stata's `outsheet` and
// `outfile` write a labeled numeric as its LABEL text by default; `nolabel`
// writes the code. The labels live in the SESSION, which is why both writers
// are PIET-side rather than engine-side.
const std::map<std::string, std::string>* labelsFor(SessionState& session,
                                                    const std::string& name) {
  auto lv = session.varLblname.find(name);
  if (lv == session.varLblname.end()) return nullptr;
  auto ld = session.labelDefs.find(lv->second);
  if (ld == session.labelDefs.end()) return nullptr;
  return &ld->second;
}

// A labeled numeric's label text, if it has one for this value.
const std::string* labelTextFor(const std::map<std::string, std::string>* labels, double v) {
  if (labels == nullptr || std::isnan(v)) return nullptr;
  auto it = labels->find(eval::pythonFloatRepr(v));
  if (it == labels->end() && v == std::floor(v)) {
    it = labels->find(std::to_string(static_cast<long long>(v)));
  }
  if (it == labels->end()) return nullptr;
  return &it->second;
}

}  // namespace

// ===========================================================================
// outfile
// ===========================================================================
//
// Decision 4. Stata's oldest writer: no header row, one observation per line,
// values separated by a single space (or a comma with `comma`), strings in
// double quotes (unless `noquote`), numbers in `%10.0g` and missing as `.` --
// in the comma form too, where an empty field would be read back as missing by
// some readers and as an empty string by others.
ojson runOutfile(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabOutfile.known,
                                            kVocabOutfile.unimplemented)) {
    return ojson(*err);
  }
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  std::vector<std::string> varlist;
  std::string pathArg;
  if (!splitUsing(dropQualifierWords(quoteAwareWords(preCommaArgs(parsed.raw))), &varlist,
                  &pathArg)) {
    return ioSyntax("outfile: missing the file. Usage: outfile [varlist] using \"file\" "
                    "[, replace comma noquote nolabel wide]");
  }

  const bool replace = hasFlag(parsed.opts, "replace");
  const bool noQuote = hasFlag(parsed.opts, "noquote");
  const bool noLabel = hasFlag(parsed.opts, "nolabel");
  const bool comma = hasFlag(parsed.opts, "comma");
  const bool wide = hasFlag(parsed.opts, "wide");

  std::string resolved = resolveUserPath(session, pathArg);
  // "If filename is specified without a suffix, .raw is assumed" (outfile
  // help; StataBE `outfile using o2` names `o2.raw`).
  assumeSuffix(&resolved, &pathArg, ".raw");
  if (auto err = refuseExisting(resolved, pathArg, replace)) return *err;
  if (auto err = refuseIfSessionHbin(session, "outfile", resolved, pathArg)) return *err;

  Datastore* base = nullptr;
  try {
    base = session.datastore();
  } catch (const std::exception& e) {
    return ioError(std::string("outfile: the dataset in memory could not be read — ") + e.what(),
                   "data_io");
  }
  if (base == nullptr) return ojson(eval::noDataLoaded());
  std::vector<std::string> cols = varlist;
  if (cols.empty()) cols = base->names();
  for (const auto& v : cols) {
    if (!base->has(v)) return ioError("variable " + v + " not found", "unknown_variable");
  }

  // Each variable's field, laid out the way Stata's outfile does (w2ar
  // log:941-999): a numeric right-aligned in its display format's width, a
  // string `"quoted"` and left-aligned in its str# width plus the two quotes,
  // a date (or a value label) quoted as text; fields two blanks apart; a
  // record wider than 80 columns continues on the next line unless `wide`.
  // `noquote` drops the quotes but the line keeps its quoted length.
  struct OutCol {
    const std::map<std::string, std::string>* labels = nullptr;
    std::string fmt;
    char dateFamily = 0;
    bool isString = false;
    int width = 0;   // the field's width, quotes included
  };
  std::vector<OutCol> oc;
  oc.reserve(cols.size());
  for (const auto& name : cols) {
    OutCol o;
    const cecon::Column& col = base->col(name);
    if (col.dtype == cecon::DType::Factor) {
      o.isString = true;
      int w = 0;
      auto vm = session.varMeta.find(name);
      if (vm != session.varMeta.end() && vm->second.stataType.has_value() &&
          vm->second.stataType->size() > 3 && vm->second.stataType->compare(0, 3, "str") == 0 &&
          std::isdigit(static_cast<unsigned char>((*vm->second.stataType)[3]))) {
        w = std::atoi(vm->second.stataType->c_str() + 3);
      }
      for (const auto& lv : col.levels) w = std::max(w, static_cast<int>(lv.size()));
      o.width = std::max(w, 1) + 2;
    } else {
      if (!noLabel) o.labels = labelsFor(session, name);
      o.fmt = session.displayFormat(base->varinfo(name));
      std::string f = o.fmt;
      if (!f.empty() && f[0] == '%') f.erase(0, 1);
      if (!f.empty() && f[0] == '-') f.erase(0, 1);
      int w = std::atoi(f.c_str());
      if (w <= 0) w = 9;
      if (f.size() >= 2 && f[0] == 't') {
        o.dateFamily = f.size() >= 2 ? f[1] : 0;
        w += 2;  // a date is written quoted
      }
      if (o.labels != nullptr) {
        // A labeled numeric is written quoted: its field is the wider of the
        // format and the longest label, plus the quotes.
        int lw = 0;
        for (const auto& kv : *o.labels) lw = std::max(lw, static_cast<int>(kv.second.size()));
        w = std::max(w, lw) + 2;
      }
      o.width = w;
    }
    oc.push_back(o);
  }

  Datastore* ds = base;
  std::unique_ptr<Datastore> filtered;
  // `outfile s g if year == 2001 using f` puts the qualifier BEFORE `using`;
  // the expression ends where `using` begins (w2ar dx_outf_if, log:4990).
  std::optional<std::string> ifExpr = parsed.ifExpr;
  if (ifExpr.has_value()) {
    static const std::regex reUsingTail(R"(\s+using\s.*$)");
    ifExpr = std::regex_replace(*ifExpr, reUsingTail, "");
  }
  if ((ifExpr.has_value() && !ifExpr->empty()) ||
      (parsed.inRange.has_value() && !parsed.inRange->empty())) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, base->path(), ifExpr, parsed.inRange);
    } catch (const CobraError& ce) {
      return ioError(cobraErrorMessage(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, parsed.inRange.value_or(std::string()), "outfile");
    filtered = std::make_unique<Datastore>(fr.dataPath);
    ds = filtered.get();
  }
  std::vector<const cecon::Column*> colp;
  for (const auto& name : cols) colp.push_back(&ds->col(name));

  auto quoted = [&](const std::string& t) { return noQuote ? t : "\"" + t + "\""; };
  auto rtrim = [](std::string t) {
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t;
  };

  std::string body;
  const int64_t n = ds->nRows();
  for (int64_t r = 0; r < n; ++r) {
    size_t budget = 0;  // each field costs its width plus 2 against the 80 columns
    if (comma) {
      // The comma form: values only, comma-separated.
      for (size_t c = 0; c < oc.size(); ++c) {
        if (c) body += ',';
        const cecon::Column& col = *colp[c];
        if (col.dtype == cecon::DType::Factor) {
          const int32_t code = col.codes()[r];
          body += quoted(code < 0 ? std::string() : col.levels[static_cast<size_t>(code)]);
          continue;
        }
        const double v = col.f64()[r];
        if (const std::string* lbl = labelTextFor(oc[c].labels, v)) {
          body += quoted(*lbl);
          continue;
        }
        body += std::isnan(v) ? std::string(".") : fmt::general(v, 10, 0);
      }
      body += '\n';
      continue;
    }
    std::string line, layout;  // `layout` = the line as quoted mode would lay it out
    for (size_t c = 0; c < oc.size(); ++c) {
      const OutCol& o = oc[c];
      const cecon::Column& col = *colp[c];
      std::string text;     // the field's characters
      bool leftAlign = false;
      bool isQuotedKind = false;
      if (col.dtype == cecon::DType::Factor) {
        const int32_t code = col.codes()[r];
        text = code < 0 ? std::string() : col.levels[static_cast<size_t>(code)];
        leftAlign = true;
        isQuotedKind = true;
      } else {
        const double v = col.f64()[r];
        if (const std::string* lbl = labelTextFor(o.labels, v)) {
          text = *lbl;
          leftAlign = true;
          isQuotedKind = true;
        } else if (std::isnan(v)) {
          text = ".";
        } else if (o.dateFamily != 0) {
          std::string d;
          try {
            d = eval::applyStataFmt(v, o.fmt);
          } catch (...) {
            d.clear();
          }
          text = trimOf(d);
          leftAlign = true;
          isQuotedKind = true;
        } else {
          std::string t;
          try {
            t = eval::applyStataFmt(v, o.fmt);
          } catch (...) {
            t = fmt::general(v, 10, 0);
          }
          text = trimOf(t);
        }
      }
      const std::string shown = isQuotedKind ? quoted(text) : text;
      const std::string laid = isQuotedKind ? "\"" + text + "\"" : text;
      const size_t w = static_cast<size_t>(o.width);
      auto pad = [&](const std::string& t, size_t width) {
        if (t.size() >= width) return t;
        return leftAlign ? t + std::string(width - t.size(), ' ')
                         : std::string(width - t.size(), ' ') + t;
      };
      const size_t fieldW = std::max(w, laid.size());
      // A record continues on a new line once its fields (each with its 2
      // separator columns) would pass 80 (dx_outf_all writes the date of
      // every record on its own line, log:969-978).
      if (!layout.empty() && !wide && budget + fieldW + 2 > 80) {
        const std::string lt = rtrim(layout);
        std::string ln = rtrim(line);
        if (ln.size() < lt.size()) ln += std::string(lt.size() - ln.size(), ' ');
        body += ln + "\n";
        line.clear();
        layout.clear();
        budget = 0;
      }
      budget += fieldW + 2;
      if (!layout.empty()) {
        line += "  ";
        layout += "  ";
      }
      // noquote: the field is two narrower, the line keeps its length at the
      // end (dx_outf_noquote_wide `       2  b c             2.02  `).
      line += pad(shown, isQuotedKind && noQuote ? (w >= 2 ? w - 2 : 0) : w);
      layout += pad(laid, w);
    }
    const std::string lt = rtrim(layout);
    std::string ln = rtrim(line);
    if (ln.size() < lt.size()) ln += std::string(lt.size() - ln.size(), ' ');
    body += ln + "\n";
  }

  if (auto err = commitBytes("outfile", resolved, pathArg, body)) return *err;
  return displayBlock("file " + pathArg + " saved");
}

}  // namespace piet::handlers
