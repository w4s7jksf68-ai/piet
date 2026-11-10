// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// table-format.js — correlate/summarize/tabulate/describe/esttab blocks -> monospace text tables.

import { fmtG, fmtP, fmtObs, padL, padR } from './util.js';

// ─── Correlation matrix (correlate / pwcorr) ───────────────────────────────────
// Stata BE 19.5 lower-triangular layout. The block (estimator==='corr') carries
// the full matrix forwarded by the dispatcher: r (2D), variables (names),
// and — for pwcorr,sig — p (2D p-values) and nobs (2D per-pair N).
export function formatCorrelate(block) {
  const names = block.variables || [];
  const r = block.r || [];
  if (!names.length || !r.length) return '(no correlation matrix)';

  const isPwcorr = block.method === 'pwcorr' || block.pairwise === true;
  const showSig  = block.sig === true && Array.isArray(block.p);
  // BS-028 companion (BETA C2+C3): `pwcorr ..., obs` — real Stata prints the
  // per-pair N beneath each coefficient (verified against a live
  // `StataBE -b do` run of `pwcorr income beertax spirits, obs` on the same
  // fatalities data this arm's other fixtures use). Gated on block.obs===true
  // (only pwcorr sets this; correlate,obs is rejected upstream by the
  // dispatcher) rather than mere nobs presence — nobs already flows on every
  // pairwise corr block regardless of the obs flag (dispatcher.cpp:2863-2867),
  // so keying off nobs alone would turn the row on unrequested.
  const showObs  = block.obs === true && Array.isArray(block.nobs);

  const fmt4 = (x) => (x === null || x === undefined || Number.isNaN(x))
    ? '' : Number(x).toFixed(4);
  const fmtNobs = (x) => (x === null || x === undefined || Number.isNaN(x))
    ? '' : String(Math.round(Number(x)));

  const COL = 9;                                  // each numeric column is %9.4f wide
  const stubW = Math.max(12, ...names.map(n => n.length));
  const ncol = names.length;

  const lines = [];
  // correlate (listwise) prints "(obs=N)"; pwcorr does not.
  if (!isPwcorr) {
    const nObs = block.n_obs ?? block.nobs ?? '';
    lines.push(`(obs=${nObs})`);
    lines.push('');
  }

  // Header: blank stub + ' │' + each var name right-aligned in COL.
  lines.push(' '.repeat(stubW) + ' │' + names.map(n => padL(n, COL)).join(''));
  // Rule: stubW+1 dashes + ┼ + COL*ncol dashes.
  lines.push('─'.repeat(stubW + 1) + '┼' + '─'.repeat(COL * ncol));

  for (let i = 0; i < ncol; i++) {
    // Coefficient row: lower-triangular (cols 0..i).
    const coefCells = [];
    for (let j = 0; j <= i; j++) coefCells.push(padL(fmt4(r[i][j]), COL));
    // Stata adds a trailing space after the last coef on pwcorr rows.
    lines.push(padL(names[i], stubW) + ' │' + coefCells.join('') + (isPwcorr ? ' ' : ''));

    if (showSig) {
      // Significance row: p-values for strictly-lower cells (cols 0..i-1);
      // the diagonal/self cell is blank.
      const sigCells = [];
      for (let j = 0; j < i; j++) sigCells.push(padL(fmt4(block.p[i][j]), COL));
      lines.push(' '.repeat(stubW) + ' │' + sigCells.join(''));
    }

    if (showObs) {
      // pwcorr,obs: per-pair N beneath the coefficient row, cols 0..i
      // INCLUDING the diagonal (nobs[i][i] is var i's own N — unlike the sig
      // row above, a pair's own obs count is well-defined, so real Stata
      // prints it; verified: the "income" row of a live `pwcorr ..., obs`
      // run shows a lone "336" under its single 1.0000 diagonal cell).
      const obsCells = [];
      for (let j = 0; j <= i; j++) obsCells.push(padL(fmtNobs(block.nobs[i][j]), COL));
      lines.push(' '.repeat(stubW) + ' │' + obsCells.join(''));
    }

    if (showSig || showObs) {
      // Blank spacer row (just the stub pipe) before the next var's row.
      lines.push(' '.repeat(stubW) + ' │');
    }
  }
  return lines.join('\n');
}

// ─── Summary stats ────────────────────────────────────────────────────────────

export function formatSummarize(block) {
  const { stats } = block;
  if (!stats) return '(no summary)';

  const vars    = Object.keys(stats);
  // Stata BE 19.5 summarize: stub column ` |`-separated, then Obs/Mean/Std. dev./Min/Max.
  const stubHdr = 'Variable';
  const dataHdrs = ['Obs', 'Mean', 'Std. dev.', 'Min', 'Max'];

  // Pre-format the 5 data columns so widths can be computed dynamically.
  const dataRows = vars.map(v => {
    const s = stats[v];
    return [
      fmtObs(s.N),
      fmtG(s.mean) ?? '',
      fmtG(s.sd)   ?? '',
      fmtG(s.min)  ?? '',
      fmtG(s.max)  ?? '',
    ];
  });

  // Stub width = max(var-name length, "Variable"); data widths = max(header, values).
  const stubW = Math.max(stubHdr.length, ...vars.map(v => v.length));
  const dataW = dataHdrs.map((h, i) =>
    Math.max(h.length, ...dataRows.map(r => r[i].length))
  );

  // Header: right-aligned stub, ` │`, then each data header right-aligned (3-space sep).
  const hdr  = padL(stubHdr, stubW) + ' │' +
    dataHdrs.map((h, i) => padL(h, dataW[i] + 3)).join('');
  // Rule: stubW+1 dashes (align ┼ with the │ after the 1-space gap) + ┼ + data region.
  const dataRegionW = dataW.reduce((a, w) => a + w + 3, 0);
  const rule = '─'.repeat(stubW + 1) + '┼' + '─'.repeat(dataRegionW);

  const lines = [hdr, rule];
  // BUG-333: `summarize, separator(#)` draws a rule every # variables. The key is
  // present only when the user asked for it, so the default rendering is
  // untouched; Stata's default is 5 and 0 means "no rules at all".
  const sep = (block.separator === undefined || block.separator === null)
    ? 5 : Number(block.separator);
  for (let r = 0; r < vars.length; r++) {
    if (sep > 0 && r > 0 && r % sep === 0) lines.push(rule);
    const row = padL(vars[r], stubW) + ' │' +
      dataRows[r].map((v, i) => padL(v, dataW[i] + 3)).join('');
    lines.push(row);
  }
  return lines.join('\n');
}

// ─── Tabulate ────────────────────────────────────────────────────────────────

/** Format a one-way frequency table block (vars.length === 1). */
function formatTabulateOneway(vars, table, chi2) {
  const lines = [];
  const [v] = vars;
  const values = table.values;
  const total = table.margins?.total ?? Object.values(values).reduce((a, b) => a + b, 0);
  const keys = Object.keys(values);
  const keyW = Math.max(v.length, ...keys.map(k => k.length));
  const numW = Math.max(5, String(total).length);

  lines.push(padR(v, keyW) + ' │ ' + padL('Freq.', numW) + '   Percent   Cum.');
  lines.push('─'.repeat(keyW) + '─┼─' + '─'.repeat(numW + 22));

  let cum = 0;
  for (const k of keys) {
    const n = values[k];
    const pct = total > 0 ? (n / total * 100) : 0;
    cum += pct;
    lines.push(
      padL(k, keyW) + ' │ ' +
      padL(String(n), numW) + '   ' +
      padL(pct.toFixed(2), 7) + '   ' +
      padL(cum.toFixed(2), 7)
    );
  }
  lines.push('─'.repeat(keyW) + '─┼─' + '─'.repeat(numW + 22));
  lines.push(padL('Total', keyW) + ' │ ' + padL(String(total), numW) + '   100.00');

  if (chi2) {
    lines.push(`χ²(${chi2.df}) = ${fmtG(chi2.statistic) ?? '?'}   Pr = ${fmtP(chi2.p_value)}`);
  }
  return lines.join('\n');
}

/**
 * BUG-1514: the statistic lines Stata prints under a two-way table, in its layout
 * (StataBE 19.5, `tabulate c d, chi2 lrchi2 V gamma taub`): a blank line, then the
 * label right-aligned in 25 columns, ` = `, the statistic as %8.4f, and `   Pr = `
 * or `  ASE = ` with %5.3f; Fisher's exact p (2 x 2 tables) ends in the Pr column.
 *           Pearson chi2(6) =   1.4977   Pr = 0.960
 *  Likelihood-ratio chi2(6) =   1.5348   Pr = 0.957
 *                Cramér's V =   0.1117
 *                     gamma =  -0.0311  ASE = 0.175
 *           Kendall's tau-b =  -0.0208  ASE = 0.117
 *            Fisher's exact =                 0.800
 *    1-sided Fisher's exact =                 0.486
 */
function formatTabulateStats(block) {
  const lab = s => s.padStart(25);
  const f4 = x => Number(x).toFixed(4).padStart(8);
  const f3 = x => Number(x).toFixed(3);
  const lines = [];
  const { chi2, lrchi2, cramers_v: v, gamma, taub } = block;
  if (chi2) lines.push(`${lab(`Pearson chi2(${chi2.df})`)} = ${f4(chi2.statistic)}   Pr = ${f3(chi2.p_value)}`);
  if (lrchi2) lines.push(`${lab(`Likelihood-ratio chi2(${lrchi2.df})`)} = ${f4(lrchi2.statistic)}   Pr = ${f3(lrchi2.p_value)}`);
  if (v != null) lines.push(`${lab('Cramér\'s V')} = ${f4(v)}`);
  if (gamma) lines.push(`${lab('gamma')} = ${f4(gamma.value)}  ASE = ${f3(gamma.ase)}`);
  if (taub) lines.push(`${lab('Kendall\'s tau-b')} = ${f4(taub.value)}  ASE = ${f3(taub.ase)}`);
  if (block.exact) {
    lines.push(`${lab('Fisher\'s exact')} = ${' '.repeat(16)}${f3(block.exact.p)}`);
    if (block.exact.p1 != null) lines.push(`${lab('1-sided Fisher\'s exact')} = ${' '.repeat(16)}${f3(block.exact.p1)}`);
  }
  return lines.length ? ['', ...lines] : [];
}

/** Format a two-way contingency table block (vars.length === 2). */
function formatTabulateTwoway(vars, table, block) {
  const lines = [];
  const [rowVar, colVar] = vars;
  const { rows, cols, values, margins } = table;
  const grandTotal = margins?.total ?? 0;

  // Compute column widths: each col cell is max(colLabel.length, max count width, total width)
  const colLabelW = cols.map(c => String(c).length);
  const colCountW = cols.map((c, ci) => {
    const maxCount = Math.max(
      ...rows.map(r => String(values[r]?.[c] ?? 0).length),
      String(margins?.col?.[c] ?? 0).length,
      colLabelW[ci]
    );
    return Math.max(5, maxCount);
  });
  const totalColW = Math.max(5, String(grandTotal).length, 'Total'.length);

  // Row stub width: max of rowVar name and row labels
  const stubW = Math.max(
    rowVar.length,
    ...rows.map(r => String(r).length),
    'Total'.length
  );

  // Banner line: colVar name centered above the column block
  const colBlockW = colCountW.reduce((a, w) => a + w, 0) + (colCountW.length - 1) * 3;
  const totalBlockW = colBlockW + 3 + totalColW;
  const bannerPad = Math.max(0, Math.floor((totalBlockW - colVar.length) / 2));
  lines.push(' '.repeat(stubW + 3) + ' '.repeat(bannerPad) + colVar);

  // Header row: stub col | col1   col2   ...   Total
  const hdrCells = cols.map((c, ci) => padL(String(c), colCountW[ci]));
  hdrCells.push(padL('Total', totalColW));
  lines.push(padR(rowVar, stubW) + ' │ ' + hdrCells.join('   '));

  // Separator rule
  const ruleRight = colCountW.map(w => '─'.repeat(w)).join('───') + '───' + '─'.repeat(totalColW);
  lines.push('─'.repeat(stubW) + '─┼─' + ruleRight);

  // Data rows
  for (const r of rows) {
    const rowTotal = margins?.row?.[r] ?? 0;
    const cells = cols.map((c, ci) => padL(String(values[r]?.[c] ?? 0), colCountW[ci]));
    cells.push(padL(String(rowTotal), totalColW));
    lines.push(padL(String(r), stubW) + ' │ ' + cells.join('   '));
  }

  // Total row
  lines.push('─'.repeat(stubW) + '─┼─' + ruleRight);
  const totCells = cols.map((c, ci) => padL(String(margins?.col?.[c] ?? 0), colCountW[ci]));
  totCells.push(padL(String(grandTotal), totalColW));
  lines.push(padL('Total', stubW) + ' │ ' + totCells.join('   '));

  lines.push(...formatTabulateStats(block));
  return lines.join('\n');
}

// WS1 #5: Stata's `tabulate, sum()` mean/sd cells use ~8 significant digits
// (%10.0g) — fmtG's 7-sig-fig cap drops the 8th digit. Local %.8g formatter.
function fmtG8(x) {
  if (x === null || x === undefined || !Number.isFinite(x)) return '';
  if (x === 0) return '0';
  let s = Number(x).toPrecision(8);
  if (s.includes('.')) s = s.replace(/0+$/, '').replace(/\.$/, '');
  return s.replace(/^(-?)0\./, '$1.');   // Stata: drop leading zero (.5 not 0.5)
}

/** Format a tabulate VAR, sum(SUMVAR) block (mode:"summarize"). */
function formatTabulateSummarize(block) {
  const { var: v, sumvar: s, rows, overall } = block;
  const lines = [];

  const colHdrs = [v ?? 'Category', 'Mean', 'Std. dev.', 'Freq.'];
  const fmtRows = (rows ?? []).map(r => [
    String(r.level),
    fmtG8(r.mean),
    fmtG8(r.sd),
    String(r.N),
  ]);
  if (overall) {
    fmtRows.push(['Total', fmtG8(overall.mean), fmtG8(overall.sd), String(overall.N)]);
  }

  const widths = colHdrs.map((h, i) =>
    Math.max(h.length, ...fmtRows.map(r => r[i].length))
  );

  const hdr  = colHdrs.map((h, i) => (i === 0 ? padR(h, widths[i]) : padL(h, widths[i]))).join('   ');
  const rule = widths.map(w => '─'.repeat(w)).join('───');

  lines.push(hdr, rule);
  for (let i = 0; i < fmtRows.length; i++) {
    const row = fmtRows[i];
    // Insert rule before the Total row
    if (overall && i === fmtRows.length - 1) lines.push(rule);
    lines.push(row.map((v, ci) => (ci === 0 ? padR(v, widths[ci]) : padL(v, widths[ci]))).join('   '));
  }
  lines.push(rule);
  return lines.join('\n');
}

export function formatTabulate(block) {
  // A1: summarize mode — tabulate VAR, sum(SUMVAR)
  if (block.mode === 'summarize') {
    return formatTabulateSummarize(block);
  }

  // A2: multi-table modes (tab1 / tab2) — delegate to per-subtable formatters
  if (block.mode === 'oneway_multi' || block.mode === 'pairwise') {
    return (block.tables ?? []).map(t => formatTabulate(t)).join('\n\n');
  }

  const { vars, table, chi2 } = block;
  if (!table) return '(no tabulation)';

  if (vars.length === 1) {
    return formatTabulateOneway(vars, table, chi2);
  } else {
    // A5: two-way contingency table (was a literal-string stub)
    return formatTabulateTwoway(vars, table, block);
  }
}

// ─── Describe ────────────────────────────────────────────────────────────────

export function formatDescribe(block) {
  const { variables } = block;
  if (!variables || variables.length === 0) return '(no variables)';

  // BUG-371: `codebook, compact`. The block is the same one plain codebook emits
  // (this arm has always carried obs/unique/mean/min/max/label per variable) —
  // `compact` only chooses which of those get printed, which is why it is a flag
  // on the existing block rather than a new type. A consumer that ignores the
  // flag still gets the describe layout, so this is additive.
  if (block.compact) {
    const num = v => (v === undefined || v === null || Number.isNaN(v)) ? '.' : fmtG(v);
    const nameW = Math.max(8, ...variables.map(v => v.name.length));
    const obsW = Math.max(4, ...variables.map(v => String(v.n_obs ?? '').length));
    const uniqW = Math.max(6, ...variables.map(v => String(v.n_unique ?? '').length));
    const meanW = Math.max(8, ...variables.map(v => num(v.mean).length));
    const minW = Math.max(5, ...variables.map(v => num(v.min).length));
    const maxW = Math.max(5, ...variables.map(v => num(v.max).length));
    const sep = '  ';
    const hdr = [padR('Variable', nameW), padL('Obs', obsW), padL('Unique', uniqW),
                 padL('Mean', meanW), padL('Min', minW), padL('Max', maxW)].join(sep)
                + sep + 'Label';
    const rule = '─'.repeat(hdr.length);
    const rows = variables.map(v => [
      padR(v.name, nameW),
      padL(String(v.n_obs ?? ''), obsW),
      padL(String(v.n_unique ?? ''), uniqW),
      padL(num(v.mean), meanW),
      padL(num(v.min), minW),
      padL(num(v.max), maxW),
    ].join(sep) + sep + (v.label || ''));
    return [hdr, rule, ...rows].join('\n');
  }

  // Stata BE 19.5 describe: two-row header.
  //   row 1:  Variable      Storage   Display    Value
  //   row 2:      name         type    format    label      Variable label
  // PIET fields: name, dtype (Storage type), format (Display format),
  //   value label (PIET tracks none → blank), label (Variable label).
  const nameW  = Math.max(13, ...variables.map(v => v.name.length));   // "Variable"/"name" col
  const typeW  = Math.max(7,  'Storage'.length, ...variables.map(v => v.dtype.length));
  const fmtW   = Math.max(7,  'Display'.length, ...variables.map(v => (v.format || '').length));
  const vlblW  = Math.max(5,  'Value'.length);                          // value-label col (blank data)
  const sep    = '   ';

  // Header row 1: top words right-aligned over their columns.
  const hdr1 = [
    padR('Variable', nameW),
    padL('Storage',  typeW),
    padL('Display',  fmtW),
    padL('Value',    vlblW),
  ].join(sep);
  // Header row 2: sub-words ("name" right-aligned under Variable) + the wide Variable-label header.
  const hdr2 = [
    padL('name',  nameW),
    padL('type',  typeW),
    padL('format', fmtW),
    padL('label', vlblW),
  ].join(sep) + sep + 'Variable label';

  const totalW = nameW + typeW + fmtW + vlblW + sep.length * 3 + sep.length + 'Variable label'.length;
  const rule = '─'.repeat(totalW);

  const rows = variables.map(v => [
    padR(v.name,         nameW),
    padL(v.dtype,        typeW),
    padL(v.format || '', fmtW),
    padL('',             vlblW),   // value label (none tracked)
  ].join(sep) + sep + (v.label || ''));

  return [hdr1, hdr2, rule, ...rows].join('\n');
}

// ─── Estimates Table (esttab) ─────────────────────────────────────────────────

/**
 * Format an EstimatesTableResult as a monospace multi-column table.
 * Layout:
 *   col 0  = coef name (left-aligned)
 *   col 1…N = model columns (right-aligned within each)
 * Each coef has two lines: coefficient (with stars) and SE/t in parentheses.
 * Footer: N and R² rows.
 */
function formatEstimatesTable(block) {
  const { names = [], coef_rows = [], footers = [], options = {} } = block;
  const b_fmt = options.b_fmt || '%9.3f';
  const show_se = options.se !== false;
  const nostar = !!options.nostar;

  // ── Format a numeric value with Stata-style %fmt ─────────────────────────
  function fmtVal(v, fmt) {
    if (v === null || v === undefined) return '';
    const m = fmt.match(/%[-]?(?:\d+)?\.(\d+)([fg])/);
    if (m) {
      const prec = parseInt(m[1], 10);
      const kind = m[2];
      if (kind === 'f') return v.toFixed(prec);
      if (kind === 'g') return v.toPrecision(prec).replace(/\.?0+$/, '');
    }
    return v.toFixed(3);
  }

  // ── Stars ─────────────────────────────────────────────────────────────────
  function stars(cell) {
    if (nostar || !cell) return '';
    return cell.stars || '';
  }

  // ── Pre-format all cells ──────────────────────────────────────────────────
  // For each coef_row, produce {b_str, paren_str} per model.
  const formatted = coef_rows.map(cr => cr.cells.map(cell => {
    if (!cell) return { b: '', p: '' };
    if (cell.omitted) return { b: '(omitted)', p: '' };
    const bVal = cell.b;
    const parenVal = show_se ? cell.se : cell.t;
    const bStr = bVal !== null ? fmtVal(bVal, b_fmt) + stars(cell) : '';
    const pStr = parenVal !== null ? `(${fmtVal(parenVal, b_fmt)})` : '';
    return { b: bStr, p: pStr };
  }));

  // ── Column widths ─────────────────────────────────────────────────────────
  const nameW = Math.max(8, ...coef_rows.map(cr => cr.name.length));
  const colWs = names.map((name, mi) => {
    let w = name.length;
    for (const row of formatted) {
      w = Math.max(w, row[mi].b.length, row[mi].p.length);
    }
    for (const f of footers) {
      const v = f.values[mi] || '';
      w = Math.max(w, v.length, f.label.length);
    }
    return Math.max(w, 9);
  });

  const sep = '─';

  // ── Header row ────────────────────────────────────────────────────────────
  const hdrCells = names.map((name, i) => padL(name, colWs[i]));
  const hdr = padR('', nameW) + '  ' + hdrCells.join('  ');

  // ── Rules ─────────────────────────────────────────────────────────────────
  const ruleTop = sep.repeat(nameW) + '─┬─' + colWs.map(w => sep.repeat(w)).join('─┬─');
  const ruleBot = sep.repeat(nameW) + '─┴─' + colWs.map(w => sep.repeat(w)).join('─┴─');

  // ── Data rows ─────────────────────────────────────────────────────────────
  const dataLines = [];
  coef_rows.forEach((cr, ri) => {
    // coefficient line
    const bCells = formatted[ri].map((c, mi) => padL(c.b, colWs[mi]));
    dataLines.push(padR(cr.name, nameW) + ' │ ' + bCells.join(' │ '));
    // parenthesised line
    const pCells = formatted[ri].map((c, mi) => padL(c.p, colWs[mi]));
    dataLines.push(padR('', nameW) + ' │ ' + pCells.join(' │ '));
  });

  // ── Footer rows ───────────────────────────────────────────────────────────
  const footLines = footers.map(f => {
    const vals = names.map((_, mi) => padL(f.values[mi] || '', colWs[mi]));
    return padR(f.label, nameW) + '  ' + vals.join('  ');
  });

  const lines = [
    hdr,
    ruleTop,
    ...dataLines,
    ruleBot,
    ...footLines,
  ];
  return lines.join('\n');
}

export { formatEstimatesTable };
