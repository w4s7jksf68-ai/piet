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

  // Multi-equation: render each outcome sub-table
  if (outcomes && outcomes.length > 0) {
    return outcomes.map((oc, i) => {
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

  if (restrictions.length) lines.push('');

  // BUG-1057: a statistic the engine could not form (R V R' exactly 0 after an
  // exact fit) arrives null, which Stata prints as `.` (StataBE 19.5 `test x` after
  // `regress y2 x` on y2 = 3 + 2*x: `F(  1,    48) =       .`, `Prob > F =
  // .`); Number(null) printed 0.00 and fmtP(null) nothing.
  const missing = (x) => x == null || !Number.isFinite(Number(x));
  const stat = (x) => (missing(x) ? '.' : Number(x).toFixed(2));
  const prob = (x) => (missing(x) ? '.' : fmtP(Number(x)));

  // iid → F statistic; robust/cluster → Wald chi2 (matches buildStats footer).
  if (block.vcov_type !== 'iid') {
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
  if (chi2 == null) return '(no results)';
  return [
    'Breusch-Pagan / Cook-Weisberg test for heteroskedasticity',
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

  // Rename (intercept) → _cons (Stata convention)
  const renamedRows = coeftable.map(r => ({
    ...r,
    displayName: r.name === '(intercept)' ? '_cons' : (r.stub || r.name),  // BUG-397: value-label display rides on `stub`; `name` stays the e(b) colname
  }));

  // A14: Stata renders the intercept/_cons row LAST, but the engine emits it
  // FIRST. Display-layer-only reorder: stable move-intercept-to-end.
  const isIntercept = r => r.name === '(intercept)' || r.name === '_cons';
  const rows = [
    ...renamedRows.filter(r => !isIntercept(r)),
    ...renamedRows.filter(r => isIntercept(r)),
  ];

  // ── Format all cell values ──────────────────────────────────────────────────
  const fB   = rows.map(r => r.b  === null ? '(omitted)' : (fmtG(r.b)  ?? ''));
  const fSE  = rows.map(r => r.se === null ? ''           : (fmtG(r.se) ?? ''));
  const fT   = rows.map(r => r.t  === null ? ''           : fmtT(r.t));
  const fPv  = rows.map(r => r.p  === null ? ''           : fmtP(r.p));
  const fCLo = rows.map(r => r.ci_lo === null ? '' : (fmtG(r.ci_lo) ?? ''));
  const fCHi = rows.map(r => r.ci_hi === null ? '' : (fmtG(r.ci_hi) ?? ''));

  // ── Column widths = max(header, all formatted values) ──────────────────────
  const nameW = Math.max(
    depvar ? depvar.length : 0,
    ...rows.map(r => r.displayName.length),
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
  const sl   = statLetter(block.estimator, vcov_type);
  const tHdr = sl;                 // 't' or 'z'
  const pHdr = 'P>|' + sl + '|';   // 'P>|t|' or 'P>|z|'

  const statsArr = buildStats({
    n_obs, r2, r2_within, sigma2, dof_resid, absKeys,
    // MI-081 new scalars
    adj_r2, F, p_F, rmse, mss, rss, df_m, df_r, df_b,
    ll, ll_0, r2_pseudo, chi2, df_chi2, p_chi2,
    r2_between, r2_overall,
  });
  const statLines = formatStatLines(title, statsArr, nameW + 1 + dataRegionW);

  // ── Rules ──────────────────────────────────────────────────────────────────
  // nameW+1 so that ┬/┼/┴ aligns with the │ that follows the 1-char space after name
  const ruleN  = '─'.repeat(nameW + 1);
  const ruleD  = '─'.repeat(dataRegionW);

  // ── Column header row ──────────────────────────────────────────────────────
  // "[95% Conf. Interval]" spans both CI columns (cLoW + 3 sep + cHiW wide)
  const ciLabel  = '[95% conf. interval]';
  const ciTotalW = cLoW + 3 + cHiW;
  const ciHeaderSection = padR(ciLabel, ciTotalW); // left-align within CI region

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
  const bLabel = block.estimator === 'margins' ? 'dy/dx'
    : block.estimator === 'logistic' ? 'Odds ratio'
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

  // ── Data rows ──────────────────────────────────────────────────────────────
  const dataRows = rows.map((r, i) => {
    const omitted = r.b === null;
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

  // ── Footers ────────────────────────────────────────────────────────────────
  const footerLines = buildFooter({
    vcov_type, n_clusters, absKeys, absorbed,
    first_stage_F, kp_wald_F, hansen_j, hansen_j_p,
  });

  // ── Assemble ───────────────────────────────────────────────────────────────
  const lines = [
    ...statLines,
    '',
    ruleN + '┬' + ruleD,
    ...(marginsBanner ? [marginsBanner] : []),   // WS1 #3: Delta-method banner (margins only)
    headerRow,
    ruleN + '┼' + ruleD,
    ...dataRows,
    ruleN + '┴' + ruleD,
    ...footerLines,
  ];

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
function statLetter(est, vcov_type) {
  if (est === 'lincom') return vcov_type === 'oim' ? 'z' : 't';
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
  n_obs, r2, r2_within, sigma2, dof_resid, absKeys,
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
      stats.push([`LR chi2${dfStr}`, fmt2f(chi2)]);
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
