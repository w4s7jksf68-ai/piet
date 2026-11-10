// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Blocking native questions: answer or cancel; ordinary overlays cannot dismiss it.
import { setMemoryBudgetMb } from './console.js';
import { setGridMemoryBudgetMb } from './grid.js';

export function createRequestDialogController({ getClient, invalidateSettingsCache }) {
  let _pendingRequestId = null;

  function openRequestDialog(detail) {
    const ov = document.getElementById('request-overlay');
    if (!ov) return;
    // Defensive: the LOCKED wire spec always sends requestId, but don't
    // silently show an unanswerable dialog if it's ever missing.
    if (!detail || !detail.requestId) {
      console.error('request_dialog event missing requestId — ignoring', detail);
      return;
    }
    _pendingRequestId = detail.requestId;
    const macroEl  = document.getElementById('request-macro');
    const promptEl = document.getElementById('request-prompt');
    const inputEl  = document.getElementById('request-input');
    const statusEl = document.getElementById('request-status');
    if (macroEl) {
      macroEl.textContent = detail.macroName
        ? `Do-run is paused — waiting on “${detail.macroName}”`
        : 'Do-run is paused, waiting for your answer.';
    }
    if (promptEl) promptEl.textContent = detail.prompt || '';
    if (inputEl)  inputEl.value = '';
    if (statusEl) statusEl.textContent = '';
    setRequestDialogBusy(false);
    // SPEC II-4 (2026-09-02, contract_B additive): a push may carry `choices`
    // ([{value,label}]) — a fixed-choice question (the memory pre-flight's
    // proceed / raise budget / cancel) instead of free text. Render buttons and
    // hide the text form; the answer wire is unchanged ({requestId, value}).
    renderRequestChoices(detail);
    ov.classList.add('open');
    if (!(Array.isArray(detail.choices) && detail.choices.length)) inputEl?.focus();
  }

  function renderRequestChoices(detail) {
    const form = document.getElementById('request-form');
    let box = document.getElementById('request-choices');
    const choices = detail && Array.isArray(detail.choices) ? detail.choices : [];
    if (!choices.length) {
      if (box) box.replaceChildren();
      if (box) box.hidden = true;
      if (form) form.hidden = false;
      return;
    }
    if (!box) {
      box = document.createElement('div');
      box.id = 'request-choices';
      box.className = 'request-actions request-choices';
      const statusEl = document.getElementById('request-status');
      (statusEl?.parentNode || form?.parentNode || document.body).insertBefore(box, statusEl || null);
    }
    box.replaceChildren();
    box.hidden = false;
    if (form) form.hidden = true;
    for (const c of choices) {
      if (!c || typeof c.value !== 'string') continue;
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'doedit-btn' + (c.value === 'cancel' ? ' doedit-btn-danger' : (c.value === 'proceed' ? ' doedit-run' : ''));
      b.dataset.value = c.value;
      b.textContent = c.label || c.value;
      b.addEventListener('click', () => onRequestChoice(c.value, detail));
      box.appendChild(b);
    }
  }

  async function onRequestChoice(value, detail) {
    if (!_pendingRequestId) return;
    const statusEl = document.getElementById('request-status');
    const box = document.getElementById('request-choices');
    if (box) for (const b of box.querySelectorAll('button')) b.disabled = true;
    try {
      if (value === 'raise' && detail && Number(detail.suggest_budget_mb) > 0) {
        const mb = Math.round(Number(detail.suggest_budget_mb));
        await getClient().setSettings({ memory_budget_mb: mb });
        setMemoryBudgetMb(mb);
        setGridMemoryBudgetMb(mb);
        invalidateSettingsCache();   // the panel re-reads on next open
      }
      await getClient().answerRequest(_pendingRequestId, value);
      closeRequestDialog();
    } catch (err) {
      if (statusEl) statusEl.textContent = '⚠ Could not send answer: ' + String(err && err.message ? err.message : err);
      if (box) for (const b of box.querySelectorAll('button')) b.disabled = false;
    }
  }

  function closeRequestDialog() {
    const ov = document.getElementById('request-overlay');
    if (ov) ov.classList.remove('open');
    _pendingRequestId = null;
    renderRequestChoices(null);   // back to the text form for the next push
    document.getElementById('cmd-input')?.focus();
  }

  function setRequestDialogBusy(busy) {
    const submitBtn = document.getElementById('request-submit');
    const cancelBtn = document.getElementById('request-cancel-run');
    const inputEl   = document.getElementById('request-input');
    if (submitBtn) submitBtn.disabled = busy;
    if (cancelBtn) cancelBtn.disabled = busy;
    if (inputEl)   inputEl.disabled   = busy;
  }

  async function onRequestSubmit(e) {
    e.preventDefault();
    if (!_pendingRequestId) return;
    const inputEl  = document.getElementById('request-input');
    const statusEl = document.getElementById('request-status');
    const value = inputEl ? inputEl.value : '';
    setRequestDialogBusy(true);
    if (statusEl) statusEl.textContent = '';
    try {
      await getClient().answerRequest(_pendingRequestId, value);
      closeRequestDialog();
    } catch (err) {
      // Leave the modal open on failure — the native thread is presumably
      // still waiting, so a failed POST (transient transport error) must stay
      // retryable rather than silently stranding the run.
      if (statusEl) {
        statusEl.textContent = '⚠ Could not send answer: '
          + String(err && err.message ? err.message : err);
      }
      setRequestDialogBusy(false);
      inputEl?.focus();
    }
  }

  async function onRequestCancelRun() {
    setRequestDialogBusy(true);
    try {
      await getClient().runCancel();
    } catch {
      // A1-06-style: a failed cancel REQUEST is a transport hiccup, not proof
      // the run is unrecoverable — dismiss anyway (see the edge-case note
      // above); staying stuck open with no way out would be worse than closing.
    }
    closeRequestDialog();
  }

  function bindControls() {
    // D9 (BETA Phase F): request-dialog overlay — Submit posts the answer;
    // Cancel run reuses the same /api/run_cancel route as #rp-cancel above. No
    // Escape/outside-click dismissal is wired here on purpose (see
    // openRequestDialog()'s doc comment) — the overlay itself swallows Escape
    // (below) so the document-level Escape handler (OVERLAYS registry) never
    // sees it, instead of relying on #request-overlay's absence from that
    // registry alone.
    const requestForm = document.getElementById('request-form');
    if (requestForm) requestForm.addEventListener('submit', onRequestSubmit);
    const requestCancelBtn = document.getElementById('request-cancel-run');
    if (requestCancelBtn) requestCancelBtn.addEventListener('click', onRequestCancelRun);
    const requestOverlay = document.getElementById('request-overlay');
    if (requestOverlay) {
      requestOverlay.addEventListener('keydown', e => {
        if (e.key === 'Escape') e.stopPropagation();
      });
    }
  }

  return { openRequestDialog, renderRequestChoices, onRequestChoice, closeRequestDialog, setRequestDialogBusy, onRequestSubmit, onRequestCancelRun, bindControls };
}
