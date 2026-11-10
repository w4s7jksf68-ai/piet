// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// api-mock.js — demo/mock-mode fixtures and the mock command dispatcher (no kernel, no native bridge).

import { ApiError } from './api-error.js';

// ─── Mock fixture data ("Fatalities" — the bundled sample dataset) ────────────
// Demo/mock mode (a plain browser tab with no kernel + no native bridge)
// mirrors the app's bundled sample: the US traffic fatalities panel, 48
// states × 1982–1988 (Ruhm 1996; the Stock & Watson textbook dataset),
// compiled from US government statistics (NHTSA/FARS, Census, BEA) —
// public-domain facts. See Resources/sample/fatalities.csv and the
// "Bundled sample dataset" section of THIRD_PARTY_NOTICES.txt for
// provenance. Values below are REAL traces from that dataset: the first
// five rows, the actual summary stats, and an actual
// `feols fatal ~ beertax unemp` HC1 fit (cobra 0.19.1) — mock-fidelity law,
// structure AND values pinned to observed output.

const MOCK_VARLIST = {
  source: 'mock',
  source_file: 'fatalities.csv',
  varlist: [
    { name: 'fatal',   dtype: 'f64',    format: '%8.0gc', label: 'Number of vehicle fatalities',  n_missing: 0 },
    { name: 'beertax', dtype: 'f64',    format: '%8.0g',  label: 'Tax on case of beer',           n_missing: 0 },
    { name: 'unemp',   dtype: 'f64',    format: '%8.0g',  label: 'Unemployment rate',             n_missing: 0 },
    { name: 'income',  dtype: 'f64',    format: '%8.0gc', label: 'Per capita personal income',    n_missing: 0 },
    // B.3.0 (bounded transcripts, contract_B 2.5.0): the bulk varlist wire
    // shape dropped inline `levels` — n_levels-only now (a 108K-row factor's
    // levels array was 2.3MB per mutation). The real level strings ('no'/
    // 'yes') live in MOCK_FACTOR_LEVELS below, mockVarDetail()'s source for
    // the lazy per-variable fetch panels.js's card now does on PIN.
    { name: 'breath',  dtype: 'factor', format: '%8.0g',  label: 'Preliminary breath test law',   n_levels: 2, n_missing: 0 },
    { name: 'jail',    dtype: 'factor', format: '%8.0g',  label: 'Mandatory jail sentence',       n_levels: 2, n_missing: 1 },
  ],
  n_rows: 336,
  n_cols: 6,
};

const MOCK_COMMAND_REGRESS = {
  source: 'mock',
  blocks: [
    { type: 'echo', text: '.  regress fatal beertax unemp, robust' },
    {
      type: 'estimation',
      estimator: 'feols',
      depvar: 'fatal',
      n_obs: 336,
      vcov_type: 'hc1',
      r2: 0.009812560224601863,
      sigma2: 869079.746032829,
      dof_resid: 333,
      coeftable: [
        { name: 'beertax',     b:  88.53936112335201,  se:  86.74234812014322,  t: 1.02, p: 0.3081, ci_lo: -82.0926781044347,   ci_hi: 259.17140035113874 },
        { name: 'unemp',       b:  31.546229333217035, se:  14.918215364889912, t: 2.11, p: 0.0352, ci_lo:   2.2004073152937433, ci_hi:  60.892051351140324 },
        { name: '(intercept)', b: 651.458823867026,    se: 135.06717071676377,  t: 4.82, p: 0.0000, ci_lo: 385.76637638787946,   ci_hi: 917.1512713461726 },
      ],
    },
  ],
  varlist_after: null,
};

const MOCK_COMMAND_UNSUPPORTED = {
  source: 'mock',
  blocks: [
    { type: 'echo', text: '.  foreach v in fatal beertax {' },
    {
      type: 'error',
      code: 'unsupported_loop',
      token: 'foreach',
      message: 'foreach is a next-milestone feature (loops and macros arrive with the Tier-A interpreter).',
      reason: 'piet_gap',
    },
  ],
};

const MOCK_SUMMARIZE = {
  source: 'mock',
  blocks: [
    { type: 'echo', text: '.  summarize fatal beertax' },
    {
      type: 'summarize',
      detail: false,
      stats: {
        fatal:   { N: 336, mean: 928.6637, sd: 934.0515, min: 79,     max: 5504   },
        beertax: { N: 336, mean: 0.5133,   sd: 0.4778,   min: 0.0433, max: 2.7208 },
      },
    },
  ],
  varlist_after: null,
};

const MOCK_BROWSE = {
  source: 'mock',
  type: 'browse',
  offset: 0,
  limit: 5,
  n_rows_total: 336,
  columns: [
    { name: 'fatal',   dtype: 'f64',    index: 17 },
    { name: 'beertax', dtype: 'f64',    index: 7 },
    { name: 'unemp',   dtype: 'f64',    index: 4 },
    { name: 'breath',  dtype: 'factor', index: 14, levels: ['no','yes'] },
  ],
  // The real first five rows (Alabama 1982–1986).
  rows: [
    [ 839, 1.5394, 14.4, 0],
    [ 930, 1.7890, 13.7, 0],
    [ 932, 1.7143, 11.1, 0],
    [ 882, 1.6525,  8.9, 0],
    [1081, 1.6099,  9.8, 0],
  ],
};

const MOCK_HISTORY = {
  source: 'mock',
  history: [
    { cmd: 'use fatalities.csv',                    ts: Date.now() - 5000, success: true },
    { cmd: 'regress fatal beertax unemp, robust',   ts: Date.now() - 2000, success: true },
    { cmd: 'foreach v in fatal {',                  ts: Date.now() - 1000, success: false },
  ],
};

// ─── Mock command dispatcher ──────────────────────────────────────────────────

function mockCommand(cmd) {
  const lc = cmd.trim().toLowerCase();

  if (/^(regress|reg|feols|xtreg|logit|probit)\b/.test(lc)) {
    return { ...MOCK_COMMAND_REGRESS, blocks: [{ type: 'echo', text: `.  ${cmd}` }, ...MOCK_COMMAND_REGRESS.blocks.slice(1)] };
  }
  if (/^(sum|summ|summarize)\b/.test(lc)) {
    return { ...MOCK_SUMMARIZE, blocks: [{ type: 'echo', text: `.  ${cmd}` }, ...MOCK_SUMMARIZE.blocks.slice(1)] };
  }
  if (/^(br|browse)\b/.test(lc)) {
    return { source: 'mock', blocks: [{ type: 'echo', text: `.  ${cmd}` }, { ...MOCK_BROWSE }] };
  }
  if (/^(tab|tabulate)\b/.test(lc)) {
    const words = cmd.trim().split(/\s+/);
    const tvar = words[1] ?? 'breath';
    return {
      source: 'mock',
      blocks: [
        { type: 'echo', text: `.  ${cmd}` },
        {
          type: 'tabulate',
          vars: [tvar],
          table: {
            // Real one-way counts of `breath` in the bundled Fatalities panel.
            values: { no: 181, yes: 155 },
            margins: { total: 336 },
          },
        },
      ],
    };
  }
  if (/^(desc|describe)\b/.test(lc)) {
    return {
      source: 'mock',
      blocks: [
        { type: 'echo', text: `.  ${cmd}` },
        {
          type: 'describe',
          variables: MOCK_VARLIST.varlist.map(v => ({
            name: v.name, dtype: v.dtype, format: v.format || '%9.0g', label: v.label || '', n_missing: v.n_missing || 0,
          })),
        },
      ],
    };
  }
  // U3: the empty-state hint teaches `sysuse fatalities` and File ▾ > Open
  // sample dataset also runs it as a console command — mock it like the real
  // `open` action (CONTRACT: action:"open" mutation carries filepath) so the
  // taught command isn't a dead end in mock/demo mode. `use` is included for
  // the same reason (both load the one canned sample in mock mode).
  if (/^(use|sysuse)\b/.test(lc)) {
    return {
      source: 'mock',
      blocks: [
        { type: 'echo', text: `.  ${cmd}` },
        { type: 'data_mutation', action: 'open', filepath: MOCK_VARLIST.source_file, varlist_after: MOCK_VARLIST.varlist, n_rows_after: MOCK_VARLIST.n_rows },
      ],
      varlist_after: MOCK_VARLIST.varlist,
    };
  }
  if (/^(gen|generate|egen)\b/.test(lc)) {
    const newVarName = cmd.split(/\s+/)[1]?.split('=')[0]?.trim() || 'newvar';
    const newVarlist = [
      ...MOCK_VARLIST.varlist,
      { name: newVarName, dtype: 'f64', format: '%9.0g', n_missing: 0 },
    ];
    return {
      source: 'mock',
      blocks: [
        { type: 'echo', text: `.  ${cmd}` },
        { type: 'data_mutation', action: 'gen', new_vars: [newVarName], varlist_after: newVarlist, n_rows_after: MOCK_VARLIST.n_rows },
      ],
      varlist_after: newVarlist,
    };
  }
  if (/^(clear)\b/.test(lc)) {
    // U1: top-level varlist_after (not just the nested block's) so
    // runCommand()'s existing "did this mutate the varlist" check fires and
    // state.currentFile correctly clears to "(no data)" in mock mode too.
    return { source: 'mock', blocks: [{ type: 'echo', text: `.  ${cmd}` }, { type: 'data_mutation', action: 'drop', varlist_after: [], n_rows_after: 0 }], varlist_after: [] };
  }
  if (/^(foreach|forvalues|while|local|global|program|#delimit)\b/.test(lc)) {
    return MOCK_COMMAND_UNSUPPORTED;
  }

  // Unknown command
  return {
    source: 'mock',
    blocks: [
      { type: 'echo', text: `.  ${cmd}` },
      {
        type: 'error',
        code: 'unsupported_command',
        token: cmd.trim().split(/\s+/)[0],
        message: `'${cmd.trim().split(/\s+/)[0]}' is not a supported command in PIET.`,
        reason: 'piet_gap',
      },
    ],
  };
}

// ─── Factor level strings (B.3.0 bounded transcripts) ──────────────────────
// MOCK_VARLIST.varlist above no longer carries inline `levels` — only
// `n_levels` — mirroring the real native varlist/data_mutation emission
// change (a 108K-row factor's levels array was 2.3MB per mutation; see
// panels.js's _renderCard()/_maybeFetchVarDetail lazy-fetch-on-PIN pattern).
// The real level strings now live ONLY behind GET /api/var_detail's
// per-variable fetch — this map is mockVarDetail()'s source for them, the
// SAME two real values ('no'/'yes') MOCK_VARLIST.varlist used to carry
// inline, just relocated rather than invented (mock-fidelity law).
//
// NOTE for whoever ratifies contract_B 2.5.0: as of this edit, the REAL
// native varDetail() handler (piet/native/src/routes.cpp) does not yet
// serialize `levels` for factor columns (only n_missing, in its factor/
// string branch) and CONTRACT.md's documented var_detail shape doesn't list
// it either. The native lane's OWN F5 comment (datastore.cpp's
// VarInfo::toJson(), same worktree) confirms the intent matches this mock —
// "the C++ `levels` vector is untouched and still read directly by
// var_detail" — so routes.cpp's varDetail() growing a `levels` field is
// expected, just not landed as of this edit. This mock runs slightly ahead
// of that so the frontend's lazy-load-on-PIN path is exercisable/testable
// now; if routes.cpp ships without it, the "click to load" affordance
// degrades quietly (placeholder just never resolves, no crash) rather than
// breaking anything.
const MOCK_FACTOR_LEVELS = {
  breath: ['no', 'yes'],
  jail:   ['no', 'yes'],
};

// ─── GET /api/var_detail mock (N7) ─────────────────────────────────────────
// Sourced from MOCK_VARLIST + (for the two vars it covers) MOCK_COMMAND_
// REGRESS-adjacent MOCK_SUMMARIZE's real cobra-fit traces — mock-fidelity
// law, no invented numbers. Unrecognized name -> ApiError, mirroring the
// real bridge's "unknown name -> bridge error" contract behavior.
function mockVarDetail(name) {
  const v = MOCK_VARLIST.varlist.find(x => x.name === name);
  if (!v) throw new ApiError(`var_detail: unknown variable '${name}'`, null);
  const detail = {
    name: v.name, dtype: v.dtype, format: v.format || '%9.0g',
    label: v.label || '', N: MOCK_VARLIST.n_rows, n_missing: v.n_missing || 0,
  };
  // B.3.0: factor level strings, now fetched here instead of shipping
  // inline on the bulk varlist (see MOCK_FACTOR_LEVELS above).
  if (v.dtype === 'factor' && MOCK_FACTOR_LEVELS[name]) {
    detail.levels = MOCK_FACTOR_LEVELS[name];
  }
  const stats = MOCK_SUMMARIZE.blocks[1]?.stats?.[name];   // only fatal/beertax carry real moments
  if (stats) {
    detail.mean = stats.mean; detail.sd = stats.sd; detail.min = stats.min; detail.max = stats.max;
  }
  return detail;
}

export { MOCK_VARLIST, MOCK_BROWSE, MOCK_HISTORY, mockCommand, mockVarDetail };
