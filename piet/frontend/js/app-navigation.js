// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Menu gestures and application-level keyboard navigation; controllers own state.
import { openFind, closeFind, isFindOpen } from './find.js';

export function bindAppNavigation({ layout, settings, execution, onClear }) {
  const { _isOverlayOpen, isSwapArmed, _disarmSwap } = layout;
  const { openSettings, closeSettings } = settings;
  const { runCommand, onOpenFile } = execution;
  // File ▾ / Data ▾ dropdowns: click-to-toggle .open class on each
  // .menu-dropdown (A2-04: click-toggle ONLY, app.js is the one owner — see
  // piet.css's comment on .menu-dropdown-content). BUG-046 added a second
  // .menu-dropdown (File ▾) alongside the original Data ▾, so this now
  // iterates every .menu-dropdown instead of assuming there is exactly one
  // (a bare querySelector() here would silently wire up only the first).
  const menuDropdowns = document.querySelectorAll('.menu-dropdown');
  menuDropdowns.forEach(dropdown => {
    const trigger = dropdown.querySelector(':scope > a');
    if (!trigger) return;
    trigger.addEventListener('click', e => {
      e.preventDefault();
      e.stopPropagation();
      // Single-menu-open: opening one closes any other already-open dropdown.
      menuDropdowns.forEach(d => { if (d !== dropdown) d.classList.remove('open'); });
      dropdown.classList.toggle('open');
    });
    // A12: a real item click (Open Data…, sysuse fatalities, browse, …) is
    // INSIDE the dropdown, so the outside-click handler below never sees it
    // and the menu stayed open after a selection. Close it here too — the
    // item's own [data-action]/[data-run] handler (wired elsewhere) still
    // runs; this only ALSO closes .open, deferred so it doesn't race that
    // handler's own work.
    const content = dropdown.querySelector(':scope > .menu-dropdown-content');
    if (content) {
      content.addEventListener('click', () => {
        setTimeout(() => menuDropdowns.forEach(d => d.classList.remove('open')), 0);
      });
    }
  });
  // Close any open dropdown on outside click
  document.addEventListener('click', e => {
    menuDropdowns.forEach(dropdown => {
      if (!dropdown.contains(e.target)) dropdown.classList.remove('open');
    });
  });

  // Menu items that RUN a command immediately (e.g. Data Browser / Data Editor)
  document.querySelectorAll('[data-run]').forEach(el => {
    el.addEventListener('click', e => {
      e.preventDefault();
      runCommand(el.dataset.run);
    });
  });

  // Global keyboard shortcuts
  document.addEventListener('keydown', e => {
    if ((e.ctrlKey || e.metaKey) && e.key === 'l') {
      e.preventDefault();
      document.getElementById('cmd-input')?.focus();
    }
    if ((e.ctrlKey || e.metaKey) && e.key === 'End') {
      e.preventDefault();
      const cb = document.getElementById('console-body');
      if (cb) cb.scrollTop = cb.scrollHeight;
    }
    if ((e.ctrlKey || e.metaKey) && e.key === 'k') {
      e.preventDefault();
      onClear();
    }
    // B11: Cmd/Ctrl+F summons the find bar, scoped to whichever console
    // surface is active (find.js's own detection — see its _activeSurface()).
    // Not a persistent-chrome toggle (R1) and not a palette (W1) — this is
    // the ONLY way it opens (plus an Edit-menu Find item, if one is wired).
    if ((e.ctrlKey || e.metaKey) && (e.key === 'f' || e.key === 'F')) {
      e.preventDefault();
      openFind();
    }
    // BUG-097 (P8.1-T2): Cmd/Ctrl+A inside an editable field (cmd-input,
    // doedit-text, find inputs, settings inputs, …) stays native select-all —
    // only OUTSIDE one of those does it scope to the Results log instead of
    // the whole app chrome (toolbar/panes text nodes had no sane "select
    // all" meaning before this).
    if ((e.ctrlKey || e.metaKey) && (e.key === 'a' || e.key === 'A')) {
      const ae = document.activeElement;
      const isEditable = !!ae && (ae.tagName === 'INPUT' || ae.tagName === 'TEXTAREA' || ae.isContentEditable);
      if (!isEditable) {
        e.preventDefault();
        const cb = document.getElementById('console-body');
        if (cb) window.getSelection().selectAllChildren(cb);
      }
    }
    // ⌘, / ⌘O (P8.1-T2): standard-idiom Settings + Open Data shortcuts.
    // Skipped while the request-dialog is up — that modal must stay modal
    // (mirrors the Escape-swallowing on #request-overlay above).
    if (!_isOverlayOpen('request-overlay')) {
      if ((e.ctrlKey || e.metaKey) && e.key === ',') {
        e.preventDefault();
        if (_isOverlayOpen('settings-overlay')) closeSettings(); else openSettings();
      }
      if ((e.ctrlKey || e.metaKey) && (e.key === 'o' || e.key === 'O')) {
        e.preventDefault();
        onOpenFile();   // self-guards on isBusy()
      }
    }
    // T3 (P8.1, UX-054 안2-lite): ⌥↑/↓ — jump between command groups in the
    // Results log (console.js's .block-fold-head echoes are the group heads).
    // Deliberately its OWN anchor class (.block-nav-anchor), NOT C4's
    // .block-active — that class has a single-owner invariant (console.js's
    // _activeBlockEl tracks the newest ESTIMATION block; navigation stealing
    // it would desync the tracker). Skipped inside editable fields — macOS
    // Option+arrows are text-navigation there.
    if (e.altKey && (e.key === 'ArrowUp' || e.key === 'ArrowDown')) {
      const ae = document.activeElement;
      const inEditable = !!ae && (ae.tagName === 'INPUT' || ae.tagName === 'TEXTAREA' || ae.isContentEditable);
      if (!inEditable) {
        const heads = Array.from(document.querySelectorAll('#console-body .block-fold-head'));
        if (heads.length) {
          e.preventDefault();
          const cur = document.querySelector('#console-body .block-nav-anchor');
          let idx = cur ? heads.indexOf(cur) : heads.length;   // no anchor yet → step lands on the last head
          idx += (e.key === 'ArrowUp' ? -1 : 1);
          idx = Math.max(0, Math.min(heads.length - 1, idx));
          if (cur) cur.classList.remove('block-nav-anchor');
          heads[idx].classList.add('block-nav-anchor');
          heads[idx].scrollIntoView?.({ block: 'center' });
        }
      }
    }
  });

  // Esc: close whichever console overlay is open (A2-05) — one top-level
  // handler driven by the OVERLAYS registry, replacing the previous ad hoc
  // per-overlay Escape handling (grid-only listener here + doedit-text's own
  // Escape branch). Each close() already restores focus to #cmd-input.
  document.addEventListener('keydown', e => {
    if (e.key !== 'Escape') return;
    // B11: the find bar closes on Esc BEFORE any console overlay (design
    // law) — checked first, ahead of both the menu-dropdown check and the
    // OVERLAYS sweep below. closeFind() restores focus to wherever it was
    // before ⌘F opened the bar.
    if (isFindOpen()) { closeFind(); e.preventDefault(); return; }
    // N5.2: Esc disarms a pending click-swap BEFORE the menu-dropdown check
    // and the OVERLAYS sweep — same "most-local transient state first"
    // ordering as the find-bar check just above.
    if (isSwapArmed()) { _disarmSwap(); e.preventDefault(); return; }
    // A12: Esc also closes an open File▾/Data▾ dropdown — menus use their
    // own .open toggle, not the OVERLAYS registry, so they were untouched
    // by the loop below.
    const openMenu = document.querySelector('.menu-dropdown.open');
    if (openMenu) {
      menuDropdowns.forEach(d => d.classList.remove('open'));
      e.preventDefault();
      return;
    }
    if (layout.closeFirstOpenOverlay()) { e.preventDefault(); return; }
  });


}
