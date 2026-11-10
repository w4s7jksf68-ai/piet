// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Split geometry, docking, overlay ownership, and persistence.
export const SPLIT_KEY = 'piet-split';
export const DOCKABLE = ['doedit-overlay', 'graphics-overlay'];

export function createLayoutController({ getGrid, isBusy, markUiStateDirty, closeDoEditor, closeGraphics, closeAbout, closeSettings, openGraphics }) {
  let splitOn = false;
  let splitRatio = 50;
  const SPLIT_RATIO_MIN = 20, SPLIT_RATIO_MAX = 80;
  let splitW1 = 1, splitW2 = 1;
  const SPLIT_W_MIN = 0.25, SPLIT_W_MAX = 4;
  let _splitVariant = 'columns';
  let _swapArmedId = null;
  const OVERLAYS = [
    {
      id: 'browser-overlay',
      isOpen: () => !!getGrid() && !getGrid()._container.hidden,
      close:  () => { if (getGrid() && !getGrid()._container.hidden) getGrid().hide(); },
    },
    { id: 'doedit-overlay',   isOpen: () => _isOverlayOpen('doedit-overlay'),   close: closeDoEditor },
    { id: 'graphics-overlay', isOpen: () => _isOverlayOpen('graphics-overlay'), close: closeGraphics },
    { id: 'about-overlay',    isOpen: () => _isOverlayOpen('about-overlay'),    close: closeAbout },
    // 2.2.0 item D: Settings joins the registry so Escape + closeAllOverlays +
    // the tab-sync sweep treat it uniformly (like About, it lights no view tab).
    { id: 'settings-overlay', isOpen: () => _isOverlayOpen('settings-overlay'), close: closeSettings },
  ];

  function dockSurface(overlayId, dock) {
    const ov = document.getElementById(overlayId);
    if (!ov) return;
    ov.classList.toggle('docked', dock);
    ov.classList.toggle('open', dock);
    applySplitClass();
    persistSplit();
    syncViewTabs();
  }

  function applySplitClass() {
    document.getElementById('pane-console')?.classList.toggle('split', splitOn);
  }

  function applySplitRatio(ratio) {
    splitRatio = Math.max(SPLIT_RATIO_MIN, Math.min(SPLIT_RATIO_MAX, Number(ratio) || 50));
    document.getElementById('pane-console')?.style.setProperty('--split-ratio', `${splitRatio}%`);
  }

  function applySplitWeights(w1, w2) {
    splitW1 = Math.max(SPLIT_W_MIN, Math.min(SPLIT_W_MAX, Number(w1) || 1));
    splitW2 = Math.max(SPLIT_W_MIN, Math.min(SPLIT_W_MAX, Number(w2) || 1));
    const pc = document.getElementById('pane-console');
    if (pc) {
      pc.style.setProperty('--split-w1', splitW1);
      pc.style.setProperty('--split-w2', splitW2);
    }
  }

  function syncSplitLayout() {
    const doeditDocked   = document.getElementById('doedit-overlay')?.classList.contains('docked');
    const graphicsDocked = document.getElementById('graphics-overlay')?.classList.contains('docked');
    const anyDocked = doeditDocked || graphicsDocked;
    const bothDocked = doeditDocked && graphicsDocked;
    const placeholder = document.getElementById('split-placeholder');
    const resizer = document.getElementById('split-resizer');
    const resizer2 = document.getElementById('split-resizer-2');
    if (placeholder) placeholder.hidden = !(splitOn && !anyDocked);
    if (resizer) resizer.hidden = !splitOn;
    // N5: the columns↔stack weight divider only means anything in the
    // 'columns' variant (two SIDE-BY-SIDE docked columns) — stacked panes have
    // no horizontal boundary to drag.
    if (resizer2) resizer2.hidden = !(splitOn && bothDocked && _splitVariant === 'columns');
    // N5.1: the layout-variant toggle.
    // B21-3 (User dogfood 07-16): it used to stay [hidden] until BOTH dockable
    // surfaces were already docked — the User couldn't find it because it only
    // ever appears once you've stumbled into the exact state that makes it
    // meaningful. Now it's visible for the whole splitOn lifetime; short of
    // both-docked it renders DISABLED instead of absent, with a title that
    // tells the User the one thing to do to unlock it — discoverable rather
    // than a control that only exists once you already know the trick.
    const layoutToggle = document.getElementById('layout-variant-toggle');
    if (layoutToggle) {
      layoutToggle.hidden = !splitOn;
      layoutToggle.disabled = !bothDocked;
      layoutToggle.title = bothDocked
        ? 'Stack the two docked panes vertically ↔ side-by-side columns'
        : 'Dock both Do-file Editor and Graphics (their view tabs) to stack them';
      // Label names the DESTINATION action (what clicking does), same
      // glyph+word shape as #split-toggle's own "⊟ Split" — ⬓/⬒ read as
      // "square split one way" / "the other way", matching the stack/columns
      // pairing without leaning on the ⊞/⊟ glyphs #split-toggle already owns.
      layoutToggle.textContent = _splitVariant === 'stack' ? '⬒ Columns' : '⬓ Stack';
    }
    // N5.2: swap-arm is scoped to "both docked" too — disarm if that stops
    // being true (a pane got undocked mid-arm) so a stale armed cue can't
    // linger on a bar that no longer has a swap partner.
    if (!(splitOn && bothDocked) && _swapArmedId) _disarmSwap();
  }

  function setSplitVariant(variant) {
    _splitVariant = (variant === 'stack') ? 'stack' : 'columns';
    document.getElementById('pane-console')?.classList.toggle('split-stack', _splitVariant === 'stack');
    const btn = document.getElementById('layout-variant-toggle');
    if (btn) btn.setAttribute('aria-pressed', _splitVariant === 'stack' ? 'true' : 'false');
    syncSplitLayout();   // resizer-2/layout-toggle visibility depend on variant too
  }

  function _bothDocked() {
    return DOCKABLE.every(id => document.getElementById(id)?.classList.contains('docked'));
  }

  function swapDockedOrder() {
    const wrap     = document.getElementById('docked-stack');
    const doedit   = document.getElementById('doedit-overlay');
    const graphics = document.getElementById('graphics-overlay');
    const resizer2 = document.getElementById('split-resizer-2');
    if (!wrap || !doedit || !graphics) return;
    // Instance access (doedit.DOCUMENT_POSITION_FOLLOWING), not the bare global
    // `Node` — Web IDL mirrors interface constants onto every instance too, and
    // this way works with no dependency on `Node` being exposed as a bare
    // global (it isn't in every headless/jsdom harness, only as window.Node —
    // real browsers/WKWebView expose both, so this is strictly more portable).
    const doeditFirst = !!(doedit.compareDocumentPosition(graphics) & doedit.DOCUMENT_POSITION_FOLLOWING);
    if (doeditFirst) {
      wrap.insertBefore(graphics, doedit);
      if (resizer2) wrap.insertBefore(resizer2, doedit);
    } else {
      wrap.insertBefore(doedit, graphics);
      if (resizer2) wrap.insertBefore(resizer2, graphics);
    }
  }

  function applyDockedOrder(order) {
    if (!Array.isArray(order) || order.length !== 2) return;
    const [firstId, secondId] = order;
    const first  = document.getElementById(firstId);
    const second = document.getElementById(secondId);
    if (!first || !second) return;
    const currentlyFirst = !!(first.compareDocumentPosition(second) & first.DOCUMENT_POSITION_FOLLOWING);   // instance access — see swapDockedOrder()'s note
    if (!currentlyFirst) swapDockedOrder();
  }

  function _onDockedTitleClick(overlayId, e) {
    e.stopPropagation();   // this IS the gesture — the document click-elsewhere
                            // listener (init()) must not ALSO see it and re-disarm.
    if (!splitOn || !_bothDocked()) return;
    if (_swapArmedId === null) {
      _armSwap(overlayId);
    } else if (_swapArmedId === overlayId) {
      _disarmSwap();
    } else {
      swapDockedOrder();
      persistSplit();
      _disarmSwap();
    }
  }

  function _armSwap(overlayId) {
    _swapArmedId = overlayId;
    document.getElementById(overlayId)?.querySelector('.doedit-bar')?.classList.add('swap-armed');
  }

  function _disarmSwap() {
    if (_swapArmedId) {
      document.getElementById(_swapArmedId)?.querySelector('.doedit-bar')?.classList.remove('swap-armed');
    }
    _swapArmedId = null;
  }

  function initSplitResizer() {
    const handle = document.getElementById('split-resizer');
    const paneConsole = document.getElementById('pane-console');
    if (!handle || !paneConsole) return;
    handle.addEventListener('pointerdown', e => {
      e.preventDefault();
      handle.setPointerCapture(e.pointerId);
      handle.classList.add('dragging');
      const startX = e.clientX;
      const startRatio = splitRatio;
      const width = paneConsole.clientWidth || 1;
      let raf = null;
      let pending = null;

      function onMove(ev) {
        const deltaPct = ((ev.clientX - startX) / width) * 100;
        pending = Math.max(SPLIT_RATIO_MIN, Math.min(SPLIT_RATIO_MAX, startRatio + deltaPct));
        if (raf) return;
        raf = requestAnimationFrame(() => { raf = null; applySplitRatio(pending); });
      }

      function onUp() {
        if (raf) { cancelAnimationFrame(raf); raf = null; }
        if (pending != null) applySplitRatio(pending);
        persistSplit();
        handle.classList.remove('dragging');
        handle.removeEventListener('pointermove', onMove);
        handle.removeEventListener('pointerup', onUp);
      }

      handle.addEventListener('pointermove', onMove);
      handle.addEventListener('pointerup', onUp);
    });
  }

  function initSplitResizer2() {
    const handle = document.getElementById('split-resizer-2');
    const col1 = document.getElementById('doedit-overlay');
    const col2 = document.getElementById('graphics-overlay');
    if (!handle || !col1 || !col2) return;
    handle.addEventListener('pointerdown', e => {
      e.preventDefault();
      handle.setPointerCapture(e.pointerId);
      handle.classList.add('dragging');
      const startX = e.clientX;
      const startW1 = splitW1;
      const startW2 = splitW2;
      const pxTotal = (col1.getBoundingClientRect().width || 1) + (col2.getBoundingClientRect().width || 1);
      const weightTotal = startW1 + startW2 || 1;
      const pxPerWeight = pxTotal / weightTotal || 1;
      let raf = null;
      let pending = null;

      function onMove(ev) {
        const deltaWeight = (ev.clientX - startX) / pxPerWeight;
        const w1 = Math.max(SPLIT_W_MIN, Math.min(SPLIT_W_MAX, startW1 + deltaWeight));
        const w2 = Math.max(SPLIT_W_MIN, Math.min(SPLIT_W_MAX, startW2 - deltaWeight));
        pending = [w1, w2];
        if (raf) return;
        raf = requestAnimationFrame(() => { raf = null; applySplitWeights(pending[0], pending[1]); });
      }

      function onUp() {
        if (raf) { cancelAnimationFrame(raf); raf = null; }
        if (pending != null) applySplitWeights(pending[0], pending[1]);
        persistSplit();
        handle.classList.remove('dragging');
        handle.removeEventListener('pointermove', onMove);
        handle.removeEventListener('pointerup', onUp);
      }

      handle.addEventListener('pointermove', onMove);
      handle.addEventListener('pointerup', onUp);
    });
  }

  function setSplit(on) {
    splitOn = on;
    document.getElementById('split-toggle')?.setAttribute('aria-pressed', on ? 'true' : 'false');
    const resetBtn = document.getElementById('reset-layout');
    if (resetBtn) resetBtn.hidden = !on;
    if (on) {
      for (const id of DOCKABLE) {
        if (_isOverlayOpen(id)) document.getElementById(id)?.classList.add('docked');
      }
    } else {
      for (const id of DOCKABLE) {
        document.getElementById(id)?.classList.remove('docked', 'open');
      }
    }
    applySplitClass();
    syncViewTabs();
  }

  function persistSplit() {
    try {
      if (!splitOn) { localStorage.removeItem(SPLIT_KEY); markUiStateDirty(); return; }
      const docked = DOCKABLE.filter(id => document.getElementById(id)?.classList.contains('docked'));
      // N5.3: current DOM order of the two dockable ids — compareDocumentPosition
      // is the source of truth (swapDockedOrder() actually MOVES the nodes;
      // this just reads back whatever order resulted), so a page reload
      // restores whichever pane the user last swapped to the front.
      const order = [...DOCKABLE].sort((a, b) => {
        const ea = document.getElementById(a), eb = document.getElementById(b);
        if (!ea || !eb) return 0;
        return (ea.compareDocumentPosition(eb) & ea.DOCUMENT_POSITION_FOLLOWING) ? -1 : 1;   // instance access — see swapDockedOrder()'s note
      });
      localStorage.setItem(SPLIT_KEY, JSON.stringify({
        on: true, docked, ratio: splitRatio, ratio2: { w1: splitW1, w2: splitW2 },
        layout: _splitVariant, order,
      }));
      markUiStateDirty();
    } catch { /* storage denied/quota — non-fatal, layout just won't persist */ }
  }

  function restoreSplit() {
    let saved = null;
    try { saved = JSON.parse(localStorage.getItem(SPLIT_KEY) || 'null'); } catch { saved = null; }
    if (!saved || !saved.on) return;
    // B4: apply the saved ratio BEFORE the layout becomes visible (setSplit
    // below), so there's no flash of the 50/50 default.
    if (Number.isFinite(saved.ratio)) applySplitRatio(saved.ratio);
    // BUG-113: same "apply before visible" treatment for the second ratio.
    if (saved.ratio2 && Number.isFinite(saved.ratio2.w1) && Number.isFinite(saved.ratio2.w2)) {
      applySplitWeights(saved.ratio2.w1, saved.ratio2.w2);
    }
    // N5.3: variant BEFORE setSplit()/dockSurface() below make anything
    // visible, same "no flash of the default" treatment as the ratios above.
    setSplitVariant(saved.layout === 'stack' ? 'stack' : 'columns');
    setSplit(true);
    for (const id of (saved.docked || [])) {
      if (DOCKABLE.includes(id)) {
        if (id === 'graphics-overlay') openGraphics();   // rebuilds the gallery + docks
        else dockSurface(id, true);
      }
    }
    // N5.3: only meaningful once both are actually docked (a lone docked
    // surface has no "order" to speak of) — applyDockedOrder() itself is a
    // no-op if either id is missing/undocked.
    if (Array.isArray(saved.order) && saved.order.length === 2 &&
        DOCKABLE.every(id => saved.docked?.includes(id))) {
      applyDockedOrder(saved.order);
    }
  }

  function resetLayout() {
    setSplit(false);
    // N5.4: back to the default variant + source order — before persistSplit()
    // below so the cleared blob (splitOn=false -> removeItem) never even
    // briefly recorded the pre-reset variant/order.
    setSplitVariant('columns');
    applyDockedOrder(DOCKABLE);
    persistSplit();   // splitOn=false -> removes the key
    // U16: also drop the persisted pane-width overrides (panes.js reads these
    // at boot) — otherwise "Reset layout" only undid the split, leaving a
    // dragged Variables/History column exactly as wide as before. Clear the
    // live inline override too so the reset is immediate, not just next-load.
    localStorage.removeItem('piet-col-vars');
    localStorage.removeItem('piet-col-history');
    markUiStateDirty();   // N2: mirror the clear into ui_state too
    const app = document.getElementById('app');
    if (app) {
      app.style.removeProperty('--col-vars');
      app.style.removeProperty('--col-history');
    }
    // B4: back to 50/50 — clear the live inline override too, same immediate-
    // effect precedent as --col-vars/--col-history above.
    splitRatio = 50;
    document.getElementById('pane-console')?.style.removeProperty('--split-ratio');
    // BUG-113: back to 1:1 for the doedit↔graphics weight pair too.
    splitW1 = 1;
    splitW2 = 1;
    const pc = document.getElementById('pane-console');
    if (pc) {
      pc.style.removeProperty('--split-w1');
      pc.style.removeProperty('--split-w2');
    }
    document.getElementById('cmd-input')?.focus();
  }

  function onViewTabClick(overlayId, openFn, closeFn) {
    if (splitOn && DOCKABLE.includes(overlayId)) {
      // BUG-095 (P8.1-T1): a view-tab gesture always dismisses the full-surface
      // panels (Settings/About) even in split mode — this branch used to bypass
      // closeAllOverlays entirely, leaving Settings stranded on top (the
      // Results tab's own handler already does this sweep; mirror it here).
      for (const o of OVERLAYS) { if (!DOCKABLE.includes(o.id)) o.close(); }
      const docked = document.getElementById(overlayId)?.classList.contains('docked');
      if (docked) closeFn(); else openFn();
      return;
    }
    openFn();
  }

  function _isOverlayOpen(id) {
    return !!document.getElementById(id)?.classList.contains('open');
  }

  function closeAllOverlays(exceptId) {
    for (const o of OVERLAYS) {
      if (o.id !== exceptId) o.close();
    }
    syncViewTabs();   // UX-047: reconcile the view tabs after the sweep
  }

  function syncViewTabs() {
    const anyOpen     = OVERLAYS.some(o => o.isOpen());
    const doeditOpen   = _isOverlayOpen('doedit-overlay');
    const graphicsOpen = _isOverlayOpen('graphics-overlay');
    const resultsTab   = document.querySelector('.view-tab[data-view="results"]');
    const doeditTab    = document.querySelector('.view-tab[data-view="doedit"]');
    const graphicsTab  = document.querySelector('.view-tab[data-view="graphics"]');
    // BUG-096 (P8.1-T1): in split mode the Results column is ALWAYS present —
    // docked editor/graphics columns must not douse its underline ("underline
    // marks every currently-visible surface"). Only a FULL-surface overlay
    // (Data Browser / About / Settings — non-dockable) actually covers it.
    if (resultsTab) {
      const covered = OVERLAYS.some(o => o.isOpen() && !DOCKABLE.includes(o.id));
      resultsTab.classList.toggle('active', splitOn ? !covered : !anyOpen);
    }
    if (doeditTab)   doeditTab.classList.toggle('active', doeditOpen);
    if (graphicsTab) graphicsTab.classList.toggle('active', graphicsOpen);
    // F10/B4 (P8-S7): every split-state transition already ends up here (dock/
    // undock, setSplit, closeAllOverlays, the Results-tab additive-close) — one
    // hook keeps the "second column" placeholder/resizer honest without
    // duplicating this call at each site.
    syncSplitLayout();
  }

  function toggleSplitVariant() { setSplitVariant(_splitVariant === 'stack' ? 'columns' : 'stack'); }
  function isSplitOn() { return splitOn; }
  function isSwapArmed() { return !!_swapArmedId; }
  function closeNonDockableOverlays() {
    for (const o of OVERLAYS) { if (!DOCKABLE.includes(o.id)) o.close(); }
    syncViewTabs();
  }
  function hasOpenOverlay() { return OVERLAYS.some(o => o.isOpen()); }
  function closeFirstOpenOverlay() {
    for (const o of OVERLAYS) {
      if (o.isOpen()) { o.close(); return true; }
    }
    return false;
  }

  function bindControls() {
    // 2.2.0 item C: split-view toggle + one-click Reset layout.
    const splitToggle = document.getElementById('split-toggle');
    if (splitToggle) splitToggle.addEventListener('click', e => { e.preventDefault(); setSplit(!isSplitOn()); persistSplit(); });
    const resetLayoutBtn = document.getElementById('reset-layout');
    if (resetLayoutBtn) resetLayoutBtn.addEventListener('click', e => { e.preventDefault(); resetLayout(); });
    // B4 (P8-S7): split-column ratio drag — reuses panes.js's pointerdown/
    // pointermove/pointerup + requestAnimationFrame-coalesced idiom, adapted to
    // a percentage flex-basis instead of a pixel grid-track width.
    initSplitResizer();
    // BUG-113: second divider — doedit↔graphics, when both are docked columns.
    initSplitResizer2();

    // N5.1: layout-variant toggle (columns ↔ stack) — hidden until both
    // dockable surfaces are docked (syncSplitLayout() owns [hidden]).
    const layoutToggle = document.getElementById('layout-variant-toggle');
    if (layoutToggle) layoutToggle.addEventListener('click', e => {
      e.preventDefault();
      toggleSplitVariant();
      persistSplit();
    });

    // N5.2: click-swap — ONLY the title span arms/resolves it (buttons in the
    // same .doedit-bar, e.g. ▶ Run/Clear, must keep their own click behavior
    // untouched — they're siblings of the title, never inside it, so this
    // listener never sees those clicks at all).
    const doeditTitleEl   = document.getElementById('doedit-title');
    const graphicsTitleEl = document.getElementById('graphics-title');
    if (doeditTitleEl)   doeditTitleEl.addEventListener('click',   e => _onDockedTitleClick('doedit-overlay', e));
    if (graphicsTitleEl) graphicsTitleEl.addEventListener('click', e => _onDockedTitleClick('graphics-overlay', e));
    // "Esc or clicking elsewhere disarms" (N5.2) — the elsewhere half. The
    // title handlers above stopPropagation() their own clicks, so this never
    // fires for the arm/swap/self-disarm gesture itself, only genuinely
    // outside clicks. Esc is handled inline in the existing document keydown
    // handler further below (checked before the OVERLAYS sweep).
    document.addEventListener('click', () => { if (isSwapArmed()) _disarmSwap(); });
  }

  function bindNavigation() {
    // UX-047: Results view tab — generalizes "Back to Results" across every
    // overlay. closeAllOverlays() with no exceptId closes all four and calls
    // syncViewTabs() itself at its end (see the registry section below).
    // F10 (P8-S7): in split mode a plain closeAllOverlays() here used to ALSO
    // undock the Do-file Editor / Graphics columns (closeDoEditor/closeGraphics
    // both undock when isSplitOn() — see below) — silently contradicting the
    // still-ON Split toggle (the reported "button reads on, nothing is visibly
    // split" symptom: clicking Results while split flattened it back to
    // single-column). Results is the always-present base column in split mode,
    // so its tab has no exclusive-select meaning there; only the non-dockable,
    // exclusive-cover overlays (Data Browser / About / Settings) should close —
    // the docked columns stay put, matching the "view tabs become additive
    // column toggles" design this file already documents for the other two tabs.
    const resultsTab = document.querySelector('.view-tab[data-view="results"]');
    if (resultsTab) resultsTab.addEventListener('click', e => {
      e.preventDefault();
      if (isSplitOn()) {
        closeNonDockableOverlays();
      } else {
        closeAllOverlays();
      }
    });

    // UX-047: #browser-overlay is grid.js's own domain (frozen to WS-F1/F2) —
    // its Back button and internal Escape handler both call this.hide()
    // DIRECTLY (grid.js:117,195), bypassing the OVERLAYS registry's close()
    // path entirely. Rather than touch grid.js, observe the `hidden` attribute
    // it toggles: this IS the "grid show/hide adapter" the tab-sync design
    // calls for — one hook that catches every path (registry close(), the
    // direct grid.show()/hide() calls in runCommand(), AND grid's own internal
    // Back/Escape) uniformly, with no grid.js changes.
    const browserOverlayEl = document.getElementById('browser-overlay');
    if (browserOverlayEl) {
      new MutationObserver(syncViewTabs)
        .observe(browserOverlayEl, { attributes: true, attributeFilter: ['hidden'] });
    }
  }

  return { dockSurface, applySplitClass, applySplitRatio, applySplitWeights, syncSplitLayout, setSplitVariant, _bothDocked, swapDockedOrder, applyDockedOrder, _onDockedTitleClick, _armSwap, _disarmSwap, initSplitResizer, initSplitResizer2, setSplit, persistSplit, restoreSplit, resetLayout, onViewTabClick, _isOverlayOpen, closeAllOverlays, syncViewTabs, isSplitOn, isSwapArmed, closeNonDockableOverlays, hasOpenOverlay, closeFirstOpenOverlay, toggleSplitVariant, bindControls, bindNavigation };
}
