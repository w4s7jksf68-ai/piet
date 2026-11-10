// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// R2 (giant-file split, 2026-09-01): the shared file-local helpers of the
// former 5,589-line data-verb source file, promoted VERBATIM (modulo
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
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
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
#include "fileio/xlsx.h"   // Stata `.` / `.a`-`.z` kinds — CSV writes the extended code
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)
#include "io/columnar.h"  // cecon::hbin_level_json_text: .dta label text (BUG-1372)

namespace piet { namespace handlers { namespace dv_detail {
using ojson = nlohmann::ordered_json;  // the TU-local alias the helpers were written against

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `trim(...)`/`lower(...)`
// call sites didn't need touching. Pure dedup, no behavior change.
inline std::string trim(const std::string& s) { return strutil::trim(s); }

inline ojson engineErrorBlock(const std::string& message, const std::string& code = "engine_error") {
  return eval::engineError(message, code);
}

// A refusal whose Stata rc the oracle pins: the block carries `r(NNN)` in its
// reason, which eval::coarseRc reads before any message-based mapping.
inline ojson rcErrorBlock(const std::string& message, int rc,
                          const std::string& code = "engine_error") {
  return ojson(eval::errorBlock(code, message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
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
// — mirrors fileio/dataset_use.cpp's own file-local ext() (each TU keeps its own copy
// per this codebase's established convention, see strutil.h's header note).
// Only the last path component has one: `save sub.d/q2` writes sub.d/q2.dta
// (StataBE 19.5), so a dot before the last '/' is part of a directory name.
inline std::string extOf(const std::string& path) {
  auto pos = path.find_last_of('.');
  if (pos == std::string::npos) return "";
  const auto slash = path.find_last_of('/');
  if (slash != std::string::npos && pos < slash) return "";
  return lower(path.substr(pos));
}

// fix/verb-overhead-sweep: name-only lookup over the resident shadow card
// (session.shadowVarlistIfCurrent()) — the same mechanism fix/sort-validate-
// lazy introduced for observation_selection.cpp's runSort, reused here by runGsort/
// runOrder so neither needs session.datastore() (a full mmap + header parse
// of however many MB the current hbin is) just to answer "does column X
// exist". File-local copy of observation_selection.cpp's identically-named helper
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

}}}  // namespace piet::handlers::dv_detail
