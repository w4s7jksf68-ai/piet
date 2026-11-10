// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Native preferences, UI persistence, consent, and appearance.
import { setMemoryBudgetMb } from './console.js';
import { setGridMemoryBudgetMb } from './grid.js';
import { RECENT_KEY } from './recent-files.js';
import { SPLIT_KEY } from './layout.js';

export function createSettingsController({ getClient, layout, flashStatus }) {
  let _settingsCache = null;
  let _uiStatePersistTimer = null;
  let _themeFadeTimer = null;
  const { closeAllOverlays, syncViewTabs, _isOverlayOpen } = layout;
  const UI_STATE_KEYS = [
    SPLIT_KEY, RECENT_KEY, 'piet-col-vars', 'piet-col-history',
    'piet-theme', 'piet-mode', 'piet-theme-follow-system', 'piet-font-size',
    'piet-paste-autorun', 'piet-auto-summarize', 'piet-editor-gutter',
  ];

  async function hydrateUiState() {
    if (!getClient()) return null;
    let s = null;
    try { s = await getClient().getSettings(); } catch { s = null; }
    _settingsCache = s;
    if (!getClient().native) return s;   // mock/browser: nothing to hydrate FROM — stays pure-localStorage
    if (!s || typeof s.ui_state !== 'string' || !s.ui_state) return s;
    let blob = null;
    try { blob = JSON.parse(s.ui_state); } catch { blob = null; }
    if (!blob || typeof blob !== 'object') return s;
    for (const key of UI_STATE_KEYS) {
      if (Object.prototype.hasOwnProperty.call(blob, key) && blob[key] != null) {
        try { localStorage.setItem(key, String(blob[key])); } catch { /* storage denied — non-fatal */ }
      }
    }
    // N2 gap fix (P8.1-W2 test-stint finding): index.html's pre-paint script read
    // piet-mode/piet-theme/piet-theme-follow-system/piet-font-size from
    // localStorage synchronously at parse time — BEFORE this async hydration
    // could land the native ui_state values. On a fresh profile that carries
    // prior ui_state (reinstall/cleared-localStorage), first paint used defaults
    // and stayed wrong all session. Re-apply those pre-paint keys live from the
    // hydrated values (syncThemeButtons() runs later in init(), after this).
    const html = document.documentElement;
    const mode = localStorage.getItem('piet-mode');
    if (mode === 'boring' || mode === 'destijl') html.dataset.mode = mode;
    if (localStorage.getItem('piet-theme-follow-system') === '1') {
      applySystemTheme();
    } else {
      const t = localStorage.getItem('piet-theme');
      if (t === 'dark') html.dataset.theme = 'dark';
      else if (t != null) delete html.dataset.theme;   // explicit light ('' persisted by toggleTheme)
    }
    const fz = localStorage.getItem('piet-font-size');
    if (fz) applyFontSize(fz);
    return s;
  }

  function markUiStateDirty() {
    if (!getClient() || !getClient().native) return;
    clearTimeout(_uiStatePersistTimer);
    _uiStatePersistTimer = setTimeout(_flushUiState, 400);
  }

  function _flushUiState() {
    const blob = {};
    for (const key of UI_STATE_KEYS) {
      const v = localStorage.getItem(key);
      if (v != null) blob[key] = v;
    }
    getClient().setSettings({ ui_state: JSON.stringify(blob) }).catch(() => {
      // Best-effort mirror: localStorage (already written by the caller before
      // markUiStateDirty() was ever invoked) stays this session's source of
      // truth regardless; the next successful flush catches up.
    });
  }

  function maybeShowConsent(s) {
    if (!getClient() || !getClient().native) return;
    if (!s || s.consent_required !== true) return;
    const ov = document.getElementById('consent-overlay');
    if (!ov) return;
    const box = document.getElementById('consent-extcompat');
    // Default ON — the dialog opens with existing do-files still working, and
    // the user opts OUT rather than having to opt in to what already shipped.
    if (box) box.checked = s.extcompat !== false;
    ov.classList.add('open');
    document.getElementById('consent-continue')?.focus();
  }

  async function acceptConsent() {
    const status = document.getElementById('consent-status');
    const btn = document.getElementById('consent-continue');
    const on = !!document.getElementById('consent-extcompat')?.checked;
    if (status) status.textContent = '';
    if (btn) btn.disabled = true;
    try {
      const partial = { extcompat: on, extcompat_consent_at: new Date().toISOString() };
      await getClient().setSettings(partial);
      _settingsCache = { ..._settingsCache, ...partial, consent_required: false };
      document.getElementById('consent-overlay')?.classList.remove('open');
      document.getElementById('cmd-input')?.focus();
    } catch (err) {
      if (status) status.textContent = '⚠ Could not save your choice: '
        + String(err && err.message ? err.message : err);
    } finally {
      if (btn) btn.disabled = false;
    }
  }

  async function openSettings() {
    const ov = document.getElementById('settings-overlay');
    if (!ov) return;
    closeAllOverlays('settings-overlay');
    ov.classList.add('open');
    syncViewTabs();   // Settings is not a view tab (like About) — no tab lights up.
    await populateSettings();
    document.getElementById('set-scratch-path')?.focus();
  }

  function closeSettings() {
    const ov = document.getElementById('settings-overlay');
    if (ov) ov.classList.remove('open');
    document.getElementById('cmd-input')?.focus();
    syncViewTabs();
  }

  async function populateSettings() {
    const status = document.getElementById('settings-status');
    if (status) status.textContent = '';
    let s = null;
    try { s = await getClient().getSettings(); } catch { s = null; }
    _settingsCache = s;   // N1: keep composeDefaultPath() current with whatever's actually persisted
    const sys = (s && s.sys) || { total_ram_mb: 0, cpu_count: 1, recommended_budget_mb: 2048 };

    const sysinfo = document.getElementById('settings-sysinfo');
    if (sysinfo) {
      sysinfo.textContent = `Detected: ${sys.cpu_count} cores · ${sys.total_ram_mb} MB RAM · `
        + `recommended budget ${sys.recommended_budget_mb} MB`
        + (s && s.settings_file ? `  ·  ${s.settings_file}` : '');
    }

    const scratch = document.getElementById('set-scratch-path');
    if (scratch) scratch.value = (s && s.scratch_path) || '';
    const workdir = document.getElementById('set-workdir');
    if (workdir) workdir.value = (s && s.default_working_dir) || '';
    // N1: "Export folder" — default_export_dir (contract_B v2.4.0, additive).
    // Empty = ask every time (native NSSavePanel); see composeDefaultPath().
    const exportDir = document.getElementById('set-export-dir');
    if (exportDir) exportDir.value = (s && s.default_export_dir) || '';

    const mem = document.getElementById('set-mem-budget');
    const memOut = document.getElementById('set-mem-value');
    const memRec = document.getElementById('set-mem-recommended');
    if (mem) {
      if (sys.total_ram_mb && sys.total_ram_mb > Number(mem.max)) mem.max = String(sys.total_ram_mb);
      const cur = (s && s.memory_budget_mb) ? s.memory_budget_mb : sys.recommended_budget_mb;
      mem.value = String(cur);
      if (memOut) memOut.textContent = `${cur} MB`;
      // P7 carry-over: keep console.js's Results-console scrollback cap
      // following the user's real configured (or recommended, pre-first-save)
      // budget instead of console.js's hardcoded 2048 default.
      setMemoryBudgetMb(cur);
      setGridMemoryBudgetMb(cur);  // SPEC II-3: grid cache/prefetch depth too
    }
    if (memRec) {
      // U12: name the real consumers instead of a vague label.
      memRec.textContent = `Handed to the engine as its memory budget (bounds parallel scratch), caps the Results `
        + `console's scrollback and graph gallery, and bounds the automatic parallel worker count (SPEC II-2). `
        + `Recommended: ${sys.recommended_budget_mb} MB.`;
    }

    const cores = document.getElementById('set-cores');
    if (cores) {
      const want = (s && s.compute_cores) || 0;
      cores.replaceChildren();
      const auto = document.createElement('option');
      auto.value = '0'; auto.textContent = 'Auto (all available)';
      cores.appendChild(auto);
      const n = Math.max(1, sys.cpu_count || 1);
      for (let i = 1; i <= n; i++) {
        const o = document.createElement('option');
        o.value = String(i); o.textContent = String(i);
        cores.appendChild(o);
      }
      cores.value = String(want);
    }

    const prov = document.getElementById('set-provenance');
    if (prov) prov.checked = !!(s && s.provenance_hash);

    // WS-B (extcompat): default TRUE, so an absent field (a mock session, or a
    // store written before this build) shows the names as enabled — which is
    // what the engine actually does in that state.
    const ext = document.getElementById('set-extcompat');
    if (ext) ext.checked = !s || s.extcompat !== false;

    // Default TRUE on the same reasoning: an absent field means it is on.
    const followDo = document.getElementById('set-follow-do-folder');
    if (followDo) followDo.checked = !s || s.follow_do_file_folder !== false;

    const fsSel = document.getElementById('set-fontsize');
    if (fsSel) fsSel.value = localStorage.getItem('piet-font-size') || 'medium';
    const pa = document.getElementById('set-paste-autorun');
    if (pa) pa.checked = localStorage.getItem('piet-paste-autorun') === '1';
    const as = document.getElementById('set-auto-summarize');
    if (as) as.checked = localStorage.getItem('piet-auto-summarize') === '1';
    const fst = document.getElementById('set-follow-system-theme');
    if (fst) fst.checked = localStorage.getItem('piet-theme-follow-system') === '1';
  }

  async function applySettings() {
    const status = document.getElementById('settings-status');
    const partial = {
      scratch_path: document.getElementById('set-scratch-path')?.value ?? '',
      default_working_dir: document.getElementById('set-workdir')?.value ?? '',
      // N1: additive field, passes straight through settings.set (api.js) —
      // empty string means "ask every time" (composeDefaultPath() below).
      default_export_dir: document.getElementById('set-export-dir')?.value ?? '',
      memory_budget_mb: Number(document.getElementById('set-mem-budget')?.value ?? 0) || 0,
      compute_cores: Number(document.getElementById('set-cores')?.value ?? 0) || 0,
      provenance_hash: !!document.getElementById('set-provenance')?.checked,
      // WS-B (extcompat): the durable half of the choice. settings.set republishes
      // it to the dispatch gate's cache, so it takes effect on the next command
      // rather than at the next launch.
      extcompat: !!document.getElementById('set-extcompat')?.checked,
      follow_do_file_folder: !!document.getElementById('set-follow-do-folder')?.checked,
    };
    try {
      await getClient().setSettings(partial);
      // N1: keep the save-time cache current so a save right after Apply (no
      // reopen of Settings in between) sees the new export folder immediately.
      _settingsCache = { ..._settingsCache, ...partial };
      if (status) status.textContent = 'Saved. Compute cores apply to the next command; '
        + 'scratch directory & working directory apply on restart.';
    } catch (err) {
      if (status) status.textContent = '⚠ Could not save settings: '
        + String(err && err.message ? err.message : err);
    }
  }

  function composeDefaultPath(name) {
    const dir = _settingsCache && _settingsCache.default_export_dir;
    return dir ? `${dir.replace(/[/\\]+$/, '')}/${name}` : undefined;
  }

  function applyFontSize(size) {
    const root = document.documentElement;
    if (size === 'small' || size === 'large') {
      root.dataset.fontsize = size;
      localStorage.setItem('piet-font-size', size);
    } else {
      delete root.dataset.fontsize;
      localStorage.setItem('piet-font-size', 'medium');
    }
    markUiStateDirty();   // N2
  }

  function setPasteAutorun(on) {
    localStorage.setItem('piet-paste-autorun', on ? '1' : '0');
    markUiStateDirty();   // N2
    const a = document.getElementById('paste-autorun-toggle');
    const b = document.getElementById('set-paste-autorun');
    if (a) a.checked = on;
    if (b) b.checked = on;
  }

  function setAutoSummarize(on) {
    localStorage.setItem('piet-auto-summarize', on ? '1' : '0');
    markUiStateDirty();   // N2
    const el = document.getElementById('set-auto-summarize');
    if (el) el.checked = on;
  }

  function applySystemTheme() {
    beginThemeFade();   // A7: arm the transition before the recolor below
    const html = document.documentElement;
    const prefersDark = !!(window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches);
    if (prefersDark) html.dataset.theme = 'dark';
    else delete html.dataset.theme;
    syncThemeButtons();   // BUG-114: both labels, not just #theme-toggle
  }

  function setFollowSystemTheme(on) {
    localStorage.setItem('piet-theme-follow-system', on ? '1' : '0');
    markUiStateDirty();   // N2
    const el = document.getElementById('set-follow-system-theme');
    if (el) el.checked = on;
    if (on) applySystemTheme();
  }

  function beginThemeFade() {
    const html = document.documentElement;
    html.classList.add('theme-fade');
    clearTimeout(_themeFadeTimer);
    _themeFadeTimer = setTimeout(() => html.classList.remove('theme-fade'), 250);
  }

  function toggleMode() {
    beginThemeFade();   // A7: arm the transition before the recolor below
    const html = document.documentElement;
    const next = html.dataset.mode === 'destijl' ? 'boring' : 'destijl';
    html.dataset.mode = next;
    localStorage.setItem('piet-mode', next);
    markUiStateDirty();   // N2
    syncThemeButtons();   // BUG-114: mode also flips theme-toggle's label (Pip Boy/De Stijl vs Dark/Light)
  }

  function toggleTheme() {
    beginThemeFade();   // A7: arm the transition before the recolor below
    // D5/T2: a manual click always wins — if "Follow system appearance" was on,
    // switch it off (+ a status flash so the change isn't silent) BEFORE
    // flipping, so the click's result isn't immediately clobbered by the next
    // matchMedia 'change' event.
    if (localStorage.getItem('piet-theme-follow-system') === '1') {
      setFollowSystemTheme(false);
      flashStatus('Follow system appearance off — manual');
    }
    const html = document.documentElement;
    const isDark = html.dataset.theme === 'dark';
    if (isDark) {
      delete html.dataset.theme;
      localStorage.setItem('piet-theme', '');
    } else {
      html.dataset.theme = 'dark';
      localStorage.setItem('piet-theme', 'dark');
    }
    markUiStateDirty();   // N2
    syncThemeButtons();   // BUG-114: both labels, not just #theme-toggle
  }

  function syncThemeButtons() {
    const html = document.documentElement;
    const destijl = html.dataset.mode === 'destijl';
    const dark = html.dataset.theme === 'dark';
    const modeBtn = document.getElementById('mode-toggle');
    if (modeBtn) modeBtn.textContent = destijl ? 'go boring' : 'add style?';
    const themeBtn = document.getElementById('theme-toggle');
    if (themeBtn) themeBtn.textContent = destijl ? (dark ? 'Pip Boy' : 'De Stijl') : (dark ? 'Dark' : 'Light');
  }

  function cachedSettings() { return _settingsCache; }
  function invalidateSettingsCache() { _settingsCache = null; }

  function bindAppearance() {
    // Mode toggle (De Stijl ↔ Boring)
    const modeToggle = document.getElementById('mode-toggle');
    if (modeToggle) modeToggle.addEventListener('click', toggleMode);

    // Theme toggle (light ↔ dark)
    const themeToggle = document.getElementById('theme-toggle');
    if (themeToggle) themeToggle.addEventListener('click', toggleTheme);

    // BUG-114: one early resync so both labels are correct from the first
    // paint onward — index.html's boot script used to do this manually (now
    // relocated here; see its own comment) and init() runs from that SAME
    // boot script, so the resync guarantee is preserved.
    syncThemeButtons();

    // D5: live "Follow system appearance" — re-syncs <html data-theme> whenever
    // the OS setting changes, but only while the Settings toggle is on
    // (checked fresh from localStorage each firing, not captured at boot).
    // Registered once; index.html's pre-paint script already applied the
    // correct initial state before this module ran.
    if (window.matchMedia) {
      const systemThemeQuery = window.matchMedia('(prefers-color-scheme: dark)');
      const onSystemThemeChange = () => {
        if (localStorage.getItem('piet-theme-follow-system') === '1') applySystemTheme();
      };
      if (systemThemeQuery.addEventListener) systemThemeQuery.addEventListener('change', onSystemThemeChange);
      else if (systemThemeQuery.addListener) systemThemeQuery.addListener(onSystemThemeChange); // older WebKit
    }
  }

  function bindPastePreference() {
    // Paste auto-run preference (opt-in follow-up to design decision 4).
    // Checkbox in the do-editor header reflects + writes localStorage
    // 'piet-paste-autorun' ('1'/'0'; absent = OFF) — same persistence idiom as
    // piet-mode/piet-theme. The behavior switch lives in handleMultilineBlock().
    const pasteToggle = document.getElementById('paste-autorun-toggle');
    if (pasteToggle) {
      pasteToggle.checked = localStorage.getItem('piet-paste-autorun') === '1';
      // 2.2.0 item D: mirror to the Settings panel checkbox via setPasteAutorun.
      pasteToggle.addEventListener('change', () => setPasteAutorun(pasteToggle.checked));
    }
  }

  function bindControls() {
    // 2.2.0 item D: Settings panel entry.
    const settingsLink = document.querySelector('[data-action="settings"]');
    if (settingsLink) settingsLink.addEventListener('click', e => {
      e.preventDefault();
      if (_isOverlayOpen('settings-overlay')) closeSettings(); else openSettings();
    });
    // BUG-095: visible ✕ in each panel header (reintroduced deliberately —
    // UX-047/III-6 removed per-overlay closes on the theory that view tabs
    // generalize them, but Settings/About light no view tab, so the theory
    // never covered these two; User dogfooding 07-12 confirmed the trap).
    document.getElementById('settings-close')?.addEventListener('click', closeSettings);
    const settingsApply = document.getElementById('settings-apply');
    if (settingsApply) settingsApply.addEventListener('click', applySettings);
    // WS-B (extcompat): the first-run consent dialog's only control. Deliberately
    // NOT wired into the OVERLAYS registry / Escape sweep — see index.html.
    document.getElementById('consent-continue')?.addEventListener('click', acceptConsent);
    const memSlider = document.getElementById('set-mem-budget');
    const memOut    = document.getElementById('set-mem-value');
    if (memSlider && memOut) memSlider.addEventListener('input', () => { memOut.textContent = `${memSlider.value} MB`; });
    const fsSel = document.getElementById('set-fontsize');
    if (fsSel) fsSel.addEventListener('change', () => applyFontSize(fsSel.value));
    const pasteMirror = document.getElementById('set-paste-autorun');
    if (pasteMirror) pasteMirror.addEventListener('change', () => setPasteAutorun(pasteMirror.checked));
    // B14(b): "Auto-summarize on load" — same localStorage-only display-setting
    // idiom as paste-autorun above (see setAutoSummarize()'s doc comment).
    const autoSummarizeToggle = document.getElementById('set-auto-summarize');
    if (autoSummarizeToggle) autoSummarizeToggle.addEventListener('change', () => setAutoSummarize(autoSummarizeToggle.checked));
    // D5: "Follow system appearance" — same localStorage-only display-setting
    // idiom (see setFollowSystemTheme()'s doc comment above).
    const followSystemToggle = document.getElementById('set-follow-system-theme');
    if (followSystemToggle) followSystemToggle.addEventListener('change', () => setFollowSystemTheme(followSystemToggle.checked));
  }

  function bindPersistenceObservation() {
    // N2: panes.js writes --col-vars/--col-history directly onto #app's inline
    // style (RAF-coalesced during a drag, one more write on pointerup) — it is
    // NOT this lane's file, so rather than touch it, observe the attribute it
    // already mutates and mirror into ui_state from here. Debounced by
    // markUiStateDirty() itself, so the RAF-coalesced drag-time writes collapse
    // into one flush same as everywhere else.
    const appEl = document.getElementById('app');
    if (appEl) {
      new MutationObserver(markUiStateDirty)
        .observe(appEl, { attributes: true, attributeFilter: ['style'] });
    }
  }

  return { hydrateUiState, markUiStateDirty, _flushUiState, maybeShowConsent, acceptConsent, openSettings, closeSettings, populateSettings, applySettings, composeDefaultPath, applyFontSize, setPasteAutorun, setAutoSummarize, applySystemTheme, setFollowSystemTheme, beginThemeFade, toggleMode, toggleTheme, syncThemeButtons, cachedSettings, invalidateSettingsCache, bindAppearance, bindPastePreference, bindControls, bindPersistenceObservation };
}
