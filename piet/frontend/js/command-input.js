// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Input normalization, paste preference, history recall, and completion.
import { getVarlist } from './panels.js';
import { normalizeSmartQuotes } from './input-text.js';

export function createCommandInputController({ runCommand, runPastedBlock, openDoEditor, flashStatus }) {
  const state = { commandHistory: [], historyIndex: -1 };
  let _tabCycle = { prefix: '', stemBefore: '', matches: [], idx: -1 };

  function pushHistory(cmd, success) {
    state.commandHistory.push({ cmd, success });
    if (state.commandHistory.length > 500) state.commandHistory.shift();
  }

  function submitCommand() {
    const inputEl = document.getElementById('cmd-input');
    if (!inputEl) return;
    // Defensive: if the value carries a newline (e.g. a future textarea), treat it
    // as a pasted do-block rather than mangling it into one command (BUG-010).
    // Design decision 4: same routing as onInputPaste (pref-gated).
    if (inputEl.value.includes('\n')) {
      const block = inputEl.value;
      inputEl.value = '';
      state.historyIndex = -1;
      handleMultilineBlock(block);
      return;
    }
    const cmd = inputEl.value.trim();
    inputEl.value = '';
    state.historyIndex = -1;
    if (cmd) runCommand(cmd);
  }


  function onInputPaste(e) {
    const raw = e.clipboardData?.getData('text');
    if (!raw) return;
    const text = raw.replace(/\r\n/g, '\n').replace(/\r/g, '\n');
    const nonEmpty = text.split('\n').filter(l => l.trim()).length;
    if (nonEmpty < 2) {
      const normalized = normalizeSmartQuotes(text);
      if (normalized === text) return;   // nothing to convert: native insert, untouched
      e.preventDefault();
      const el = e.target;
      const start = el.selectionStart ?? el.value.length;
      const end   = el.selectionEnd ?? el.value.length;
      el.setRangeText(normalized, start, end, 'end');
      flashStatus('Smart quotes converted to straight quotes');
      return;
    }
    e.preventDefault();
    e.target.value = '';
    const normalized = normalizeSmartQuotes(text);
    if (normalized !== text) flashStatus('Smart quotes converted to straight quotes');
    handleMultilineBlock(normalized);
  }

  function handleMultilineBlock(text) {
    if (localStorage.getItem('piet-paste-autorun') === '1') {
      runPastedBlock(text);
    } else {
      loadBlockIntoDoEditor(text);
    }
  }

  function loadBlockIntoDoEditor(text) {
    const ta = document.getElementById('doedit-text');
    if (ta) ta.value = ta.value ? `${ta.value}\n${text}` : text;
    openDoEditor();
  }

  function onInputKeydown(e) {
    const inputEl = e.target;

    if (e.key === 'Enter') {
      submitCommand();
      return;
    }

    // History recall: ArrowUp / PageUp
    if (e.key === 'ArrowUp' || e.key === 'PageUp') {
      e.preventDefault();
      const hist = state.commandHistory;
      if (!hist.length) return;
      if (state.historyIndex === -1) state.historyIndex = hist.length;
      if (state.historyIndex > 0) {
        state.historyIndex--;
        inputEl.value = hist[state.historyIndex].cmd;
        inputEl.select();
      }
      return;
    }

    // History forward: ArrowDown / PageDown
    if (e.key === 'ArrowDown' || e.key === 'PageDown') {
      e.preventDefault();
      const hist = state.commandHistory;
      if (state.historyIndex === -1 || state.historyIndex >= hist.length - 1) {
        state.historyIndex = -1;
        inputEl.value = '';
      } else {
        state.historyIndex++;
        inputEl.value = hist[state.historyIndex].cmd;
        inputEl.select();
      }
      return;
    }

    // Tab: varname completion with multi-match cycling
    if (e.key === 'Tab') {
      e.preventDefault();
      const varlist = getVarlist();
      if (!varlist.length) return;

      const val        = inputEl.value;
      const pos        = inputEl.selectionStart;
      const before     = val.slice(0, pos);
      const word       = before.match(/\S+$/)?.[0] ?? '';
      const stemBefore = before.slice(0, before.length - word.length);
      if (!word) return;

      const inCycle = _tabCycle.stemBefore === stemBefore &&
                      _tabCycle.idx >= 0 &&
                      _tabCycle.matches[_tabCycle.idx]?.name === word;

      if (!inCycle) {
        _tabCycle.prefix     = word;
        _tabCycle.stemBefore = stemBefore;
        _tabCycle.matches    = varlist.filter(v => v.name.startsWith(word));
        _tabCycle.idx        = -1;
      }
      if (!_tabCycle.matches.length) return;

      _tabCycle.idx = (_tabCycle.idx + 1) % _tabCycle.matches.length;
      const match   = _tabCycle.matches[_tabCycle.idx];
      const after   = val.slice(pos);
      inputEl.value = stemBefore + match.name + after;
      const newPos  = stemBefore.length + match.name.length;
      inputEl.setSelectionRange(newPos, newPos);
      return;
    }
  }

  function resetHistoryIndex() { state.historyIndex = -1; }

  function bindControls() {
    // Command input
    const inputEl = document.getElementById('cmd-input');
    if (inputEl) {
      inputEl.addEventListener('keydown', onInputKeydown);
      inputEl.addEventListener('paste', onInputPaste);
    }

    // Run button — same path as Enter (Stata-like explicit submit)
    const runBtn = document.getElementById('btn-run');
    if (runBtn) {
      runBtn.addEventListener('click', () => {
        submitCommand();
        document.getElementById('cmd-input')?.focus();
      });
    }
  }

  return { pushHistory, submitCommand, normalizeSmartQuotes, onInputPaste, handleMultilineBlock, loadBlockIntoDoEditor, onInputKeydown, resetHistoryIndex, bindControls };
}
