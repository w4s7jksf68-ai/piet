// R2 (giant-file split, 2026-09-01): the shared file-local helpers of the
// former 5,589-line handlers_data_verbs.cpp, promoted VERBATIM (modulo
// inline linkage) so its three split TUs keep one copy. Include only from
// those TUs. The preamble below is the original file head, platform guards
// and all — scraping bare #include lines out of it is how the first cut of
// this header put <windows.h> into a macOS build.
#pragma once
// M3b: MI-066 data verbs (merge/reshape/append/duplicates/gsort/order/
// recode) via cobra 0.18.0's native CLI verbs. C++ ports of the
// corresponding piet/backend/piet/{merge_op,reshape,append,duplicates,
// gsort,order,recode}.py oracle modules — bug-for-bug where cobra 0.18.0
// and the Python backend agree; where they differ, the FROZEN 1.16.0
// corpus wins (this file pre-validates the oracle's own error conditions
// in C++ BEFORE shelling out to cobra, so error text matches byte-for-byte
// even where cobra's native error text differs — see each handler's notes).
#include "piet/handlers.h"
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

namespace piet { namespace handlers { namespace dv_detail {
using ojson = nlohmann::ordered_json;  // the TU-local alias the helpers were written against

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `trim(...)`/`lower(...)`
// call sites didn't need touching. Pure dedup, no behavior change.
inline std::string trim(const std::string& s) { return strutil::trim(s); }

inline ojson engineErrorBlock(const std::string& message, const std::string& code = "engine_error") {
  return eval::engineError(message, code);
}

// BUG-038(a): forwards to the shared mapping point (piet::cobraErrorMessage,
// cobra_runner.h) — kept as a thin wrapper (not removed) so this file's
// call sites didn't need touching. Pure dedup, no behavior change here.
inline std::string cobraErrorStr(const CobraError& ce) { return cobraErrorMessage(ce); }

// Several cobra verbs (gsort/order/recode/duplicates drop|tag/append/merge's
// success path) print a plain-text status line to STDERR and JSON only on
// error — runCobraJson()'s "parse stdout as JSON" contract doesn't fit,
// so these call the raw runCobra() and build the CobraError manually from
// its stderr (mirrors cobra_runner.cpp's own errorFromResult(), which is
// file-local there).
inline CobraError errorFromRaw(const CobraRunResult& r) {
  std::string stderrText = r.stderr_text;
  while (!stderrText.empty() &&
         (stderrText.back() == '\n' || stderrText.back() == '\r' || stderrText.back() == ' ')) {
    stderrText.pop_back();
  }
  try {
    nlohmann::json payload = nlohmann::json::parse(stderrText);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                        err.at("message").get<std::string>(), err.value("detail", std::string()));
    }
  } catch (const nlohmann::json::exception&) {
  }
  std::string firstLine = stderrText;
  auto nl = firstLine.find('\n');
  if (nl != std::string::npos) firstLine = firstLine.substr(0, nl);
  if (firstLine.empty()) firstLine = "cobra exited " + std::to_string(r.exit_code);
  return CobraError("engine_error", firstLine, stderrText);
}

// P5 (BUG-062): scratch-filename collision fix. The old scheme derived the
// filename from wall-clock time (`std::time(nullptr)` — ONE-SECOND
// resolution; multiplying by 1000 only manufactures millisecond-LOOKING
// digits, it adds no real millisecond precision), so any two scratch files
// requested with the same prefix inside the same wall-clock SECOND — e.g.
// two `order` or `gsort` calls back to back in one .do script, entirely
// realistic since each cobra round-trip is only a few ms — collided on the
// exact same path: the second cobra invocation then wrote over (or was read
// back from) the first call's in-flight file instead of getting its own.
// Fixed with a per-process monotonic counter (can never repeat or go
// backwards for the life of this process, regardless of how fast the wall
// clock ticks) plus the OS pid, so two concurrent piet_native processes
// sharing a workdir (e.g. $PIET_WORKDIR pinned by a test harness) can't
// collide with each other either. Every scratch-path call site in this file
// (nextPath() below, plus rename's/clonevar's own inline CSV scratch paths)
// draws from this one shared counter.
inline std::string scratchToken() {
  static std::atomic<uint64_t> counter{0};
  static const long long pid = static_cast<long long>(cecon::native_process_id());
  return std::to_string(pid) + "_" +
         std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

inline std::string nextPath(const std::string& workdir, const std::string& prefix) {
  std::string path = workdir + "/" + prefix + "_" + scratchToken() + ".hbin";
  registerScratchMint(path);  // BUG-389: the data verbs' own mints join the registry
  return path;
}

inline bool fileExists(const std::string& path) { return cecon::native_exists(path); }

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — see the
// trim() wrapper above for why this stays a thin pass-through.
inline std::string lower(std::string s) { return strutil::lower(std::move(s)); }

// Lower-cased extension (including the leading dot), or "" if there is none
// — mirrors handlers.cpp's own file-local ext() (each TU keeps its own copy
// per this codebase's established convention, see strutil.h's header note).
inline std::string extOf(const std::string& path) {
  auto pos = path.find_last_of('.');
  if (pos == std::string::npos) return "";
  return lower(path.substr(pos));
}

// CSV-escape a field (RFC4180 doubled-quote) — same convention as
// strmutate.cpp's own csvEscape()/postfile.cpp's, kept as a file-local copy
// rather than a new shared header. Still used by the remaining whole-frame
// CSV writers in this file (runDestring / runTostring / runJoinby), none of
// which has a native cobra verb yet.
inline std::string csvEscape(const std::string& text) {
  std::string esc;
  esc.reserve(text.size() + 2);
  esc += '"';
  for (char c : text) {
    if (c == '"') esc += '"';
    esc += c;
  }
  esc += '"';
  return esc;
}

// fix/verb-overhead-sweep: name-only lookup over the resident shadow card
// (session.shadowVarlistIfCurrent()) — the same mechanism fix/sort-validate-
// lazy introduced for handlers_m3.cpp's runSort, reused here by runGsort/
// runOrder so neither needs session.datastore() (a full mmap + header parse
// of however many MB the current hbin is) just to answer "does column X
// exist". File-local copy of handlers_m3.cpp's identically-named helper
// (this codebase's established per-TU convention — see this file's own
// trim()/errorFromRaw() note above) rather than a new shared header.
//
// S4a sibling (fix/verb-overhead-sweep S4): the same card lookup, but handing
// back the ENTRY rather than a yes/no — runEncode needs the dtype alongside the
// name, and reading it off the card is what lets that verb skip its own
// datastore open too. cardHasName forwards to this rather than keeping a second
// copy of the scan (IDIOMS.md "third copy -> hoist"; the hoist target is this
// TU's own anonymous namespace, per the per-TU convention noted above).
inline const VarInfo* cardFind(const std::vector<VarInfo>& card, const std::string& name) {
  for (const auto& info : card) {
    if (info.name == name) return &info;
  }
  return nullptr;
}

inline bool cardHasName(const std::vector<VarInfo>& card, const std::string& name) {
  return cardFind(card, name) != nullptr;
}

// P1-A3 (W1-D3 / WP-14): the `save FILE.dta` metadata sidecar.
//
// `export-dta --in-meta` is the ONLY channel by which a declared Stata storage
// type, display format, variable label or value-label set can reach the writer
// (IDIOMS.md §`import-dta`/`export-dta` round trip; src/io/dta_writer.h:80-88):
// the hbin the session serializes carries f64/factor, a name and a format
// string, and nothing else. A `save x.dta` that shelled out to a BARE
// `export-dta` would therefore write a file whose VALUES are right and whose
// entire label/type layer is silently gone — the downgrade class Doctrine I-3
// bars, and the exact reason this verb's `.dta` arm is a sidecar write rather
// than a one-line exec.
//
// The sidecar is built from the LIVE session maps (varMeta / varLblname /
// labelDefs / dataLabel / characteristics — what applyDtaMeta() fills on
// `use FILE.dta`, and what every in-session `label`/`format` verb then updates
// for the first three; dataLabel/characteristics have no in-session mutator
// yet, so they carry through unchanged from the source .dta), so what
// round-trips is the session's CURRENT metadata, never the source file's
// original.
//
// Two filters, both correctness rather than taste:
//   · only columns present in the live card get an entry. `session.varMeta`
//     keeps entries for dropped variables, and a hint naming a column the Frame
//     does not have is one the writer silently never matches.
//   · `value_label_name` is emitted only for a NON-factor column whose set is
//     actually defined in `labelDefs`. The writer THROWS on both other cases by
//     design (a name with no table; a value label on a string column —
//     dta_writer.cpp:352-360, and rightly: Stata attaches value labels to
//     numeric variables only), so passing them through would turn a perfectly
//     legal session (`tostring` a labeled numeric, `label drop`) into a failed
//     save.
//
// `data_label` and `characteristics` (FIX:PIET-SAVE-DTA-CHARS-CARRIAGE): carried
// the same way as the label/format layer above — from session.dataLabel /
// session.characteristics, which applyDtaMeta() (handlers.cpp) now populates on
// `use A.dta`. characteristics is emitted VERBATIM, unlike the per-column
// filter above: the writer's own DtaWriteChar comment (src/io/dta_writer.h)
// is explicit that chars are never validated against the live column list,
// because Stata itself keeps a variable's chars after a later `drop` — so
// filtering here would be a policy this codebase has already decided against.
//
// Returns the sidecar path, or "" when the session declares nothing at all
// (then the caller runs `export-dta` without `--in-meta` and the writer's own
// narrowest-lossless scan decides — the documented fallback, value-identical
// either way). A genuine write failure sets `errOut` instead: falling back to
// the scan there would silently drop the labels this whole function exists to
// carry.
inline std::string writeDtaMetaSidecar(SessionState& session, const std::vector<VarInfo>& card,
                                std::string& errOut) {
  nlohmann::ordered_json vars = nlohmann::ordered_json::array();
  std::set<std::string> referencedSets;
  for (const auto& info : card) {
    std::string label, format, stataType;
    auto mit = session.varMeta.find(info.name);
    if (mit != session.varMeta.end()) {
      if (mit->second.label.has_value()) label = *mit->second.label;
      if (mit->second.format.has_value()) format = *mit->second.format;
      if (mit->second.stataType.has_value()) stataType = *mit->second.stataType;
    }
    // BUG-295: an in-session `gen`/`egen` with no declared type is float by
    // Stata's own default. Numeric columns only — PIET carries strings as
    // `factor`, and gen's string default is str#, not float, so a factor
    // column stays with the writer's scan.
    if (stataType.empty() && info.dtype != "factor" &&
        session.implicitFloatVars.count(info.name) != 0) {
      stataType = "float";
    }
    std::string lblSet;
    auto lit = session.varLblname.find(info.name);
    if (lit != session.varLblname.end() && info.dtype != "factor" &&
        session.labelDefs.count(lit->second) != 0) {
      lblSet = lit->second;
    }
    // Nothing declared for this column: leave it out entirely so the writer
    // takes the hbin column's own format and scans for the narrowest lossless
    // type. An entry carrying only a name would say the same thing at the cost
    // of a bigger sidecar.
    if (label.empty() && format.empty() && stataType.empty() && lblSet.empty()) continue;
    nlohmann::ordered_json v;
    v["name"] = info.name;
    if (!label.empty()) v["label"] = label;
    if (!format.empty()) v["format"] = format;
    if (!stataType.empty()) v["stata_type"] = stataType;
    if (!lblSet.empty()) {
      v["value_label_name"] = lblSet;
      referencedSets.insert(lblSet);
    }
    vars.push_back(std::move(v));
  }

  // Only the sets an emitted column actually points at — the writer's rule runs
  // the other way (a NAME with no table is fatal), so a tight table is free.
  nlohmann::ordered_json valueLabels = nlohmann::ordered_json::object();
  for (const auto& setName : referencedSets) {
    nlohmann::ordered_json codes = nlohmann::ordered_json::object();
    for (const auto& [code, text] : session.labelDefs.at(setName)) codes[code] = text;
    valueLabels[setName] = std::move(codes);
  }

  // Verbatim carry, no per-column filter (see doc comment above): whatever
  // `use` captured (or a later `char`/`label data` command changed) round-trips
  // as-is.
  nlohmann::ordered_json chars = nlohmann::ordered_json::array();
  for (const auto& c : session.characteristics) {
    chars.push_back({{"varname", c.varname}, {"charname", c.charname}, {"value", c.value}});
  }

  if (vars.empty() && valueLabels.empty() && chars.empty() && session.dataLabel.empty()) {
    return std::string();
  }

  nlohmann::ordered_json meta;
  if (!session.dataLabel.empty()) meta["data_label"] = session.dataLabel;
  meta["variables"] = std::move(vars);
  meta["value_labels"] = std::move(valueLabels);
  if (!chars.empty()) meta["characteristics"] = std::move(chars);

  const std::string path = session.workdir() + "/save_meta" + scratchSeqSuffix() + ".json";
  registerScratchMint(path);  // BUG-389
  {
    std::ofstream f(cecon::to_native_path(path), std::ios::binary | std::ios::trunc);
    if (!f) {
      errOut = "cannot write the .dta metadata sidecar " + path;
      return std::string();
    }
    f << meta.dump();
    f.flush();
    if (!f) {
      errOut = "failed writing the .dta metadata sidecar " + path;
      std::error_code ec;
      std::filesystem::remove(cecon::to_native_path(path), ec);
      return std::string();
    }
  }
  return path;
}

}}}  // namespace piet::handlers::dv_detail

// R2: cross-TU declarations for the four duplicates workers. In the single-TU
// era they were "declared" by definition order (workers above the router); the
// cut put the router in TU 2 and the workers in TU 1, so the visibility that
// file order used to provide has to be said out loud.
namespace piet::handlers {
using ojson = nlohmann::ordered_json;
ojson runDuplicatesReport(const std::optional<std::vector<std::string>>& varlist,
                          SessionState& session, const std::optional<std::string>& ifExpr,
                          const std::optional<std::string>& inRange);
ojson runDuplicatesDrop(const std::optional<std::vector<std::string>>& varlist,
                        SessionState& session, const std::optional<std::string>& ifExpr,
                        const std::optional<std::string>& inRange);
ojson runDuplicatesTag(const std::optional<std::vector<std::string>>& varlist,
                       const std::string& genvar, SessionState& session,
                       const std::optional<std::string>& ifExpr,
                       const std::optional<std::string>& inRange);
ojson runDuplicatesList(const std::optional<std::vector<std::string>>& varlist,
                        SessionState& session, const std::optional<std::string>& ifExpr,
                        const std::optional<std::string>& inRange);
}  // namespace piet::handlers

