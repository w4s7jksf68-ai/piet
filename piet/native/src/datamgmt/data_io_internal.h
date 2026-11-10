// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "piet/handlers/io.h"
#include "piet/translate.h"

#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/clonefile.h>  // BUG-138/L5-IOSAVE: O(1) same-volume save (runSaveData)
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // MoveFileExW / GetLastError (runSaveData's atomic replace)
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <charconv>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <iconv.h>   // AER scope-2 W3a: `import delimited, encoding()`

#include "piet/cobra_runner.h"
#include "piet/dispatcher.h"  // dispatch(): xtile/cumul evaluate exp with the user's generate
#include "piet/counters.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // gFormat/applyStataFmt (BS-003 tostring default + usedisplayformat rendering)
#include "piet/eval/pyvalue.h"
#include "piet/display_hardening.h"  // resolveVarName: xtile cutpoints()
#include "piet/delimited_io.h"  // import delimited: the one native reader
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/strutil.h"
#include "data/missing.h"
#include "fileio/xlsx.h"   // Stata `.` / `.a`-`.z` kinds — CSV writes the extended code
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)

// ---------------------------------------------------------------------------
// BUG-233 / FIX:OPTNET-ABBREV-SWEEP — this file's option vocabularies.
//
// Every set below used to be a function-local `static const` sitting next to its
// reject net, which meant nothing upstream of the handler could see it — and the
// handler's own `k == "fullname"` comparisons therefore ran against RAW keys
// while eval::rejectUnknownOptions had already ACCEPTED an abbreviation of them.
// That is BUG-231's mechanism, and it was live in nets across six files.
//
// Registering here (static init, before any dispatch) lets dispatcher.cpp's single
// resolution point canonicalize the keys BEFORE routing. The sets did not get
// copied: each net now binds a reference back to its registered entry, so there is
// still exactly one place each spelling is written.
#include "datamgmt_support.h"
#include "using_dataset.h"
#include "estimation/estimation_sample_internal.h"  // e(sample) T2: the copy verbs' mask helpers
#include "engine/scratch_columns.h"          // xtile's sample, source and weight columns


namespace piet::handlers {
nlohmann::ordered_json runXtileStata(const std::string& newvar, const std::string& expText,
                    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                    const std::optional<std::string>& ifExpr,
                    const std::optional<std::string>& inRange,
                    const std::optional<std::string>& weightVar, SessionState& session,
                    const std::optional<std::string>& weightType);
nlohmann::ordered_json runExpandIf(const std::string& countArg, const std::optional<std::string>& genVar,
                  const std::optional<std::string>& ifExpr,
                  const std::optional<std::string>& inRange, SessionState& session);
}
namespace piet::handlers::data_io_detail {
using ojson = nlohmann::ordered_json;
using namespace dv_detail;

using ojson = nlohmann::ordered_json;

using namespace dv_detail;


// Resolve joinby's `using` file to a locally-readable hbin path. The engine's
// `cobra joinby` verb takes .hbin only (and PIET's own pre-validation reads
// the using file via Datastore), unlike
// append (which only needs cobra's OWN --using argument — cmd_append
// auto-detects .dta/.csv internally) or merge (which pre-converts ONLY for
// its own pre-validation peek, then hands cobra the original path). Mirrors
// runMerge's using-file resolution above, kept as a SEPARATE copy per this
// codebase's established per-TU/per-callsite convention (see errorFromRaw's
// own header note above) rather than refactoring runMerge to share it.
struct JoinbyUsingResult {
  bool ok = false;
  std::string hbinPath;
  std::string usingPath;  // the resolved file (a checkpoint's metadata is keyed by it)
  std::string metaPath;
  ojson errBlock;
};


std::string stripOneQuotePair(const std::string& v);


JoinbyUsingResult resolveJoinbyUsing(SessionState& session, const std::string& usingPathArg);


// The variables only the using file has keep what it states about them -- its
// variable label, format, storage type and value label (dc_j_base_v5 uval
// `float|%7.1f|[u val]`; BUG-1157: joinby and cross of a file whose int u
// carries ul list u through ul, StataBE).
void adoptUsingOwnVars(SessionState& session, const JoinbyUsingResult& ur,
                       const std::map<std::string, UsingVarMeta>& uMeta,
                       const std::vector<std::string>& usingOnly);
inline constexpr int kXtileDefaultNq = 2;


bool parseIntOptDV(const std::string& text, int& out);


// Does the live shadow card (or, failing that, the datastore) already carry this
// name? Used to refuse overwriting an existing column, matching Stata's r(110).
bool columnExistsDV(SessionState& session, const std::string& name);


// A refused new-variable name under Stata's own code (strutil::NameRefusal).
ojson nameRefusalBlock(const strutil::NameRefusal& r);


ojson newVarGuardDV(SessionState& session, const std::string& verb, const std::string& newvar);




// `[type] newvar` as typed before the `=`. A storage type in front of a name is
// accepted and ignored; the same word alone is what Stata's syntax reads as a
// misplaced type, hence its sentence about n() (StataBE 19.5).
std::optional<ojson> readXtileTarget(const std::string& typed, std::string* name);


// cutpoints(): one numeric variable, abbreviations as anywhere in a varlist.
std::optional<ojson> readXtileCutVariable(const eval::OptValue& option, SessionState& session,
                                          std::string* resolved);


// nquantiles(): `2`, `2.0` and `3.` are integers as written; `2.5` and `1e1` are
// not; a bare word is evaluated as a name, which is how `n(abc)` comes to be
// "abc not found". `shown` keeps the number as typed, for the variable label.
std::optional<ojson> readXtileCount(const eval::OptValue& option, SessionState& session,
                                    long long* count, std::string* shown);


// Stata's limit on a variable label, in characters.
std::string clippedVarLabel(const std::string& text);


// The smallest integer type that holds the result's largest category.
const char* xtileStorageType(double largestCategory);


// ---------------------------------------------------------------------------
// export / import delimited (P1 wave). PIET-native: the writer needs the
// session's value labels, which live here and not in the .hbin.




// A numeric cell the way Stata's `export delimited` writes it. A missing is
// `.` or its extended code `.a`..`.z` (oracle: Stata's own p1_roundtrip.csv
// carries literal `.a`).
//
// W:P8. The numeric spelling used to be the shortest round-trip decimal
// (`pythonFloatRepr`), which is NOT what Stata writes — measured cell-by-cell
// against Stata's own export of the same fixture, every one of 4,800 data rows
// differed. Two separate deviations: an integral value came out `1.0` where
// Stata writes `1` (so every id/year/region column in every exported CSV
// carried a spurious `.0` for whatever read it next), and a fractional value
// came out at 17 significant digits with its leading zero where Stata writes
// 16 without it. Stata's real rule, pinned by probing StataBE 19.5 with
// exact-valued doubles rather than inferred: `%.16g`, leading zero dropped
// for |x|<1, and the exponent branch re-spelled `%.11e` (`1e-20` ->
// `1.00000000000e-20`, `123456789012345678` -> `1.23456789012e+17`).
//
// Deliberate consequence, not an oversight: 16 digits is one short of a
// guaranteed round trip for a double, so a value can lose its last bit
// through a CSV. That is Stata's own behaviour and gate ① puts matching it
// first; PIET's own lossless path is `.dta`/`.hbin`.
//
// Known remaining divergence, out of reach here rather than unnoticed: Stata
// writes a `float` column at 8 significant digits, and PIET's in-memory
// datastore is all-f64, so a float column loaded from a .dta exports at
// double width. Closing that needs per-column storage types the datastore
// does not carry.
// `v` written with `dec` decimals, rounded HALF AWAY FROM ZERO (printf rounds
// half to even, which is what put 22 of 28,649 measured cells one unit below
// Stata in the 16th digit), then tidied Stata's way: trailing zeros and a bare
// trailing point dropped, and no leading zero on |x|<1.
// `v` written with `dec` decimals, then tidied Stata's way: trailing zeros and
// a bare trailing point dropped, and no leading zero on |x|<1.
std::string fixedDecimals(double v, int dec);


// P2-PERF ARC-B/S08. The same spelling as fixedDecimals(), written straight
// into a caller-owned buffer instead of a fresh std::string, and formatted from
// the MAGNITUDE so the "drop the leading zero of |x|<1" rule is a returned
// offset rather than an erase-in-the-middle. The caller prepends the sign;
// printf rounds on magnitude, so |round(v)| == round(|v|) and the bytes are
// identical either way. Returns the trimmed span inside `buf`.
struct FixedSpan {
  const char* p = nullptr;
  size_t len = 0;
};

FixedSpan fixedDecimalsAbs(char* buf, size_t cap, double av, int dec);


std::string csvNumeric(double v);


// P2-PERF ARC-B/S08 — the same function, appending instead of returning, with
// one shortcut. `export delimited` on a 500k-cell frame called csvNumeric once
// per cell and csvNumeric allocated a std::string per snprintf attempt; the
// writer measured 21 MB/s, which is not a formatting rate, it is an allocation
// rate.
//
// The shortcut is the INTEGRAL case (`id`, `year`, `region`, any coded
// variable — most columns of most datasets): the general loop reaches it by
// asking snprintf for "%.17f" and then deleting the seventeen zeros it just
// asked for. An integral double under 1e17 spells identically through
// std::to_chars, in one pass, with no allocation and no trimming. Above 1e17
// the integer part alone exceeds the 17-character budget, so that case still
// belongs to the loop (which sends it to the `%.11e` branch, unchanged).
//
// Everything else is byte-for-byte the old path: same widest-plain-decimal
// search from dec=17 down, same "rounded away to nothing" skip, same `%.11e`
// fallback. The gate is a byte-identity diff of an exported CSV, not a reading
// of this comment.
void appendCsvNumeric(std::string& out, double v);
inline constexpr size_t kCsvBlockBytes = 1u << 20;


// P2-PERF ARC-B/S08: csvField's no-quoting-needed case returned a COPY of the
// level string for every cell. Appending skips it.
void appendCsvField(std::string& out, const std::string& s, char delim);
inline constexpr int64_t kDaysTdEpochToUnix = 3653;
inline constexpr int64_t kMsPerDay = 86400000;


int64_t floorDiv(int64_t a, int64_t b);

int64_t floorMod(int64_t a, int64_t b);


// days since 1970-01-01 -> civil y/m/d (Hinnant's civil_from_days).
void civilFromDays(int64_t z, int64_t* y, unsigned* m, unsigned* d);


std::string tdDayString(int64_t dayIndex);


// The letter after `%t` for a date-family format, or 0 when `fmt` is not one
// this renderer owns. A detail suffix (`%tdNN/DD/CCYY`) yields 0 on purpose.
char dateFamilyOf(const std::string& fmt);


std::string formatStataDate(double v, char family);



bool hasFlagOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                const std::string& name);


// An option's value with its surrounding double quotes off. `sheet("Alpha")`
// and `sheet(Alpha)` are the same request.
std::string optDequote(const std::string& raw);


// The value of a named option, or nullopt when it is absent or was given as a
// bare flag.
std::optional<std::string> optStrOf(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                    const std::string& name);


// Stata's pathsuffix(): the extension of the last path component, "" when it
// has none. export delimited / import delimited supply ".csv" and the excel
// pair ".xlsx" to a name without one (w2ar dx_exd_noext log:760-769 writes
// ne1.csv and leaves the bare name absent; dx_exx_noext log:1527).
bool pathHasSuffix(const std::string& p);


std::string withDefaultSuffix(const std::string& p, const char* suffix);




// The file as the user typed it, with the suffix the command supplies: what
// Stata's export messages name (`file out.csv saved`, `file ./sub/o2.csv
// saved`, `file out.xlsx already exists`), never the resolved absolute path,
// which would put this machine's working directory into every captured
// transcript (BUG-410).
std::string typedFileName(const std::string& pathArg, const char* suffix);


// True when the directory a file would be written into does not exist:
// Stata's export answers r(603) "file could not be opened" there (w2ar
// dx_exd_missing_dir, log:796).
bool parentDirMissing(const std::string& p);


// A float column's text: Stata writes float storage at 8 significant digits
// (w2ar dx_exd_plain_L2 `1.01` for float 1.01, log:455; w2b 7.0100002,
// w2b log:420-423), integers in full, without the leading zero of |x|<1 (the
// same tidy rule the double path uses).
void appendFloatNumeric(std::string& out, double v);


std::string trimBlanks(const std::string& s);


void appendQuotedField(std::string& out, const std::string& s, char delim,
                       DelimWriteSpec::Quote q);






// The block an import answers with: `use`'s own `open` shape (the frontend
// reads source_file/filepath off it), over the frame just adopted.
ojson openBlockFor(SessionState& session, const std::string& path);




using ImportOptionList = std::vector<std::pair<std::string, eval::OptValue>>;


// Stata's sentence under r(198): every option-grammar refusal below is one.
ojson importRefusal(const std::string& sentence);


bool abbreviates(const std::string& typed, const char* word, size_t shortest);


// A whole number spelled with digits and at most a leading sign. Far too many
// digits saturate rather than fail, so they still read as "a big number".
bool readWholeNumber(const std::string& text, long long* out);


// One inclusive 1-based stretch of records or columns; 0 leaves a side open.
struct ImportSpan {
  long long first = 0;
  long long last = 0;
};


// rowrange() and colrange() share one grammar and differ in wording: Stata says
// "start column" but "end col", and answers a lone colon differently for each.
struct SpanWording {
  const char* option;
  const char* item;
  const char* endItem;
  const char* loneColon;
};

const SpanWording kRecordSpan{"rowrange", "row", "row", ": invalid end row in rowrange() option"}
;

const SpanWording kColumnSpan{"colrange", "column", "col",
                              " is an invalid end column in colrange() option"}
;


std::optional<ojson> readSpan(const std::string& typed, const SpanWording& words,
                              ImportSpan* span);


// numericcols()/stringcols(): file column numbers, held as stepped runs so that
// a huge `1/99999999` costs nothing.
struct ColumnPicks {
  struct Run {
    long long from, step, to;
  };
  bool everyColumn = false;
  std::vector<Run> runs;

  bool holds(long long column) const {
    if (everyColumn) return true;
    return std::any_of(runs.begin(), runs.end(), [column](const Run& r) {
      const long long gap = column - r.from;
      return gap % r.step == 0 && gap / r.step >= 0 && gap / r.step <= (r.to - r.from) / r.step;
    });
  }
};


bool readColumnRun(const std::string& word, ColumnPicks::Run* run);


std::optional<ojson> readColumnPicks(const std::string& typed, const std::string& option,
                                     ColumnPicks* picks);


// delimiters(): a shortcut word or a quoted set of characters, then at most one
// sub-option after a comma. *named stays false for `""` and for nothing at all,
// which both leave the choice to the reader.
std::optional<ojson> readDelimiters(const std::string& raw, delimited::ReadSpec* reader,
                                    bool* named);


// Everything `import delimited`'s options ask for, once they have been accepted.
struct ImportRequest {
  delimited::ReadSpec reader;
  bool delimiterNamed = false;
  delimited::Case nameCase = delimited::Case::kLower;
  bool noNamesRecord = false;
  long long namesRecord = 0;  // 1-based; 0 = decide from the first record
  std::string namesRecordTyped;
  ImportSpan records;
  ImportSpan columns;
  ColumnPicks numericColumns;
  ColumnPicks stringColumns;
  bool asDouble = false;
  bool asFloat = false;
  std::string encoding;  // "" = work it out from the bytes
};


std::optional<ojson> readOneImportOption(const std::string& key, const std::string& raw,
                                         ImportRequest* request);


std::optional<ojson> readImportOptions(const ImportOptionList& opts, ImportRequest* request);


// Strict RFC 3629 shape: no overlong forms, no surrogates, nothing past U+10FFFF.
bool holdsOnlyUtf8(const std::string& bytes);


// nullopt when the bytes are not valid in `encoding`.
std::optional<std::string> decodeToUtf8(const std::string& bytes, const std::string& encoding);


// The file's text as UTF-8; an error block when it cannot be had.
std::optional<ojson> loadImportText(const std::string& path, const std::string& shownName,
                                    const std::string& encoding, std::string* text);


// With no varnames(), the first record names the columns when, leaving its
// missing tokens aside, it holds text and no number.
bool recordReadsAsNames(const std::vector<std::string>& record);


ojson answerEmptyImport(SessionState& session);


// The namelist of `import delimited a b using f`: the leading names change and
// nothing else does.
std::optional<ojson> renameLeadingColumns(const std::vector<std::string>& namelist,
                                          std::vector<std::string>* names);


// ---------------------------------------------------------------------------
// export / import excel (P1 wave). The .xlsx container work lives in
// piet/native/src/xlsx.{h,cpp}; this layer is the Stata surface.



// `case(preserve|lower|upper)` — [D] import excel p.495. The case rule applies
// to the names read by `firstrow`, and Stata's default is preserve.
enum class NameCase { kPreserve, kLower, kUpper };



// One A1-style cell address. `col`/`row` come back 0 for a side the address
// leaves out, so "A" is column 1 with an unbounded row and "8" is row 8 with an
// unbounded column.
bool parseCellAddr(const std::string& s, int* col, int* row);


// `cellrange([start][:end])` — [D] import excel p.495. Both sides are optional,
// so `A8`, `A3:AG281`, `:D70` and `A3:` are all legal spellings.
bool parseCellRangeOpt(const std::string& spec, xlsx::ReadOptions* out, std::string* err);


// Stata's own default variable names when there is no `firstrow`: the SHEET's
// column letters, upper case (BUG-425). `idx0` is 0-based from column A.
std::string excelColumnName(size_t idx0);


// `import excel [using] FILE [, sheet("S") cellrange(R) firstrow case(C)
//                              allstring describe clear]` — AER scope-2 W3a.
//
// The option cluster this build was missing is the single most frequent one in
// the twelve-paper corpus (`import excel` appears 273 times, 243 of them in
// paper 015), and every one of those calls names a sheet.


// How Stata's import excel reads an Excel date/time number format (StataBE
// scratch-r150/t7-007/stata/dt: dt2.golden + dt3.golden -- every built-in date
// id and four custom codes, as numeric, allstring and text-mixed columns; AER
// 007's survey exports, whose dates sort and merge its cleaning). A numeric
// column holds the Stata date (`time` false: days, %td) or datetime (`time`
// true: milliseconds, %tc) with the fraction kept, shown in `stataFmt`. Read as
// text (allstring, or a column that also holds text) a built-in id is drawn as
// Excel draws it -- four-digit years, id 22's `h` a 12-hour clock with no
// AM/PM -- right-aligned in `width`; a custom code is drawn in Stata's own
// default (%tc `08may2019 05:58:59`, %td `08may2019`, time-only `05:58:59`).
struct ExcelDateStyle {
  bool time = false;
  std::string stataFmt;
  int id = 0;  // the built-in id drawn Excel's way; 0 = drawn in stataFmt's default
  size_t width = 0;
};


ExcelDateStyle excelDateStyle(int id, const std::string& code);


// The Stata milliseconds of an Excel serial. Excel counts a 29feb1900 that
// never was, so a serial before 61 is one day later than the 21916 offset
// gives: serial 0 is 31dec1899, 59.5 noon of 28feb1900, 61.25 is 01mar1900
// 06:00 (dt2.golden). Days, then seconds, then milliseconds, in that order:
// 43593.24929861111 is 1872914339399.999756 in Stata, where days*86400000
// gives ...400; the %td value is this over 86400000 (21677.2492986111065).
double excelSerialToTc(double serial);

double excelSerialToTd(double serial);


// A date cell read as text. The clock comes from the %tc value, truncated
// like every %tc display: 23:59:59 stored as 44555.99998842592 is 23:59:58.
std::string excelDateText(double serial, const ExcelDateStyle& st);
}
