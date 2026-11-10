// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: spreadsheets.
#include "selftest_support.h"
#include "selftest_cases.h"


// P1 wave: the .xlsx layer (piet/native/src/xlsx.{h,cpp}). The decisive leg is
// the REAL vector — a workbook written by Stata itself, whose parts are all
// DEFLATE-compressed, so it can only be read if the inflate implementation is
// correct. A self-roundtrip alone would prove nothing about that, because our
// own writer emits STORED entries.
void testP1XlsxLayer() {
  std::printf("[selftest] --- P1 xlsx: self-roundtrip, real Stata vector, honest rejects ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_xlsx").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // --- ① self-roundtrip: values and blanks survive a write/read cycle.
  {
    std::string path = workdir + "/rt.xlsx";
    std::vector<piet::xlsx::OutColumn> cols(2);
    cols[0].name = "num";
    cols[0].isNumber = {true, true, false};
    cols[0].isBlank = {false, false, true};
    cols[0].numbers = {1.5, -2.0, 0.0};
    cols[0].strings = {"", "", ""};
    cols[1].name = "txt";
    cols[1].isNumber = {false, false, false};
    cols[1].isBlank = {false, false, false};
    cols[1].numbers = {0, 0, 0};
    cols[1].strings = {"a,b", "quote\"inside", "<xml>&amp;"};
    bool wrote = true;
    try { piet::xlsx::write(path, cols, /*writeHeader=*/true); }
    catch (const std::exception&) { wrote = false; }
    check(wrote, "P1 xlsx ①: writer produced a workbook");

    if (wrote) {
      piet::xlsx::Sheet s = piet::xlsx::read(path, /*firstRowIsHeader=*/true);
      check(s.header.size() == 2 && s.header[0] == "num" && s.header[1] == "txt",
            "P1 xlsx ①: header round-trips");
      check(s.rows.size() == 3, "P1 xlsx ①: 3 data rows round-trip, got " +
                                    std::to_string(s.rows.size()));
      if (s.rows.size() == 3) {
        check(s.rows[0][0].isNumber && s.rows[0][0].number == 1.5,
              "P1 xlsx ①: a number round-trips exactly");
        check(s.rows[2][0].blank, "P1 xlsx ①: a blank cell stays blank");
        check(s.rows[1][1].text == "quote\"inside",
              "P1 xlsx ①: an embedded quote round-trips (XML escaping)");
        check(s.rows[2][1].text == "<xml>&amp;",
              "P1 xlsx ①: markup-looking text round-trips verbatim");
      }
    }
  }

  // --- ② REAL vector: Stata's own p8_results.xlsx (every part DEFLATEd).
  //        This is the leg that actually exercises inflate.
  {
    // The real workbook is a GENERATED artifact under the repo's waves dir (it
    // is gitignored bulk, so a clean checkout legitimately lacks it). Walk up
    // from the running binary to find the repo, and SKIP honestly when absent.
    std::string real;
    {
      std::optional<std::string> exe = piet::executablePath();
      fs::path dir = exe.has_value() ? fs::path(*exe).parent_path() : fs::current_path();
      for (int up = 0; up < 8 && !dir.empty(); ++up) {
        fs::path cand = dir / "tests" / "kitchensink" / "pangram" / "waves" / "_runs" / "p8_results.xlsx";
        if (fs::exists(cand)) { real = cand.u8string(); break; }
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
      }
    }
    if (real.empty()) {
      skip("P1 xlsx ②: real Stata workbook", "generated artifact absent from this tree");
    } else {
      bool ok = true;
      size_t nrows = 0, ncols = 0;
      try {
        piet::xlsx::Sheet s = piet::xlsx::read(real, /*firstRowIsHeader=*/true);
        nrows = s.rows.size();
        ncols = s.header.size();
      } catch (const std::exception&) { ok = false; }
      check(ok, "P1 xlsx ②: a Stata-written (DEFLATE) workbook decompresses — inflate works");
      check(nrows == 4800, "P1 xlsx ②: real workbook yields its 4800 data rows, got " +
                               std::to_string(nrows));
      check(ncols > 0, "P1 xlsx ②: real workbook yields a header row");
    }
  }

  // --- ③ honest rejects: a non-ZIP and a truncated file must say so, not crash.
  {
    std::string bogus = workdir + "/not_a_workbook.xlsx";
    { std::ofstream f(bogus, std::ios::binary); f << "this is plainly not a zip archive"; }
    bool threw = false;
    try { (void)piet::xlsx::read(bogus, true); } catch (const std::exception&) { threw = true; }
    check(threw, "P1 xlsx ③: a non-ZIP file is an honest error, not a crash");
  }

  fs::remove_all(workdir, ec);
}


// AER scope-2 W3a: `import excel` / `export excel` / `import delimited`'s option
// cluster -- the highest-frequency one in the twelve-paper corpus (`import
// excel` appears 273 times, 243 of them in paper 015).
//
// The decisive leg is ⑤, and it is the reason this function exists at all. Until
// W3a the reader located "the worksheet" by trying xl/worksheets/sheet1.xml and
// then any xl/worksheets/*.xml, and NEVER opened xl/workbook.xml. Sheet order on
// disk is not sheet order in the workbook and the part filename carries no
// identity, so that reader returns a DIFFERENT SHEET at rc 0 with plausible
// numbers in it. Leg ⑤ builds exactly the package that exposes it -- one sheet,
// whose relationship Target is the package-absolute `/xl/worksheets/sheet2.xml`,
// with a decoy sheet1.xml also present -- and asserts the value that only a
// reader walking name -> r:id -> Target -> part can return.
void testW3aSpreadsheetOptions() {
  std::printf("[selftest] --- AER scope-2 W3a: import/export excel options + import delimited "
              "row/col windows ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w3a").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  auto at = [&](const std::string& rel) { return (fs::path(workdir) / rel).u8string(); };

  // A numeric column and a string column, in the OutColumn shape the writer takes.
  auto numCol = [](const std::string& name, std::vector<double> v) {
    piet::xlsx::OutColumn c;
    c.name = name;
    c.isNumber.assign(v.size(), true);
    c.isBlank.assign(v.size(), false);
    c.numbers = std::move(v);
    c.strings.assign(c.numbers.size(), std::string());
    return c;
  };

  const std::string book = at("w3a.xlsx");
  std::vector<piet::xlsx::OutColumn> alpha = {numCol("a1", {1, 2, 3}), numCol("a2", {4, 5, 6})};
  std::vector<piet::xlsx::OutColumn> beta = {numCol("x", {10, 20, 30}), numCol("y", {40, 50, 60}),
                                             numCol("z", {70, 80, 90})};
  bool built = true;
  try {
    piet::xlsx::write(book, alpha, /*writeHeader=*/true, "Alpha");
    piet::xlsx::writeSheetInto(book, "Beta", beta, /*writeHeader=*/true);
  } catch (const std::exception&) {
    built = false;
  }
  check(built, "W3a ①: `export excel, sheet(Alpha)` then `sheet(Beta) sheetreplace` builds a "
               "TWO-sheet workbook. This is the merge path -- read every part, insert the new "
               "worksheet, patch workbook.xml + its rels + [Content_Types].xml, repackage -- and a "
               "failure here means the workbook a user asked to add a sheet to was not rewritten.");

  if (built) {
    // ── ② the workbook names its sheets, and `describe` reads them from there.
    std::vector<piet::xlsx::SheetInfo> info;
    try { info = piet::xlsx::sheets(book); } catch (const std::exception&) {}
    check(info.size() == 2, "W3a ②: the merged workbook declares 2 worksheets, got " +
                                std::to_string(info.size()));
    if (info.size() == 2) {
      check(info[0].name == "Alpha" && info[1].name == "Beta",
            "W3a ②: sheets come back in WORKBOOK order with their declared names, got {" +
                info[0].name + ", " + info[1].name + "}");
      check(info[0].range == "A1:B4" && info[1].range == "A1:C4",
            "W3a ②: each sheet reports its own used range (r(range_#)), got {" + info[0].range +
                ", " + info[1].range + "}");
    }

    // ── ③ sheet("Beta") returns BETA. Alpha is 2 columns of 1..6, Beta is 3 of
    //      10..90, so a reader that picked the wrong part cannot pass this.
    {
      piet::xlsx::ReadOptions ro;
      ro.firstRowIsHeader = true;
      ro.sheetName = "Beta";
      piet::xlsx::Sheet s;
      bool ok = true;
      try { s = piet::xlsx::read(book, ro); } catch (const std::exception&) { ok = false; }
      check(ok && s.header.size() == 3, "W3a ③: sheet(\"Beta\") reads Beta's THREE columns, got " +
                                            std::to_string(s.header.size()));
      const double first = (!s.rows.empty() && !s.rows[0].empty() && s.rows[0][0].isNumber)
                               ? s.rows[0][0].number
                               : std::nan("");
      check(first == 10.0,
            "W3a ③: sheet(\"Beta\")'s first cell is Beta's 10, not Alpha's 1. A reader that guesses "
            "the part by filename answers 1 here -- at rc 0, with plausible data.");
      check(s.firstCol == 1, "W3a ③: with no cellrange the window starts at spreadsheet column A");
    }

    // ── ④ a MISSING sheet is Stata's r(601), with Stata's own sentence
    //      (aer_b4 pins_oracle_g.log:136-139).
    {
      piet::xlsx::ReadOptions ro;
      ro.sheetName = "NoSuchSheet";
      std::string got = "<no throw>";
      bool rightType = false;
      try {
        (void)piet::xlsx::read(book, ro);
      } catch (const piet::xlsx::SheetNotFound& e) {
        rightType = true;
        got = e.what();
      } catch (const std::exception& e) {
        got = e.what();
      }
      check(rightType && got == "worksheet NoSuchSheet not found",
            "W3a ④: a sheet name the workbook does not have throws SheetNotFound carrying Stata's "
            "captured message `worksheet NoSuchSheet not found`; got `" + got + "`");
    }

    // ── cellrange, closed and open-ended.
    {
      piet::xlsx::ReadOptions ro;
      ro.firstRowIsHeader = true;
      ro.sheetName = "Beta";
      ro.firstCol = 1; ro.firstRow = 1; ro.lastCol = 2; ro.lastRow = 4;
      piet::xlsx::Sheet s;
      bool ok = true;
      try { s = piet::xlsx::read(book, ro); } catch (const std::exception&) { ok = false; }
      check(ok && s.header.size() == 2 && s.rows.size() == 3,
            "W3a ⑤: a CLOSED cellrange(A1:B4) over Beta yields 2 vars x 3 obs, got " +
                std::to_string(s.header.size()) + " x " + std::to_string(s.rows.size()));
    }
    {
      // cellrange(B2): open on both ends of the far corner. The window starts at
      // spreadsheet column B, and firstCol is what keeps `import excel`'s default
      // names Stata's (`B`, `C`) instead of renaming the window's head to `A`.
      piet::xlsx::ReadOptions ro;
      ro.sheetName = "Beta";
      ro.firstCol = 2; ro.firstRow = 2;
      piet::xlsx::Sheet s;
      bool ok = true;
      try { s = piet::xlsx::read(book, ro); } catch (const std::exception&) { ok = false; }
      const double first = (ok && !s.rows.empty() && !s.rows[0].empty() && s.rows[0][0].isNumber)
                               ? s.rows[0][0].number
                               : std::nan("");
      check(ok && s.rows.size() == 3 && s.firstCol == 2 && first == 40.0,
            "W3a ⑤: an OPEN-ENDED cellrange(B2) yields the 3 data rows from column B on, and "
            "remembers that the window's first column IS spreadsheet column B (firstCol=" +
                std::to_string(s.firstCol) + ", first cell " +
                (std::isnan(first) ? std::string("<none>") : std::to_string(first)) + ")");
    }

    // ── B4 P2: a cellrange PAST the data area REFUSES, and the two axes say
    //    different things. W3a clamped and named it a limitation "not a measured
    //    Stata match"; oracle_q.log is the measurement, and it says r(198) with
    //    two distinct sentences (:74-79 column, :84-89 row). Beta is A1:C4, so
    //    Z is past its last column and 200 past its last row.
    //
    //    Both sentences are compared IN FULL, second line included, because the
    //    second line is the entire difference between them — an assertion on the
    //    first line alone would pass with the axes swapped.
    {
      struct Axis {
        int lastCol;
        int lastRow;
        bool wantColumn;
        std::string want;
        const char* range;
        const char* logLine;
      };
      const std::vector<Axis> kAxes = {
          {26, 4, true,
           "invalid column range in option cellrange();\n"
           "    The lower right column is right of the data area in the worksheet.",
           "A1:Z4", "oracle_q.log:74-79"},
          {3, 200, false,
           "invalid row range in option cellrange();\n"
           "    The lower right row is after the data area in the worksheet.",
           "A1:C200", "oracle_q.log:84-89"},
      };
      for (const auto& ax : kAxes) {
        piet::xlsx::ReadOptions ro;
        ro.firstRowIsHeader = true;
        ro.sheetName = "Beta";
        ro.firstCol = 1;
        ro.firstRow = 1;
        ro.lastCol = ax.lastCol;
        ro.lastRow = ax.lastRow;
        std::string got = "<no throw>";
        bool rightType = false;
        try {
          (void)piet::xlsx::read(book, ro);
        } catch (const piet::xlsx::CellRangeOutsideData& e) {
          rightType = e.isColumn == ax.wantColumn;
          got = e.what();
        } catch (const std::exception& e) {
          got = e.what();
        }
        check(rightType && got == ax.want,
              std::string("W3a/B4 P2: cellrange(") + ax.range +
                  ") past the data area throws CellRangeOutsideData on the right AXIS carrying "
                  "StataBE's own two-line sentence (" + ax.logLine +
                  ") -- before this it CLAMPED and returned data; got `" + got + "`");
      }
      // The user-visible surface: rc, not just the exception. r(198), which is
      // what `unsupported_option` maps to.
      {
        piet::SessionState session(
            (std::filesystem::temp_directory_path() / "piet_native_selftest_b4p2").u8string());
        std::string rc = "0";
        std::string msg;
        for (const auto& b : piet::dispatch(
                 "import excel using \"" + book + "\", sheet(\"Beta\") cellrange(A1:Z4) firstrow "
                 "clear",
                 session)) {
          if (!b.is_object() || b.value("type", std::string()) != "error") continue;
          rc = piet::eval::coarseRc(b);
          msg = b.value("message", std::string());
        }
        check(rc == "198",
              "W3a/B4 P2: ...and `import excel ..., cellrange(A1:Z4)` answers r(198) as StataBE "
              "does, not rc 0 with a clamped window; got " + rc);
        check(msg.find("invalid column range") != std::string::npos,
              "W3a/B4 P2: ...with Stata's wording reaching the user unprefixed; got " + msg);
      }
      // Non-regression: a range that ENDS EXACTLY on the data area is inside it.
      // An off-by-one in the bound would refuse a legal `cellrange(A1:C4)`.
      {
        piet::xlsx::ReadOptions ro;
        ro.firstRowIsHeader = true;
        ro.sheetName = "Beta";
        ro.firstCol = 1;
        ro.firstRow = 1;
        ro.lastCol = 3;
        ro.lastRow = 4;
        piet::xlsx::Sheet s;
        bool ok = true;
        try { s = piet::xlsx::read(book, ro); } catch (const std::exception&) { ok = false; }
        check(ok && s.header.size() == 3 && s.rows.size() == 3,
              "W3a/B4 P2: cellrange(A1:C4) — exactly the data area — still READS; the bound is "
              "`past`, not `at`, and an off-by-one here would refuse a legal range");
      }
    }
  }

  // ── ⑥ THE SEEDED-RED. A package whose ONLY declared sheet resolves, through
  //      the package-ABSOLUTE relationship Target `/xl/worksheets/sheet2.xml`,
  //      to sheet2 -- while a decoy sheet1.xml sits in the same package. The
  //      pre-W3a reader takes sheet1.xml and returns 111 at rc 0; only a reader
  //      that walks name -> r:id -> Target -> part returns 222.
  {
    // A minimal STORED .xlsx, written here rather than through xlsx::write
    // because the whole point is a relationship shape this writer never emits.
    auto crc32of = [](const std::string& data) {
      static uint32_t table[256];
      static bool built2 = false;
      if (!built2) {
        for (uint32_t i = 0; i < 256; ++i) {
          uint32_t c = i;
          for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
          table[i] = c;
        }
        built2 = true;
      }
      uint32_t c = 0xFFFFFFFFu;
      for (unsigned char ch : data) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
      return c ^ 0xFFFFFFFFu;
    };
    auto le16 = [](std::string& b, uint16_t v) {
      b.push_back(static_cast<char>(v & 0xFF));
      b.push_back(static_cast<char>((v >> 8) & 0xFF));
    };
    auto le32 = [](std::string& b, uint32_t v) {
      for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    };
    auto storedZip = [&](const std::vector<std::pair<std::string, std::string>>& entries) {
      std::string out;
      std::vector<uint32_t> offsets;
      std::vector<uint32_t> crcs;
      for (const auto& e : entries) {
        offsets.push_back(static_cast<uint32_t>(out.size()));
        const uint32_t crc = crc32of(e.second);
        crcs.push_back(crc);
        out += "PK\x03\x04";
        le16(out, 20); le16(out, 0); le16(out, 0); le16(out, 0); le16(out, 0);
        le32(out, crc);
        le32(out, static_cast<uint32_t>(e.second.size()));
        le32(out, static_cast<uint32_t>(e.second.size()));
        le16(out, static_cast<uint16_t>(e.first.size())); le16(out, 0);
        out += e.first;
        out += e.second;
      }
      const uint32_t cdOff = static_cast<uint32_t>(out.size());
      for (size_t i = 0; i < entries.size(); ++i) {
        out += "PK\x01\x02";
        le16(out, 20); le16(out, 20); le16(out, 0); le16(out, 0); le16(out, 0); le16(out, 0);
        le32(out, crcs[i]);
        le32(out, static_cast<uint32_t>(entries[i].second.size()));
        le32(out, static_cast<uint32_t>(entries[i].second.size()));
        le16(out, static_cast<uint16_t>(entries[i].first.size()));
        le16(out, 0); le16(out, 0); le16(out, 0); le16(out, 0);
        le32(out, 0);
        le32(out, offsets[i]);
        out += entries[i].first;
      }
      const uint32_t cdSize = static_cast<uint32_t>(out.size()) - cdOff;
      out += "PK\x05\x06";
      le16(out, 0); le16(out, 0);
      le16(out, static_cast<uint16_t>(entries.size()));
      le16(out, static_cast<uint16_t>(entries.size()));
      le32(out, cdSize); le32(out, cdOff); le16(out, 0);
      return out;
    };
    auto oneCellSheet = [](const char* v) {
      return std::string(
                 "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                 "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
                 "<sheetData><row r=\"1\"><c r=\"A1\"><v>") +
             v + "</v></c></row></sheetData></worksheet>";
    };
    const std::vector<std::pair<std::string, std::string>> parts = {
        {"[Content_Types].xml",
         "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
         "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
         "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package."
         "relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/>"
         "</Types>"},
        {"_rels/.rels",
         "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
         "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
         "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
         "relationships/officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>"},
        {"xl/workbook.xml",
         "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
         "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
         "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
         "<sheets><sheet name=\"Only\" sheetId=\"7\" r:id=\"rId9\"/></sheets></workbook>"},
        {"xl/_rels/workbook.xml.rels",
         "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
         "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
         "<Relationship Id=\"rId9\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
         "relationships/worksheet\" Target=\"/xl/worksheets/sheet2.xml\"/></Relationships>"},
        // The decoy. Alphabetically and conventionally first; not the sheet the
        // workbook points at.
        {"xl/worksheets/sheet1.xml", oneCellSheet("111")},
        {"xl/worksheets/sheet2.xml", oneCellSheet("222")},
    };
    const std::string relsBook = at("rels_target.xlsx");
    {
      std::ofstream f(cecon::to_native_path(relsBook), std::ios::binary | std::ios::trunc);
      const std::string bytes = storedZip(parts);
      f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    double got = std::nan("");
    std::string byName = "<none>";
    try {
      piet::xlsx::Sheet s = piet::xlsx::read(relsBook, /*firstRowIsHeader=*/false);
      if (!s.rows.empty() && !s.rows[0].empty() && s.rows[0][0].isNumber) got = s.rows[0][0].number;
      const std::vector<piet::xlsx::SheetInfo> info = piet::xlsx::sheets(relsBook);
      if (info.size() == 1) byName = info[0].name + " -> " + info[0].part;
    } catch (const std::exception& e) {
      byName = std::string("threw: ") + e.what();
    }
    check(got == 222.0,
          "W3a ⑥ (seeded-RED): the workbook's ONLY sheet resolves through the package-absolute "
          "relationship Target `/xl/worksheets/sheet2.xml`, and a decoy sheet1.xml is present. The "
          "cell must read 222. A reader that ignores xl/_rels/workbook.xml.rels reads the decoy and "
          "answers 111 -- rc 0, plausible data, WRONG SHEET. Got " +
              (std::isnan(got) ? std::string("<none>") : std::to_string(got)));
    check(byName == "Only -> xl/worksheets/sheet2.xml",
          "W3a ⑥: and the sheet's declared NAME comes from workbook.xml, mapped to the part the "
          "rels Target names; got `" + byName + "`");
  }

  // ── ⑦ the Stata surface: describe leaves the data alone, case()/allstring
  //      reshape the load, a missing sheet is rc 601, and `import delimited`'s
  //      row/col window reproduces [D] import delimited's own example.
  {
    auto exec = [](const std::string& src, piet::SessionState& session) {
      return piet::doexec::executeDoSource(src, session, piet::dispatch);
    };
    auto varNames = [](piet::SessionState& s) {
      std::vector<std::string> out;
      const nlohmann::ordered_json blocks = piet::dispatch("describe", s);
      for (const auto& b : blocks)
        if (b.is_object() && b.value("type", std::string()) == "describe" && b.contains("variables"))
          for (const auto& v : b["variables"]) out.push_back(v.value("name", std::string()));
      return out;
    };
    auto dtypes = [](piet::SessionState& s) {
      std::vector<std::string> out;
      const nlohmann::ordered_json blocks = piet::dispatch("describe", s);
      for (const auto& b : blocks)
        if (b.is_object() && b.value("type", std::string()) == "describe" && b.contains("variables"))
          for (const auto& v : b["variables"]) out.push_back(v.value("dtype", std::string()));
      return out;
    };
    auto join = [](const std::vector<std::string>& v) {
      std::string s;
      for (const auto& n : v) { if (!s.empty()) s += " "; s += n; }
      return s.empty() ? std::string("<none>") : s;
    };
    auto firstError = [](const nlohmann::ordered_json& blocks) -> nlohmann::ordered_json {
      for (const auto& b : blocks)
        if (b.is_object() && b.value("type", std::string()) == "error") return b;
      return nlohmann::ordered_json();
    };

    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp).

    // `describe` is a QUERY: it publishes r() and must not disturb the dataset.
    auto r0 = exec("sysuse fatalities, clear\nkeep in 1/5\n", s);
    (void)r0;
    const std::vector<std::string> before = varNames(s);
    auto rd = exec("import excel using \"w3a.xlsx\", describe\n", s);
    check(firstError(rd.blocks).is_null(),
          "W3a ⑦: `import excel using \"w3a.xlsx\", describe` runs clean. Got `" +
              (firstError(rd.blocks).is_object()
                   ? firstError(rd.blocks).value("message", std::string())
                   : std::string("<none>")) + "`");
    const double* nws = s.eval.rValues.find("N_worksheet");
    check(nws != nullptr && *nws == 2.0,
          "W3a ⑦: describe stores r(N_worksheet) == 2 ([D] import excel), got " +
              (nws ? std::to_string(*nws) : std::string("<unset>")));
    check(varNames(s) == before,
          "W3a ⑦: and it does NOT touch the dataset -- `describe` is a query over the FILE. The "
          "loaded variables were {" + join(before) + "}, they are now {" + join(varNames(s)) + "}");

    auto ru = exec("import excel using \"w3a.xlsx\", sheet(\"Beta\") firstrow case(upper) clear\n",
                   s);
    check(firstError(ru.blocks).is_null() && join(varNames(s)) == "X Y Z",
          "W3a ⑦: case(upper) upper-cases the names `firstrow` read, got {" + join(varNames(s)) +
              "}");

    auto ras = exec("import excel using \"w3a.xlsx\", sheet(\"Beta\") firstrow allstring clear\n",
                    s);
    const std::vector<std::string> dt = dtypes(s);
    bool allStr = !dt.empty();
    for (const auto& d : dt) if (d.rfind("str", 0) != 0) allStr = false;
    check(firstError(ras.blocks).is_null() && allStr,
          "W3a ⑦: allstring makes EVERY column a string column, got {" + join(dt) + "}");

    // The captured rc. `sheet("NoSuchSheet")` is r(601) in Stata, with the
    // message `worksheet NoSuchSheet not found` (aer_b4 pins_oracle_g.log).
    auto rmiss =
        exec("import excel using \"w3a.xlsx\", sheet(\"NoSuchSheet\") firstrow clear\n", s);
    const nlohmann::ordered_json e = firstError(rmiss.blocks);
    check(e.is_object() && e.value("message", std::string()) == "worksheet NoSuchSheet not found" &&
              piet::eval::coarseRc(e) == "601",
          "W3a ⑦: `sheet(\"NoSuchSheet\")` answers Stata's captured r(601) and its captured "
          "sentence. Got rc " + (e.is_object() ? piet::eval::coarseRc(e) : std::string("<none>")) +
              " / `" + (e.is_object() ? e.value("message", std::string()) : std::string("<none>")) +
              "`");

    // Follows the SHAPE of the import delimited help file's rowrange/colrange
    // example, in our own invented data: rowrange(2:4) colrange(1:4) over a
    // five-column csv gives 4 vars and 3 obs, and the varnames still come from
    // physical row 1 even though the range starts at 2.
    {
      std::ofstream f(cecon::to_native_path(at("auto.csv")), std::ios::binary | std::ios::trunc);
      f << "make,price,mpg,rep78,foreign\n"
           "\"Alder Coupe\",5230,24,4,\"Local\"\n"
           "\"Birch Sedan\",6115,19,2,\"Local\"\n"
           "\"Cedar Wagon\",4870,26,,\"Import\"\n"
           "\"Dogwood Van\",7340,16,3,\"Local\"\n";
    }
    auto rdl = exec("import delimited using \"auto.csv\", rowrange(2:4) colrange(1:4) clear\n", s);
    const double* nAfter = nullptr;
    (void)piet::dispatch("count", s);
    nAfter = s.eval.rValues.find("N");
    check(firstError(rdl.blocks).is_null() && join(varNames(s)) == "make price mpg rep78" &&
              nAfter != nullptr && *nAfter == 3.0,
          "W3a ⑦: `rowrange(2:4) colrange(1:4)` (the help file's example shape, our data) -- 4 "
          "vars, 3 obs, and the names still read from physical row 1. Got {" +
              join(varNames(s)) + "} x " +
              (nAfter ? std::to_string(*nAfter) : std::string("<unset>")));

    // BUG-477: `import delimited`'s OWN case() rule. This is a DIFFERENT
    // default from import excel's (tested at "case(upper) upper-cases the
    // names `firstrow` read" above, whose default is `preserve` --
    // [D] import excel p.495): import delimited's own default LOWER-CASES
    // the header. Measured against StataBE 19.5 on a `make,price,MPG,Foreign`
    // header (tests/eval/golden/aer_b4/b5_importcase_pins.log):
    //   B5VAL|vars_default|make price mpg foreign|x|0   <- no option at all
    //   B5VAL|vars_preserve|make price MPG Foreign|x|0  <- case(preserve)
    //   B5VAL|vars_lower|make price mpg foreign|x|0
    //   B5VAL|vars_upper|MAKE PRICE MPG FOREIGN|x|0
    //   B5REC|import_delimited|case_bogus|198|
    {
      std::ofstream f(cecon::to_native_path(at("b477.csv")), std::ios::binary | std::ios::trunc);
      f << "make,price,MPG,Foreign\n"
           "\"Elm Roadster\",8420,21,\"Import\"\n"
           "\"Fir Hatchback\",3960,29,\"Local\"\n";
    }
    auto rDefault = exec("import delimited using \"b477.csv\", clear\n", s);
    check(firstError(rDefault.blocks).is_null() && join(varNames(s)) == "make price mpg foreign",
          "BUG-477: import delimited's DEFAULT (no case() at all) lower-cases the header -- "
          "Stata's own default, oracle B5VAL|vars_default|make price mpg foreign. Got {" +
              join(varNames(s)) + "}");

    auto rPreserve = exec("import delimited using \"b477.csv\", case(preserve) clear\n", s);
    check(firstError(rPreserve.blocks).is_null() && join(varNames(s)) == "make price MPG Foreign",
          "BUG-477: case(preserve) keeps the header exactly as authored, oracle "
          "B5VAL|vars_preserve|make price MPG Foreign. Got {" + join(varNames(s)) + "}");

    auto rLower = exec("import delimited using \"b477.csv\", case(lower) clear\n", s);
    check(firstError(rLower.blocks).is_null() && join(varNames(s)) == "make price mpg foreign",
          "BUG-477: case(lower) answers the same as the default, oracle "
          "B5VAL|vars_lower|make price mpg foreign. Got {" + join(varNames(s)) + "}");

    auto rUpper2 = exec("import delimited using \"b477.csv\", case(upper) clear\n", s);
    check(firstError(rUpper2.blocks).is_null() && join(varNames(s)) == "MAKE PRICE MPG FOREIGN",
          "BUG-477: case(upper) upper-cases every name, oracle "
          "B5VAL|vars_upper|MAKE PRICE MPG FOREIGN. Got {" + join(varNames(s)) + "}");

    auto rBogus = exec("import delimited using \"b477.csv\", case(bogus) clear\n", s);
    const nlohmann::ordered_json eBogus = firstError(rBogus.blocks);
    check(eBogus.is_object() && piet::eval::coarseRc(eBogus) == "198",
          "BUG-477: case(bogus) is refused, not silently ignored -- oracle "
          "B5REC|import_delimited|case_bogus|198. Got rc " +
              (eBogus.is_object() ? piet::eval::coarseRc(eBogus) : std::string("<none>")));

    // item2 (this lane's brief): `collapse ..., fast` was classified
    // unimplemented even though the handler already answers rc 0 for it --
    // measured cell for cell against StataBE 19.5 (tests/eval/golden/aer_b4/
    // b5_importcase_pins.log): B5REC|collapse|fast|0| / B5VAL|rows_collapse|2.
    {
      std::ofstream f(cecon::to_native_path(at("b2_collapse.csv")), std::ios::binary | std::ios::trunc);
      f << "price,foreign\n10,0\n20,0\n30,1\n40,1\n50,1\n";
    }
    auto rCollapse =
        exec("import delimited using \"b2_collapse.csv\", clear\ncollapse (mean) price, by(foreign) fast\n", s);
    (void)piet::dispatch("count", s);
    const double* nCollapse = s.eval.rValues.find("N");
    check(firstError(rCollapse.blocks).is_null() && nCollapse != nullptr && *nCollapse == 2.0,
          "item2: `collapse ..., fast` is rc 0 with the grouped row count intact -- oracle "
          "B5REC|collapse|fast|0 / B5VAL|rows_collapse|2. Got " +
              (nCollapse ? std::to_string(*nCollapse) : std::string("<unset>")) + " rows");

    // item3 (this lane's brief): `joinby ..., unmatched(none)` is a real Stata
    // spelling for the drop-every-unmatched-row join this build's bare
    // joinby already computes (measured, StataBE 19.5: b5_io_pins.log
    // B5REC|joinby|unmatched_none|0, rows_none == rows on that fixture without
    // unmatched() at all). `unmatched(master)`/`unmatched(both)` need the
    // engine to keep rows this build's joinby drops -- named refusals, not
    // landed here (see the session report).
    {
      std::ofstream fm(cecon::to_native_path(at("b3_jmaster.csv")), std::ios::binary | std::ios::trunc);
      fm << "jkey,mval\n1,10\n2,20\n3,30\n";
      std::ofstream fu(cecon::to_native_path(at("b3_jusing.csv")), std::ios::binary | std::ios::trunc);
      fu << "jkey,uval\n2,200\n3,300\n4,400\n";
    }
    auto rjNone = exec("import delimited using \"b3_jmaster.csv\", clear\n"
                       "joinby jkey using \"b3_jusing.csv\", unmatched(none)\n",
                       s);
    (void)piet::dispatch("count", s);
    const double* nJoinNone = s.eval.rValues.find("N");
    check(firstError(rjNone.blocks).is_null() && nJoinNone != nullptr && *nJoinNone == 2.0,
          "item3: `joinby ..., unmatched(none)` runs -- it is this build's existing bare-join "
          "behaviour under its real Stata name. Got " +
              (nJoinNone ? std::to_string(*nJoinNone) : std::string("<unset>")) + " rows, want 2");

    // unmatched(master)/unmatched(both) are served since the engine keeps the
    // unmatched rows (joinby.ado; w2aw_data_combine_family.log dc_j_master
    // N=6, dc_j_both N=7): master keeps jkey 1 (3 rows), both adds jkey 4 (4).
    auto rjMaster = exec("import delimited using \"b3_jmaster.csv\", clear\n"
                         "joinby jkey using \"b3_jusing.csv\", unmatched(master)\n",
                         s);
    (void)piet::dispatch("count", s);
    const double* nJoinMaster = s.eval.rValues.find("N");
    check(firstError(rjMaster.blocks).is_null() && nJoinMaster != nullptr && *nJoinMaster == 3.0,
          "item3: `unmatched(master)` keeps the master row without a using match. Got " +
              (nJoinMaster ? std::to_string(*nJoinMaster) : std::string("<unset>")) + " rows, want 3");

    auto rjBoth = exec("import delimited using \"b3_jmaster.csv\", clear\n"
                       "joinby jkey using \"b3_jusing.csv\", unmatched(both)\n",
                       s);
    (void)piet::dispatch("count", s);
    const double* nJoinBoth = s.eval.rValues.find("N");
    check(firstError(rjBoth.blocks).is_null() && nJoinBoth != nullptr && *nJoinBoth == 4.0,
          "item3: `unmatched(both)` keeps the unmatched rows of both sides with a `_merge` "
          "marker. Got " +
              (nJoinBoth ? std::to_string(*nJoinBoth) : std::string("<unset>")) + " rows, want 4");
  }

  fs::remove_all(workdir, ec);
}
