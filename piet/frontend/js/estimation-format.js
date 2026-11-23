// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// estimation-format.js — EstimationResult block -> Stata-style monospace text (coefficient table, header stats, footers).
// formatEstimation() is the critical path — verified against piet/fixtures/golden/*.json.

import { fmtG, fmtT, fmtP, fmtObs, padL, padR } from './util.js';

// ─── THE CRITICAL PATH: formatEstimation() ───────────────────────────────────
// Verified against piet/fixtures/golden/{feols_plain,feols_robust,feols_cluster,
//                                         feols_factor,feols_fe,feols_multi}.json

/**
 * Given an EstimationResult (CONTRACT.md shape), return a multi-line monospace
 * string that looks like Stata's regress/feols output.
 */
export function formatEstimation(block) {
  const { estimator, outcomes } = block;

  // BUG-015: `test` (post-estimation Wald) has NO coeftable — render the
  // constraint list + test statistic straight from the block fields, before
  // the coeftable path turns it into "(no results)".
  if (estimator === 'test') {
    return formatTestBlock(block);
  }

  // BS-026 (BETA Phase C batch C1 estimation_m4.cpp:161-179): `tabstat` has NO
  // coeftable — it carries `vars`/`stats`/`total` (+ optional `by`) instead.
  // Render straight from those fields, before the coeftable path turns it
  // into "(no results)".
  if (estimator === 'tabstat') {
    return formatTabstatBlock(block);
  }

  // A19: `estat vif`/`estat hettest`/`estat ovtest` have NO Coefficient/Std.
  // err./t/P>|t| coeftable in the Stata sense — vif's coeftable rows carry
  // `b` only (se/t/p/ci null), and hettest/ovtest carry no coeftable at all.
  // Render each from its own dedicated fields before the coeftable path
  // below turns them into a mislabeled shell or "(no results)".
  if (estimator === 'vif') {
    return formatVifBlock(block);
  }
  if (estimator === 'hettest') {
    return formatHettestBlock(block);
  }
  if (estimator === 'ovtest') {
    return formatOvtestBlock(block);
  }
  // vs-stata stage 4: hausman in Stata's own layout (column titles from the
  // stored names or tconsistent()/tefficient(), the fits' commands in the
  // footer). Blocks without the titles keep the generic table.
  if (estimator === 'hausman' && block.b_title !== undefined) {
    return formatHausmanBlock(block);
  }
  // vs-stata stage 4: ttesti in Stata's own one-/two-sample table.
  if (estimator === 'ttest' && block.ttest_table) {
    return formatTtestTable(block.ttest_table);
  }

  // teffects ra: the outcome-model equations (OME<level>) are printed only under
  // aequations (StataBE 19.5, vs-stata bank 2b9789b5); the block carries every column
  if (estimator === 'teffects' && block.subcmd === 'ra' && !block.aequations && Array.isArray(block.coeftable)) {
    return formatOneTable({ ...block, coeftable: block.coeftable.filter(r => !String(r.name).startsWith('OME')) });
  }

  // oaxaca: the decomposition's header, definitions, equation-sectioned table
  if (estimator === 'oaxaca') {
    return formatOaxacaBlock(block);
  }

  // Multi-equation: render each outcome sub-table
  if (outcomes && outcomes.length > 0) {
    const tables = outcomes.map((oc, i) => {
      const sub = {
        estimator,
        depvar:     oc.depvar,
        n_obs:      oc.n_obs    || block.n_obs,
        coeftable:  oc.coeftable,
        r2:         oc.r2,
        r2_within:  oc.r2_within,
        vcov_type:  oc.vcov_type || block.vcov_type,
        n_clusters: oc.n_clusters != null ? oc.n_clusters : block.n_clusters,
        dof_resid:  oc.dof_resid,
        absorbed:   {},
        sigma2:     oc.sigma2,
      };
      const label = `── ${oc.depvar} `;
      return label + '\n' + formatOneTable(sub);
    }).join('\n\n');
    // constraints(): the constraint lines Stata prints above the table
    // (`( 1)  [Black]o.age = 0`, mlogit ..., constraints(1)).
    const cns = Array.isArray(block.constraint_lines) ? block.constraint_lines : [];
    return cns.length ? cns.map(l => ' ' + l).join('\n') + '\n\n' + tables : tables;
  }

  if (!block.coeftable || !block.coeftable.length) return formatGenericBlock(block);
  return formatOneTable(block);
}

/**
 * BUG-015: render a `test` (post-estimation Wald) block. It carries no
 * coeftable — only the joint-restriction fields. Mirrors Stata's `test`:
 *   ( 1)  mpg = 0
 *   ( 2)  weight = 0
 *
 *        F(  2,    71) =   14.74
 *             Prob > F =   0.0000
 * iid vcov → F form (F / Prob > F); robust/cluster vcov → chi2 form
 * (chi2 / Prob > chi2) — same OLS-vs-chi2 split used for the model-fit header.
 */
function formatTestBlock(block) {
  const lines = [];

  const restrictions = block.restrictions || [];
  restrictions.forEach((r, i) => {
    lines.push(`( ${padL(String(i + 1), 1)})  ${r}`);
  });

  // test, notest: the constraints alone, nothing tested (StataBE 19.5, bank 37fd4049).
  if (block.notest) return lines.join('\n');

  if (restrictions.length) lines.push('');

  // BUG-1057: a statistic the engine could not form (R V R' exactly 0 after an
  // exact fit) arrives null, which Stata prints as `.` (StataBE 19.5 `test x` after
  // `regress y2 x` on y2 = 3 + 2*x: `F(  1,    48) =       .`, `Prob > F =
  // .`); Number(null) printed 0.00 and fmtP(null) nothing.
  const missing = (x) => x == null || !Number.isFinite(Number(x));
  const stat = (x) => (missing(x) ? '.' : Number(x).toFixed(2));
  const prob = (x) => (missing(x) ? '.' : fmtP(Number(x)));

  // test, mtest(bonferroni|holm|sidak): Stata's table of each constraint
  // tested alone (its adjusted p starred) above the joint test (StataBE 19.5,
  // vs-stata bank 336b8488):
  //   --------------------------------------
  //          |    F(df,70)     df      p > F
  //   -------+------------------------------
  //      (1) |        0.09      1     1.0000*
  //   -------+------------------------------
  //      All |       34.77      2     0.0000
  //   --------------------------------------
  //   * Bonferroni-adjusted p-values
  if (Array.isArray(block.mtest) && block.mtest.length) {
    const p4 = (x) => (missing(x) ? '.' : Number(x).toFixed(4));
    const rule = '-'.repeat(38);
    const mid = '-------+------------------------------';
    lines.push(rule);
    lines.push('       |' + padL(`F(df,${Math.round(block.df2)})`, 12) + padL('df', 7) + padL('p > F', 11));
    lines.push(mid);
    block.mtest.forEach((row, i) => {
      lines.push(padL(`(${i + 1})`, 6) + ' |' + padL(stat(row.F), 12) + padL(String(Math.round(row.df)), 7) +
                 padL(p4(row.adjustp), 11) + '*');
    });
    lines.push(mid);
    lines.push('   All |' + padL(stat(block.F), 12) + padL(String(block.df1), 7) + padL(p4(block.p_F), 11));
    lines.push(rule);
    const method = { bonferroni: 'Bonferroni', holm: 'Holm', sidak: 'Sidak' }[block.mtmethod] || block.mtmethod;
    lines.push(`* ${method}-adjusted p-values`);
    return lines.join('\n');
  }

  // iid → F statistic; robust/cluster → Wald chi2 (matches buildStats footer).
  // test/testparm df(#) replaces the chi2 by F(q, #) after any fit (the block
  // then carries F, df2 and p_F and no chi2; bank 3b1dd274 after logit: r(F)
  // 8.890745, r(df_r) 30), so the chi2 form is drawn only when a chi2 is there.
  if (block.vcov_type !== 'iid' && Object.prototype.hasOwnProperty.call(block, 'chi2')) {
    lines.push(`chi2(${block.df1}) = ${stat(block.chi2)}`);
    lines.push(`Prob > chi2 = ${prob(block.p_chi2)}`);
  } else {
    lines.push(`F(${block.df1}, ${Math.round(block.df2)}) = ${stat(block.F)}`);
    lines.push(`Prob > F = ${prob(block.p_F)}`);
  }

  return lines.join('\n');
}

/**
 * BS-026 (BETA Phase C batch C1): render a `tabstat` block — one row per
 * `vars[]` entry, one column per `stats[]` entry, values from `total`
 * (`{var:{stat:value}}`). Mirrors formatSummarize()'s stub+data-column layout.
 * Numbers go through fmtG (the same %9.0g-ish general formatter every other
 * table in this file uses) so tabstat reads consistently with summarize/
 * regress output.
 *
 * `by(...)` support: the engine currently echoes back the by-var name but
 * does NOT (yet) compute per-group breakdowns — `total` is always the
 * grand-total table regardless of `by`. Rather than silently drop the
 * by() request or fabricate group rows, render the honest total table plus
 * a note that per-group rows are pending engine support.
 */
function formatTabstatBlock(block) {
  const { vars, stats, total, by } = block;
  if (!vars || !vars.length || !stats || !stats.length || !total) return '(no results)';

  const stubHdr = 'Variable';
  const dataHdrs = stats;

  const dataRows = vars.map(v => {
    const row = total[v] || {};
    return stats.map(s => fmtG(row[s]) ?? '');
  });

  const stubW = Math.max(stubHdr.length, ...vars.map(v => v.length));
  const dataW = dataHdrs.map((h, i) =>
    Math.max(h.length, ...dataRows.map(r => r[i].length))
  );

  const hdr = padL(stubHdr, stubW) + ' │' +
    dataHdrs.map((h, i) => padL(h, dataW[i] + 3)).join('');
  const dataRegionW = dataW.reduce((a, w) => a + w + 3, 0);
  const rule = '─'.repeat(stubW + 1) + '┼' + '─'.repeat(dataRegionW);

  const lines = [hdr, rule];
  for (let r = 0; r < vars.length; r++) {
    lines.push(padL(vars[r], stubW) + ' │' +
      dataRows[r].map((v, i) => padL(v, dataW[i] + 3)).join(''));
  }

  // `total` never carries by-group rows today (see doc comment above) — the
  // check is written against that fact rather than assuming it forever, so
  // a future engine change that adds real per-group data isn't silently
  // masked by this note.
  const hasPerGroupData = by && Array.isArray(block.by_groups) && block.by_groups.length > 0;
  if (by && !hasPerGroupData) {
    lines.push('');
    lines.push(`(per-group breakdown by ${by} is engine-pending — showing overall totals only)`);
  }

  return lines.join('\n');
}

/**
 * A19: render `estat vif` as its own Variable/VIF/1-over-VIF table — NOT
 * the Coefficient/Std.err./t/P>|t| coeftable shell (vif's coeftable rows
 * carry `b` only; se/t/p/ci are null, which is what produced the
 * mislabeled empty-column render this fixes). Diagnosed native shape
 * (piet_native --do, 2026-07-16): the block carries VIF values twice — once
 * folded into `coeftable[].b`/`.name` (compat shape shared with other
 * estimators) and once in a clean `coefs: [{name, b}]` array. We read
 * `coefs` first and fall back to `coeftable` so either shape renders.
 * No native `tol` (1/VIF) field is emitted — computed client-side from `b`.
 * Row order is left exactly as the native block emits it (no client-side
 * re-sort) rather than guessing at a descending-VIF convention.
 */
function formatVifBlock(block) {
  const rows = (block.coefs || block.coeftable || [])
    .map(r => ({ name: r.name, vif: r.b }))
    .filter(r => r.vif != null);
  if (!rows.length) return '(no results)';

  const stubHdr = 'Variable', vifHdr = 'VIF', invHdr = '1/VIF';
  const fVif = rows.map(r => r.vif.toFixed(2));
  const fInv = rows.map(r => (1 / r.vif).toFixed(6));

  const stubW = Math.max(stubHdr.length, ...rows.map(r => r.name.length));
  const vifW  = Math.max(vifHdr.length, ...fVif.map(s => s.length));
  const invW  = Math.max(invHdr.length, ...fInv.map(s => s.length));

  const hdr  = padL(stubHdr, stubW) + ' │' + padL(vifHdr, vifW + 3) + padL(invHdr, invW + 3);
  const rule = '─'.repeat(stubW + 1) + '┼' + '─'.repeat(vifW + 3 + invW + 3);

  const lines = [hdr, rule];
  rows.forEach((r, i) => {
    lines.push(padL(r.name, stubW) + ' │' + padL(fVif[i], vifW + 3) + padL(fInv[i], invW + 3));
  });

  const meanVif = rows.reduce((a, r) => a + r.vif, 0) / rows.length;
  lines.push(rule);
  lines.push(padL('Mean VIF', stubW) + ' │' + padL(meanVif.toFixed(2), vifW + 3));

  return lines.join('\n');
}

/**
 * A19: `estat hettest` (Breusch-Pagan / Cook-Weisberg) has NO coeftable —
 * diagnosed native shape carries `chi2`/`df`/`p` (plus an unused `F`/`p_F`
 * pair the native block also emits; Stata's default hettest report is the
 * single chi2(df) statistic, so only that pair is surfaced here) straight
 * on the estimation block, same no-coeftable family as `test`
 * (formatTestBlock above).
 */
function formatHettestBlock(block) {
  const { chi2, df, p } = block;
  // hettest, fstat: the auxiliary regression's F (block F / df_m / df_r).
  if (chi2 == null && block.F != null) {
    return [
      'Breusch-Pagan / Cook-Weisberg test for heteroskedasticity',
      ...(block.assumption ? [`Assumption: ${block.assumption}`] : []),
      '    Ho: Constant variance',
      '',
      `F(${block.df_m}, ${Math.round(block.df_r)}) = ${Number(block.F).toFixed(2)}`,
      `Prob > F = ${Number(p).toFixed(4)}`,
    ].join('\n');
  }
  if (chi2 == null) return '(no results)';
  return [
    'Breusch-Pagan / Cook-Weisberg test for heteroskedasticity',
    ...(block.assumption ? [`Assumption: ${block.assumption}`] : []),
    '    Ho: Constant variance',
    '',
    `    chi2(${df}) = ${Number(chi2).toFixed(2)}`,
    `    Prob > chi2 = ${fmtP(p)}`,
  ].join('\n');
}

/**
 * A19: `estat ovtest` (Ramsey RESET) has NO coeftable — diagnosed native
 * shape carries `F`/`df_m`/`df_r`/`p` straight on the estimation block,
 * same no-coeftable family as `test`/`hettest`.
 */
function formatOvtestBlock(block) {
  const { F, df_m, df_r, p } = block;
  if (F == null) return '(no results)';
  return [
    'Ramsey RESET test using powers of the fitted values',
    '    Ho: model has no omitted variables',
    '',
    `    F(${df_m}, ${Math.round(df_r)}) = ${Number(F).toFixed(2)}`,
    `    Prob > F = ${fmtP(p)}`,
  ].join('\n');
}

/**
 * hausman: the coefficient-difference table and the chi2 test, laid out on
 * Stata's columns (titles centred in 12 at columns 17 and 30, values %9.0g at
 * columns 18, 31, 47 and 63, the footer right-aligned in 78).
 */
function formatHausmanBlock(block) {
  const at = (line, col, text) => {
    const s = line.length < col - 1 ? line + ' '.repeat(col - 1 - line.length) : line;
    return s + text;
  };
  const abbrev12 = (t) => {
    const s = String(t ?? '');
    if (s === '') return '.';
    return s.length <= 12 ? s : s.slice(0, 10) + '~' + s.slice(-1);
  };
  const centre12 = (t) => {
    const s = abbrev12(t);
    const left = Math.floor((12 - s.length) / 2);
    return ' '.repeat(Math.max(0, left)) + s + ' '.repeat(Math.max(0, 12 - s.length - left));
  };
  // %9.0g: below 1 in magnitude Stata keeps at most seven decimals (bank
  // 30280271c2143616: .060326952 prints .060327, .00078846 prints .0007885).
  const g9 = (x) => {
    if (x === null || x === undefined || !Number.isFinite(x)) return '.'.padStart(9);
    let s = fmtG(x) ?? '.';
    const dot = s.indexOf('.');
    if (Math.abs(x) < 1 && !s.includes('e') && dot >= 0 && s.length - dot - 1 > 7) {
      s = x.toFixed(7).replace(/0+$/, '').replace(/\.$/, '').replace(/^(-?)0\./, '$1.');
    }
    return s.padStart(9);
  };
  const ralign78 = (t) => t.padStart(78);
  const lines = [''];
  lines.push(at('', 18, '---- Coefficients ----'));
  let h = at('', 14, '|');
  h = at(h, 21, '(b)'); h = at(h, 34, '(B)'); h = at(h, 49, '(b-B)');
  lines.push(at(h, 59, 'sqrt(diag(V_b-V_B))'));
  let t = at('', 14, '|');
  t = at(t, 17, centre12(block.b_title)); t = at(t, 30, centre12(block.B_title));
  t = at(t, 46, 'Difference');
  lines.push(at(t, 63, 'Std. err.'));
  const rule = '-'.repeat(13) + '+' + '-'.repeat(64);
  lines.push(rule);
  let eqNow;
  (block.coefs || []).forEach((c, i) => {
    if (c.eq !== undefined && c.eq !== eqNow) {
      if (i > 0) lines.push(rule);
      lines.push(at(abbrev12(c.eq), 14, '|'));
      eqNow = c.eq;
    }
    let r = abbrev12(c.name).padStart(12) + ' |';
    r = at(r, 18, g9(c.b_fe)); r = at(r, 31, g9(c.b_re));
    r = at(r, 47, g9(c.diff));
    lines.push(at(r, 63, g9(c.se_diff)));
  });
  lines.push('-'.repeat(78));
  lines.push(ralign78(`b = Consistent under H0 and Ha; obtained from ${block.b_cmd ?? ''}.`));
  lines.push(ralign78(`B = Inconsistent under Ha, efficient under H0; obtained from ${block.B_cmd ?? ''}.`));
  lines.push('');
  lines.push('Test of H0: Difference in coefficients not systematic');
  lines.push('');
  const df = block.dof;
  const dfText = Number.isInteger(df) ? String(df) : (fmtG(df) ?? String(df));
  const chiLabel = `chi2(${dfText})`;
  const chi2 = Number(block.chi2);
  if (chi2 >= 0) {
    const len = Math.max(11, chiLabel.length);
    const a = chi2.toFixed(2);
    const b = block.p === null || block.p === undefined || !Number.isFinite(Number(block.p))
      ? '.' : Number(block.p).toFixed(4);
    const w = Math.max(a.length, b.length);
    lines.push(chiLabel.padStart(len) + " = (b-B)'[(V_b-V_B)^(-1)](b-B)");
    lines.push(''.padStart(len) + ' = ' + a.padStart(w));
    lines.push('Prob > chi2'.padStart(len) + ' = ' + b.padStart(w));
  } else {
    const len = chiLabel.length;
    lines.push(chiLabel.padStart(len) + " = (b-B)'[(V_b-V_B)^(-1)](b-B)");
    lines.push(' '.repeat(len) + ' = ' + chi2.toFixed(2));
  }
  for (const n of block.notes || []) lines.push(n);
  return lines.join('\n');
}

// Stata's %w.0g: width-2 significant digits in the fixed form when its integer
// part fits in width-2 digits and the value is at least 1e-5, the field budget
// width-1 characters (the sign not counted), decimals shed until it fits; a
// leading zero dropped; otherwise d.ddde+XX (piet::fmt::general, the native
// twin). Missing is `.`.
function stataGeneral(x, width) {
  if (x === null || x === undefined || Number.isNaN(Number(x))) return '.';
  const v = Number(x);
  if (v === 0) return '0';
  const sig = Math.max(1, width - 2);
  const budget = width - 1 + (v < 0 ? 1 : 0);
  const a = Math.abs(v);  // its magnitude decides the fixed form
  const trim = (t) => {
    let u = t;
    if (u.includes('.')) u = u.replace(/0+$/, '').replace(/\.$/, '');
    return u.replace(/^(-?)0\./, '$1.');
  };
  if (Number.isFinite(v)) {
    const e10 = Math.floor(Math.log10(a));
    const intDigits = e10 >= 0 ? e10 + 1 : 0;
    if (intDigits <= Math.max(1, width - 2) && a >= 1e-5) {
      let decimals = Math.max(0, e10 >= 0 ? sig - intDigits : sig + (-e10 - 1));
      for (; decimals >= 0; --decimals) {
        const t = trim(v.toFixed(decimals));
        if (t === '0' || t === '-0') break;
        if (t.length <= budget) return t;
      }
    }
  }
  if (!Number.isFinite(v)) return v > 0 ? '+Inf' : '-Inf';
  // d.ddde+XX with min(sig-1, width-7) decimals, shed until it fits
  let out = '';
  for (let edec = Math.max(0, Math.min(sig - 1, width - 7)); edec >= 0; --edec) {
    const [mant, ex] = v.toExponential(edec).split('e');
    const e = parseInt(ex, 10);
    out = mant + 'e' + (e >= 0 ? '+' : '-') + String(Math.abs(e)).padStart(2, '0');
    if (out.length <= budget) break;
  }
  return out;
}

/**
 * ttesti: Stata's one- and two-sample t-test table (the immediate form's
 * display), from the block's `ttest_table` (descriptive_statistics.cpp
 * runTtesti). Layout as StataBE 19.5 prints it (bank 3a8d742de94ef851
 * one-sample, 5ae23a055c3c4d91 unequal, 19147700e0806102 welch):
 *
 *   One-sample t test
 *   ------------------------------------------------------------------------------
 *            |     Obs        Mean    Std. err.   Std. dev.   [95% conf. interval]
 *   ---------+--------------------------------------------------------------------
 *          x |      24        62.6    3.225161        15.8    55.92825    69.27175
 *   ------------------------------------------------------------------------------
 *       mean = mean(x)                                                t =  -3.8448
 *   H0: mean = 75                                    Degrees of freedom =       23
 *
 *       Ha: mean < 75               Ha: mean != 75                 Ha: mean > 75
 *    Pr(T < t) = 0.0004         Pr(|T| > |t|) = 0.0008          Pr(T > t) = 0.9996
 *
 * Each row: the name in %8s (abbreviated to 8 with ~), Obs %7.0fc at column
 * 12, Mean / Std. err. / Std. dev. / the bounds %9.0g at columns 22, 34, 46,
 * 58, 70; the diff row has no Obs and no Std. dev. The alternatives are
 * centred about columns 11, 40 and 70 (79 - half its length when the third,
 * markers counted, is longer than 20).
 */
function formatTtestTable(tt) {
  const at = (line, col, text) => {
    const s = line.length < col - 1 ? line + ' '.repeat(col - 1 - line.length) : line;
    return s + text;
  };
  const abbrev = (t, n) => {
    const s = String(t ?? '');
    return s.length <= n ? s : s.slice(0, n - 2) + '~' + s.slice(-1);
  };
  const g = (x, w) => stataGeneral(x, w).padStart(w);
  const missing = (x) => x === null || x === undefined || !Number.isFinite(Number(x));
  const f = (x, w, d) => (missing(x) ? '.' : Number(x).toFixed(d)).padStart(w);
  const obs = (n) => (missing(n) ? '.' : Math.round(Number(n)).toLocaleString('en-US')).padStart(7);
  const rowLine = (r, withObs) => {
    let l = abbrev(r.name, 8).padStart(8) + ' |';
    if (withObs) l = at(l, 12, obs(r.n));
    l = at(l, 22, g(r.mean, 9));
    l = at(l, 34, g(r.se, 9));
    if (withObs) l = at(l, 46, g(r.sd, 9));
    l = at(l, 58, g(r.lb, 9));
    return at(l, 70, g(r.ub, 9));
  };
  // Three strings centred about three columns, as Stata places them.
  const centre3 = (a, b, c) => {
    let line = '';
    let last = 0;
    const put = (str, center) => {
      const skip = Math.max(last !== 0 ? 1 : 0, center - Math.floor(str.length / 2) - 1 - last);
      line += ' '.repeat(skip) + str;
      last += skip + str.length;
    };
    put(a, 11);
    put(b, 40);
    // the third column's width test counts the two colour markers around its value
    put(c.text, c.markedLength > 20 ? 79 - Math.floor(c.markedLength / 2) : 70);
    return line;
  };
  const third = (text, hasValue) => ({ text, markedLength: text.length + (hasValue ? 2 : 0) });

  const lines = ['', tt.title];
  const levelText = String(tt.level);
  const cil = levelText.length;
  const ciLabel = `[${levelText}% ${cil === 5 ? 'conf.interval' : 'conf. interval'}]`;
  lines.push('-'.repeat(78));
  let h = at(tt.col1 || '', 10, '|');
  h = at(h, 16, 'Obs'); h = at(h, 27, 'Mean'); h = at(h, 35, 'Std. err.'); h = at(h, 47, 'Std. dev.');
  lines.push(at(h, 61 - cil + (cil === 5 ? 1 : 0), ciLabel));
  const divline = '-'.repeat(9) + '+' + '-'.repeat(68);
  lines.push(divline);
  for (const r of tt.rows || []) lines.push(rowLine(r, true));
  if (tt.form === 'two') {
    lines.push(divline);
    lines.push(rowLine(tt.combined, true));
    lines.push(divline);
    lines.push(rowLine(tt.diff, false));
  }
  lines.push('-'.repeat(78));
  const p1 = f(tt.p_l, 6, 4), p2 = f(tt.p, 6, 4), p3 = f(tt.p_u, 6, 4);
  if (tt.form === 'two') {
    const dfText = { welch: "Welch's degrees of freedom = ",
                     satterthwaite: "Satterthwaite's degrees of freedom = " }[tt.df_kind] ||
                   'Degrees of freedom = ';
    const dfCol = { welch: 42, satterthwaite: 34 }[tt.df_kind] || 50;
    lines.push(at(`    diff = mean(${abbrev(tt.xname, 16)}) - mean(${abbrev(tt.yname, 16)})`, 67,
                  't = ' + f(tt.t, 8, 4)));
    lines.push(at('H0: diff = 0', dfCol, dfText + g(tt.df, 8)));
    lines.push('');
    lines.push(centre3('Ha: diff < 0', 'Ha: diff != 0', third('Ha: diff > 0', false)));
  } else {
    // H0's value as typed; a typed value longer than 8 characters in %8.0g.
    let m0 = String(tt.h0_text ?? '');
    if (m0.length > 8) m0 = stataGeneral(tt.h0, 8);
    lines.push(at(`    mean = mean(${tt.xname})`, 67, 't = ' + f(tt.t, 8, 4)));
    lines.push(at(`H0: mean = ${m0}`, 50, 'Degrees of freedom = ' + f(tt.df, 8, 0)));
    lines.push('');
    lines.push(centre3(`Ha: mean < ${m0}`, `Ha: mean != ${m0}`, third(`Ha: mean > ${m0}`, true)));
  }
  lines.push(centre3(`Pr(T < t) = ${p1}`, `Pr(|T| > |t|) = ${p2}`, third(`Pr(T > t) = ${p3}`, true)));
  return lines.join('\n');
}

/** Render estimation payloads that have useful scalars/rows but no coeftable. */
function formatGenericBlock(block) {
  const mappedTitle = estimatorTitle(block.estimator, []);
  const title = mappedTitle === 'Estimation' && block.estimator
    ? String(block.estimator)
    : mappedTitle;
  const lines = [title];
  const skip = new Set(['type', 'estimator', 'captured', 'coeftable']);

  const valueText = (value) => {
    if (typeof value === 'number') return fmtG(value) ?? String(value);
    if (typeof value === 'string' || typeof value === 'boolean') return String(value);
    if (value === null || value === undefined) return '';
    try { return JSON.stringify(value); }
    catch (_) { return String(value); }
  };

  const scalars = Object.entries(block).filter(([key, value]) => {
    if (skip.has(key) || (key === 'depvar' && !value)) return false;
    return ['number', 'string', 'boolean'].includes(typeof value);
  });
  if (scalars.length) {
    const nameW = Math.max(...scalars.map(([key]) => key.length));
    lines.push('');
    for (const [key, value] of scalars) {
      lines.push(`${padL(key, nameW)} = ${valueText(value)}`);
    }
  }

  for (const [name, rows] of Object.entries(block)) {
    if (!Array.isArray(rows) || rows.length === 0 || skip.has(name)) continue;
    const objectRows = rows.every(row => row && typeof row === 'object' && !Array.isArray(row));
    if (!objectRows) {
      lines.push('', `${name} = ${valueText(rows)}`);
      continue;
    }

    const keys = [];
    for (const row of rows) {
      for (const key of Object.keys(row)) {
        if (!keys.includes(key)) keys.push(key);
      }
    }
    if (!keys.length) continue;
    const cells = rows.map(row => keys.map(key => valueText(row[key])));
    const widths = keys.map((key, i) => Math.max(key.length, ...cells.map(row => row[i].length)));
    lines.push('', `${name}:`);
    lines.push(keys.map((key, i) => padR(key, widths[i])).join('   '));
    lines.push(widths.map(width => '─'.repeat(width)).join('───'));
    for (const row of cells) {
      lines.push(row.map((cell, i) => padR(cell, widths[i])).join('   '));
    }
  }

  return lines.join('\n');
}

function formatOneTable(block) {
  const {
    depvar, n_obs, coeftable, r2, r2_within, sigma2,
    vcov_type, n_clusters, absorbed, dof_resid,
    first_stage_F, kp_wald_F, hansen_j, hansen_j_p,
    // MI-081 / Contract B v1.8.0 model-fit scalars (all optional):
    adj_r2, F, p_F, rmse, mss, rss, df_m, df_r,          // OLS family
    df_b,                                                  // xtreg FE header numerator
    ll, ll_0, r2_pseudo, chi2, df_chi2, p_chi2,           // logit/probit / xtreg RE
    r2_between, r2_overall,                                // xtreg RE
  } = block;

  if (!coeftable || coeftable.length === 0) return '(no results)';

  // `ereturn display` (results/ereturn_commands.cpp runDisplay): e(b)'s columns in
  // e(b)'s order, an `eq:name` column shown as `name` under a line naming its
  // equation (StataBE 19.5 bank 88509ffa: `foreign      |` above mpg, weight, _cons).
  const eretDisplay = block.estimator === 'ereturn_display';
  const eqOf = r => {
    const c = eretDisplay && typeof r.name === 'string' ? r.name.indexOf(':') : -1;
    return c > 0 ? r.name.slice(0, c) : null;
  };

  // Rename (intercept) → _cons (Stata convention)
  const renamedRows = coeftable.map(r => ({
    ...r,
    displayName: r.name === '(intercept)' ? '_cons'
      : eqOf(r) !== null ? r.name.slice(eqOf(r).length + 1)
      : (r.stub || r.name),  // BUG-397: value-label display rides on `stub`; `name` stays the e(b) colname
  }));

  // A14: Stata renders the intercept/_cons row LAST, but the engine emits it
  // FIRST. Display-layer-only reorder: stable move-intercept-to-end.
  const isIntercept = r => r.name === '(intercept)' || r.name === '_cons';
  const rows = eretDisplay ? renamedRows : [
    ...renamedRows.filter(r => !isIntercept(r)),
    ...renamedRows.filter(r => isIntercept(r)),
  ];

  // ── Format all cell values ──────────────────────────────────────────────────
  // proportion, percent: estimate, std. err. and limits in percent, %9.2f (bank cdcb1db2)
  const pct = block.estimator === 'proportion' && block.percent === true;
  const fPct = x => (x == null || !isFinite(x)) ? '' : (100 * x).toFixed(2);
  const fB   = rows.map(r => r.b  === null ? '(omitted)' : pct ? fPct(r.b) : (fmtG(r.b)  ?? ''));
  const fSE  = rows.map(r => r.se === null ? ''           : pct ? fPct(r.se) : (fmtG(r.se) ?? ''));
  const fT   = rows.map(r => r.t  === null ? ''           : fmtT(r.t));
  const fPv  = rows.map(r => r.p  === null ? ''           : fmtP(r.p));
  const fCLo = rows.map(r => r.ci_lo === null ? '' : pct ? fPct(r.ci_lo) : (fmtG(r.ci_lo) ?? ''));
  const fCHi = rows.map(r => r.ci_hi === null ? '' : pct ? fPct(r.ci_hi) : (fmtG(r.ci_hi) ?? ''));

  // ── Column widths = max(header, all formatted values) ──────────────────────
  const nameW = Math.max(
    depvar ? depvar.length : 0,
    ...rows.map(r => r.displayName.length),
    ...rows.map(r => (eqOf(r) || '').length),
    4
  );
  const bW   = Math.max(11, ...fB.map(s => s.length));   // "Coefficient"
  const seW  = Math.max(9,  ...fSE.map(s => s.length));  // "Std. err."
  const tW   = Math.max(6,  ...fT.map(s => s.length));   // "t" → 6 for spacing
  const pW   = Math.max(5,  ...fPv.map(s => s.length));  // "P>|t|"
  const cLoW = Math.max(9,  ...fCLo.map(s => s.length));
  const cHiW = Math.max(9,  ...fCHi.map(s => s.length));

  // The column data region (after the ` │`; note rule uses nameW+1 to align ┬ with │)
  const dataRegionW = 1 + bW + 3 + seW + 3 + tW + 3 + pW + 5 + cLoW + 3 + cHiW;

  // ── Header section ─────────────────────────────────────────────────────────
  const absKeys = absorbed ? Object.keys(absorbed) : [];
  let title = estimatorTitle(block.estimator, absKeys);

  // BUG-017: MLE estimators (logit/probit/poisson/nbreg/...) report z / P>|z|;
  // OLS-family estimators report t / P>|t|. estimator is on the block (Boundary B).
  // BUG-205(1): `lincom`'s own estimator label never changes with what it was
  // computed FROM — pass vcov_type too so statLetter can key `lincom` on the
  // fit (oim -> z) rather than on the verb (which would always say t).
  // ereturn display: t(e(df_r)) when the posting has a residual dof, z otherwise.
  const sl   = eretDisplay ? (typeof df_r === 'number' && df_r > 0 ? 't' : 'z')
                           : statLetter(block.estimator, vcov_type, block.df);
  const tHdr = sl;                 // 't' or 'z'
  const pHdr = 'P>|' + sl + '|';   // 'P>|t|' or 'P>|z|'

  const statsArr = buildStats({
    n_obs, r2, r2_within, sigma2, dof_resid, absKeys, chi2type: block.chi2type,
    // MI-081 new scalars
    adj_r2, F, p_F, rmse, mss, rss, df_m, df_r, df_b,
    ll, ll_0, r2_pseudo, chi2, df_chi2, p_chi2,
    r2_between, r2_overall,
  });
  const statLines = formatStatLines(title, statsArr, nameW + 1 + dataRegionW);
  // xtreg, re theta: the header's last line (Stata 19.5 "theta        = .86060683")
  if (block.estimator === 'xtreg_re' && typeof block.theta === 'number')
    statLines.push(`theta        = ${fmtG(block.theta)}`);

  // ── Rules ──────────────────────────────────────────────────────────────────
  // nameW+1 so that ┬/┼/┴ aligns with the │ that follows the 1-char space after name
  const ruleN  = '─'.repeat(nameW + 1);
  const ruleD  = '─'.repeat(dataRegionW);

  // ── Column header row ──────────────────────────────────────────────────────
  // "[95% Conf. Interval]" spans both CI columns (cLoW + 3 sep + cHiW wide)
  // mean / proportion record the level their intervals were built at (level(), a replay)
  // xtreg, re and the IV fits (ivregress, ivreg2, ivreghdfe) record their level(#)
  // the same way (their limits are formed natively at that level)
  const LEVEL_ESTIMATORS = new Set(['mean', 'proportion', 'xtreg_re', 'iv', 'iv2sls', 'ivregress',
                                    'ivreg2', 'ivreghdfe', 'liml', 'gmm', 'ereturn_display']);
  const ciLevel = LEVEL_ESTIMATORS.has(block.estimator) &&
    typeof block.level === 'number' && block.level !== 95 ? block.level : 95;
  const ciLabel  = `[${ciLevel}% conf. interval]`;
  const ciTotalW = cLoW + 3 + cHiW;
  // regress `beta`: the standardized coefficients take the interval's place, right-
  // aligned, `.` on _cons (Stata 19.5 `regress price mpg weight, beta`)
  const showBeta = block.beta === true;
  const ciHeaderSection = showBeta ? padL('Beta', ciTotalW)
                                   : padR(ciLabel, ciTotalW); // left-align within CI region

  // WS1 #1: Stata flips the SE label to lowercase under a non-iid VCE.
  //   iid → "Std. err." ; robust/hc1/cluster(...) → "std. err."
  const _vc = (vcov_type || '').toLowerCase();
  const seLabel = (_vc === 'robust' || _vc === 'hc1' || _vc.startsWith('cluster('))
    ? 'std. err.' : 'Std. err.';
  // WS1 #2: margins reports marginal effects → the coef column header is "dy/dx".
  // BS-022 companion (BETA Phase C batch C1 dispatcher.cpp:2706): `logistic`
  // exponentiates the coeftable's b/se/ci to odds ratios (log-odds for `logit`
  // is untouched) — the coef column header must say so, or the values read as
  // a mislabeled "Coefficient" column.
  // eform_label: an exponentiated table's column title (suest's eform(OR)).
  const bLabel = block.estimator === 'margins' ? 'dy/dx'
    : typeof block.eform_label === 'string' && block.eform_label ? block.eform_label
    : block.estimator === 'logistic' ? 'Odds ratio'
    : pct ? 'Percent'
    : 'Coefficient';

  const headerRow = [
    padL(depvar || '', nameW), ' │',
    padL(bLabel,   bW  + 1),   // 1 leading space + bW ("Coefficient" / "dy/dx")
    padL(seLabel,  seW + 3),   // 3 leading spaces + seW ("Std. err." / "std. err.")
    padL(tHdr,        tW  + 3),   // 3 leading spaces + tW ('t' or 'z')
    padL(pHdr,        pW  + 3),   // 3 leading spaces + pW ('P>|t|' or 'P>|z|')
    '     ',                       // 5-char gap before CI columns
    ciHeaderSection,
  ].join('');

  // WS1 #3: margins prepends a "Delta-method" banner line above the column names,
  //   right-aligned over the SE column (Stata layout).
  const marginsBanner = block.estimator === 'margins'
    ? [padL('', nameW), ' │',
       padL('', bW + 1),
       padL('Delta-method', seW + 3)].join('')
    : null;

  // proportion names the interval's type above it (Logit by default, or citype()'s), and
  // mean / proportion under vce(cluster) put "Robust" above the std. err. column
  // (StataBE 19.5, vs-stata bank 0c939704 citype(wilson), 33bdd87d vce(cluster rep78)).
  const ciTypeLabel = block.estimator === 'proportion' && block.citype
    ? ({ logit: 'Logit', normal: 'Normal', wilson: 'Wilson', exact: 'Exact',
         jeffreys: 'Jeffreys', agresti: 'Agresti-Coull' }[block.citype] || '')
    : '';
  const robustLabel = (block.estimator === 'mean' || block.estimator === 'proportion') &&
    _vc.startsWith('cluster(') ? 'Robust' : '';
  const ciTypeBanner = (ciTypeLabel || robustLabel)
    ? [padL('', nameW), ' │',
       padL('', bW + 1),
       padL(robustLabel, seW + 3),
       padL('', tW + 3),
       padL('', pW + 3),
       '     ',
       padL(ciTypeLabel, ciTotalW)].join('').replace(/\s+$/, '')
    : null;

  // ── Data rows ──────────────────────────────────────────────────────────────
  const dataRowsFlat = rows.map((r, i) => {
    const omitted = r.b === null;
    // constraints(): a coefficient the constraints fix shows its value and
    // `(constrained)` in place of the rest (bank eae58c67 `mpg |  -.1  (constrained)`).
    if (r.constrained === true && !omitted) {
      const note = r.b === 0 ? '  (omitted)' : '  (constrained)';
      return [padL(r.displayName, nameW), ' │', padL(fB[i], bW + 1), note].join('');
    }
    if (showBeta) {
      const bs = isIntercept(r) ? '.' : (typeof r.beta === 'number' ? (fmtG(r.beta) ?? '') : '');
      return [
        padL(r.displayName, nameW), ' │',
        padL(fB[i],                  bW  + 1),
        padL(omitted ? '' : fSE[i],  seW + 3),
        padL(omitted ? '' : fT[i],   tW  + 3),
        padL(omitted ? '' : fPv[i],  pW  + 3),
        '     ',
        padL(omitted ? '' : bs, ciTotalW),
      ].join('');
    }
    return [
      padL(r.displayName, nameW), ' │',
      padL(fB[i],                  bW  + 1),
      padL(omitted ? '' : fSE[i],  seW + 3),
      padL(omitted ? '' : fT[i],   tW  + 3),
      padL(omitted ? '' : fPv[i],  pW  + 3),
      '     ',                                          // 5-char gap
      padL(omitted ? '' : fCLo[i], cLoW),              // CI lo right-aligned
      '   ',                                            // 3-char separator
      padL(omitted ? '' : fCHi[i], cHiW),              // CI hi right-aligned
    ].join('');
  });

  // ereturn display: each equation's name on its own line ahead of its rows.
  const dataRows = [];
  let lastEq = null;
  rows.forEach((r, i) => {
    const eq = eqOf(r);
    if (eq !== null && eq !== lastEq) dataRows.push(padR(eq, nameW) + ' │');
    lastEq = eq;
    dataRows.push(dataRowsFlat[i]);
  });

  // ── Footers ────────────────────────────────────────────────────────────────
  const footerLines = eretDisplay ? [] : buildFooter({
    vcov_type, n_clusters, absKeys, absorbed,
    first_stage_F, kp_wald_F, hansen_j, hansen_j_p,
  });

  // ── Assemble ───────────────────────────────────────────────────────────────
  // mean / proportion `noheader`: no title, no Number of obs line
  // regress / areg `notable`: the header alone, no coefficient table
  if (block.notable) return block.noheader ? '' : statLines.join('\n');
  const lines = [
    ...(block.noheader ? [] : [...statLines,
      // mean / proportion stdize(): the number of standard strata (bank 56f2dacb)
      ...(block.n_stdize != null ? [`N. of std strata = ${block.n_stdize}`] : []), '']),
    // constraints(): ` ( 1)  [rep78]mpg = -.1` between the header and the table
    ...(Array.isArray(block.constraint_lines) ? block.constraint_lines.map(l => ' ' + l) : []),
    ruleN + '┬' + ruleD,
    ...(marginsBanner ? [marginsBanner] : []),   // WS1 #3: Delta-method banner (margins only)
    ...(ciTypeBanner ? [ciTypeBanner] : []),     // proportion's CI type / mean, proportion's Robust
    headerRow,
    ruleN + '┼' + ruleD,
    ...dataRows,
    ruleN + '┴' + ruleD,
    ...footerLines,
    // proportion citype(agresti): an upper limit past 1 is cut to 1, and said so
    ...(block.estimator === 'proportion' && block.agresti_truncated
      ? ['Note: The Agresti\u2013Coull CI was truncated at 1 at the upper endpoint.'] : []),
  ];

  return lines.join('\n');
}

// ── oaxaca ────────────────────────────────────────────────────────────────────
// Ben Jann's oaxaca display (StataBE 19.5, vs-stata bank 0016e9 / 299759b7):
//   Blinder-Oaxaca decomposition                    Number of obs     =      2,229
//                                                   Model             =     linear
//   Group 1: south = 0                              N of obs 1        =      1,294
//   Group 2: south = 1                              N of obs 2        =        935
//
//      endowments: (X1 - X2) * b2          <- the definitions (nodefinitions drops them)
//    coefficients: X2 * (b1 - b2)
//     interaction: (X1 - X2) * (b1 - b2)
//
// then one table, the rows grouped under their equation (overall, endowments, ...),
// `eform` exponentiating b, se and the interval under an "exp(b)" header, the detail()
// legend (nolegend drops it), "(adjusted by ...)", and with `xb` the models'
// coefficients and means under "Coefficients (b) and means (x)". noheader / notable
// drop those parts.
function oaxacaDefinitions(block) {
  const tf = block.tf;
  if (tf === 1 || tf === 2) {
    const rev = tf === 2;
    return [
      `   endowments: (X1 - X2) * ${rev ? 'b1' : 'b2'}`,
      ` coefficients: ${rev ? 'X1' : 'X2'} * (b1 - b2)`,
      `  interaction: (X1 - X2) * (${rev ? 'b2 - b1' : 'b1 - b2'})`,
    ];
  }
  const w = block.weights_macro || '';
  let bref = 'b', xref = '';
  if (w === '1') { bref = 'b1'; xref = 'X2'; }
  else if (w === '0') { bref = 'b2'; xref = 'X1'; }
  const out = [`    explained: (X1 - X2) * ${bref}`];
  if (block.split) {
    out.push(` unexplained1: X1 * (b1 - ${bref})`);
    out.push(` unexplained2: X2 * (${bref} - b2)`);
  } else if (!xref) {
    out.push(`  unexplained: X1 * (b1 - ${bref}) + X2 * (${bref} - b2)`);
  } else {
    out.push(`  unexplained: ${xref} * (b1 - b2)`);
  }
  if (!xref) {
    let bdef;
    if (block.ref === 'pooled') bdef = 'from pooled model (including group dummy)';
    else if (block.ref === 'omega') bdef = 'from pooled model (without group dummy)';
    else if (block.ref === 'reference') bdef = `from model ${block.refname || ''}`;
    else {
      const ws = w.split(/\s+/).filter(Boolean);
      const wd = ws.length === 1 ? ws[0] : 'w';
      bdef = `= ${wd} * b1  + (1 - ${wd}) * b2`;
    }
    out.push(`               with ${bref} ${bdef}`);
  }
  return out;
}

function oaxacaTable(rows, depvar, opts) {
  const eform = !!opts.eform;
  const withSe = opts.se !== false;
  const tr = (x, f) => (x == null || !Number.isFinite(x)) ? null : (eform ? f(x) : x);
  const cells = rows.map(r => {
    const split = String(r.name).lastIndexOf(':');
    const eq = split >= 0 ? r.name.slice(0, split) : '';
    const nm = split >= 0 ? r.name.slice(split + 1) : r.name;
    const omitted = r.b === 0 && r.se === 0;
    const b = tr(r.b, Math.exp);
    const se = withSe && r.se != null && Number.isFinite(r.se) ? (eform ? Math.exp(r.b) * r.se : r.se) : null;
    return {
      eq, nm, omitted,
      b: omitted ? '0' : (b == null ? '.' : fmtG(b)),
      se: omitted ? '(omitted)' : (se == null ? '.' : (se === 0 ? '.' : fmtG(se))),
      t: omitted ? '' : (r.t == null ? '.' : fmtT(r.t)),
      p: omitted ? '' : (r.p == null ? '.' : fmtP(r.p)),
      lo: omitted ? '' : (r.ci_lo == null ? '.' : fmtG(tr(r.ci_lo, Math.exp))),
      hi: omitted ? '' : (r.ci_hi == null ? '.' : fmtG(tr(r.ci_hi, Math.exp))),
    };
  });
  const nameW = Math.max(12, (depvar || '').length, ...cells.map(c => c.nm.length), ...cells.map(c => c.eq.length));
  const bW = 11, seW = 9, tW = 6, pW = 5, cW = 9;
  const dataW = 1 + bW + 3 + seW + 3 + tW + 3 + pW + 5 + cW + 3 + cW;
  const ruleN = '─'.repeat(nameW + 1), ruleD = '─'.repeat(dataW);
  const lvl = typeof opts.level === 'number' ? opts.level : 95;
  const lines = [ruleN + '┬' + ruleD];
  if (opts.vcetype) lines.push(padL('', nameW) + ' │' + padL('', bW + 1) + padL(opts.vcetype, seW + 3));
  lines.push([padL(depvar || '', nameW), ' │', padL(eform ? 'exp(b)' : 'Coefficient', bW + 1),
    padL(withSe ? (opts.vcetype ? 'std. err.' : 'Std. err.') : '', seW + 3), padL('z', tW + 3), padL('P>|z|', pW + 3),
    '     ', padR(`[${lvl}% conf. interval]`, cW + 3 + cW)].join(''));
  let cur = null;
  for (const c of cells) {
    if (c.eq !== cur) {
      lines.push(ruleN + '┼' + ruleD);
      lines.push(padR(c.eq, nameW) + ' │');
      cur = c.eq;
    }
    if (c.omitted) {
      lines.push([padL(c.nm, nameW), ' │', padL(c.b, bW + 1), '  ', c.se].join(''));
      continue;
    }
    lines.push([padL(c.nm, nameW), ' │', padL(c.b, bW + 1), padL(c.se, seW + 3), padL(c.t, tW + 3),
      padL(c.p, pW + 3), '     ', padL(c.lo, cW), '   ', padL(c.hi, cW)].join(''));
  }
  lines.push(ruleN + '┴' + ruleD);
  return lines;
}

function formatOaxacaBlock(block) {
  const lines = [];
  if (block.show_header !== false) {
    const stats = [['Number of obs', fmtObs(block.n_obs)], ['Model', block.model || 'linear'],
      ['N of obs 1', fmtObs(block.N_1)], ['N of obs 2', fmtObs(block.N_2)]];
    const width = 78;
    const lefts = ['Blinder-Oaxaca decomposition', '',
      `Group 1: ${block.by} = ${block.group_1}`, `Group 2: ${block.by} = ${block.group_2}`];
    stats.forEach(([k, v], i) => {
      const right = padR(k, 17) + ' = ' + padL(String(v), 10);
      lines.push(lefts[i] + ' '.repeat(Math.max(2, width - lefts[i].length - right.length)) + right);
    });
    lines.push('');
    if (block.show_definitions !== false) {
      lines.push(...oaxacaDefinitions(block));
      lines.push('');
    }
  }
  if (block.show_table !== false) {
    lines.push(...oaxacaTable(block.coeftable || [], block.depvar,
      { eform: block.eform, se: block.se, level: block.level, vcetype: block.vcetype }));
    if (block.show_legend !== false && block.legend) {
      const groups = String(block.legend).match(/"[^"]*"/g) || [];
      for (const g of groups) lines.push(g.slice(1, -1));
    }
    if (block.adjusted && block.adjust) lines.push(`(adjusted by ${block.adjust})`);
  }
  if (Array.isArray(block.xb) && block.xb.length) {
    lines.push('');
    lines.push('Coefficients (b) and means (x)');
    lines.push(...oaxacaTable(block.xb, '', { se: block.se, level: block.level }));
  }
  return lines.join('\n');
}

// ── Helpers ───────────────────────────────────────────────────────────────────

// BUG-017: test-statistic letter is z (P>|z|) for large-sample-normal estimators
// — MLE, GMM-2SLS, SUR/3SLS, GLS random-effects, and TS-ML — and t (P>|t|) only
// for least-squares / small-sample-normal estimators. We invert to a small OLS_T
// allow-list and DEFAULT everything else to z, so a newly-supported estimator
// (which is almost always large-sample) is never silently mislabeled t.
//
// t / P>|t| — least-squares family (must list every OLS-ish estimatorTitle key):
//   feols, feols_multi, ols, reg, r, regress, areg, reghdfe, xtreg_fe, xtreg_be,
//   newey, prais, qreg, sqreg, iqreg.
// z / P>|z| — EVERYTHING ELSE estimatorTitle knows, i.e. the MLE / 2SLS / SUR /
//   RE-GLS / TS-ML estimators: logit, logistic, probit, cloglog, poisson, fepois,
//   ppmlhdfe, nbreg, tobit, heckman, glm, mlogit, ologit, oprobit, truncreg,
//   xtlogit, xtprobit, xtpoisson, xtnbreg, xtreg / xtreg_re, iv, iv2sls,
//   iv2sls_multi, ivregress, reg3, sureg, arima, arch, var, vec.
const OLS_T_ESTIMATORS = new Set([
  'feols','feols_multi','ols','reg','r','regress','areg',
  // BS-026 companion: `reghdfe` IS feols --fe under the hood (cobra has no
  // separate reghdfe estimator — handlers.cpp:1022 label-only override), so
  // it belongs in the same least-squares t-stat class as feols/areg.
  'reghdfe',
  'xtreg_fe','xtreg_be','newey','prais','qreg','sqreg','iqreg',
  // NOTE: 'lincom' deliberately does NOT live in this allow-list (BUG-205(1)
  // reverted WS1 #10's addition of it) — `lincom`'s own estimator label never
  // changes with what it was computed FROM, so keying off the VERB always
  // said `t` even after logit/probit/poisson. See the `lincom` branch below,
  // which keys off the fit instead.
]);
// BUG-205(1): `lincom` is the one estimator whose OWN label ('lincom') tells
// you nothing about the model that produced it — Stata prints `z` for
// `lincom` after logit/probit/poisson and `t` after `regress`. The engine
// already carries the discriminator on the wire: `vcov_type` is `oim` on the
// ML (post-estimation covariance) refit and `iid`/`cluster(...)`/robust/... on
// the least-squares one (cmds_postest.cpp `cmd_lincom`). `nlcom` needs no such
// branch — it is already outside OLS_T_ESTIMATORS, i.e. always `z`, which is
// correct for BOTH its OLS and ML paths (Stata's nlcom is always a delta-
// method z, even after `regress`).
// lincom ..., df(#) puts a t(#) reference on any fit, an ML one too, and the
// block's df says which reference the statistics were computed from: a
// positive df is t, a null one the normal (the same rule that posts r(t) or
// r(z), route_postestimation.cpp). Blocks without df keep the vcov_type rule.
function statLetter(est, vcov_type, df) {
  if (est === 'lincom') {
    if (df === null) return 'z';
    if (typeof df === 'number') return df > 0 ? 't' : 'z';
    return vcov_type === 'oim' ? 'z' : 't';
  }
  return OLS_T_ESTIMATORS.has(est) ? 't' : 'z';
}

function estimatorTitle(est, absKeys) {
  const base = {
    feols:        'Linear regression',
    feols_multi:  'Linear regression (multiple outcomes)',
    ols:          'Linear regression',
    reg:          'Linear regression',
    // BS-026 companion: `reghdfe` IS feols --fe under the hood (see
    // OLS_T_ESTIMATORS comment above) — same title family as feols/regress.
    reghdfe:      'Linear regression',
    iv:           'IV (2SLS) regression',
    iv2sls:       'IV (2SLS) regression',
    iv2sls_multi: 'IV (2SLS) regression (multiple outcomes)',
    ivregress:    'IV regression',
    logit:        'Logistic regression',
    logistic:     'Logistic regression',
    probit:       'Probit regression',
    cloglog:      'Complementary log\u2013log regression',
    fepois:       'Poisson regression',
    poisson:      'Poisson regression',
    nbreg:        'Negative binomial regression',
    tobit:        'Tobit regression',
    heckman:      'Heckman selection model',
    xtreg:        'Random-effects GLS regression',
    xtreg_re:     'Random-effects GLS regression',
    xtreg_fe:     'Fixed-effects regression',
    xtreg_be:     'Between-effects GLS regression',
    xtlogit:      'Random-effects logistic regression',
    xtlogit_fe:   'Conditional fixed-effects logistic regression',
    xtprobit:     'Random-effects probit regression',
    xtpoisson:    'Random-effects Poisson regression',
    xtpoisson_fe: 'Conditional fixed-effects Poisson regression',
    qreg:         'Quantile regression',
    sqreg:        'Simultaneous quantile regression',
    iqreg:        'Interquantile range regression',
    reg3:         'Three-stage least squares',
    sureg:        'Seemingly unrelated regression',
    arima:        'ARIMA regression',
    arch:         'ARCH/GARCH regression',
    newey:        'Newey-West regression',
    prais:        'Prais-Winsten AR(1) regression',
    var:          'Vector autoregression (VAR)',
    vec:          'Vector error-correction model',
    margins:      'Average marginal effects',   // WS1 #4
    lincom:       'Linear combination',          // WS1 #10 (companion title)
    nlcom:        'Nonlinear combination',
  }[est] ?? 'Estimation';

  if (absKeys && absKeys.length > 0) {
    return base + ' with absorbed FEs';
  }
  return base;
}

/**
 * Build the right-column stats array for the estimation header.
 * Each element is [label, value_string].
 * Emits Stata-style model-fit rows from Contract B v1.8.0 scalars (all optional).
 *
 * Priority order (mirrors Stata output):
 *   OLS family   : Number of obs / F(df_m,df_r) / Prob>F / R² / Adj R² / Root MSE
 *   logit/probit : Number of obs / LR chi2(df) / Prob>chi2 / Log likelihood / Pseudo R²
 *   xtreg RE     : Number of obs / R-sq within/between/overall / Wald chi2(df) / Prob>chi2
 *   fallback     : Number of obs / R² / Root MSE (legacy path)
 */
function buildStats({
  n_obs, r2, r2_within, sigma2, dof_resid, absKeys, chi2type,
  // OLS family
  adj_r2, F, p_F, rmse, mss, rss, df_m, df_r,
  // BUG-1291: xtreg, fe prints its model F as F(e(df_b), e(df_r))
  // (xtreg,fe's header), not over e(df_m), which counts the panel means
  // (StataBE "F(2, 148) = 1191.52" where e(df_m) is 31). Only that block carries it.
  df_b,
  // logit/probit / xtreg RE
  ll, ll_0, r2_pseudo, chi2, df_chi2, p_chi2,
  // xtreg RE
  r2_between, r2_overall,
}) {
  const stats = [];

  // WS1 #8/#9: model-fit header scalars follow Stata's fixed-decimal conventions
  //   (not fmtG/fmtP): chi2 & F test statistics use %9.2f (keep the leading zero,
  //   e.g. 0.31 not .31); header probabilities (Prob > chi2 / Prob > F) use 4 dp.
  const fmt2f  = (x) => (x == null || !Number.isFinite(x)) ? '?' : Number(x).toFixed(2);
  const fmtP4h = (x) => {
    if (x == null || !Number.isFinite(x)) return '?';
    if (x < 0.00005) return '0.0000';
    if (x > 0.99995) return '1.0000';
    return Number(x).toFixed(4);
  };

  // Always show obs count first.
  if (n_obs != null) stats.push(['Number of obs', fmtObs(n_obs)]);

  const hasAbsorbed = absKeys && absKeys.length > 0;

  // ── Panel R² path (xtreg): has r2_within + r2_between + r2_overall ──────────
  // Covers both xtreg,RE (Wald chi2 test) and xtreg,FE / single-FE feols
  // (F test — cobra 0.15.1 emits F/p_F/df_m/df_r and NO chi2 for the FE route).
  if (r2_between != null || r2_overall != null) {
    // Show whichever of the three R² types are present.
    if (r2_within  != null) stats.push(['R-sq: within',  r2_within.toFixed(4)]);
    if (r2_between != null) stats.push(['R-sq: between', r2_between.toFixed(4)]);
    if (r2_overall != null) stats.push(['R-sq: overall', r2_overall.toFixed(4)]);
    if (chi2 != null) {
      // RE case: Wald chi2 test.
      const dfStr = df_chi2 != null ? `(${df_chi2})` : '';
      stats.push([`Wald chi2${dfStr}`, fmt2f(chi2)]);
      if (p_chi2 != null) stats.push(['Prob > chi2', fmtP4h(p_chi2)]);
    } else if (F != null) {
      // FE case (xtreg,fe / areg / single-FE feols): F test, not Wald chi2.
      const dfNum = df_b != null ? df_b : df_m;
      const dfStr = (dfNum != null && df_r != null) ? `(${dfNum}, ${df_r})` : '';
      stats.push([`F${dfStr}`, fmt2f(F)]);
      if (p_F != null) stats.push(['Prob > F', fmtP4h(p_F)]);
    }
    return stats;
  }

  // ── logit/probit path: has ll or r2_pseudo ───────────────────────────────
  if (ll != null || r2_pseudo != null) {
    if (chi2 != null) {
      const dfStr = df_chi2 != null ? `(${df_chi2})` : '';
      // a constraints() fit's model test is Wald (bank eae58c67 `Wald chi2(1) = 25.02`)
      stats.push([`${chi2type === 'Wald' ? 'Wald' : 'LR'} chi2${dfStr}`, fmt2f(chi2)]);
    }
    if (p_chi2 != null) stats.push(['Prob > chi2', fmtP4h(p_chi2)]);
    if (ll     != null) stats.push(['Log likelihood', fmtG(ll) ?? '?']);
    if (r2_pseudo != null) stats.push(['Pseudo R2', r2_pseudo.toFixed(4)]);
    return stats;
  }

  // ── OLS family path: has F or adj_r2 or rmse ────────────────────────────
  if (F != null || adj_r2 != null || rmse != null) {
    if (F != null) {
      // a weighted xtreg, fe block (no between/overall R-squared) lands here
      const dfNum = df_b != null ? df_b : df_m;
      const dfStr = (dfNum != null && df_r != null) ? `(${dfNum}, ${df_r})` : '';
      stats.push([`F${dfStr}`, fmt2f(F)]);
    }
    if (p_F     != null) stats.push(['Prob > F', fmtP4h(p_F)]);
    if (r2      != null) stats.push(['R-squared', r2.toFixed(4)]);
    if (adj_r2  != null) stats.push(['Adj R-squared', adj_r2.toFixed(4)]);
    if (rmse    != null) stats.push(['Root MSE', fmtG(rmse) ?? '?']);
    return stats;
  }

  // ── Legacy / fallback path ───────────────────────────────────────────────
  if (r2_within != null) {
    stats.push([hasAbsorbed ? 'Within R-sq.' : 'R-squared', r2_within.toFixed(4)]);
  } else if (r2 != null) {
    stats.push(['R-squared', r2.toFixed(4)]);
  }
  if (sigma2 != null && sigma2 > 0) {
    const rootMse = Math.sqrt(sigma2);
    stats.push(['Root MSE', fmtG(rootMse) ?? '?']);
  }
  return stats;
}

function formatStatLines(title, stats, tableWidth) {
  if (stats.length === 0) return [title];

  const maxLabelW = Math.max(...stats.map(([k]) => k.length));
  const maxValW   = Math.max(...stats.map(([, v]) => String(v).length));
  const statLineW = maxLabelW + 3 + maxValW + 2; // "label   =  value"

  const lines = [];
  for (let i = 0; i < Math.max(1, stats.length); i++) {
    const left  = i === 0 ? title : '';
    const right = i < stats.length
      ? padR(stats[i][0], maxLabelW) + '   = ' + padL(String(stats[i][1]), maxValW + 1)
      : '';
    const gap = Math.max(2, tableWidth - left.length - right.length);
    lines.push(left + ' '.repeat(gap) + right);
  }
  return lines;
}

function buildFooter({ vcov_type, n_clusters, absKeys, absorbed,
                        first_stage_F, kp_wald_F, hansen_j, hansen_j_p }) {
  const lines = [];

  if (vcov_type) {
    const lc = vcov_type.toLowerCase();
    if (lc.startsWith('cluster(')) {
      const clusterVar = vcov_type.slice(8, -1);
      const nc = n_clusters ?? '?';
      lines.push(`(Std. err. adjusted for ${nc} clusters in ${clusterVar})`);
    } else if (lc === 'hc1' || lc === 'robust') {
      lines.push('(Std. err. adjusted for heteroskedasticity)');
    }
  }

  if (absKeys && absKeys.length > 0 && absorbed) {
    const feStr = Object.entries(absorbed)
      .map(([k, v]) => `${k} (${fmtObs(v)} levels)`)
      .join(', ');
    lines.push(`Absorbed: ${feStr}`);
  }

  if (first_stage_F) {
    for (const [endog, fval] of Object.entries(first_stage_F)) {
      lines.push(`First-stage F (${endog}): ${fmtG(fval) ?? '?'}`);
    }
  }
  if (kp_wald_F != null) {
    lines.push(`Kleibergen-Paap Wald F: ${fmtG(kp_wald_F) ?? '?'}`);
  }
  if (hansen_j != null) {
    const pStr = hansen_j_p != null ? ` (p = ${fmtP(hansen_j_p)})` : '';
    lines.push(`Hansen J: ${fmtG(hansen_j) ?? '?'}${pStr}`);
  }

  return lines;
}
