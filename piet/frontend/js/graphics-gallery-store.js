// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// graphics-gallery-store.js — session store of graph blocks for the Graphics gallery (byte/count caps, disk spill).

import { createSpill, idbStore } from './graph_spill.js';
import { consoleCaps, estimateBlockBytes } from './console-budget.js';

// ─── Graphics gallery store (WS4) ──────────────────────────────────────────────
// Every `graph` block rendered to the console is also collected here so the
// Graphics overlay can present a session gallery. Thumbnails re-render the SAME
// block through renderGraph() (no new rendering code, Doctrine §3 lightness).
const _graphGallery = [];   // [{ id, title, kind, block|null, bytes, stored, storedBytes, state }], oldest-first
let _galleryBytes  = 0;     // bytes of RESIDENT blocks only
let _galleryElided = 0;     // cumulative thumbnails dropped (not preserved) this session
let _spillBytes    = 0;     // stored (compressed) bytes on disk
let _spillSeq      = 0;
let _galleryGen    = 0;     // bumped by clearGraphGallery so late writes self-delete
const _spillPending = new Set();
// Spilled-storage cap: generous, oldest-first drop with an honest marker.
let GRAPH_SPILL_MAX_BYTES = 256 * 1024 * 1024;
/** Override the spilled-storage cap (tests); non-positive restores the default. */
export function setGraphSpillMaxBytes(n) { GRAPH_SPILL_MAX_BYTES = n > 0 ? n : 256 * 1024 * 1024; }
let _spill;                 // undefined = not yet resolved; null = unavailable
function spill() {
  if (_spill === undefined) {
    let s = null;
    try { const st = idbStore(); s = st ? createSpill({ store: st }) : null; } catch (_) { s = null; }
    _spill = s;
  }
  return _spill;
}
/** Inject a spill (tests) or null to disable spilling. */
export function setGraphSpill(s) { _spill = s; }
// WS-F2 mission 4 / audit A6-01 sibling: an unbounded session (a long .do loop
// emitting graphs) would otherwise grow this array without limit. Cap it and
// drop the oldest — the gallery is a recent-session convenience, not an archive.
const GRAPH_GALLERY_MAX = 50;

// BUG-066 (C3-F5 "gallery stores byte-bounded thumbnails" — the last component):
// GRAPH_GALLERY_MAX alone bounds the gallery by COUNT, and a graph block's size
// is not bounded by the live-view decimation the native side applies: that
// budget (graph_m4.cpp kMaxViewPoints = 5000) is per points-ARRAY, so a block's
// bytes still scale with its layer/series count. Measured on the 108K corpus
// (soccer_game_panel, post-decimation): 1-series scatter 191 KB · 4-series
// scatter 553 KB · 5-layer twoway 764 KB — so 50 slots is ~38 MB and climbs
// with every extra layer a user writes. The gallery also OUTLIVES the console's
// own P7 eviction (it holds the block objects; _liveBlocks holds only DOM
// nodes + sizes), which makes it the dominant retainer in a long session.
//
// So bound it by bytes too, reusing the console's own budget primitives
// (consoleCaps().byteBudget, estimateBlockBytes) rather than inventing a second
// scheme — the gallery may retain at most as many bytes as the live console
// may. The newest thumbnail is ALWAYS kept, even if it alone exceeds the budget
// (mirrors MIN_BLOCK_COUNT=1 for the console cap): a single huge plot must
// still be viewable.
//
// This bounds the gallery's OWN copy only. `session.lastGraph` / `namedGraphs`
// live native-side and back `graph export/save/combine` (the golden-pinned SVG
// seam) — untouched here, by construction.
// e.counted = this entry's bytes are currently included in _galleryBytes.
function uncount(e) { if (e.counted) { e.counted = false; _galleryBytes -= e.bytes; } }

function dropEntry(e, countElided) {
  const i = _graphGallery.indexOf(e);
  if (i >= 0) _graphGallery.splice(i, 1);
  uncount(e);
  if (e.stored) {
    _spillBytes -= e.storedBytes;
    const sp = spill();
    if (sp) sp.del(e.id).catch(() => {});
  }
  e.state = 'dead';
  e.block = null;
  if (countElided) _galleryElided++;
}

function spillEntry(e) {
  const sp = spill();
  const gen = _galleryGen;
  e.state = 'spilling';
  const p = sp.put(e.id, e.block).then(r => {
    if (e.state === 'dead' || gen !== _galleryGen) {
      sp.del(e.id).catch(() => {});
      return;
    }
    e.stored = true; e.storedBytes = r.stored; _spillBytes += r.stored;
    e.state = 'stored';
    e.block = null;
    enforceSpillCap();
  }).catch(() => {
    if (e.state !== 'dead' && gen === _galleryGen) dropEntry(e, true);
  }).finally(() => { _spillPending.delete(p); });
  _spillPending.add(p);
}

function enforceSpillCap() {
  while (_spillBytes > GRAPH_SPILL_MAX_BYTES) {
    const e = _graphGallery.find(g => g.stored && !g.block);
    if (!e) break;
    dropEntry(e, true);
  }
}

function enforceGalleryCap() {
  const { byteBudget } = consoleCaps();
  const resident = () => _graphGallery.filter(g => g.block && g.state !== 'spilling');
  for (;;) {
    const res = resident();
    if (!(res.length > 1 && (res.length > GRAPH_GALLERY_MAX || _galleryBytes > byteBudget))) break;
    const e = res[0];
    if (e.stored) { e.block = null; uncount(e); e.state = 'stored'; }   // already on disk
    else if (spill()) { uncount(e); spillEntry(e); }
    else dropEntry(e, true);
  }
}

/** Collect a graph block for the Graphics gallery (called from appendBlock). */
export function registerGraph(block) {
  const bytes = estimateBlockBytes(block);
  _graphGallery.push({
    id: `g${++_spillSeq}`, title: block && block.title, kind: block && (block.kind || block.type),
    block, bytes, counted: true, stored: false, storedBytes: 0, state: 'resident',
  });
  _galleryBytes += bytes;
  enforceGalleryCap();
}

/** Resident graph blocks, in render order (sync; spilled entries are skipped). */
export function getGraphs() {
  return _graphGallery.filter(g => g.block).map(g => g.block);
}

/** Every gallery entry in order (sync, tiny): { id, title, kind, bytes, storedBytes, resident }. */
export function getGraphEntries() {
  return _graphGallery.map(g => ({
    id: g.id, title: g.title, kind: g.kind, bytes: g.bytes,
    storedBytes: g.storedBytes, resident: !!g.block,
  }));
}

async function rehydrate(e) {
  if (e.block) return e.block;
  const sp = spill();
  if (!sp || !e.stored) return null;
  let block = null;
  try { block = await sp.get(e.id); } catch (_) { block = null; }
  if (!block) { if (e.state !== 'dead') dropEntry(e, true); return null; }
  if (e.state === 'dead') return block;
  if (!e.block) { e.block = block; e.counted = true; _galleryBytes += e.bytes; }
  enforceGalleryCap();
  return block;
}

/** Block for entry id, rehydrating from disk when spilled (null if lost). */
export async function getGraphById(id) {
  const e = _graphGallery.find(g => g.id === id);
  return e ? rehydrate(e) : null;
}

/** Block for entry id if it is resident now (sync), else null: a spilled entry
 *  is loaded only when asked for, through getGraphById. */
export function getResidentGraph(id) {
  const e = _graphGallery.find(g => g.id === id);
  return e && e.block ? e.block : null;
}

/** All blocks in order, rehydrating spilled ones one at a time (lost ones
 *  skipped). An async iterator, so the caller holds one rehydrated block at a
 *  time: collecting them all in one array kept every spilled plot in memory
 *  at once, past the gallery's byte budget (BUG-1472). */
export async function* getGraphsAsync() {
  for (const e of _graphGallery.slice()) {
    const b = await rehydrate(e);
    if (b) yield b;
  }
}

/** Entries preserved on disk right now (the gallery's "N stored on disk"). */
export function getGraphSpilledCount() {
  return _graphGallery.filter(g => g.stored).length;
}

/** Resolves once all in-flight spill writes settle. */
export async function flushGraphSpill() {
  while (_spillPending.size) await Promise.all([..._spillPending]);
}

/** Thumbnails dropped for good (storage unavailable/failed/over its cap). */
export function getGraphDroppedCount() {
  return _galleryElided;
}

/** Plots the sync getGraphs() does not return: dropped for good plus those
 *  preserved on disk. Keeps the legacy "N older plots trimmed" note honest for
 *  callers that only render getGraphs(). */
export function getGraphGalleryElidedCount() {
  return _galleryElided + _graphGallery.filter(g => !g.block).length;
}

/** U7 (P8 Honest Cockpit): explicit gallery clear. `cls` no longer wipes the
 *  gallery (Stata parity), so this is the dedicated primitive for dropping
 *  collected plots — the Stata semantic owner is `graph drop _all`. (Wiring
 *  that command to this is a small follow-up.) */
export function clearGraphGallery() {
  for (const e of _graphGallery) e.state = 'dead';
  _graphGallery.length = 0;
  _galleryBytes  = 0;
  _galleryElided = 0;
  _spillBytes    = 0;
  _galleryGen++;
  const sp = _spill;
  if (sp) sp.clear().catch(() => {});
}
