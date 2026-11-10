// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Supplement mutation blocks without changing wire block accounting.

function summarizeMutationExtras(block) {
  if (!block || block.type !== 'data_mutation') return [];
  const extra = [];

  if (block.merge_counts) {
    const mc = block.merge_counts;
    // Real wire shape (native handlers_data_verbs.cpp:1911-1913): named keys
    // master_only (_merge==1) / using_only (_merge==2) / matched (_merge==3).
    const n1 = Number(mc.master_only) || 0;  // from master
    const n2 = Number(mc.using_only)  || 0;  // from using
    const n3 = Number(mc.matched)     || 0;  // matched
    const lines = [
      '    Result                           # of obs.',
      '    -----------------------------------------',
      `    not matched                    ${String(n1 + n2).padStart(7)}`,
      `        from master                    ${String(n1).padStart(3)}  (_merge==1)`,
      `        from using                     ${String(n2).padStart(3)}  (_merge==2)`,
      '',
      `    matched                        ${String(n3).padStart(7)}  (_merge==3)`,
      '    -----------------------------------------',
    ];
    extra.push({ type: 'display', text: lines.join('\n') });
  }

  if (block.n_dropped != null) {
    extra.push({ type: 'display', text: `(${block.n_dropped} observations deleted)` });
  }

  if (block.direction && (block.n_wide != null || block.n_long != null)) {
    extra.push({ type: 'display',
      text: `(data reshaped to ${block.direction}: ${block.n_wide ?? '?'} wide obs -> ${block.n_long ?? '?'} long obs)` });
  }

  return extra;
}

export function withMutationSummaries(blocks) {
  const out = [];
  for (const b of blocks) {
    out.push(b);
    out.push(...summarizeMutationExtras(b));
  }
  return out;
}
