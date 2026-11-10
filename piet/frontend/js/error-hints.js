// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// error-hints.js — curated plain-language hint line for known error codes/messages.

// A18: `factor_misuse`'s engine message tells the user to "re-run to_hbin
// WITHOUT --factor on 'col'" — to_hbin is an internal dev CLI tool end users
// never touch. Extract the column name out of that message (both emission
// sites end it `on 'NAME'` — apps/cmds/common.cpp:448 (structured,
// code=factor_misuse) and apps/cmds/cmds_ts.cpp:147 (plain stderr text,
// code defaults to engine_error) — so the message-substring fallback below
// covers the second site too) and translate to the Stata-idiom fix.
function toHbinHint(message) {
  const m = /on '([^']+)'/.exec(message || '');
  const col = m ? m[1] : 'this column';
  return `'${col}' is a text/factor variable — make a numeric id first: ` +
    `\`egen id = group(${col})\`, then use \`id\`.`;
}

// B1 (SEED, hard-capped ≤20 entries): curated code/pattern → ONE
// plain-language why/fix line, appended BELOW the real code+message (never
// replacing it — two-tier: honest machine line + one plain line). Keyed by
// `code` first (the engine's stable vocabulary — src/core/error.h — plus
// the PIET-layer parse/dispatch codes — piet/native/include/piet/eval/
// errors.h); message-substring patterns below catch the same failure when
// it surfaces under a different/generic code. Table, not branches
// (IDIOMS.md "declarative tables over bespoke branches").
const ERROR_HINTS_BY_CODE = {
  factor_misuse:       (b) => toHbinHint(b.message),
  missing_column:      () => "Check the variable name for typos — it isn't in the currently loaded dataset (`describe` lists what's loaded).",
  unsupported_option:  () => 'This option isn’t recognized/supported for this command — check the spelling, or drop it and re-run without it.',
  unsupported_command: () => "This Stata command isn't implemented in PIET yet.",
  no_data_loaded:      () => 'No dataset is loaded — run `use "yourfile.dta"` (or `sysuse`) first.',
  syntax_error:        () => "PIET couldn't parse this line — check for a missing comma before options, an unmatched quote, or a typo in the command name.",
  collinear:           () => 'The regressors are perfectly collinear (one is an exact combination of the others) — drop or combine the redundant variable(s).',
  under_identified:    () => 'Too few instruments for the endogenous regressors — add another excluded instrument or drop an endogenous variable.',
  no_obs:              () => 'Too few usable observations after listwise deletion — check for missing values in the variables you’re using.',
  no_convergence:      () => 'The iterative estimator didn’t converge — try dropping variables, rescaling, or simplifying the model.',
  common_sample:       () => "The outcomes in this multi-equation command aren't jointly observed — check for missing values across all outcome variables.",
  bad_input:           () => 'One of the option/argument values isn’t valid for this command — double-check the syntax.',
  data_io:             () => "The dataset file couldn't be read — check the path and that it's a valid .dta file.",
};
const ERROR_HINTS_BY_MESSAGE = [
  { test: /to_hbin/i,        hint: (msg) => toHbinHint(msg) },
  // BUG-128: "file not found" (joinby/merge/append using, do, confirm file, …)
  // was falling through to the generic /not found/i entry below and getting
  // the variable-name hint — misdirects a missing-FILE error toward variable
  // typos. Must come first in the list: Array.find() takes the first match.
  { test: /file not found/i, hint: () => "Check the file path — PIET couldn't find that file on disk (relative paths resolve from the current working directory)." },
  { test: /not found/i,      hint: () => "Check the variable name for typos — it isn't in the currently loaded dataset (`describe` lists what's loaded)." },
];
export function noviceErrorHint(block) {
  const byCode = ERROR_HINTS_BY_CODE[block.code];
  if (byCode) return byCode(block);
  const msg = block.message || '';
  const entry = ERROR_HINTS_BY_MESSAGE.find(e => e.test.test(msg));
  return entry ? entry.hint(msg) : null;
}
