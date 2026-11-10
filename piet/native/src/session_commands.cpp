// SPDX-License-Identifier: GPL-3.0-or-later
#include "piet/bcal.h"
// Copyright (C) 2026 M.K. Kim
// Session commands.

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

ojson runClear(const std::string& subIn, SessionState& session) {
  std::string sub = lower(trim(subIn));

  auto silentEcho = []() {
    ojson out;
    out["type"] = "echo";
    out["text"] = "";
    return out;
  };

  if (sub.empty()) {
    // Bare `clear`: dataset (+ its label defs) ONLY.
    session.clearData();
    session.clearMetadata();
    return silentEcho();
  }

  if (sub == "all" || sub == "*") {
    session.clearData();
    session.clearMetadata();
    // Macros are NOT cleared: clear.ado's `all` branch never runs `macro drop`
    // (it drops data, frames, labels, matrices, scalars, constraints, files,
    // programs, timers, Mata ...), and StataBE keeps both globals and locals
    // across it. 007's `Wave 1 cleaning.do` opens with `clear all` and then reads
    // `$data`, a global its settings file set one level up.
    session.eval.scalars.clear();
    session.eval.rValues = eval::OrderedFloatDict();
    session.eval.rStrings.clear();
    session.eval.eScalars = eval::OrderedFloatDict();
    session.eval.eStrings.clear();
    session.eval.matrices.clear();
    session.eval.mataMatrices.clear();  // clear.ado's `all` branch runs `mata: mata clear`
    session.eval.eMatrices.clear();
    session.eval.rMatrices.clear();
    session.lastModel_reset();
    session.panelvar.reset();
    session.timevar.reset();
    session.programs.clear();       // ado's `program drop _all` under "all".
    ++session.programsEpoch;
    session.returnStaging.clear();  // ado's `_return drop _all` under "all".
    // BUG-392 (2026-09-02): the rest of clear.ado's `all` branch — `_est drop _all`,
    // `graph drop _all`, `postutil clear`, `file close _all`, `frames reset`, the
    // preserve stack — plus the session's own `use` memos and the engine's stashes
    // (BUG-390), none of which any earlier `clear all` touched.
    // BUG-1079: `postutil clear` CLOSES each postfile, so what was posted is on
    // disk afterwards (StataBE 19.5, 2026-10-28: three posted rows, then `clear
    // all`, then `describe using` reports N 3 -- evidence/t1_clearall_postfile).
    // The postfiles are closed through runPostclose first; a failed close does
    // not stop `clear all`, exactly as Stata's own clear.ado runs it.
    {
      std::vector<std::string> open;
      for (const auto& kv : session.postfiles) open.push_back(kv.first);
      for (const auto& h : open) (void)runPostclose(h, session);
    }
    session.clearAllSessionObjects();
    // BUG-1461: clear.ado's `all` branch also runs `timer clear` (StataBE 19.5:
    // after `clear all` a stopped timer lists nothing and r(nt#) is missing).
    session.timers.fill(TimerSlot{});
    return silentEcho();
  }

  if (sub == "results" || sub == "matrix") {
    // Shared ado branch: r()/e()/s() results + return-staging.
    session.eval.rValues = eval::OrderedFloatDict();
    session.eval.rStrings.clear();
    session.eval.eScalars = eval::OrderedFloatDict();
    session.eval.eStrings.clear();
    session.eval.eMatrices.clear();
    session.eval.rMatrices.clear();
    session.lastModel_reset();
    session.returnStaging.clear();
    if (sub == "matrix") {
      // `clear matrix` additionally: `matrix drop _all` + `_est drop _all`,
      // which takes the stores' `_est_*` variables too (BUG-1212).
      session.eval.matrices.clear();
      clearStoredEstimates(session);
    }
    return silentEcho();
  }

  if (sub == "programs") {
    session.programs.clear();
    ++session.programsEpoch;
    return silentEcho();
  }

  // ml-getmata T4: Mata now HAS a store (session.eval.mataMatrices, the M0
  // subset's namespace), so `clear mata` is a real clear and no longer one of
  // the scoped no-ops below. It touches nothing else -- data, Stata matrices
  // and results all survive it, which selftest's C4/BS-044 leg (d) pins.
  if (sub == "mata") {
    session.eval.mataMatrices.clear();
    return silentEcho();
  }

  static const std::unordered_set<std::string> kNoStoreSubs = {
      "ado", "rngstream", "rngstreams", "frames", "collect"};
  if (kNoStoreSubs.count(sub)) {
    ojson out;
    out["type"] = "echo";
    out["text"] = "(clear " + sub + ": no-op -- this build of PIET has no " + sub + " store)";
    return out;
  }

  return engineErrorBlock("clear: '" + sub + "' not allowed", "syntax_error");
}

ojson runSetObs(const std::string& nArg, SessionState& session) {
  // BUG-1463: Stata takes the count as typed digits (`05`, `+5` are 5) between
  // the current _N and 2,147,483,619. Every other argument -- below _N, negative,
  // `8.0`, `3.5`, `1e9`, `abc` -- is one refusal, r(198), with its range line
  // (StataBE 19.5: `set obs 3` on 5 rows). This used to answer "cannot reduce
  // the number of observations" and the parse failures at rc 1, and to truncate
  // `8.5` to 8.
  Datastore* dsHave = session.currentHbin().has_value() ? session.datastore() : nullptr;
  const long long curN = dsHave ? static_cast<long long>(dsHave->nRows()) : 0;
  const std::string typed = trim(nArg);
  static const std::regex reCount(R"(^\+?\d{1,10}$)");
  const long long nTarget = std::regex_match(typed, reCount) ? std::stoll(typed) : -1;
  if (nTarget < curN || nTarget > 2147483619LL) {
    std::string lo = std::to_string(curN);
    for (int at = static_cast<int>(lo.size()) - 3; at > 0; at -= 3) lo.insert(static_cast<size_t>(at), ",");
    return engineErrorBlockRc("observation number out of range\n    Observation number must be "
                              "between " + lo + " and 2,147,483,619.  (Observation numbers are "
                              "typed without commas.)", 198);
  }

  std::string out = nextPath(session.workdir(), "setobs");
  // A dataset with NO variables (`clear` + `set obs 0`, or `set obs 3` on an
  // empty session) has nothing to pad, and the pad-and-append path below would
  // hand cobra a header-only CSV with no columns. Growing it is the same act as
  // building a blank frame from nothing, so it takes the branch that does that.
  Datastore* dsNow = session.currentHbin().has_value() ? session.datastore() : nullptr;
  const bool growNoVars =
      dsNow != nullptr && dsNow->names().empty() && nTarget > dsNow->nRows();
  if (!session.currentHbin().has_value() || growNoVars) {
    // No data in memory -> N blank observations, 0 variables. Build via a
    // throwaway 1-column CSV (a dummy value repeated nTarget times) then
    // `cobra eval --drop` the dummy column, landing on {n_rows:N,
    // columns:[]} — bit-identical to the oracle's _write_hbin(out,[],[],n).
    std::string csvPath = session.workdir() + "/setobs_seed.csv";
    registerScratchMint(csvPath);  // BUG-389
    {
      std::ofstream csv(cecon::to_native_path(csvPath));
      csv << "_z\n";
      for (long long i = 0; i < nTarget; ++i) csv << "1\n";
      // BUG-124 sibling: surface a silently-failed CSV write (ENOSPC) here
      // instead of letting convert build a short dataset from it.
      csv.flush();
      if (!csv) {
        cecon::native_unlink(csvPath);
        return engineErrorBlock("set obs: failed writing scratch CSV (disk full or I/O error)");
      }
    }
    std::string seedHbin = session.workdir() + "/setobs_seed.hbin";
    registerScratchMint(seedHbin);  // BUG-389
    std::vector<std::string> convArgs = {"convert", "--data", csvPath, "--out", seedHbin,
                                          "--delim", ","};
    // BUG-124: runCobra does not throw — check the result (the old catch was
    // dead) and remove any torn partial before it can be load()ed/mmap'd.
    CobraRunResult convCr = runCobra(convArgs);
    if (!convCr.ok) {
      cecon::native_unlink(seedHbin);
      CobraError ce = errorFromRaw(convCr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    // FU-1252: `set obs 0` (and the empty dataset `generate`/`save, emptyok`
    // materialize after `clear`) has NO variables -- the seed's `_z` used to
    // stay behind as a variable (w2ar dx_save_079_emptyok k=0 N=0, log:4521).
    // `cobra eval --drop` on the 0-row seed is measured to leave 0 columns.
    {
      std::vector<std::string> dropArgs = {"eval", "--data", seedHbin, "--drop", "_z", "--out",
                                           out};
      try {
        runCobraJson(dropArgs);
      } catch (const CobraError& ce) {
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      session.load(out);
    }
    return mutationResult(session, "set_obs");
  }

  Datastore* ds = session.datastore();
  int64_t curRows = ds ? ds->nRows() : 0;
  if (nTarget == curRows) {
    return mutationResult(session, "set_obs");
  }

  // Extend: pad every existing column to nTarget rows with system-missing.
  // cobra has no direct "append N blank rows" verb; build a blank frame of
  // (nTarget - curRows) rows with the SAME column set (each column = "."),
  // then `cobra append` it onto the current dataset.
  //
  // BUG-697: this path never worked against the current engine, three ways:
  //  1. `cobra append --out` prints a plain-text notice on stderr and NOTHING on
  //     stdout (apps/cmds/cmds_data.cpp cmd_append), and this call went through
  //     runCobraJson, so every extension of a non-empty dataset -- `set obs 8`
  //     on 6 rows, svmat/getmata with more rows than _N -- died as "cobra
  //     produced non-JSON output", rc 1. It is a PIET call-site bug, not an
  //     engine one: the verb's contract is the file, as runVerb/runCobra use it
  //     everywhere else (handlers_aer_reshape.cpp's insobs, this file's
  //     line-level note "use runCobra (not runCobraJson ...)").
  //  2. The pad cells were BLANK. cobra's CSV reader skips a blank line, so on a
  //     one-variable dataset every pad row vanished ("6 master + 0 using") and
  //     _N silently stayed put -- hidden until now behind (1).
  //  3. A blank-or-`.` column in the pad CSV reads as NUMERIC, and append's
  //     unflagged default promotes a numeric/string clash to f64 -- a string
  //     variable would have come back as numbers. `--type-clash force` makes
  //     the pad cells missing OF THE MASTER'S TYPE (BUG-550's Stata rule), which
  //     is exactly "extend with missing".
  // Measured against the frozen engine: `.` cells + --type-clash force give
  // "6 master + 2 using = 8 rows", f64 stays f64, a factor stays a factor.
  int64_t padRows = nTarget - curRows;
  std::string padCsv = session.workdir() + "/setobs_pad.csv";
  registerScratchMint(padCsv);  // BUG-389
  {
    std::ofstream csv(cecon::to_native_path(padCsv));
    const auto& names = ds->names();
    for (size_t i = 0; i < names.size(); ++i) {
      if (i) csv << ",";
      csv << names[i];
    }
    csv << "\n";
    for (int64_t r = 0; r < padRows; ++r) {
      for (size_t i = 0; i < names.size(); ++i) {
        if (i) csv << ",";
        csv << ".";  // never blank: see BUG-697 (2) above
      }
      csv << "\n";
    }
    // BUG-124 sibling: flush + check — a silently short pad CSV appends fewer
    // blank rows than `set obs` promised.
    csv.flush();
    if (!csv) {
      cecon::native_unlink(padCsv);
      return engineErrorBlock("set obs: failed writing scratch CSV (disk full or I/O error)");
    }
  }
  std::vector<std::string> args = {"append",       "--data", *session.currentHbin(),
                                    "--using",      padCsv,   "--type-clash",
                                    "force",        "--out",  out};
  CobraRunResult appendCr = runCobra(args);
  if (!appendCr.ok) {
    CobraError ce = errorFromRaw(appendCr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  // BUG-124's torn-write guard, as at every other file-contract call site.
  if (!cecon::native_exists(out)) {
    return engineErrorBlock("set obs: failed writing output dataset (disk full or I/O error)");
  }
  session.load(out);
  // The count is asserted, not assumed: (2) above extended nothing at rc 0.
  if (Datastore* grown = session.datastore(); grown == nullptr || grown->nRows() != nTarget) {
    return engineErrorBlock("set obs: the dataset has " +
                            std::to_string(grown ? grown->nRows() : 0) +
                            " observations after extending, not " + std::to_string(nTarget));
  }
  return mutationResult(session, "set_obs");
}

ojson runCd(const std::string& dirArgIn, SessionState& session) {
  std::string dirArg = trim(dirArgIn);
  // dequote (routes_data.py's _dequote: strip matching outer quotes)
  if (dirArg.size() >= 2 && ((dirArg.front() == '"' && dirArg.back() == '"') ||
                             (dirArg.front() == '\'' && dirArg.back() == '\''))) {
    dirArg = dirArg.substr(1, dirArg.size() - 2);
  }
  // `~` expansion (os.path.expanduser) — HOME-relative only, matches the
  // common corpus usage; `~user` forms are not exercised.
  if (!dirArg.empty() && dirArg[0] == '~') {
    std::string home = cecon::env_utf8("HOME");
#ifdef _WIN32
    // Windows has no HOME by default; ~ resolves against USERPROFILE (matches
    // resolveUserPath's _WIN32 branch). POSIX path unchanged.
    if (home.empty()) home = cecon::env_utf8("USERPROFILE");
#endif
    if (!home.empty()) dirArg = home + dirArg.substr(1);
  }
  namespace fs = std::filesystem;
  fs::path p = cecon::to_native_path(dirArg);
  if (p.is_relative()) {
    std::string base = session.cwd.has_value() && !session.cwd->empty() ? *session.cwd
                        : fs::current_path().u8string();
    p = cecon::to_native_path(base) / p;
  }
  std::error_code ec;
  fs::path abs = fs::weakly_canonical(p, ec);
  if (ec) abs = p.lexically_normal();
  if (!fs::is_directory(abs)) {
    return engineErrorBlock("Directory not found: " + abs.u8string());
  }
  session.cwd = abs.u8string();
  session.eval.cwd = *session.cwd;
  // AER scope-2 B2-3: bofd()/dofb() resolve `<name>.stbcal` against the
  // session's working directory, and the expression surfaces that call them
  // (display / local = / gen) reach the loader through a process-level search
  // directory rather than a session handle -- macros.cpp's evalArith uses the
  // immutable process namespace. Keeping it in step HERE is what makes
  // `cd sub` + `bofd("cal", d)` find sub/cal.stbcal (measured: Stata finds a
  // calendar in the new cwd after cd).
  bcal::setSearchDir(*session.cwd);
  ojson out;
  out["type"] = "echo";
  out["text"] = abs.u8string();
  return out;
}

ojson runPwd(SessionState& session) {
  namespace fs = std::filesystem;
  std::string cwd = session.cwd.has_value() && !session.cwd->empty() ? *session.cwd
                     : fs::current_path().u8string();
  ojson out;
  out["type"] = "echo";
  out["text"] = cwd;
  return out;
}

// BUG-028 / BS-006: `log using FILE [, replace|append] [text|smcl]` really
// opens FILE now (SessionState::openLog) — every subsequent OUTERMOST
// dispatch() call's finalized blocks get teed to it (see dispatcher.cpp's
// dispatch()/dispatchImpl() split + SessionState::logTee's doc comments).
// `log close` flushes+closes; `log off`/`log on` pause/resume an already-open
// log (stream stays open, writes suspended while paused). Sub-keyword +
// `using` path extraction is UNCHANGED from the original BUG-028 shim
// (mirrors router/commands.py's parse: sub = first varlist token; path via a
// `using ("..."|'...'|\S+)` regex on the raw command text, resolved against
// session.cwd like shims.py's _resolve(session, path)) — everything below the
// path-resolution is new.
//
// Format, per the log help file ("Options for use with log"), identified
// against StataBE 19.5 captures: `text` and `smcl` decide the format when
// given; otherwise a ".smcl" file is SMCL, ".log" is text, and a name with no
// extension follows `set logtype` (not settable here, so its default, SMCL)
// and gets ".smcl" appended — ".log" for a text log. The SMCL writer is real
// (session_state.cpp, above openLog): the file starts "{smcl}", carries the
// header/footer blocks and tags every line, so a file that claims SMCL is SMCL
// — PIET's standing "never silently wrong" rule is kept by building the
// format instead of refusing it. Two documented deviations: the text format
// is PIET's own plain transcript (no Stata-style header/footer, unchanged),
// and any OTHER extension (".txt", ".tex", ...) without `text`/`smcl` stays
// text, as it has always been here, where StataBE would write SMCL.
//
// Mode: `replace` truncates; `append` (or bare with no existing file) opens
// in append mode — C++'s ios::app already creates a fresh file when none
// exists, matching Stata's own `append` semantics with no extra logic.
// Bare with an EXISTING file is Stata's real "file already exists" hard
// stop — kept honest (not silently defaulted either way) rather than
// guessing; the message text below deliberately contains "file" +
// "already exists" so errors.cpp's coarseRc() maps a `capture`-wrapped
// call's `_rc` to 602 (Stata's real code) via its existing engine_error/
// mentionsFileOrDir branch, with no new mapping rule needed there.
//
// Only one log open at a time: a second `log using` while one is already
// open is an honest reject too (matching real Stata — you must `log close`
// first). BS-117 (multi named-log bookkeeping / `name()`) stays post-beta/
// OUT; analysis.do and the eval golden fixtures never open more than one.

}  // namespace piet::handlers
