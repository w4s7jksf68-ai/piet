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
#include "handlers_data_verbs_internal.h"
namespace {
const piet::eval::OptVocab& kVocabOrder = piet::eval::registerOptVocab(
    {"order"}, {{"first", "last", "before",
                                                                   "after"},
     {}});
const piet::eval::OptVocab& kVocabReshape = piet::eval::registerOptVocab(
    {"reshape"}, {{"i", "j"},
     {}});
// 2026-09-10: `generate`, `update` and `replace` REMOVED from this row's unimplemented set.
// They are implemented — `handlers_data_verbs2.cpp`'s kVocabMerge (the one `runMerge` actually
// reads) carries all three in `known`, and this build answers Stata's cells exactly:
// 74 non-missing after update, `_merge==4` on the 5 filled rows, 69 matched, `mflag` created and
// `_merge` absent (tests/eval/golden/aer_b4/b5_riders_pins.log).
//
// This registration is DEAD CODE — there is no runMerge in this translation unit — but
// `registerOptVocab` UNIONS same-named registrations across translation units, so its
// unimplemented set repolluted the merged vocabulary even though `known` wins at runtime. The
// live surface was therefore correct while the CENSUS, which reads the vocabulary and not the
// code, counted all 83 corpus uses of `merge, update`/`merge, gen()` as gaps. A vocabulary row is
// not documentation: something reads it, and here that something was the instrument measuring us.
const piet::eval::OptVocab& kVocabMerge = piet::eval::registerOptVocab(
    {"merge"}, {{"keepusing", "nogenerate",
                                                                   "keep", "assert"},
     {"nolabel", "nonotes", "noreport", "force", "sorted"}});
const piet::eval::OptVocab& kVocabTsfill = piet::eval::registerOptVocab(
    {"tsfill"}, {{"full"},
     {}});
}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

// R2: the shared helpers that lived here moved VERBATIM to
// handlers_data_verbs_internal.h (dv_detail) — three TUs consume them now.
using namespace dv_detail;

// ---------------------------------------------------------------------------
// save FILE [, replace] — C++ port of dispatcher.py's save_data handler.
// Persists the current dataset by copying the session hbin to FILE
// (in-memory dataset unchanged, Stata-faithful).
ojson runSaveData(const std::string& filepathArg, bool replace, SessionState& session) {
  // C4-L2A (D1 card plane, brief §1 save row): this entry check needs only
  // "is a dataset loaded", which is hasData() — the old
  // `session.datastore() == nullptr` half was logically REDUNDANT (datastore()
  // returns nullptr exactly when currentHbin_ is unset, session_state.h) yet
  // forced a full Datastore open (mmap + whole-file FNV) and, on a dirty
  // virtual head, the EMH frame snapshot, before EVERY save — on the
  // minted-tempfile virtual-save path below that construct is 100% pure waste
  // (residentSaveVirtual snapshots worker RAM, never reads the mmap). The
  // real-file copy path gets its on-disk source via the explicit
  // ensureMaterializedHead below instead. Escape arm (PIET_NO_RESIDENT_READ=1)
  // keeps the old entry verbatim — construct, EMH and the BUG-124 throw
  // semantics included — so the RED/A-B arm reproduces pre-D1 behavior.
  if (piet::residentReadDisabled()) {
    if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
      return engineErrorBlock("save: no dataset in memory");
    }
  } else if (!session.hasData()) {
    return engineErrorBlock("save: no dataset in memory");
  }
  std::string rawFp = trim(filepathArg);
  if (rawFp.size() >= 2 && ((rawFp.front() == '"' && rawFp.back() == '"') ||
                            (rawFp.front() == '\'' && rawFp.back() == '\''))) {
    rawFp = rawFp.substr(1, rawFp.size() - 2);
  }

  // BUG A FIX (W3 fuzzer stabilization, fail_024): this used to byte-copy
  // the session's internal .hbin straight to ANY destination name, INCLUDING
  // a `.dta` extension — a silent format lie (Doctrine I-3: never silently
  // downgrade/mislabel a request). The bytes ARE hbin, not a real Stata
  // .dta file, so a later `use SAME.dta` died with a cryptic engine error
  // ("unsupported legacy format version 67" — the hbin magic misread as a
  // .dta header byte). piet/native has no .dta WRITER at all (only
  // `import-dta`, a READER — see apps/cecon_cli.cpp's full command table)
  // and no CSV-export verb either (grepped: no "export"/"outsheet" anywhere
  // in this tree) — `.hbin` is the ONLY format this build can honestly
  // write. Real Stata defaults an extensionless `save` to `.dta`; since we
  // can't honor that default, substitute our OWN real, working format and
  // say so in the returned filename (never silently write one format's
  // bytes under a name that implies another).
  std::string rawExt = extOf(rawFp);
  std::string effectiveFp = rawFp;
  if (rawExt.empty()) {
    effectiveFp = rawFp + ".hbin";
    rawExt = ".hbin";
  }
  // BUG-085 FIX (SE-044/045): `.tmp` is an honest native-hbin write target,
  // NOT a foreign format. It is the suffix runTempfile() mints for its locals
  // (handlers_m3.cpp:761 → `<tempdir>/NAME.tmp`), so the classroom-core
  // checkpoint idiom `tempfile p` → `save "\`p'"` → `use "\`p'"` writes and
  // reads native content under a `.tmp` name. The BUG A honesty rule (below)
  // only bars writing hbin bytes under a name that *implies another format*
  // (e.g. `.dta`); `.tmp` implies nothing, and `use` closes the round-trip by
  // content-sniffing the hbin magic (looksLikeHbin, handlers.cpp:runUse). The
  // 07-09 "honest .dta save" fix (@ad23151) over-blocked this — regression.
  // Keep the `.dta` rejection (its correct half); accept `.tmp` alongside
  // `.hbin`. Do NOT change runTempfile's minted suffix (postfile.cpp couples).
  // P1-A3 (W1-D3 / WP-14 product face): `.dta` IS a save format now. The BUG A
  // note above says PIET "has no .dta WRITER at all" — true when it was written,
  // false since cobra's `export-dta` landed (format 118, src/io/dta_writer.h),
  // so the honest-reject that stood in for the missing writer is RETIRED IN
  // FAVOUR OF A REAL WRITE, not relaxed: the honesty invariant it protected —
  // never write one format's bytes under a name implying another — is now
  // satisfied by producing genuine Stata bytes instead of by refusing. The
  // `.dta` producer lives in the write block below; every other extension keeps
  // the generic reject, and the EXTENSIONLESS default deliberately stays
  // `.hbin` (Stata's own default is `.dta`, but flipping it would re-point the
  // `tempfile`/checkpoint idiom and every pinned `save NAME` transcript — a
  // behavior row of its own, not a rider on this one).
  const bool isDta = (rawExt == ".dta");
  if (!isDta && rawExt != ".hbin" && rawExt != ".tmp") {
    std::string suggested = rawFp.substr(0, rawFp.size() - rawExt.size()) + ".hbin";
    return engineErrorBlock(
        "save: `" + rawExt + "` is not a save format this version of PIET can write. The only "
        "format supported is the native `.hbin`: use `save " + suggested +
        (replace ? ", replace" : "") + "`.");
  }

  // Resolve against session.cwd, NOT session.workdir() (parity FIX, native-
  // paths stabilization item 2): `save x.hbin` now lands next to the user's
  // own files (cwd), matching real Stata — session.workdir() is the
  // internal hbin-chain scratch dir and was never a user-meaningful
  // location for a relative `save`.
  std::string dest = resolveUserPath(session, effectiveFp);
  if (fileExists(dest) && !replace) {
    return engineErrorBlock("file " + effectiveFp + " already exists (specify option `replace`)");
  }
  // MAXPERF Stage 2 (item 2): a genuine tempfile save — a `.tmp` under the
  // session's own minted tempdir (runTempfile) — is an EPHEMERAL session
  // artifact. Route it to the engine's virtual-file table (save_virtual: an
  // O(columns) RAM snapshot, NO full-dataset byte-copy, no disk write) when the
  // resident plane is active. A later `use \`x'` restores it from RAM via
  // runUse's isVirtualSave() fast path. Falls back to the real byte-copy below
  // when the resident plane is unavailable, so a stateless `use` still finds a
  // file on disk. A NON-tempfile `save` (a user artifact) ALWAYS writes a real
  // file — user-visible bytes must exist on disk.
  bool isMintedTempfile = rawExt == ".tmp" && session.runTempdir.has_value() &&
                          dest.rfind(*session.runTempdir + "/", 0) == 0;
  if (isMintedTempfile && session.residentSaveVirtual(dest)) {
    // BUG-198: the virtual arm writes no file, so there is no header to state
    // the session's format/label overrides in. Park them beside the entry so
    // the `use` that restores it re-applies exactly what the disk arm would
    // have round-tripped through the header (both sides call hbinMetaPatch, so
    // the two planes agree by construction rather than by inspection).
    // Costs one card read, and ONLY when the session has an opinion at all —
    // hasHbinMetaOverrides answers "no" from an empty varMeta without a walk,
    // which is the whole tempfile-checkpoint hot path.
    if (!session.varMeta.empty()) {
      std::vector<VarInfo> ownedCard;
      const std::vector<VarInfo>* card = session.residentReadCard();
      if (card == nullptr) {
        if (Datastore* ds = session.datastore()) {
          ownedCard = ds->varlist();
          card = &ownedCard;
        }
      }
      if (card != nullptr) session.noteVirtualSaveMeta(dest, session.hbinMetaPatch(*card));
    }
    // BUG-659: nor a char table to state the ts declaration in (see the disk
    // arm's twin below).
    session.noteCheckpointTsDecl(dest);
    session.datasetChanged = false;  // BS-091: a successful save resets c(changed).
    ojson out;
    out["type"] = "echo";
    out["text"] = "file " + effectiveFp + " saved";
    return out;
  }
  // BUG-138 (CRITICAL) + L5-IOSAVE, one change: NEVER open `dest` for write.
  //
  // This site used to be an open-coded `std::ofstream dst(dest, trunc);
  // dst << src.rdbuf();` with no check that `dest` IS `*session.currentHbin()`
  // — and after a plain `use FILE` (no *mutating* verb since) it still is. The
  // trunc-open destroyed the user's own file while `src` was still pointed at
  // it, the copy then moved 0 bytes, libc++ set failbit on the empty insert,
  // and the BUG-124 torn-write handler `remove()`d what it correctly believed
  // was a partial. Net: `use X` → `save X, replace` — the commonest Stata idiom
  // there is — DELETED X and blamed a disk that was not full.
  //
  // Adopt the shape the engine's own writer has carried for exactly this hazard
  // (op-cobra `src/data/frame.cpp:2103-2148`, the `path == src_path_` branch)
  // and that IDIOMS.md §Filesystem already prescribes ("save-in-place: write to
  // a same-dir temp + atomic rename, writer state CHECKED before the rename
  // commits"): produce the bytes in a sibling temp in the SAME directory (same
  // filesystem, so the commit cannot EXDEV), check them, then `rename(2)` the
  // temp over `dest` in one atomic step. `dest` is never opened for write at
  // all, so save-over-source is non-destructive BY CONSTRUCTION, and a failure
  // anywhere in the copy leaves the original file exactly as it was rather than
  // removed.
  //
  // The same shape carries the L5-IOSAVE perf half for free (§L5 DIAG in
  // private-notes/plans/GATE23-stage0-RUN.md): `dst << src.rdbuf()` is a
  // byte-at-a-time streambuf loop, ~5.5 ns/byte and *user*-CPU bound (0.91 user
  // / 0.22 sys on the 164 MB reference corpus = ~1.10 s, ~95% of the probe
  // wall). A same-volume `clonefile(2)` into the temp is O(1) (1.10 s →
  // 0.0003 s, measured); the cross-volume fallback is a 1 MB buffered loop
  // (0.51 s, 3.5× the streambuf loop). Every arm produced byte-identical output
  // (sha256, 4 arms) ⇒ value-identical, no golden re-pin.
  //
  // NOTE (inode/symlink edge): rename() retargets the directory entry to the
  // temp's NEW inode, so another fd or hardlink holding the OLD inode keeps
  // seeing the pre-save bytes, and a `dest` that is a symlink is REPLACED
  // rather than written through. Same tradeoff `mv`, most editors, and the
  // engine's own writer already make (frame.cpp carries the identical note).
  {
    // Sibling-temp target: a hidden temp in dest's OWN directory (same FS, so
    // the commit rename cannot EXDEV). Computed before the write path is chosen
    // because BOTH the C4-L2C direct-snapshot and the copy fallback land here.
    // Split with std::filesystem, NOT find_last_of('/'): on Windows a user path
    // is normally `C:\dir\file.hbin`, which contains no '/' at all. The manual
    // scan then made destDir "." and destBase the WHOLE path, so the sibling
    // temp became `./.C:\dir\file.hbin.savetmp…` — an invalid Windows filename
    // (a ':' and separators in a leaf), which failed the save with EINVAL, and
    // would have put the temp on the wrong volume even where the name was legal.
    // parent_path()/filename() handle '\', '/', and the drive prefix, and are
    // byte-identical to the old scan on POSIX.
    const std::filesystem::path destPath = cecon::to_native_path(dest);
    const std::filesystem::path destParent = destPath.parent_path();
    std::string destDir =
        destParent.empty() ? std::string(".") : cecon::from_native_path(destParent);
    std::string destBase = cecon::from_native_path(destPath.filename());
    // Per-CALL unique (IDIOMS §Filesystem — never name-keyed, never epoch-keyed:
    // BUG-111b): pid distinguishes concurrent processes, the counter
    // distinguishes two saves from the same process to the same directory.
    static std::atomic<unsigned long long> saveTmpSeq{0};
    std::string tmp = destDir + "/." + destBase + ".savetmp" +
                      std::to_string(static_cast<long long>(cecon::native_process_id())) + "_" +
                      std::to_string(saveTmpSeq++);
    std::error_code tmpEc;
    // Clear any stale temp from a killed run BEFORE the direct snapshot too — a
    // leftover file at `tmp` would make frameSnapshot/clonefile fail on EEXIST.
    std::filesystem::remove(cecon::to_native_path(tmp), tmpEc);

    // Mode fidelity: the old trunc-open wrote THROUGH an existing `dest`, so
    // that file kept its own permissions. rename() installs the temp's inode
    // instead — and `clonefile(2)`/frameSnapshot create the temp with their own
    // mode — so an existing destination's mode must be carried across explicitly
    // or a `save over_an_0600_file, replace` would silently re-permission it.
    // POSIX-only, and DELIBERATELY so — this was considered, not overlooked.
    // The Windows equivalent of "carry the destination's permissions" is not a
    // mode word at all, so there is nothing for a `_wchmod` to do here: the
    // CRT's `_wchmod` maps the whole of `st_mode` onto ONE bit, FILE_ATTRIBUTE_
    // _READONLY, and the real access control lives in the file's security
    // descriptor (DACL), which no POSIX call reaches.
    //
    // What the commit below actually does to that DACL (see the MoveFileExW
    // block): a same-volume MoveFileExW is a directory-entry rename, so the
    // file that ends up at `dest` keeps the TEMP's security descriptor and the
    // old destination's is destroyed with the old file. The temp is a sibling
    // in `dest`'s OWN directory (BUG-138 requires that for the EXDEV-free
    // commit), so it was created with exactly the ACEs that directory hands
    // down — which is also what a brand-new `save newfile.hbin` in that
    // directory would get. For the overwhelmingly common case (a destination
    // whose ACL is purely inherited from its directory) the before and after
    // descriptors are therefore equivalent and there is nothing to carry.
    //
    // The residual, stated honestly: if the destination carried EXPLICIT
    // (non-inherited) ACEs — someone ran `icacls dest /grant`, or the file was
    // protected with inheritance disabled — those ACEs are NOT reproduced on
    // the replacement. `save over_that_file, replace` re-permissions it to the
    // directory default. That is the same thing `mv`, and every other
    // temp-then-rename writer on Windows, does.
    //
    // Not fixed here, on purpose. The two candidate fixes both cost more than
    // the gap:
    //   * GetNamedSecurityInfoW/SetNamedSecurityInfoW (advapi32, already
    //     linked) round-trip. Correct only with ACL surgery, not a straight
    //     copy: writing back a descriptor that still contains the INHERITED_ACE
    //     entries re-materializes them as EXPLICIT ACEs on the new file, so the
    //     file stops tracking later changes to the directory's ACL — a
    //     different, quieter corruption of the thing we were trying to
    //     preserve. Doing it properly means reading SE_DACL_PROTECTED, sieving
    //     the inherited ACEs out, and rebuilding an ACL by hand, plus WRITE_DAC
    //     on the target and a decision about owner/SACL. Too much machinery,
    //     and untestable here, for the payload.
    //   * cecon::native_replace_file() (ReplaceFileW) DOES preserve the
    //     destination's DACL by contract — that is the function's reason to
    //     exist — but it requires `dest` to already exist and it has failure
    //     modes (ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) that can leave the
    //     destination already deleted. That would trade an ACL nicety for the
    //     BUG-138/BUG-124 guarantee this whole block is built around, "the
    //     existing file, if any, is unchanged". Not a good trade.
#ifndef _WIN32
    struct ::stat destSt;
    bool destHadMode = (::stat(cecon::to_native_path(dest).c_str(), &destSt) == 0);
#endif

    bool ok = false;
    int failErrno = 0;
    std::string failWhy;  // Win32 replace failure text (GetLastError, not errno)

    // C4-L2C (D3, C4-RESIDENT-READ-SCOPE-brief §3 close/§4): a dirty virtual head
    // is serialized STRAIGHT into `tmp` with one frameSnapshot — no scratch
    // materialization, no copy. `tmp` is already a BUG-138 sibling temp, so the
    // rename below still makes save-over-source non-destructive. The head is left
    // virtual (a later disk consumer materializes lazily). The escape arm
    // PIET_NO_RESIDENT_READ=1 skips this so the A/B parity + seeded-RED arm
    // reproduces the pre-C4 materialize-then-copy path (and its snapshot) exactly.
    bool wroteDirect = false;
    if (!isDta && !piet::residentReadDisabled() &&
        session.trySaveHeadDirect(tmp) == SessionState::SaveHeadPath::WroteDirect) {
      wroteDirect = true;
      ok = true;
    }

    // P1-A3: the `.dta` plane. Neither hbin plane applies — the bytes are
    // PRODUCED by cobra's format-118 writer, not copied or snapshotted — but
    // everything around the producer is deliberately the SAME machinery: the
    // BUG-138 sibling temp, the mode carry, the checked `rename(2)` commit and
    // the "the existing file, if any, is unchanged" failure contract all hold
    // for a `.dta` exactly as they do for an `.hbin`.
    //
    // Semantics ruling (this stint, DoD ②): routing the session's own scratch
    // hbin through the writer is legitimate HERE, where the datasignature
    // surface's mirror-image argument (42nd NO-GO ④) said it was not. That
    // argument is that a scratch round-trip makes `datasignature` fingerprint
    // what the scratch WRITE produced rather than the user's session. `save` is
    // the opposite case by definition: serializing the session IS the request,
    // the scratch hbin is the session's own current bytes, and the `--in-meta`
    // sidecar carries the label/format layer the hbin cannot — so the artifact
    // is the session, losslessly, not an artifact of the plumbing.
    std::string dtaErr;
    if (isDta) {
      // cobra's writer reads `--data` from DISK (contract_A §25), so a dirty
      // virtual head must be materialized first — the same precondition, for
      // the same reason, that the copy path states below.
      session.ensureMaterializedHead("save-dta");
      const std::string srcPath = *session.currentHbin();

      // Live card for the sidecar's presence filter (name + dtype only) —
      // resident when current, else the datastore. Taken AFTER materialization
      // so the card and the file the writer reads describe the same head.
      std::vector<VarInfo> ownedCard;
      const std::vector<VarInfo>* card = session.residentReadCard();
      if (card == nullptr) {
        if (Datastore* ds = session.datastore()) {
          ownedCard = ds->varlist();
          card = &ownedCard;
        }
      }
      std::string metaPath;
      if (card != nullptr) metaPath = writeDtaMetaSidecar(session, *card, dtaErr);

      if (dtaErr.empty()) {
        // `--data`/`--out` in FLAG form, never the positional fallback: `tmp` is
        // a hidden sibling (`.name.savetmp<pid>_<n>`) and the positional parse
        // keys on "argument does not start with --", so the flags keep an
        // unusual filename from ever being read as the wrong slot.
        std::vector<std::string> args = {"export-dta", "--data", srcPath, "--out", tmp};
        if (!metaPath.empty()) {
          args.push_back("--in-meta");
          args.push_back(metaPath);
        }
        CobraRunResult cr = runCobra(args);
        ok = cr.ok;
        if (!ok) {
          // ENGINE-CAPABILITY case, and the reason this branch exists at all:
          // the writer is NEWER than the pinned engine this build resolves by
          // default (Art 7, `releases/engine/current/cobra` — measured
          // 2026-07-31: 0.34.3 answers `export-dta` with `unknown command`).
          // Leaking that raw would tell a user their SAVE failed for reasons
          // naming a command they never typed, so it is mapped back to the
          // shape the pre-writer honest-reject had: name the cause, name the
          // format this engine CAN write, and never leave a half-file behind.
          if (cr.stderr_text.find("unknown command: export-dta") != std::string::npos) {
            const std::string suggested = rawFp.substr(0, rawFp.size() - rawExt.size()) + ".hbin";
            std::filesystem::remove(cecon::to_native_path(tmp), tmpEc);
            if (!metaPath.empty()) {
              std::error_code metaEc;
              std::filesystem::remove(cecon::to_native_path(metaPath), metaEc);
            }
            return engineErrorBlock(
                "save: writing Stata `.dta` files needs a cobra engine with the `export-dta` "
                "writer, and the engine this build resolves to does not have it (" +
                resolveCobraBin() +
                "). Save in the native format instead: `save " + suggested +
                (replace ? ", replace" : "") + "` — it loads back with a plain `use " + suggested +
                "`. (Set COBRA_BIN to an engine that ships `export-dta` to write .dta here.)");
          }
          // Everything else is the writer's own refusal, and those are the ones
          // this surface must NOT swallow: a string over the str# cap (strL is
          // out of the writer's scope, Q4), a malformed sidecar, an unwritable
          // target. Same stderr→message mapping every other verb here uses.
          dtaErr = cobraErrorStr(errorFromRaw(cr));
          if (dtaErr.empty()) dtaErr = "export-dta failed";
        }
      }
      if (!metaPath.empty()) {
        std::error_code metaEc;
        std::filesystem::remove(cecon::to_native_path(metaPath), metaEc);
      }
    } else if (!wroteDirect) {
      // Copy path: clean head, a divergent intermediate temp (currentHbin_ is
      // then already real), the escape arm, or a direct-write failure that fell
      // back. The copy reads *currentHbin() as a FILE, so a dirty virtual head
      // must be on disk first. Idempotent no-op on a clean head and on the escape
      // arm (whose entry datastore() already materialized). A FAILED
      // materialization (ENOSPC/dead worker) leaves no file at srcPath and the
      // copy reports it honestly below — "file not saved … unchanged".
      session.ensureMaterializedHead("save-entry");
      const std::string srcPath = *session.currentHbin();
#if defined(__APPLE__)
      {
        // Keep the paths alive across the call — path::c_str() is const char* here.
        std::filesystem::path nsrc = cecon::to_native_path(srcPath);
        std::filesystem::path ntmp = cecon::to_native_path(tmp);
        ok = (::clonefile(nsrc.c_str(), ntmp.c_str(), 0) == 0);
        // clonefile fails on a non-APFS FS, a cross-device pair, or an EEXIST
        // race — all handled by the buffered fallback below, which is also the
        // only path compiled on non-Apple builds.
      }
#endif
      if (!ok) {
        std::ifstream in(cecon::to_native_path(srcPath), std::ios::binary);
        std::ofstream out(cecon::to_native_path(tmp), std::ios::binary | std::ios::trunc);
        if (!in || !out) {
          failErrno = errno;
        } else {
          std::vector<char> buf(1 << 20);
          while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            std::streamsize got = in.gcount();
            if (got > 0) out.write(buf.data(), got);
            if (!out) break;
          }
          // BUG-124: check the writer's state before committing — an ENOSPC
          // mid-copy used to leave a silently short file at the USER's
          // destination while `save` still echoed "file ... saved". Now the torn
          // bytes are confined to the temp, which never becomes `dest`.
          out.flush();
          ok = static_cast<bool>(out) && !in.bad();
          if (!ok) failErrno = errno;
        }
      }
    }
    // BUG-198: state the session's format/label overrides INTO the produced
    // bytes. This sits AFTER both hbin arms and before the commit on purpose —
    // the resident-frame snapshot and the clonefile/copy of the scratch head
    // both drop the layer (measured on both by WS-D3), and neither can be
    // taught to carry it without reaching into the worker's Frame. Patching the
    // FILE is arm-independent: whichever arm made `tmp`, one site makes it
    // honest. `.dta` is excluded because its own `--in-meta` sidecar already
    // carried this layer through cobra's writer (P1-A3).
    //
    // Cost discipline: patch_hbin_meta writes NOTHING when the header already
    // says what the session says, so the ordinary save (no overrides, or
    // overrides that came out of this very file's header on `use`) is
    // byte-identical to before this change — which is what keeps the freeze
    // corpus still.
    //
    // A failure here is a save failure, not a warning: the alternative is
    // committing a file that silently lost the metadata the user set, which is
    // the bug this whole path exists to close. `tmp` is removed and `dest` is
    // untouched, exactly like every other failure on this path.
    // The `varMeta.empty()` gate is not a micro-optimization: reading the card
    // can cost a Datastore construct (mmap + whole-file FNV) and, on a virtual
    // head, an EMH snapshot — exactly the waste C4-L2A removed from this
    // function's entry. A session that has never set a format or a label has
    // nothing to state, and answers that from an empty map.
    if (ok && !isDta && !session.varMeta.empty()) {
      std::vector<VarInfo> ownedCard;
      const std::vector<VarInfo>* card = session.residentReadCard();
      if (card == nullptr) {
        if (Datastore* ds = session.datastore()) {
          ownedCard = ds->varlist();
          card = &ownedCard;
        }
      }
      if (card != nullptr) {
        std::vector<cecon::ColMetaPatch> metaPatch = session.hbinMetaPatch(*card);
        if (!metaPatch.empty()) {
          try {
            cecon::patch_hbin_meta(tmp, metaPatch);
          } catch (const std::exception& e) {
            std::filesystem::remove(cecon::to_native_path(tmp), tmpEc);
            return engineErrorBlock(
                std::string("save: failed writing the display-format/label layer into ") + dest +
                " (" + e.what() + ") — file not saved (the existing file, if any, is unchanged)");
          }
        }
      }
    }
    // Apply half of the mode carry captured above. POSIX-only for the reasons
    // stated there; the Windows arm intentionally has no counterpart, because
    // the sibling temp already holds the destination directory's inherited ACL.
#ifndef _WIN32
    if (ok && destHadMode) {
      ::chmod(cecon::to_native_path(tmp).c_str(), destSt.st_mode & 07777);
    }
#endif
    if (ok) {
#ifdef _WIN32
      // POSIX rename() over an existing file is not available on Windows —
      // the CRT's rename fails with EEXIST. MoveFileExW is the atomic
      // replace, and it is the only wide form, so a Korean `dest` survives.
      // WRITE_THROUGH makes the metadata commit before we report success,
      // which is the whole point of the temp-then-rename dance.
      // NOT cecon::native_replace_file(): ReplaceFileW requires the
      // destination to already exist, and `save newfile.hbin` is the common
      // case here. MOVEFILE_REPLACE_EXISTING covers both.
      //
      // Security-descriptor consequence, since the two calls differ on it and
      // the choice above is what settles it: this is a same-volume move, i.e.
      // a directory-entry rename, so the file left at `dest` carries the TEMP's
      // security descriptor — the old destination's descriptor goes away with
      // the old file. (ReplaceFileW is the call that preserves the replaced
      // file's DACL; MoveFileExW makes no such promise.) The temp is a sibling
      // in `dest`'s own directory, so its ACL is that directory's inherited
      // ACL, identical to what a newly created `dest` would carry — the visible
      // difference is confined to a destination that had explicit,
      // non-inherited ACEs. Full rationale, and why it is not compensated for,
      // at the mode-fidelity block above.
      std::filesystem::path nativeTmp = cecon::to_native_path(tmp);
      std::filesystem::path nativeDest = cecon::to_native_path(dest);
      ok = MoveFileExW(nativeTmp.c_str(), nativeDest.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
      if (!ok) {
        // GetLastError() is not an errno — carry its own message across.
        failWhy = std::error_code(static_cast<int>(GetLastError()),
                                  std::system_category())
                      .message();
      }
#else
      ok = (::rename(cecon::to_native_path(tmp).c_str(),
                     cecon::to_native_path(dest).c_str()) == 0);
      if (!ok) failErrno = errno;
#endif
    }
    if (!ok) {
      std::filesystem::remove(cecon::to_native_path(tmp), tmpEc);  // never leave the temp behind
      // BUG-124 honesty, corrected: report the cause the OS actually gave
      // instead of asserting "disk full" (BUG-138's second half — the old text
      // named a full disk on a volume with 770 GiB free). `dest` is untouched,
      // so say that too: the user's file is still there.
      std::string why = !dtaErr.empty()   ? dtaErr
                        : !failWhy.empty() ? failWhy
                        : failErrno != 0  ? std::string(std::strerror(failErrno))
                                          : std::string("copy failed");
      return engineErrorBlock("save: failed writing " + dest + " (" + why +
                              ") — file not saved (the existing file, if any, is unchanged)");
    }
  }
  // BS-091 (BETA A4): a successful `save` resets c(changed) to false,
  // matching Stata's own "changed since load/save" semantics — see
  // session_state.h's load()/setCurrentFile() for the other two edges of
  // this dirty bit.
  session.datasetChanged = false;
  // Report-only bookkeeping for the `datastore_reloads` counter (counters.h):
  // remember that THIS session wrote this path, so a later `use` of it can be
  // told apart from a first load of a foreign file. Never gates anything.
  //
  // History, because the absence is the interesting part: B5-③ registered a
  // resident-stash memo here so the next `use` of the saved file would be a
  // 0-reload restore. It worked — datastore_reloads 12 → 0 on a save→use loop
  // — and it bought no wall at all (paired median −0.2%), because a native
  // .hbin open is an mmap + header parse with no whole-file hash, so the work
  // removed was already cheap and the added stash/unstash trips ate it.
  // Reverted on project adjudication (2026-08-06, precedent A4): against zero
  // measured gain it was carrying a memo, an invalidation contract, and a
  // silent-wrong-answer hazard. The counter stayed, because knowing how often
  // a session re-reads its own output is worth knowing either way.
  session.recordSaveWrite(dest);
  // BUG-659: a native `.hbin`/`.tmp` has no char table, so the xtset/tsset
  // declaration live now is recorded beside the path for runUse to restate
  // (a `.dta` carries it in its own _dta chars via writeDtaMetaSidecar).
  if (!isDta) session.noteCheckpointTsDecl(dest);

  ojson out;
  out["type"] = "echo";
  // Report the EFFECTIVE filename (with `.hbin` appended when the user gave
  // none), not the bare name they typed — the honesty requirement behind
  // this whole fix is exactly that the reported name must match the real
  // bytes on disk.
  out["text"] = "file " + effectiveFp + " saved";
  return out;
}

// ---------------------------------------------------------------------------
// rename OLDVAR NEWVAR — C++ port of mutate.py's run_rename (two-arg form
// only — wildcard/multi-var syntax is OUT of scope, matching the oracle's
// own commands.py guard).
//
// rename is pure column-metadata relabeling — it must never touch a row's
// VALUE. L1/M3 (CSV-shim retirement): this calls cobra's native `rename`
// verb (`--pairs old=new`), which renames the header cell in place and
// leaves every other column — and the renamed column's own values and
// factor dictionary — byte-identical. Numeric and string/factor columns take
// the IDENTICAL path, no dtype branch.
//
// History (superseded, kept for the honesty rationale): this was first a
// `cobra eval --gen NEW=OLD --drop OLD` value-copy that only worked for f64
// columns and leaked a raw "[internal] expr: ..." engine exception when OLD
// was string/factor (BUG B, W3 stabilization); the fix routed it through a
// whole-frame CSV/`cobra convert` round-trip (writeColumnCsvRoundTrip, since
// deleted) because piet/native then had no way to rename a header without an
// hbin writer. The native verb supersedes both — no round-trip, no
// re-factorization.
ojson runRename(const std::string& oldvar, const std::string& newvar, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // fix/verb-overhead-sweep S4: rename's entry does nothing but ask whether one
  // name exists and another does not — the exact question runGsort/runOrder/
  // runDrop/runKeep already answer from the resident shadow card. It was
  // answering it with session.datastore(), i.e. a full cecon::Dataset::open of
  // the current hbin (mmap + 192-column header parse). Falls back to the exact
  // prior behavior whenever the card cannot be trusted — see
  // session.shadowVarlistIfCurrent()'s doc comment (session_state.h).
  //
  // SCOPE, STATED HONESTLY. This is NOT where rename's measured ~0.19 s/call
  // goes. The dominant term is engine-side: Frame::rename sets structure_dirty_
  // (src/data/frame.cpp), which disqualifies write_hbin's header-patch path and
  // forces write_hbin_full — a complete 164 MB serialize to change one string in
  // a JSON header. That is a cobra-owned fix in a file this stint does not
  // touch; removing the wasted open here does not substitute for it and is not
  // claimed to.
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    if (!cardHasName(*card, oldvar)) {
      return engineErrorBlock("variable `" + oldvar + "' not found");
    }
    if (cardHasName(*card, newvar)) {
      return engineErrorBlock("variable `" + newvar + "' already exists");
    }
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    const auto& names = ds->names();
    if (std::find(names.begin(), names.end(), oldvar) == names.end()) {
      return engineErrorBlock("variable `" + oldvar + "' not found");
    }
    if (std::find(names.begin(), names.end(), newvar) != names.end()) {
      return engineErrorBlock("variable `" + newvar + "' already exists");
    }
  }
  // BUG C: shared validator (piet::strutil::isValidStataIdentifier, see its
  // header comment) — a rename TARGET is a new column name exactly like
  // gen's LHS, so it goes through the identical rule at the identical
  // helper (handlers.cpp::runGen / strmutate.cpp::runStringGen are the
  // other two call sites).
  if (!strutil::isValidStataIdentifier(newvar)) {
    return engineErrorBlock(
        "rename: invalid variable name `" + newvar + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");
  }

  // L1/M3: native `cobra rename` — renames the header cell in place and
  // byte-preserves every other column plus the source's own values and factor
  // dictionary (no whole-frame CSV round-trip, no re-factorization). Output
  // --out lands on a fresh scratch path under the "rename" prefix (that prefix
  // is what scratch_path_monotonic.py's rename-chain leg counts). Mirrors
  // runCollapse's own single-verb invocation machinery (runCobra +
  // errorFromRaw). Numeric and string/factor columns take the identical path.
  std::string outPath = nextPath(session.workdir(), "rename");
  // *session.currentHbin() rather than ds->path() — equivalent whenever data is
  // loaded (a Datastore is always constructed from *currentHbin_), and it is
  // what lets the card branch above skip building one at all. Same substitution
  // runGsort's own fix already made for its --data arg.
  std::vector<std::string> args = {"rename",  "--data",  *session.currentHbin(), "--out",
                                   outPath,   "--pairs", oldvar + "=" + newvar};
  {
    // P2-PERF ARC-B/B2: rename's `--out` becomes the head and is never read
    // back as a file — mutationResult renders from the shadow card, and the
    // card knows how to fold a rename (session_state.cpp CardFoldPlan::Rename).
    // That is exactly the invariant ResidentEligible asserts, and it is the
    // same opt-in gen/replace/egen/drop/keep have had all along. Without it the
    // call is a stateless mutation, and Frame::rename sets structure_dirty_, so
    // the engine re-serialises the WHOLE frame to change one string in a JSON
    // header: 18.6 ms per call, measured, on a 48,000 x 31 frame.
    SessionState::ResidentEligible residentRename(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  session.load(outPath);

  // Carry metadata: var-label/format/stata_type + value-label attachment.
  auto metaIt = session.varMeta.find(oldvar);
  if (metaIt != session.varMeta.end()) {
    session.varMeta[newvar] = metaIt->second;
    session.varMeta.erase(oldvar);
  }
  auto lblIt = session.varLblname.find(oldvar);
  if (lblIt != session.varLblname.end()) {
    session.varLblname[newvar] = lblIt->second;
    session.varLblname.erase(oldvar);
  }
  auto vlIt = session.valueLabels.find(oldvar);
  if (vlIt != session.valueLabels.end()) {
    session.valueLabels[newvar] = vlIt->second;
    session.valueLabels.erase(oldvar);
  }

  return mutationResult(session, "rename", {newvar}, {oldvar});
}

// ---------------------------------------------------------------------------
// clonevar NEWVAR = OLDVAR — value copy + metadata clone (BUG-003, 1.6.0).
// C++ port of dispatcher.py's clonevar handler: gen NEWVAR=OLDVAR (action
// stays "gen", not "clonevar" — the oracle's own quirk) + clone
// label/format/stata_type/value-label attachment.
//
// Numeric clonevar uses the fast `cobra eval --gen NEW=OLD` path (also the
// one place `clonevar ... if` is supported, folded into a cond()); the
// string/factor path — which `cobra eval --gen`'s numeric-only RHS cannot
// serve (it once leaked "[internal] expr: string/factor variable used where
// a number is required" on `clonevar st2 = state`, BUG-037) — now calls
// cobra's native `clonevar` verb (`--pairs old=new`, L1/M3): the clone is
// appended at the end with the source's factor dictionary SHARED, OLDVAR
// survives untouched. (History: BUG-037 first routed the string/factor path
// through a whole-frame CSV round-trip, writeColumnCsvRoundTrip, since
// deleted — the native verb supersedes it.)
//
// BS-031(c) (BETA campaign C2, silent-if-drop sweep): `ifExpr` was
// previously accepted by the tokenizer but never threaded to this handler
// at all — `clonevar inc2 = income if year==1982` silently cloned ALL 336
// rows instead of only the 48 in-sample ones (oracle-verified live this
// stint: 48 non-missing, 288 missing — clonevar is gen-class, so an
// if-excluded row's target is MISSING, same as plain `gen`). The numeric
// fast path folds `if` into cobra's own `cond()` the exact same way
// handlers.cpp::runGen already does for plain `gen ... if` — no filtering/
// merge-back needed since this is a per-row passthrough. The string/factor
// path does NOT get the same treatment (`cobra clonevar` is a whole-column
// copy with no per-row conditional) — an `if` there is honestly rejected
// instead of silently ignored.
//
// C4-L4B (FIX-C4L4B-RESIDUALS ①): the encode recipe, applied to the residual
// wrap class the C4-L4 capture named. clonevar's entry reads a NAME and a
// DTYPE and nothing else — no column data — so the resident card answers both
// (the D1 rule) and the datastore opens LAZILY; the engine call takes the head
// TOKEN (`*session.currentHbin()`) instead of `ds->path()` and is wrapped in
// ResidentEligible, which routes it through the frame plane. `cobra clonevar`
// rides the SAME §26 acquire_data/commit_frame bridge as `cmd_encode`
// (apps/cmds/cmds_data.cpp:365/:368 vs :693/:697 — read cross-lane, zero cobra
// change), and the numeric arm is plain `eval --gen`, the hot resident path
// itself. Escape (`PIET_NO_RESIDENT_READ` for the C4 family,
// `PIET_NO_RESIDENT_SETCOL` for the C2-specific arm) restores the pre-C4 entry
// and `ds->path()` VERBATIM, so the floor RED arm reproduces the old path with
// one env, exactly as at the destring/encode sites.
ojson runClonevar(const std::string& newvar, const std::string& oldvar, SessionState& session,
                  const std::optional<std::string>& ifExpr) {
  // runSaveData's entry idiom: `datastore() == nullptr` is exactly
  // `!currentHbin().has_value()` (session_state.h), so hasData() is the same
  // question without the mmap + 192-column header parse (and, on a dirty
  // virtual head, without the EMH snapshot that open would force).
  if (piet::residentReadDisabled()) {
    if (session.datastore() == nullptr) return eval::noDataLoaded();
  } else if (!session.hasData()) {
    return eval::noDataLoaded();
  }
  Datastore* ds = nullptr;
  auto ensureDs = [&]() -> Datastore* {
    if (ds == nullptr) ds = session.datastore();
    return ds;
  };
  const std::vector<VarInfo>* card = session.residentReadCard();
  // S4a's rule (runEncode): an unrecognized dtype spelling is REFUSED rather
  // than guessed — the card's vocabulary is exactly Datastore::dtype()'s
  // ("factor" | "f64"), and anything else falls through to the datastore for
  // the whole answer rather than being mapped by hand.
  auto lookupDtype = [&](const std::string& name) -> std::optional<std::string> {
    if (card != nullptr) {
      const VarInfo* vi = cardFind(*card, name);
      if (vi == nullptr) return std::nullopt;  // a current card is authoritative for absence
      if (vi->dtype == "factor" || vi->dtype == "f64") return vi->dtype;
    }
    Datastore* d = ensureDs();
    if (d == nullptr || !d->has(name)) return std::nullopt;
    return d->dtype(name);
  };
  std::optional<std::string> srcDtype = lookupDtype(oldvar);
  if (!srcDtype.has_value()) {
    return engineErrorBlock("clonevar: variable `" + oldvar + "` not found in dataset");
  }
  // BUG C parity: clonevar's target is a new column name exactly like
  // gen's LHS / rename's target — this path never had the shared validator
  // wired at all before; mirror it here too (piet::strutil::
  // isValidStataIdentifier, see its header comment).
  if (!strutil::isValidStataIdentifier(newvar)) {
    return engineErrorBlock(
        "clonevar: invalid variable name `" + newvar + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");
  }
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();

  // C4-L4B: the residency switch, hoisted above both arms (the destring site's
  // shape). Resident path: `--data` rides the current head token and the
  // frame-run channel strips it. Escape/stateless path: pre-C4 verbatim —
  // materialize via ensureDs() and pass the real file.
  const bool residentClone = std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr &&
                             !residentReadDisabled();
  std::string dataPath;
  if (residentClone) {
    dataPath = *session.currentHbin();
  } else {
    Datastore* d = ensureDs();
    if (d == nullptr) return eval::noDataLoaded();
    dataPath = d->path();
  }

  if (*srcDtype == "f64") {
    // Numeric fast path — UNCHANGED from before this fix when no `if` is
    // given; `if` folds into a cond() exactly like plain `gen ... if`.
    std::string outPath = nextPath(session.workdir(), "gen");
    std::string genExpr = haveIf ? (newvar + "=cond((" + *ifExpr + ")," + oldvar + ",.)")
                                  : (newvar + "=" + oldvar);
    std::vector<std::string> args = {"eval", "--data", dataPath, "--gen", genExpr, "--out",
                                      outPath};
    std::optional<SessionState::ResidentEligible> cloneResident;
    if (residentClone) cloneResident.emplace(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(outPath);
  } else {
    // String/factor path — the BUG-037 fix. BS-031(c): `cobra clonevar` is a
    // whole-column copy with no per-row conditional — an `if` here would need
    // the same kind of row-wise masking runStringGen already does, which this
    // path doesn't share. Honest-reject rather than silently ignoring `if`
    // (matching the sweep's own "silent-wrong -> honest-reject or real-fix,
    // never leave it silently wrong" rule).
    if (haveIf) {
      return eval::unsupportedOption(
          "if",
          "`clonevar` with `if` is only supported for a numeric source variable in this "
          "build; `" + oldvar + "` is string/factor-typed.");
    }
    // L1/M3: native `cobra clonevar` — appends the clone at the END of the
    // column order with the factor dictionary SHARED from the source (no
    // whole-frame CSV round-trip, no re-factorization); OLDVAR survives
    // untouched (a clone is a COPY, not a move). Output keeps the "gen"
    // scratch prefix (scratch_path_monotonic.py's clonevar-string leg counts
    // gen_*.hbin; the numeric fast path above shares that prefix).
    std::string outPath = nextPath(session.workdir(), "gen");
    std::vector<std::string> args = {"clonevar", "--data", dataPath, "--out",
                                     outPath,     "--pairs", oldvar + "=" + newvar};
    std::optional<SessionState::ResidentEligible> cloneResident;
    if (residentClone) cloneResident.emplace(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(outPath);
  }

  // dispatcher.py's clonevar handler ORDERING BUG (verified against the
  // frozen 1.16.0 oracle directly, MI-XXX not filed but reproduced
  // bug-for-bug per Doctrine): `gen_result = run_gen(...)` computes its
  // `varlist_after` (via _mutation_result -> varlist_with_meta) BEFORE the
  // metadata-clone lines run below, and the handler returns that ALREADY-
  // COMPUTED gen_result unless it was None — so the returned block's
  // varlist_after reflects the label/format/etc BEFORE cloning (label=""
  // for the new var), even though session._var_meta[newvar] itself DOES
  // end up correctly populated (a later command's varlist_after — e.g. the
  // next `describe` — WILL show the cloned label; only THIS command's own
  // return block is stale). Compute the result block first, THEN clone
  // metadata, to reproduce this exact ordering.
  ojson result = mutationResult(session, "gen", {newvar});

  auto metaIt = session.varMeta.find(oldvar);
  if (metaIt != session.varMeta.end()) {
    const VarMeta& oldMeta = metaIt->second;
    VarMeta& newMeta = session.varMeta[newvar];
    if (oldMeta.label.has_value()) newMeta.label = oldMeta.label;
    if (oldMeta.format.has_value()) newMeta.format = oldMeta.format;
    if (oldMeta.stataType.has_value()) newMeta.stataType = oldMeta.stataType;
  }
  auto lblIt = session.varLblname.find(oldvar);
  if (lblIt != session.varLblname.end()) session.varLblname[newvar] = lblIt->second;
  auto vlIt = session.valueLabels.find(oldvar);
  if (vlIt != session.valueLabels.end()) session.valueLabels[newvar] = vlIt->second;

  return result;
}

// ---------------------------------------------------------------------------
// collapse (stat) varlist [(stat) varlist...] [, by(g1 g2)] [if COND] — C++
// port of dispatcher.py's _run_collapse. L1/M3 (CSV-shim retirement):
// cobra's native `collapse --by` now accepts factor by-vars directly — the
// output by-col stays factor with a fresh, self-consistent dictionary in
// first-seen output order (the same convention `convert --factor` produces,
// and what the old oracle's restore-levels dance rebuilt from scratch each
// collapse). Older cobra required all --by columns to be f64, so this
// handler used to pre-convert factor by-vars to f64 CODES via a CSV
// round-trip + egen-group() proxy and restore level text afterward; that
// whole pre/post dance is gone, by-vars pass straight through to `--by`.
ojson runCollapse(const std::string& rawLine, const std::optional<std::string>& ifExpr,
                  SessionState& session, const std::optional<std::string>& weightVar,
                  const std::optional<std::string>& inRange) {
  // ONESHOT-LASTFLUSH: this handler used to open session.datastore() on entry and
  // then use nothing from it but `ds->path()`. On a virtual head that open is a
  // FULL materialization of the pre-collapse frame — 14 of the flushes left after
  // the §38 wave, and the most expensive ones in the run (the collapsed result is
  // ~5% of its input). *currentHbin() answers the same question without opening
  // anything; the same substitution runRename and runGsort already made.
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  std::string raw = trim(rawLine);

  // BUG-042 (collapse if-misparse): `rawLine` is `parsed.raw` — the FULL,
  // untouched original command text, which still contains any `if COND`
  // clause verbatim (the tokenizer only strips `if` out of the internal
  // varlist/opts split it builds for `parsed.varlist`/`parsed.opts`; the
  // `raw` field it also carries is deliberately left whole). This handler
  // re-derives its own stat-varlist directly from `raw` via `reCollapse`
  // below, so an unstripped `if id<=4` used to leak through as bogus
  // "variable name" tokens ("if", the condition's own identifiers) reaching
  // cobra's `--stat` argument ("engine error: no column: if"). Strip the
  // SAME `if` clause back out here (mirroring tokenizer.cpp's own if-split:
  // only the portion before the first top-level comma is examined, so a
  // `by(...)` option tail is never touched) before parsing anything else.
  // `ifExpr` is already correctly threaded through to cobra's own
  // `--keepif` below — this fixes the PARSE, not the filtering itself.
  if (ifExpr.has_value() && !ifExpr->empty()) {
    size_t commaPos = raw.find(',');
    std::string head = (commaPos == std::string::npos) ? raw : raw.substr(0, commaPos);
    std::string tail = (commaPos == std::string::npos) ? std::string() : raw.substr(commaPos);
    static const std::regex reSplitIfCollapse(R"(\s+if\s+)", std::regex::icase);
    std::smatch ifm;
    if (std::regex_search(head, ifm, reSplitIfCollapse)) {
      head = head.substr(0, static_cast<size_t>(ifm.position(0)));
    }
    raw = trim(head) + tail;
  }

  // BS-090 (BETA Phase G): `in a/b` sits in the same pre-comma head as `if`
  // (and, same as the if-strip above, would otherwise leak "in"/the range
  // token through as bogus stat-varlist entries). The `if`-strip above
  // already removes any `in` clause that happens to come AFTER `if` in the
  // raw text (it truncates the whole head at the `if` keyword), so this is
  // a no-op in that ordering — it only actually fires when `in` precedes
  // `if` (or `if` is absent). `inRange` is already threaded to
  // applyIfAndInFilter below — this fixes the PARSE, not the filtering.
  if (inRange.has_value() && !inRange->empty()) {
    size_t commaPos = raw.find(',');
    std::string head = (commaPos == std::string::npos) ? raw : raw.substr(0, commaPos);
    std::string tail = (commaPos == std::string::npos) ? std::string() : raw.substr(commaPos);
    static const std::regex reSplitInCollapse(R"(\s+in\s+\S+)", std::regex::icase);
    head = std::regex_replace(head, reSplitInCollapse, "");
    raw = trim(head) + tail;
  }

  // project Step-1 D3: the `[fw=w]` weight clause also sits in the pre-comma head
  // and (like the `if` above) would otherwise leak into cobra's --stat argument
  // ("engine error: no column: w]"). The weight is already threaded to cobra via
  // --fweight below, so strip the clause from the stat head here. Gated on
  // weightVar so the no-weight collapse path stays byte-identical.
  if (weightVar.has_value() && !weightVar->empty()) {
    size_t commaPos = raw.find(',');
    std::string head = (commaPos == std::string::npos) ? raw : raw.substr(0, commaPos);
    std::string tail = (commaPos == std::string::npos) ? std::string() : raw.substr(commaPos);
    static const std::regex reWeightClauseCollapse(
        R"(\s*\[(?:fweight|fw|pweight|pw|iweight|iw|aweight|aw)\s*=\s*[^\]]*\])",
        std::regex::icase);
    head = std::regex_replace(head, reWeightClauseCollapse, "");
    raw = trim(head) + tail;
  }

  std::string byVal;
  {
    static const std::regex reBy(R"(\bby\s*\(\s*([^)]+)\))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(raw, m, reBy)) byVal = trim(m.str(1));
  }
  std::vector<std::string> byVars;
  {
    std::istringstream iss(byVal);
    std::string tok;
    while (iss >> tok) byVars.push_back(tok);
  }

  // stat-varlist: everything between `collapse` and `, by(` (or end).
  static const std::regex reCollapse(R"(\bcollapse\b(.+?)(?:,\s*by\s*\(|$))",
                                     std::regex::icase);
  std::smatch cm;
  if (!std::regex_search(raw, cm, reCollapse)) {
    return engineErrorBlock(
        "collapse: cannot parse stat varlist from command — expected "
        "`collapse (stat) varlist [(stat) varlist ...] [, by(varlist)]`");
  }
  std::string statRaw = trim(cm.str(1));
  if (statRaw.empty()) return engineErrorBlock("collapse: empty stat-varlist");

  // L1/M3 (CSV-shim retirement): cobra's native `collapse --by` now accepts
  // factor by-vars directly — the output by-col stays factor with a fresh,
  // self-consistent dictionary in first-seen output order (the same
  // convention `convert --factor` produces). The former workaround — dump the
  // whole frame to CSV -> convert -> materialize an egen-group() f64 proxy per
  // factor by-var -> collapse on the proxy -> restore the level text via a
  // second CSV round-trip — is gone; by-vars pass straight through to `--by`.
  std::string dataPath = *session.currentHbin();

  std::string byCsv;
  for (size_t i = 0; i < byVars.size(); ++i) {
    if (i) byCsv += ",";
    byCsv += byVars[i];
  }

  // Normalise the stat-varlist for cobra: `(mean) y1 y2 (sum) z` — vars
  // stay SPACE-separated within a (stat) block (cobra's own format;
  // comma-joining breaks multi-var blocks per dispatcher.py's own
  // BUG-FIX note).
  std::string statCobra;
  {
    static const std::regex reParenBlock(R"(\([^)]+\))");
    auto begin = std::sregex_iterator(statRaw.begin(), statRaw.end(), reParenBlock);
    auto end = std::sregex_iterator();
    size_t lastEnd = 0;
    std::vector<std::string> parts;
    for (auto it = begin; it != end; ++it) {
      if (static_cast<size_t>(it->position(0)) > lastEnd) {
        std::string between = statRaw.substr(lastEnd, static_cast<size_t>(it->position(0)) - lastEnd);
        std::istringstream iss(between);
        std::string tok, joined;
        bool first = true;
        while (iss >> tok) {
          if (!first) joined += " ";
          joined += tok;
          first = false;
        }
        if (!joined.empty()) parts.push_back(joined);
      }
      parts.push_back(it->str());
      lastEnd = static_cast<size_t>(it->position(0) + it->length(0));
    }
    if (lastEnd < statRaw.size()) {
      std::string tail = statRaw.substr(lastEnd);
      std::istringstream iss(tail);
      std::string tok, joined;
      bool first = true;
      while (iss >> tok) {
        if (!first) joined += " ";
        joined += tok;
        first = false;
      }
      if (!joined.empty()) parts.push_back(joined);
    }
    for (size_t i = 0; i < parts.size(); ++i) {
      if (i) statCobra += " ";
      statCobra += parts[i];
    }
  }
  if (statCobra.empty()) return engineErrorBlock("collapse: empty stat-varlist");

  // BS-090 (BETA Phase G): collapse's own cobra CLI has a --keepif of its
  // own (unlike tabstat/corr/ttest), but it runs through the SAME
  // non-bysort Frame::keep_if() as every other --keepif call, so an
  // `in`-derived `_n` predicate can't go through it either. Pre-filter
  // `dataPath` (the raw session hbin) via the shared helper and point the
  // collapse call at the result instead of forwarding a raw `--keepif`.
  if ((ifExpr.has_value() && !ifExpr->empty()) || (inRange.has_value() && !inRange->empty())) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, dataPath, ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *inRange, "collapse");
    dataPath = fr.dataPath;
  }

  std::string outPath = nextPath(session.workdir(), "collapse");
  std::vector<std::string> args = {"collapse", "--data", dataPath, "--stat", statCobra, "--out",
                                    outPath};
  if (!byCsv.empty()) {
    args.push_back("--by");
    args.push_back(byCsv);
  }
  // project Step-1 D3: forward the frequency weight to cobra's collapse. cobra
  // owns the Σw math (and rejects what it doesn't support, e.g. weighted
  // median/p## — PIET does not pre-empt that here, it just passes through).
  if (weightVar.has_value() && !weightVar->empty()) {
    args.push_back("--fweight");
    args.push_back(*weightVar);
  }
  {
    // ONESHOT-LASTFLUSH: collapse's `--out` becomes the head (session.load
    // immediately below) and is never read back as a file — which is exactly and
    // only what ResidentEligible asserts. The guard is scoped to THIS call, not
    // the whole handler, because applyIfAndInFilter above writes a temp that IS
    // read back as a file (collapse's own --data); eligibility is a session
    // counter, so an opt-in spanning both would silently drag the filter onto the
    // resident plane and hand cobra a --data that was never written.
    //
    // Unlike gen/replace, collapse reports card_stable=false (the row count and
    // column set both move), so mutation_result.cpp still rebuilds a Datastore
    // afterwards. That rebuild materializes the COLLAPSED head — measured at ~5%
    // of the frame this used to flush on the way in, and the engine no longer
    // reads the big one back at all.
    SessionState::ResidentEligible residentCollapse(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }

  session.load(outPath);
  return mutationResult(session, "collapse");
}

// ---------------------------------------------------------------------------
// gsort [-]VAR [[-]VAR ...] — C++ port of gsort.py (parse_gsort_varlist +
// run_gsort, which delegates to editor.py's run_sort). Native cobra 0.18.0
// gsort's own error text diverges from the oracle's ("no column: X" vs
// "Variable 'X' not found.") — pre-validate here so error cells stay
// byte-identical; success path routes to cobra's native --vars "-a,b" verb.
ojson runGsort(const std::vector<std::string>& tokens, SessionState& session) {
  if (tokens.empty()) {
    return engineErrorBlock("gsort requires at least one variable");
  }
  static const std::regex reTok(R"(^([+-]?)([A-Za-z_]\w*)$)");
  std::vector<std::string> varnames;
  std::vector<bool> ascending;
  for (const auto& tok : tokens) {
    std::smatch m;
    if (!std::regex_match(tok, m, reTok)) {
      return engineErrorBlock("gsort: invalid varspec '" + tok + "' — expected [-]varname");
    }
    varnames.push_back(m.str(2));
    ascending.push_back(m.str(1) != "-");
  }
  // fix/verb-overhead-sweep: same mechanism as runSort's fix/sort-validate-
  // lazy fix (handlers_m3.cpp) — this varlist check only ever needs column
  // NAMES, so answer it from the resident shadow card when it is provably
  // current for the loaded hbin, instead of unconditionally forcing
  // session.datastore() to reopen the file from disk. Falls back to the
  // exact prior behavior (session.datastore() + ds->has()) otherwise.
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& v : varnames) {
      if (!cardHasName(*card, v)) return engineErrorBlock("Variable '" + v + "' not found.");
    }
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    for (const auto& v : varnames) {
      if (!ds->has(v)) return engineErrorBlock("Variable '" + v + "' not found.");
    }
  }

  std::string joined;
  for (size_t i = 0; i < varnames.size(); ++i) {
    if (i) joined += ",";
    if (!ascending[i]) joined += "-";
    joined += varnames[i];
  }
  std::string outPath = nextPath(session.workdir(), "gsort");
  // fix/verb-overhead-sweep: *session.currentHbin() (not a ds->path() read)
  // — equivalent whenever data is loaded (Datastore is always constructed
  // from *currentHbin_, see SessionState::datastore()), and lets the fast
  // (card) branch above skip building a Datastore entirely, same choice
  // runSort's own fix already made for its --data arg.
  std::vector<std::string> args = {"gsort", "--data", *session.currentHbin(), "--vars", joined,
                                    "--out", outPath};
  // MAXPERF Stage 2 (item 3): single terminal row-reorder; --out never read back
  // as a file. Opt into the resident frame plane (no-op on the fallback plane).
  SessionState::ResidentEligible resident(session);
  CobraRunResult cr = runCobra(args);
  if (!cr.ok) return engineErrorBlock(cobraErrorStr(errorFromRaw(cr)), errorFromRaw(cr).code);
  session.load(outPath);
  return mutationResult(session, "sort");
}

// ---------------------------------------------------------------------------
// order VARLIST [, first|last|before(VAR)|after(VAR)] — C++ port of
// order.py's run_order. Pre-validates everything the oracle validates
// (anchor existence, anchor-not-in-varlist, mode exclusivity) so error text
// matches byte-for-byte, then calls cobra's native order verb for the
// actual column-permutation write.
ojson runOrder(const std::vector<std::string>& varlist,
              const std::vector<std::pair<std::string, eval::OptValue>>& opts,
              SessionState& session) {
  // fix/verb-overhead-sweep: same mechanism as runSort's fix/sort-validate-
  // lazy fix (handlers_m3.cpp) — order's own entry validation (varlist +
  // before()/after() anchor existence, all below) only ever needs column
  // NAMES, so answer it from the resident shadow card when it is provably
  // current, instead of unconditionally forcing session.datastore() open.
  // Falls back to the exact prior behavior (session.datastore() +
  // ds->names()) whenever the card cannot be trusted.
  //
  // NOTE on scope: order's own Frame-level mutation (src/data/frame.cpp,
  // Frame::order()'s rvalue overload) unconditionally sets
  // structure_dirty_=true ("a column permutation can't be patched onto the
  // source"), so the engine reports card_stable=false on every single call
  // (apps/cecon_cli.cpp's card_stable computation) and mutationResult's own
  // post-mutation Datastore rebuild further down is NEVER skippable via the
  // resident fast path — unlike gsort/sort, whose take_rows-based Frame
  // mutation never sets that bit. This fix removes only the AVOIDABLE
  // entry-side reopen (immediately below); the unavoidable post-mutation
  // rebuild is untouched (and untouchable from this file — it lives in
  // mutation_result.cpp).
  const auto* card = session.shadowVarlistIfCurrent();
  Datastore* ds = nullptr;
  if (card == nullptr) {
    ds = session.datastore();
    if (ds == nullptr) return engineErrorBlock("No dataset loaded. Use `use` first.", "no_data_loaded");
  }
  if (varlist.empty()) return engineErrorBlock("order: variable list required");

  // BUG-105 (옵션망): order accepts only first/last/before()/after() — any
  // other residual option token (a Stata option this build doesn't
  // implement, or a typo) must be a named reject, not a silent no-op.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnownOrderOpts = kVocabOrder.known;
  if (auto err = eval::rejectUnknownOptions(opts, kKnownOrderOpts)) {
    return *err;
  }

  auto hasName = [&](const std::string& n) {
    if (card != nullptr) return cardHasName(*card, n);
    const std::vector<std::string>& allNames = ds->names();
    return std::find(allNames.begin(), allNames.end(), n) != allNames.end();
  };
  for (const auto& v : varlist) {
    if (!hasName(v)) return engineErrorBlock("order: variable '" + v + "' not found in dataset");
  }

  bool first = false, last = false;
  bool beforeIsBool = false, afterIsBool = false;
  std::optional<std::string> beforeVar, afterVar;
  for (const auto& [k, v] : opts) {
    if (k == "first" && v.isBool && v.boolValue) first = true;
    if (k == "last" && v.isBool && v.boolValue) last = true;
    if (k == "before") {
      if (v.isBool) beforeIsBool = true;
      else beforeVar = v.strValue;
    }
    if (k == "after") {
      if (v.isBool) afterIsBool = true;
      else afterVar = v.strValue;
    }
  }
  if (beforeIsBool) {
    return engineErrorBlock("order, before(): requires a variable name, e.g. before(weight)");
  }
  if (afterIsBool) {
    return engineErrorBlock("order, after(): requires a variable name, e.g. after(make)");
  }
  if (beforeVar.has_value()) *beforeVar = trim(*beforeVar);
  if (afterVar.has_value()) *afterVar = trim(*afterVar);
  if (beforeVar.has_value() && beforeVar->empty()) beforeVar.reset();
  if (afterVar.has_value() && afterVar->empty()) afterVar.reset();

  int nModes = (first ? 1 : 0) + (last ? 1 : 0) + (beforeVar.has_value() ? 1 : 0) +
              (afterVar.has_value() ? 1 : 0);
  if (nModes > 1) {
    return engineErrorBlock("order: specify at most one of first, last, before(), after()");
  }
  if (nModes == 0) first = true;

  std::string mode = "first";
  std::string anchor;
  if (beforeVar.has_value()) {
    if (!hasName(*beforeVar)) {
      return engineErrorBlock("order, before(): variable '" + *beforeVar + "' not found");
    }
    if (std::find(varlist.begin(), varlist.end(), *beforeVar) != varlist.end()) {
      return engineErrorBlock("order, before(" + *beforeVar +
                              "): anchor variable must not be in the reorder list (it is "
                              "ambiguous where '" +
                              *beforeVar + "' would land)");
    }
    mode = "before";
    anchor = *beforeVar;
  } else if (afterVar.has_value()) {
    if (!hasName(*afterVar)) {
      return engineErrorBlock("order, after(): variable '" + *afterVar + "' not found");
    }
    if (std::find(varlist.begin(), varlist.end(), *afterVar) != varlist.end()) {
      return engineErrorBlock("order, after(" + *afterVar +
                              "): anchor variable must not be in the reorder list (it is "
                              "ambiguous where '" +
                              *afterVar + "' would land)");
    }
    mode = "after";
    anchor = *afterVar;
  } else if (last) {
    mode = "last";
  }

  std::string joined;
  for (size_t i = 0; i < varlist.size(); ++i) {
    if (i) joined += ",";
    joined += varlist[i];
  }
  std::string outPath = nextPath(session.workdir(), "order");
  // fix/verb-overhead-sweep: *session.currentHbin() (not a ds->path() read)
  // — equivalent whenever data is loaded, and lets the fast (card) branch
  // above skip building a Datastore entirely, same choice runSort's own fix
  // and this file's runGsort fix already made for their --data args.
  std::vector<std::string> args = {"order", "--data", *session.currentHbin(), "--vars", joined};
  if (mode == "first") {
    args.push_back("--first");
  } else if (mode == "last") {
    args.push_back("--last");
  } else if (mode == "before") {
    args.push_back("--before");
    args.push_back(anchor);
  } else if (mode == "after") {
    args.push_back("--after");
    args.push_back(anchor);
  }
  args.push_back("--out");
  args.push_back(outPath);
  {
    // MAXPERF Stage 2 (item 3): pure column reorder — single terminal mutation,
    // --out never read back as a file. Opt into the resident frame plane.
    SessionState::ResidentEligible resident(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  session.load(outPath);
  return mutationResult(session, "order");
}

// ---------------------------------------------------------------------------
// recode VARNAME (rules...) [if COND] [, gen(NEWVAR)] — C++ port of
// recode.py's parse_recode_command + run_recode_cmd. cobra 0.18.0's native
// `recode` verb accepts the SAME Stata rule-group syntax verbatim (verified:
// `(1 2=1)(3 4=2)(5=3)` parses identically) — this ports only the
// pre-validation (var-exists / gen-var-not-exists) that produces the
// oracle's exact error text, then passes the raw rules substring through.
//
// BS-031(b) (BETA campaign C2, silent-if-drop sweep): `ifExpr` is threaded
// through by the caller (dispatcher.cpp — parsed.ifExpr was ALREADY
// correctly extracted by the tokenizer; this function's own regex parsing
// below never needed to see it in the first place, since it truncates
// `rest` at the last matching ')' — an embedded trailing "if COND, gen(..)"
// tail was always silently discarded from rulePart by construction). Real
// Stata (live-verified this stint, StataBE): recode's gen() target is
// gen-class — an if-excluded row's gen'd value is MISSING (oracle:
// `recode year (1982/1984=1) if high==0, gen(rec1)` -> 230 non-missing, 106
// missing == count if high==1). recode's per-row rule mapping never
// depends on any OTHER row (a pure function of that row's own source
// value), so running the SAME unrestricted recode call as before (byte-
// identical when no `if` is given) and then masking the result down to
// missing for if-excluded rows reproduces the oracle exactly, without
// needing any filter/merge-back dance. In-place recode (no gen()) combined
// with `if` would need the OPPOSITE mask (if-excluded rows keep their
// ORIGINAL value, replace-class) — that path is honestly rejected instead
// of silently ignoring `if`, rather than risking a second, less-verified
// code path in the same sweep.
ojson runRecode(const std::string& rawLine, SessionState& session,
                const std::optional<std::string>& ifExpr) {
  std::string line = trim(rawLine);
  static const std::regex reCmd(R"(^\s*recode\s+)", std::regex::icase);
  std::smatch cmdm;
  if (!std::regex_search(line, cmdm, reCmd)) {
    return engineErrorBlock("parse_recode_command: line does not start with 'recode': '" + line +
                            "'");
  }
  std::string rest = line.substr(static_cast<size_t>(cmdm.length(0)));

  // Extract & remove gen()/generate() option anywhere in the string.
  std::optional<std::string> genName;
  static const std::regex reGen(R"(\bgen(?:erate)?\s*\(\s*(\w+)\s*\))", std::regex::icase);
  std::smatch genm;
  if (std::regex_search(rest, genm, reGen)) {
    genName = genm.str(1);
    rest = rest.substr(0, static_cast<size_t>(genm.position(0))) +
           rest.substr(static_cast<size_t>(genm.position(0) + genm.length(0)));
  }

  // Keep up to and including the LAST ')'.
  std::string rulePart;
  size_t lastParen = rest.rfind(')');
  rulePart = trim(lastParen == std::string::npos ? rest : rest.substr(0, lastParen + 1));

  size_t firstParen = rulePart.find('(');
  if (firstParen == std::string::npos) {
    return engineErrorBlock("recode: missing rule groups. Expected: recode VARNAME (val=new)...");
  }
  std::string varname = trim(rulePart.substr(0, firstParen));
  if (varname.empty()) {
    return engineErrorBlock("recode: missing variable name before first rule group");
  }
  std::string rulesStr = rulePart.substr(firstParen);

  // Oracle divergence (editor.py's _apply_recode, verified empirically
  // against cobra 0.18.0's native `recode`): an `(else=X)` rule matches
  // "unmatched AND NOT MISSING" values (mask = ~assigned & ~is_missing) —
  // missing source values stay missing unless a SEPARATE (missing=...) /
  // (.=...) rule is also given. cobra's native recode's `else` has no such
  // carve-out (it also catches missing values). Per M3b directive: where
  // cobra and the oracle differ, the frozen corpus wins — inject a
  // synthetic `(.=.)` (missing stays missing) rule immediately before the
  // FIRST `else` rule whenever the rule string has an `else` clause but no
  // explicit missing/`.`-code rule of its own (verified this reproduces
  // the oracle's N-count exactly: `(1=10)(else=99)` on a 5-missing column
  // -> N=69, not 74).
  {
    static const std::regex reElse(R"(\(\s*else\s*=)", std::regex::icase);
    static const std::regex reMissingRule(R"(\(\s*(?:missing|\.)\s*=)", std::regex::icase);
    std::smatch elseM;
    if (std::regex_search(rulesStr, elseM, reElse) &&
        !std::regex_search(rulesStr, reMissingRule)) {
      size_t insertPos = static_cast<size_t>(elseM.position(0));
      rulesStr = rulesStr.substr(0, insertPos) + "(.=.)" + rulesStr.substr(insertPos);
    }
  }

  // C4-L4B (class sweep for the clonevar wrap above — A11): recode's entry
  // reads two NAMES and no column data, so the resident card answers both and
  // the datastore opens LAZILY. This is not an independent nicety: `recode`'s
  // own floor fixture runs `clonevar x1c = x1` one line earlier
  // (tests/kitchensink/freeze/recode.json), so the moment clonevar stopped
  // materializing, recode's eager open became the EMH site for clonevar's
  // dirty virtual head — measured, not reasoned: datastore_constructs 0->1 AND
  // snapshot_count 0->1 on the recode row, RED against its own budgets, from
  // the clonevar change alone. Fixing the immediate consumer keeps the window
  // resident rather than relocating the cost by one line. Escape
  // (PIET_NO_RESIDENT_READ) restores the eager entry verbatim.
  Datastore* ds = nullptr;
  auto ensureDs = [&]() -> Datastore* {
    if (ds == nullptr) ds = session.datastore();
    return ds;
  };
  const std::vector<VarInfo>* card = session.residentReadCard();
  auto nameExists = [&](const std::string& name) -> std::optional<bool> {
    if (card != nullptr) return cardHasName(*card, name);  // current card: authoritative both ways
    Datastore* d = ensureDs();
    if (d == nullptr) return std::nullopt;
    return d->has(name);
  };
  if (card == nullptr && ensureDs() == nullptr) {
    return engineErrorBlock("No dataset loaded. Use 'use' first.");
  }
  std::optional<bool> srcExists = nameExists(varname);
  if (!srcExists.has_value()) return engineErrorBlock("No dataset loaded. Use 'use' first.");
  if (!*srcExists) return engineErrorBlock("Variable '" + varname + "' not found.");
  if (genName.has_value() && nameExists(*genName).value_or(false)) {
    return engineErrorBlock("Variable '" + *genName + "' already exists.");
  }
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  if (haveIf && !genName.has_value()) {
    return eval::unsupportedOption(
        "if",
        "`recode` with `if` requires a `gen()` target in this build — add gen(NEWVAR) so the "
        "filtered result can be isolated to a new variable, e.g. `recode " + varname +
            " (...) if ..., gen(NEWVAR)`.");
  }

  if (!haveIf) {
    std::string outPath = nextPath(session.workdir(), "recode");
    // C4-L4B: the head TOKEN on the resident arm (the frame-run channel strips
    // it); the real file otherwise. `cobra recode` rides the same §26
    // acquire_data/commit_frame bridge the ResidentEligible guard below already
    // relied on — this line is what stops the guard from having to materialize
    // its own input first.
    std::string dataPath;
    if (!residentReadDisabled()) {
      dataPath = *session.currentHbin();
    } else {
      Datastore* d = ensureDs();
      if (d == nullptr) return engineErrorBlock("No dataset loaded. Use 'use' first.");
      dataPath = d->path();
    }
    std::vector<std::string> args = {"recode", "--data", dataPath, "--var", varname, "--rules",
                                      rulesStr};
    if (genName.has_value()) {
      args.push_back("--gen");
      args.push_back(*genName);
    }
    args.push_back("--out");
    args.push_back(outPath);
    {
      // MAXPERF Stage 2 (item 3): the no-`if` recode is a single terminal
      // mutation (--out becomes the head, never read back as a file) — opt into
      // the resident frame plane. The if-aware path below stays STATELESS: it
      // chains recode->mask->drop through intermediate temps that ARE read back
      // as files, which a virtual head cannot supply.
      SessionState::ResidentEligible resident(session);
      CobraRunResult cr = runCobra(args);
      if (!cr.ok) {
        CobraError ce = errorFromRaw(cr);
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
    }
    session.load(outPath);
    if (genName.has_value()) {
      return mutationResult(session, "recode", {*genName});
    }
    return mutationResult(session, "recode");
  }

  // BS-031(b): if-aware gen() path — recode into a THROWAWAY column name
  // first (unrestricted, exactly as above), then mask it down into the
  // REAL gen() target: if-selected rows keep their mapped value,
  // if-excluded rows become missing.
  const std::string tmpGen = "__recode_tmp__";
  std::string tmpPath = nextPath(session.workdir(), "recode_tmp");
  {
    // STATELESS by design (unchanged): this arm chains recode->mask->drop
    // through intermediate temps that ARE read back as files, which a virtual
    // head cannot supply — so it takes the honest materialization, exactly as
    // before. ensureDs() is where the lazy open finally pays.
    Datastore* d = ensureDs();
    if (d == nullptr) return engineErrorBlock("No dataset loaded. Use 'use' first.");
    std::vector<std::string> args = {"recode", "--data", d->path(), "--var", varname, "--rules",
                                      rulesStr,  "--gen",  tmpGen,    "--out", tmpPath};
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  std::string maskedPath = nextPath(session.workdir(), "recode_mask");
  {
    std::string maskExpr = *genName + "=cond((" + *ifExpr + ")," + tmpGen + ",.)";
    std::vector<std::string> args = {"eval", "--data", tmpPath, "--gen", maskExpr, "--out",
                                      maskedPath};
    try {
      runCobraJson(args);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  std::string finalPath = nextPath(session.workdir(), "recode_final");
  {
    std::vector<std::string> args = {"eval", "--data", maskedPath, "--drop", tmpGen, "--out",
                                      finalPath};
    try {
      runCobraJson(args);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  session.load(finalPath);
  return mutationResult(session, "recode", {*genName});
}

// ---------------------------------------------------------------------------
// duplicates report|drop|tag|list [VARLIST] — C++ port of duplicates.py.
// report/drop/tag route to cobra's native `duplicates` verb; `list` (no
// cobra native equivalent) is built from `duplicates tag` + a keepif filter
// on the tag column (group_size-1 > 0 <=> duplicate).

namespace {

// _resolve_varlist: varlist or all names; validates each var exists
// ("variable `{v}' not found" — backtick-quote, matching Python's f-string).
std::optional<std::string> resolveVarlist(Datastore* ds,
                                          const std::optional<std::vector<std::string>>& varlist,
                                          std::vector<std::string>& outCols) {
  if (!varlist.has_value() || varlist->empty()) {
    outCols = ds->names();
    return std::nullopt;
  }
  for (const auto& v : *varlist) {
    if (!ds->has(v)) return "variable `" + v + "' not found";
  }
  outCols = *varlist;
  return std::nullopt;
}

// GA sweep 2026-08-22. The card-answered twin. Everything the Datastore version
// asks of the frame is METADATA — the column names, and whether a name exists —
// which the resident shadow card holds exactly, so a caller does not have to
// materialize the whole virtual head to ask it. Same idiom and the same safety
// rule as joinby's INSPECT-2608 fix and runSort's varlist check: the card answers
// only when it provably describes the current head, and a nullptr card means the
// caller falls back to the byte-identical Datastore path above.
//
// Measured cause (census47h, caller-attributed materialization log): of the 962
// head flushes triggered by a `datastore()` call in one 47-task run,
// runDuplicatesDrop was 320 — and its entry call existed only to resolve a
// varlist. Its TAIL flush was already removed in the 08-20 phase-2 work; this is
// the entry half the same census named.
std::optional<std::string> resolveVarlistFromCard(const std::vector<VarInfo>* card,
                                                  const std::optional<std::vector<std::string>>& varlist,
                                                  std::vector<std::string>& outCols) {
  if (card == nullptr) return std::string("__no_card__");  // sentinel: caller falls back
  if (!varlist.has_value() || varlist->empty()) {
    outCols.clear();
    outCols.reserve(card->size());
    for (const auto& vi : *card) outCols.push_back(vi.name);
    return std::nullopt;
  }
  for (const auto& v : *varlist) {
    bool found = false;
    for (const auto& vi : *card) if (vi.name == v) { found = true; break; }
    if (!found) return "variable `" + v + "' not found";
  }
  outCols = *varlist;
  return std::nullopt;
}

}  // namespace

ojson runDuplicatesReport(const std::optional<std::vector<std::string>>& varlist,
                          SessionState& session, const std::optional<std::string>& ifExpr,
                          const std::optional<std::string>& inRange) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  std::vector<std::string> cols;
  auto err = resolveVarlist(ds, varlist, cols);
  if (err.has_value()) return engineErrorBlock(*err);

  std::string joined;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (i) joined += ",";
    joined += cols[i];
  }

  // BUG-042: `duplicates report ... if COND` — cobra's `duplicates` verb has
  // no --keepif of its own, so pre-filter via the same row-filter infra
  // summarize/count already use (`cobra eval --keepif` into a temp hbin,
  // then point the report at the filtered copy instead of the live data).
  // BS-090 (BETA Phase G): `in` now composes with `if` via AND here too
  // (piet::handlers::applyIfAndInFilter).
  std::string dataPath = ds->path();
  if ((ifExpr.has_value() && !ifExpr->empty()) || (inRange.has_value() && !inRange->empty())) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, dataPath, ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *inRange, "duplicates report");
    dataPath = fr.dataPath;
  }

  ojson raw;
  try {
    raw = runCobraJson({"duplicates", "report", "--data", dataPath, "--vars", joined});
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }

  bool allVars = (!varlist.has_value() || varlist->empty());
  std::string colStr = allVars ? "(all variables)" : joined;
  {
    // oracle's col_str is SPACE-joined, not comma.
    colStr = allVars ? "(all variables)" : std::string();
    if (!allVars) {
      for (size_t i = 0; i < cols.size(); ++i) {
        if (i) colStr += " ";
        colStr += cols[i];
      }
    }
  }

  std::ostringstream out;
  out << "Duplicates in terms of " << colStr << "\n\n";
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%8s  %14s  %9s", "copies", "observations", "surplus");
  out << buf << "\n";
  out << std::string(38, '-') << "\n";

  int64_t totalSurplus = 0;
  if (raw.contains("rows") && raw["rows"].is_array()) {
    for (const auto& row : raw["rows"]) {
      int64_t copies = row.value("copies", 0);
      int64_t observations = row.value("observations", 0);
      int64_t surplus = row.value("surplus", 0);
      totalSurplus += surplus;
      std::snprintf(buf, sizeof(buf), "%8lld  %14lld  %9lld", static_cast<long long>(copies),
                    static_cast<long long>(observations), static_cast<long long>(surplus));
      out << buf << "\n";
    }
  }
  out << std::string(38, '-') << "\n";
  std::snprintf(buf, sizeof(buf), "%24s  %9lld", "Total surplus:",
               static_cast<long long>(totalSurplus));
  out << buf;

  ojson block;
  block["type"] = "display";
  block["text"] = out.str();
  return block;
}

ojson runDuplicatesDrop(const std::optional<std::vector<std::string>>& varlist,
                        SessionState& session, const std::optional<std::string>& ifExpr,
                        const std::optional<std::string>& inRange) {
  // Card first (see resolveVarlistFromCard): this entry used to open the frame —
  // i.e. materialize the virtual head — purely to resolve a varlist. `ds` stays
  // null on the card path and every later use of it below is guarded.
  Datastore* ds = nullptr;
  std::vector<std::string> cols;
  auto err = resolveVarlistFromCard(session.shadowVarlistIfCurrent(), varlist, cols);
  if (err.has_value() && *err == "__no_card__") {
    ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    err = resolveVarlist(ds, varlist, cols);
  }
  if (err.has_value()) return engineErrorBlock(*err);

  // BS-031 (BETA campaign C2, silent-if-drop sweep): `duplicates drop ...
  // if COND` previously ignored `if` entirely (dedup ran over the WHOLE
  // dataset, live-verified this stint: `duplicates drop year if year==1982`
  // silently collapsed all 336 rows down to 7 — one per year — instead of
  // leaving the 288 out-of-sample rows untouched). A correct fix needs to
  // dedup ONLY the if-sample and re-splice the untouched out-of-sample rows
  // back in at their original relative position (row-id tag + append +
  // re-sort) — meaningfully more moving parts than tag/list's fixes below,
  // and NOT exercised by any oracle-pinned scenario this stint (real
  // Stata's `duplicates drop VARLIST` also requires `,force` whenever
  // VARLIST narrows from "all variables", a separate unimplemented gap this
  // sweep doesn't touch — so almost every real `if`-qualified call already
  // errors on that unrelated ground first). Converting the silent-wrong
  // into an honest reject here (rather than shipping an unverified
  // multi-step engine choreography with no oracle to pin it against) is the
  // deliberate, disclosed scope boundary for this item.
  if (ifExpr.has_value() && !ifExpr->empty()) {
    return eval::unsupportedOption(
        "if",
        "`duplicates drop` with `if` is not yet supported in this build (dropping duplicates "
        "within an if-restricted sample while leaving other rows untouched needs more than a "
        "simple filter) — `duplicates report`/`tag`/`list` all honor `if`.");
  }
  // BS-090 (BETA Phase G): `in` needs the EXACT same dedup-then-splice-back
  // machinery `if` does (a plain filter would silently dedup only the
  // in-range rows and drop the out-of-range ones' own duplicates too) — the
  // SAME disclosed scope boundary as the `if` reject just above, not a new
  // gap this phase introduces.
  if (inRange.has_value() && !inRange->empty()) {
    return eval::unsupportedOption(
        "in " + *inRange,
        "`duplicates drop` with `in` is not yet supported in this build (dropping duplicates "
        "within an in-restricted sample while leaving other rows untouched needs more than a "
        "simple filter) — `duplicates report`/`tag`/`list` all honor `in`.");
  }

  std::string joined;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (i) joined += ",";
    joined += cols[i];
  }
  std::string outPath = nextPath(session.workdir(), "dup_drop");
  // On the card path `ds` is deliberately null (no head was materialized), so the
  // two facts this call still needs come from the session instead: the head PATH
  // it would have reported, and the pre-mutation row count. Both are exactly the
  // substitutions the prior phase-2 work established for this file — a `--data`
  // that only needs a path takes `*currentHbin()`, and a row count the card holds
  // is read from the card. The Datastore path below is byte-identical to before.
  const std::string dataPath = ds ? ds->path() : *session.currentHbin();
  std::vector<std::string> args = {"duplicates", "drop", "--data", dataPath, "--vars", joined,
                                    "--out", outPath};
  std::optional<int64_t> cardRows = session.residentRowsIfCurrent();
  if (ds == nullptr && !cardRows.has_value()) {
    // The card answered the varlist but cannot answer the row count. Rather than
    // report a wrong `nBefore`, fall back to the exact prior behaviour.
    ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
  }
  int64_t nBefore = ds ? ds->nRows() : *cardRows;  // pre-mutation row count.
  {
    // MAXPERF Stage 2 (item 3): single terminal row-dedup mutation; --out never
    // read back as a file. Opt into the resident frame plane.
    SessionState::ResidentEligible resident(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  session.load(outPath);
  // Phase 2 (2026-08-20). This used to be `session.datastore()->nRows()`, which
  // MATERIALIZED the whole virtual head — the entire dedup output written to
  // disk — to learn ONE integer, and it did so BEFORE mutationResult ran, so the
  // card fold below could never save the flush it had already paid for. Measured
  // by the Phase 0 caller-attributed census: runDuplicatesDrop forced 100 of the
  // run's 476 head materializations (21%), second only to rename's 220.
  //
  // mutationResult already computes that integer, on BOTH of its paths, and the
  // ordering is the whole point: taking it from the result means the number
  // shipped as `n_dropped` is derived from the very card being shipped as
  // `varlist_after`, instead of from a separate read that could disagree with it.
  // Fold path: `out["n_rows_after"] = delta.rows` (session_state.cpp), the engine's
  // own post-mutation count. Rebuild path: `ds->nRows()` — the identical value
  // this line used to fetch, just no longer fetched twice.
  ojson result = mutationResult(session, "duplicates_drop");
  const int64_t nAfter = result.value("n_rows_after", int64_t{0});
  result["n_dropped"] = nBefore - nAfter;
  return result;
}

// BS-031(a) (BETA campaign C2, silent-if-drop sweep): `duplicates tag ...
// if COND, gen(NEWVAR)` previously ignored `if` entirely — the sub-command
// dispatch in runDuplicates() below never even forwarded ifExpr to this
// function. Live-verified this stint (StataBE): tag is computed WITHIN the
// if-sample only, and if-EXCLUDED rows get NEWVAR = MISSING (oracle:
// `duplicates tag year if year==1982, gen(dtag)` -> dtag has N=48 (matches
// the if-sample size), mean=min=max=47, and the other 288 rows are
// missing). Unlike recode, `duplicates tag` is a genuine AGGREGATE over the
// CURRENT rows (group size depends on which OTHER rows are present) — a
// "compute unrestricted, mask after" shortcut would silently give the
// WRONG group sizes whenever `if` doesn't happen to align with whole
// dedup-key groups, so this must actually restrict the input before
// tagging. cobra's own `duplicates tag --keepif EXPR` (apps/cmds/
// cmds_data.cpp) does the restricting, but its `--out` is then only the
// N_sub-row filtered frame — reintegrating that into the FULL N-row
// dataset (untouched rows kept, tag missing for them) needs a join key:
// stamp every row with its own position (`_n`) before filtering, tag the
// filtered subset, then `cobra merge 1:1` the tagged subset's `genvar`
// back onto the full id-stamped frame by that same position (an ordinary
// master-keeps-all merge — unmatched rows get missing "for free", exactly
// Stata's semantics) and drop the synthetic id column again.
ojson runDuplicatesTag(const std::optional<std::vector<std::string>>& varlist,
                       const std::string& genvar, SessionState& session,
                       const std::optional<std::string>& ifExpr,
                       const std::optional<std::string>& inRange) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  std::vector<std::string> cols;
  auto err = resolveVarlist(ds, varlist, cols);
  if (err.has_value()) return engineErrorBlock(*err);
  if (ds->has(genvar)) {
    return engineErrorBlock("variable `" + genvar + "' already defined");
  }

  std::string joined;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (i) joined += ",";
    joined += cols[i];
  }

  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveIn = inRange.has_value() && !inRange->empty();
  if (!haveIf && !haveIn) {
    std::string outPath = nextPath(session.workdir(), "dup_tag");
    std::vector<std::string> args = {"duplicates", "tag",    "--data", ds->path(), "--vars",
                                      joined,       "--gen",  genvar,   "--out",    outPath};
    // MAXPERF Stage 2 (item 3): the unrestricted `duplicates tag` is a single
    // terminal pure-ADD mutation (--out becomes the head, never read back as a
    // file). Opt into the resident frame plane. The if/in path below stays
    // STATELESS: it stamps _n, filters, tags, and merges back through
    // intermediate temps that are read AS FILES.
    SessionState::ResidentEligible resident(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(outPath);
    return mutationResult(session, "duplicates_tag", {genvar});
  }

  const std::string ridCol = "__dup_rid__";
  std::string idPath = nextPath(session.workdir(), "dup_tag_id");
  try {
    // egen seq() is the engine's row-number op — bare `_n` in engine eval
    // requires a bysort context (rejected), unlike PIET-side gen.
    runCobraJson({"eval", "--data", ds->path(), "--egen", ridCol + "=seq()", "--out", idPath});
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  // BS-090 (BETA Phase G): `ridCol` (already the GLOBAL row index, 1..N,
  // materialized above for the merge-back key regardless of `in`) doubles
  // as the `in a/b` predicate's target — no SEPARATE idx materialization
  // needed here, unlike piet::handlers::applyIfAndInFilter's general case.
  std::string combinedIf = ifExpr.value_or("");
  if (haveIn) {
    InRangeBounds rr = parseInRangeBounds(*inRange, ds->nRows());
    if (!rr.ok) {
      InRangeFilterResult fr;
      fr.ok = false;
      fr.negative = rr.negative;
      return inRangeRejectError(fr, *inRange, "duplicates tag");
    }
    std::string rangePred = ridCol + ">=" + std::to_string(rr.a) + " & " + ridCol + "<=" +
                            std::to_string(rr.b);
    combinedIf = haveIf ? ("(" + *ifExpr + ") & (" + rangePred + ")") : rangePred;
  }
  std::string taggedPath = nextPath(session.workdir(), "dup_tag_sample");
  {
    std::vector<std::string> args = {"duplicates", "tag",     "--data", idPath, "--vars",
                                      joined,       "--gen",   genvar,   "--keepif", combinedIf,
                                      "--out",      taggedPath};
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  std::string mergedPath = nextPath(session.workdir(), "dup_tag_merged");
  {
    // merge prints a plain-text "merge 1:1: wrote ..." line, not JSON —
    // runCobra (ok-check), never runCobraJson (same idiom as the
    // duplicates-tag call above).
    CobraRunResult cr = runCobra({"merge", "--data", idPath, "--using", taggedPath, "--type",
                                  "1:1", "--key", ridCol, "--keepusing", genvar, "--nogenerate",
                                  "--out", mergedPath});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  std::string finalPath = nextPath(session.workdir(), "dup_tag_final");
  {
    CobraRunResult cr = runCobra({"eval", "--data", mergedPath, "--drop", ridCol, "--out",
                                  finalPath});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  session.load(finalPath);
  return mutationResult(session, "duplicates_tag", {genvar});
}

ojson runDuplicatesList(const std::optional<std::vector<std::string>>& varlist,
                        SessionState& session, const std::optional<std::string>& ifExpr,
                        const std::optional<std::string>& inRange) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  std::vector<std::string> cols;
  auto err = resolveVarlist(ds, varlist, cols);
  if (err.has_value()) return engineErrorBlock(*err);

  std::string joined;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (i) joined += ",";
    joined += cols[i];
  }
  std::string tagVar = "__dup_tag__";
  std::string tagPath = nextPath(session.workdir(), "dup_list_tag");
  {
    // BS-031(a) (BETA campaign C2, silent-if-drop sweep): `duplicates list
    // ... if COND` previously ignored `if` (tag was always computed over
    // the FULL dataset). `list` is read-only/display-only — unlike tag's
    // gen() target, there is no full-N column to preserve, so pre-filtering
    // the SOURCE data (BS-090, BETA Phase G: piet::handlers::
    // applyIfAndInFilter — composes `if`/`in` via AND, replacing the old
    // direct `--keepif` forward on this internal `duplicates tag` call,
    // which could not have evaluated an `in`-derived `_n` predicate anyway)
    // is enough: it restricts BOTH which rows are considered for the dedup
    // AND which rows land in the (already scratch, never session-loaded)
    // `tagPath` output — the browse below then naturally only ever sees the
    // if/in-sample. Live-verified this stint (StataBE): `duplicates list
    // state if year==1982` correctly reports 0 duplicates (each state
    // appears once per year), `duplicates list year if year==1982` lists
    // all 48 in-sample rows (all share year 1982). Known, disclosed
    // cosmetic gap: real Stata's list table shows each row's ORIGINAL
    // (pre-filter) observation number in its "Obs" column — this handler's
    // browse()-based output has no such column at all (pre-existing, not
    // introduced by this fix), so that particular renumbering nuance isn't
    // addressed here.
    std::string dupListDataPath = ds->path();
    if ((ifExpr.has_value() && !ifExpr->empty()) || (inRange.has_value() && !inRange->empty())) {
      InRangeFilterResult fr;
      try {
        fr = applyIfAndInFilter(session, dupListDataPath, ifExpr, inRange);
      } catch (const CobraError& ce) {
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      if (!fr.ok) return inRangeRejectError(fr, *inRange, "duplicates list");
      dupListDataPath = fr.dataPath;
    }
    std::vector<std::string> args = {"duplicates", "tag",  "--data", dupListDataPath, "--vars",
                                      joined,       "--gen", tagVar,  "--out",         tagPath};
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  std::string filteredPath = nextPath(session.workdir(), "dup_list_filtered");
  try {
    runCobraJson({"eval", "--data", tagPath, "--keepif", tagVar + ">0", "--out", filteredPath});
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  Datastore filteredDs(filteredPath);
  if (filteredDs.nRows() == 0) {
    ojson block;
    block["type"] = "display";
    block["text"] = "No duplicates found.";
    return block;
  }
  std::optional<std::vector<std::string>> browseVars;
  if (varlist.has_value() && !varlist->empty()) browseVars = *varlist;
  // Value labels live on the SESSION keyed by varname, so they apply to the
  // cobra-filtered temp dataset too (same column names) — an empty map here
  // rendered value-labeled f64 columns as raw codes, diverging from the
  // oracle's duplicates.py (value_label_map=session.get_value_label_map()).
  ojson block = filteredDs.browse(0, 200, browseVars, session.valueLabelMapForBrowse());
  block["editable"] = false;
  block["note"] = std::to_string(filteredDs.nRows()) + " duplicate observations";
  return block;
}

// ---------------------------------------------------------------------------
// reshape long|wide STUB..., i(IDVARS) j(JVAR) — C++ port of reshape.py's
// run_reshape (direction dispatch + _parse_ivar_jvar) + run_reshape_long/
// run_reshape_wide's OWN pre-validation (id/j/stub existence, i()/j()
// presence). cobra's native `reshape` verb requires --i/--j as mandatory
// CLI args and has no equivalent to the oracle's "i() option required —
// e.g. ..." message (a bare CLI usage error, not the structured JSON error
// shape) — pre-validate everything here so error cells match byte-for-byte,
// success path calls cobra for the actual melt/pivot.


}  // namespace piet::handlers
