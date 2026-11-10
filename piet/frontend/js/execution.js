// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command/file execution and authoritative dataset-view synchronization.
import { appendBlock, appendBlocks, clearConsole, endDisplayLine } from './console.js';
import { clearGraphGallery } from './graphics-gallery-store.js';
import { updateVariables, appendHistory } from './panels.js';
import { withMutationSummaries } from './result-blocks.js';

export function createExecutionController({ getClient, getGrid, editor, layout, graphics, progress, pushHistory, resetHistoryIndex, trackRecentFile, flashStatus }) {
  // This controller owns the live dataset view and command busy state.
  const state = { currentFile: null, varlist: [], nRows: 0, nCols: 0, busy: false, kernelReady: false };
  let _lastTitledFile = undefined;
  const { setDoEditorText, setDoEditorPath, selectEditorLine, openDoEditor } = editor;
  const { closeAllOverlays, _isOverlayOpen } = layout;
  const { refreshGraphicsIfOpen } = graphics;
  const { startProgressPolling, stopProgressPolling } = progress;

  async function checkKernel() {
    const dotEl = document.getElementById('kernel-dot');
    const { ok } = await getClient().ping();
    state.kernelReady = ok;
    if (dotEl) {
      dotEl.className = ok ? 'kernel-dot dot-ok' : 'kernel-dot dot-err';
      dotEl.title = ok ? 'kernel ready' : 'kernel not reachable';
    }
  }

  function parseSaveTarget(cmd) {
    const m = cmd.trim().match(/^save\s+(.+)$/i);
    if (!m) return null;
    let arg = m[1].split(',')[0].trim();     // drop `, replace` etc.
    arg = arg.replace(/^["']|["']$/g, '');   // strip a matching pair of quotes
    return arg || null;
  }

  async function runCommand(cmd) {
    if (!cmd.trim() || state.busy) return;

    // FR-cls: Stata's Results-window clear command. Exact match (case per
    // Stata is lowercase `cls`; the .trim() above tolerates surrounding
    // whitespace); any extra tokens ("cls foo") do NOT match here and fall
    // through to the engine, which honestly errors it as unsupported (BUG-040
    // principle — don't silently accept what Stata rejects). Client-side only:
    // clears the console via the same clear machinery the × button uses and
    // never reaches the bridge (cls touches no data).
    if (cmd.trim() === 'cls') {
      clearConsole();
      pushHistory('cls', true);
      resetHistoryIndex();
      appendHistory('cls', true);
      document.getElementById('cmd-input')?.focus();
      return;
    }

    // B7 (P8-S6): bare `help` (no argument) — Stata opens a documentation
    // viewer PIET doesn't have; sending it to the engine bare would either
    // error or silently no-op. Intercept client-side and render ONE honest
    // hint line instead (W10: no modal, no wall of text — SPEC III-6 label
    // honesty over building a help system that doesn't exist yet). `help
    // <command>` (WITH an argument) is left to pass through to the engine
    // unchanged — it's RESERVED for the future Bridge Tutor; this does not
    // special-case it further.
    if (/^help$/i.test(cmd.trim())) {
      appendBlock({ type: 'echo', text: 'help' });
      appendBlock({ type: 'display', text: 'Type a command, or `help <command>` for a specific one — full reference in the handbook.' });
      pushHistory('help', true);
      resetHistoryIndex();
      appendHistory('help', true);
      document.getElementById('cmd-input')?.focus();
      return;
    }

    // B9 (P8-S7): `note <text>` / `* <text>` — a plain-text annotation, not a
    // command. Rendered as an italic, prompt-less note block (console.js's
    // renderNote() — a .block-display variant with NO '. ' prefix, so it
    // visibly reads as "not a command", SPEC III-6 label honesty) and NEVER
    // reaches the engine. `* ...` is Stata's own full-line-comment syntax —
    // the dispatcher already silently no-ops a bare comment line (BUG-040
    // principle: pure comment lines produce zero blocks today), so this only
    // ADDS visible feedback for what already renders nothing; it doesn't
    // override any real engine behavior. `note` (bare Stata dataset-annotation
    // command) has no dispatcher handler at all, so nothing is shadowed there
    // either. Matched only at the START of the trimmed command, so e.g.
    // `regress y x, note("...")` is untouched — the option's own text is never
    // re-parsed as a command here.
    const noteMatch = cmd.trim().match(/^(?:note\s+(.*)|\*\s?(.*))$/is);
    if (noteMatch) {
      const noteText = (noteMatch[1] ?? noteMatch[2] ?? '').trim();
      const noteCmd = cmd.trim();
      appendBlock({ type: 'note', text: noteText });
      pushHistory(noteCmd, true);
      resetHistoryIndex();
      appendHistory(noteCmd, true);
      document.getElementById('cmd-input')?.focus();
      return;
    }

    const browseCmds = /^(br|browse|ed|edit)\b/i.test(cmd.trim());
    // B1/B2: a console `do FILE` / `run FILE` blocks while the file runs — start
    // the liveness strip so it isn't a silent freeze; the echo carries resolved_path.
    const isDoRun = /^(do|run)\b/i.test(cmd.trim());

    setBusy(true);
    if (isDoRun) startProgressPolling();

    try {
      const result = await getClient().command(cmd);
      const blocks = result.blocks ?? [];

      // B2 (1.16.0): a console `do FILE` echoes {resolved_path, source} — load that
      // file's source into the do-file editor pane so the user can inspect/edit it.
      if (isDoRun) {
        const echoB = blocks.find(b => b.type === 'echo' && b.resolved_path);
        if (echoB && typeof echoB.source === 'string') {
          setDoEditorText(echoB.source);
          setDoEditorPath(echoB.resolved_path);
          trackRecentFile(echoB.resolved_path, 'do');   // B3: console `do FILE`/`run FILE`
        }
      }

      // Detect browse/edit command in result — open the overlay
      if (browseCmds || blocks.some(b => b.type === 'browse')) {
        const browseBlock = blocks.find(b => b.type === 'browse');
        if (browseBlock && getGrid()) {
          // Echo first
          const echoB = blocks.find(b => b.type === 'echo');
          if (echoB) appendBlock(echoB);
          // Mission 1: the browse/edit overlay shares the console area with the
          // other three — close them first (registry-driven, same as every
          // other openX()).
          closeAllOverlays('browser-overlay');
          // editable iff the backend flagged it (the `edit` command); browse = read-only
          getGrid().setEditable(!!browseBlock.editable);
          await getGrid().load(browseBlock);
          getGrid().show();
        } else {
          appendBlocks(withMutationSummaries(blocks));
          if (blocks.some(b => b.type === 'graph')) refreshGraphicsIfOpen();
        }
      } else {
        appendBlocks(withMutationSummaries(blocks));
        if (blocks.some(b => b.type === 'graph')) refreshGraphicsIfOpen();
      }

      // Update varlist + dims if mutation changed it
      if (result.varlist_after) {
        state.varlist = result.varlist_after;
        state.nCols   = result.varlist_after.length;
        updateVariables(result.varlist_after);
        const mutBlock = blocks.find(b => b.type === 'data_mutation');
        if (mutBlock?.n_rows_after != null) state.nRows = mutBlock.n_rows_after;
        // U1: keep the tracked file honest for console-typed loads too (use/
        // sysuse) — File▾ Open already knows its own filepath (onOpenFile(),
        // below); this is the console/do-file leg, which doesn't, so it reads
        // the CONTRACT-B filepath the "open" mutation now carries. A mutation
        // that empties the varlist (clear, drop _all, ...) means data is gone.
        if (result.varlist_after.length === 0) state.currentFile = null;
        else if (mutBlock?.action === 'open' && mutBlock.filepath) state.currentFile = mutBlock.filepath;
        if (mutBlock?.action === 'open') {
          scheduleAutoSummarize();   // B14(b)
          if (mutBlock.filepath) trackRecentFile(mutBlock.filepath, 'data');   // B3: console `use`/`sysuse`
        }
        updateStatus();
      }

      // History
      const success = !blocks.some(b => b.type === 'error');
      // U1: `save filename` (Stata parity) updates the tracked file. No wire
      // field carries this (CONTRACT only adds filepath to the open action),
      // so the target is parsed straight from the command text.
      if (success && /^save\b/i.test(cmd.trim())) {
        const target = parseSaveTarget(cmd);
        if (target) { state.currentFile = target; updateStatus(); }
      }
      // U7 (P8.1): mirror a SUCCESSFUL engine-side `graph drop _all` into the
      // frontend Graphics gallery. Response-driven, NOT a cls-style intercept —
      // the native dispatcher already owns this command (dispatcher.cpp BS-056,
      // clears session.namedGraphs); intercepting client-side would leave that
      // engine store stale. Named drops (`graph drop g1`) stay engine-only: the
      // gallery is an ordered visual log, not a name-indexed store.
      if (success && /^graph\s+drop\b/i.test(cmd.trim()) && /\b_all\b/i.test(cmd)) {
        clearGraphGallery();
        refreshGraphicsIfOpen();
      }
      pushHistory(cmd, success);
      resetHistoryIndex();
      appendHistory(cmd, success);

    } catch (err) {
      appendBlock({ type: 'error', code: 'engine_error', message: String(err.message ?? err) });
      pushHistory(cmd, false);
      appendHistory(cmd, false);
      checkKernel();   // A1-03: refresh the kernel-dot after a surfaced engine_error
    } finally {
      if (isDoRun) stopProgressPolling();
      setBusy(false);
      document.getElementById('cmd-input')?.focus();
    }
  }

  function scheduleAutoSummarize() {
    if (localStorage.getItem('piet-auto-summarize') !== '1') return;
    setTimeout(runAutoSummarize, 0);
  }

  async function runAutoSummarize() {
    if (state.busy || !state.varlist.length) return;
    if (localStorage.getItem('piet-auto-summarize') !== '1') return;
    setBusy(true);
    try {
      appendBlock({ type: 'echo', text: 'summarize (auto)' });
      const result = await getClient().command('summarize');
      const blocks = result.blocks ?? [];
      appendBlocks(withMutationSummaries(blocks));
      const success = !blocks.some(b => b.type === 'error');
      pushHistory('summarize (auto)', success);
      resetHistoryIndex();
      appendHistory('summarize (auto)', success);
    } catch (err) {
      appendBlock({ type: 'error', code: 'engine_error', message: String(err.message ?? err) });
      checkKernel();   // A1-03: refresh the kernel-dot after a surfaced engine_error
    } finally {
      setBusy(false);
    }
  }

  function openRecentFile(entry) {
    if (state.busy) return;
    if (entry.kind === 'do') onOpenDoFile(entry.path);
    else onOpenFile(entry.path);
  }

  async function onOpenFile(knownPath) {
    // A1-01: don't race a command/do-run/mutation already in flight.
    if (state.busy) return;
    // 2.0.1: open a native Finder file picker (NSOpenPanel) via the dialog.open
    // bridge route. The old code checked window.pywebview (the retired 1.x
    // launcher) then fell back to window.prompt — but the 2.0 native shell has
    // neither pywebview nor a WKWebView prompt panel, so this button was a dead
    // no-op. getClient().pickFile() resolves to the chosen absolute path (or null on
    // cancel); errors from open() below still surface in the try/catch.
    let filepath = knownPath;
    setBusy(true);
    try {
      if (!filepath) {
        filepath = await getClient().pickFile('dta,csv,hbin');
        if (!filepath) return;   // user cancelled — no error
      }

      const ext = filepath.split('.').pop().toLowerCase();
      const format = ext === 'dta' ? 'dta' : ext === 'csv' ? 'csv' : 'hbin';

      const result = await getClient().open(filepath, format);

      // CONTRACT B v1.1: failure returns HTTP 200 + {type:"error", ...}
      if (result.type === 'error') {
        appendBlock(result);
      } else {
        state.currentFile = filepath;
        state.varlist     = result.varlist_after ?? [];
        state.nRows       = result.n_rows_after ?? 0;
        state.nCols       = state.varlist.length;

        updateVariables(state.varlist);
        updateStatus();

        const fname = filepath.split(/[/\\]/).pop();
        appendBlock({ type: 'echo', text: `use ${fname}` });
        appendBlock({ type: 'data_mutation', action: 'open', varlist_after: state.varlist, n_rows_after: state.nRows });
        scheduleAutoSummarize();   // B14(b)
        trackRecentFile(filepath, 'data');   // B3
      }

    } catch (err) {
      appendBlock({ type: 'error', code: 'engine_error', message: `Failed to open file: ${err.message ?? err}` });
      checkKernel();   // A1-03: refresh the kernel-dot after a surfaced engine_error
    } finally {
      setBusy(false);
      document.getElementById('cmd-input')?.focus();
    }
  }

  function noteWorkingDirectory(dir) {
    if (dir) appendBlock({ type: 'note', text: `Working directory is now ${dir} (the do-file's folder)` });
  }

  async function onRunDo() {
    // A1-01: don't race a command/do-run/mutation already in flight.
    if (state.busy) return;
    // 2.0.1: native Finder picker (.do) via the dialog.open bridge route — was a
    // dead window.pywebview/window.prompt fallback in the WKWebView shell. Errors
    // from readDoFile() below still surface in the try/catch.
    let filepath;
    setBusy(true);
    try {
      filepath = await getClient().pickFile('do');
      if (!filepath) { document.getElementById('cmd-input')?.focus(); return; }   // user cancelled — no error
      const { source, resolved_path, cwd_changed } = await getClient().readDoFile(filepath, { followCwd: true });
      noteWorkingDirectory(cwd_changed);
      setDoEditorText(source);
      setDoEditorPath(resolved_path);
      trackRecentFile(resolved_path || filepath, 'do');   // B3 — reopening from
      // Recent always goes through onOpenDoFile (load-only), never back through
      // this run-immediately path, so a recent-file click can't silently re-run.
      // Guard interplay: runDoSourceCore() assumes the CALLER already holds
      // state.busy (see its doc comment below) — this call is still inside the
      // setBusy(true)/finally pair above, so that contract holds. Calling
      // runDoEditor() here instead would self-block: it re-checks state.busy
      // at its own top, which is already true from setBusy(true) above, so it
      // would return immediately without ever running the buffer.
      await runDoSourceCore(source, 'do (editor)');
    } catch (err) {
      // readDoFile() rejects with bridge error code "bad_request" for a
      // missing/non-.do/>2MB file — _nativeRequest() folds that into a normal
      // Error, so this reads the same as every other engine_error call site.
      appendBlock({ type: 'error', code: 'engine_error', message: String(err.message ?? err) });
      checkKernel();   // A1-03: refresh the kernel-dot after a surfaced engine_error
    } finally {
      setBusy(false);
      // Don't steal focus from the editor if runDoSourceCore just re-opened it
      // (the stopped-on-error path) — openDoEditor() already focused #doedit-text.
      if (!_isOverlayOpen('doedit-overlay')) document.getElementById('cmd-input')?.focus();
    }
  }

  function routeDroppedFile(path) {
    const ext = String(path).split('.').pop().toLowerCase();
    if (ext === 'do') { onOpenDoFile(path); return; }
    if (ext === 'dta' || ext === 'csv' || ext === 'hbin') { onOpenFile(path); return; }
    appendBlock({
      type: 'error',
      code: 'engine_error',
      message: `Dropped file has extension .${ext}, which PIET does not open. ` +
               'Drop a dataset (.dta, .csv, .hbin) or a do-file (.do).',
    });
  }

  async function onOpenDoFile(knownPath) {
    // A1-01: don't race a command/do-run/mutation already in flight.
    if (state.busy) return;
    let filepath = knownPath;
    setBusy(true);
    try {
      if (!filepath) {
        filepath = await getClient().pickFile('do');
        if (!filepath) { document.getElementById('cmd-input')?.focus(); return; }   // user cancelled — no error
      }
      const { source, resolved_path, cwd_changed } = await getClient().readDoFile(filepath, { followCwd: true });
      noteWorkingDirectory(cwd_changed);
      setDoEditorText(source);
      setDoEditorPath(resolved_path);
      openDoEditor();   // foreground; focuses #doedit-text itself — don't refocus cmd-input below
      trackRecentFile(resolved_path || filepath, 'do');   // B3
    } catch (err) {
      appendBlock({ type: 'error', code: 'engine_error', message: String(err.message ?? err) });
      checkKernel();   // A1-03: refresh the kernel-dot after a surfaced engine_error
      document.getElementById('cmd-input')?.focus();
    } finally {
      setBusy(false);
    }
  }

  async function runDoSourceCore(source, label, lineOffset = 0) {
    appendBlock({ type: 'echo', text: label });
    startProgressPolling();                          // B1: live liveness strip
    try {
      const result = await getClient().runDoString(source);
      applyDoResult(result);
      const stopped = (result.blocks ?? []).some(b => b.type === 'error') ||
                      result.stopped_at_line != null || !!result.stopped_at_reason;
      if (stopped) {
        openDoEditor();   // fix-in-place: bring the loaded source back to front
        // U6: point at the exact failing line — lineOffset accounts for a
        // partial-selection run (runDoEditorSelection), where stopped_at_line
        // is relative to the SUBMITTED source, not the whole buffer.
        if (result.stopped_at_line != null) selectEditorLine(result.stopped_at_line + lineOffset);
      }
      const ok = !stopped;
      pushHistory(label, ok);
      appendHistory(label, ok);
    } catch (err) {
      appendBlock({ type: 'error', code: 'engine_error', message: String(err.message ?? err) });
      checkKernel();   // A1-03: refresh the kernel-dot after a surfaced engine_error
    } finally {
      stopProgressPolling();
    }
  }

  function _tailBlocksToRender(blocks, streamed) {
    if (streamed <= 0) return blocks;                              // nothing streamed — render everything, unchanged
    if (blocks.length > streamed) return blocks.slice(streamed);   // legacy full-array shape — slice off the streamed prefix
    return blocks;                                                 // 2.5.0+ tail-only shape — already just the remainder, append as-is
  }

  function applyDoResult(result) {
    const doBlocks = result.blocks ?? [];
    // N6 (P8.1-W2, contract_B v2.4.0): blocks[0..streamed) were already
    // rendered live via the {kind:"block"} push (initPietEventsBridge, above)
    // — render only the remainder so nothing double-appears. `streamed` is
    // additive/optional; absent or 0 (a transport that never pushes — fetch/
    // mock, or a native run with nothing to stream) renders every block here,
    // byte-identical to pre-N6 behavior. Everything ELSE in this function
    // (stopped_at handling, lastMut, refreshVarlistAuthoritative) still reads
    // the FULL doBlocks/result — only the live-console RENDER is deduped.
    // B.3.0 (contract_B 2.5.0): _tailBlocksToRender() replaces a bare
    // doBlocks.slice(streamed) — see its doc comment just above for the shape
    // change and the temporary compat guard.
    const streamed = Number(result.streamed) > 0 ? Number(result.streamed) : 0;
    appendBlocks(withMutationSummaries(_tailBlocksToRender(doBlocks, streamed)));
    if (doBlocks.some(b => b.type === 'graph')) refreshGraphicsIfOpen();   // U8
    // BUG-150: distinguish a HALT from a FAILURE. doexec.cpp sets
    // stopped_at_reason only in the else-branch of handleUncapturedError() —
    // i.e. never for a clean `exit`, which instead records only the line and an
    // exit code. "line but no reason" is therefore exactly, and only, a clean
    // exit; a real stop always names its reason. Saying "stopped at line 284"
    // about a run that finished the way its author wrote it reads as a failure,
    // which is the other half of the red-block defect (console.js suppresses the
    // sentinel itself).
    const cleanExit = result.stopped_at_line != null && !result.stopped_at_reason;
    if (cleanExit) {
      appendBlock({ type: 'echo', text: 'do-file ended (exit)' });
    } else if (result.stopped_at_line != null || result.stopped_at_reason) {
      const linePart   = result.stopped_at_line != null ? ` at line ${result.stopped_at_line}` : '';
      const reasonPart = result.stopped_at_reason ? ` — ${result.stopped_at_reason}` : '';
      appendBlock({ type: 'echo', text: `do-file stopped${linePart}${reasonPart}` });
    }
    // A13(3) (P8.1-T5): mirror the stop onto the statusbar too — flashStatus()
    // is the one-glance surface (B2), so noticing a stop shouldn't require
    // scrolling the log. The wire carries STRING error codes (syntax_error,
    // no_data_loaded, …), not Stata's numeric r() — mirrored honestly as the
    // code itself rather than fabricating an r()-shaped number. Independent of
    // the appendBlock() branch above (its gate is stopped_at_reason, not code).
    {
      // BUG-150: same clean-exit exemption as the console line above — a
      // finished run must not flash "stopped" at the one-glance surface either.
      const stopCode = doBlocks.find(b => b.type === 'error')?.code;
      if (!cleanExit && (result.stopped_at_line != null || stopCode)) {
        const lineBit = result.stopped_at_line != null ? `stopped at line ${result.stopped_at_line}` : 'stopped';
        flashStatus(stopCode ? `${lineBit} — ${stopCode}` : lineBit, 6000);
      }
    }
    // Cheap within-run update from the last mutation block, if any (top-level
    // varlist_after is not in the /api/do contract)...
    const lastMut = [...doBlocks].reverse().find(b => b.type === 'data_mutation');
    if (lastMut?.varlist_after) {
      state.varlist = lastMut.varlist_after;
      state.nCols   = lastMut.varlist_after.length;
      updateVariables(lastMut.varlist_after);
    }
    if (lastMut?.n_rows_after != null) state.nRows = lastMut.n_rows_after;
    if (lastMut) updateStatus();
    if (lastMut?.action === 'open') scheduleAutoSummarize();   // B14(b): do-file `use`/`sysuse`
    // ...then ALWAYS resync from the authoritative session state (BUG-044): a
    // run can change the dataset without emitting any data_mutation block —
    // `clear all` wipes it, a stop-at-error run may mutate then halt — and the
    // heuristic above then leaves the variables pane + statusbar showing a
    // ghost of the pre-run dataset while gen/describe correctly see none.
    refreshVarlistAuthoritative();
  }

  async function refreshVarlistAuthoritative() {
    try {
      const vl = await getClient().varlist();
      const list = vl?.varlist ?? [];
      state.varlist = list;
      state.nCols   = vl?.n_cols ?? list.length;
      state.nRows   = vl?.n_rows ?? 0;
      if (list.length === 0) state.currentFile = null;  // data gone → "(no data)"
      // U1: the do-file/editor run path's authoritative leg — a `use`/`sysuse`
      // inside a .do file has no local filepath the way File▾ Open does, so
      // pick it up from the wire's source_file (CONTRACT B) once there's data
      // again. `||` (not `??`) deliberately keeps the previous value on an
      // empty-string/falsy source_file rather than clobbering it.
      else state.currentFile = vl?.source_file || state.currentFile;
      updateVariables(list);
      updateStatus();
    } catch { /* bridge hiccup: keep current UI; ping/kernel-dot owns liveness */ }
  }

  function updateStatus() {
    const fileEl   = document.getElementById('status-file');
    const dimsEl   = document.getElementById('status-dims');
    const tbFileEl = document.getElementById('toolbar-filename');
    // U1: the statusbar reads "<cwd> · <file> · N vars, M obs · ●" — cwd and
    // file are both derived from the one tracked state.currentFile path (no
    // separate "current directory" field exists on the wire — CONTRACT B only
    // adds source_file/filepath); the toolbar strip (tbFileEl) stays
    // filename-only, unchanged.
    let dir = '', fname = '';
    if (state.currentFile) {
      const parts = state.currentFile.split(/[/\\]/);
      fname = parts.pop() || '';
      dir   = parts.join('/');
    }
    if (fileEl)   fileEl.textContent   = state.currentFile ? [dir, fname].filter(Boolean).join(' · ') : '(no data)';
    if (tbFileEl) tbFileEl.textContent = fname;
    if (dimsEl) {
      // A1-05: key the blank fallback off state.currentFile (like fname above),
      // not state.nRows — a genuinely-loaded 0-observation dataset used to show
      // blank dims instead of "N vars, 0 obs".
      dimsEl.textContent = state.currentFile
        ? `${state.nCols} vars, ${state.nRows.toLocaleString('en-US')} obs`
        : '';
    }
    // N2(b): native window title — 'PIET — <basename>', or bare 'PIET' with no
    // file loaded. Feature-detected (no bridge -> no window chrome to retitle,
    // mock/browser stays exactly as before) and fire-and-forget (a failed
    // retitle is cosmetic only, never surfaced as an error block).
    if (getClient() && getClient().native && state.currentFile !== _lastTitledFile) {
      _lastTitledFile = state.currentFile;
      const title = fname ? `PIET — ${fname}` : 'PIET';
      getClient().setWindowTitle(title).catch(() => { /* cosmetic only */ });
    }
  }

  function setBusy(busy) {
    state.busy = busy;
    // BUG-1473: a run starts on its own line; a `_continue` line joins only
    // within the run that opened it (console.js endDisplayLine).
    if (busy) endDisplayLine();
    const inputEl = document.getElementById('cmd-input');
    if (inputEl) inputEl.disabled = busy;
    // #cmd-bar.busy (piet.css) replaces the old per-input opacity mutation —
    // same 0.5 visual, plus pointer-events:none for the whole bar.
    const cmdBar = document.getElementById('cmd-bar');
    if (cmdBar) cmdBar.classList.toggle('busy', busy);
    const runBtn = document.getElementById('btn-run');
    if (runBtn) runBtn.disabled = busy;

    // A1-01: File▾Open Data / Run .do / File▾Open do-file… are plain <a>
    // toolbar links (no native `disabled`) — onOpenFile()/onRunDo()/
    // onOpenDoFile() already guard on state.busy so this is belt-and-suspenders,
    // plus an a11y signal. `#toolbar .toolbar-menu a.busy` (piet.css) dims +
    // pointer-events:none's any matching descendant, including dropdown-content
    // items (BUG-046) — no new CSS needed.
    const openLink   = document.querySelector('[data-action="open"]');
    const doLink     = document.querySelector('[data-action="do"]');
    const doOpenLink = document.querySelector('[data-action="do-open"]');
    for (const el of [openLink, doLink, doOpenLink]) {
      if (!el) continue;
      el.classList.toggle('busy', busy);
      el.setAttribute('aria-disabled', busy ? 'true' : 'false');
    }

    // A1-02: visual/pointer-events lock signal on the Variable Editor grid
    // while a command/mutation is in flight (the piet:var-edit listener and
    // the grid's cell-commit callback both also guard on state.busy directly —
    // see above). Same `.busy`-needs-a-piet.css-rule caveat as the toolbar
    // links.
    const varWrap = document.getElementById('var-list-wrap');
    if (varWrap) varWrap.classList.toggle('busy', busy);
  }

  async function runPastedBlock(source) {
    if (!source.trim() || state.busy) return;
    setBusy(true);
    try {
      appendBlock({ type: 'echo', text: 'do (pasted)' });
      startProgressPolling();                          // B1: live liveness strip
      const result = await getClient().runDoString(source);
      applyDoResult(result);
      const ok = !(result.blocks ?? []).some(b => b.type === 'error');
      pushHistory('do (pasted)', ok);
      appendHistory('do (pasted)', ok);
    } catch (err) {
      appendBlock({ type: 'error', code: 'engine_error', message: String(err.message ?? err) });
      checkKernel();   // A1-03: refresh the kernel-dot after a surfaced engine_error
    } finally {
      stopProgressPolling();
      setBusy(false);
      document.getElementById('cmd-input')?.focus();
    }
  }

  function isBusy() { return state.busy; }
  function getVariables() { return state.varlist; }
  function applyViewMutation(result) {
    state.varlist = result.varlist_after;
    state.nCols = result.varlist_after.length;
    updateVariables(result.varlist_after);
    if (result.n_rows_after != null) state.nRows = result.n_rows_after;
    updateStatus();
  }
  function seedMockDataset(vl) {
    state.varlist = vl.varlist;
    state.nRows = vl.n_rows ?? 0;
    state.nCols = vl.n_cols ?? vl.varlist.length;
    state.currentFile = vl.source_file ?? 'fatalities.csv';
    updateVariables(vl.varlist);
    updateStatus();
  }

  function bindFileControls() {
    // File open
    const openLink = document.querySelector('[data-action="open"]');
    if (openLink) openLink.addEventListener('click', e => { e.preventDefault(); onOpenFile(); });

    // Run .do
    const doLink = document.querySelector('[data-action="do"]');
    if (doLink) doLink.addEventListener('click', e => { e.preventDefault(); onRunDo(); });

    // File ▾ > Open do-file… (BUG-046): load a .do file into the editor
    // WITHOUT running it — onRunDo's pre-BUG-045 behavior, relocated here.
    const doOpenLink = document.querySelector('[data-action="do-open"]');
    if (doOpenLink) doOpenLink.addEventListener('click', e => { e.preventDefault(); onOpenDoFile(); });
  }

  function bindVariableEditing() {
    // Variable Editor mutations (dispatched by panels.js)
    document.addEventListener('piet:var-edit', async (e) => {
      // A1-02: don't race a command/do-run/other mutation already in flight.
      if (isBusy()) return;
      const { action, ...params } = e.detail;
      setBusy(true);
      try {
        let result;
        if      (action === 'rename')         result = await getClient().rename(params.varname, params.newname);
        else if (action === 'label-variable') result = await getClient().labelVariable(params.varname, params.label);
        else if (action === 'label-define')   result = await getClient().labelDefine(params.lblname, params.mapping);
        else if (action === 'label-values')   result = await getClient().labelValues(params.varname, params.lblname);
        else if (action === 'sort')           result = await getClient().sort(params.varlist, params.ascending);
        else if (action === 'recode')         result = await getClient().recode(params.varname, params.rules, params.gen);
        else return;

        if (result.type === 'error') {
          appendBlock(result);
          updateVariables(state.varlist);
        } else if (result && result.varlist_after) {
          applyViewMutation(result);
        } else {
          // A4-02: malformed DataMutationResult (non-error type, but missing
          // varlist_after — the native envelope bug A4-01 is being fixed
          // separately by WS-E1) — don't crash the listener, skip the refresh.
          updateVariables(state.varlist);
        }
      } catch (err) {
        appendBlock({ type: 'error', code: 'engine_error', message: String(err.message ?? err) });
        updateVariables(state.varlist);
        checkKernel();   // A1-03: refresh the kernel-dot after a surfaced engine_error
      } finally {
        setBusy(false);
        document.getElementById('cmd-input')?.focus();
      }
    });
  }

  return { checkKernel, parseSaveTarget, runCommand, scheduleAutoSummarize, runAutoSummarize, openRecentFile, onOpenFile, noteWorkingDirectory, onRunDo, routeDroppedFile, onOpenDoFile, runDoSourceCore, _tailBlocksToRender, applyDoResult, refreshVarlistAuthoritative, updateStatus, setBusy, runPastedBlock, isBusy, getVariables, applyViewMutation, seedMockDataset, bindFileControls, bindVariableEditing };
}
