// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Gallery selection and rendering; console.js owns graph storage.
import { getGraphEntries, getGraphById, getResidentGraph, getGraphSpilledCount, getGraphDroppedCount } from './console.js';
import { renderGraph } from './graph.js';

export function createGraphicsController({ layout, svgToPng }) {
  let _graphicsFullIndex = -1;
  const { closeAllOverlays, dockSurface, syncViewTabs, isSplitOn, onViewTabClick } = layout;

  function refreshGraphicsIfOpen() {
    const ov = document.getElementById('graphics-overlay');
    if (ov && ov.classList.contains('open')) openGraphics();
  }

  function openGraphics() {
    const ov = document.getElementById('graphics-overlay');
    const gal = document.getElementById('graphics-gallery');
    if (!ov || !gal) return;
    // Mission 1 / 2.2.0 item C: single-view closes the other overlays first;
    // split-view docks Graphics as a column beside Results (coexists) — the
    // gallery is rebuilt either way.
    if (!isSplitOn()) closeAllOverlays('graphics-overlay');

    // Rebuild the gallery from the session's collected graph entries — every
    // one, in order: resident blocks render now, and a plot console.js has
    // stored on disk (gallery spill) is a card loaded when it is opened or
    // downloaded (BUG-1471; getGraphs() was resident-only).
    gal.replaceChildren();
    const entries = getGraphEntries();
    if (!entries.length) {
      _graphicsFullIndex = -1;   // B10: nothing to view full
      const empty = document.createElement('div');
      empty.className = 'graphics-empty';
      empty.textContent = 'No graphs yet — run a plotting command (e.g. histogram, scatter, marginsplot).';
      gal.appendChild(empty);
    } else {
      // B10: out-of-range guard — the gallery may have shrunk (a fresh session,
      // `graph drop _all`-equivalent, the BUG-066 byte/count cap, …) since a
      // prior full view was open.
      if (_graphicsFullIndex >= entries.length) _graphicsFullIndex = -1;
      // BUG-066: the gallery is byte- and count-bounded (console.js
      // enforceGalleryCap). Say so when it has actually dropped something —
      // the same honesty the console's elision marker carries, rather than
      // letting older plots vanish silently — and how many it keeps on disk.
      const galleryNote = text => {
        const note = document.createElement('div');
        note.className = 'graphics-elision';
        note.textContent = text;
        gal.appendChild(note);
      };
      const dropped = getGraphDroppedCount();
      if (dropped > 0) galleryNote(`…${dropped} older plot${dropped === 1 ? '' : 's'} trimmed (memory budget)`);
      const stored = getGraphSpilledCount();
      if (stored > 0) galleryNote(`${stored} plot${stored === 1 ? '' : 's'} stored on disk (memory budget) — loaded when opened`);
      entries.forEach((ent, i) => {
        const card = document.createElement('div');
        card.className = 'graphics-card';
        card.tabIndex = 0;
        card.setAttribute('role', 'button');
        card.setAttribute('aria-label', `View ${ent.kind || 'graph'} ${i + 1} of ${entries.length} full size`);
        // Re-render the SAME block as a thumbnail (no new rendering code); a
        // stored plot shows a placeholder until it is loaded.
        const block = getResidentGraph(ent.id);
        let thumb;
        if (block) {
          thumb = renderGraph(block);
        } else {
          thumb = document.createElement('div');
          thumb.className = 'graphics-empty';
          thumb.textContent = `${ent.title || ent.kind || 'graph'} — stored on disk; open to view`;
        }
        card.appendChild(thumb);
        const dl = document.createElement('button');
        dl.className = 'graphics-dl-btn';
        dl.textContent = '⬇ PNG';
        dl.addEventListener('click', async e => {
          e.stopPropagation();   // B10: a download click must not ALSO open the full view
          if (!thumb.querySelector('svg')) {
            const loaded = await getGraphById(ent.id);
            if (!loaded) { thumb.textContent = 'This plot could not be loaded from disk.'; return; }
            const t = renderGraph(loaded);
            card.replaceChild(t, thumb);
            thumb = t;
          }
          const svg = thumb.querySelector('svg');
          if (svg) svgToPng(svg, `${ent.kind || 'graph'}-${i + 1}.png`);
        });
        card.appendChild(dl);
        // B10: click (or Enter/Space) the card, outside the download button
        // (which stops its own propagation above) — open the full-size view.
        card.addEventListener('click', () => openGraphicsFull(i));
        card.addEventListener('keydown', e => {
          if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); openGraphicsFull(i); }
        });
        gal.appendChild(card);
      });
    }

    renderGraphicsFullView();   // B10: (re)apply whichever mode (gallery/full) is active

    // 2.2.0 item C: split-view docks Graphics as a column; single-view shows it
    // as the exclusive overlay.
    if (isSplitOn()) {
      dockSurface('graphics-overlay', true);
    } else {
      ov.classList.add('open');
      syncViewTabs();   // UX-047: Graphics tab active
    }
  }

  function closeGraphics() {
    _graphicsFullIndex = -1;   // B10: gallery is always the default on reopen
    // 2.2.0 item C: in split-view "close" = undock the Graphics column.
    if (isSplitOn()) { dockSurface('graphics-overlay', false); return; }
    const ov = document.getElementById('graphics-overlay');
    if (ov) ov.classList.remove('open');
    document.getElementById('cmd-input')?.focus();
    syncViewTabs();   // UX-047: Results tab active again (if nothing else is open)
  }

  function renderGraphicsFullView() {
    const gal     = document.getElementById('graphics-gallery');
    const full    = document.getElementById('graphics-full');
    const actions = document.getElementById('graphics-full-actions');
    const posEl   = document.getElementById('graphics-full-pos');
    if (!gal || !full) return;
    const entries = getGraphEntries();
    const showFull = _graphicsFullIndex >= 0 && _graphicsFullIndex < entries.length;

    gal.hidden  = showFull;
    full.hidden = !showFull;
    if (actions) actions.hidden = !showFull;
    if (!showFull) return;

    const ent = entries[_graphicsFullIndex];
    const show = block => {
      const big = renderGraph(block);
      big.classList.add('graphics-full-svg');
      full.replaceChildren(big);
    };
    if (posEl) posEl.textContent = `${_graphicsFullIndex + 1} / ${entries.length}`;
    const block = getResidentGraph(ent.id);
    if (block) { show(block); return; }
    // BUG-1471: a plot stored on disk is loaded when it is opened.
    const wait = document.createElement('div');
    wait.className = 'graphics-empty';
    wait.textContent = 'Loading the plot from disk…';
    full.replaceChildren(wait);
    const idx = _graphicsFullIndex;
    getGraphById(ent.id).then(loaded => {
      if (_graphicsFullIndex !== idx || full.hidden) return;   // moved on meanwhile
      if (loaded) show(loaded);
      else wait.textContent = 'This plot could not be loaded from disk.';
    });
  }

  function openGraphicsFull(idx) {
    _graphicsFullIndex = idx;
    renderGraphicsFullView();
  }

  function graphicsFullStep(delta) {
    const n = getGraphEntries().length;
    if (!n || _graphicsFullIndex < 0) return;
    _graphicsFullIndex = (_graphicsFullIndex + delta + n) % n;
    renderGraphicsFullView();
  }

  function closeGraphicsFull() {
    _graphicsFullIndex = -1;
    renderGraphicsFullView();
  }

  function bindEntryControl() {
    // Graphics gallery overlay (WS4) — 2.2.0 item C: same toggle-in-split rule.
    const graphicsLink = document.querySelector('[data-action="graphics"]');
    if (graphicsLink) graphicsLink.addEventListener('click', e => { e.preventDefault(); onViewTabClick('graphics-overlay', openGraphics, closeGraphics); });
    // UX-047: #graphics-close removed from index.html (III-6) — see doedit's note above.
  }

  function bindGalleryNavigation() {
    // B10 (P8-S7): Graphics gallery full-view prev/next/back.
    const gfPrev = document.getElementById('graphics-full-prev');
    if (gfPrev) gfPrev.addEventListener('click', () => graphicsFullStep(-1));
    const gfNext = document.getElementById('graphics-full-next');
    if (gfNext) gfNext.addEventListener('click', () => graphicsFullStep(1));
    const gfBack = document.getElementById('graphics-full-back');
    if (gfBack) gfBack.addEventListener('click', closeGraphicsFull);
    // Arrow-key nav while the full view is showing — scoped to the graphics
    // overlay (only fires while focus is inside it, e.g. on one of the buttons
    // above) so it can't collide with #cmd-input's own ArrowUp/Down history nav.
    const graphicsOv = document.getElementById('graphics-overlay');
    if (graphicsOv) {
      graphicsOv.addEventListener('keydown', e => {
        const full = document.getElementById('graphics-full');
        if (!full || full.hidden) return;
        if (e.key === 'ArrowLeft')  { e.preventDefault(); graphicsFullStep(-1); }
        if (e.key === 'ArrowRight') { e.preventDefault(); graphicsFullStep(1); }
      });
    }
  }

  return { refreshGraphicsIfOpen, openGraphics, closeGraphics, renderGraphicsFullView, openGraphicsFull, graphicsFullStep, closeGraphicsFull, bindEntryControl, bindGalleryNavigation };
}
