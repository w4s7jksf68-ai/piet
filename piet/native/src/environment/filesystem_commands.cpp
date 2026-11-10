// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Environment commands.
// mkdir, rmdir and erase / rm: one-level directory create/remove and file delete.

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

#include "engine/command_support.h"
#include "estimation/estimation_options.h"
#include "piet/handlers/session_commands.h"
#include "piet/user_paths.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace command_detail {}
using namespace command_detail;
using namespace session_vocab;

namespace command_detail {
std::string joinSpaceLocal(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += " ";
    s += v[i];
  }
  return s;
}

// Same quote-aware first-argument extraction as firstArgAfter, but
// anchored to "whatever the line's own first token is" rather than one
// specific literal word — lets a single handler (runErase) serve more than
// one Stata command spelling for the SAME command. BS-045 (BETA campaign
// C4): `rm` is a genuine Stata BUILT-IN synonym of `erase` (live-probed
// `which rm` -> "built-in command: rm", not an invented alias), so
// runErase needs to extract its path argument correctly regardless of
// whether the raw line says "erase ..." or "rm ...".
std::string firstArgAfterCmd(const std::string& raw) {
  std::string t = trim(raw);
  size_t sp = t.find_first_of(" \t");
  if (sp == std::string::npos) return "";
  std::string rest = trim(t.substr(sp));
  static const std::regex reArg(R"re(^("([^"]+)"|'([^']+)'|(\S+)))re");
  std::smatch m;
  if (std::regex_search(rest, m, reArg)) {
    return m[2].matched ? m[2].str() : (m[3].matched ? m[3].str() : m[4].str());
  }
  return "";
}
}  // namespace

// `mkdir DIRNAME [, public]` — create ONE directory level.
//
// BS-045 (BETA campaign C4): live-probed against StataNow 19.5 — real
// Stata's `mkdir` has NO parents-creation option at all (the pre-existing
// `,p` here was an INVENTED option; `help mkdir`'s only option is
// `public`). A missing parent directory is a genuine failure in real
// Stata, not silently bridged: `mkdir /tmp/nonexistent_parent/child`
// fails with the SAME message/rc as re-mkdir'ing an already-existing
// directory: "could not create directory <path>", rc=693 (oracle-pinned
// for the already-exists case; live-probed for the missing-parent case
// too — identical wording both times). `,public` makes the new directory
// world-readable+executable (chmod 0755); omitting it leaves whatever the
// platform's own umask-derived create_directory result is. Unknown
// options are rejected via the shared net (A2 idiom), matching live-probed
// Stata (`mkdir DIR, bogusopt` -> "option bogusopt not allowed", rc=198).
ojson runMkdir(const std::string& raw, const std::vector<std::string>& varlist,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
               SessionState& session) {
  std::string path = trim(firstArgAfter(raw, "mkdir"));
  if (path.empty() && !varlist.empty()) path = joinSpaceLocal(varlist);
  if (path.empty()) return engineErrorBlock("mkdir: directory name required");

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kMkdirKnownOpts = kVocabMkdir.known;
  if (auto err = eval::rejectUnknownOptions(opts, kMkdirKnownOpts)) {
    return ojson(*err);
  }
  bool isPublic = false;
  for (const auto& [k, v] : opts) {
    if (k == "public" && v.isBool && v.boolValue) isPublic = true;
  }

  std::string resolved = resolveUserPath(session, path);
  namespace fs = std::filesystem;
  std::error_code ec;
  // create_directory (NOT create_directories): single level only, exactly
  // like real Stata's own mkdir — a missing parent must fail, not be
  // silently bridged. Returns false (with ec cleared, not an error) for
  // BOTH "already exists" and most creation failures; the missing-parent
  // case sets ec (ENOENT) on some platforms/libstdc++ versions and merely
  // returns false on others, so both `ec` and `!created` are treated as
  // the same real-Stata "could not create directory" outcome.
  bool created = fs::create_directory(cecon::to_native_path(resolved), ec);
  if (ec || !created) {
    return engineErrorBlock("could not create directory " + resolved);
  }
  if (isPublic) {
    fs::permissions(cecon::to_native_path(resolved),
                     fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                         fs::perms::others_read | fs::perms::others_exec,
                     fs::perm_options::replace, ec);
    // Best-effort: a chmod failure doesn't invalidate the directory that
    // was already successfully created above.
  }
  return ojson::array();  // Stata: silent on success
}

// `rmdir DIRNAME` — remove ONE empty directory.
//
// BUG-180 (P2-PREP): PIET had no `rmdir` at ALL — registry.cpp registered
// mkdir/erase/rm but never this, so the command fell through the whole
// routing table to the generic unsupported_command fallback. Measured
// consequence: the cross-engine benchmark harness clears its scratch dirs
// with `capture quietly rmdir "$BENCH_ROOT/dir_`rep'"` before each timed
// `mkdir`; under PIET that line did nothing, the directories from earlier
// runs survived, and every subsequent `mkdir` answered r(693) — the
// benchmark reported mkdir failing 10/10 for a reason that was not mkdir.
//
// Semantics live-probed against StataNow 19.5 this stint (not guessed):
//   rmdir "empty_dir"        -> rc 0, silent
//   rmdir "nonempty_dir"     -> "could not remove directory <path>", rc 693
//   rmdir "no_such_dir"      -> "could not remove directory <path>", rc 693
//   rmdir "some_file.txt"    -> "could not remove directory <path>", rc 693
//   rmdir                    -> "invalid syntax", rc 198
//   rmdir DIR, anyopt        -> "options not allowed", rc 101
// Every failure mode shares ONE message and ONE rc — hence the single
// `fail` lambda below rather than a per-case wording that real Stata does
// not actually distinguish.
//
// Two deliberate notes:
//  - The is_directory() guard is load-bearing, not defensive padding:
//    std::filesystem::remove() would happily delete a plain FILE, so
//    without it `rmdir notes.txt` would silently destroy a file that real
//    Stata refuses to touch. remove() on a NON-EMPTY directory fails on
//    its own (ENOTEMPTY), which is the "empty only" half.
//  - Options: real Stata answers r(101), a bucket this codebase has
//    nowhere else. Routed through the shared reject net instead (r(198),
//    "option `X' not allowed"), which is the uniform idiom every other
//    arm here uses — a deliberate, reported deviation rather than a new
//    one-command rc bucket.
ojson runRmdir(const std::string& raw, const std::vector<std::string>& varlist,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
               SessionState& session) {
  std::string path = trim(firstArgAfterCmd(raw));
  if (path.empty() && !varlist.empty()) path = joinSpaceLocal(varlist);
  if (path.empty()) return engineErrorBlock("rmdir: directory name required", "syntax_error");

  if (auto err = eval::rejectUnknownOptions(opts, {})) {
    return ojson(*err);
  }

  std::string resolved = resolveUserPath(session, path);
  namespace fs = std::filesystem;
  auto native = cecon::to_native_path(resolved);
  // "could not remove directory <path>" is Stata's answer to ALL of:
  // missing path, path is a file, directory not empty, permission denied.
  auto fail = [&resolved]() {
    return engineErrorBlock("could not remove directory " + resolved);
  };

  std::error_code ec;
  if (!fs::is_directory(native, ec) || ec) return fail();
  // remove() (NOT remove_all()): single EMPTY directory only, exactly like
  // real Stata — a non-empty directory must fail, never be recursively
  // erased. Returns false when nothing was removed; ec carries ENOTEMPTY.
  if (!fs::remove(native, ec) || ec) return fail();
  return ojson::array();  // Stata: silent on success
}

// `erase FILE` / `rm FILE` — delete a file. BS-045 (BETA campaign C4):
// `rm` is a genuine Stata BUILT-IN synonym of `erase` (live-probed `which
// rm` -> "built-in command: rm"), routed to this SAME handler by the
// dispatcher; firstArgAfterCmd (unlike the old firstArgAfter(raw,
// "erase")) doesn't hardcode the command word, so it extracts the path
// correctly for either spelling.
ojson runErase(const std::string& raw, const std::vector<std::string>& varlist,
               SessionState& session) {
  std::string path = trim(firstArgAfterCmd(raw));
  if (path.empty() && !varlist.empty()) path = joinSpaceLocal(varlist);
  if (path.empty()) return engineErrorBlock("erase: filename required");

  std::string resolved = resolveUserPath(session, path);
  namespace fs = std::filesystem;
  if (!fs::exists(cecon::to_native_path(resolved))) {
    // StataBE: `file sub/nofile.txt not found` r(601), the name as typed (BUG-410).
    return engineErrorBlockRc("file " + typedUserPath(path) + " not found", 601);
  }
  std::error_code ec;
  // BUG-1461: erase removes files only. fs::remove would take an empty
  // directory at rc 0; Stata refuses a directory as a file it cannot erase
  // (StataBE 19.5: "file DIR is read-only; cannot be modified or erased",
  // r(608)), and rmdir is the command that removes one.
  if (fs::is_directory(cecon::to_native_path(resolved), ec))
    return engineErrorBlockRc("file " + path + " is read-only; cannot be modified or erased", 608);
  fs::remove(cecon::to_native_path(resolved), ec);
  if (ec) return engineErrorBlock("erase: " + ec.message());
  return ojson::array();  // Stata: silent on success
}


}  // namespace piet::handlers
