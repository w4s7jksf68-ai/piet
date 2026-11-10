// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Do-file buffer identity, dirty state, selection, and editor-run interaction.

import { normalizeSmartQuotes } from './input-text.js';
import { setGutterErrorLine } from './gutter.js';

export function createEditorController({ layout, isBusy, setBusy, runDoSourceCore, flashStatus }) {
  let _doEditorPath = null;
  let _doEditorDirty = false;
  const { dockSurface, closeAllOverlays, syncViewTabs, _isOverlayOpen, isSplitOn, onViewTabClick } = layout;

  function setDoEditorText(src) {
    const ta = document.getElementById('doedit-text');
    if (ta && typeof src === 'string') ta.value = src;
    markDoEditorClean();   // a fresh load has no unsaved changes yet
  }

  function setDoEditorPath(path) {
    _doEditorPath = (path && typeof path === 'string') ? path : null;
    renderDoEditorTitle();
  }

  function renderDoEditorTitle() {
    const el = document.getElementById('doedit-title');
    if (!el) return;
    const mark = _doEditorDirty ? ' *' : '';
    if (_doEditorPath) {
      const base = _doEditorPath.split('/').pop().split('\\').pop();
      el.textContent = `Do-file Editor — ${base}${mark}`;
      el.title = _doEditorPath;
    } else {
      el.textContent = `Do-file Editor${mark}`;
      el.removeAttribute('title');
    }
  }

  function markDoEditorDirty() {
    if (_doEditorDirty) return;   // already marked — skip the redundant re-render
    _doEditorDirty = true;
    renderDoEditorTitle();
  }

  function markDoEditorClean() {
    if (!_doEditorDirty) return;
    _doEditorDirty = false;
    renderDoEditorTitle();
  }

  function selectEditorLine(lineNum) {
    const ta = document.getElementById('doedit-text');
    if (!ta || lineNum == null) return;
    const lines = ta.value.split('\n');
    const idx = Math.max(0, Math.min(lineNum - 1, lines.length - 1));
    let start = 0;
    for (let i = 0; i < idx; i++) start += lines[i].length + 1;   // +1 for the '\n'
    const end = start + lines[idx].length;
    ta.focus();
    ta.setSelectionRange(start, end);
    // textarea has no native "scroll to selection" API — approximate from line
    // height, with a little context above the target line.
    const lineHeight = parseFloat(getComputedStyle(ta).lineHeight) || 16;
    ta.scrollTop = Math.max(0, (idx - 3) * lineHeight);
  }

  function openDoEditor() {
    const ov = document.getElementById('doedit-overlay');
    if (!ov) return;
    // 2.2.0 item C: in split-view the editor is a docked column beside Results
    // (coexists with the other surfaces) — don't close anything, just dock it.
    if (isSplitOn()) { dockSurface('doedit-overlay', true); document.getElementById('doedit-text')?.focus(); return; }
    // Mission 1: close whichever other overlay shares the console area first
    // (registry-driven — replaces the old ad hoc per-function close subset).
    closeAllOverlays('doedit-overlay');
    ov.classList.add('open');
    document.getElementById('doedit-text')?.focus();
    syncViewTabs();   // UX-047: Do-file tab active
  }

  function closeDoEditor() {
    // 2.2.0 item C: in split-view "close" = undock the editor column.
    if (isSplitOn()) { dockSurface('doedit-overlay', false); return; }
    const ov = document.getElementById('doedit-overlay');
    if (ov) ov.classList.remove('open');
    document.getElementById('cmd-input')?.focus();
    syncViewTabs();   // UX-047: Results tab active again (if nothing else is open)
  }

  async function runDoEditor() {
    const ta = document.getElementById('doedit-text');
    if (!ta) return;
    await _runEditorBuffer(ta.value, 'do (editor)');
  }

  async function runDoEditorSelection() {
    const ta = document.getElementById('doedit-text');
    if (!ta) return;
    const start = ta.selectionStart ?? 0;
    const end   = ta.selectionEnd ?? 0;
    const hasSel = end > start;
    const source = hasSel ? ta.value.slice(start, end) : ta.value;
    // U6: a selection-run's stopped_at_line is relative to the SUBMITTED
    // source, not the whole buffer — count the newlines before the selection
    // start so runDoSourceCore can point the editor at the right absolute line.
    const lineOffset = hasSel ? (ta.value.slice(0, start).match(/\n/g) ?? []).length : 0;
    await _runEditorBuffer(source, hasSel ? 'do (selection)' : 'do (editor)', lineOffset);
  }

  async function _runEditorBuffer(source, label, lineOffset = 0) {
    if (!source.trim() || isBusy()) return;
    setBusy(true);
    const runBtn = document.getElementById('doedit-run');
    if (runBtn) runBtn.disabled = true;
    // A6 (P8.1-T2): the new Run-selection button shares the same busy-guard as
    // ▶ Run — both funnel through this one shared core.
    const runSelBtn = document.getElementById('doedit-run-selection');
    if (runSelBtn) runSelBtn.disabled = true;
    try {
      // Single-view: hide the editor so results are visible in the console
      // (runDoSourceCore re-opens it iff the run stops on an error). Split-view:
      // Results is already a column beside the editor — leave the editor docked.
      if (!isSplitOn()) closeDoEditor();
      await runDoSourceCore(source, label, lineOffset);
    } finally {
      setBusy(false);
      if (runBtn) runBtn.disabled = false;
      if (runSelBtn) runSelBtn.disabled = false;
      // Don't steal focus from the editor if runDoSourceCore just re-opened it
      // (single-view stop-on-error) or it's a docked column (split).
      if (!_isOverlayOpen('doedit-overlay')) document.getElementById('cmd-input')?.focus();
    }
  }

  function bindControls() {
    // Do-file editor (in-app) — 2.2.0 item C: the view tab toggles the docked
    // column in split-view, opens the exclusive overlay in single-view.
    const doEditLink = document.querySelector('[data-action="doedit"]');
    if (doEditLink) doEditLink.addEventListener('click', e => { e.preventDefault(); onViewTabClick('doedit-overlay', openDoEditor, closeDoEditor); });
    // UX-047: #doedit-close (the "← Back to Results" button) was removed from
    // index.html — Escape + the Results view tab generalize it (III-6). Its
    // wiring is gone too, not just left as a guarded no-op.
    const doEditClear = document.getElementById('doedit-clear');
    if (doEditClear) doEditClear.addEventListener('click', () => {
      const ta = document.getElementById('doedit-text');
      if (ta) { ta.value = ''; ta.focus(); }
      markDoEditorClean();   // dirty dot (P8.1-T5): Clear is itself a no-unsaved-changes state
    });
    const doEditRun = document.getElementById('doedit-run');
    if (doEditRun) doEditRun.addEventListener('click', runDoEditor);
    // A6 (P8.1-T2): explicit Run-selection button — same runDoEditorSelection()
    // path as the ⇧⌘D keybinding (doedit-text's keydown handler above).
    const doEditRunSelection = document.getElementById('doedit-run-selection');
    if (doEditRunSelection) doEditRunSelection.addEventListener('click', runDoEditorSelection);
    const doEditText = document.getElementById('doedit-text');
    if (doEditText) {
      doEditText.addEventListener('keydown', e => {
        // 2.2.0 item B: Execute selection (do) — Stata's Do-button keybinding
        // (⇧⌘D on macOS / Ctrl+Shift+D elsewhere; verified against [GSM] 13's
        // "If text is highlighted, the button becomes Execute selection (do)").
        // Runs the highlighted range alone — or the whole buffer if nothing is
        // highlighted (Stata parity) — through the SAME do_string path Run .do
        // uses. Checked before the whole-file Cmd/Ctrl+Enter binding (distinct key).
        if ((e.ctrlKey || e.metaKey) && e.shiftKey && (e.key === 'd' || e.key === 'D')) {
          e.preventDefault();
          runDoEditorSelection();
          return;
        }
        if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') { e.preventDefault(); runDoEditor(); return; }
        // Escape is handled by the single top-level overlay handler below (A2-05).
        // Hygiene (design 8): Tab inserts 4 spaces instead of moving focus off
        // the textarea. setRangeText repositions the caret after the inserted
        // text ('end' mode); it does not participate in the native undo stack
        // (a known, accepted trade-off — execCommand('insertText') would, but
        // is deprecated and inconsistently supported across engines).
        if (e.key === 'Tab') {
          e.preventDefault();
          const ta = e.target;
          ta.setRangeText('    ', ta.selectionStart, ta.selectionEnd, 'end');
          markDoEditorDirty();   // setRangeText is programmatic — no native 'input' event
        }
      });
      // A11 (P8.1-T5): same smart-quote normalization as #cmd-input's paste
      // handler — the do-editor is exactly where a pasted `use "curly file"`
      // do-block would otherwise carry the un-normalized quotes straight in.
      doEditText.addEventListener('paste', e => {
        const raw = e.clipboardData?.getData('text');
        if (!raw) return;
        const normalized = normalizeSmartQuotes(raw);
        if (normalized === raw) return;   // nothing to convert: native insert, untouched
        e.preventDefault();
        const start = doEditText.selectionStart ?? doEditText.value.length;
        const end   = doEditText.selectionEnd ?? doEditText.value.length;
        doEditText.setRangeText(normalized, start, end, 'end');
        markDoEditorDirty();   // setRangeText is programmatic — no native 'input' event
        flashStatus('Smart quotes converted to straight quotes');
      });
      // Dirty dot (P8.1-T5): any normal (native) edit — typing, native paste
      // insert, cut, IME composition, … — fires 'input'; the two programmatic
      // setRangeText() paths above (Tab, smart-quote paste) mark dirty inline
      // since setRangeText does NOT dispatch 'input' itself.
      doEditText.addEventListener('input', markDoEditorDirty);
    }
  }

  function bindNavigationEvents() {
    // B6 (P8-S7): History row's second verb (panels.js's hover-revealed "Send
    // to Do-file Editor" button) — panels.js has no app.js import (it already
    // dispatches piet:var-edit for the same reason), so this mirrors that
    // CustomEvent idiom rather than introducing a new cross-module call.
    document.addEventListener('piet:hist-send-doedit', e => {
      const sendCmd = e.detail?.cmd;
      if (!sendCmd) return;
      const ta = document.getElementById('doedit-text');
      if (ta) ta.value = ta.value ? `${ta.value}\n${sendCmd}` : sendCmd;
      openDoEditor();
    });

    // T4 (P8.1): error-block click → jump to the failing line in the Do-file
    // Editor (console.js's renderError dispatches this when the block carries
    // line_number — same CustomEvent idiom as piet:hist-send-doedit above).
    document.addEventListener('piet:error-line-click', e => {
      const line = e.detail?.line;
      if (line == null) return;
      openDoEditor();
      selectEditorLine(line);
      setGutterErrorLine(line);
    });
  }

  return { setDoEditorText, setDoEditorPath, renderDoEditorTitle, markDoEditorDirty, markDoEditorClean, selectEditorLine, openDoEditor, closeDoEditor, runDoEditor, runDoEditorSelection, _runEditorBuffer, bindControls, bindNavigationEvents };
}
