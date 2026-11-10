// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command entry points: dataset load/save, import/export, raw text, archives, postfile,
// file. Implemented under piet/native/src/fileio/; input in src/datamgmt/.
#ifndef PIET_NATIVE_HANDLERS_IO_H
#define PIET_NATIVE_HANDLERS_IO_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

// `use FILEPATH [, clear]` — load a dataset (dta/csv/hbin) into the session.
// M3b: .dta loads via cobra 0.18.0's native `import-dta` (formats 115/117/
// 118) — value-labeled numerics stay f64 raw codes, genuine strings become
// factor columns; labels/formats/value-labels applied to SessionState from
// the --out-meta sidecar (mirrors routes_data.py's _convert_to_hbin +
// _apply_dta_var_labels).
// BUG-023 (bug-for-bug, frozen 1.16.0 oracle, the internal bug ledger):
// unconditionally rewrites session.cwd to the loaded file's directory. Fixing
// this is an explicit 2.0-scope decision deferred to M3/M4 adjudication +
// Eval corpus regeneration — NOT this milestone's call.
nlohmann::ordered_json runUse(const std::string& filepathArg, SessionState& session);

// `input varlist` ... `end`. The BLOCK is folded into one line by
// `captureInputBlocks` during preprocessing (0x1F separators -- a byte no Stata
// source contains), the same shape `program`/`end` already uses, so the
// dispatcher sees a single command rather than a statement stream.
nlohmann::ordered_json runInput(const std::string& rawCmd, SessionState& session);
std::vector<std::string> captureInputBlocks(const std::vector<std::string>& lines);

// ---- AER-SCOPE1 bundle G (IO-TEXT, 2026-09-04) -----------------------------
// The older text I/O verbs, as thin shims over the loaders that already exist,
// plus `putexcel` as a cell-addressed extension of the xlsx writer. Nothing
// here is a second reader or a second writer: `insheet` IS `import delimited`
// with Stata's own naming rules applied afterwards, `outsheet` IS
// `export delimited` with a tab default, and `saveold` IS `save` with an
// honest note about the format actually written.

// `insheet using f [, clear names nonames comma tab delimiter() case]`.
nlohmann::ordered_json runInsheet(const eval::ParsedCommand& parsed, SessionState& session);
// `outsheet [varlist] using f [, comma replace noquote nolabel names]`.
nlohmann::ordered_json runOutsheet(const eval::ParsedCommand& parsed, SessionState& session);
// `infile varlist using f [, clear]` -- free format: whitespace/comma tokens,
// quoted strings, `.` for missing, records wrapping every `varlist.size()`
// values regardless of source line breaks (Stata's own reader).
nlohmann::ordered_json runInfile(const eval::ParsedCommand& parsed, SessionState& session);
// `infix [type] var #-# ... using f [, clear]` -- fixed columns, 1-based and
// inclusive.
nlohmann::ordered_json runInfix(const eval::ParsedCommand& parsed, SessionState& session);
// `outfile [varlist] using f [, replace noquote comma nolabel]`.
nlohmann::ordered_json runOutfile(const eval::ParsedCommand& parsed, SessionState& session);
// `saveold f [, replace version(#)]`. This build writes Stata format 118 and
// SAYS so on every call rather than silently producing a 118 file under a name
// that promises an older format -- the note is the whole point of the row.
nlohmann::ordered_json runSaveold(const eval::ParsedCommand& parsed, SessionState& session);
// `putexcel set|A1 = ...|save|clear|describe`. Stateful across statements; the
// cell map lives in `SessionState::putexcel` and the workbook is rewritten in
// full on every assignment.
nlohmann::ordered_json runPutexcel(const std::string& rawCmd, SessionState& session);
// `filefilter in out, from() to() [replace]` -- publishes r(occurrences).
nlohmann::ordered_json runFilefilter(const eval::ParsedCommand& parsed, SessionState& session);
// `copy src dst [, replace text]` -- a URL source is refused by name; this
// build has no network access from Stata verbs.
nlohmann::ordered_json runCopy(const eval::ParsedCommand& parsed, SessionState& session);

// `postfile H type1 name1 ... using PATH [, replace]` / `post H (e1)(e2)...`
// / `postclose H` — C++ port of postfile.py (S8). All three take the
// post-macro-expansion raw line (postfile/post) or bare handle name
// (postclose); silent (empty array) on success like the oracle.
nlohmann::ordered_json runPostfile(const std::string& rawLine, SessionState& session);

// BUG-343: `file open|write|read|close` (text mode). One entry point taking the
// RAW line, like runPostfile above — the sub-verb, the handle name, the `using`
// path and the write payload all have to be read off the original text (quotes
// and all), which a pre-parsed varlist has already destroyed.
nlohmann::ordered_json runFileCmd(const std::string& rawLine, SessionState& session);
nlohmann::ordered_json runPost(const std::string& rawLine, SessionState& session);
nlohmann::ordered_json runPostclose(const std::string& handle, SessionState& session);

// `save FILE [, replace]` — persist the current dataset to FILE.
// `dtaRelease` > 0 writes a genuine Stata .dta of that release (115/117/118)
// whatever FILE's extension is (`saveold`'s target; a `.tmp` tempfile included).
nlohmann::ordered_json runSaveData(const std::string& filepath, bool replace,
                                    SessionState& session, int dtaRelease = 0);

// `export delimited [varlist] using FILE [, replace delimiter(D) nolabel]` and
// `import delimited [using] FILE [, clear delimiter(D)]` — P1 wave.
// PIET-native rather than engine-side because the WRITER needs the session's
// value labels: Stata's export delimited writes a labeled numeric as its LABEL
// text by default (`nolabel` writes the code), and those labels live only in
// PIET's session, not in the .hbin. Missing cells are written the way Stata
// writes them — `.`, or the extended code `.a`..`.z` (oracle: the Stata-written
// p1_roundtrip.csv carries literal `.a` in 22 cells of `y`).
// import delegates to the same content-sniffing loader `use` already uses.
nlohmann::ordered_json runExportDelimited(const std::vector<std::string>& varlist,
                                          const std::string& pathArg,
                                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                          SessionState& session);
// The same with `[if] [in]` (export delimited takes [if] [in]; w2ar dx_exd_ifin log:746).
nlohmann::ordered_json runExportDelimited(const std::vector<std::string>& varlist,
                                          const std::string& pathArg,
                                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                          const std::optional<std::string>& ifExpr,
                                          const std::optional<std::string>& inRange,
                                          SessionState& session);

// One writer for export delimited and outsheet: the two differ only in these
// switches (w2ar log:453-929). `quote`:
//   kMinimal     quote a string only when it holds the delimiter, `"`, CR or
//                LF; double an inner quote (export delimited's default)
//   kAll         quote every string and value-label text, doubling inner
//                quotes (export delimited, quote)
//   kAllVerbatim quote every string and label text, inner quotes NOT doubled
//                (outsheet's default: `"q"r"`, log:815)
//   kNone        never quote (outsheet, noquote)
// `dataFmt` writes every numeric through its display format (export
// delimited, datafmt; outsheet always). Dates are written formatted either
// way.
struct DelimWriteSpec {
  enum class Quote { kMinimal, kAll, kAllVerbatim, kNone };
  char delim = ',';
  bool header = true;
  bool noLabel = false;
  bool dataFmt = false;
  Quote quote = Quote::kMinimal;
  std::string verb = "export delimited";
  // The file as the user typed it, suffix included: every message names it, never
  // the resolved path, which would put this machine's working directory into each
  // captured transcript (BUG-410). Empty = the path itself.
  std::string shown;
};
// Writes `resolvedPath` (already resolved; the caller owns the exists/replace
// decision) from the rows `ifExpr`/`inRange` select.
nlohmann::ordered_json writeDelimitedFile(const std::vector<std::string>& varlist,
                                          const std::string& resolvedPath,
                                          const DelimWriteSpec& spec,
                                          const std::optional<std::string>& ifExpr,
                                          const std::optional<std::string>& inRange,
                                          SessionState& session);
// A block whose text or message names the RESOLVED path (the xlsx writer's
// exceptions), re-spoken with the file as the user typed it in that place
// (BUG-410). A miss leaves the block as it was.
nlohmann::ordered_json reEchoPath(nlohmann::ordered_json block, const std::string& resolved,
                                  const std::string& shown);
nlohmann::ordered_json runImportDelimited(const std::string& pathArg,
                                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                          SessionState& session);
// `import delimited [namelist] using FILE [, options]`.
nlohmann::ordered_json runImportDelimited(const std::vector<std::string>& varlist,
                                          const std::string& pathArg,
                                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                          SessionState& session);

// `export excel [varlist] using FILE [, replace firstrow(variables) nolabel]`
// `import excel [using] FILE [, firstrow clear]` — P1 wave. Same value-label
// rule as the delimited pair. The .xlsx layer is piet/native/src/xlsx.{h,cpp}.
nlohmann::ordered_json runExportExcel(const std::vector<std::string>& varlist,
                                      const std::string& pathArg,
                                      const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                      SessionState& session);
nlohmann::ordered_json runExportExcel(const std::vector<std::string>& varlist,
                                      const std::string& pathArg,
                                      const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                      const std::optional<std::string>& ifExpr,
                                      const std::optional<std::string>& inRange,
                                      SessionState& session);
nlohmann::ordered_json runImportExcel(const std::string& pathArg,
                                      const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                      SessionState& session);
// The file `import delimited` (excel=false) or `import excel` (excel=true)
// reads for `pathArg`, with the suffix a stem takes. The dispatcher's r(4)
// check asks it too: a missing file is answered first, r(601).
std::string importSourcePath(SessionState& session, const std::string& pathArg, bool excel);

// ---- AER tier-1 (2026-10-28), handlers_t1_*.cpp ----------------------------
// The difficulty-1 bundle: every command below is wiring over pieces that
// already existed. Answers: tests/eval/golden/aer_w2/w2ax_t1_official.log and
// w2ax_t1_community.log. One TU per lane, each lane writing only its own file.
//
// zip_commands.cpp -- the ZIP verbs over zip_io.h.
// `unzipfile zipfile[.zip] [, replace]`; existing files are skipped without
// replace (rc 0, "can't overwrite ... skipping").
nlohmann::ordered_json runUnzipfile(const eval::ParsedCommand& parsed, SessionState& session);
// `zipfile file [file ...], saving(name[, replace])` -- stored, not deflated.
nlohmann::ordered_json runZipfile(const eval::ParsedCommand& parsed, SessionState& session);
// `zipuse [varlist] using file | zipuse file [, clear]` (zipsave package).
nlohmann::ordered_json runZipuse(const eval::ParsedCommand& parsed, SessionState& session);
// `zipsave file [, replace]` (zipsave package) -- writes <file>.dta.zip.
nlohmann::ordered_json runZipsave(const eval::ParsedCommand& parsed, SessionState& session);

// `postutil clear | dir`.
nlohmann::ordered_json runPostutil(const eval::ParsedCommand& parsed, SessionState& session);

}  // namespace piet::handlers

#endif  // PIET_NATIVE_HANDLERS_IO_H
