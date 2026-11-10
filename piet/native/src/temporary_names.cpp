// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Temporary names.

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

ojson runTempfile(const std::vector<std::string>& names, SessionState& session) {
  if (!session.runTempdir.has_value()) {
    std::string dir = session.workdir() + "/tmp_" + std::to_string(std::time(nullptr));
    // BUG-111(a): create the dir via the portable API on BOTH platforms. The old
    // POSIX path shelled out `std::system("mkdir -p '<dir>'")` and DISCARDED the
    // return code — a workdir/TMPDIR containing an apostrophe (macOS volume names
    // like `Nam's MacBook`) broke the single-quoting, mkdir silently failed, and
    // every subsequent tempfile save then errored cryptically. create_directories
    // handles any path; surface an honest error on failure instead of swallowing it.
    std::error_code mkec;
    std::filesystem::create_directories(cecon::to_native_path(dir), mkec);
    if (mkec)
      return ojson(eval::engineError("tempfile: could not create scratch directory '" + dir +
                                     "': " + mkec.message()));
    session.runTempdir = dir;
  }
  for (const auto& name : names) {
    // BUG-111(b): per-CALL unique filename (Stata mints a fresh file each call) —
    // the macro `name` binds to this path, so scripts using `\`name'` are unchanged.
    std::string path = *session.runTempdir + "/" + name + "_" +
                       std::to_string(session.tempSeq++) + ".tmp";
    // BUG-360: record WHO minted it. A tempfile's lifetime in Stata is the
    // program or do-file that created it, not the run — so the returning
    // program needs to know which of these are its own (program.cpp's
    // ScopeGuard). Level 0 = the top-level run, still reaped by doexec's
    // TempdirReapGuard at the end of the run, which is the same thing Stata
    // does for a do-file-level tempfile. BUG-922: tagged with the preserve
    // level, so a nested do/run file's tempfiles die when it concludes too
    // (program::concludeLevel).
    session.mintedTempfiles.emplace_back(session.preserveLevel(), path);
    session.eval.locals.set(name, path);
  }
  return ojson::array();
}

ojson runTempname(const std::vector<std::string>& names, SessionState& session) {
  for (const auto& name : names) {
    // BUG-111(b): epoch-SECONDS collided for two `tempname` calls in the same second;
    // use the per-session monotonic counter so each call's basename is unique.
    std::string basename = "__" + name + "_" + std::to_string(session.tempSeq++);
    session.eval.locals.set(name, basename);
    // BUG-922: dropped (scalar or matrix of this name) when the minting
    // program or nested do/run file concludes -- program::concludeLevel
    // (w2ad_prog_preserve_nest pn_tv_do_gone: scalar=111).
    session.mintedTempnames.emplace_back(session.preserveLevel(), basename);
  }
  return ojson::array();
}

// BS-037: tempvar -- sibling of tempfile/tempname above, previously ZERO
// implementation (the raw token "tempvar" was still listed in the
// tokenizer's early macro-statement keyword net, eval/tokenizer.cpp:80,
// so `tempvar tv` tripped the generic "does not expand macros" tripwire
// before ever reaching dispatch — see dispatcher.cpp's U4 block, which now
// intercepts tempfile/tempname/tempvar identically, BEFORE tokenize() ever
// sees the line, same as it already did for tempfile/tempname). Real
// Stata: mints a scratch-safe NEW variable name (the column itself is only
// created later by e.g. `gen `tv''`) and auto-drops it at scope end — since
// BUG-922 a program or nested do/run file drops its own tempvars when it
// concludes (mintedTempvars); a top-level tempvar still outlives the run.
// Unlike tempname's arbitrary "__NAME_TIMESTAMP" basename (fine for a FILE
// path only, never validated as an identifier), a tempvar's minted name
// must be a LEGAL Stata variable name AND must not collide with any column
// already in the current dataset — real Stata's own pattern is
// `__000001`, `__000002`, ...; mirrored here via a per-process monotonic
// counter, skipping forward past any collision against the loaded dataset
// (if any — tempvar is legal even before `use`/`sysuse`).
ojson runTempvar(const std::vector<std::string>& names, SessionState& session) {
  static std::atomic<uint64_t> counter{0};
  Datastore* ds = session.datastore();
  for (const auto& name : names) {
    std::string minted;
    for (;;) {
      uint64_t n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
      char buf[16];
      std::snprintf(buf, sizeof(buf), "__%06llu", static_cast<unsigned long long>(n));
      minted = buf;
      if (ds == nullptr || !ds->has(minted)) break;
    }
    session.eval.locals.set(name, minted);
    // BUG-922: dropped when the minting program or nested do/run file
    // concludes -- program::concludeLevel (pn_tv_do_gone: var=111).
    session.mintedTempvars.emplace_back(session.preserveLevel(), minted);
  }
  return ojson::array();
}

// ---------------------------------------------------------------------------
// levelsof


}  // namespace piet::handlers
