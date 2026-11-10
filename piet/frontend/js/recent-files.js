// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Recent-file storage and menu. App callbacks own navigation and native mirroring;
// the list is read from localStorage on demand, including after UI-state hydration.

export const RECENT_KEY = 'piet-recent-files';
const RECENT_MAX = 8;

/** No DOM/storage work at construction; call render after app hydration.
 *  onOpen(entry) chooses the existing data/do route (and checks busy state).
 *  onChange() mirrors persistence without giving this module app state. */
export function createRecentFiles({ onOpen, onChange }) {
  function loadRecentFiles() {
    try {
      const list = JSON.parse(localStorage.getItem(RECENT_KEY) || '[]');
      return Array.isArray(list) ? list : [];
    } catch { return []; }
  }

  /** Record `path` as most-recently-opened (moves to front if already present,
   *  dedupe by path), cap at RECENT_MAX, persist, and refresh the File▾ menu. */
  function trackRecentFile(path, kind) {
    if (!path || typeof path !== 'string') return;
    let list = loadRecentFiles().filter(f => f.path !== path);
    list.unshift({ path, kind, ts: Date.now() });
    if (list.length > RECENT_MAX) list.length = RECENT_MAX;
    try { localStorage.setItem(RECENT_KEY, JSON.stringify(list)); } catch { /* storage full/disabled — non-fatal */ }
    onChange();   // App mirrors the changed key into native ui_state
    renderRecentFiles();
  }

  /** Rebuild the File▾ "Recent" group's DOM from localStorage. Safe to call any
   *  time (boot, and after every trackRecentFile()). Discoverability fix
   *  (III-6-compatible): the group now stays VISIBLE even while empty, showing
   *  one non-clickable "(no recent files)" row instead of hiding the whole
   *  group — a hidden-until-populated group gave no hint the feature existed
   *  at all; the label now honestly explains its own empty state. */
  function renderRecentFiles() {
    const group = document.getElementById('file-menu-recent-group');
    const list  = document.getElementById('file-menu-recent');
    if (!group || !list) return;
    const entries = loadRecentFiles();
    list.innerHTML = '';
    group.hidden = false;
    if (!entries.length) {
      const empty = document.createElement('div');
      empty.className = 'menu-recent-item menu-recent-empty';
      empty.textContent = '(no recent files)';
      list.appendChild(empty);
      return;
    }
    for (const entry of entries) {
      const a = document.createElement('a');
      a.href = '#';
      a.className = 'menu-recent-item';
      a.textContent = entry.path.split(/[/\\]/).pop() || entry.path;
      a.title = entry.path;
      a.addEventListener('click', e => {
        e.preventDefault();
        onOpen(entry);
      });
      list.appendChild(a);
    }
  }

  return { track: trackRecentFile, render: renderRecentFiles };
}
