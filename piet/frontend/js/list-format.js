// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// list-format.js — Stata's `list` layout ([D] list) for the `listing` object the
// native list / duplicates list handlers attach to their browse block
// (descriptive/data_listing.cpp buildListing). The native side carries every cell
// as Stata displays it (display format, value label); this file decides the
// widths, the table-or-display layout and draws the boxes, separators, headers
// and the mean/sum/N lines.
//
// The rules below are the ones the 2026-11-10 StataBE 19.5 truth bank's list
// output shows (vs-stata stage 4): a column is as wide as its widest cell, its
// header abbreviated (name~x) to no fewer than 8 columns (5 with compress or
// abbreviate(#)); columns are three blanks apart (` | ` with divider); the
// observation numbers are right-aligned in max(3, digits) + 1 columns (widened
// for a Mean/Sum/N label); a separator every separator(#) rows, or wherever a
// sepby() value changes; a table wider than linesize() is drawn one box per
// observation (display layout), its cells packed into lines and each line
// stretched to the widest one.

// Display columns of one character: East Asian wide forms take two (Stata's
// "display columns", [D] list abbreviate()), combining marks none.
function charWidth(cp) {
  if (cp === 0) return 0;
  if ((cp >= 0x0300 && cp <= 0x036f) || (cp >= 0x200b && cp <= 0x200f)) return 0;
  if ((cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf && cp !== 0x303f) ||
      (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
      (cp >= 0xfe30 && cp <= 0xfe4f) || (cp >= 0xff00 && cp <= 0xff60) ||
      (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x20000 && cp <= 0x3fffd)) return 2;
  return 1;
}
export function displayWidth(s) {
  let w = 0;
  for (const ch of String(s)) w += charWidth(ch.codePointAt(0));
  return w;
}
// The longest head of `s` no wider than `w` columns.
function headCols(s, w) {
  let out = '', used = 0;
  for (const ch of String(s)) {
    const cw = charWidth(ch.codePointAt(0));
    if (used + cw > w) break;
    out += ch;
    used += cw;
  }
  return out;
}

function padR(s, w) { const d = displayWidth(s); return d >= w ? s : s + ' '.repeat(w - d); }
function padL(s, w) { const d = displayWidth(s); return d >= w ? s : ' '.repeat(w - d) + s; }

// Stata's abbreviation keeps a name's trailing digits whole after the `~`
// (fatal1517 -> fat~1517, ttl_exp68 -> ttl_e~68), else its last character
// (displacement -> displa~t).
export function abbrevName(name, w) {
  if (displayWidth(name) <= w) return name;
  if (w < 3) return headCols(name, w);
  const chars = [...name];
  const digits = /\d+$/.exec(name);
  let tail = chars.slice(-1)[0];
  if (digits && digits[0].length < chars.length && digits[0].length <= w - 2) tail = digits[0];
  return headCols(name, w - 1 - displayWidth(tail)) + '~' + tail;
}

function truncDots(s, w) {
  if (w < 1 || displayWidth(s) <= w) return s;
  return w <= 2 ? headCols(s, w) : headCols(s, w - 2) + '..';
}

// One cell's text as list shows it: a string's trailing blanks dropped (unless
// notrim); leading blanks are kept (bank 52398c9fefaaac26 `"  "+make`).
function cellText(col, raw, o) {
  let s = raw == null ? '' : String(raw);
  if (col.str && !o.notrim) s = s.replace(/\s+$/, '');
  return s;
}

// A numeric cell that shows a value label is text too (string(#) cuts it).
const isText = (s) => !/^-?[\d.,]*\d[\d.,]*(e[+-]\d+)?$|^\.[a-z]?$/.test(s);

function align(col, s, w) { return col.left ? padR(s, w) : padL(s, w); }

export function formatListing(L) {
  if (!L || !Array.isArray(L.cols)) return '';
  const o = L.opts || {};
  const out = [];
  // duplicates list / examples: "Duplicates in terms of ..." above the table.
  if (L.title) out.push(String(L.title));
  const constant = Array.isArray(L.constant) ? L.constant : [];
  if (constant.length) {
    out.push(...drawTable({
      cols: constant, rows: [constant.map(c => c.cell)], obs: [], breaks: [], stats: [],
      labvar: -1, omitted: 0,
      opts: Object.assign({}, o, { noobs: true, separator: 0, sepby: false }),
    }));
  }
  if (L.cols.length) out.push(...drawTable(L));
  if (L.omitted > 0) {
    out.push('');
    out.push(`  (${L.omitted.toLocaleString('en-US')} more observations not listed here; ` +
             'use `list in` or `browse` to see them)');
  }
  return out.join('\n');
}

function drawTable(L) {
  // notrim draws the table without its rules, as clean does (bank 7d730af2f8a2e70f).
  const o = Object.assign({}, L.opts || {});
  if (o.notrim) o.clean = true;
  const cols = L.cols;
  const nc = cols.length;
  const rows = (L.rows || []).map(r => r.map((v, j) => cellText(cols[j], v, o)));
  const stats = Array.isArray(L.stats) ? L.stats : [];
  const labvar = Number.isInteger(L.labvar) ? L.labvar : -1;

  // ---- column widths -------------------------------------------------------
  const abMin = o.abbreviate > 0 ? Math.max(o.abbreviate, 5) : (o.compress ? 5 : 8);
  const widths = cols.map((c, j) => {
    let content = 0;
    if (o.nocompress && c.fmtw > 0) content = c.fmtw;
    for (const r of rows) content = Math.max(content, displayWidth(r[j]));
    if (j !== labvar)
      for (const s of stats) if (s.cells && s.cells[j] != null) content = Math.max(content, displayWidth(s.cells[j]));
    if (j === labvar) for (const s of stats) content = Math.max(content, s.label.length);
    const head = String(c.head ?? c.name);
    // A characteristic header (subvarname) is cut to the cells under compress
    // (duplicates list, compress: `G`, `Ob`; bank 57e8e1ffaa08d2b5).
    if (c.charhead && o.compress) return Math.max(content, 1);
    // string(#): a text column is # + 2 wide at most; a longer cell is cut to
    // the width less two and "..".
    if (o.string > 0 && (c.str || rows.some(r => isText(r[j])))) content = Math.min(content, o.string + 2);
    return Math.max(content, Math.min(displayWidth(head), abMin), 1);
  });
  if (o.string > 0) {
    cols.forEach((c, j) => {
      for (const r of rows) if ((c.str || isText(r[j])) && displayWidth(r[j]) > widths[j]) r[j] = truncDots(r[j], widths[j]);
    });
  }
  const heads = cols.map((c, j) => (c.charhead ? headCols(String(c.head ?? c.name), widths[j])
                                                : abbrevName(String(c.head ?? c.name), widths[j])));

  // ---- the observation-number column --------------------------------------
  const showObs = !o.noobs;
  let labW = 0;
  if (showObs) {
    let digits = 1;
    for (const n of (L.obs || [])) digits = Math.max(digits, String(n).length);
    labW = Math.max(3, digits) + 1;
  }
  // A Mean/Sum/N line widens the label column to five ("Mean" and a blank),
  // whichever lines are asked for (bank 4c98235d4047ff9c `sum` alone).
  if (labvar < 0 && stats.length) labW = Math.max(labW, 5);
  const P = showObs || (stats.length && labvar < 0) ? labW + 1 : 2;
  const lead = ' '.repeat(P);
  const label = (s) => padL(s, P - 1) + ' ';

  const gap = o.divider && !o.clean ? ' | ' : '   ';
  const naturalInner = widths.reduce((a, b) => a + b, 0) + 3 * (nc - 1) + 2;
  const linesize = o.linesize > 0 ? o.linesize : 80;
  const tooWide = P + naturalInner + 2 > linesize;
  const useDisplay = !o.clean && !o.table && (o.display || (tooWide && nc > 1));
  if (useDisplay) return drawDisplay(L, rows, cols, widths, heads, P, linesize, o);

  // One column wider than the line: cut it to fit, with "..".
  if (tooWide && nc === 1 && !o.table) {
    const avail = Math.max(1, linesize - P - 4);
    if (widths[0] > avail) {
      widths[0] = avail;
      cols[0] = Object.assign({}, cols[0], { left: true });   // a cut column is drawn left-aligned
      for (const r of rows) r[0] = truncDots(r[0], avail);
      heads[0] = abbrevName(String(cols[0].head ?? cols[0].name), avail);
    }
  }
  const inner = widths.reduce((a, b) => a + b, 0) + 3 * (nc - 1) + 2;
  const bar = o.clean ? ' ' : '|';
  const line = (cells) => bar + ' ' + cells.map((s, j) => align(cols[j], s, widths[j])).join(gap) + ' ' + (o.clean ? ' ' : '|');
  const topRule = lead + '+' + '-'.repeat(inner) + '+';
  const midRule = o.divider
    ? lead + '|' + widths.map(w => '-'.repeat(w + 2)).join('+') + '|'
    : lead + '|' + '-'.repeat(inner) + '|';
  const headLine = lead + line(heads.map((h, j) => h));
  const showHeader = !o.noheader;

  const out = [];
  if (showHeader) out.push('');
  const openBox = () => {
    if (!o.clean) out.push(topRule);
    if (showHeader) {
      out.push(o.clean ? headLine.replace(/\s+$/, m => m) : headLine);
      if (!o.clean) out.push(midRule);
    }
  };
  openBox();
  const breaks = new Set(Array.isArray(L.breaks) ? L.breaks : []);
  const every = o.header_every > 0 ? o.header_every : 0;
  const sep = o.sepby ? 0 : (o.separator > 0 ? o.separator : 0);
  let sinceHeader = 0, sinceSep = 0;
  for (let i = 0; i < rows.length; i++) {
    if (i > 0 && every > 0 && sinceHeader === every) {
      if (!o.clean) out.push(topRule);
      out.push('');
      openBox();
      sinceHeader = 0;
      sinceSep = 0;
    } else if (i > 0 && o.ds) {
      // ds: a blank line after every observation (at the sepby() breaks only, with
      // sepby), in place of the separator rules (bank f426200b9344eed6).
      if (!o.sepby || breaks.has(i)) out.push(o.clean ? '' : lead + '|' + ' '.repeat(inner) + '|');
      sinceSep = 0;
    } else if (i > 0 && !o.clean && ((sep > 0 && sinceSep === sep) || breaks.has(i))) {
      out.push(midRule);
      sinceSep = 0;
    }
    const lab = showObs ? label(String(L.obs[i]) + '.') : lead;
    out.push(lab + line(rows[i]));
    sinceHeader++;
    sinceSep++;
  }
  if (stats.length) {
    if (!o.clean) out.push(midRule);
    for (const s of stats) {
      const cells = cols.map((c, j) => (j === labvar ? s.label : (s.cells && s.cells[j] != null ? String(s.cells[j]) : '')));
      const lineCells = cells.map((v, j) => (j === labvar ? padL(v, widths[j]) : v));
      const lab = labvar < 0 ? label(s.label) : lead;
      out.push(lab + bar + ' ' + lineCells.map((v, j) => (j === labvar ? v : align(cols[j], v, widths[j]))).join(gap) +
               ' ' + (o.clean ? ' ' : '|'));
    }
  }
  if (o.footer && showHeader) {
    if (!o.clean) out.push(midRule);
    out.push(headLine);
  }
  if (!o.clean) out.push(topRule);
  return out;
}

// The display layout: one box per observation; the cells packed into lines no
// wider than the page, every line stretched (extra columns shared out evenly)
// to the widest one, cells separated by ` | `, headers above their values.
function drawDisplay(L, rows, cols, widths, heads, P, linesize, o) {
  const nc = cols.length;
  const avail = Math.max(1, linesize - P - 4);     // the widest cell content a line can take
  const w = widths.map(x => Math.min(x, avail));
  const groups = [];
  let cur = [], curW = 0;
  for (let j = 0; j < nc; j++) {
    const add = cur.length ? w[j] + 3 : w[j];
    if (cur.length && curW + add > avail) {
      groups.push(cur);
      cur = [];
      curW = 0;
    }
    cur.push(j);
    curW += cur.length > 1 ? w[j] + 3 : w[j];
  }
  if (cur.length) groups.push(cur);
  const natural = (g) => g.reduce((a, j) => a + w[j], 0) + 3 * (g.length - 1);
  const inner = Math.max(...groups.map(natural)) + 2;
  const lead = ' '.repeat(P);
  const rule = lead + '+' + '-'.repeat(inner) + '+';
  // The rule between two lines of cells has a `+` wherever a cell boundary of the
  // line above meets one of the line below.
  const stretched = (g) => {
    const extra = inner - 2 - natural(g);
    const each = Math.floor(extra / g.length), rem = extra - each * g.length;
    return g.map((j, k) => w[j] + each + (k >= g.length - rem ? 1 : 0));
  };
  const bounds = (g) => {
    const ws = stretched(g), out = new Set();
    let pos = 0;
    for (let k = 0; k < ws.length - 1; k++) { pos += ws[k] + 3; out.add(pos - 1); }
    return out;
  };
  const sepRuleBetween = (ga, gb) => {
    const a = bounds(ga), b = bounds(gb);
    let s = '';
    for (let x = 0; x < inner; x++) s += a.has(x) && b.has(x) ? '+' : '-';
    return lead + '|' + s + '|';
  };
  const lineOf = (g, texts) => {
    const extra = inner - 2 - natural(g);
    const each = Math.floor(extra / g.length);
    const rem = extra - each * g.length;   // one more each for the last `rem` cells
    const parts = g.map((j, k) => {
      const cut = widths[j] > w[j];
      const field = (cut ? padR : (cols[j].left ? padR : padL))(truncDots(texts[k], w[j]), w[j]);
      const add = each + (k >= g.length - rem ? 1 : 0);
      const left = Math.floor(add / 2), right = add - left;
      return ' '.repeat(left) + field + ' '.repeat(right);
    });
    return '| ' + parts.join(' | ') + ' |';
  };
  const out = [];
  for (let i = 0; i < rows.length; i++) {
    out.push('');
    out.push(rule);
    groups.forEach((g, gi) => {
      if (gi > 0) out.push(sepRuleBetween(groups[gi - 1], g));
      const lab = gi === 0 && !o.noobs ? padL(String(L.obs[i]) + '.', P - 1) + ' ' : lead;
      out.push(lab + lineOf(g, g.map(j => heads[j])));
      out.push(lead + lineOf(g, g.map(j => rows[i][j])));
    });
    out.push(rule);
  }
  return out;
}
