// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// spreadsheet import.
#include "data_io_internal.h"
namespace piet::handlers::data_io_detail {


ExcelDateStyle excelDateStyle(int id, const std::string& code) {
  switch (id) {
    case 14: return {false, "%tdnn/dd/CCYY", 14, 10};
    case 15: return {false, "%tddd-Mon-YY", 15, 9};
    case 16: return {false, "%tddd-Mon", 16, 6};
    case 17: return {false, "%tdMon-YY", 17, 6};
    case 18: return {true, "%tchh:MM_AM", 18, 8};
    case 19: return {true, "%tchh:MM:SS_AM", 19, 11};
    case 20: return {true, "%tchH:MM", 20, 5};
    case 21: case 46: return {true, "%tcHH:MM:SS", 21, 8};
    case 22: return {true, "%tcnn/dd/ccYY_hh:MM", 22, 16};
    case 45: case 47: return {true, "%tcMM:SS", 45, 5};
    default: break;
  }
  // DESIGN DECISION (no oracle arm): a locale id (27-36, 50-58) carries no
  // code here and reads as id 14.
  if (code.empty()) return {false, "%tdnn/dd/CCYY", 14, 10};
  // A custom code: its date letters (y, d) and clock letters (h, s) outside
  // quoted literals, escapes and [brackets]; `m` is a month or a minute and
  // decides nothing.
  bool date = false, clock = false, inQ = false, inB = false;
  for (size_t i = 0; i < code.size(); ++i) {
    const char c = code[i];
    if (c == '"') { inQ = !inQ; continue; }
    if (inQ) continue;
    if (c == '[') { inB = true; continue; }
    if (c == ']') { inB = false; continue; }
    if (inB) continue;
    if (c == '\\') { ++i; continue; }
    const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l == 'y' || l == 'd') date = true;
    if (l == 'h' || l == 's') clock = true;
  }
  if (clock && date) return {true, "%tc", 0, 0};
  if (clock) return {true, "%tcHH:MM:SS", 0, 0};
  return {false, "%td", 0, 0};
}


// The Stata milliseconds of an Excel serial. Excel counts a 29feb1900 that
// never was, so a serial before 61 is one day later than the 21916 offset
// gives: serial 0 is 31dec1899, 59.5 noon of 28feb1900, 61.25 is 01mar1900
// 06:00 (dt2.golden). Days, then seconds, then milliseconds, in that order:
// 43593.24929861111 is 1872914339399.999756 in Stata, where days*86400000
// gives ...400; the %td value is this over 86400000 (21677.2492986111065).
double excelSerialToTc(double serial) {
  return ((serial - (serial < 61.0 ? 21915.0 : 21916.0)) * 86400.0) * 1000.0;
}

double excelSerialToTd(double serial) { return excelSerialToTc(serial) / 86400000.0; }


// A date cell read as text. The clock comes from the %tc value, truncated
// like every %tc display: 23:59:59 stored as 44555.99998842592 is 23:59:58.
std::string excelDateText(double serial, const ExcelDateStyle& st) {
  static const char* const kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const int64_t ms = static_cast<int64_t>(std::floor(excelSerialToTc(serial)));
  const int64_t day =
      st.time ? floorDiv(ms, kMsPerDay) : static_cast<int64_t>(std::floor(excelSerialToTd(serial)));
  const int64_t msd = ms - floorDiv(ms, kMsPerDay) * kMsPerDay;
  const int hh = static_cast<int>(msd / 3600000), mi = static_cast<int>(msd / 60000 % 60),
            ss = static_cast<int>(msd / 1000 % 60);
  const int h12 = hh % 12 == 0 ? 12 : hh % 12;
  const char* ampm = hh < 12 ? "AM" : "PM";
  int64_t y;
  unsigned m, d;
  civilFromDays(day - kDaysTdEpochToUnix, &y, &m, &d);
  const long long yy = static_cast<long long>(y), y2 = ((yy % 100) + 100) % 100;
  char buf[64];
  switch (st.id) {
    case 14: std::snprintf(buf, sizeof buf, "%u/%u/%lld", m, d, yy); break;
    case 15: std::snprintf(buf, sizeof buf, "%u-%s-%02lld", d, kMon[m - 1], y2); break;
    case 16: std::snprintf(buf, sizeof buf, "%u-%s", d, kMon[m - 1]); break;
    case 17: std::snprintf(buf, sizeof buf, "%s-%02lld", kMon[m - 1], y2); break;
    case 18: std::snprintf(buf, sizeof buf, "%d:%02d %s", h12, mi, ampm); break;
    case 19: std::snprintf(buf, sizeof buf, "%d:%02d:%02d %s", h12, mi, ss, ampm); break;
    case 20: std::snprintf(buf, sizeof buf, "%d:%02d", hh, mi); break;
    case 21: std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", hh, mi, ss); break;
    case 22: std::snprintf(buf, sizeof buf, "%u/%u/%lld %d:%02d", m, d, yy, h12, mi); break;
    case 45: std::snprintf(buf, sizeof buf, "%02d:%02d", mi, ss); break;
    default:
      if (st.stataFmt == "%td") return tdDayString(day);
      std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", hh, mi, ss);
      return st.stataFmt == "%tc" ? tdDayString(day) + " " + buf : std::string(buf);
  }
  std::string t(buf);
  if (t.size() < st.width) t.insert(0, st.width - t.size(), ' ');
  return t;
}
}
namespace piet::handlers {
using namespace data_io_detail;


ojson runImportExcel(const std::string& pathArg,
                     const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                     SessionState& session) {
  const bool firstrow = hasFlagOpt(opts, "firstrow");
  const bool wantDescribe = hasFlagOpt(opts, "describe");
  const bool allstring = hasFlagOpt(opts, "allstring");

  // case(preserve|lower|upper). Stata's default is preserve ([D] import excel
  // p.495); this build hard-lowercased until W3a, which is case(lower) wired as
  // if it were the only rule.
  // The value takes abbreviations: `case(l)` is lower (w2ar
  // dx_imx_077_case_l_ok rc 0, log:1954).
  NameCase nameCase = NameCase::kPreserve;
  if (auto c = optStrOf(opts, "case")) {
    const std::string v = strutil::lower(*c);
    auto isPrefixOf = [&](const char* full) {
      const std::string f(full);
      return !v.empty() && v.size() <= f.size() && f.compare(0, v.size(), v) == 0;
    };
    if (isPrefixOf("preserve")) nameCase = NameCase::kPreserve;
    else if (isPrefixOf("lower")) nameCase = NameCase::kLower;
    else if (isPrefixOf("upper")) nameCase = NameCase::kUpper;
    else {
      return engineErrorBlock(
          "import excel: case(" + *c + ") is not one of preserve, lower or upper",
          "unsupported_option");
    }
  }

  xlsx::ReadOptions ro;
  ro.firstRowIsHeader = firstrow;
  // The sheet name is taken verbatim inside its quotes: `sheet("8-2 ")` names
  // a sheet whose name ends in a blank (w2ar log:1655).
  for (const auto& kv : opts) {
    if (kv.first != "sheet" || kv.second.isBool) continue;
    std::string v = kv.second.strValue;
    const size_t a = v.find_first_not_of(" \t");
    const size_t b = v.find_last_not_of(" \t");
    v = a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    ro.sheetName = v;
  }
  if (auto cr = optStrOf(opts, "cellrange")) {
    std::string err;
    // A range ending in `:` names no end cell: r(198) (w2ar
    // dx_imx_bad_range_syntax `cellrange(A1:)`, log:2047).
    if (!cr->empty() && cr->back() == ':') {
      return engineErrorBlock("import excel: cellrange(" + *cr + ") invalid", "unsupported_option");
    }
    if (!parseCellRangeOpt(*cr, &ro, &err)) {
      return engineErrorBlock("import excel: " + err, "unsupported_option");
    }
  }
  // `allstring("fmt")` names the display format used to render the numbers.
  // This build renders through `tostring`'s default, so an explicit format
  // would be silently ignored — say so instead.
  if (auto af = optStrOf(opts, "allstring")) {
    if (!af->empty()) {
      return engineErrorBlock(
          "import excel: allstring(" + *af +
              ") is not supported — this build's allstring renders through tostring's default "
              "format (bare `allstring` works)",
          "unsupported_option");
    }
  }

  std::string path = importSourcePath(session, pathArg, true);
  if (!cecon::native_exists(path)) {
    // StataBE: `file sub/nofile.xlsx not found` r(601), the file as typed; a
    // stem found under neither suffix is named bare (`file nofile not found`).
    return eval::errorBlock("engine_error", "file " + stripOneQuotePair(trim(pathArg)) + " not found",
                            std::nullopt, std::nullopt, std::nullopt, "r(601)");
  }

  // ---- `describe`: a pure QUERY. It must not touch the dataset, so it returns
  // before anything below can. Stored results are [D] import excel's own
  // (p.499): r(N_worksheet), r(worksheet_#), r(range_#).
  if (wantDescribe) {
    std::vector<xlsx::SheetInfo> info;
    try {
      info = xlsx::sheets(path);
    } catch (const std::exception& e) {
      return engineErrorBlock(std::string("import excel: ") + e.what(), "data_io");
    }
    session.eval.rValues.set("N_worksheet", static_cast<double>(info.size()));
    for (size_t i = 0; i < info.size(); ++i) {
      const std::string n = std::to_string(i + 1);
      session.eval.rStrings.set("worksheet_" + n, info[i].name);
      session.eval.rStrings.set("range_" + n, info[i].range);
    }
    size_t nameWidth = 5;  // "Sheet"
    // Widths count CHARACTERS (a sheet may be named in Arabic), not bytes.
    for (const auto& si : info) nameWidth = std::max(nameWidth, cecon::ident::codePointCount(si.name));
    std::string text = "\n";
    text += "Sheet" + std::string(nameWidth - 5 + 3, ' ') + "Range\n";
    text += std::string(nameWidth + 3 + 20, '-') + "\n";
    for (const auto& si : info) {
      text += si.name + std::string(nameWidth - cecon::ident::codePointCount(si.name) + 3, ' ') +
              si.range + "\n";
    }
    ojson block;
    block["type"] = "display";
    block["text"] = text;
    return block;
  }

  xlsx::Sheet sheet;
  try {
    sheet = xlsx::read(path, ro);
  } catch (const xlsx::SheetNotFound& e) {
    // `clear` drops the data before the sheet is looked up, so a missing
    // sheet leaves k=0 (w2ar dx_imx_bad_sheet 601 k=0, log:2028).
    if (hasFlagOpt(opts, "clear") && session.hasData()) (void)runClear("", session);
    // Stata's own code and message, both captured (aer_b4
    // pins_oracle_g.log:136-139: `worksheet NoSuchSheet not found`, r(601)).
    // The rc travels as an explicit `reason`, because coarseRc's file/directory
    // rule would otherwise never see a message that says "worksheet".
    return eval::errorBlock("engine_error", e.what(), std::nullopt, std::nullopt, std::nullopt,
                            "r(601)");
  } catch (const xlsx::CellRangeOutsideData& e) {
    // B4 P2: Stata's own two messages, verbatim and unprefixed, under
    // `unsupported_option` -> r(198) (oracle_q.log:74-79 column, :84-89 row).
    // W3a clamped instead, and said in its own comment that it had no capture
    // to settle it against; oracle_q is that capture. Thrown BEFORE any data is
    // installed, which is also what Stata does — it leaves c(k)==0 and _N==0
    // (:81-82, :91-92).
    return engineErrorBlock(e.what(), "unsupported_option");
  } catch (const std::exception& e) {
    return engineErrorBlock(std::string("import excel: ") + e.what(), "data_io");
  }

  size_t nCols = sheet.header.size();
  for (const auto& r : sheet.rows) nCols = std::max(nCols, r.size());
  if (nCols == 0) return engineErrorBlock("import excel: " + path + " has no cells to read");

  // BUG-425: without `firstrow`, Stata names the variables after the SHEET's own
  // column letters — A, B, ... Z, AA, AB — the ORIGINAL spreadsheet's, not the
  // window's (`cellrange(C3:D9)` gives C and D).
  //
  // With `firstrow`, the header cell is legalized with the case kept (`Value X`
  // -> ValueX, `2nd col` -> ndcol) and becomes EVERY column's label, [id]
  // included; a blank header cell gives the column letter and no label (w2ar
  // log:1702-1716, :1784). case(lower|upper) folds the names, and two that
  // fold together are r(110) before any data moves (log:1727, :1914).
  const size_t colBase = sheet.firstCol > 0 ? static_cast<size_t>(sheet.firstCol) - 1 : 0;
  std::vector<std::string> names;
  std::vector<std::string> labels;
  {
    std::set<std::string> taken;
    std::set<std::string> seenHeaders;  // legal header names, case kept, in column order
    const delimited::Case dc = nameCase == NameCase::kLower   ? delimited::Case::kLower
                               : nameCase == NameCase::kUpper ? delimited::Case::kUpper
                                                              : delimited::Case::kPreserve;
    for (size_t c = 0; c < nCols; ++c) {
      std::string raw = (firstrow && c < sheet.header.size()) ? sheet.header[c] : std::string();
      std::string nm;
      std::string label;
      if (firstrow && !raw.empty()) {
        // excel cuts the RAW header to 32 characters BEFORE it drops what a name
        // cannot hold, so `Have you heard about this program?` becomes
        // `Haveyouheardaboutthisprogra` (27 letters: the cut lands on "progra"),
        // where import delimited cleans first and keeps 28. StataBE probe sp.do
        // (scratch-r150/uni-oracle); 007's Wave 1 cleaning reads the 27-letter name.
        const std::string cut = cecon::ident::truncateCodePoints(raw, 32);
        nm = delimited::legalNames({cut}, dc).names.front();
        // legalNames' positional fallback means nothing legal was left in the
        // cell; excel's fallback is the column letter instead.
        if ((nm == "v1" || nm == "V1") && strutil::lower(raw) != "v1") nm.clear();
        label = raw;
        // A header whose legal name (case kept) repeats an EARLIER column's gets
        // the column letter too, not an error: StataBE names the second `source`
        // of 007's baseline sheet `L`, and two long headers that truncate to the
        // same 32 characters `AZ` and `BD` (T7 probe ie2.do, scratch-r150/t7-int).
        // The r(110) below stays for names that only COLLIDE once case() folds
        // them, which the w2ar oracle pins.
        if (!nm.empty()) {
          std::string kept = delimited::legalNames({cut}, delimited::Case::kPreserve).names.front();
          if (!seenHeaders.insert(kept).second) nm.clear();
        }
      }
      if (nm.empty()) {
        // The letter goes in VERBATIM: Stata's generated names are upper case
        // and case() has no say over them.
        nm = excelColumnName(colBase + c);
      }
      if (taken.count(nm)) {
        return eval::errorBlock("engine_error", "variable " + nm + " already defined",
                                std::nullopt, std::nullopt, std::nullopt, "r(110)");
      }
      taken.insert(nm);
      names.push_back(nm);
      labels.push_back(label);
    }
  }

  // Column types from the cells themselves (C16): a column holding any text
  // cell is a string, its numbers written as text; a numeric column is
  // byte/int/long when every value is an integer in range, double otherwise --
  // never float -- shown %10.0g; a column of date-formatted numbers is int
  // the Stata date or datetime its number format names (excelDateStyle;
  // integer serials under id 14 are int %tdnn/dd/CCYY, log:1624-1639); a
  // 0.00-formatted one shows %14.2f (log:1091-1104). `allstring` makes every
  // column a string: numbers at 16 significant digits, date cells drawn by
  // excelDateText (m/d/yyyy right-aligned in 10 under id 14, log:1607-1622).
  auto numText = [](double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.16g", v);
    std::string t(buf);
    if (t.rfind("0.", 0) == 0) t.erase(0, 1);
    else if (t.rfind("-0.", 0) == 0) t.erase(1, 1);
    return t;
  };
  delimited::Table table;
  table.names = names;
  table.labels = labels;
  table.types.resize(nCols);
  std::vector<std::string> formats(nCols);
  std::vector<std::vector<std::string>> cells(sheet.rows.size(), std::vector<std::string>(nCols));
  for (size_t c = 0; c < nCols; ++c) {
    bool anyText = allstring, anyNum = false, allInt = true, allDate = true, all2dec = true;
    double lo = 0.0, hi = 0.0;
    for (const auto& row : sheet.rows) {
      if (c >= row.size() || row[c].blank) continue;
      const xlsx::Cell& cell = row[c];
      if (!cell.isNumber) {
        anyText = true;
        continue;
      }
      const double v = cell.number;
      if (!anyNum) { lo = hi = v; anyNum = true; }
      lo = std::min(lo, v);
      hi = std::max(hi, v);
      if (v != std::floor(v)) allInt = false;
      if (!cell.isDate) allDate = false;
      if (cell.numFmtId != 2) all2dec = false;
    }
    if (anyText) {
      size_t width = 0;
      for (size_t r = 0; r < sheet.rows.size(); ++r) {
        const auto& row = sheet.rows[r];
        std::string t;
        if (c < row.size() && !row[c].blank) {
          const xlsx::Cell& cell = row[c];
          t = !cell.isNumber ? cell.text
              : cell.isDate  ? excelDateText(cell.number, excelDateStyle(cell.numFmtId, cell.numFmtCode))
                             : numText(cell.number);
        }
        width = std::max(width, t.size());
        cells[r][c] = std::move(t);
      }
      table.types[c].isString = true;
      table.types[c].stataType =
          width > 2045 ? std::string("strL") : "str" + std::to_string(std::max<size_t>(1, width));
      continue;
    }
    std::string type = "byte";
    std::string fmt = "%10.0g";
    if (anyNum && allDate) {
      // One format for the column: its cells' when they agree, else plain %tc
      // when any cell has a clock (dt3.golden m1422/m2214: id 14 and id 22
      // cells -> %tc) and %td otherwise. The value keeps its fraction, so a
      // column is int/long only when every value is whole (log:1624-1639).
      bool first = true, same = true, time = false;
      std::string fmt0;
      for (const auto& row : sheet.rows) {
        if (c >= row.size() || row[c].blank || !row[c].isNumber) continue;
        const ExcelDateStyle st = excelDateStyle(row[c].numFmtId, row[c].numFmtCode);
        if (first) fmt0 = st.stataFmt;
        else if (st.stataFmt != fmt0) same = false;
        first = false;
        time = time || st.time;
      }
      fmt = same ? fmt0 : (time ? "%tc" : "%td");
      bool whole = true, seen = false;
      double vlo = 0.0, vhi = 0.0;
      for (size_t r = 0; r < sheet.rows.size(); ++r) {
        const auto& row = sheet.rows[r];
        if (c < row.size() && !row[c].blank && row[c].isNumber) {
          const double v = time ? excelSerialToTc(row[c].number) : excelSerialToTd(row[c].number);
          if (v != std::floor(v)) whole = false;
          vlo = seen ? std::min(vlo, v) : v;
          vhi = seen ? std::max(vhi, v) : v;
          seen = true;
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%.17g", v);
          cells[r][c] = buf;
        }
      }
      if (whole && vlo >= -32767 && vhi <= 32740) type = "int";
      else if (whole && vlo >= -2147483647.0 && vhi <= 2147483620.0) type = "long";
      else type = "double";
    } else {
      if (anyNum) {
        if (allInt && lo >= -127 && hi <= 100) type = "byte";
        else if (allInt && lo >= -32767 && hi <= 32740) type = "int";
        else if (allInt && lo >= -2147483647.0 && hi <= 2147483620.0) type = "long";
        else type = "double";
        if (all2dec) fmt = "%14.2f";
      }
      for (size_t r = 0; r < sheet.rows.size(); ++r) {
        const auto& row = sheet.rows[r];
        if (c < row.size() && !row[c].blank && row[c].isNumber) {
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%.17g", row[c].number);
          cells[r][c] = buf;
        }
      }
    }
    table.types[c].isString = false;
    table.types[c].stataType = type;
    formats[c] = fmt;
  }
  table.rows = std::move(cells);
  if (auto err = delimited::stageAndAdopt(session, table, "import excel", path)) return *err;
  for (size_t c = 0; c < nCols; ++c) {
    if (!formats[c].empty()) session.varMeta[table.names[c]].format = formats[c];
  }
  session.invalidateDatastore();
  return openBlockFor(session, path);
}
}
