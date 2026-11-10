// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/program.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>   // BUG-360: ScopeGuard reaps this call's tempfiles
#include <regex>
#include <sstream>
#include <system_error>

#include "piet/cclass.h"  // BUG-1330: kVersionLocal
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"
#include "piet/eval/tokenizer.h"
#include "piet/datastore.h"
#include "piet/handlers/observations.h"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::to_native_path (BUG-360 reap)
#include "program_internal.h"

namespace piet::program {

using namespace program_detail;

namespace {

constexpr int kMaxRecursion = 100;

// ---------------------------------------------------------------------------
// P4: eclass/sclass support.
//
// ProgramDef (piet/session_state.h) carries only the pre-existing `rclass`
// bool — this stint's file manifest does NOT include session_state.h (or
// any other header), so the struct cannot grow an `eclass`/`sclass` field.
// Two reserved-string conventions stand in for the header change that would
// otherwise be needed, both confined to this translation unit's own state
// flow (captureProgams -> runProgram -> handleReturnStmt):
//
//   kProgClassSentinelPrefix — captureProgams() APPENDS one inert trailing
//     comment line to an eclass/sclass program's body (never prepended, so
//     no real body line's position/number shifts). runProgram() has full
//     access to progDef.body already, so it recovers the class from that
//     trailing line with no new struct field.
//
//   kProgClassMarkerKey — runProgram() stages the recovered class onto a
//     reserved-name hidden local (a leading space can never occur in a real
//     Stata local name; see reLocalDef in eval/macros.cpp) on the callee's
//     freshly-pushed scope dict. handleReturnStmt()'s ereturn/sreturn
//     honesty checks read it back while that scope is live; ScopeGuard's
//     restore (below) discards it for free on return, exactly like any
//     other local — this is the SAME save/restore shape the rclass
//     r()/e() flush already uses, just carried on session.eval.locals
//     instead of a dedicated field.
//
// kSClassKeyPrefix must stay byte-identical to eval/macros.cpp's own copy
// (no shared header for this one either) — see that file's note. Both
// pieces exist ONLY because sreturn-local values need a persistent string
// store and "no new session stores" (P4 task constraint) rules out adding
// a dedicated one; session.eval.rStrings already IS such a store (used by
// `return local`/r()), so sclass values live there under a prefixed key
// that can never collide with a real r()-name (those are captured by an
// identifier-only regex — see reRRefDelim/reRRef in eval/macros.cpp).
const std::string kProgClassSentinelPrefix = "* __piet_progclass__ ";
const std::string kProgClassMarkerKey = " __piet_progclass";
const std::string kSClassKeyPrefix = "\x01s:";



// `program [define] NAME [, OPTION]` header line.
// Matches: program define myname / program myname / program define myname,
// rclass / program myname , rclass
const std::regex& reProgHeader() {
  // AER-SCOPE1 bundle A: `prog` and `prog def NAME` are what four AER packages
  // write (180 occurrences), and a program header is matched HERE, in the
  // pre-tokenize capture pass, so a registry abbreviation would never be
  // consulted. `de`/`def` join `define` for the same reason.
  static const std::regex re(R"(^prog(?:ram)?\s+(?:de(?:f(?:ine)?)?\s+)?(\w+)(?:\s*,\s*(\S+))?)",
                              std::regex::icase);
  return re;
}

// `end` terminator (whole line, any case).
const std::regex& reEnd() {
  static const std::regex re(R"(^end\s*$)", std::regex::icase);
  return re;
}

// `[capture] program drop NAME` — the redefine-idiom drop that PRECEDES a
// `program define NAME` (see captureProgams doc comment).
const std::regex& reProgDrop() {
  // `capt prog drop mht_rw` is the corpus's own spelling of the redefine idiom;
  // capture's truncations were already accepted here, program's were not.
  static const std::regex re(
      R"(^(?:cap(?:t(?:u(?:r(?:e)?)?)?)?\s+)?prog(?:ram)?\s+drop\s+(\w+)\s*$)",
      std::regex::icase);
  return re;
}

}  // namespace

// BUG-342 — see program.h. The prefix is a Stata comment, so a stray sentinel
// that ever escaped the interception in doexec.cpp would be inert rather than
// an honest-looking command; and the reserved token can never collide with a
// user line, since preprocessDo has already stripped every real comment by the
// time captureProgams inserts these.
const std::string kProgInstallSentinelPrefix = "* __piet_proginstall__ ";

bool isProgInstallSentinel(const std::string& line, std::string* nameOut) {
  if (line.compare(0, kProgInstallSentinelPrefix.size(), kProgInstallSentinelPrefix) != 0)
    return false;
  if (nameOut) *nameOut = line.substr(kProgInstallSentinelPrefix.size());
  return true;
}

// ---------------------------------------------------------------------------
// Capture pass (runs in executeDoSource BEFORE brace aggregation)

std::vector<std::string> captureProgams(const std::vector<std::string>& lines, SessionState& session) {
  std::vector<std::string> out;
  size_t i = 0;
  size_t n = lines.size();

  while (i < n) {
    const std::string& line = lines[i];
    std::string stripped = trim(line);

    std::smatch m;
    if (std::regex_search(stripped, m, reProgHeader())) {
      std::string progName = m[1].str();

      // BS-014: `program dir` / `program list` / bare `program drop NAME`
      // are management/list forms, not definitions — reProgHeader()'s
      // optional `define` group means it ALSO (wrongly) matches these bare
      // forms, capturing the reserved subcommand word as if it were a NEW
      // program name, then the body-consuming loop below hunts for a
      // matching `end` that will never come — silently swallowing every
      // remaining line of the do-file as a bogus program body. Bare (no
      // explicit `define` keyword) dir/list/drop fall through untouched to
      // the normal per-line path at the bottom of this function instead
      // (dispatcher.cpp's own `program dir`/`drop` pre-pass handles those
      // two at dispatch time; a bare `program list` reaches an honest
      // unsupported-command gap). The explicit `program define dir` form is
      // NOT affected — only the ambiguous bare spelling is exempted here.
      static const std::regex reDefineKeyword(R"(^prog(?:ram)?\s+de(?:f(?:ine)?)?\s+)",
                                             std::regex::icase);
      std::string progNameLower = lower(progName);
      bool isReservedBareForm = !std::regex_search(stripped, reDefineKeyword) &&
                                (progNameLower == "dir" || progNameLower == "list" ||
                                 progNameLower == "drop");
      if (isReservedBareForm) {
        out.push_back(line);
        ++i;
        continue;
      }

      std::string progOption = m[2].matched ? lower(trim(m[2].str())) : std::string();
      // Strip a leading/trailing comma defensively (mirrors Python's
      // `.lower().strip(",")`; the regex group itself excludes the comma,
      // but keep the strip for parity with the oracle's exact transform).
      while (!progOption.empty() && progOption.front() == ',') progOption.erase(progOption.begin());
      while (!progOption.empty() && progOption.back() == ',') progOption.pop_back();
      bool isRclass = (progOption == "rclass");
      // P4: eclass/sclass — see the kProgClassSentinelPrefix note above for
      // why this can't just be a new ProgramDef bool.
      bool isEclass = (progOption == "eclass");
      bool isSclass = (progOption == "sclass");

      // Redefine idiom: strip a `[capture] program drop NAME` that
      // immediately precedes this definition (skipping blank/comment
      // lines). In sequential Stata that drop targets a PRE-EXISTING NAME;
      // in this pre-pass it would otherwise run LATER (after the defines
      // are pulled out it becomes adjacent to the driver) and delete the
      // just-captured program, so it is stripped. A drop AFTER a define
      // (genuine mid-script undefine) is left in place.
      long long k = static_cast<long long>(out.size()) - 1;
      while (k >= 0) {
        std::string ls = trim(out[static_cast<size_t>(k)]);
        if (ls.empty() || ls.rfind("*", 0) == 0 || ls.rfind("//", 0) == 0) {
          --k;
          continue;
        }
        std::smatch md;
        if (std::regex_search(ls, md, reProgDrop()) && lower(md[1].str()) == lower(progName)) {
          out.erase(out.begin() + k);
        }
        break;
      }

      // Consume body lines until we hit `end`.
      std::vector<std::string> body;
      ++i;
      while (i < n) {
        const std::string& bline = lines[i];
        std::string bstripped = trim(bline);
        ++i;
        if (std::regex_match(bstripped, reEnd())) {
          break;  // end of program
        }
        // wt_x01: a program body is stored like a loop body, each unquoted
        // blank run collapsed to one blank (eval::collapseUnquotedBlanks;
        // StataBE probe: `program echo0` lines `[a  "q  q"  b]` -> `[a "q  q" b]`).
        body.push_back(eval::collapseUnquotedBlanks(bline));
      }

      // P4: append the eclass/sclass sentinel AFTER every real body line was
      // consumed above — see kProgClassSentinelPrefix's note.
      if (isEclass) {
        body.push_back(kProgClassSentinelPrefix + "eclass");
      } else if (isSclass) {
        body.push_back(kProgClassSentinelPrefix + "sclass");
      }

      // Store (or replace) the definition.
      ProgramDef def;
      def.name = progName;
      def.rclass = isRclass;
      def.body = std::move(body);
      session.programs[progName] = def;
      // BUG-342: keep an untouched copy that `clear all` / `program drop _all`
      // cannot reach, and leave ONE sentinel where the block stood. Reaching it
      // during execution re-installs the copy, so the definition takes effect at
      // its own source position instead of at preprocess time. Registering into
      // session.programs here as well is deliberate: it keeps every pre-existing
      // path (program dir, a body that calls a program defined further down)
      // behaving exactly as before, and makes the install a no-op re-store in
      // every do-file that never clears the table.
      session.programArchive[progName] = std::move(def);
      ++session.programsEpoch;
      out.push_back(kProgInstallSentinelPrefix + progName);
      // Body lines consumed; only the sentinel goes to `out`.
      continue;
    }

    // Bare `end` outside a program block — drop silently.
    if (std::regex_match(stripped, reEnd())) {
      ++i;
      continue;
    }

    out.push_back(line);
    ++i;
  }

  return out;
}

// ---------------------------------------------------------------------------
// Invocation

std::map<std::string, std::string> buildArgs(const std::string& argstr) {
  std::map<std::string, std::string> mapping;
  mapping["0"] = argstr;
  std::string rest = argstr;
  int idx = 1;
  while (!trim(rest).empty() && idx <= 20) {
    std::string prev = rest;
    // BUG-1049: `0' keeps the line as typed, so a compound-quoted argument
    // arrives here with its `"..."' -- and Stata's `1'..`n' drop one level of
    // them (`pn `"a "b" c"'' gives `1' = a "b" c), as they drop a plain "..".
    auto [tok, newRest] = eval::scanOneToken(rest, /*bind=*/true);
    rest = newRest;
    if (rest == prev) break;  // no progress (defensive) — avoid an infinite loop
    // Store the token even when EMPTY: an explicit `""` argument is a real
    // positional that holds a slot. Dropping it shifts every later `1'..`9'
    // reference — mirrors the oracle's comment verbatim (see program.py).
    mapping[std::to_string(idx)] = tok;
    ++idx;
  }
  return mapping;
}

namespace {

// BUG-360/BUG-922: erase the tempfiles and the tempname scalars/matrices
// minted deeper than `callerLevel` (SessionState::preserveLevel() of the
// caller). Never throws: ScopeGuard's destructor calls it.
void dropTempfilesAndNamesDeeperThan(int callerLevel, SessionState& session) {
  // BUG-360: ScopeGuard calls this before anything else and unconditionally:
  // its early return is about scope restoration, and a program whose scope
  // stack is already unwound has still created files that are now
  // unreachable. Stata: "When the program or do-file
  // concludes, any datasets created with these assigned names are erased",
  // and for tempfile specifically it happens "regardless of whether your
  // program ends with an error" ([P] macro) — which is exactly why this sits
  // in the destructor, on the throwing path too, rather than after the body.
  //
  // Anything minted at a preserve level DEEPER than our caller's belongs to
  // this call (or to a nested program or do-file that already concluded and,
  // if it had its own entries, already took them). Best-effort, error_code'd,
  // never throws out of a destructor — the same discipline as the two reapers
  // this joins (doexec.cpp's TempdirReapGuard, parallel.cpp's per-task
  // remove_all).
  if (!session.mintedTempfiles.empty()) {
    auto& mt = session.mintedTempfiles;
    auto keep = mt.begin();
    for (auto it = mt.begin(); it != mt.end(); ++it) {
      if (it->first > callerLevel) {
        std::error_code rmEc;
        std::filesystem::remove(cecon::to_native_path(it->second), rmEc);
        // BUG-384: the worker's virtual-save table held a ref-snapshot (and a
        // whole root mmap) for every tempfile ever saved, with no eraser —
        // unbounded hidden disk retention once the GC unlinks mapped roots.
        // The tempfile's Stata death is the contract point; forget it there.
        session.residentVirtualDrop(it->second);
      } else {
        if (keep != it) *keep = std::move(*it);
        ++keep;
      }
    }
    mt.erase(keep, mt.end());
  }
  // BUG-922: a tempname's scalar or matrix dies with the program that minted
  // it (oracle w2_prog_controlflow pc_tv_gone: `confirm scalar` and `confirm
  // matrix` on the names afterwards are r(111)). Same level rule as above.
  if (!session.mintedTempnames.empty()) {
    auto& tn = session.mintedTempnames;
    auto keep = tn.begin();
    for (auto it = tn.begin(); it != tn.end(); ++it) {
      if (it->first > callerLevel) {
        session.eval.scalars.erase(it->second);
        auto& mats = session.eval.matrices;
        const std::string& nm = it->second;
        mats.erase(std::remove_if(mats.begin(), mats.end(),
                                  [&nm](const auto& kv) { return kv.first == nm; }),
                   mats.end());
      } else {
        if (keep != it) *keep = std::move(*it);
        ++keep;
      }
    }
    tn.erase(keep, tn.end());
  }
}

// RAII scope guard: always restores session.eval.locals from scopeStack and
// session.progCallDepth, even if dispatchFn/executeDoSource throws. Mirrors
// Python's try/finally around push_scope()/pop_scope() in run_program.
class ScopeGuard {
 public:
  ScopeGuard(SessionState& session, int callerDepth, int callerLevel)
      : session_(session), callerDepth_(callerDepth), callerLevel_(callerLevel) {}

  ~ScopeGuard() {
    // BUG-360: erase the tempfiles THIS program minted, before anything else
    // and unconditionally (see dropTempfilesAndNamesDeeperThan).
    dropTempfilesAndNamesDeeperThan(callerLevel_, session_);
    if (session_.scopeStack.empty()) return;
    // _rc is one value for the whole session, as in the do-file scope
    // (doexec PrivateLocalScope): what the body left is the caller's.
    std::optional<std::string> rc;
    if (const std::string* bodyRc = session_.eval.locals.find("_rc")) rc = *bodyRc;
    session_.eval.locals = std::move(session_.scopeStack.back());
    session_.scopeStack.pop_back();
    if (rc.has_value()) session_.eval.locals.set("_rc", *rc);
    session_.progCallDepth = callerDepth_;
  }

  ScopeGuard(const ScopeGuard&) = delete;
  ScopeGuard& operator=(const ScopeGuard&) = delete;

 private:
  SessionState& session_;
  int callerDepth_;
  int callerLevel_;  // BUG-922: the caller's preserveLevel()
};

}  // namespace

void concludeLevel(int callerLevel, SessionState& session, ojson& blocks) {
  // The restore goes first, so a tempvar that only the restored-away data held
  // needs no drop. (restorePreservesDeeperThan is defined in datamgmt/preserve_restore.cpp,
  // beside runRestore.)
  restorePreservesDeeperThan(callerLevel, session, blocks);
  dropTempfilesAndNamesDeeperThan(callerLevel, session);
  if (session.mintedTempvars.empty()) return;
  std::vector<std::string> mine;
  auto& tv = session.mintedTempvars;
  auto keep = tv.begin();
  for (auto it = tv.begin(); it != tv.end(); ++it) {
    if (it->first > callerLevel) {
      mine.push_back(it->second);
    } else {
      if (keep != it) *keep = std::move(*it);
      ++keep;
    }
  }
  tv.erase(keep, tv.end());
  std::vector<std::string> live;
  if (!mine.empty() && session.currentHbin().has_value()) {
    if (Datastore* ds = session.datastore()) {
      for (const auto& v : mine)
        if (ds->has(v)) live.push_back(v);
    }
  }
  if (!live.empty()) (void)handlers::runDrop(live, session);
}

void concludeLevelAfterThrow(int callerLevel, SessionState& session) noexcept {
  try {
    ojson ignored = ojson::array();
    concludeLevel(callerLevel, session, ignored);
  } catch (...) {
  }
}

LoopFloorGuard::LoopFloorGuard(SessionState& session)
    : session_(session), saved_(session.loopBodyFloor) {
  session_.loopBodyFloor = loops::loopBodyDepth();
}
LoopFloorGuard::~LoopFloorGuard() { session_.loopBodyFloor = saved_; }

ojson runProgram(const ProgramDef& progDef, const std::string& argstr, SessionState& session,
                  const DispatchFn& dispatchFn) {
  // --- Recursion guard ---
  int depth = session.progCallDepth;
  const int callerPreserveLevel = session.preserveLevel();
  if (depth >= kMaxRecursion) {
    ojson err = eval::engineError("program " + progDef.name + ": recursion depth exceeded " +
                                   std::to_string(kMaxRecursion));
    ojson out = ojson::array();
    out.push_back(std::move(err));
    return out;
  }
  session.progCallDepth = depth + 1;

  // --- Snapshot and reset returnStaging ---
  std::map<std::string, std::pair<std::string, std::string>> callerStaging = session.returnStaging;
  session.returnStaging.clear();

  // --- Build scope args ---
  std::map<std::string, std::string> args = buildArgs(argstr);

  // --- Push scope ---
  session.scopeStack.push_back(session.eval.locals);
  eval::OrderedDict fresh;
  // StataBE 19.5, 2026-11-03: the body reads the caller's _rc (`capture error
  // 459`, then a program's `display _rc` prints 459, and its `exit _rc` ends
  // it with r(459)).
  if (const std::string* callerRc = session.eval.locals.find("_rc")) fresh.set("_rc", *callerRc);
  for (const auto& [k, v] : args) fresh.set(k, v);
  // BUG-1330: the body runs under its caller's `version` until it sets its own,
  // which is gone when it returns (StataBE: P1|15 under `version 15`, then
  // `version 12` in the body, and the caller still at 15).
  for (const char* key : {kVersionLocal, kUserVersionLocal})
    if (const std::string* ver = session.eval.locals.find(key)) fresh.set(key, *ver);
  fresh.set(kProgramBodyLocal, "1");
  session.eval.locals = std::move(fresh);

  // P4: recover eclass/sclass from captureProgams' trailing sentinel (see
  // kProgClassSentinelPrefix's note) and stage it onto a hidden local of
  // THIS call's fresh scope so handleReturnStmt's ereturn/sreturn honesty
  // checks can see it while the body runs. `progClass` (the plain C++
  // local, not the hidden Stata local) survives past the scope-pop below
  // for the flush step further down.
  std::string progClass;
  if (!progDef.body.empty() && progDef.body.back().rfind(kProgClassSentinelPrefix, 0) == 0) {
    progClass = progDef.body.back().substr(kProgClassSentinelPrefix.size());
    session.eval.locals.set(kProgClassMarkerKey, progClass);
  }

  ojson allBlocks = ojson::array();
  std::optional<int> exitCode;
  try {
    // --- Pop scope (always), via RAII so this runs even if the call below
    // throws (mirrors the Python try/finally).
    ScopeGuard guard(session, depth, callerPreserveLevel);
    LoopFloorGuard loopFloor(session);  // BUG-920: the body starts outside any loop
    std::string source;
    for (size_t i = 0; i < progDef.body.size(); ++i) {
      if (i) source += "\n";
      source += progDef.body[i];
    }
    doexec::ExecuteResult result = doexec::executeDoSource(source, session, dispatchFn);
    allBlocks = result.blocks;
    exitCode = result.stoppedAtExitCode;
    // <- guard fires at the end of this block: pop_scope + restore progCallDepth
  } catch (...) {
    // BUG-922: no copy or temporary of this call outlives it on the
    // exception path either (the guard has already fired).
    concludeLevelAfterThrow(callerPreserveLevel, session);
    throw;
  }

  // BUG-922: what Stata undoes when a program returns, on success and on error
  // alike. (1) The program's own preserved copy is restored (oracle
  // w2_prog_controlflow pc_pr_in_program: N back to 6 after the body dropped
  // to 2; pc_pr_in_program_err: also when the body fails with r(459);
  // pc_pr_in_program_not: not when the body said `restore, not`). (2) Its
  // tempvars are dropped (pc_tv_gone: `confirm variable` afterwards r(111)).
  // The restore goes first, so a tempvar that only the restored-away data held
  // needs no drop. Its tempfiles and tempnames went with the scope guard.
  concludeLevel(callerPreserveLevel, session, allBlocks);

  // BUG-1462: `exit N` with N other than 0 ends the program with return code
  // N, which the caller sees as the call's own rc (StataBE 19.5: `program p5`
  // ending in `exit 5`, then `capture p5` leaves _rc 5; `exit _rc` hands on
  // the caller's _rc). The executor took the halt sentinel out of the body's
  // stream; the same sentinel goes back on the call's, so the caller halts or
  // captures it as it would its own `exit N`. A bare `exit` or `exit 0` is a
  // clean return.
  if (exitCode.has_value() && *exitCode != 0) {
    ojson halt = eval::engineError("exit " + std::to_string(*exitCode), "program_exit");
    halt["reason"] = "r(" + std::to_string(*exitCode) + ")";
    allBlocks.push_back(std::move(halt));
  }

  // --- Flush return values AFTER pop_scope (so caller scope sees them) ---
  // "scalar"/"local" (rclass, S7 precedent) are UNCHANGED — same guard, same
  // bodies, just re-homed one level deeper into the per-kind branch below so
  // the P4 "escalar"/"slocal" kinds can share the same loop. "escalar"/
  // "slocal" are only ever staged by ereturn/sreturn, and those already
  // self-gate on progClass in handleReturnStmt (a mismatched class never
  // reaches session.returnStaging at all) — the progClass re-check here is
  // defense in depth, not the primary gate.
  if (!session.returnStaging.empty()) {
    for (const auto& [rname, kindVal] : session.returnStaging) {
      const std::string& kind = kindVal.first;
      const std::string& val = kindVal.second;
      if (progDef.rclass && kind == "scalar") {
        double d = 0.0;
        try {
          d = std::stod(val);
        } catch (...) {
          d = std::nan("");
        }
        session.eval.rValues.set(rname, d);
      } else if (progDef.rclass && kind == "local") {
        session.eval.rStrings.set(rname, val);
      } else if (progClass == "eclass" && kind == "escalar") {
        double d = 0.0;
        try {
          d = std::stod(val);
        } catch (...) {
          d = std::nan("");
        }
        session.eval.setEScalar(rname, d);
        // BUG-1064: the next estimation replaces all of e() (StataBE: e(bar) is
        // `.` after the following regress), so the install must drop it too.
        session.noteEOnFit(rname);
      } else if (progClass == "sclass" && kind == "slocal") {
        session.eval.rStrings.set(kSClassKeyPrefix + rname, val);
      }
    }
  }

  // Restore caller's staging (nested rclass programs layer correctly).
  session.returnStaging = std::move(callerStaging);

  return allBlocks;
}

// ---------------------------------------------------------------------------
// program drop / program dir (native handler)

ojson handleProgramManage(const std::string& sub, const std::vector<std::string>& names,
                           SessionState& session) {
  if (sub == "dir") {
    std::string text;
    if (!session.programs.empty()) {
      std::vector<std::string> keys;
      keys.reserve(session.programs.size());
      for (const auto& [k, v] : session.programs) keys.push_back(k);
      std::sort(keys.begin(), keys.end());
      for (size_t i = 0; i < keys.size(); ++i) {
        if (i) text += " ";
        text += keys[i];
      }
    } else {
      text = "(no programs defined)";
    }
    ojson b;
    b["type"] = "display";
    b["text"] = text;
    return b;
  }

  if (sub == "drop") {
    // BUG-923: `program drop _all` drops every program (oracle
    // w2_prog_controlflow pc_pg_prog_drop_all: both programs then r(199)).
    // `_all` used to be looked up as a program name, missed, and was dropped
    // silently as an absent name. The archive survives, as under `clear all`
    // (BUG-342): a definition further down the do-file still installs when
    // its own line is reached.
    for (const auto& name : names) {
      if (name == "_all") {
        session.programs.clear();
        ++session.programsEpoch;
        ojson b;
        b["type"] = "display";
        b["text"] = "dropped: _all";
        return b;
      }
    }
    std::vector<std::string> dropped;
    std::vector<std::string> missing;
    for (const auto& name : names) {
      std::optional<std::string> match = findProgram(session, name);
      if (match.has_value()) {
        session.programs.erase(*match);
        ++session.programsEpoch;
        dropped.push_back(*match);
      } else {
        missing.push_back(name);
      }
    }
    if (!missing.empty()) {
      std::string joined;
      for (size_t i = 0; i < missing.size(); ++i) {
        if (i) joined += ", ";
        joined += missing[i];
      }
      // BUG-923: Stata answers r(111) "program NAME not found" (oracle
      // pc_pg_capdrop_absent: `capture program drop pc_never_defined` is
      // _rc 111). The dispatcher used to swallow this block as a benign no-op.
      return eval::errorBlock("engine_error", "program " + joined + " not found", std::nullopt,
                              std::nullopt, std::nullopt, std::string("r(111)"));
    }
    std::string joined;
    for (size_t i = 0; i < dropped.size(); ++i) {
      if (i) joined += " ";
      joined += dropped[i];
    }
    ojson b;
    b["type"] = "display";
    b["text"] = "dropped: " + joined;
    return b;
  }

  return eval::engineError("program " + sub + ": unknown subcommand");
}


// ---------------------------------------------------------------------------
// S7: return scalar/local step-0 handlers

bool handleReturnStmt(const std::string& rawLine, SessionState& session, ojson& outErrorBlock) {
  outErrorBlock = ojson();

  std::string stripped = trim(rawLine);

  // A2/A3: this function is SEVEN regex searches, and it sits in the dispatch
  // prologue ahead of the macro-definition intercept — so every `local` line in
  // every loop body paid all seven to be told "not mine". A symbolized profile
  // put **24% of a pure-macro loop's samples inside this call**. Every form it
  // handles begins with one of five literal keywords, so the first token settles
  // it. Kept HERE rather than at the call site so every caller inherits the
  // gate, and written as a first-token test (not a `find`) so a keyword
  // appearing later in a line — inside a string, as an argument — cannot be
  // mistaken for one of these statements, exactly as the anchored `^` patterns
  // below require.
  {
    size_t e = stripped.find_first_of(" \t");
    std::string head = stripped.substr(0, e == std::string::npos ? stripped.size() : e);
    for (char& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (head != "return" && head != "ereturn" && head != "sreturn" && head != "syntax" &&
        head != "tokenize") {
      return false;
    }
  }

  // `return scalar NAME = EXPR`
  {
    static const std::regex re(R"(^return\s+scalar\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$)",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(stripped, m, re)) {
      std::string name = m[1].str();
      std::string expr = trim(m[2].str());

      // Evaluate arithmetic if possible.
      if (eval::hasMacroRefs(expr)) {
        eval::ExpandResult expanded = eval::expandPass(expr, session.eval);
        if (!expanded.isError) expr = expanded.line;
      }
      std::optional<std::string> val = eval::evalArith(expr, &session.eval);  // WS-C: return scalar n = b[1,1]
      double floatVal;
      try {
        floatVal = val.has_value() ? std::stod(*val) : std::stod(expr);
      } catch (...) {
        floatVal = std::nan("");
      }
      // BUG-423 (adjudicated 2026-09-06, User: "버그는 재정하고").
      //
      // AT THE TOP LEVEL THERE IS NOTHING TO RETURN FROM. Bundle B read that as
      // "so set r() immediately", on the strength of a comment here that said
      // "Real Stata sets r() immediately at the top level". That claim was never
      // measured, and it is FALSE: StataBE 19.5 answers **r(151)**, "non r-class
      // program may not set r()", for `return scalar q = 3` in a do-file. The
      // wave pin bundle B's own plan asked for is what measured it.
      //
      // The ruling is to refuse, and the reason is the product's premise rather
      // than a preference: accepting what Stata refuses is the ONE direction of
      // divergence that silently produces a do-file which cannot be run in
      // Stata. Nothing is lost — a program that wants to return a value still
      // stages and flushes below, which is the path every r-class program takes
      // and the path Stata itself documents.
      //
      // progCallDepth is the discriminator because it is the ONE counter that
      // answers "is a program on the stack" (runProgram increments it around
      // every call, including recursive ones).
      if (session.progCallDepth == 0) {
        outErrorBlock = eval::errorBlock(
            "engine_error", "return scalar: non r-class program may not set r()", std::nullopt,
            std::nullopt, std::nullopt, "r(151)");
        return true;
      }
      // Store the staged value as its canonical string form; runProgram's
      // flush parses it back with std::stod. Use a round-trip-safe
      // formatting (max precision) so the double survives the string hop
      // unchanged for any finite value, and "nan" for NaN (std::stod("nan")
      // parses back to NaN on all supported platforms).
      char buf[64];
      if (std::isnan(floatVal)) {
        session.returnStaging[name] = {"scalar", "nan"};
      } else {
        std::snprintf(buf, sizeof(buf), "%.17g", floatVal);
        session.returnStaging[name] = {"scalar", std::string(buf)};
      }
      return true;
    }
  }

  // `return local NAME value`
  {
    static const std::regex re(R"(^return\s+local\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s+(.*))?$)",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(stripped, m, re)) {
      std::string name = m[1].str();
      std::string val = m[2].matched ? trim(m[2].str()) : std::string();

      // Stata expands macro refs in the value at the point `return local`
      // runs (in the program's local scope) — e.g.
      //   tempfile pf
      //   return local pfpath "`pf'"
      // must store the resolved temp path, not the literal `pf'. This
      // step-0 handler fires BEFORE 0b macro-expansion (dispatcher.cpp), so
      // the ref is still unexpanded here — expand it ourselves, mirroring
      // the `return scalar` branch above.
      // EM3a: expanded to a fixed point with macval() verbatim, as a `local`
      // definition is (w2af mt_retloc_macval: `return local text `"`macval(txt)'"'`
      // over a value holding a backtick, w2d_uncovered.do:130).
      if (eval::hasMacroRefs(val)) {
        eval::ExpandResult expanded = eval::expandDefinitionText(val, session.eval);
        if (!expanded.isError) val = expanded.line;
      }

      // Strip ONE layer of balanced delimiting quotes, Stata-faithfully:
      //   return local x "age edu income"   stores  age edu income
      //   return local x `"compound"'       stores  compound
      // so a downstream `use "`r(x)'"` / `r(x)' resolves to a clean token
      // rather than a nested compound-quote artifact (`"…"') that macro
      // hardening would reject. Bare values (no delimiters) pass through
      // as-is.
      if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
        val = val.substr(1, val.size() - 2);
      } else if (val.size() >= 4 && val.rfind("`\"", 0) == 0 && val.substr(val.size() - 2) == "\"'") {
        val = val.substr(2, val.size() - 4);
      }
      // BUG-423, the `return local` twin: same discriminator, same ruling. Stata
      // refuses at the top level with r(151) — measured on StataBE 19.5, 2026-09-06
      // — and the comment that used to sit here claiming otherwise was the
      // unmeasured half of the same pair. See the long note on `return scalar`.
      if (session.progCallDepth == 0) {
        outErrorBlock = eval::errorBlock(
            "engine_error", "return local: non r-class program may not set r()", std::nullopt,
            std::nullopt, std::nullopt, "r(151)");
        return true;
      }
      session.returnStaging[name] = {"local", val};
      return true;
    }
  }

  // ---------------------------------------------------------------------
  // P4: ereturn scalar / sreturn local / syntax varlist(min=N) / tokenize.
  //
  // These four all belong at this SAME step-0 interception point as
  // `return scalar`/`return local` above — dispatcher.cpp calls
  // handleReturnStmt() on the raw line before macro expansion, before
  // try_capture_definition, and before eval::tokenize()'s command-word
  // gate, and this stint's file manifest has no header to add a second
  // call site through, so every new construct is folded into this one
  // function's body rather than a new dispatcher arm (would need
  // dispatcher.cpp, off limits) or a second exported hook (would need
  // program.h, off limits).

  // `ereturn scalar NAME = EXPR` — same evaluation path as `return scalar`
  // above (duplicated rather than shared, deliberately: the S7 return-
  // scalar path is a proven, freeze-pinned path and this stint does not
  // touch it). Gated on the CURRENT program being eclass, via the hidden
  // local runProgram() stages — see kProgClassMarkerKey's note at the top
  // of this file. Honest reject outside an eclass program (or at top
  // level, where the marker is simply absent) rather than a silent no-op.
  {
    static const std::regex re(R"(^ereturn\s+scalar\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$)",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(stripped, m, re)) {
      const std::string* cls = session.eval.locals.find(kProgClassMarkerKey);
      if (cls == nullptr || *cls != "eclass") {
        // Stata: r(152) "non e-class program may not set e()" (w2p_estim_ev.log:907
        // C_ereturn_scalar), the code `ereturn local` already answers.
        outErrorBlock = eval::errorBlock("engine_error", "non e-class program may not set e()",
                                         std::nullopt, std::nullopt, std::nullopt, "r(152)");
        return true;
      }
      std::string name = m[1].str();
      std::string expr = trim(m[2].str());

      if (eval::hasMacroRefs(expr)) {
        eval::ExpandResult expanded = eval::expandPass(expr, session.eval);
        if (!expanded.isError) expr = expanded.line;
      }
      std::optional<std::string> val = eval::evalArith(expr, &session.eval);
      double floatVal;
      try {
        floatVal = val.has_value() ? std::stod(*val) : std::stod(expr);
      } catch (...) {
        floatVal = std::nan("");
      }
      // "escalar" kind (not "scalar") — runProgram's flush routes it to
      // session.eval.eScalars, NOT session.eval.rValues, so `ereturn` never
      // shows up under r(). Same round-trip-safe %.17g formatting as
      // `return scalar` above.
      char buf[64];
      if (std::isnan(floatVal)) {
        session.returnStaging[name] = {"escalar", "nan"};
      } else {
        std::snprintf(buf, sizeof(buf), "%.17g", floatVal);
        session.returnStaging[name] = {"escalar", std::string(buf)};
      }
      return true;
    }
  }

  // `sreturn local NAME value` — same staging path as `return local` above
  // (again duplicated, not shared, for the same reason). Gated on the
  // current program being sclass.
  {
    static const std::regex re(R"(^sreturn\s+local\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s+(.*))?$)",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(stripped, m, re)) {
      const std::string* cls = session.eval.locals.find(kProgClassMarkerKey);
      if (cls == nullptr || *cls != "sclass") {
        outErrorBlock = eval::engineError(
            "sreturn local: not allowed, not in an sclass program.");
        return true;
      }
      std::string name = m[1].str();
      std::string val = m[2].matched ? trim(m[2].str()) : std::string();

      if (eval::hasMacroRefs(val)) {
        eval::ExpandResult expanded = eval::expandDefinitionText(val, session.eval);  // EM3a
        if (!expanded.isError) val = expanded.line;
      }
      if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
        val = val.substr(1, val.size() - 2);
      } else if (val.size() >= 4 && val.rfind("`\"", 0) == 0 && val.substr(val.size() - 2) == "\"'") {
        val = val.substr(2, val.size() - 4);
      }
      // "slocal" kind -> runProgram's flush routes it into
      // session.eval.rStrings under the kSClassKeyPrefix-reserved key so
      // eval/macros.cpp's `s(name)' reader can find it without colliding
      // with any real r()-name.
      session.returnStaging[name] = {"slocal", val};
      return true;
    }
  }

  // `syntax varlist(min=N)` — minimal subset (P4 task scope: nothing else).
  // Binds the caller's varlist (Stata's `0', already set by buildArgs from
  // the program's own argstr) into the local `varlist' after checking the
  // minimum count; any OTHER `syntax` spec (max=, options, if/in/using,
  // [...] blocks) is an explicit reject naming what's unsupported — never a
  // silent partial bind, per the task brief's "a program that silently sees
  // the wrong varlist produces a wrong regression" warning.
  {
    static const std::regex reVarlistMin(
        R"(^syntax\s+varlist\s*\(\s*min\s*=\s*(\d+)\s*\)\s*$)", std::regex::icase);
    std::smatch m;
    if (std::regex_match(stripped, m, reVarlistMin)) {
      long long minN = 0;
      try {
        minN = std::stoll(m.str(1));
      } catch (...) {
        minN = 0;
      }
      std::string argstr = session.eval.locals.get("0", "");
      std::istringstream argIss(argstr);
      std::vector<std::string> vars;
      std::string tok;
      while (argIss >> tok) vars.push_back(tok);
      if (static_cast<long long>(vars.size()) < minN) {
        outErrorBlock = eval::engineError(
            "syntax: varlist requires a minimum of " + std::to_string(minN) +
            " variable(s); " + std::to_string(vars.size()) + " specified.");
        return true;
      }
      std::string joined;
      for (size_t i = 0; i < vars.size(); ++i) {
        if (i) joined += " ";
        joined += vars[i];
      }
      session.eval.locals.set("varlist", joined);
      return true;
    }
    static const std::regex reSyntaxAny(R"(^syntax\b(.*)$)", std::regex::icase);
    if (std::regex_search(stripped, m, reSyntaxAny)) {
      // Every other spec: the general binder above (bindSyntax), which binds
      // what it can parse in full and refuses the rest by name.
      ojson err;
      if (!bindSyntax(m.str(1), session, &err)) outErrorBlock = err;
      return true;
    }
  }

  // `tokenize STRING[, ...]` — splits STRING on whitespace and binds
  // numbered locals `1'..`N' (20-slot cap, matching buildArgs' own cap
  // just above in this file: tokenize and program positional args share
  // ONE numbered-local namespace in real Stata, so reusing the same
  // ceiling is consistency, not a new invented limit). Any previously
  // bound numbered locals beyond the new count are cleared first, so a
  // shorter re-tokenize doesn't leave stale trailing values reachable.
  // Runs on the RAW line, well before eval::tokenize()'s own command-word
  // gate would otherwise reject "tokenize" outright (eval/tokenizer.cpp's
  // macroStmts() list) — that gate is simply never reached for this word
  // now, left in place as a defensive fallback, not touched.
  {
    static const std::regex reTokenizeCmd(R"(^tokenize(?:\s+(.*))?$)", std::regex::icase);
    std::smatch m;
    if (std::regex_search(stripped, m, reTokenizeCmd)) {
      std::string rest = m[1].matched ? m.str(1) : std::string();

      // Split off a comma-introduced option tail the same quote-aware way
      // every other Stata command does; `parse()` and friends are not
      // implemented, so any such tail is an honest reject, not a silent
      // ignore.
      long long commaPos = eval::findUnquotedComma(rest);
      std::string arg = commaPos == -1 ? rest : rest.substr(0, static_cast<size_t>(commaPos));
      std::string optsTail =
          commaPos == -1 ? std::string() : trim(rest.substr(static_cast<size_t>(commaPos) + 1));
      if (!optsTail.empty()) {
        outErrorBlock = eval::engineError(
            "tokenize: options (`, " + optsTail +
            "`) are not implemented in this engine — only plain whitespace "
            "splitting of the string argument is supported.");
        return true;
      }

      arg = trim(arg);
      // A double-quoted argument's outer quotes are delimiters, not
      // content — `tokenize one two three` (no quotes) is equally legal
      // Stata and needs no stripping.
      if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"') {
        arg = arg.substr(1, arg.size() - 2);
      }
      if (eval::hasMacroRefs(arg)) {
        eval::ExpandResult expanded = eval::expandPass(arg, session.eval);
        if (!expanded.isError) arg = expanded.line;
      }

      for (int i = 1; i <= 20; ++i) session.eval.locals.erase(std::to_string(i));
      std::istringstream argIss(arg);
      std::string tok;
      int idx = 1;
      while (idx <= 20 && (argIss >> tok)) {
        session.eval.locals.set(std::to_string(idx), tok);
        ++idx;
      }
      return true;
    }
  }

  return false;
}

// ---------------------------------------------------------------------------
// Internal helpers

std::optional<std::string> findProgram(const SessionState& session, const std::string& name) {
  std::string nameLower = lower(name);
  for (const auto& [k, v] : session.programs) {
    if (lower(k) == nameLower) return k;
  }
  return std::nullopt;
}

}  // namespace piet::program
