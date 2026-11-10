// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// spreadsheet export.
#include "data_io_internal.h"
namespace piet::handlers::data_io_detail {



// One A1-style cell address. `col`/`row` come back 0 for a side the address
// leaves out, so "A" is column 1 with an unbounded row and "8" is row 8 with an
// unbounded column.
bool parseCellAddr(const std::string& s, int* col, int* row) {
  size_t i = 0;
  long long c = 0;
  while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) {
    c = c * 26 + (std::toupper(static_cast<unsigned char>(s[i])) - 'A' + 1);
    if (c > 1000000) return false;
    ++i;
  }
  long long r = 0;
  bool anyDigit = false;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
    r = r * 10 + (s[i] - '0');
    if (r > 100000000LL) return false;
    anyDigit = true;
    ++i;
  }
  if (i != s.size()) return false;
  if (c == 0 && !anyDigit) return false;
  *col = static_cast<int>(c);
  *row = anyDigit ? static_cast<int>(r) : 0;
  return true;
}


// `cellrange([start][:end])` — [D] import excel p.495. Both sides are optional,
// so `A8`, `A3:AG281`, `:D70` and `A3:` are all legal spellings.
bool parseCellRangeOpt(const std::string& spec, xlsx::ReadOptions* out, std::string* err) {
  const std::string s = trim(spec);
  if (s.empty()) {
    *err = "cellrange(): expected a cell or a cell range, for example cellrange(A3:AG281)";
    return false;
  }
  const size_t colon = s.find(':');
  const std::string start = colon == std::string::npos ? s : trim(s.substr(0, colon));
  const std::string end = colon == std::string::npos ? std::string() : trim(s.substr(colon + 1));
  if (!start.empty() && !parseCellAddr(start, &out->firstCol, &out->firstRow)) {
    *err = "cellrange(): `" + start + "` is not a cell reference";
    return false;
  }
  if (!end.empty() && !parseCellAddr(end, &out->lastCol, &out->lastRow)) {
    *err = "cellrange(): `" + end + "` is not a cell reference";
    return false;
  }
  if (out->lastRow > 0 && out->firstRow > out->lastRow) {
    *err = "cellrange(): the range ends before it starts (" + s + ")";
    return false;
  }
  if (out->lastCol > 0 && out->firstCol > out->lastCol) {
    *err = "cellrange(): the range ends before it starts (" + s + ")";
    return false;
  }
  return true;
}


// Stata's own default variable names when there is no `firstrow`: the SHEET's
// column letters, upper case (BUG-425). `idx0` is 0-based from column A.
std::string excelColumnName(size_t idx0) {
  std::string s;
  for (size_t x = idx0 + 1; x > 0;) {
    const size_t rem = (x - 1) % 26;
    s.insert(s.begin(), static_cast<char>('A' + rem));
    x = (x - 1) / 26;
  }
  return s;
}
}
namespace piet::handlers {
using namespace data_io_detail;


ojson runExportExcel(const std::vector<std::string>& varlist, const std::string& pathArg,
                     const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                     SessionState& session) {
  return runExportExcel(varlist, pathArg, opts, std::nullopt, std::nullopt, session);
}


ojson runExportExcel(const std::vector<std::string>& varlist, const std::string& pathArg,
                     const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                     const std::optional<std::string>& ifExpr,
                     const std::optional<std::string>& inRange, SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  Datastore* ds = session.datastore();
  const bool replace = hasFlagOpt(opts, "replace");
  const bool noLabel = hasFlagOpt(opts, "nolabel");
  // `firstrow(variables|varlabels)` writes row 1: the names, or each
  // variable's label (its name when it has none). The value takes the
  // abbreviations `var` and `varl` (w2ar dx_exx_first_var `first(var)` rc 0,
  // log:1073; dx_exx_varlabels log:1394; the import-excel fixture's
  // `firstrow(varlabels)` Sheet1, log:1691-1716).
  bool header = false;
  bool headerLabels = false;
  for (const auto& kv : opts) {
    if (kv.first != "firstrow") continue;
    header = true;
    if (!kv.second.isBool) {
      std::string v = strutil::lower(optDequote(kv.second.strValue));
      if (v.size() >= 3 && std::string("variables").compare(0, v.size(), v) == 0) {
        headerLabels = false;
      } else if (v.size() >= 4 && std::string("varlabels").compare(0, v.size(), v) == 0) {
        headerLabels = true;
      } else {
        return engineErrorBlock(
            "export excel: firstrow(" + v + ") — the value is `variables` or `varlabels`",
            "unsupported_option");
      }
    }
  }

  // `sheet("Alpha")` names the worksheet. Stata's full grammar is
  // `sheet("name"[, modify|replace])`, with the sub-option INSIDE the
  // parentheses; `sheetreplace` / `sheetmodify` are the same request spelled as
  // top-level flags (the spelling the corpus actually uses — 020, 038, 091, 108).
  std::string sheetName;
  bool sheetMerge = false;      // the sub-option asks to keep the rest of the workbook
  std::string sheetMergeWord;
  std::optional<std::string> sheetRaw;
  for (const auto& kv : opts) {
    if (kv.first == "sheet" && !kv.second.isBool) sheetRaw = kv.second.strValue;
  }
  if (sheetRaw.has_value()) {
    // The name is verbatim inside its quotes: `sheet("8-2 ")` keeps the
    // trailing blank (w2ar log:1655 `[8-2 ]`).
    auto unquote = [](std::string t) {
      const size_t a = t.find_first_not_of(" \t");
      const size_t b = t.find_last_not_of(" \t");
      t = a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
      if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.substr(1, t.size() - 2);
      return t;
    };
    std::string v = *sheetRaw;
    size_t comma = std::string::npos;
    bool inQ = false;
    for (size_t i = 0; i < v.size(); ++i) {
      if (v[i] == '"') inQ = !inQ;
      else if (v[i] == ',' && !inQ) { comma = i; break; }
    }
    if (comma == std::string::npos) v = unquote(v);
    if (comma != std::string::npos) {
      sheetMergeWord = strutil::lower(trim(v.substr(comma + 1)));
      v = unquote(v.substr(0, comma));
      if (sheetMergeWord != "modify" && sheetMergeWord != "replace") {
        return engineErrorBlock(
            "export excel: sheet(\"" + v + "\", " + sheetMergeWord +
                ") — the sub-option is `modify` or `replace`",
            "unsupported_option");
      }
      sheetMerge = true;
    }
    sheetName = v;
  }
  if (hasFlagOpt(opts, "sheetreplace")) { sheetMerge = true; sheetMergeWord = "sheetreplace"; }
  if (hasFlagOpt(opts, "sheetmodify")) { sheetMerge = true; sheetMergeWord = "sheetmodify"; }
  if (sheetMerge && sheetName.empty()) {
    return engineErrorBlock(
        "export excel: " + sheetMergeWord + " needs the sheet it applies to — add sheet(\"name\")",
        "unsupported_option");
  }

  std::vector<std::string> cols = varlist;
  if (cols.empty()) cols = ds->names();
  for (const auto& v : cols) {
    if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found", "unknown_variable");
  }

  std::string path = withDefaultSuffix(resolveUserPath(session, pathArg), ".xlsx");
  const std::string typed = typedFileName(pathArg, ".xlsx");
  const bool workbookExists = cecon::native_exists(path);
  // the manual rules out combining replace with a per-worksheet modify or
  // replace ([D] import excel) — the two mean opposite things about
  // the rest of the workbook, so accepting both would have to pick one
  // silently.
  if (sheetMerge && replace) {
    return engineErrorBlock(
        "export excel: `replace` overwrites the whole workbook and cannot be combined with " +
            sheetMergeWord,
        "unsupported_option");
  }
  if (workbookExists && !replace && !sheetMerge) {
    return eval::errorBlock("engine_error", "file " + typed + " already exists", std::nullopt,
                            std::nullopt, std::nullopt, "r(602)");
  }
  // A workbook whose directory is missing (StataBE 19.5, BUG-410): `replace`
  // attempts the write, `file nodir/o.xlsx could not be saved` r(603); a sheet()
  // first loads the workbook, `could not be loaded` r(603); with neither,
  // export excel's own existence test answers `already exists` r(602).
  if (parentDirMissing(path)) {
    const bool loads = sheetRaw.has_value() || sheetMerge;
    const std::string what = replace ? " could not be saved"
                             : loads ? " could not be loaded"
                                     : " already exists";
    return eval::errorBlock("engine_error", "file " + typed + what, std::nullopt, std::nullopt,
                            std::nullopt, replace || loads ? "r(603)" : "r(602)");
  }

  // [if] [in] (export excel): the rows come from a
  // filtered copy; names, labels and formats stay the session's.
  std::unique_ptr<Datastore> filtered;
  if ((ifExpr.has_value() && !ifExpr->empty()) || (inRange.has_value() && !inRange->empty())) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, ds->path(), ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, inRange.value_or(std::string()), "export excel");
    filtered = std::make_unique<Datastore>(fr.dataPath);
    ds = filtered.get();
  }

  const int64_t n = ds->nRows();
  std::vector<xlsx::OutColumn> out;
  out.reserve(cols.size());
  for (const auto& name : cols) {
    const cecon::Column& col = ds->col(name);
    xlsx::OutColumn oc;
    oc.name = name;
    if (headerLabels) {
      auto vm = session.varMeta.find(name);
      if (vm != session.varMeta.end() && vm->second.label.has_value() && !vm->second.label->empty())
        oc.name = *vm->second.label;
    }
    // A %td variable is written as Excel date serials (Stata date + 21916)
    // under the m/d/yyyy format, a %#.2f one under 0.00 (w2ar
    // dx_exx_first_var_s1n_v7 int %tdnn/dd/CCYY and _v3 double %14.2f read
    // back, log:1095-1104).
    bool asExcelDate = false;
    if (col.dtype != cecon::DType::Factor) {
      const std::string fmt = session.displayFormat(session.datastore()->varinfo(name));
      std::string f = fmt;
      if (!f.empty() && f[0] == '%') f.erase(0, 1);
      if (!f.empty() && f[0] == '-') f.erase(0, 1);
      if (f.rfind("td", 0) == 0) {
        asExcelDate = true;
        oc.numFmtId = 14;
      } else if (!hasFlagOpt(opts, "keepcellfmt")) {
        // Under keepcellfmt the 0.00 format is not written (w2ar dx_exx_097
        // reads x back %10.0g, log:1375; without it %14.2f, log:1196).
        static const std::regex reTwoDec(R"(^%-?[0-9]*\.2fc?$)");
        if (std::regex_match(fmt, reTwoDec)) oc.numFmtId = 2;
      }
    }
    oc.isNumber.assign(static_cast<size_t>(n), false);
    oc.isBlank.assign(static_cast<size_t>(n), false);
    oc.numbers.assign(static_cast<size_t>(n), 0.0);
    oc.strings.assign(static_cast<size_t>(n), std::string());

    const std::map<std::string, std::string>* labels = nullptr;
    if (!noLabel) {
      auto lv = session.varLblname.find(name);
      if (lv != session.varLblname.end()) {
        auto ld = session.labelDefs.find(lv->second);
        if (ld != session.labelDefs.end()) labels = &ld->second;
      }
    }

    for (int64_t r = 0; r < n; ++r) {
      size_t i = static_cast<size_t>(r);
      if (col.dtype == cecon::DType::Factor) {
        int32_t code = col.codes()[r];
        if (code < 0) { oc.isBlank[i] = true; continue; }
        oc.strings[i] = col.levels[static_cast<size_t>(code)];
      } else {
        double v = col.f64()[r];
        if (std::isnan(v)) {
          // Excel has no missing sentinel; Stata writes the cell EMPTY. The
          // extended code is not representable here — a named limitation, not
          // a silent substitution (the delimited writer does carry it).
          oc.isBlank[i] = true;
          continue;
        }
        if (labels != nullptr) {
          std::string key = eval::pythonFloatRepr(v);
          auto it = labels->find(key);
          if (it == labels->end() && v == std::floor(v))
            it = labels->find(std::to_string(static_cast<long long>(v)));
          if (it != labels->end()) { oc.strings[i] = it->second; continue; }
        }
        oc.isNumber[i] = true;
        oc.numbers[i] = asExcelDate ? v + 21916.0 : v;
      }
    }
    out.push_back(std::move(oc));
  }

  try {
    // A sheetreplace against a workbook that is not there yet is just a write:
    // a sheet name that the workbook does not have yet simply gets created
    // ([D] import excel), and there is no
    // workbook to preserve.
    if (sheetMerge && workbookExists) xlsx::writeSheetInto(path, sheetName, out, header);
    else xlsx::write(path, out, header, sheetName);
  } catch (const std::exception& e) {
    return reEchoPath(engineErrorBlock(std::string("export excel: ") + e.what(), "data_io"), path,
                      typed);
  }
  ojson block;
  block["type"] = "display";
  block["text"] = "file " + typed + " saved";
  return block;
}
}
