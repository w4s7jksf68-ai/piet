// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// SessionState: scratch-file ownership and reclamation, live-session registry (and the destructor), orphan sweep, census.
#include "piet/session_state.h"
#ifndef _WIN32
#include <execinfo.h>
#include <dlfcn.h>
#endif
#include "io/win_compat.h"  // cecon::to_native_path (BUG-050 wide open)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdio>   // C4-L1 window trace (stderr fprintf, env-gated)
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>  // std::greater<> (BUG-381 census sort)
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>

#include "piet/bcal.h"  // bcal::clearCache (resetProcessCaches)
#include "piet/cclass.h"  // BUG-1006: resolveCValue (macroCValue)
#include "piet/counters.h"
#include "piet/eval/errors.h"  // eval::coarseRc (the r(N) of a logged error)
#include "piet/eval/macros.h"  // eScalarKey (eNameOrderFor)
#include "piet/display_hardening.h"  // BUG-718: readVarObservation (macroVarObs)
#include "piet/scratch_seq.h"  // BUG-389: scratchMintSeq (sweep throttle)
#include "piet/storage_type.h"  // stataDefaultFormat (displayFormat)

namespace piet {

namespace {
// True iff `child` is lexically contained in `dir` (component-wise, after
// weakly_canonical) — stricter than a string prefix so ".../piet_native_ab"
// is never treated as under ".../piet_native_a".
bool isContainedIn(const std::filesystem::path& child, const std::filesystem::path& dir) {
  auto d = dir.begin();
  auto c = child.begin();
  for (; d != dir.end(); ++d, ++c) {
    if (c == child.end() || *c != *d) return false;
  }
  return c != child.end();  // child must have at least one component beyond dir
}
}  // namespace

namespace {
// Scratch-budget ceiling in bytes: env PIET_SCRATCH_BUDGET_MB overrides the
// default. "0" means evict immediately (no retained scratch — the eager policy).
// Read fresh each call so a test can flip it; the parse is trivial.
//
// The default was 4096 MB and is 0 as of 2026-08-31 (User ruling), because the
// census that measured what retention BUYS came back with "nothing":
//
//  - Functionally nothing, ONCE THE PIN LIST IS RIGHT — and it was not. The
//    claim "a path only reaches supersededScratch_ after passing every pin
//    exclusion, so nothing can read it again" was written here first and then
//    falsified by this codebase's own selftest within the hour: flipping the
//    default turned 8 checks red. Two pins were missing, and the 4096 MB
//    ceiling had been standing in for them — exclusions (7) LastModel's
//    dataPath/fitDataPath (every post-estimation replay) and (8) currentFile_
//    (a user file that resolves INSIDE the workdir). Both are now in
//    reclaimSupersededScratch with their own locks and seeded-RED certificates.
//    Read the claim as a property this file now MAINTAINS, not one it inherited:
//    a new reader of a superseded path must add its pin, and the eager default
//    is what makes that failure loud instead of a rare long-session mystery.
//  - Nothing measurable in wall. Two 47-artifact corpus runs, same workload:
//    eager 5016 s vs 4096 MB 4975 s — 0.8%, and the runs used different binaries
//    so even that sign is not quotable ([[iobound-storage-regime]]).
//  - It cost 3.9 GiB of peak. 8.916 vs 5.009 GiB high-water; the delta IS the
//    ceiling, to within rounding. Peak is what a user's free space must survive,
//    and the product promise is 10 GiB (internal note GA-SWEEP-ADJ1-MEMO.md A1), so
//    retention was spending 39% of the promise on files nothing reads.
//
// Full attribution: internal note GA-SWEEP-CENSUS-20260822.md ADDENDUM 6 and 7.
std::uintmax_t scratchBudgetBytes() {
  std::uintmax_t mb = 0;  // evict on supersede — retention buys nothing (above)
  if (const char* env = std::getenv("PIET_SCRATCH_BUDGET_MB"); env && *env) {
    char* end = nullptr;
    unsigned long long v = std::strtoull(env, &end, 10);
    if (end != env) mb = static_cast<std::uintmax_t>(v);
  }
  return mb * 1024ull * 1024ull;
}
}  // namespace

// True when `canonicalPath` (weakly canonical) is the on-disk base frame of the live e(sample)
// mask install: the head is still the install frame built from it. See exclusion
// (6c) in reclaimSupersededScratch and the "esample_base" claim in scratchOwners.
bool SessionState::isLiveEsampleBase(const std::string& canonicalPath) const {
  if (!esampleHeadIsInstallOf(esampleLastInstallBase)) return false;
  std::error_code ec;
  const std::filesystem::path bp =
      std::filesystem::weakly_canonical(cecon::to_native_path(esampleLastInstallBase), ec);
  return !ec && bp.u8string() == canonicalPath;
}

// 2.2.0 item E (SPEC II-0 ladder rung b): bounded scratch-budget eviction.
// REGISTER the superseded predecessor as reclaimable (subject to the safety
// exclusions), then evict the OLDEST registered files while the running total
// exceeds the budget. A short mutation burst stays under budget => zero
// deletion I/O on the critical path (Gate ② — an eager unlink contends for
// disk bandwidth with the next mutation's write, regressing p2_load3gen ~0.9s
// on the reference machine); a long session's scratch stays bounded.
//
// Safety exclusions (the "NEVER deletes user files" guarantee): register only
// a regular file that is (1) contained in workdir_ — the session-owned scratch
// dir, so a user's own .hbin/.dta anywhere else is untouchable — and (2) not
// referenced by any live preserve snapshot (preserveStack). Best-effort:
// every filesystem call is error_code'd, nothing throws into load().
void SessionState::reclaimSupersededScratch(const std::string& oldPath, bool abandoningHead) {
  if (oldPath.empty()) return;
  if (scratchTraceOn()) scratchTrace("RECLAIM", oldPath, "");
  // Debug/opt-out escape hatch (defaults ON — reclaiming the scratch
  // accumulation is the point of item E).
  if (const char* off = std::getenv("PIET_NO_SCRATCH_GC"); off && *off) return;

  namespace fs = std::filesystem;
  std::error_code ec;

  fs::path wd = fs::weakly_canonical(cecon::to_native_path(workdir_), ec);
  if (ec) return;
  fs::path op = fs::weakly_canonical(cecon::to_native_path(oldPath), ec);
  if (ec) return;

  // (0) BUG-390 (2026-09-02): never the LIVE head. Every earlier caller handed
  // this a path it had just superseded, so the primitive never needed to ask;
  // the byte-LRU over the `use` memos can evict the entry whose hbin is the
  // dataset the user is looking at right now (the memo for `use big1` is the
  // LRU victim the moment `use big2` memoizes, before big2 is adopted). The
  // path stays owned by the head and is reclaimed by load()'s ordinary
  // supersede when the head moves on.
  if (!abandoningHead && currentHbin_.has_value() && !currentHbin_->empty()) {
    fs::path hp = fs::weakly_canonical(cecon::to_native_path(*currentHbin_), ec);
    if (!ec && hp == op) {
      if (scratchTraceOn()) scratchTrace("SPARE(live-head)", "reclaim", oldPath);
      return;
    }
    if (ec) ec.clear();
  }
  // (1) session-owned scratch only — never a user file outside workdir_.
  if (!isContainedIn(op, wd)) {
    if (scratchTraceOn()) scratchTrace("SPARE(user-file)", "reclaim", oldPath);
    return;
  }
  // (2) never a live preserve snapshot (deep-copy files on the LIFO stack).
  for (const auto& frame : preserveStack) {
    fs::path pp = fs::weakly_canonical(cecon::to_native_path(frame.hbinPath), ec);
    if (!ec && pp == op) {
      if (scratchTraceOn()) scratchTrace("SPARE(preserve)", "reclaim", oldPath);
      return;
    }
  }
  // (3) PP-L2A / K1a: never a LIVE `use` conversion memo. Same shape as (2) —
  // the entry is the pin, so a memo that useConvCachePut has already replaced
  // (or that a stat mismatch retired) is reclaimable on its next supersede. A
  // cached hbin IS superseded on every following mutation (`use foo.dta` then
  // `gen`), so without this exclusion the budget eviction would delete exactly
  // the file the next `use foo.dta` wants to re-open — the cache would degrade
  // to a no-op under load, silently, which is the failure mode a pure
  // exists()-check fallback would hide rather than prevent.
  for (const auto& [src, entry] : useConvCache_) {
    (void)src;
    fs::path cp = fs::weakly_canonical(cecon::to_native_path(entry.hbinPath), ec);
    if (!ec && cp == op) return;
  }
  // (4) PP-L2B / K1b: never a LIVE `use` STASH memo's on-disk twin. For a
  // .dta/.csv source this is the same conversion output exclusion (3) already
  // pins (K1b memoizes alongside K1a), so this is belt-and-braces there; but it
  // makes K1b self-contained — its restore re-adopts hbinPath as a CLEAN head,
  // and a scratch-GC unlink of that twin between the stash and the re-`use`
  // would drop the restore to the dirty-virtual fallback (an extra
  // materialization the pin exists to prevent).
  for (const auto& [src, entry] : useStashCache_) {
    (void)src;
    fs::path cp = fs::weakly_canonical(cecon::to_native_path(entry.hbinPath), ec);
    if (!ec && cp == op) return;
  }
  // (5) P6 frames: never a LIVE frame's hbin. A frame is a named dataset the
  // user still owns — it is superseded the moment the session loads anything
  // else, which for frames is EVERY following command, so without this
  // exclusion the budget eviction deletes exactly the file the next
  // `frame F:`/`frlink`/`frget` needs. Same shape as (2)-(4): the entry is the
  // pin, so a frame that `frame drop` has already forgotten is reclaimable on
  // its next supersede.
  for (const auto& [name, path] : frames) {
    (void)name;
    fs::path fp = fs::weakly_canonical(cecon::to_native_path(path), ec);
    if (!ec && fp == op) return;
  }
  // (6) P6 resampling: never a file the CURRENTLY RUNNING command pinned. A
  // resampling prefix writes several scratch files per replicate, so its own
  // base dataset becomes the oldest entry and would be the first evicted —
  // while the loop is still drawing from it. See SessionState::ScratchPin.
  for (const auto& pinned : scratchPins) {
    fs::path pp2 = fs::weakly_canonical(cecon::to_native_path(pinned), ec);
    if (!ec && pp2 == op) return;
  }
  // (6b) e(sample) T2: never a head an estimation's mask install has just
  // moved off while the command that ran it may still hold the path
  // (SessionState::pinPreInstallHead).
  for (const auto& pinned : preInstallPins_) {
    fs::path pp3 = fs::weakly_canonical(cecon::to_native_path(pinned), ec);
    if (!ec && pp3 == op) return;
    if (ec) ec.clear();
  }
  // (6c) e(sample) T2: never the on-disk BASE frame of the live mask install.
  // While the head is still the install frame built from `esampleLastInstallBase`
  // (esampleHeadIsInstallOf) the next fit and the read-only replays alias the
  // base instead of writing the head to disk (estimation/estimation_sample_install.cpp esampleInstallOnHead,
  // estimation/linear_estimation.cpp's `if` filter source, runViaCobraSession's esample aliases).
  // The base is owned by the head, not by the model: `ereturn clear` (any
  // lastModel_reset) releases the LastModel pin (7) while the head still IS the
  // install of this base, and the crash-floor reclaim (flushDeferredReclaim, at
  // the next materialization) then offers the file with nothing left to spare it;
  // the detached unlink raced the next fit's `native_exists()` check into
  // "[data_io] cannot open .../gen_*.hbin", rc 1 (330-370 of 900 fits in 30
  // runs of a 30-fit `regress ... if ..., vce(...)` loop with an `ereturn clear`
  // between each; 0 with PIET_NO_SCRATCH_GC=1). The
  // entry is the pin: once the head moves (load()), esampleHeadIsInstallOf turns
  // false and the orphan sweep takes the file at the next dispatch exit.
  if (isLiveEsampleBase(op.u8string())) {
    if (scratchTraceOn()) scratchTrace("SPARE(esample-base)", "reclaim", oldPath);
    return;
  }
  // (7) BUG-381: never a file the LIVE estimation still names. `predict`,
  // `test`, `lincom`, `margins`, `boottest` and the resampling prefixes all
  // replay LastModel::dataPath / ::fitDataPath — paths captured when the model
  // was FIT — back to the engine as `--data` / `--fit-data`. Those files are
  // superseded the moment anything mutates the head, which for `regress y x` +
  // `predict a, xb` + `predict b, resid` is the FIRST predict, so without this
  // exclusion the second one reads a path that has already been unlinked.
  //
  // session_state.h says of these fields "setLastModel() replaces the struct
  // wholesale, so none of these can outlive the model they describe". That is
  // an argument about the STRUCT's lifetime and it is correct; it is not an
  // argument about the FILE's, and the two came apart here. The entry is the
  // pin, exactly as in (2)-(6): setLastModel replacing the struct (or
  // lastModel_reset) releases the path on its next supersede.
  //
  // This was always the contract — the 4096 MB budget merely made the window
  // wide enough to hide it. A session that pushed more than the ceiling of
  // scratch between the fit and the post-estimation command hit it anyway.
  if (lastModel_.has_value()) {
    for (const std::string* mp : {&lastModel_->dataPath, &lastModel_->fitDataPath}) {
      if (mp->empty()) continue;
      fs::path lp = fs::weakly_canonical(cecon::to_native_path(*mp), ec);
      if (!ec && lp == op) return;
      if (ec) ec.clear();
    }
  }
  // (8) BUG-381: never the user's OWN loaded file. Exclusion (1) refuses user
  // files by LOCATION — "outside workdir_" — which is the right test only while
  // no user file is inside it. `use <f>` where <f> resolves into the session
  // workdir gives a head that is simultaneously a user file and, by (1)'s test,
  // session scratch; the next `replace` supersedes it and the GC deletes the
  // user's data. currentFile_ is the load's ORIGINAL resolved path, so it is
  // the pin that states ownership directly instead of inferring it from where
  // the file happens to sit.
  if (!currentFile_.empty()) {
    fs::path cf = fs::weakly_canonical(cecon::to_native_path(currentFile_), ec);
    if (!ec && cf == op) return;
    if (ec) ec.clear();
  }
  // Defensive: only track an existing regular file (never a dir/symlink).
  if (!fs::is_regular_file(op, ec)) {
    if (scratchTraceOn()) scratchTrace("SPARE(no-file)", "reclaim", oldPath);
    return;
  }

  std::uintmax_t sz = fs::file_size(op, ec);
  if (ec) sz = 0;
  if (scratchTraceOn()) scratchTrace("REGISTER", "reclaim", op.u8string());
  supersededScratch_.emplace_back(op.u8string(), sz);
  supersededBytes_ += sz;

  // Evict oldest-first while over budget. Eviction is off the critical path
  // (a detached unlink) AND only happens once the retained scratch genuinely
  // exceeds the ceiling — so the common short-burst case never touches disk.
  const std::uintmax_t budget = scratchBudgetBytes();
  while (supersededBytes_ > budget && !supersededScratch_.empty()) {
    auto victim = supersededScratch_.front();
    supersededScratch_.erase(supersededScratch_.begin());
    supersededBytes_ -= std::min(supersededBytes_, victim.second);
    std::thread([p = victim.first]() {
      if (scratchTraceOn()) scratchTrace("UNLINK", "reclaim-thread", p);
      std::error_code rmEc;
      std::filesystem::remove(p, rmEc);  // best-effort — a failed unlink is not fatal.
    }).detach();
  }
}

void SessionState::flushDeferredReclaim() {
  if (deferredReclaim_.empty()) return;
  std::vector<std::string> pending;
  pending.swap(deferredReclaim_);
  for (const auto& p : pending) reclaimSupersededScratch(p);
}

void SessionState::scratchTrace(const char* what, const std::string& a,
                                const std::string& b) {
  if (!scratchTraceOn()) return;
  auto base = [](const std::string& p) {
    size_t i = p.find_last_of('/');
    return i == std::string::npos ? p : p.substr(i + 1);
  };
  std::fprintf(stderr, "[scratch] %s %s <- %s\n", what, base(a).c_str(), base(b).c_str());
}

void SessionState::reapProjectionScratch() {
  if (projectionScratch_.empty()) return;
  const std::string head = currentHbin_.value_or(std::string());
  for (const auto& path : projectionScratch_) {
    if (!head.empty() && path == head) continue;
    if (scratchTraceOn()) scratchTrace("UNLINK", "projection-reap", path);
    std::error_code ec;
    std::filesystem::remove(cecon::to_native_path(path), ec);
  }
  projectionScratch_.clear();
}

// BUG-381 DIAGNOSTIC. See the declaration in session_state.h for why this walks
// rather than reasons from the notes above it.
// BUG-389 (2026-09-02): the one owner table the census reports from and the
// orphan sweep spares from. FIRST claim wins, so the order IS the attribution
// priority: a file that is both the live head and a tracked projection is the
// head, because that is what it is. Every path is entered twice — as written and
// canonicalized — so a caller can look up a directory-walk path without a
// realpath syscall per file.
std::map<std::string, std::string> SessionState::scratchOwners() const {
  namespace fs = std::filesystem;
  std::map<std::string, std::string> owner;
  auto claim = [&](const std::string& p, const char* who) {
    if (p.empty()) return;
    owner.emplace(p, who);
    std::error_code e;
    fs::path c = fs::weakly_canonical(cecon::to_native_path(p), e);
    if (!e) owner.emplace(c.u8string(), who);
  };
  claim(currentHbin_.value_or(std::string()), "head");
  // e(sample) T2: the install head's on-disk base, owned for as long as the head
  // is that install (reclaimSupersededScratch exclusion (6c)). Claimed right
  // after the head so the orphan sweep and the census agree with the reclaim.
  if (esampleHeadIsInstallOf(esampleLastInstallBase)) claim(esampleLastInstallBase, "esample_base");
  for (const auto& f : preserveStack) claim(f.hbinPath, "preserve");
  for (const auto& [k, e] : useConvCache_) { (void)k; claim(e.hbinPath, "use_conv_cache"); claim(e.metaPath, "use_conv_cache"); }
  for (const auto& [k, e] : useStashCache_) { (void)k; claim(e.hbinPath, "use_stash_cache"); claim(e.metaPath, "use_stash_cache"); }
  for (const auto& [n, fp] : frames) { (void)n; claim(fp, "frame"); }
  for (const auto& sp : scratchPins) claim(sp, "scratch_pin");
  for (const auto& pp : preInstallPins_) claim(pp, "pre_install_pin");  // e(sample) T2
  for (const auto& [sp, sz] : supersededScratch_) { (void)sz; claim(sp, "superseded_pending"); }
  for (const auto& pp : projectionScratch_) claim(pp, "projection_pending");
  for (const auto& [d, tp] : mintedTempfiles) { (void)d; claim(tp, "tempfile_pending"); }
  if (lastModel_.has_value()) {
    claim(lastModel_->dataPath, "last_model");
    claim(lastModel_->fitDataPath, "last_model");
  }
  for (const auto& [n, ph] : postfiles) { (void)n; claim(ph.target, "postfile_open"); }
  for (const auto& [n, fh] : fileHandles) { (void)n; claim(fh.path, "file_handle_open"); }
  claim(currentFile_, "user_file");
  // The crash-recovery floor of a virtual resident chain. When the head is a
  // virtual token, the last snapshot written to disk (ensureMaterializedHead,
  // or a real-file load) is what rollbackToFloor() re-adopts if the worker
  // dies; load() parks it on deferredReclaim_ and flushDeferredReclaim()
  // releases it at the NEXT materialization, when a newer floor replaces it.
  // Until then the file is owned — but this table never said so, so: the exit
  // census reported it ORPHAN (`gen str1 s = ""` materializes strmut_2 to read
  // rows, its setcol --out goes virtual, and strmut_2 is left as the floor —
  // probe_b4's ORPHAN n=1, ~65 KB), and for a REGISTERED mint (setobs_*, ...)
  // the dispatch-exit sweep deleted the live floor, so a rollback would have
  // re-adopted a file that no longer exists. Only the current floor is
  // claimed: an older one is on deferredReclaim_ and gets its reclaim there.
  // After user_file so a user's own file that is also the floor reads as the
  // user's.
  claim(lastMaterializedHead_, "crash_floor");
  claim(workdir_ + "/.piet_owner_pid", "owner_marker");
  return owner;
}

namespace {
// Every SessionState alive in this process (see sweepOrphanScratch's header
// note). Dispatch is single-threaded; the mutex only keeps a construct or
// destruct on another thread (a worker harness) from tearing the vector.
std::mutex& liveSessionsMu() {
  static std::mutex mu;
  return mu;
}
std::vector<const SessionState*>& liveSessions() {
  static std::vector<const SessionState*> v;
  return v;
}
}  // namespace

void SessionState::registerLiveSession() {
  std::lock_guard<std::mutex> lk(liveSessionsMu());
  liveSessions().push_back(this);
}

SessionState::~SessionState() {
  std::lock_guard<std::mutex> lk(liveSessionsMu());
  auto& v = liveSessions();
  v.erase(std::remove(v.begin(), v.end(), this), v.end());
}

int SessionState::sweepOrphanScratch(const char* site) {
  namespace fs = std::filesystem;
  if (const char* off = std::getenv("PIET_NO_ORPHAN_SWEEP"); off && *off) return 0;
  // The scratch-GC opt-out (PIET_NO_SCRATCH_GC, honoured by reclaimSupersededScratch) covers
  // this reaper too: a diagnostic that wants to LOOK at the minted files afterwards
  // (piet/native/tests/scratch_path_monotonic.py) must not have them swept from under it.
  if (const char* gc = std::getenv("PIET_NO_SCRATCH_GC"); gc && *gc) return 0;
  // No mint-sequence throttle (first cut had one): a verb can create workdir files
  // without nextPath (recode's mask/tmp pair did) and the following commands may
  // mint nothing, which left those two as the last orphans standing. The walk over
  // a session's handful of files is microseconds; correctness is not worth them.
  mintSeqAtLastSweep_ = scratchMintSeq();
  if (workdir_.empty()) return 0;
  std::error_code ec;
  const fs::path wd = cecon::to_native_path(workdir_);
  if (!fs::is_directory(wd, ec) || ec) return 0;
  std::map<std::string, std::string> owner = scratchOwners();
  {
    // e(sample) T2: a sibling session's files in the SAME workdir are owned.
    std::lock_guard<std::mutex> lk(liveSessionsMu());
    for (const SessionState* other : liveSessions()) {
      if (other == this || other->workdir_ != workdir_) continue;
      for (auto& kv : other->scratchOwners()) owner.emplace(kv.first, "live_session");
    }
  }
  int removed = 0;
  fs::recursive_directory_iterator it(wd, fs::directory_options::skip_permission_denied, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code e;
    const fs::path& p = it->path();
    if (it->is_directory(e) && !e) {
      // Another owner's workdir (a parallel worker, a nested session) is theirs.
      std::error_code e2;
      if (fs::exists(p / ".piet_owner_pid", e2)) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(e) || e) continue;
    if (p.filename() == ".piet_owner_pid") continue;
    const std::string raw = p.u8string();
    // Candidates are ONLY paths this process minted (scratch_seq.h registry). A
    // user's `save` into the workdir, a file another process wrote, anything the
    // registry never saw — never touched, whatever it is named. (First cut swept
    // every unowned file and deleted the selftest's own freshly saved .dta.)
    if (!isRegisteredScratchMint(raw)) continue;
    if (owner.count(raw)) continue;
    std::error_code e3;
    fs::path c = fs::weakly_canonical(p, e3);
    if (!e3 && owner.count(c.u8string())) continue;
    if (scratchTraceOn()) scratchTrace("UNLINK", std::string("orphan-sweep:") + site, raw);
    std::error_code e4;
    if (fs::remove(p, e4) && !e4) {
      ++removed;
      forgetScratchMint(raw);
    }
  }
  lastOrphanSweepRemoved_ = removed;
  return removed;
}

std::string SessionState::scratchCensusJson() const {
  namespace fs = std::filesystem;
  std::error_code ec;
  const std::map<std::string, std::string> owner = scratchOwners();

  struct Agg { long long n = 0; long long bytes = 0; };
  std::map<std::string, Agg> byClass;
  std::map<std::string, Agg> orphanShape;  // basename with each digit run -> '#'
  std::vector<std::pair<long long, std::string>> orphanBig;
  long long totalN = 0, totalBytes = 0;

  fs::path wd = fs::weakly_canonical(cecon::to_native_path(workdir_), ec);
  if (ec) return std::string("{\"error\":\"workdir not canonicalizable\"}");
  fs::recursive_directory_iterator it(wd, fs::directory_options::skip_permission_denied, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code e;
    if (!it->is_regular_file(e) || e) continue;
    long long sz = 0;
    { std::error_code e2; auto v = fs::file_size(it->path(), e2); if (!e2) sz = (long long)v; }
    std::error_code e3;
    fs::path cp = fs::weakly_canonical(it->path(), e3);
    const std::string key = e3 ? it->path().u8string() : cp.u8string();
    auto o = owner.find(key);
    const std::string cls = (o == owner.end()) ? std::string("ORPHAN") : o->second;
    auto& a = byClass[cls];
    a.n++;
    a.bytes += sz;
    totalN++;
    totalBytes += sz;
    if (cls == "ORPHAN") {
      const std::string b = it->path().filename().u8string();
      std::string shape;
      bool inNum = false;
      for (char ch : b) {
        if (ch >= '0' && ch <= '9') {
          if (!inNum) { shape += '#'; inNum = true; }
        } else {
          shape += ch;
          inNum = false;
        }
      }
      auto& s2 = orphanShape[shape];
      s2.n++;
      s2.bytes += sz;
      orphanBig.emplace_back(sz, b);
    }
  }

  nlohmann::ordered_json out = nlohmann::ordered_json::object();
  out["workdir"] = workdir_;
  out["files"] = totalN;
  out["bytes"] = totalBytes;
  nlohmann::ordered_json cls = nlohmann::ordered_json::object();
  for (const auto& [k, a] : byClass) {
    nlohmann::ordered_json r = nlohmann::ordered_json::object();
    r["n"] = a.n;
    r["bytes"] = a.bytes;
    cls[k] = r;
  }
  out["by_class"] = cls;
  // The orphan SHAPE table is the one that names a producer: a basename with its
  // digit runs collapsed is that call site's minting template.
  std::vector<std::pair<long long, std::string>> shapes;
  shapes.reserve(orphanShape.size());
  for (const auto& [k, a] : orphanShape) shapes.emplace_back(a.bytes, k);
  std::sort(shapes.begin(), shapes.end(), std::greater<>());
  nlohmann::ordered_json sh = nlohmann::ordered_json::array();
  for (size_t i = 0; i < shapes.size() && i < 25; ++i) {
    nlohmann::ordered_json r = nlohmann::ordered_json::object();
    r["shape"] = shapes[i].second;
    r["n"] = orphanShape[shapes[i].second].n;
    r["bytes"] = shapes[i].first;
    sh.push_back(r);
  }
  out["orphan_shapes"] = sh;
  std::sort(orphanBig.begin(), orphanBig.end(), std::greater<>());
  nlohmann::ordered_json big = nlohmann::ordered_json::array();
  for (size_t i = 0; i < orphanBig.size() && i < 15; ++i) {
    nlohmann::ordered_json r = nlohmann::ordered_json::object();
    r["name"] = orphanBig[i].second;
    r["bytes"] = orphanBig[i].first;
    big.push_back(r);
  }
  out["orphan_largest"] = big;
  return out.dump();
}

}  // namespace piet
