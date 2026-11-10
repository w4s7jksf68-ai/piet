// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command support.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // BUG-540: stataMissingWithKind — `.a`-`.z` identity in matrix literals
#include "piet/eval/macros.h"
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/program.h"  // BUG-922: program::restorePreservesDeeperThan is defined here
#include "piet/serset.h"  // BUG-1461: serset::storageType, signed into `datasignature`
#include "piet/strmutate.h"  // BUG-532: needsStringCount, the string-condition test under by:
#include "piet/stata_format.h"  // eb-order T-P1: `matrix list ..., format()`
#include "piet/strutil.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path / env_utf8 (BUG-050/051)
#include "data/missing.h"   // Stata `.` / `.a`-`.z` kinds + stata_cmp (shared with the engine)
#include "core/stata_arith.h"  // `^` with StataBE's bits (frame.cpp's)

#if defined(__APPLE__)
#include <sys/clonefile.h>  // Q1 (gate ③): APFS copy-on-write snapshot for preserve
#endif  // BUG-1469: the vocabularies below sat inside it, undeclared off macOS

#include "command_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace command_detail {}
using namespace command_detail;
using namespace session_vocab;

namespace command_detail {

// A5-04: forward to the shared strutil copy (piet/strutil.h) — kept as thin
// wrappers (not removed) so this file's many trim(...)/lower(...) call
// sites didn't need touching. Pure dedup, no behavior change.
std::string trim(const std::string& s) { return strutil::trim(s); }

std::string lower(std::string s) { return strutil::lower(std::move(s)); }

ojson engineErrorBlock(const std::string& message, const std::string& code) {
  return eval::engineError(message, code);
}

// B5-D14 (BUG-454): an error whose Stata rc is KNOWN but which coarseRc's
// message rules cannot recognise -- same shape as handlers_aer_util.cpp's
// utilErrorRc. `reason` is the only channel carrying an exact rc (BUG-187)
// and it outranks the coarse mapping.
ojson engineErrorBlockRc(const std::string& message, int rc) {
  return eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                           "r(" + std::to_string(rc) + ")");
}

// BUG-038(a): forwards to the shared mapping point (piet::cobraErrorMessage,
// cobra_runner.h) — kept as a thin wrapper (not removed) so this file's
// call sites didn't need touching. Pure dedup, no behavior change here.
std::string cobraErrorStr(const CobraError& ce) { return cobraErrorMessage(ce); }

std::string nextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

// Q1 (gate ③): snapshot an on-disk dataset file to `dst`. On APFS a copy-on-
// write clone is effectively free (the ~164MB preserve byte-copy is ~0.6s on
// the reference corpus), so attempt clonefile FIRST and fall back to the
// verbatim byte-copy on ANY failure — non-APFS / cross-volume / a dst-exists
// race, or any non-Apple build (Linux/Windows), where the #if simply compiles
// to the byte-copy alone. The fallback is byte-for-byte the pre-existing copy,
// so behavior is identical everywhere the clone can't be taken. Mirrors the
// engine's own clone-first-fallback (src/data/frame.cpp clone_file, used by
// patch_hbin). clonefile refuses to overwrite an existing dst, so unlink
// first; `dst` is a fresh nextPath() here, so that removal is normally a
// no-op and never touches a live file.
// BUG-124: returns false (and removes any partial dst) when the byte-copy
// fallback fails — an unchecked ENOSPC here left a TORN preserve snapshot
// that a later restore would load. The clonefile path is all-or-nothing.
bool cloneOrCopyFile(const std::string& src, const std::string& dst) {
#if defined(__APPLE__)
  // path::c_str() is const char* on POSIX (Apple); keep the paths alive across
  // the clonefile call so the pointers don't dangle.
  std::filesystem::path nsrc = cecon::to_native_path(src);
  std::filesystem::path ndst = cecon::to_native_path(dst);
  cecon::native_unlink(dst);
  if (::clonefile(nsrc.c_str(), ndst.c_str(), 0) == 0) return true;
  // clonefile failed (non-APFS FS, cross-device, EEXIST race, ...) -> byte-copy.
#endif
  std::ifstream in(cecon::to_native_path(src), std::ios::binary);
  std::ofstream out(cecon::to_native_path(dst), std::ios::binary);
  if (!in || !out) return false;
  // A11 (BUG-138/L5-IOSAVE class sweep): this fallback used to be
  // `out << in.rdbuf()` — libc++ implements that as a byte-at-a-time
  // sgetc/sputc virtual-call loop, ~5.5 ns/byte and *user*-CPU bound, which
  // measured 1.20-1.30 s on the 164 MB reference corpus against 0.35 s for the
  // 1 MB buffered loop below (3.5×, sha256-identical output — see
  // internal capture). Only the cross-volume
  // / non-APFS preserve snapshot reaches here (the clone above takes the
  // same-volume case), so this is the slowest path getting the 3.5×.
  {
    std::vector<char> buf(1 << 20);
    while (in) {
      in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
      std::streamsize got = in.gcount();
      if (got > 0) out.write(buf.data(), got);
      if (!out) break;
    }
  }
  out.flush();
  if (!out || in.bad()) {
    out.close();
    std::error_code rmEc;
    std::filesystem::remove(cecon::to_native_path(dst), rmEc);
    return false;
  }
  return true;
}

// BS-001: `gsort` (real cobra verb, cmds_data.cpp's cmd_gsort) prints a
// plain-text status line to STDERR and JSON only on error — runCobraJson()'s
// "parse stdout as JSON" contract doesn't fit, so runSort (below) calls the
// raw runCobra() and builds the CobraError manually from its stderr. File-
// local copy of handlers_data_verbs.cpp's identically-named helper (this
// codebase's established per-TU convention — see this file's own trim()/
// lower() forwarding-wrapper note above) rather than a new shared header.
CobraError errorFromRaw(const CobraRunResult& r) {
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

// fix/verb-overhead-sweep: name-only lookup over the resident shadow card
// (session.shadowVarlistIfCurrent()) — the same mechanism fix/sort-validate-
// lazy introduced for runSort below, reused here by runDrop/runKeep so
// neither needs session.datastore() (a full mmap + header parse of however
// many MB the current hbin is) just to answer "does column X exist". Plain
// linear scan over VarInfo entries, mirroring Datastore::has()'s own O(cols)
// name lookup (cecon::Dataset::has) — nothing this file does needs it to be
// more than that.
// C4-L2A sibling (D1 card plane): the same card lookup handing back the ENTRY
// rather than a yes/no — runConfirm needs dtype alongside the name. File-local
// twin of handlers_data_verbs.cpp's identically-named S4a helper (the
// established per-TU convention — see that file's own note); cardHasName
// forwards to it rather than keeping a second copy of the scan.
const VarInfo* cardFind(const std::vector<VarInfo>& card, const std::string& name) {
  for (const auto& info : card) {
    if (info.name == name) return &info;
  }
  return nullptr;
}

bool cardHasName(const std::vector<VarInfo>& card, const std::string& name) {
  return cardFind(card, name) != nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------

}  // namespace piet::handlers
