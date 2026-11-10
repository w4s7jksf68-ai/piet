// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// dataset save.
#include "data_mutation_internal.h"
namespace piet::handlers {
using namespace data_mutation_detail;


// ---------------------------------------------------------------------------
// save FILE [, replace] — C++ port of dispatcher.py's save_data handler.
// Persists the current dataset by copying the session hbin to FILE
// (in-memory dataset unchanged, Stata-faithful).
ojson runSaveData(const std::string& filepathArg, bool replace, SessionState& session,
                  int dtaRelease) {
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

  // Public save uses Stata format 118, including extensionless names and
  // tempfile paths. A tempfile's suffix does not change its file format:
  // w2ar_data_io_family dx_sa_004 / dx_save_079_emptyok read the actual
  // <release>118</release> bytes back. Explicit .hbin remains the native plane.
  std::string rawExt = extOf(rawFp);
  std::string effectiveFp = rawFp;
  if (rawExt.empty()) {
    effectiveFp = rawFp + ".dta";
    rawExt = ".dta";
  }
  const bool isDta = rawExt == ".dta" || rawExt == ".tmp" || dtaRelease > 0;
  if (!isDta && rawExt != ".hbin") {
    std::string suggested = rawFp.substr(0, rawFp.size() - rawExt.size()) + ".dta";
    return engineErrorBlock(
        "save: `" + rawExt + "` is not a save format this version of PIET can write. Use `save " +
        suggested + (replace ? ", replace" : "") + "` for Stata format or an explicit `.hbin`.");
  }

  // Resolve against session.cwd, NOT session.workdir() (parity FIX, native-
  // paths stabilization item 2): `save x.hbin` now lands next to the user's
  // own files (cwd), matching real Stata — session.workdir() is the
  // internal hbin-chain scratch dir and was never a user-meaningful
  // location for a relative `save`.
  std::string dest = resolveUserPath(session, effectiveFp);
  // Both refusals name the file as typed, in Stata's words (BUG-410): StataBE
  // `file s1.dta already exists` r(602), and for a directory that is not there
  // `file nodir/s.dta could not be opened` r(603).
  auto refuse = [&](const std::string& why, const char* rc) {
    return ojson(eval::errorBlock("engine_error", "file " + effectiveFp + " " + why, std::nullopt,
                                  std::nullopt, std::nullopt, std::string(rc)));
  };
  if (fileExists(dest) && !replace) return refuse("already exists", "r(602)");
  {
    const size_t slash = dest.find_last_of('/');
    std::error_code dirEc;
    if (slash != std::string::npos && slash > 0 &&
        !std::filesystem::is_directory(cecon::to_native_path(dest.substr(0, slash)), dirEc))
      return refuse("could not be opened", "r(603)");
  }
  // Explicit native checkpoints under the session tempdir retain the RAM
  // snapshot optimization. Public tempfile saves above produce actual DTA bytes.
  const bool isMintedTempfile = !isDta && rawExt == ".hbin" && session.runTempdir.has_value() &&
                                dest.rfind(*session.runTempdir + "/", 0) == 0;
  if (isMintedTempfile &&
      std::none_of(session.mintedTempfiles.begin(), session.mintedTempfiles.end(),
                   [&dest](const auto& item) { return item.second == dest; })) {
    session.mintedTempfiles.emplace_back(session.preserveLevel(), dest);
  }
  // A real file is about to land at `dest`: an earlier RAM-only save under the
  // same name must not answer a later `use` instead of it.
  if (isDta && session.isVirtualSave(dest)) session.residentVirtualDrop(dest);
  // e(sample) T2: the estimation-sample mask is a hidden column of the working
  // frame (piet/datastore.h kEsampleColumn), not the user's data, and Stata's
  // `save` does not write its marker: `save` + `use` reads e(sample) 0, and an
  // `append using` of a saved copy adds rows outside the sample
  // (w2_esample.log:1272 E14 |0|0|0|28|, :1104 E7 |0|0|28|122|). The `.dta`
  // writer (export-dta) already leaves `__piet_*` columns out; the two hbin
  // planes copy the head's bytes, so when the head carries the mask the file
  // is written by an engine `eval --drop` instead (below). Checked only in a
  // session where a fit installed one.
  bool stripMask = false;
  if (!isDta && session.esampleColumnInstalls > 0) {
    try {
      Datastore* mds = session.datastore();
      stripMask = mds != nullptr && mds->has(kEsampleColumn);
    } catch (const std::exception&) {
      stripMask = false;  // an unreadable head fails the copy below, honestly
    }
  }
  if (isMintedTempfile && !stripMask && session.residentSaveVirtual(dest)) {
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
  // internal note GATE23-stage0-RUN.md): `dst << src.rdbuf()` is a
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
    if (!isDta && !stripMask && !piet::residentReadDisabled() &&
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
        if (dtaRelease > 0 && dtaRelease != 118) {
          args.push_back("--release");
          args.push_back(std::to_string(dtaRelease));
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
    } else if (stripMask) {
      // e(sample) T2 (see stripMask above): the head minus the hidden mask,
      // written by the engine straight into the sibling temp; the commit below
      // is the same. The engine call reads the head as a file (stateless), so a
      // dirty virtual head is materialized first by the runner itself.
      const CobraRunResult cr =
          runCobra({"eval", "--data", *session.currentHbin(), "--drop", kEsampleColumn, "--out",
                    tmp});
      ok = cr.ok && cecon::native_exists(tmp);
      if (!ok) failErrno = EIO;
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
                std::string("save: failed writing the display-format/label layer into ") + effectiveFp +
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
      return engineErrorBlock("save: failed writing " + effectiveFp + " (" + why +
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
  // Reverted on maintainer adjudication (2026-08-06, precedent A4): against zero
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
  // Report the EFFECTIVE filename (with `.dta` appended when the user gave
  // none), not the bare name they typed — the honesty requirement behind
  // this whole fix is exactly that the reported name must match the real
  // bytes on disk.
  out["text"] = "file " + effectiveFp + " saved";
  return out;
}
}
