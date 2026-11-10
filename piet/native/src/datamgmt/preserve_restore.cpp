// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Preserve restore.

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

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace command_detail {}
using namespace command_detail;
using namespace session_vocab;

namespace command_detail {
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

// B5-D14 (BUG-454): the snapshot half of runPreserve, factored out so
// runRestore's `, preserve` re-arm can call it on the JUST-RESTORED state
// instead of trying to push the frame it popped back onto the stack (which
// may be invalid: MAXPERF Stage 2's unstash can CONSUME the resident stash,
// and the scratch-reclaim exemption below is keyed on live preserveStack
// membership, so a pop-then-repush of the SAME frame would open a reclaim
// window). Re-snapshotting from a known-good, already-restored state avoids
// both hazards. Returns an error block on snapshot failure, std::nullopt on
// success (frame pushed).
//
// BUG-922: `level` is the preserve level that owns the copy: the level of the
// `preserve`, or the popped frame's own level on a `restore, preserve` re-arm.
// This is the only writer of PreserveFrame::level (session_state.h I1).
std::optional<ojson> pushPreserveSnapshot(SessionState& session, int level) {
  PreserveFrame frame;
  frame.noData = !session.currentHbin().has_value();  // BUG-1139: nothing to snapshot
  // MAXPERF Stage 2 (item 1): when the resident plane is active, snapshot the
  // worker's resident data-state via `stash` — an O(columns) RAM capture with
  // NO disk write and NO full-dataset byte-copy. residentPreserve() also keeps
  // the crash-recovery floor (it materializes a dirty virtual head first, which
  // is a no-op on the common clean/real head). The clonefile plane REMAINS the
  // fallback when the resident plane is unavailable (§26-only engine / serve
  // disabled / worker down) — Q1 (gate ③) clone-first (APFS CoW) with the
  // pre-existing byte-copy as the universal fallback (see cloneOrCopyFile).
  if (frame.noData) {
    // No snapshot: the metadata below is all there is to keep.
  } else if (std::optional<std::string> stashName = session.residentPreserve()) {
    frame.stashName = *stashName;
    // Record the preserve-time head file alongside the stash: residentPreserve()
    // materialized the head just before stashing, so this on-disk file is
    // byte-identical to the stash by construction. restore re-adopts it as a
    // CLEAN head (zero-write card build) instead of re-materializing the frame
    // via a dirty virtual token; it also doubles as a recovery source if the
    // worker (and its RAM stash) dies before restore. While this frame is live
    // the file is exempt from scratch reclaim (reclaimSupersededScratch checks
    // preserveStack), so no extra copy or pin is needed.
    frame.hbinPath = *session.currentHbin();
    if (piet::SessionState::scratchTraceOn()) {
      std::error_code tec;
      piet::SessionState::scratchTrace(
          std::filesystem::exists(frame.hbinPath, tec) ? "PRESERVE(twin-real)"
                                                       : "PRESERVE(twin-MISSING)",
          "resident", frame.hbinPath);
    }
  } else {
    std::string snapPath = nextPath(session.workdir(), "preserve");
    // Fallback plane: a virtual resident head must be snapshotted to disk first
    // (cloneOrCopyFile bypasses datastore()). restore() below hands the snapshot
    // file straight to session.load() (zero-copy) on this plane.
    session.ensureMaterializedHead();
    if (!cloneOrCopyFile(*session.currentHbin(), snapPath)) {
      // BUG-124 sibling: fail the preserve HERE, honestly — the old void call
      // pushed a torn (or missing) snapshot onto the stack for restore to load.
      return ojson(engineErrorBlock(
          "preserve: could not snapshot the dataset (disk full or I/O error)"));
    }
    frame.hbinPath = snapPath;
  }
  frame.varMeta = session.varMeta;
  frame.implicitFloatVars = session.implicitFloatVars;
  frame.labelDefs = session.labelDefs;
  frame.varLblname = session.varLblname;
  frame.valueLabels = session.valueLabels;
  frame.dataLabel = session.dataLabel;
  frame.characteristics = session.characteristics;
  frame.level = level;
  session.preserveStack.push_back(std::move(frame));
  return std::nullopt;
}

// BUG-1139: preserve/restore's block when the dataset is the empty one.
ojson emptyDatasetResult(SessionState& session, const std::string& action) {
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = action;
  out["preserve_depth"] = static_cast<int64_t>(session.preserveStack.size());
  out["varlist_after"] = ojson::array();
  out["n_rows_after"] = 0;
  return out;
}
}  // namespace

ojson runPreserve(SessionState& session) {
  // BUG-922: a second `preserve` while this program or do-file level still
  // holds its copy is r(621) "already preserved", and the first copy is kept
  // (oracle w2_prog_controlflow pc_pr_twice 621, then pc_pr_twice_restored
  // brings back the FIRST copy and pc_pr_twice_none_left is r(622)). It used to
  // stack a second copy at rc 0. A program, or a nested do/run file, started
  // while its caller holds a copy may preserve its own (a deeper level;
  // w2ad_prog_preserve_nest pn_d_nested_pres, pn_p_nested_pres), and an
  // included file shares its includer's level (pn_i_pres_held: 621).
  if (session.ownPreserveFrame()) {
    return engineErrorBlockRc("already preserved", 621);
  }
  if (std::optional<ojson> err = pushPreserveSnapshot(session, session.preserveLevel())) {
    return *err;
  }
  if (session.preserveStack.back().noData) return emptyDatasetResult(session, "preserve");

  // P2-PERF ARC-B/B3: render the unchanged varlist from the resident card, the
  // exact transition runUse already made (fileio/dataset_use.cpp). `preserve` does not
  // change the data at all — it only stacks a copy — so calling datastore()
  // here was asking for a full open of the head purely to re-print what the
  // session already knows, and on a dirty virtual head it forced a snapshot
  // besides. Honest fallback to the old path whenever the card cannot vouch
  // for itself.
  //
  // This rung was reverted once, in session 1, because the card could not
  // answer `format` and this render printed %9.0g where the pins said %9s.
  // That was BUG-190 and it is fixed at the root now (session_state.cpp
  // seedShadowFromFrameVars), which is what lets the rung return.
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    if (std::optional<int64_t> cardRows = session.residentRowsIfCurrent()) {
      ojson out;
      out["type"] = "data_mutation";
      out["action"] = "preserve";
      out["preserve_depth"] = static_cast<int64_t>(session.preserveStack.size());
      ojson varlistAfter = ojson::array();
      auto vlist = session.varlistWithMetaFrom(*card);
      for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
      session.updateNMissCache(vlist);
      out["varlist_after"] = varlistAfter;
      out["n_rows_after"] = *cardRows;
      return out;
    }
  }
  Datastore* ds = session.datastore();
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "preserve";
  out["preserve_depth"] = static_cast<int64_t>(session.preserveStack.size());
  ojson varlistAfter = ojson::array();
  auto vlist = session.varlistWithMeta(ds);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: keep the cache current for the next mutation
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = ds ? ds->nRows() : 0;
  return out;
}

namespace command_detail {
// BUG-922: restore (and with `rearm` re-arm) the top preserved copy, whoever
// owns it. Two callers only: runRestore, after its reach test, and the
// unwinder program::restorePreservesDeeperThan below, which restores the
// copies of a level that has already concluded.
ojson restoreTopFrame(SessionState& session, bool rearm) {
  PreserveFrame frame = std::move(session.preserveStack.back());
  session.preserveStack.pop_back();
  if (piet::SessionState::scratchTraceOn()) {
    std::error_code tec;
    piet::SessionState::scratchTrace(
        std::filesystem::exists(frame.hbinPath, tec) ? "RESTORE(twin-real)"
                                                     : "RESTORE(twin-MISSING)",
        frame.stashName.empty() ? "clone" : "stash", frame.hbinPath);
  }

  // MAXPERF Stage 2 (item 1): a stash-backed preserve restores via `unstash`
  // (O(columns) RAM, no disk read); a clone-backed preserve (fallback plane)
  // reloads its file. The restored data-state is byte-identical on both planes
  // (§27 parity law), so the rendered card below is identical either way.
  bool restored = false;
  const bool stashBacked = !frame.stashName.empty();
  if (frame.noData) {
    session.clearData();  // BUG-1139: back to the empty dataset that was preserved
    restored = true;
  } else if (stashBacked) {
    restored = session.residentRestore(frame.stashName, frame.hbinPath);
  }
  if (!restored) {
    // Stash-backed and the unstash failed (worker restarted — the RAM snapshot
    // is gone): the preserve-time head file recorded alongside the stash is
    // byte-identical to the snapshot by construction, so load() it below as a
    // full recovery when it still exists. Only when no on-disk twin survives
    // either: honest error, never silent stale data. Clone-backed frames
    // (fallback plane) take the load() path exactly as before.
    std::error_code restoreEc;
    if (frame.hbinPath.empty() ||
        (stashBacked && !std::filesystem::exists(cecon::to_native_path(frame.hbinPath),
                                                 restoreEc))) {
      return engineErrorBlock(
          "restore: the preserved snapshot was held in the compute worker and is no longer "
          "available (the worker restarted). Re-run from your last saved dataset.");
    }
    session.load(frame.hbinPath);
  }
  session.varMeta = frame.varMeta;
  // An untyped variable is float again after restore, whatever the preserved
  // block did to its claim: `gen x = 1.5`, preserve, `drop x`, restore, then
  // `replace x = 1/3` stores the float +1.5555560000000X-002 in StataBE 19.5,
  // where the lost claim fell back to double (and fcollapse ..., append then
  // left a byte claim on 2.5 and 40000, BUG-1100).
  session.implicitFloatVars = frame.implicitFloatVars;
  session.labelDefs = frame.labelDefs;
  session.varLblname = frame.varLblname;
  session.valueLabels = frame.valueLabels;
  session.dataLabel = frame.dataLabel;
  session.characteristics = frame.characteristics;
  // xtset-file-persistence: the preserved char table carries the declaration
  // that was live at `preserve`; restoring the one restores the other.
  session.adoptTsFromChars();

  if (rearm) {
    // B5-D14 (BUG-454, measured B5REC|restore|preserve_after_preserve|0| +
    // B5VAL|zz_exists_after|0| i.e. a following `gen` succeeds, tests/eval/
    // golden/aer_b4/b5_misc_pins.log:97-103): `restore, preserve` restores
    // AND keeps the preserved copy, so a do-file can restore repeatedly from
    // one snapshot. Re-snapshot from the state JUST restored above (not the
    // popped `frame`) -- see pushPreserveSnapshot's comment for why reusing
    // the popped frame is unsafe. BUG-922: the copy keeps the level that
    // owned it (session_state.h I1).
    if (std::optional<ojson> err = pushPreserveSnapshot(session, frame.level)) {
      return *err;
    }
  }
  if (frame.noData) return emptyDatasetResult(session, "restore");

  // P2-PERF ARC-B/B3, the other end of the same window (see runPreserve above).
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    if (std::optional<int64_t> cardRows = session.residentRowsIfCurrent()) {
      ojson out;
      out["type"] = "data_mutation";
      out["action"] = "restore";
      out["preserve_depth"] = static_cast<int64_t>(session.preserveStack.size());
      ojson varlistAfter = ojson::array();
      auto vlist = session.varlistWithMetaFrom(*card);
      for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
      session.updateNMissCache(vlist);
      out["varlist_after"] = varlistAfter;
      out["n_rows_after"] = *cardRows;
      return out;
    }
  }
  Datastore* ds = session.datastore();
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "restore";
  out["preserve_depth"] = static_cast<int64_t>(session.preserveStack.size());
  ojson varlistAfter = ojson::array();
  auto vlist = session.varlistWithMeta(ds);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: keep the cache current for the next mutation
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = ds ? ds->nRows() : 0;
  return out;
}
}  // namespace

ojson runRestore(SessionState& session, bool rearm) {
  if (!session.ownPreserveFrame()) {
    // B5-D14 (BUG-454, measured B5REC|restore|preserve_nopreserve|622|,
    // tests/eval/golden/aer_b4/b5_misc_pins.log:90-91): `restore, preserve`
    // on an EMPTY stack is r(622), not the generic "1". BUG-922: so is bare
    // `restore` (oracle w2_prog_controlflow pc_pr_none and
    // pc_pr_not_then_restore: 622). The piet2 cell that pinned rc 1 here
    // (test_preserve_restore.jsonl:12) belongs to the corpus archived by
    // BUG-400c, which no gate reads.
    //
    // BUG-922: and so is a restore from a level that preserved nothing while
    // its caller holds a copy: the caller's copy is out of reach and stays
    // where it is (oracle w2ad_prog_preserve_nest pn_d_restore_caller,
    // pn_p_restore_caller, pn_d_rp_caller, pn_r_rp_caller, pn_p_rp_caller:
    // in1 622:4:30, then the caller's own restore brings its copy back).
    return engineErrorBlockRc(
        "restore: no previous preserve — use `preserve` before `restore`", 622);
  }
  return restoreTopFrame(session, rearm);
}

}  // namespace piet::handlers

namespace piet::program {

// BUG-922: the one unwinder (session_state.h I3). It lives beside runRestore
// because it must restore copies that are no longer in the reach of the level
// now running: the level that owned them has concluded.
void restorePreservesDeeperThan(int callerLevel, SessionState& session, ojson& blocks) {
  while (!session.preserveStack.empty() && session.preserveStack.back().level > callerLevel) {
    ojson rb = handlers::restoreTopFrame(session, false);
    if (rb.is_object() && rb.value("type", std::string()) == "error") {
      blocks.push_back(std::move(rb));
      break;
    }
  }
}

}  // namespace piet::program
