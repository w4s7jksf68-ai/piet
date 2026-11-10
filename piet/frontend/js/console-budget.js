// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// console-budget.js — memory-budget-derived caps shared by the Results console and the graphics gallery store.

// ─── Console scrollback cap (P7 — resource safety, COUNCIL-20260710 D5/F6) ────
// A runaway `.do` loop (e.g. `forvalues i=1/100000 { display ... }`) must not
// be able to grow the console DOM without bound: every appended block is a
// <div> that never goes away on its own, so an unbounded session is the
// "classroom-afternoon OOM" (RAW-resources.md F6) — the webview grows
// monotonically all day. Cap live console blocks by BOTH total byte size and
// block count; whichever trips first, drop the OLDEST live blocks (the
// newest output stays visible — same precedent as Stata's own Results-buffer
// truncation) and collapse everything dropped into ONE elision marker.
//
// The cap is derived from the user's configured memoryBudgetMb (Settings
// panel -> native piet::settings::Settings::memoryBudgetMb, SPEC II-2) — a
// small, generous share of it, so a normal classroom day of output never
// trips this and only a genuine runaway does. console.js has no standing
// route to the live settings value by itself (that lives behind api.js's
// settings.get bridge call, fetched by app.js's Settings panel — a different
// lane this sprint); initConsole's optional 4th argument and the exported
// setMemoryBudgetMb() are the hook a caller uses to push the real value in
// once fetched. Until one does, DEFAULT_MEMORY_BUDGET_MB mirrors index.html's
// shipped Settings slider default (#set-mem-budget value="2048"), so the
// fallback cap is exactly what a fresh install's configured budget would
// derive anyway.
export const DEFAULT_MEMORY_BUDGET_MB = 2048;

const BYTE_BUDGET_FRACTION = 0.01;               // 1% of the memory budget
const MIN_BYTE_BUDGET      = 1024;               // floor: never literally zero
const MAX_BYTE_BUDGET      = 128 * 1024 * 1024;  // ceiling: 128 MB even at an 8192 MB budget

const BLOCKS_PER_BUDGET_MB = 24;                 // ~24 retained blocks per configured MB
const MIN_BLOCK_COUNT      = 1;                  // always keep at least the newest block
const MAX_BLOCK_COUNT      = 50000;              // ceiling regardless of configured budget
                                                  // (DOM node count is a layout cost, not
                                                  // purely a memory-size one)

let _memoryBudgetMb = DEFAULT_MEMORY_BUDGET_MB;

/** Push the live memoryBudgetMb setting in (Settings panel apply, or a
 *  boot-time settings.get fetch) so the console cap tracks the user's
 *  configured budget. Ignores non-positive/non-finite input (keeps the last
 *  good value, or the shipped default if none was ever set). */
export function setMemoryBudgetMb(mb) {
  if (Number.isFinite(mb) && mb > 0) _memoryBudgetMb = mb;
}

function consoleCaps() {
  const byteBudget = Math.min(MAX_BYTE_BUDGET, Math.max(MIN_BYTE_BUDGET,
    Math.round(_memoryBudgetMb * 1024 * 1024 * BYTE_BUDGET_FRACTION)));
  const blockCount = Math.min(MAX_BLOCK_COUNT, Math.max(MIN_BLOCK_COUNT,
    Math.round(_memoryBudgetMb * BLOCKS_PER_BUDGET_MB)));
  return { byteBudget, blockCount };
}

// Rough byte-size estimate for a block: the source JSON's serialized length.
// Cheap, uniform across every block type (including graph blocks' numeric
// arrays), and correlates with both DOM footprint and bridge-payload size —
// exactly the two things this cap protects against.
function estimateBlockBytes(block) {
  try {
    return JSON.stringify(block).length;
  } catch {
    return 0;
  }
}

export { consoleCaps, estimateBlockBytes };
