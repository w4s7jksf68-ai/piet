// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// console.js — append-only results console
// Text formatting lives in estimation-format.js (the critical path, formatEstimation()) and table-format.js.

import { fmtObs, escHtml } from './util.js';
import { renderGraph, renderEventStudyBlock } from './graph.js';
import { getCachedPietVersion } from './api.js';
import { cacheSummarizeStats } from './varstats.js';
import { setMemoryBudgetMb, consoleCaps, estimateBlockBytes } from './console-budget.js';
import { registerGraph } from './graphics-gallery-store.js';
import { noviceErrorHint } from './error-hints.js';
import { formatEstimation } from './estimation-format.js';
import { formatCorrelate, formatSummarize, formatTabulate, formatDescribe, formatEstimatesTable } from './table-format.js';

// Compatibility re-exports: the DOM runner and the browser test pages import these names from console.js.
export { DEFAULT_MEMORY_BUDGET_MB, setMemoryBudgetMb } from './console-budget.js';
export { setGraphSpillMaxBytes, setGraphSpill, registerGraph, getGraphs, getGraphEntries, getGraphById,
         getResidentGraph, getGraphsAsync, getGraphSpilledCount, flushGraphSpill, getGraphDroppedCount,
         getGraphGalleryElidedCount, clearGraphGallery } from './graphics-gallery-store.js';
export { noviceErrorHint } from './error-hints.js';
export { formatEstimation } from './estimation-format.js';
export { formatCorrelate, formatSummarize, formatTabulate, formatDescribe } from './table-format.js';

// ─── DOM refs (set by init) ───────────────────────────────────────────────────
let _bodyEl  = null;   // #console-body — where blocks are appended
let _wrapEl  = null;   // #console-wrap — the scrollable container (parent of _bodyEl)
let _emptyEl = null;   // the empty-state SVG overlay (z-index 1)

// Scroll state
let _userScrolledUp = false;
let _jumpPill = null;

// Live blocks currently held in the DOM, oldest-first (mirrors _bodyEl's
// children minus the elision marker, which is tracked separately below).
let _liveBlocks  = [];     // [{ el, bytes }]
let _liveBytes   = 0;
let _elisionEl   = null;   // the single "…older output trimmed" marker node
let _elidedCount = 0;      // cumulative blocks dropped this session

// C4 (P8-S9): live-coding anchor — the single most-recent ESTIMATION block,
// tracked so the NEXT one can strip .block-active off it before handing the
// class to itself. Only ONE block ever carries .block-active at a time (the
// "say it, point at it" target during a lecture — mockup §2 `.blk.active`,
// state grammar §4 "active (tab/latest block/Run)"). Not reset by the P7 cap
// eviction above — a block only loses .block-active when a NEWER estimation
// block supersedes it, never by aging out of the byte/count budget (an
// evicted-while-still-active node is simply detached; classList ops on a
// detached node are harmless no-ops, and the next estimation block corrects
// the reference regardless).
let _activeBlockEl = null;

// BUG-1473: `display ... _continue` leaves its line open (a display block
// flagged `continue`), and the next display's text goes on that line, past any
// command echo between them -- the join doexec.cpp's pushBlock makes in the
// stored transcript. A streaming do-run has already shown the first block when
// the next one comes (streamed, not stored), so it is joined here. Any other
// block closes the line, and so does the start of a run (endDisplayLine, from
// app.js's setBusy): the transcript joins within one run only.
let _openDisplayEl = null;
/** Close an open `_continue` display line (a new run starts on its own line). */
export function endDisplayLine() { _openDisplayEl = null; }

// T3 (P8.1): output fold/unfold ("안2-lite"). An `echo` block (the
// `. <command>` line) is a fold head; its group is every flat sibling that
// follows it up to (not including) the next fold head — computed
// POSITIONALLY at click-time by _toggleFold() below, not stored on the
// blocks (see the deviation note at its call site in appendBlock). Zero DOM
// structure change either way (constraint (a)): every block stays a flat
// sibling of #console-body, so the P7 cap eviction, Save Results' direct-
// children walk, and the elision marker all keep working unmodified. Blocks
// appended before the FIRST echo of a session (e.g. a boot-time note) have
// no fold head to their left and are simply never foldable.

function updateElisionMarker() {
  if (!_elisionEl) {
    _elisionEl = document.createElement('div');
    _elisionEl.className = 'block block-elision block-echo';
    _bodyEl.insertBefore(_elisionEl, _bodyEl.firstChild);   // stays at the oldest position
  }
  _elisionEl.textContent = `…older output trimmed (${_elidedCount} blocks)`;
}

// Drop the OLDEST live blocks until both the byte and block-count budgets
// are satisfied again. Only ever removes whole blocks already in the DOM —
// never touches the content of a block that survives, so anything below the
// cap renders byte-identically to an uncapped session.
function enforceConsoleCap() {
  const { byteBudget, blockCount } = consoleCaps();
  let trimmed = 0;
  while (_liveBlocks.length > 0 &&
         (_liveBlocks.length > blockCount || _liveBytes > byteBudget)) {
    const oldest = _liveBlocks.shift();
    oldest.el.remove();
    _liveBytes -= oldest.bytes;
    trimmed++;
  }
  if (trimmed > 0) {
    _elidedCount += trimmed;
    updateElisionMarker();
  }
}

export function initConsole(consoleEl, emptyEl, jumpPill, memoryBudgetMb) {
  _bodyEl  = consoleEl;
  _wrapEl  = consoleEl.parentElement;  // #console-wrap is the overflow:auto element
  _emptyEl = emptyEl;
  _jumpPill = jumpPill;
  if (memoryBudgetMb != null) setMemoryBudgetMb(memoryBudgetMb);

  _wrapEl.addEventListener('scroll', () => {
    const atBottom =
      _wrapEl.scrollHeight - _wrapEl.scrollTop - _wrapEl.clientHeight < 4;
    _userScrolledUp = !atBottom;
    if (_jumpPill) _jumpPill.hidden = atBottom;
  });
}

// ─── Public API ───────────────────────────────────────────────────────────────

// BUG-150: `exit` is not an error — it is the executor's HALT SENTINEL, and
// its sentinel happens to be an untagged error block whose message is exactly
// "exit" or "exit <n>" (dispatcher.cpp's exit arm; the CommandErrorResult
// `code` enum is frozen, so the intended "program_exit" is coerced to
// engine_error at construction and cannot be used to tell the two apart —
// recognising the message SHAPE is the established mechanism, doexec.cpp's
// parseCleanExitMessage, BUG-139).
//
// Headless already reinterprets it twice: --do returns the requested rc, and
// handleUncapturedError() ERASES the sentinel from the stored block list. The
// GUI still painted it red because the live {kind:"block"} stream (N6) pushes
// each block as it completes — BEFORE that erase — so a clean run's last card
// was an alarming engine_error even though the batch response no longer
// carried it. Recognising it here, at the ONE append point both the streamed
// and the batch path go through, makes the two transports agree: the sentinel
// renders nothing at all, and applyDoResult() prints the single neutral
// "do-file ended" line instead (app.js).
const CLEAN_EXIT_RE = /^exit(\s+-?\d+)?$/;
export function isCleanExitSentinel(block) {
  return !!block && block.type === 'error' && !block.captured &&
         CLEAN_EXIT_RE.test(String(block.message ?? '').trim());
}

/** Append a ResultBlock (dispatch on .type). */
export function appendBlock(block) {
  if (!_bodyEl) return;
  if (isCleanExitSentinel(block)) return;   // BUG-150 — see above
  hideEmpty();

  // BUG-1473: a display continuing an open `_continue` line joins it.
  if (block.type === 'display' && _openDisplayEl && _openDisplayEl.isConnected) {
    _openDisplayEl.textContent += block.text ?? '';
    const live = _liveBlocks.find(b => b.el === _openDisplayEl);
    const bytes = estimateBlockBytes(block);
    if (live) { live.bytes += bytes; _liveBytes += bytes; }
    if (!block.continue) _openDisplayEl = null;
    enforceConsoleCap();
    if (!_userScrolledUp) _wrapEl.scrollTop = _wrapEl.scrollHeight;
    return;
  }
  if (block.type !== 'echo') _openDisplayEl = null;

  let el;
  switch (block.type) {
    case 'echo':             el = renderEcho(block);           break;
    case 'display':          el = renderDisplay(block);        break;
    // B9 (P8-S7): synthetic, client-only block — app.js's runCommand()
    // intercepts `note <text>` / `* <text>` before it ever reaches
    // client.command(), so this block type never crosses the wire.
    case 'note':              el = renderNote(block);           break;
    case 'estimation':       el = renderEstimation(block);     break;
    case 'summarize':
      el = renderSummarize(block);
      cacheSummarizeStats(block);   // B14(a): passive per-var stats cache for panels.js's detail card
      break;
    case 'tabulate':         el = renderTabulate(block);       break;
    case 'describe':         el = renderDescribe(block);       break;
    case 'data_mutation':    el = renderMutation(block);       break;
    case 'graph':
      registerGraph(block);            // gallery always collects (even suppressed)
      if (block.suppressed) return;     // set graphics off → skip live console append
      el = renderGraph(block);
      break;
    case 'error':            el = renderError(block);          break;
    case 'estimates_table':  el = renderEstimatesTable(block); break;
    default:
      el = renderEcho({ type: 'echo', text: JSON.stringify(block) });
  }

  // T3 (P8.1): fold-group tagging — an echo block becomes a clickable fold
  // head; every following flat sibling up to (not including) the next
  // .block-fold-head is that group's membership, determined POSITIONALLY at
  // fold-time (_toggleFold below) rather than stamped into the DOM here.
  //
  // Deviation from the original brief (which specified a module-level
  // counter written to el.dataset.group at append time): that shape leaks
  // append-call-count into a block's own outerHTML, which broke the P7 cap
  // suite's byte-identical guarantee ("P7: console cap (block-count-bound)…"
  // dom_runner.mjs:730) — its two comparison legs re-render the SAME block
  // fresh (no clearConsole reset, no replay of prior siblings) and diff
  // outerHTML against the capped survivor; a counter value baked into the
  // survivor (e.g. group="200" after 200 prior echoes) can never match the
  // fresh single-append render (group="1"). Positional grouping needs no
  // value stored on the block at all, so byte-identity holds unconditionally
  // — and it's semantically equivalent (DOM order already IS the grouping
  // signal; appendBlock never reorders blocks). See report "T3 deviation".
  if (block.type === 'echo') {
    el.classList.add('block-fold-head');
    el.tabIndex = 0;
    el.setAttribute('role', 'button');
    el.setAttribute('aria-expanded', 'true');
    el.addEventListener('click', () => _toggleFold(el));
    el.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        _toggleFold(el);
      }
    });
  }

  // A10 (P8.1): generalize esttab's "Copy table" affordance (:1447-1464
  // below) to every coeftable-bearing block type — hover-revealed action bar
  // that copies the PRE's own textContent. Scoped by block.type rather than
  // a post-hoc querySelector so a block that merely happens to contain a
  // <pre class="coeftable"> for unrelated reasons never picks this up.
  if (block.type === 'estimation' || block.type === 'summarize' ||
      block.type === 'tabulate'   || block.type === 'describe') {
    _addCopyBar(el);
  }

  _bodyEl.appendChild(el);
  if (block.type === 'display' && block.continue) _openDisplayEl = el;   // BUG-1473

  // C4 (P8-S9): live-coding anchor — hand .block-active to this block if
  // it's an estimation result (the mockup's demoed case: `regress` sits
  // active while the `summarize` above it stays plain), stripping it from
  // whichever block held it before. Scoped to 'estimation' only — echo/
  // display/mutation/etc. aren't "results" in the pointing-target sense.
  if (block.type === 'estimation') {
    if (_activeBlockEl) _activeBlockEl.classList.remove('block-active');
    el.classList.add('block-active');
    _activeBlockEl = el;
  }

  // P7: track this block against the byte/block-count cap and drop the
  // oldest live blocks if either budget is now exceeded. Pure bookkeeping —
  // the block just appended (and every surviving one) is never mutated.
  const bytes = estimateBlockBytes(block);
  _liveBlocks.push({ el, bytes });
  _liveBytes += bytes;
  enforceConsoleCap();

  if (!_userScrolledUp) {
    _wrapEl.scrollTop = _wrapEl.scrollHeight;
  }
}

// T3 (P8.1): fold/unfold a group in place — click/Enter/Space on an echo
// (fold head) toggles every flat sibling FOLLOWING it, up to (not
// including) the next .block-fold-head, between visible and
// `.block-folded` (display:none, CSS-APPEND). Positional, not a stored-id
// lookup — see the deviation note where this head's class is assigned in
// appendBlock. The head itself never hides — it flips `.fold-closed`
// (chevron pseudo-element swap) and carries `data-fold-count` (the closed-
// count pseudo-element) instead.
// Guard: an in-progress text selection means this click was a copy-drag
// over the echo text (e.g. ⌘C-by-drag of the command line), not a fold
// request — bail without touching anything.
function _toggleFold(headEl) {
  if (window.getSelection().toString() !== '') return;

  const closing = !headEl.classList.contains('fold-closed');
  let hiddenCount = 0;
  for (let node = headEl.nextElementSibling;
       node && !node.classList.contains('block-fold-head');
       node = node.nextElementSibling) {
    node.classList.toggle('block-folded', closing);
    if (closing) hiddenCount++;
  }

  headEl.classList.toggle('fold-closed', closing);
  headEl.setAttribute('aria-expanded', String(!closing));
  if (closing) {
    headEl.dataset.foldCount = String(hiddenCount);
  } else {
    delete headEl.dataset.foldCount;
  }
}

// A10 (P8.1): shared "Copy table" action bar — same clipboard idiom as the
// esttab actions bar below (:1447-1464 pre-T3 line numbers), generalized to
// any block whose rendered <pre class="coeftable"> should be copyable.
// Appended INSIDE the block, AFTER the <pre> (design constraint (b): never
// touches textContent; buttons live in their own action-bar div so a later
// integration step can strip `.console-action` nodes from saved logs).
function _addCopyBar(el) {
  const pre = el.querySelector('pre.coeftable');
  if (!pre) return;

  const bar = document.createElement('div');
  bar.className = 'esttab-actions block-copy-bar console-action';

  const copyBtn = document.createElement('button');
  copyBtn.textContent = 'Copy table';
  copyBtn.className = 'esttab-btn';
  copyBtn.addEventListener('click', () => {
    const text = pre.textContent;
    navigator.clipboard.writeText(text).catch(() => {
      // Fallback for environments without Clipboard API
      const ta = document.createElement('textarea');
      ta.value = text;
      document.body.appendChild(ta);
      ta.select();
      document.execCommand('copy');
      document.body.removeChild(ta);
    });
    copyBtn.textContent = 'Copied!';
    setTimeout(() => { copyBtn.textContent = 'Copy table'; }, 1500);
  });
  bar.appendChild(copyBtn);

  el.appendChild(bar);
}

/** Append multiple blocks in order, stopping at the first error for .do replay. */
export function appendBlocks(blocks) {
  let stopped = false;
  for (const b of blocks) {
    if (stopped) break;
    appendBlock(b);
    // BUG-150: a clean-exit sentinel is a halt, not a failure — it renders
    // nothing and must not set the "stop the replay" flag either, or a
    // trailing `exit` would suppress blocks the engine did emit after it.
    if (b.type === 'error' && !isCleanExitSentinel(b)) stopped = true;
  }
}

/** Clear console and re-show the empty-state overlay. */
export function clearConsole() {
  if (!_bodyEl) return;
  const blocks = _bodyEl.querySelectorAll('.block');
  blocks.forEach(b => b.remove());
  // U7 (P8 Honest Cockpit): `cls` clears the console TEXT only — the graphics
  // gallery survives (Stata parity: plots die on `graph drop _all`, not on cls).
  // P7: the elision marker (class "block") was just removed above along with
  // everything else — drop the JS-side cap-tracking state to match.
  _liveBlocks = [];
  _liveBytes = 0;
  _elisionEl = null;
  _elidedCount = 0;
  // C4: the active block (if any) was just removed above too — nothing left
  // to strip .block-active from until the next estimation block arrives.
  _activeBlockEl = null;
  _openDisplayEl = null;   // BUG-1473: its line went with the blocks
  // T3: no group-counter state to reset — fold membership is computed
  // positionally at click-time (_toggleFold), and every block it could
  // touch was just removed above.
  showEmpty();
}

// ─── Empty-state management ──────────────────────────────────────────────────

function hideEmpty() {
  if (_emptyEl) _emptyEl.classList.add('hidden');
}

function showEmpty() {
  if (_emptyEl) _emptyEl.classList.remove('hidden');
}

// ─── Block renderers ─────────────────────────────────────────────────────────

function renderEcho(block) {
  const div = document.createElement('div');
  div.className = 'block block-echo';
  div.textContent = '. ' + block.text;
  return div;
}

function renderDisplay(block) {
  // `display` output is plain result text, NOT a command echo — no '. ' prefix.
  const div = document.createElement('div');
  div.className = 'block block-display';
  div.textContent = block.text ?? '';
  return div;
}

/** B9 (P8-S7): a plain-text annotation — NOT a command, NOT engine output —
 *  rendered italic with NO '. ' prompt (SPEC III-6 label honesty: it visibly
 *  reads as "not a command" at a glance). Extends renderDisplay()'s plain-
 *  text idiom with one added class (.block-note, piet.css) rather than a
 *  second render path. */
function renderNote(block) {
  const div = document.createElement('div');
  div.className = 'block block-display block-note';
  div.textContent = block.text ?? '';
  return div;
}

function renderEstimation(block) {
  const div = document.createElement('div');
  div.className = 'block block-estimation';

  // BUG-112(8): non-fatal model-spec notes from the engine, rendered ABOVE the
  // table because that is where Stata prints them. One `note: ` line each, the
  // engine's own wording — this layer does not rephrase what the engine said.
  if (Array.isArray(block.warnings) && block.warnings.length) {
    const notes = document.createElement('pre');
    notes.className = 'coeftable est-notes';
    notes.textContent = block.warnings.map(w => `note: ${w}`).join('\n');
    div.appendChild(notes);
  }

  const pre = document.createElement('pre');
  pre.className = 'coeftable';
  // corr/correlate/pwcorr → dedicated matrix renderer (Stata lower-triangular).
  pre.textContent = block.estimator === 'corr'
    ? formatCorrelate(block)
    : formatEstimation(block);
  div.appendChild(pre);

  // csdid `estat event`: draw the event-study plot below the coefficient table.
  if (block.estat === 'event' && Array.isArray(block.event_study) && block.event_study.length) {
    div.appendChild(renderEventStudyBlock(block));
  }
  return div;
}

function renderSummarize(block) {
  const div = document.createElement('div');
  div.className = 'block block-table';

  const pre = document.createElement('pre');
  pre.className = 'coeftable';
  pre.textContent = formatSummarize(block);
  div.appendChild(pre);
  return div;
}

function renderDescribe(block) {
  const div = document.createElement('div');
  div.className = 'block block-table';

  const pre = document.createElement('pre');
  pre.className = 'coeftable';
  pre.textContent = formatDescribe(block);
  div.appendChild(pre);
  return div;
}

function renderMutation(block) {
  const div = document.createElement('div');
  div.className = 'block block-mutation';

  const { action, new_vars, dropped_vars, n_rows_after, varlist_after, renamed } = block;
  let msg = '';
  if (action === 'open') {
    const nCols = varlist_after ? varlist_after.length : '?';
    msg = `(${nCols} vars, ${fmtObs(n_rows_after)} obs loaded)`;
  } else if (action === 'gen' || action === 'egen') {
    if (new_vars && new_vars.length) {
      msg = new_vars.map(v => `(variable ${v} created)`).join('\n');
    }
  } else if (action === 'drop') {
    if (dropped_vars && dropped_vars.length) {
      msg = `(${dropped_vars.length} variable${dropped_vars.length > 1 ? 's' : ''} dropped)`;
    } else {
      msg = `(obs dropped, ${fmtObs(n_rows_after)} obs remaining)`;
    }
  } else if (action === 'keep' || action === 'keepif' || action === 'replace') {
    msg = `(${fmtObs(n_rows_after)} obs remaining)`;
  } else if (action === 'rename' && renamed) {
    msg = `(${renamed.from} renamed to ${renamed.to})`;
  } else if (action === 'recode' && new_vars && new_vars.length) {
    msg = `(recode: new variable ${new_vars[0]} created)`;
  } else {
    msg = `(${action} completed)`;
  }
  div.textContent = msg;
  return div;
}

function renderError(block) {
  const div = document.createElement('div');
  div.className = 'block block-error';

  const { code, message, token, line_number, context, reason } = block;
  // Show error code + token so users can identify what was rejected
  // (e.g. "unsupported_option: aweights" alongside the human-readable message)
  const codeLabel = token
    ? `${escHtml(code)}: ${escHtml(token)}`
    : escHtml(code);
  let html = `<span class="err-code">${codeLabel}</span>\n<span class="err-msg">${escHtml(message)}</span>`;
  if (line_number !== undefined && context) {
    html += `\n<span class="err-stop">line ${line_number}: ${escHtml(context)}</span>`;
  }

  // Contract B v1.8.0: optional `reason` field with a user-facing hint.
  // Also surface the cached PIET version (fetched from GET /api/version on connect).
  if (reason) {
    const pietVer = getCachedPietVersion();
    const verStr  = pietVer ? `PIET ${escHtml(pietVer)}` : 'PIET';
    let hint = '';
    if (reason === 'piet_gap') {
      hint = `(not yet wired in ${verStr} — a future update will add it)`;
    } else if (reason === 'engine_gap') {
      hint = '(the cobra engine does not support this yet)';
    }
    if (hint) {
      html += `\n<span class="err-hint">${hint}</span>`;
    }
  }

  // A18/B1: curated plain-language fix line for known novice failures —
  // reuses .err-hint styling (same muted supplementary-note treatment as
  // the reason banner above) rather than adding a new CSS class, since this
  // fix is JS-only scope. Never replaces the real code/message above it.
  const novice = noviceErrorHint(block);
  if (novice) {
    html += `\n<span class="err-hint">${escHtml(novice)}</span>`;
  }

  div.innerHTML = html;

  // T4 (P8.1): error-click hook — a block carrying line_number becomes
  // clickable, dispatching a CustomEvent for the Do-file Editor / gutter to
  // react to (see INTEGRATION in the sprint report for the app.js listener
  // that calls openDoEditor()/selectEditorLine()/setGutterErrorLine()). No
  // app.js import here — mirrors panels.js's own cross-module CustomEvent
  // idiom ('piet:var-edit', 'piet:hist-send-doedit'). textContent is
  // untouched (constraint (b)) — only a class, a title, and a listener.
  if (line_number !== undefined) {
    div.classList.add('err-jump');
    div.title = `Click to jump to line ${line_number} in the Do-file Editor`;
    div.addEventListener('click', () => {
      document.dispatchEvent(new CustomEvent('piet:error-line-click', {
        detail: { line: line_number },
      }));
    });
  }

  return div;
}

function renderTabulate(block) {
  const div = document.createElement('div');
  div.className = 'block block-table';

  const pre = document.createElement('pre');
  pre.className = 'coeftable';
  pre.textContent = formatTabulate(block);
  div.appendChild(pre);
  return div;
}

// ─── Estimates Table (esttab) ─────────────────────────────────────────────────

/**
 * Render an EstimatesTableResult block as a multi-column regression table.
 * Mirrors the 1.4.0 two-way tab render pattern: <pre> inside a .block-table,
 * with Copy and Export-CSV buttons.
 */
function renderEstimatesTable(block) {
  const div = document.createElement('div');
  div.className = 'block block-table block-estimates-table';

  // ── Formatted text rendering ──────────────────────────────────────────────
  const pre = document.createElement('pre');
  pre.className = 'coeftable';
  pre.textContent = formatEstimatesTable(block);
  div.appendChild(pre);

  // ── Action bar: Copy + Export CSV ─────────────────────────────────────────
  // A10 (P8.1): + console-action so the save-log strip (a later integration
  // step) covers this bar the same way it covers the T3 generalized one.
  const bar = document.createElement('div');
  bar.className = 'esttab-actions console-action';

  const copyBtn = document.createElement('button');
  copyBtn.textContent = 'Copy table';
  copyBtn.className = 'esttab-btn';
  copyBtn.addEventListener('click', () => {
    const text = pre.textContent;
    navigator.clipboard.writeText(text).catch(() => {
      // Fallback for environments without Clipboard API
      const ta = document.createElement('textarea');
      ta.value = text;
      document.body.appendChild(ta);
      ta.select();
      document.execCommand('copy');
      document.body.removeChild(ta);
    });
    copyBtn.textContent = 'Copied!';
    setTimeout(() => { copyBtn.textContent = 'Copy table'; }, 1500);
  });
  bar.appendChild(copyBtn);

  const csvBtn = document.createElement('button');
  csvBtn.textContent = 'Export CSV';
  csvBtn.className = 'esttab-btn';
  csvBtn.addEventListener('click', () => {
    const csv = block.csv || '';
    const blob = new Blob([csv], { type: 'text/csv' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'esttab.csv';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  });
  bar.appendChild(csvBtn);

  div.appendChild(bar);
  return div;
}
