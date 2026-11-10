// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Progress polling, native streamed blocks, and native file-drop events.
import { appendBlocks } from './console.js';
import { withMutationSummaries } from './result-blocks.js';

export function createRunProgressController({ getClient, openRequestDialog, routeDroppedFile }) {
  let _progressTimer = null;
  let _progressTier1Timer = null;
  let _progressTier2Timer = null;
  let _progressTier1Reached = false;
  let _progressTier2Reached = false;
  let _progressLastSnap = null;
  let _streamedSeq = 0;

  function initPietEventsBridge() {
    if (typeof window === 'undefined' || !window.pietEvents) return;
    window.pietEvents.addEventListener('piet-event', (ev) => {
      const snap = ev.detail;
      // D9 (BETA Phase F): request_dialog is a NEW discriminated push — the
      // native do-run thread is blocked mid-run waiting on an answer. Checked
      // FIRST via the "kind" discriminator (existing RunProgress ticks carry no
      // such field, so this can never misfire against them); everything below
      // is the pre-existing shape-based RunProgress handling, untouched.
      if (snap && typeof snap === 'object' && snap.kind === 'request_dialog') {
        openRequestDialog(snap);
        return;
      }
      // N6 (P8.1-W2, contract_B v2.4.0): {kind:"block", seq, block} — a do/
      // do_string run's blocks streamed live as they complete, same
      // discriminator idiom as request_dialog just above (checked first, for
      // the same reason: existing RunProgress ticks carry no "kind" field at
      // all, so this can never misfire against them). appendBlock() renders it
      // immediately; applyDoResult()'s FINAL render (below) dedups against the
      // response's additive `streamed` count, so nothing double-renders.
      if (snap && typeof snap === 'object' && snap.kind === 'block') {
        // N6 integration fix: route through withMutationSummaries so a streamed
        // data_mutation block gets its merge-counts/n_dropped/reshape decoration
        // exactly like the batch path renders it — the synthesized display
        // blocks are client-side extras and do NOT advance the wire seq count
        // (dedup counts WIRE blocks only, matching the contract's `streamed`).
        appendBlocks(withMutationSummaries([snap.block]));
        _streamedSeq++;
        return;
      }
      // S6 (OVERHEAD-ZERO, drag-and-drop file open): the native window/webview
      // delegate (shell.cpp's installFileDropHandler()) is the ONLY thing that
      // ever sees a dropped file's real filesystem path — WKWebView never
      // exposes one to page script. It pushes the real path here, same "kind"
      // discriminator idiom as request_dialog/block above. Route it through
      // onOpenFile(path) — the SAME knownPath leg openRecentFile() already
      // uses (see above) — so a dropped file is byte-identical to picking it
      // via the toolbar: same /api/open -> handlers::runUse() path the `use`
      // command runs, same card, same errors. Guard the path itself so a
      // malformed push can never pop the native file picker as a side effect
      // (onOpenFile() falls back to pickFile() when its argument is falsy).
      if (snap && typeof snap === 'object' && snap.kind === 'file_dropped') {
        if (typeof snap.path === 'string' && snap.path) routeDroppedFile(snap.path);
        return;
      }
      // RunProgress ticks are self-identifying by shape (running + current_line
      // + total_lines) — other future pietEvents payloads (none yet) would be
      // ignored here rather than mis-rendered.
      if (snap && typeof snap === 'object' && 'running' in snap && 'current_line' in snap) {
        _progressLastSnap = snap;
        _paintProgress();
      }
    });
  }

  function _fmtProgress(snap) {
    const line = snap.total_lines
      ? `${snap.current_line}/${snap.total_lines}`
      : `${snap.current_line}`;
    const cmd = (snap.current_cmd || '').trim();
    const secs = Math.floor((snap.elapsed_ms || 0) / 1000);
    const m = Math.floor(secs / 60), s = secs % 60;
    const elapsed = m ? `${m}m ${String(s).padStart(2, '0')}s` : `${s}s`;
    const parts = [`running ${line}`];
    if (cmd) parts.push(cmd);
    parts.push(elapsed);
    return parts.join(' · ');
  }

  function _paintProgress() {
    const bar = document.getElementById('run-progress');
    const txt = document.getElementById('rp-text');
    const cancelBtn = document.getElementById('rp-cancel');
    if (!bar || !txt) return;
    const snap = _progressLastSnap;
    const running = !!(snap && snap.running);
    if (!running || !_progressTier1Reached) {
      bar.hidden = true;
      if (cancelBtn) { cancelBtn.hidden = true; cancelBtn.disabled = true; }
      return;
    }
    bar.hidden = false;
    if (_progressTier2Reached) {
      txt.textContent = _fmtProgress(snap);
      if (cancelBtn) { cancelBtn.hidden = false; cancelBtn.disabled = false; }
    } else {
      txt.textContent = 'running…';
      if (cancelBtn) { cancelBtn.hidden = true; cancelBtn.disabled = true; }
    }
  }

  function startProgressPolling() {
    stopProgressPolling();
    _streamedSeq = 0;   // N6: fresh run — reset the live-push bookkeeping counter
    // B21-1: was `setTimeout(..., 1000)` — N6 streams blocks live during a
    // do-run, so gating the strip's own appearance behind a 1s delay meant
    // fast runs rendered their (streamed) result blocks BEFORE the strip that
    // announces the run is in progress — the loading cue trailing its own
    // content. Setting the flag synchronously means the very first snapshot
    // (the immediate poll() below, or an early native piet-event push) paints
    // the strip the moment there's something real to show, with no artificial
    // floor. Nothing shows before that first snapshot regardless — see
    // _paintProgress()'s `!running` check, and stopProgressPolling() just
    // above cleared _progressLastSnap back to null.
    _progressTier1Reached = true;
    _paintProgress();
    _progressTier2Timer = setTimeout(() => { _progressTier2Reached = true; _paintProgress(); }, 10000);
    const poll = async () => {
      try {
        const snap = await getClient().runProgress();
        _progressLastSnap = snap;
        _paintProgress();
      } catch { /* transient — keep polling */ }
    };
    poll();                                   // immediate first paint (tier1 already open; tier2 still gated)
    _progressTimer = setInterval(poll, 1000); // ~1s cadence
  }

  function stopProgressPolling() {
    if (_progressTimer)      { clearInterval(_progressTimer);      _progressTimer = null; }
    if (_progressTier1Timer) { clearTimeout(_progressTier1Timer);  _progressTier1Timer = null; }
    if (_progressTier2Timer) { clearTimeout(_progressTier2Timer);  _progressTier2Timer = null; }
    _progressTier1Reached = false;
    _progressTier2Reached = false;
    _progressLastSnap = null;
    const bar = document.getElementById('run-progress');
    if (bar) bar.hidden = true;
    const cancelBtn = document.getElementById('rp-cancel');
    if (cancelBtn) { cancelBtn.hidden = true; cancelBtn.disabled = true; }
  }

  async function onCancelRun() {
    const cancelBtn = document.getElementById('rp-cancel');
    if (cancelBtn) cancelBtn.disabled = true;   // debounce; re-enabled on next run
    const txt = document.getElementById('rp-text');
    if (txt) txt.textContent = 'cancelling…';
    try {
      await getClient().runCancel();
    } catch {
      // A1-06: a failed cancel REQUEST (network/transport error, not a failed
      // cancellation) shouldn't leave the button stuck disabled until the next
      // run starts — the run is presumably still alive, so let the user retry
      // immediately.
      if (cancelBtn) cancelBtn.disabled = false;
    }
  }

  function bindCancelControl() {
    // B1 (1.16.0): run-progress strip cancel button.
    const rpCancel = document.getElementById('rp-cancel');
    if (rpCancel) rpCancel.addEventListener('click', onCancelRun);
  }

  return { initPietEventsBridge, _fmtProgress, _paintProgress, startProgressPolling, stopProgressPolling, onCancelRun, bindCancelControl };
}
