// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Application composition and DOM boot. Feature state lives in its controller.
import { HTTPClient } from './api.js';
import { initConsole, appendBlock, setMemoryBudgetMb } from './console.js';
import { initPanels, loadHistory } from './panels.js';
import { VirtualizedGrid, setGridMemoryBudgetMb } from './grid.js';
import { initPanes } from './panes.js';
import { initFind } from './find.js';
import { initGutter } from './gutter.js';
import { createRecentFiles } from './recent-files.js';
import { createLayoutController } from './layout.js';
import { createEditorController } from './editor.js';
import { createGraphicsController } from './graphics-gallery.js';
import { createSettingsController } from './settings.js';
import { createFileExportController } from './file-export.js';
import { createAboutController } from './about-panel.js';
import { createRequestDialogController } from './request-dialog.js';
import { createRunProgressController } from './run-progress.js';
import { createCommandInputController } from './command-input.js';
import { createExecutionController } from './execution.js';
import { flashStatus } from './status-flash.js';
import { bindAppNavigation } from './app-navigation.js';

let client = null;
let grid = null;
const getClient = () => client;
const getGrid = () => grid;

// Controllers retain their own state. Deferred callbacks close feature interactions
// here without cyclic module imports or exposing one mutable application context.
const layout = createLayoutController({
  getGrid, isBusy: () => execution.isBusy(),
  markUiStateDirty: () => settings.markUiStateDirty(),
  closeDoEditor: () => editor.closeDoEditor(), closeGraphics: () => graphics.closeGraphics(),
  closeAbout: () => about.closeAbout(), closeSettings: () => settings.closeSettings(),
  openGraphics: () => graphics.openGraphics(),
});
const settings = createSettingsController({ getClient, layout, flashStatus });
const fileExport = createFileExportController({ getClient, composeDefaultPath: settings.composeDefaultPath, flashStatus });
const graphics = createGraphicsController({ layout, svgToPng: fileExport.svgToPng });
const editor = createEditorController({
  layout, isBusy: () => execution.isBusy(), setBusy: busy => execution.setBusy(busy),
  runDoSourceCore: (...args) => execution.runDoSourceCore(...args), flashStatus,
});
const about = createAboutController({ getClient, layout });
const requestDialog = createRequestDialogController({ getClient, invalidateSettingsCache: settings.invalidateSettingsCache });
const progress = createRunProgressController({
  getClient, openRequestDialog: requestDialog.openRequestDialog,
  routeDroppedFile: path => execution.routeDroppedFile(path),
});
const execution = createExecutionController({
  getClient, getGrid, editor, layout, graphics, progress, flashStatus,
  pushHistory: (...args) => input.pushHistory(...args), resetHistoryIndex: () => input.resetHistoryIndex(),
  trackRecentFile: (...args) => recent.track(...args),
});
const input = createCommandInputController({
  runCommand: execution.runCommand, runPastedBlock: execution.runPastedBlock,
  openDoEditor: editor.openDoEditor, flashStatus,
});
const recent = createRecentFiles({ onOpen: execution.openRecentFile, onChange: settings.markUiStateDirty });
const { render: renderRecentFiles } = recent;
const { restoreSplit, syncViewTabs } = layout;
const { hydrateUiState, maybeShowConsent, cachedSettings } = settings;
const { initPietEventsBridge } = progress;
const { checkKernel, isBusy, applyViewMutation, seedMockDataset } = execution;

export async function init() {
  // Transport: the native bridge when the shell installed it, mock fixtures
  // otherwise (a plain browser tab / the DOM test harness). The 1.x
  // ?kernel=/PIET_KERNEL_URL fetch leg is gone with the Python backend
  // (I-8 cutover sweep, 2026-07-09).
  const hasNativeBridge = typeof window !== 'undefined' && !!window.pietNative;
  const useMock = !hasNativeBridge;

  client = new HTTPClient(useMock);
  initPietEventsBridge();

  // N2 (P8.1-W2): pull the native ui_state blob into localStorage BEFORE any
  // module below (initPanes()/initGutter() a few lines down, restoreSplit()
  // later in this function) reads its own localStorage key — see
  // hydrateUiState()'s doc comment for the write-through/fallback contract.
  // No-op (instant) in mock/browser sessions (no native bridge to hydrate
  // FROM) so this await never meaningfully delays boot there.
  await hydrateUiState();

  // Wire up DOM
  const varListEl  = document.getElementById('var-list-wrap');
  const propsEl    = document.getElementById('properties');
  const histListEl = document.getElementById('hist-list');
  const consoleEl  = document.getElementById('console-body');
  const emptyEl    = document.getElementById('console-empty');
  const jumpPill   = document.getElementById('jump-pill');
  const gridEl     = document.getElementById('browser-overlay');

  initPanes();
  initPanels(varListEl, propsEl, histListEl);
  // T4 (P8.1): do-file editor line-number gutter (gutter.js — self-injecting;
  // honors the 'piet-editor-gutter' localStorage toggle, absent = ON).
  initGutter();
  initConsole(consoleEl, emptyEl, jumpPill);
  // B11/B12: ⌘F find bar — Results find-only, do-file editor find+replace
  // (piet/frontend/js/find.js). Keyboard wiring (Cmd/Ctrl+F, Escape) below.
  initFind();

  if (gridEl) {
    grid = new VirtualizedGrid(
      gridEl,
      (offset, limit, vars) => client.browse(offset, limit, vars),
      // Cell-commit callback (edit mode): write value at (row, varname) to the kernel.
      async (varname, row, value) => {
        // A1-02: don't race a concurrent command/mutation already in flight —
        // decline honestly; grid.js treats a {type:'error'} return as "revert
        // this edit" (its existing error path), so no grid.js change needed.
        if (isBusy()) {
          return { type: 'error', code: 'busy', message: 'Kernel is busy — try again in a moment.' };
        }
        const res = await client.cell(varname, row, value);
        if (res && res.type !== 'error' && res.varlist_after) {
          applyViewMutation(res);
        }
        return res;
      },
    );
    gridEl.hidden = true;
  }

  input.bindControls();

  // S6 (OVERHEAD-ZERO, drag-and-drop file open): preventDefault() on BOTH
  // dragover AND drop — WKWebView's default action for an un-prevented drop
  // is to navigate the whole window to the dropped file (dragover alone is
  // NOT enough to stop it; see shell.cpp's installFileDropHandler() doc
  // comment for the matching native half). Deliberately does nothing else:
  // the real filesystem path never reaches JS (WKWebView only ever exposes
  // a sandboxed/opaque File reference to page script) — it arrives via the
  // 'file_dropped' pietEvents push handled in initPietEventsBridge() below,
  // pushed by the native drag-destination override in shell.cpp.
  document.addEventListener('dragover', (e) => e.preventDefault());
  document.addEventListener('drop', (e) => e.preventDefault());

  // Jump-to-latest pill
  if (jumpPill) {
    jumpPill.addEventListener('click', () => {
      // #console-body never overflows (min-height:100%) — the real scroll
      // container is the parent #console-wrap (console.js's _wrapEl convention).
      const wrapEl = consoleEl.parentElement;
      wrapEl.scrollTop = wrapEl.scrollHeight;
    });
    jumpPill.hidden = true;
  }

  fileExport.bindControls();

  settings.bindAppearance();

  execution.bindFileControls();

  // B3 (P8-S6): File ▾ > Recent — populate from localStorage at boot (each
  // open thereafter re-renders itself via trackRecentFile()).
  renderRecentFiles();

  editor.bindControls();

  settings.bindPastePreference();

  progress.bindCancelControl();

  requestDialog.bindControls();

  graphics.bindEntryControl();

  about.bindControls();

  settings.bindControls();

  layout.bindControls();

  settings.bindPersistenceObservation();

  graphics.bindGalleryNavigation();

  editor.bindNavigationEvents();

  layout.bindNavigation();

  bindAppNavigation({ layout, settings, execution, onClear: fileExport.onClear });

  execution.bindVariableEditing();

  // Probe kernel
  await checkKernel();

  // Load history (mock or live)
  try {
    const h = await client.history();
    loadHistory(h.history ?? []);
  } catch { /* ignore */ }

  // P7 carry-over: seed console.js's Results-console scrollback cap from the
  // user's real configured/recommended budget at boot too — not only once
  // the Settings panel happens to be opened later in the session (which also
  // calls setMemoryBudgetMb(), see settings.js's populateSettings()). Fire-and-forget: a failure here just leaves console.js on its
  // DEFAULT_MEMORY_BUDGET_MB fallback.
  try {
    // N2: hydrateUiState() (above) already fetched + cached this — reuse it
    // instead of a second round trip; falls back to a fresh fetch if that
    // earlier call failed, so this block's own behavior is unchanged.
    const s = cachedSettings() ?? await client.getSettings();
    const cur = (s && s.memory_budget_mb) || (s && s.sys && s.sys.recommended_budget_mb);
    if (cur) { setMemoryBudgetMb(cur); setGridMemoryBudgetMb(cur); }  // SPEC II-3
  } catch { /* ignore */ }

  // In mock mode: show persistent banner + pre-populate varlist so the panel isn't empty
  if (useMock) {
    document.getElementById('mock-banner')?.removeAttribute('hidden');
    appendBlock({ type: 'echo', text: 'Open a dataset with File ▾ > Open Data…, or type a command below.' });
    try {
      const vl = await client.varlist();
      if (vl?.varlist?.length) {
        seedMockDataset(vl);
      }
    } catch { /* ignore */ }
  }

  // 2.2.0 item C: restore a persisted split layout (if any) BEFORE the boot
  // tab reconcile, so the restored columns light their tabs correctly.
  restoreSplit();

  // UX-047: establish the boot tab-active state deterministically (Results —
  // no overlay is open yet). index.html hardcodes .active on the Results tab
  // for zero-FOUC, this just reconciles it via the same code path every
  // later transition uses.
  syncViewTabs();

  // WS-B (extcompat): first-run consent. LAST in init() on purpose — the app
  // behind the dialog is fully wired by the time it paints, so accepting drops
  // the user straight into a live session with nothing left to initialise.
  // Everything above already ran, so this can never delay boot either.
  maybeShowConsent(cachedSettings());
}
