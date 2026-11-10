// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "piet/handlers.h"
#include "piet/column_fetch.h"  // BUG-394: reshape long j-values via §38
#include "piet/translate.h"
#include "piet/stata_format.h"  // BUG-918: padMissingLeftJustify (tostring format())
#include "io/columnar.h"  // cecon::hbin_level_unescape: encode's escaped levels (BUG-1372)

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
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/counters.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // gFormat/applyStataFmt (BS-003 tostring default + usedisplayformat rendering)
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/strutil.h"
#include "data/missing.h"
#include "xlsx.h"   // Stata `.` / `.a`-`.z` kinds — CSV writes the extended code
#include "piet/scratch_seq.h"
#include "piet/storage_type.h"  // stataDefaultFormat (tostring usedisplayformat)
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
#include "handlers_data_verbs_internal.h"


namespace piet::handlers {
nlohmann::ordered_json runReshape(const std::vector<std::string>& stubsIn,
                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                 SessionState& session);
std::string dv_detail::resolveUsingDataset(SessionState& session, const std::string& rawPath,
                                           std::string* shown);
void dv_detail::adoptUsingValueLabels(SessionState& session, const std::string& usingPath,
                                      const std::string& usingMetaPath,
                                      const std::vector<std::string>& brought,
                                      bool copyDefinitions);
std::map<std::string, dv_detail::UsingVarMeta> dv_detail::usingVarMetaOf(SessionState& session, const std::string& usingPath,
                                                   const std::string& usingMetaPath,
                                                   Datastore& usingDs);
nlohmann::ordered_json runAppend(const std::string& filepathArg, SessionState& session,
                 const std::optional<std::string>& genVar,
                 const std::optional<std::string>& keepSpec, bool force, bool nolabel,
                 bool nonotes, std::vector<std::string>* typeNotes);
nlohmann::ordered_json runAppendFiles(const std::vector<std::string>& files, SessionState& session,
                     const std::optional<std::string>& genVar,
                     const std::optional<std::string>& keepSpec, bool force, bool nolabel,
                     bool nonotes, std::vector<std::string>* typeNotes);
nlohmann::ordered_json runMerge(const std::string& mergeType, const std::vector<std::string>& keylist,
               const std::string& usingPathArg,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
               SessionState& session, bool sequential, bool oldSyntax, bool oldSort);
nlohmann::ordered_json runDuplicates(const std::string& sub, const std::optional<std::vector<std::string>>& varlist,
                    const std::optional<std::string>& genvar, SessionState& session,
                    const std::optional<std::string>& ifExpr,
                    const std::optional<std::string>& inRange);
nlohmann::ordered_json runInspect(const std::vector<std::string>& varlist, SessionState& session);
nlohmann::ordered_json runXtsum(const std::vector<std::string>& varlist, SessionState& session);
nlohmann::ordered_json runXttab(const std::vector<std::string>& varlist, SessionState& session);
}
namespace piet::handlers::data_combine_detail {
using ojson = nlohmann::ordered_json;
using namespace dv_detail;

using ojson = nlohmann::ordered_json;

using namespace dv_detail;
extern const piet::eval::OptVocab& kVocabReshape;
extern const piet::eval::OptVocab& kVocabMerge;


// _parse_ivar_jvar: opts["i"]/["j"] are space-separated token strings
// (tokenizer stores paren-option values as raw text: `i(id year)` ->
// {"i": "id year"}).
std::pair<std::vector<std::string>, std::optional<std::string>> parseIvarJvar(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts);


// Defined below (the merge/append helpers); runReshape types its results with them.
std::optional<std::string> appendPromotedType(const std::optional<std::string>& m,
                                              const std::optional<std::string>& u);

int strWidthOf(const std::string& t);


// ---------------------------------------------------------------------------
// BUG-548 / BUG-549: the keep-list of `merge ..., keepusing(varlist)` and
// `append using F, keep(varlist)` is a VARLIST parsed against the USING data's
// names, not a list of literal names. Both verbs ask this one function, so they
// cannot disagree about what `mother*` means. Oracles (StataBE 19.5):
//   tests/eval/golden/aer_w2/w2_smalls.log section E --
//     keepusing(mother*)            rc 0, k 8 -> 10  (glob)
//     keepusing(mother_a-father_a)  rc 0, k 8 -> 11  (range, using order)
//     keepusing(zzz)                r(111) "variable zzz not found"
//     keepusing(moth)               r(111) "moth ambiguous abbreviation"
//   tests/eval/golden/aer_w2/w2_data_io.log append arms --
//     keep(z s id)  rc 0, columns land in the USING file's order (s before z)
//     keep(id zz) / keep(id m)  r(111), data untouched
//     keep(i)       rc 0 -- a UNIQUE abbreviation is accepted (-> id)
//     keep(i* z) / keep(id-s)   glob and range, rc 0
// Grammar: `*` and `?` through strutil::globMatch (no second matcher); `~` is
// the unique-match wildcard (zero or several matches = r(111), the shape the
// unab measurement in loops.cpp records for `~e` on auto); `a-b` ranges in the
// using file's column order, endpoints resolved like names; `_all`; otherwise
// an exact name, then a unique abbreviation. Result: the using file's column
// order, deduplicated. On error `out` is unspecified and the block carries the
// exact rc in `reason`.

std::optional<ojson> expandUsingKeepList(const std::string& spec,
                                         const std::vector<std::string>& usingNames,
                                         std::vector<std::string>& out);


bool isNumericStataType(const std::string& t);


// BUG-666: the storage type of every variable a using .dta declares, read from
// the import-dta sidecar (the one `use` reads, handlers.cpp applyDtaMeta). An
// unreadable sidecar gives an empty map, so its columns keep no type claim.
std::map<std::string, std::string> sidecarStataTypes(const std::string& metaPath);


// The numeric storage type a column claims: a declaration, else gen/egen's
// implicit float (BUG-295), else none.
std::optional<std::string> numericTypeClaim(const SessionState& session, const std::string& name);


// Replace a column's storage-type claim on both planes (nullopt = no claim).
// describe, `: type`, the .dta export and tostring's float/double reading all
// read it.
void setNumericTypeClaim(SessionState& session, const std::string& name,
                         const std::optional<std::string>& t);


// The current dataset's (name, dtype) pairs: the resident card when it is
// current (mutationResult has just folded the verb into it), else the
// Datastore. Reading the card avoids writing a virtual head to disk.
std::vector<std::pair<std::string, std::string>> currentNameDtypes(SessionState& session);


// BUG-666: the type append leaves on a numeric variable both datasets hold.
// StataBE 19.5 (measured 2026-09-27): float+double -> double, long+double ->
// double, long+float -> double (either way round), byte+int -> int, each with
// "(variable x was float, now double to accommodate using data's values)".
// Within byte<int<long or byte<int<float the wider one wins. A side with no
// recorded type gives no claim unless the other is double; runAppend then keeps
// that double only when the master rows are float-exact (below).
std::optional<std::string> appendPromotedType(const std::optional<std::string>& m,
                                              const std::optional<std::string>& u);


// The type -> default display format map is shared with replace's widening
// (storage_type.h), so there is one table.
using storage::stataDefaultFormat;


// merge.ado result(): a result word in keep()/assert() is a digit
// 1-5 or a prefix of masters (3+ letters), usings (2+), matches / matched
// (3+), match_updates (8+) or match_conflicts (8+). Anything else is
// `xyz:  invalid resulttype` r(198) (w2aw_data_combine_family.log dc_mk_bad,
// dc_ma_bad); `keep(mas mat)` is keep(1 3) (dc_mk_mas_mat).
std::optional<int> mergeResultWord(const std::string& w);


std::optional<ojson> parseMergeResults(const std::string& spec, std::vector<int>& out);


// str# width, or -1 (strL / not a str# spelling).
int strWidthOf(const std::string& t);


// merge/append bring the using file's notes along: each variable's (and
// _dta's) using notes are numbered on after the master's, and note0 counts
// them all (w2aw_data_combine_family.log dc_ml_notes_n / dc_a_notes_n:
// `[2]|[master note]|[u note]|...|[2]|[x master note]|[x u note]|`). The using
// char table is the .dta sidecar's, or the one a native checkpoint this
// session saved recorded beside its path. A variable the result does not have
// takes none.
void appendUsingNotes(SessionState& session, const std::string& usingPath,
                      const std::string& usingMetaPath);


// ---------------------------------------------------------------------------
// merge 1:1|m:1|1:m KEYLIST using FILE [, keepusing() nogenerate keep()
// assert()] — C++ port of merge_op.py's run_merge. cobra 0.18.0's native
// `merge` verb performs the actual join (factor-key label-join, outer join,
// keep()/assert()/keepusing()/nogenerate all natively supported per
// CONTRACT-engine §22) but its error TEXT diverges from the oracle's
// (verified empirically: cobra's cardinality error omits the duplicate-key
// value and its column-conflict message is shorter) — pre-validate the
// oracle's own checks (key existence, column conflict, cardinality) here so
// error cells match byte-for-byte; success path + option semantics delegate
// to cobra entirely.



// _check_cardinality's _find_dup: first duplicated key (by first appearance
// order, mirroring pandas' duplicated(keep=False).idxmax() — the FIRST row
// index where a dup exists) — read raw column values directly (f64 raw
// value or factor level string) to format `{k=v}` like the oracle's
// DataFrame cell repr.
struct DupKeyResult {
  bool found = false;
  std::vector<std::string> keyValueStrs;  // formatted per keylist entry
};


DupKeyResult findDupKey(Datastore& ds, const std::vector<std::string>& keylist);


// BUG-371: `inspect VARNAME` — Stata's quick "what is in this column" readout:
// how many values are negative / zero / positive, how many are integers vs not,
// how many are missing, and how many distinct values there are. It is pure
// counting over one mmap'd column, so it is PIET-native for the same reason
// `isid` is (shelling out would cost a scratch .hbin to answer an arithmetic
// question). Emitted as a `display` text block: the frontend has no inspect
// renderer, and a text block renders TODAY rather than degrading to a JSON echo.

// Stata's %9.0g rendering: 7 significant digits, trailing zeros trimmed, and the
// leading zero dropped for |v| < 1 (Stata writes .4780935, never 0.4780935).
// Checked against eight values in the live StataBE capture
// (internal capture conventions.log): .4780935, 1.674907, 5.263916,
// -.4077221, .424569, 3.912023, .29266, 4.78367 — all exact.
// BUG-1126: the shared %9.0g rule (stata_format.h general), not printf's %.7g,
// whose own small and large sides are not Stata's: xtsum printed 1e-06, 2e-05,
// 1.234568e+08 and 8.582929e-06 where StataBE prints 1.00e-06, .00002,
// 1.23e+08 and 8.58e-06 (and .0000115, -1.69e-21) in the same cells.
std::string stataG(double v);


// BUG-371: `xtsum` and `xttab` — the two panel summaries the idiom probe named.
//
// Both are PIET-native for the same reason `isid` and `inspect` are: they are
// arithmetic over mmap'd columns, and shelling out would cost a scratch .hbin to
// answer a question the data in hand already answers. Neither exists engine-side.
//
// The decompositions are the textbook ones, and they are what makes xtsum worth
// having at all — an overall SD tells a panel researcher nothing about whether
// the variation is across units or across time:
//   overall  x_it                      N = non-missing rows,   n/a
//   between  x̄_i                       n = units with any obs
//   within   x_it - x̄_i + x̄            same N as overall
// T-bar is N/n, the average number of observations per unit.


struct PanelCols {
  const double* id = nullptr;
  const double* x = nullptr;
  int64_t n = 0;
};


// Sample SD (ddof=1), matching Stata's summarize. Fewer than two values -> missing.
double sampleSdDV(const std::vector<double>& v);
}
