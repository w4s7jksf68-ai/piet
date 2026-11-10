// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/rng_mt64.h"       // Stata-exact session RNG stream (SeedSlot::stream)
#include "piet/cobra_session.h"  // Stage 1b: persistent `cobra serve` worker + frame metadata
#include "piet/counters.h"       // liveNamedBytesPeakSample() save/restore of the counter globals
#include "piet/datastore.h"
#include "piet/eval/session.h"
#include "piet/serset.h"  // serset-twoway SS: the graph-owned serset store
#include "vendor/nlohmann/json.hpp"

namespace piet {
// B1 (1.16.0) run-progress schema — C++ port of session.py's RunProgress.
// Frozen wire schema (CONTRACT.md v1.11.0, contract_B). M3a: doexec.cpp's
// executor publishes this per top-level item; a headless (--run-block /
// gate) run publishes to the SAME struct but nothing reads it — a live
// GET /api/run_progress consumer arrives at the M4 frontend swap.
struct RunProgress {
  bool running = false;
  int currentLine = 0;
  int totalLines = 0;
  std::string currentCmd;             // truncated to ~120 chars
  double startedAt = 0.0;              // monotonic seconds at run start
  long long elapsedMs = 0;             // frozen at completion
  int blocksReady = 0;
  std::optional<std::string> doPath;   // resolved abs path; unset for do_string
  bool cancelRequested = false;
  std::optional<std::string> stoppedReason;  // "completed"|"cancelled"|"error"

  nlohmann::ordered_json asDict() const;
};

// session.py's LastModel — PIET's e() equivalent, sufficient for
// test/lincom/predict/margins/estat to reconstruct the cobra call.
struct LastModel {
  std::string cobraCmd;
  // BUG-458 (2026-09-08): the STATA spelling of the command that produced this
  // model ("regress", "reghdfe"), where cobraCmd is the engine's ("feols").
  // e(cmd) is published from THIS field when it is set and from cobraCmd when
  // it is not, so a handler that has not learned its Stata name keeps printing
  // exactly what `ereturn list` printed before. Never read by the replay
  // machinery (test/lincom/margins/estat keep keying on cobraCmd).
  std::string stataCmd;
  // False for an e-class program that never ran `ereturn post` (rddensity:
  // `ereturn clear` + scalars): its e() is live, but
  // estimates store / eststo refuse it r(301) "last estimates not found"
  // (estimates store on an unposted e()). runEststo reads it.
  bool ePosted = true;
  std::string y;
  std::vector<std::string> x;
  std::vector<std::string> fe;
  std::string vcov = "iid";
  // Stata's spelling of e(vce) when it is not the engine-derived one
  // setLastModel() publishes from `vcov` (areg/reghdfe `ols`, ivreghdfe
  // `robust cluster`, xtpoisson/xtlogit fe `oim`; "" erases e(vce)). Carried on
  // the model so a later setLastModel() on the same model (setEstHandleM4,
  // reached by test/lincom/predict) republishes it instead of `iid`.
  std::optional<std::string> eVce;
  std::vector<std::string> cluster;
  std::optional<std::string> ivar;
  std::string depvar;
  std::string dataPath;
  // W:P5 fit-sample fidelity (BUG-163): the restriction and weights the FIT
  // actually ran under, so post-estimation refits (test/lincom/nlcom/margins/
  // boottest) reconstruct the fitted sample instead of silently refitting on
  // the full dataPath — measured: after `regress ... if region<=4 in 1/4000`
  // (n=3,106) a `test` refit answered from n=4,649. Two filter forms, matching
  // the two styles this codebase uses:
  //  - fitDataPath: a materialized if/in scratch file (runRegress's
  //    applyIfAndInFilter output). Empty => the fit ran on dataPath itself.
  //  - fitKeepif: the --keepif expression passed straight to the engine
  //    (buildEstimationArgvM4 style). Empty => none.
  // fitWeight/fitWtype mirror --weight/--wtype for the same reason (replayed
  // only into engine CLIs that accept them). setLastModel() replaces the
  // struct wholesale, so none of these can outlive the model they describe
  // (same safety argument as the §31 handle below).
  std::string fitDataPath;
  std::string fitKeepif;
  std::string fitWeight;
  std::string fitWtype;
  // The weight expression as typed, for e(wexp) (Stata stores "= " + the text
  // after `=`, leading blanks removed: `[aw=  wf ]` -> "= wf ", w2aj_qualifiers
  // qf_rw_aw_wide). Empty => fitWeight. setLastModel() fills it from
  // commandWexp when the fit left it empty.
  std::string fitWexp;
  // csdid / DiD post-estimation context (not exercised by M2's vertical
  // slice — csdid handlers land at M3 — but the fields are ported now so
  // LastModel is a complete, not partial, struct).
  std::optional<std::string> timevar;
  std::optional<std::string> gvar;
  std::optional<std::string> method;
  std::optional<std::string> comparison;
  std::optional<std::string> pscoretrim;
  // csdid `long2` (e(typebase) long2): the `estat event, window()` re-run must
  // forward it, or the long-difference cells silently become the short ones
  // (w2ag_csdid_drdid.log:670-683 ag_l2ny_estore/ev).
  bool long2 = false;
  std::optional<nlohmann::ordered_json> result;  // cached raw cobra output JSON
  // IV/LIML endogenous-regressor / instrument role split (BETA Phase G,
  // estat endogenous/overid wiring). `x` above conflates endog+exog into
  // ONE list (endog first, per Stata's own coeftable order) for the
  // OLS-refit approximation test/lincom/margins/predict already share —
  // see runIvRegress's own comment — which loses the information needed to
  // reconstruct a genuine `cobra iv-diag` call (a SEPARATE cobra command
  // from `cobra iv`, computing Durbin/Wu-Hausman/Sargan/Basmann under the
  // classical iid VCE — apps/cmds/cmds_diag.cpp::cmd_ivdiag on the cobra
  // side). These three preserve the ORIGINAL (endog = instruments) block's
  // role split so `estat endogenous`/`estat overid` can invoke iv-diag
  // with the right --endog/--x/--instruments. Populated by runIvRegress
  // for both ivregress and ivreghdfe/liml (cheap to always fill; the
  // cobraCmd=="iv" gate in runEstat's endogenous/overid branch is what
  // actually restricts iv-diag to plain `iv`, not liml, matching
  // estat firststage's existing gate).
  std::vector<std::string> ivEndog;
  std::vector<std::string> ivExog;
  std::vector<std::string> ivInstruments;

  // contract_A §31 session-estimation-store handle ("est:<nonce>:<n>"), or
  // empty when this model has none. OPAQUE — never parsed, constructed or
  // predicted. Filled by estimation_m4.cpp's post-estimation path from the
  // `est.handle` the engine mints on the FIRST lincom/nlcom/test after a fit;
  // every subsequent combination on the same model addresses the stored
  // (names, β, V) tuple through it instead of re-specifying the model and
  // making the engine fit it again.
  //
  // Living on LastModel — rather than on the session or on the worker — is the
  // load-bearing safety property, not a convenience. setLastModel() replaces
  // this struct WHOLESALE, so fitting a new model necessarily resets the handle
  // to empty: a handle can never outlive the model it was minted for and be
  // spent on a different one. That is the single way this optimization could
  // return a confidently wrong answer (model A's combination reported for model
  // B), and the struct's own lifetime is what forecloses it. The engine's
  // per-process nonce is the second, independent guard: a handle from an
  // earlier worker resolves nowhere rather than resolving to the wrong model.
  std::string estHandle;

  // e(sample) T2 (internal note esample/SPEC.md §4.2). Whether this
  // model's estimation sample lives on the working frame as the hidden f64
  // column `__piet_esample__` (piet/datastore.h kEsampleColumn):
  //   kColumn      -- the fit handler wrote the engine's admitted rows
  //                   (--esample-out) into that column BEFORE setLastModel.
  //                   An admitted row holds the install's SERIAL
  //                   (esampleSerial below), any other row 0; rows added later
  //                   carry missing. The row reading is
  //                   `(__piet_esample__==SERIAL)`, which reads both as 0.
  //   kUnavailable -- the handler has no engine mask (not wired, or a
  //                   restored/used model whose mask was not carried): e(sample)
  //                   is a named r(198), never a guess.
  //   kStale       -- `estimates restore` of results whose `_est_NAME` variable
  //                   is no longer on the data (T3); a named r(198).
  //   kNone        -- the state of "no LastModel at all", whose producers are a
  //                   fresh session and SessionState::lastModel_reset()
  //                   (`ereturn clear`, `ereturn post`, `clear all`/`clear
  //                   results`), and the one model state `estimates use` sets:
  //                   results read from a file carry no e(sample) (T3). e(sample)
  //                   is then 0 (w2_esample.log A1-A5, G11, :1531 G6 `0|0|0|28|`).
  //                   See SessionState::esampleReading.
  // The DEFAULT is kUnavailable: setLastModel() replaces the struct wholesale,
  // so a fit handler that never sets the field must not answer 0 or 1.
  enum class EsampleState { kNone, kColumn, kUnavailable, kStale };
  EsampleState esample = EsampleState::kUnavailable;
  // kColumn only: which install wrote the mask (SessionState::
  // esampleColumnInstalls at that install, >= 1). The column follows the rows,
  // so the working frame can come to carry ANOTHER fit's mask while this model
  // is live -- a resampling prefix leaves its last replicate's model and
  // returns to the frame it started from (handlers_resample.cpp). The serial in
  // the cells is what tells the two apart (esampleReading refuses a mismatch).
  unsigned long long esampleSerial = 0;
  // kColumn only: the working frame the install produced (the fit's frame plus
  // the mask column). dataPath stays the frame the fit READ -- the path every
  // refit (predict/test/lincom/margins, `estimates save`/`use`) replays -- so
  // the install does not change what those verbs see; this second path only
  // lets predict tell "the head is still the fit's frame" (handlers' headMoved).
  std::string esampleInstallPath;
  // The fit kept its singleton fixed-effect groups (`--keep-singletons`): areg
  // and xtreg,fe always do, as Stata does (w2_esample.log:897 D3 e(N) 59 / N_g
  // 11, :925 D4 59, :1957 H8 58), and reghdfe with `keepsingletons`. Every
  // engine refit forwards it (estimation_m4c.cpp fitConventionFlagsM4) so the
  // refit's sample is the fit's.
  bool keepSingletons = false;
  // BUG-1067: the fit omitted the LATER member of each collinear set
  // (`--collinear-forward`), which implements reghdfe, areg and xtreg,fe per StataBE
  // probes (z = 2*x: `x o.z`; regress keeps the engine's pivoted `o.x z`). Every
  // engine refit forwards it (fitConventionFlagsM4) so the refit's coefficients
  // are the fit's.
  bool collinearForward = false;

  // eb-order T-P3 part 0 (internal note eb-order/SPEC.md):
  // which fit this e() is. A fresh construction mints a new serial and a copy
  // keeps it, so setLastModel can tell a NEW e() (a fit; clears the previous
  // fit's estimation block and posted e(b)/e(V)) from a copy-modify-set of the
  // live one (setEstHandleM4, the `estat event` re-run, erepost, estimates
  // restore of the live store), which keeps them.
  std::uint64_t fitSerial = nextFitSerial();
  static std::uint64_t nextFitSerial() {
    static std::atomic<std::uint64_t> seq{0};
    return ++seq;
  }
};

struct HistoryEntry {
  std::string cmd;
  double ts = 0.0;
  bool success = false;
};

// program.py's ProgramDef — a captured `program define … end` block.
//   name   — canonical name (case as written in `program define`).
//   rclass — true if declared `, rclass` (flushes returnStaging on return).
//   body   — post-preprocess lines (continuation-joined, comment-stripped;
//            empty lines preserved for approximate line-number context).
//   ado    — the command whose `<ado>.ado` autoload defined it; empty for a
//            program defined in a do-file or interactively.
struct ProgramDef {
  std::string name;
  bool rclass = false;
  std::vector<std::string> body;
  std::string ado;
};

// Per-variable editor metadata (session._var_meta[name]): label / format /
// stata_type — whichever keys are present (mirrors the Python dict-of-dict).
struct VarMeta {
  std::optional<std::string> label;
  std::optional<std::string> format;
  std::optional<std::string> stataType;
};

// One Stata `char` entry — `char varname[charname] value` or the dataset-level
// `char _dta[charname] value` (varname literal "_dta"). Flat triple, byte-for-
// byte the same shape as cobra's import-dta --out-meta / export-dta --in-meta
// `characteristics` JSON array (apps/cmds/cmds_data.cpp) and src/io's
// DtaChar/DtaWriteChar, so the round trip needs no reshaping at either hop.
struct DtaCharacteristic {
  std::string varname;
  std::string charname;
  std::string value;
};

// preserve_restore.py's PreserveFrame — one snapshot on the LIFO preserve
// stack. hbinPath is a file-copy of the hbin at snapshot time (deep copy);
// the display-layer maps are value-copied (std::map's copy ctor is already a
// deep copy for these plain-value maps, mirroring Python's
// `{k: dict(v) for k, v in ...}` comprehensions).
struct PreserveFrame {
  std::string hbinPath;   // clone/byte-copy fallback plane: a real file snapshot.
  std::string stashName;  // §27 resident plane: the worker's stash key (no file).
  // BUG-1139: `preserve` with no dataset loaded preserves the empty dataset
  // (StataBE 19.5: rc 0, and `restore` brings back _N 0 with no variables), so
  // the frame holds no snapshot and restoring it clears what was built since.
  bool noData = false;
  std::map<std::string, VarMeta> varMeta;
  // The implicit float of an untyped creation (SessionState::implicitFloatVars)
  // is the other half of a variable's type claim, so it is kept with varMeta.
  std::set<std::string> implicitFloatVars;
  std::map<std::string, std::map<std::string, std::string>> labelDefs;
  std::map<std::string, std::string> varLblname;
  std::map<std::string, std::map<int, std::string>> valueLabels;
  std::string dataLabel;                          // dataset label (Stata `label data`)
  std::vector<DtaCharacteristic> characteristics;  // Stata `char` table
  // BUG-922: the preserve level (SessionState::preserveLevel) that ran
  // `preserve`. A frame belongs to that level for its whole life:
  //   I1 only pushPreserveSnapshot writes this field, with the level of the
  //      `preserve` (a `restore, preserve` re-arms with the popped frame's
  //      level, never the current one);
  //   I2 levels never decrease from the bottom of preserveStack to the top
  //      and never exceed preserveLevel(), so "this level holds a copy" is
  //      SessionState::ownPreserveFrame() != nullptr;
  //   I3 when a program returns or a nested do/run file or `parallel` child
  //      concludes, on success or error, every frame deeper than the caller's
  //      level is restored and popped by program::restorePreservesDeeperThan,
  //      the one unwinder.
  // Only the owning level may restore, cancel or re-arm its frame: a deeper
  // level's `restore` in any form is r(622) and leaves the caller's copy alone
  // (oracle w2ad_prog_preserve_nest pn_*_restore_caller, pn_*_not_caller,
  // pn_*_rp_caller: in1 622:4:30). 0 = the top-level run.
  int level = 0;
};

// postfile.py's PostHandle — an open `postfile` collector. names/types are
// parallel arrays (types: byte|int|long|float|double|str<N>|strL, lowercase).
// rows accumulate as (kind, text) pairs — kind "numeric" stores the Python
// repr()-equivalent text already formatted by the post-expr evaluator (M3b:
// postfile.cpp is the C++ arithmetic-evaluator port), "string" stores the
// raw string. postclose() factorizes string columns and writes an hbin via
// a `cobra convert` round-trip (native hbin WRITE has no direct API in this
// monorepo's linked columnar.h — read-only; see postfile.cpp's notes).
struct PostHandle {
  std::vector<std::string> names;
  std::vector<std::string> types;
  std::string target;
  std::vector<std::vector<std::pair<std::string, std::string>>> rows;  // (kind, text) per cell

  bool isStringCol(size_t idx) const {
    if (idx >= types.size()) return false;
    std::string t = types[idx];
    return t.size() >= 3 && (t[0] == 's' || t[0] == 'S') && (t[1] == 't' || t[1] == 'T') &&
           (t[2] == 'r' || t[2] == 'R');
  }
  size_t nCols() const { return names.size(); }
};

// What `log off` / `log on` / `log close` asked of an SMCL log. Stata records
// the command line and only then the paused/closed footer (or, for `log on`,
// the resumed header without the line); PIET's blocks reach logTee() AFTER the
// handler ran, so each request waits in LogState::ops until the tee meets the
// command's own echo line (SessionState::logTee). Text logs never use this:
// they stop and start at once.
enum class LogOp { Pause, Resume, Close };

// BUG-028 / BS-006: one open `log using` file. path is the resolved
// absolute path (post handlers_m3.cpp::runLog's cwd-relative resolution);
// stream is held open for the log's whole lifetime (SessionState::openLog/
// closeLog own it); paused mirrors Stata's `log off`/`log on` (the stream
// stays open but writes are suspended while true). The text format keeps its
// original plain transcript; the SMCL format (`smcl` below) is written by the
// writer in session_state.cpp -- see the comment above logTee() there for the
// byte layout and what was identified against StataBE.
// W:P8: `name` is Stata's log name — empty for the unnamed log opened by a
// plain `log using`, non-empty for `log using FILE, name(X)`. Several logs may
// be open at once (Stata tees output to all of them), which is why
// SessionState holds a vector rather than a single slot.
struct LogState {
  std::string path;
  std::ofstream stream;
  bool paused = false;
  std::string name;
  // Dispatch depth at which this log was opened. The unnamed log keeps the
  // original "outermost frame only" rule (depth 1) so every pre-existing
  // fixture is byte-unchanged; a NAMED log records the frames at its own
  // depth, which is what makes `log using …, name(X)` work inside a do-file:
  // the outermost tee fires once, AFTER the whole file has run, by which time
  // a log opened and closed mid-file is long gone. Frames deeper than `depth`
  // are not teed separately — their blocks are already flattened into the
  // depth-`depth` frame's result, so they would double-write.
  int depth = 1;
  // -- SMCL format only (all inert for a text log) ----------------------------
  bool smcl = false;
  // The {txt}/{res}/{com}/{err} mode the stream is currently in, as the
  // letters t/r/c/e ('?' = unknown): Stata writes a mode tag only where the
  // mode changes.
  char smclMode = 'r';
  // The next command echo at this log's depth is the `log using` line itself,
  // which Stata does not record (the log was not on yet).
  bool skipNextEcho = false;
  // Requests from the handlers (`log off` / `log on` / `log close`) whose
  // command line the tee has not reached. For an SMCL log `paused` is the
  // state the TEE has written up to; the handlers' effective state is `paused`
  // with these applied in order.
  std::vector<LogOp> ops;
};

// BS-124 (BETA campaign C4): one `timer` stopwatch slot (Stata's own
// `timer on/off/clear/list N`, N in 1..100). Monotonic clock only (never
// wall-clock) — matches Stata's own elapsed-time semantics. `running` +
// `startedAt` are transient (only meaningful between an `on` and its
// matching `off`); `totalSeconds`/`count` accumulate across on/off cycles
// so `timer list` can report total / count = average, oracle-pinned
// format (see handlers_m3.cpp::runTimer's doc comment for the exact
// column widths).
struct TimerSlot {
  bool running = false;
  std::chrono::steady_clock::time_point startedAt;
  double totalSeconds = 0.0;
  long long count = 0;
};

// PP-L2A / K1a (PINGPONG-SCOPE-brief §2 K1a): one memo of a completed
// `.dta`/`.csv` -> scratch-hbin conversion, keyed in useConvCache_ by the
// RESOLVED source path. `statKey` is the whole guard: (inode, size, mtime)
// of the source at conversion time — a re-`use` reuses hbinPath/metaPath only
// when a fresh stat of the source produces the byte-identical key. Anything
// else (a stat failure, any field moved, a vanished scratch file, the
// PIET_NO_USE_CACHE escape) falls through to the ordinary conversion, so the
// cache is a pure fast path with no semantic surface of its own.
struct UseConvCacheEntry {
  std::string statKey;   // "" is never stored — an unkeyable source is not cached
  std::string hbinPath;  // conversion output inside workdir_ (read-only re-open)
  std::string metaPath;  // .dta label sidecar; "" for the .csv/.tsv branch
};

// PP-L2B / K1b (PINGPONG-SCOPE-brief §2 K1b): one memo of a `use`d source whose
// resident frame was stashed in the worker's RAM (§27 frameStash) right after
// the disk load. A re-`use` of the UNCHANGED source restores it via frameUnstash
// (O(columns) RAM, no disk open, no conversion, no re-hash) instead of the
// convert+load path — the layer K1a (UseConvCacheEntry) cannot reach because K1a
// still re-opens the hbin from disk. Keyed in useStashCache_ by the RESOLVED
// source path; `statKey` is the WHOLE guard, identical to K1a's (inode,size,
// mtime) — any mismatch, a stat failure, a dead stash (worker restart), or the
// PIET_NO_USE_CACHE escape all fall through to the disk path, so the cache adds
// no semantic surface. `stashName` is a client-owned "__usecache:<src>" name
// (STABLE per source so a changed source re-stashes in place, freeing the old
// frame's RAM — the engine has no drop-stash-by-name op, and unstash-to-drop
// would restore it as a side effect; the stat guard lives entirely here on the
// PIET side, so the name need not carry the stat). `hbinPath` is the byte-twin
// on disk the restore re-adopts as a CLEAN head (zero-write card build — the
// residentRestore +7% lesson) and the crash-recovery floor if the stash dies.
struct UseStashCacheEntry {
  std::string statKey;    // "" is never stored — an unkeyable source is not cached
  std::string stashName;  // "__usecache:<resolved-src>" — the worker-RAM stash key
  std::string hbinPath;   // on-disk byte-twin: clean-head re-adopt + recovery floor
  std::string metaPath;   // .dta label sidecar to re-apply; "" for the .csv/.tsv/.hbin path
};

}
