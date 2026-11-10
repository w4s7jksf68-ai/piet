// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "options.h"
#include "handler_declarations.h"

namespace piet::dispatch_detail {

// B5-D-USE-REPLACE (92 corpus uses inside the twelve AER papers, this lane's
// highest-weight item): `replace` joins `clear`. Measured StataBE 19.5
// (tests/eval/golden/aer_b4/b5_io_pins.log:37-38 `B5REC|use|replace|0|` +
// :49-61 the discriminating pair: a variable `gen`erated AFTER the prior
// `use` does NOT survive `use FILE, replace` — `confirm variable junk`
// answers r(111) afterwards, exactly what `, clear` does too) —
// `, replace` DISCARDS unsaved changes, it is not a no-op and not a merge.
// `use` is in kOptMinComplete (generated/opt_minimums_bug264.inc) with NO
// measured minimum row for either spelling, so only the EXACT literal
// "clear"/"replace" resolve here — no abbreviation at all. That costs
// nothing measured: all 92 census occurrences spell the option out in full
// (internal capture lexgaps-12.json, command="use" option="replace",
// every one of the 92 `uses[].text` entries matches `,\s*replace` verbatim).
const piet::eval::OptVocab& kVocabUse = piet::eval::registerOptVocab(
    {"use"}, {{"clear", "replace", "nolabel"},
     {}});

// The period options are xtset.ado's too (xtset hands `panelvar timevar` to
// tsset): `xtset id year, yearly` formats year %ty and posts r(unit) yearly
// (aer_w2 w2b_dataio xtset_yearly / xtset_monthly, w2b_dataio.log:256-301).
// mi, noblank, panelname() and displayindent() are tsset.ado's display options,
// which xtset.ado's PanelSet takes too (takeTsDisplayOptions; BUG-1163 review).
const piet::eval::OptVocab& kVocabXtset = piet::eval::registerOptVocab(
    {"xtset"}, {{"delta", "clear", "yearly", "halfyearly", "quarterly", "monthly", "weekly",
                 "daily", "generic", "clocktime", "format", "mi", "noblank", "panelname",
                 "displayindent"},
     {}});

// BUG-896: `clear` and `clocktime` are tsset.ado's own option names (its
// `syntax` line: CLEAR ... Clocktime). BUG-1163: the obsolete `tset` is tset.ado's
// one line `tsset `0'`, so it reads the same options.
const piet::eval::OptVocab& kVocabTsset = piet::eval::registerOptVocab(
    {"tsset", "tset"},
    {{"delta", "yearly", "halfyearly", "quarterly", "monthly", "weekly", "daily", "generic",
      "clock", "clocktime", "format", "clear", "mi", "noblank", "panelname", "displayindent"},
     {}});

// BUG-664: float/dpcomma/percent are destring options documented in [D] destring,
// read by runDestring (DestringOpts). `g` is declared for the same
// reason as tostring's below -- w2b_dataio.log destring_g_force runs
// `destring s, g(sn) force` at rc 0.
const piet::eval::OptVocab& kVocabDestring = piet::eval::registerOptVocab(
    {"destring"}, {{"replace", "gen", "generate",
                                                                        "ignore", "force", "float",
                                                                        "dpcomma", "percent"},
     {},
     {{"g", "generate"}, {"ge", "generate"}}});

// B5-D1 / BUG-665: `force` overrides tostring.ado's reversibility test (a
// value whose string does not read back equal is converted anyway instead of
// the "cannot be converted reversibly" note); without the token a legal script
// died at r(198) (measured B4REC|tostring|replace_force|0| —
// tests/eval/golden/aer_b4/pins_oracle_r.log:29; StataBE renders 42/./.a
// with `force` on a double, internal capture
// tostring_missing_oracle.log:67, rc 0). `format` is the ado's `format(str)`.
// `g` is Stata's abbreviation of `generate`; it cannot resolve via the
// unique-prefix rule because `gen`/`generate` are BOTH already-registered
// literal spellings that `g` prefixes ambiguously, so it is declared instead
// (BUG-430 aliases mechanism, kVocabPredict precedent above). `ge` is the same
// case one letter longer: it prefixes both spellings, and the ado's
// `Generate(string)` accepts it (w2z_tostring tz_ge_abbrev: `tostring vfi,
// ge(vfi_s)` at rc 0, str2; destring documents the same g[enerate]() abbreviation).
const piet::eval::OptVocab& kVocabTostring = piet::eval::registerOptVocab(
    {"tostring"}, {{"replace", "gen", "generate",
                                                                        "usedisplayformat", "force",
                                                                        "format"},
     {},
     {{"g", "generate"}, {"ge", "generate"}}});

// BUG-370. `nq` is Stata's own abbreviation of nquantiles, and the vocab layer
// resolves prefixes, so registering the long spelling accepts both.
// BUG-1096: maxlength() is read by runDecode; its minimum `maxl' is a floor row
// (`max(3)' is "option max() not allowed", StataBE 19.5). BUG-1141: Generate()
// takes `g()', below the prefix floor, so declared.
const piet::eval::OptVocab& kVocabDecode = piet::eval::registerOptVocab(
    {"decode"}, {{"generate", "maxlength"},
     {},
     {{"g", "generate"}}});

// B5-D7: `not` moves to known -- see the `ds` arm below for the complement
// implementation and the measured receipt. has/alpha/varwidth/detail stay
// unimplemented (refused by name, not silently dropped).
const piet::eval::OptVocab& kVocabDs = piet::eval::registerOptVocab(
    {"ds"}, {{"not"},
     {"detail", "varwidth", "alpha", "has"}});

// BUG-1015: `n()` is xtile.ado's own minimum abbreviation of nquantiles()
// (`Nquantiles(string)`), below the 2-character prefix floor, so it is declared
// (w2as_summary_stats_family sx_xtile_n: rc 0 in Stata, r(198) here before).
const piet::eval::OptVocab& kVocabXtile = piet::eval::registerOptVocab(
    {"xtile"}, {{"nquantiles", "cutpoints"},
     {"altdef"},
     {{"n", "nquantiles"}, {"c", "cutpoints"}}});

const piet::eval::OptVocab& kVocabIpolate = piet::eval::registerOptVocab(
    {"ipolate"}, {{"generate", "epolate", "by"},
     {}});

const piet::eval::OptVocab& kVocabTssmooth = piet::eval::registerOptVocab(
    {"tssmooth"}, {{"window"},
     {"replace"}});

// B5-D10: `generate(NAME)` accepted -- see the `append` arm below for the
// src-marker implementation and the measured receipt
// (B4REC|append|generate|0|). `append` is in kOptMinComplete
// (opt_minimums_bug264.inc), so this net runs exactOnly: only the EXACT
// spelling `generate` resolves -- `gen(...)` stays r(198), because no oracle
// cell measures that abbreviation on THIS verb (fabricating acceptance would
// be unmeasured parity). BUG-549: `keep(varlist)` restricts which USING
// columns are taken (master's own columns are padded missing on the using rows):
// runAppend expands it against the using names and cmd_append projects the using
// frame (`--keepusing`) before the concat. Oracle: w2_data_io.log app1-app7,
// app10, app12-app14. (That log's app9 now measures `gen(src)` at rc 0, so the
// r(198) above is stale -- still pinned by selftest.cpp's B5-D10 arm, left for
// its owner.) BUG-550: a string/numeric clash is r(106) without `force` and
// missing on the using rows with it (app15-app17), see the append arm below.
// BUG-551: a .dta using's value-label definitions are copied unless `nolabel`
// (app1-app13), and generate()'s marker carries append.ado's `_append` label.
// `nonotes` (app13: rc 0, same result as keep(id a)) asks that the using file's
// notes not be copied; runAppend copies none, so it is honoured as given.
const piet::eval::OptVocab& kVocabAppend = piet::eval::registerOptVocab(
    {"append"}, {{"force", "generate", "keep", "nolabel", "nonotes"},
     {}});

const piet::eval::OptVocab& kVocabFramePut = piet::eval::registerOptVocab(
    {"frame"}, {{"into"},
     {}});

const piet::eval::OptVocab& kVocabFrlink = piet::eval::registerOptVocab(
    {"frlink"}, {{"frame", "generate", "gen"},
     {}});

const piet::eval::OptVocab& kVocabFrget = piet::eval::registerOptVocab(
    {"frget"}, {{"from"},
     {}});

// B4-W3b: `force` joins the KNOWN set. In Stata it is an ACKNOWLEDGEMENT, not
// a modifier — `duplicates drop VARLIST` requires it precisely because dropping
// on a narrowed key discards rows that differ elsewhere, and Stata makes you
// say so. It changes no value: `duplicates drop foreign, force` on
// `sysuse auto` is rc 0 with _N 74 -> 2 (pins_oracle_g.log:250-256), the
// same rows this build's varlist path already keeps. So it is accepted and
// read as the acknowledgement it is, rather than rejected as unknown.
// `ge` prefixes both registered spellings, so it is declared too: duplicates
// tag's Generate() takes it (aer_w2 w2b_dataio duptag_ge(dup) rc 0).
// BUG-1096: examples and list hand every other option to `list` (duplicates.ado
// `[, *]`), so list's own options are real there ([D] list's syntax table;
// StataBE 19.5 runs `duplicates examples s, sepby(s)`, `ab(4)`, `clean`, `nol`,
// `sep(0)` and `N`, and refuses `bogus` as list does). This build draws list's
// default table only: they are the unimplemented set, refused by name on those
// two subcommands and not allowed on the others. `ab` and `sep` are declared
// because each prefixes two of them (absolute; sepby, sepbyexp).
const piet::eval::OptVocab& kVocabDuplicates = piet::eval::registerOptVocab(
    {"duplicates"}, {{"gen", "generate", "force"},
     {"compress", "nocompress", "fast", "abbreviate", "string", "noobs", "fvall", "table",
      "display", "header", "noheader", "footer", "clean", "divider", "separator", "sepby",
      "sepbyexp", "ds", "nolabel", "mean", "sum", "n", "labvar", "constant", "notrim",
      "absolute", "relative", "nodotz", "subvarname", "linesize"},
     {{"g", "generate"}, {"ge", "generate"}, {"ab", "abbreviate"}, {"sep", "separator"}}});

const piet::eval::OptVocab& kVocabLevelsof = piet::eval::registerOptVocab(
    {"levelsof"}, {{"local", "clean", "missing",
                                                                        "separate"},
     {}});

const piet::eval::OptVocab& kVocabWinsor2 = piet::eval::registerOptVocab(
    {"winsor2"}, {{"cuts", "suffix", "by", "trim", "replace"},
     {}});

// B4-W3b: `generate(stub)` (13 corpus uses) joins the KNOWN set — it now
// creates the level dummies rather than answering r(198).
// B5-D-TAB-DISPLAY: `sort`/`cell`/`col`/`row`/`nofreq` join the KNOWN set too
// (all five rc 0 in StataBE 19.5, tests/eval/golden/aer_b4/
// b5_importcase_pins.log:103-243; oracle receipts: `B5REC|tabulate|sort|0|`
// `B5REC|tabulate|cell|0|` `B5REC|tabulate|nofreq|0|` `B5REC|tabulate|col|0|`
// `B5REC|tabulate|row|0|`). The oracle captures rc ONLY — it does NOT capture
// the rendered table text for any of these five, and PIET's own table
// renderer lives in the frontend (piet/frontend/**, out of scope this lane).
// `sort` is the one with an observable, cheap, IN-SCOPE effect (row order in
// the JSON `table.rows` this file already builds), so it is genuinely
// implemented — see runTabulate/tabulateOne's `sortByFreq` param.
// `cell`/`col`/`row` (percentage lines) and `nofreq` (suppress the frequency
// line) are accepted as VALIDATED NO-OPS, deliberately: this backend never
// invents percentage-line formatting to pin against an oracle cell that does
// not exist, and there is no renderer here (or byte-pin) for this lane to
// wire one into anyway. The rendered output under these four options is
// UNMEASURED and this build does NOT claim Stata parity for that layout —
// only rc and the option vocabulary are.
// W1a-1 (2026-09-14): `nolabel` and `nokey` join the KNOWN set. Both are rc 0
// in StataBE 19.5 at the full spelling (tests/eval/golden/aer_w1/
// w1a_optmin_pins3.log), and their ABSENCE is what BUG-485 was made of: with
// neither in the net, `no` looked like a unique prefix of `nofreq`. `nolabel`
// ([R] tabulate oneway: "display numeric codes rather than value labels",
// minimum `nol` -- measured `n`/`no` r(198), `nol` rc 0) is IMPLEMENTED: the
// arm below skips the BUG-355 value-label substitution. `nokey` (the two-way
// key line printed above cell/col/row percentages) is a validated no-op for
// the same reason cell/col/row are: this build draws no key to suppress.
// BUG-1514: the two-way statistics lrchi2, V (keys arrive lower-cased: `v`), gamma
// and taub, and `all` (= chi2 lrchi2 V gamma taub), are implemented (runTabulate's
// twoWayStats). `column` is the full name of `col` (StataBE 19.5 `tabulate c d,
// column row` rc 0; `co`, `colu`, `colum` rc 0 too), declared as aliases so `co`
// stays a unique prefix of `col`. `wrap` ("do not wrap wide tables") holds as
// written: this table is never wrapped. Fisher's `exact` (and `exact(#)`, whose #
// only sizes FEXACT's workspace) is served on 2 x 2 tables and refused by name on
// larger ones (tabulate.cpp); its minimum is `ex` (StataBE `tabulate c d, ex` rc 0,
// `exp` is expected). `nolog`, the FEXACT enumeration log's switch, is allowed
// only with it (`tabulate c d, chi2 nolog` r(198)); `nol` stays nolabel through
// the measured minimums table. The cell-content options cchi2, clrchi2 and
// expected are real options this build does not compute, refused by name.
const piet::eval::OptVocab& kVocabTabulate = piet::eval::registerOptVocab(
    {"tabulate", "tab", "ta", "tab1", "tab2"},
    {{"chi2", "sum", "summarize", "missing", "generate", "sort", "cell", "col", "row", "nofreq",
      "nolabel", "nokey", "matcell", "matrow", "matcol", "lrchi2", "v", "gamma", "taub", "all",
      "wrap", "exact", "nolog"},
     {"cchi2", "clrchi2", "expected"},
     {{"column", "col"}, {"colu", "col"}, {"colum", "col"}, {"ex", "exact"}}});

const piet::eval::OptVocab& kVocabTable = piet::eval::registerOptVocab(
    {"table"}, {{"statistic", "stat", "contents"},
     {},
     {{"c", "contents"}}});
  // BUG-1015: c() is table_16's own spelling of contents()
// B4-W3b: `nototal` and `save` join the KNOWN set — both change which numbers
// appear (`nototal` drops the overall row, `save` publishes r(Stat#)/r(name#)),
// which is exactly why they were rejected before and exactly why they are
// implemented rather than accepted-and-ignored now.
// tabstat: `f` is format's declared minimum
// abbreviation (w2d_uncovered.log tabstat_f_abbrev_077 `f(%9.3g)`, rc 0).
const piet::eval::OptVocab& kVocabTabstat = piet::eval::registerOptVocab(
    {"tabstat"}, {{"by", "stat", "stats",
                   "statistics", "columns", "nototal", "save", "format"},
     {},
     {{"f", "format"}}});

const piet::eval::OptVocab& kVocabCorr = piet::eval::registerOptVocab(
    {"corr", "cor", "pwcorr", "correlate"}, {{"pairwise", "sig", "obs", "covariance"},
     {}});


// ===========================================================================
// AER scope-2 W2 — THE OPTION-VOCABULARY REGISTRATION SWEEP.
//
// WHAT THIS IS FOR, and it is not the same thing BUG-233 was for. BUG-233
// registered the vocabularies of nets that already CALLED
// eval::rejectUnknownOptions, so the dispatcher could canonicalize their keys.
// Everything below is the other population: commands whose option surface the
// registry could not see AT ALL, so `piet_native --dump-optvocab --resolve
// <cmd> <opt>` answered `vocab_registered:false` for every spelling — the
// census's UNMEASURED answer, not a gap and not a clean bill either.
//
// Measured on the 12 AER packages the morning of 2026-09-09, before this block:
//   option_pairs=596 known=153 unknown=74 unimplemented=20 no_vocabulary=349
// `no_vocabulary=349` is 349 (command, option) pairs the papers really use
// where the product could not say whether it knows the option.
//
// THE CLASSIFICATION RULE, which is the whole point and is not negotiable:
//   `known`         = ONLY the option keys the handler ACTUALLY READS. Every
//                     row below was produced by grepping the named handler
//                     body for that key; the file:line of the read is cited.
//   `unimplemented` = a real option of that command which the handler does not
//                     read (an honest refusal, and the bucket W3/W4/W5 empty).
//   neither         = the product does not know the spelling at all; the census
//                     reports it `unknown`, which is a MEASURED answer.
// An option wrongly in `known` is a silent wrong answer — the product claiming
// to honour something it ignores. When in doubt this block refuses.
//
// THE ONE SIDE EFFECT, stated rather than buried. Registering a vocabulary
// arms eval::canonicalizeForCommand for that command, so a >=2-char unique
// prefix of a `known` key now reaches the handler under its canonical
// spelling. For the nets already calling rejectUnknownOptions that is
// bit-neutral (errors.h's one-sidedness argument: the net resolved the same
// prefixes itself). For the HAND-ROLLED nets below (regress, xtreg,
// summarize, save, predict, estimates table) it is not: their loops compare
// raw keys, so `regress y x, vc(robust)` was rejected before and is honoured
// now. That is BUG-231's own repair direction — accepted-then-dropped is the
// defect, not the fix — but it is a real widening and is called out here so
// the next audit does not have to rediscover it.
//
// Commands deliberately NOT registered here, each with its reason:
//   odbc            designed refusal (registry.cpp:1241 `refused:no_odbc_driver`).
//   gtools, dropmiss  not commands in this build at all (absent from
//                   --dump-registry's 299 rows), so they answer refusedCommand;
//                   an option vocabulary for a refused name would be a claim
//                   about a surface that does not exist.
//   the graph family  W4's (see the graph arm's own note).

// --- the BUG-336 batch, promoted from inline literals ----------------------
// These sets are UNCHANGED. Each net below now reads its own registration
// instead of a brace literal, so the vocabulary is written once and the
// dispatcher can see it; the accepted/rejected sets are byte-identical.
const piet::eval::OptVocab& kVocabCodebook = piet::eval::registerOptVocab(
    {"codebook"}, {{"compact"},
     {"mv", "tabulate", "notes", "detail", "all", "header", "dots", "languages", "problems"}});

// B5-D5: `fullnames` moves to known -- a VALIDATED NO-OP, not a semantics
// change. The describe block always carries the FULL, untruncated variable
// name (handlers.cpp's describeEntry: entry["name"] = name;) and the one
// renderer sizes its name column to the longest name present
// (console.js:formatDescribe, nameW = Math.max(13, ...v.name.length)) -- PIET
// never abbreviates, so `fullnames` is satisfied by construction. Measured
// B4REC|describe|fullname|0| (tests/eval/golden/aer_b4/pins_oracle_r.log:
// 130-131) and the in-tree StataBE rendered table at tests/eval/golden/
// f16_tw_transport_oracle.log:43,85 (`describe, fullnames`). short/simple/
// numbers/detail stay unimplemented -- those DO change what prints.
// short/simple/varlist/replace/clear (aer_w2 w2b_dataio log:162-238): short
// prints the header and posts r(N) r(k) r(changed); simple lists the names;
// varlist posts r(varlist) and r(sortlist); replace [clear] makes the
// description the dataset.
const piet::eval::OptVocab& kVocabDescribe = piet::eval::registerOptVocab(
    {"describe"}, {{"fullnames", "short", "simple", "varlist", "replace", "clear"},
     {"numbers", "detail"}});

// rename's 1:1 and wildcard forms read no option at all and Stata's own 1:1
// form has none either (dispatcher.cpp's rename arm). Registering the EMPTY
// vocabulary is not a no-op: it is the difference between "we looked and there
// are none" and "nobody has looked".
// B4-W3b: the GROUP form's case options (29 corpus uses of `rename _all,
// lower` / `rename *, upper`). Measured rc 0 on StataBE 19.5
// (pins_oracle_g.log:101-105), which also shows the note Stata prints when
// nothing moves: "(all newnames==oldnames)".
const piet::eval::OptVocab& kVocabRename = piet::eval::registerOptVocab(
    {"rename"}, {{"lower", "upper", "proper", "renumber", "addnumber", "replace"},
     {}});

const piet::eval::OptVocab& kVocabGenerate = piet::eval::registerOptVocab(
    {"generate"}, {{},
     {"before", "after"}});

// BUG-471 T2: replace's one option (generate.sthlp: `replace ... [, nopromote]`;
// w2e_float_default.log replace_byte_nopromote :827). It used to be dropped on
// the floor with every other option, so `nopromote` changed nothing.
const piet::eval::OptVocab& kVocabReplace = piet::eval::registerOptVocab(
    {"replace"}, {{"nopromote"},
     {}});

const piet::eval::OptVocab& kVocabNlGmm = piet::eval::registerOptVocab(
    {"nl", "gmm"}, {{"instruments", "onestep", "vce", "vcov", "robust", "cluster", "level"},
     {}});

const piet::eval::OptVocab& kVocabMixed = piet::eval::registerOptVocab(
    {"mixed"}, {{"vce", "vcov", "robust", "cluster", "level"},
     {}});

// BUG-1500: `twostep`, ivprobit's Newey two-step estimator (runIvMl); ivtobit's
// two-step form is refused there by name.
const piet::eval::OptVocab& kVocabIvMl = piet::eval::registerOptVocab(
    {"ivprobit", "ivtobit"}, {{"vce", "vcov", "robust", "cluster", "ll", "ul", "level", "twostep"},
     {}});

const piet::eval::OptVocab& kVocabFracreg = piet::eval::registerOptVocab(
    {"fracreg"}, {{"vce", "vcov", "robust", "cluster", "level"},
     {}});

const piet::eval::OptVocab& kVocabCompress = piet::eval::registerOptVocab(
    {"compress"}, {{},
     {"nocoalesce"}});

// `stable` keeps tied rows in their current order; PIET's sort is always
// stable (the engine's radix gsort), so the option is served as written
// (w2_smalls.log:148 sort_stable rc 0).
const piet::eval::OptVocab& kVocabSort = piet::eval::registerOptVocab(
    {"sort"}, {{"stable"},
     {}});

const piet::eval::OptVocab& kVocabGsort = piet::eval::registerOptVocab(
    {"gsort"}, {{},
     {"generate", "mfirst"}});

// data-verbs-effects-rename-recode-label: recode's options per [D] recode's
// syntax diagram are g:enerate(), into(), l:abel(), pre:fix(), copy:rest and
// t:est. `g' (w2ah_data_verbs.log ah_recode_g_lab, rc 0) cannot resolve by
// prefix, so it is declared, as are `l' and `t' (BUG-1141); runRecode reads them
// all, and the prefix/copyrest minimums are floor rows.
const piet::eval::OptVocab& kVocabRecode = piet::eval::registerOptVocab(
    {"recode"}, {{"generate", "into", "label", "prefix", "copyrest", "test"},
     {},
     {{"g", "generate"}, {"l", "label"}, {"t", "test"}}});

// B5-D6: `separator` moves to known -- runList renders a GUI grid (same
// validated-no-op class as noobs/clean just above: the grid draws no
// every-N-rows rule either way, so accepting and discarding the count changes
// nothing observable). Measured B4REC|list|sep(0)|0| (tests/eval/golden/
// aer_b4/pins_oracle_r.log:143-144) and the in-tree StataBE rendered
// table at pins_oracle_h.log:94 / pins_oracle_i.log:126 (`list ...,
// sep(0) noobs`). `sep` is a prefix of BOTH `separator` and `sepby`, so the
// unique-prefix rule alone leaves it ambiguous -> r(198) even after this
// move; the aliases row below is the only mechanism that can carry it
// (BUG-430, kVocabPredict precedent). `sepby` stays unimplemented -- it takes
// a varlist and has no rendering support.
// W1a-1 (2026-09-14): `abbreviate(#)` moves from unimplemented to KNOWN. [D]
// list: "abbreviate variable names to # display columns; default is ab(8)" --
// a width knob for the TEXT header. Measured: `list price in 1/2, ab(25)` rc 0
// and `a` r(198) (tests/eval/golden/aer_w1/w1a_optmin_pins.log), so the
// minimum is 2; `ab` collides with `absolute` only in THIS net, not in Stata,
// which is why the generated minimum carries a re-adjudication (gen_table.py).
// runList renders a grid whose column heads are never abbreviated, so the
// value is validated (strict integer, the separator idiom) and then dropped:
// the corpus spelling is `abbreviate(40)` -- "do not shorten" -- which is what
// the grid already does. A # smaller than a name would shorten Stata's header
// and not ours; that layout is UNMEASURED and this build does not claim it.
const piet::eval::OptVocab& kVocabList = piet::eval::registerOptVocab(
    {"list"}, {{"noobs", "clean", "separator", "abbreviate"},
     {"mean", "sum", "noheader", "nolabel", "sepby", "divider",
      "table", "display", "nocompress", "compress", "fast", "header", "notrim", "nodotz",
      "absolute", "subvarname"},
     {{"sep", "separator"}}});

// B5-D3/D4: BUG-475 -- `label define LB ...` without add/modify/replace on an
// EXISTING label now rejects r(110) ("label already defined", matching
// Stata's own text -- oracle_r.log:58), and `replace` moves from
// unimplemented into known so the wholesale-redefine spelling itself is
// accepted (B4REC|label_define|<redefine_no_replace>|110| and
// B4REC|label_define|replace|0|, tests/eval/golden/aer_b4/
// pins_oracle_r.log:60-66). KNOWN NARROWING: runLabelDefine re-parses
// the RAW option tail by substring match (same as the pre-existing add/modify
// scan), so an ABBREVIATED `repl`/`repla`/... reaches this net (now resolved
// to "replace" by the dispatcher's prefix rule) but is invisible to the raw
// scan and falls through to the r(110) guard -- a pre-existing class of gap
// (the same is true of `mod`/`ad` today), recorded rather than fixed here.
// BUG-524: `label language`'s new/rename/delete join the unimplemented set so
// the refusal names them as valid Stata (rc 198), instead of "not allowed".
// BUG-527: `label language NAME, new` is intercepted BEFORE this net runs
// (oracle rc 0, arm lang_new) -- `new` stays listed here so `rename`/`delete`
// (unmeasured) and `, new` on every OTHER label subcommand still get the
// honest "not yet implemented" wording instead of "not allowed".
const piet::eval::OptVocab& kVocabLabel = piet::eval::registerOptVocab(
    {"label"}, {{"add", "modify", "replace", "nofix"},
     {"new", "rename", "delete"}});

const piet::eval::OptVocab& kVocabLabelbook = piet::eval::registerOptVocab(
    {"labelbook"}, {{},
     {"alpha", "length", "list", "problems", "detail"}});

// import/export: the net is per (command, subcommand), so the four branch sets
// stay the literals the net uses and the REGISTRATION is their union per
// command token. Unioning across subcommands is the registry's documented
// behaviour (errors.h: csdid and its estat sub-net), and the branch net is
// still the authority on what is accepted.
// export delimited's grammar: DELIMiter(string) NOVARnames NOLABel DATAFmt
// Quote REPLACE. novarnames/datafmt/quote are written by runExportDelimited
// (w2ar dx_exd_novarnames log:501, dx_exd_datafmt :629, dx_exd_quote :703).
const std::unordered_set<std::string> kExportDelimKnown = {"delimiter", "delim", "nolabel",
                                                           "replace", "quote", "novarnames",
                                                           "datafmt"};

const std::unordered_set<std::string> kExportDelimUnimpl = {};

// AER scope-2 W3a: `sheet()` and `sheetreplace`/`sheetmodify` land; `cell()`
// (a start cell for the rectangle) and `keepcellfmt` (needs a styles part this
// writer does not author) stay named gaps.
// keepcellfmt is accepted: this writer authors no styles part, so there is no
// cell format to keep, and a sheetreplace keeps every other part of the
// workbook verbatim (w2ar dx_exx_sheetreplace_keep rc 0, log:1207).
const std::unordered_set<std::string> kExportExcelKnown = {"replace", "nolabel", "firstrow",
                                                           "sheet", "sheetreplace", "sheetmodify",
                                                           "keepcellfmt"};

// `datestring()`/`missing()` are deliberately NOT listed: adding `datestring`
// would make `da` an ambiguous prefix against export delimited's `datafmt` in
// the per-command union, trading one honest message for a worse one on a
// spelling that already resolved.
const std::unordered_set<std::string> kExportExcelUnimpl = {"cell", "locale"};

// AER scope-2 W3a: `varnames`/`rowrange`/`colrange`/`numericcols`/`encoding`
// land. BUG-477: `case` for DELIMITED lands too -- it is a DIFFERENT rule
// from excel's (delimited's own default is `lower`, not `preserve` --
// b5_importcase_pins.log) but the same three-value grammar, so it moves to
// KNOWN alongside them. `asdouble`/`bindquote`/`stringcols` stay named gaps.
// 2026-10-21: every option help import delimited documents is read by the
// native reader (delimited_io.cpp): DELIMiters (with `delimiter`/`delim` kept as exact
// spellings), BINDQuotes, STRIPQuotes, MAXQUOTEDRows, STRINGCols, NUMERICCols,
// ASDOUBle, ASFLOat, COLLAPSEDelimiters, EMPTYLines, CHARSET (w2ar
// log:2570-3513). The locale/separator knobs stay named gaps.
const std::unordered_set<std::string> kImportDelimKnown = {
    "delimiter",  "delim",       "delimiters",  "clear",      "varnames",
    "rowrange",   "colrange",    "numericcols", "stringcols", "encoding",
    "charset",    "case",        "bindquotes",  "stripquotes", "maxquotedrows",
    "asdouble",   "asfloat",     "collapsedelimiters", "emptylines"};

const std::unordered_set<std::string> kImportDelimUnimpl = {
    "favorstrfixed", "parselocale", "groupseparator", "decimalseparator"};

// AER scope-2 W3a: this is the highest-frequency option cluster in the whole
// twelve-paper corpus (`import excel` appears 273 times, 243 in paper 015).
// locale(): the reader decodes the workbook's own UTF-8 XML whatever the
// locale names, so it is accepted (w2ar dx_imx_locale `locale("UTF-8")` rc 0,
// log:2055).
const std::unordered_set<std::string> kImportExcelKnown = {
    "firstrow", "clear", "sheet", "cellrange", "allstring", "case", "describe", "locale"};

const std::unordered_set<std::string> kImportExcelUnimpl = {};

std::unordered_set<std::string> unionOf(const std::unordered_set<std::string>& a,
                                        const std::unordered_set<std::string>& b) {
  std::unordered_set<std::string> out = a;
  out.insert(b.begin(), b.end());
  return out;
}

// `delimi`/`delimit`/`delimite` are DELIMiters abbreviations the prefix rule
// finds ambiguous (they also prefix the exact spelling `delimiter`).
const piet::eval::OptVocab& kVocabImport = piet::eval::registerOptVocab(
    {"import"}, {unionOf(kImportDelimKnown, kImportExcelKnown),
     unionOf(kImportDelimUnimpl, kImportExcelUnimpl),
     {{"delimi", "delimiters"}, {"delimit", "delimiters"}, {"delimite", "delimiters"}}});

// Declared spellings the prefix rule cannot reach: export excel's
// `SHeet(string asis)` (`sh` also prefixes sheetreplace/sheetmodify; w2ar
// dx_exx_sheet_sub_replace `sh("2", replace)` rc 0, log:1259) and
// export delimited's `Quote` (one letter, below the 2-char floor).
const piet::eval::OptVocab& kVocabExport = piet::eval::registerOptVocab(
    {"export"}, {unionOf(kExportDelimKnown, kExportExcelKnown),
     unionOf(kExportDelimUnimpl, kExportExcelUnimpl),
     {{"sh", "sheet"}, {"q", "quote"}}});

// Item 5 (User ruling 2026-09-09): `post` moves from `unimplemented` to
// `known` — it now really is implemented (estimation_m4c.cpp's runNlcom).
// `nlcom` is NOT in kOptMinComplete (generated/opt_minimums_bug264.inc), so
// this arms the generic >=2-char unique-prefix rule for `post` (`po`/`pos`
// would start resolving) — an UNMEASURED widening, not captured by any
// oracle cell here; recorded rather than floored (no StataBE measurement of
// `nlcom ..., po` to floor against). `lincom`'s own inline `{"post"}`
// unimplemented list (this file, the lincom arm above) is the same class
// and is DELIBERATELY untouched — out of scope for this item.
const piet::eval::OptVocab& kVocabNlcom = piet::eval::registerOptVocab(
    {"nlcom"}, {{"level", "post"},
     {}});

// B4-W3b: `generate` moves to KNOWN — it now writes the original/copy marker.
const piet::eval::OptVocab& kVocabExpand = piet::eval::registerOptVocab(
    {"expand"}, {{"generate"},
     {}});

// item3 (this lane, BUG-477 brief): `unmatched` moves to KNOWN -- it is a real
// Stata joinby option, and `unmatched(none)` is a legal VALUE this build can
// answer honestly today (see the dispatch site's own comment for why). The
// NAME is accepted here; the VALUE is what the dispatch site still polices --
// `master`/`both` stay named, honest gaps.
const piet::eval::OptVocab& kVocabJoinby = piet::eval::registerOptVocab(
    {"joinby"}, {{"unmatched", "_merge"},
     {"nolabel", "update", "replace"}});

// `g' and `ge' (w2ah_data_verbs.log ah_encode_g: `encode es, g(sector)' at rc
// 0) are below the prefix rule's 2-character floor or ambiguous, so declared.
// BUG-1135: isid.ado's `syntax varlist [, Sort Missok]`; the one-letter
// minimums are below the prefix floor, so declared.
const piet::eval::OptVocab& kVocabIsid = piet::eval::registerOptVocab(
    {"isid"}, {{"sort", "missok"},
     {},
     {{"s", "sort"}, {"m", "missok"}}});

const piet::eval::OptVocab& kVocabEncode = piet::eval::registerOptVocab(
    {"encode"}, {{"generate", "label", "noextend"},
     {},
     {{"g", "generate"}, {"ge", "generate"}}});


// --- the hand-rolled nets: registered, NOT rewired --------------------------
// Each of these rejects unknown options with its own `k == "..."` loop rather
// than through eval::rejectUnknownOptions, so there is no set to bind a
// reference to. The vocabulary is transcribed here from the loop, the read
// site is cited, and testAerW2OptVocabTwoWay() pins the transcription against
// the live dispatcher — a drifted copy fails the selftest, it does not rot.
//
// regress/feols: handlers.cpp:1571-1615 reads vce/vcov/robust/cluster/
// noconstant/nocons/level/absorb; :1645 names `beta` as the real-but-unwired
// one. `keepsingletons` is deliberately in NEITHER set: handlers.cpp:1638
// rejects it outright for regress/feols (real Stata has it on reghdfe only),
// and reghdfe's own vocabulary is kVocabIvRegress (estimation_m4.cpp:141).
// BUG-473: `absorb` stays in this shared set (so `ab()` keeps resolving to
// the full spelling for the reject message) but is gated by the ACTUAL Stata
// word the user typed, same shape as `keepsingletons` — handlers.cpp's own
// reject net answers r(198) "option absorb() not allowed" when stataCmd ==
// "regress", and still builds the FE model for feols/reghdfe/areg.
const piet::eval::OptVocab& kVocabRegress = piet::eval::registerOptVocab(
    {"regress", "feols"}, {{"vce", "vcov", "robust", "cluster", "noconstant", "nocons",
                            "level", "absorb"},
     {"beta"}});

// xtreg: estimation_m4.cpp:1316-1324 reads fe/re; :1386 the RE vcov guard;
// the shared builder at :702-725 reads noconstant/nocons/level and :617
// absorb. :1303-1310 refuses gls/mle/ec1/pa BY NAME as real xtreg
// estimators this build does not have — the definition of unimplemented;
// `be` fits (BUG-1515), its `wls` is refused by name.
const piet::eval::OptVocab& kVocabXtreg = piet::eval::registerOptVocab(
    {"xtreg"}, {{"fe", "re", "be", "vce", "vcov", "robust", "cluster", "noconstant", "nocons",
                 "level", "absorb"},
     {"gls", "mle", "ec1", "pa", "wls"}});

// summarize: the arm reads detail/d (the `detail` bool) and separator (the
// renderer field). `d` is BELOW resolveOptionAbbrev's 2-char floor and so
// unreachable by the prefix rule — a DECLARED alias (the BUG-430 mechanism),
// and the in-tree evidence for the declaration is the arm's own
// `k == "detail" || k == "d"` plus registry.cpp's summarize row, whose
// known_options list is literally {detail, d, meanonly}.
//
// `meanonly` IS read by that arm and is DELIBERATELY NOT REGISTERED. Putting a
// key in `known` arms the generic 2-char unique-prefix rule for it, and
// BUG-264 measured StataBE 19.5 refusing `summarize x, mea` — meanonly's
// minimum is FOUR (generated/opt_minimums_bug264.inc:107). summarize is not in
// kOptMinComplete (detail/separator were never probed), so the narrowing flip
// that would honour the measured minimum is unavailable here, and registering
// meanonly would make PIET accept a spelling Stata rejects. An unmeasured
// widening is fabricated parity, so the census reads `summarize|meanonly` as
// `unknown` — an under-claim, which is the safe direction, and 1 use across the
// 12 papers. The fix is upstream of W2: adjudicate summarize's remaining
// options so it can join kOptMinComplete. testBUG264 pins both sides today.
const piet::eval::OptVocab& kVocabSummarize = piet::eval::registerOptVocab(
    {"summarize"}, {{"detail", "separator"},
     {},
     {{"d", "detail"}}});

// save: dispatcher.cpp's save arm re-parses the RAW tail, so canonicalization
// cannot reach it — registering is pure measurement here, exactly the case the
// W2 brief carves out. `replace`/`emptyok` are the spellings the loop reads;
// the three remaining unimplemented names are the arm's OWN reject message,
// not a guess.
// B5-D9: `emptyok` accepted as a validated no-op. Measured
// B4REC|save|replace_emptyok|0| (tests/eval/golden/aer_b4/pins_oracle_r.log
// :90-93) on `clear` + `set obs 0` + `save F, replace emptyok`. runSetObs's
// zero-data-in-memory branch (handlers_m3.cpp:runSetObs) already loads a real
// {n_rows:0, columns:[]} hbin at that point, so runSaveData's only gate
// (session.hasData()) already passes and the ordinary copy-the-current-hbin
// path already writes a valid empty file — there is no refusal here for
// `emptyok` to relax, unlike saveold's emptyok (handlers_aer_iotext.cpp:
// 163-166), which stays unimplemented because saveold's writer DOES drop
// metadata under nolabel/orphans/emptyok. `save` has no such lossy path.
const piet::eval::OptVocab& kVocabSave = piet::eval::registerOptVocab(
    {"save"}, {{"replace", "emptyok"},
     {"nolabel", "orphans", "version"}});

// predict: a CENSUS row. No rejectUnknownOptions call reads kVocabPredict;
// runPredict (estimation_m4c.cpp) refuses by name itself. The registry row feeds
// canonicalizeForCommand (prefix and alias resolution), the floor table's exact-
// spelling test and `piet_native --dump-optvocab`. FU-19: `known` is the set
// runPredict serves -- xb/resid/residuals/fe (kSupported), pr/n after
// logit/probit/poisson (its glmPredict arm), and predictServesM4's four family
// sets (regress: score stdp stdf stdr rstandard rstudent hat|leverage; areg:
// dresid xbd d; xtreg,fe: xbu u e ue). `unimplemented` keeps what it still
// refuses (cooksd, which selftest.cpp pins, dfbeta, the GLM residual family,
// equation). The abbreviations predictStatisticM4 also reads (rsta rstu lev sc
// dr) are not listed: the prefix rule already resolves each to a known name,
// so an alias would break BUG-430's "only what the prefix rule cannot produce",
// and as names they would join the prefix search -- `le`, Stata's
// {opt l:everage} (regress_postestimation's help), resolved to leverage
// today, would turn ambiguous between `lev` and `leverage`.
// B4 P1: the three DECLARED abbreviations of `residuals`, and why the prefix
// rule cannot produce them. `resid` and `residuals` are both in `known` — an
// INTERNAL synonym pair, one Stata option this arm reads under two spellings —
// so `re`/`res`/`resi` prefix two members and the uniqueness test calls them
// ambiguous. They then fell through to the unimplemented vocabulary, where
// `response` is the unique `re`/`res` match, and PIET refused naming a
// statistic the user never typed.
//
// StataBE resolves all three to `residuals` and runs: oracle_p.log:87-90
// (`res`), :92-95 (`re`), :97-100 (`resi`), rc 0 every time. This is exactly
// what OptVocab::aliases is for (BUG-430) — a spelling the prefix rule cannot
// reach, declared instead of guessed — and each row here is one captured line.
const piet::eval::OptVocab& kVocabPredict = piet::eval::registerOptVocab(
    {"predict"}, {{"xb", "resid", "residuals", "fe", "pr", "n", "score", "stdp", "stdf", "stdr",
                   "rstandard", "rstudent", "leverage", "hat", "dresid", "xbd", "d", "xbu", "u",
                   "e", "ue"},
     {"cooksd", "dfbeta", "anscombe", "deviance", "pearson", "response", "working",
      "equation"},
     {{"re", "residuals"}, {"res", "residuals"}, {"resi", "residuals"}}});

// ivreg2/xtivreg2: handlers_aer_estim.cpp:326-338 consumes r/robust/cl/cluster/
// gmm2s/fe/small and passes everything else to runIvRegress, whose net is
// kVocabIvRegress (estimation_m4.cpp:141) — so the known set is the union of
// the two. The unimplemented names are the ivreg2 options the 12 papers use
// that neither layer reads (sfirst/saverfprefix); each
// is corpus-attested Stata that ran, not a recollection. FU-252: `first` and
// `partial` are read by runIvRegress since w2b-iv-options (estimation_m4b.cpp
// wantFirst/wantPartial, w2b_estim.log ivreg2_partial), so they are known, and so
// are ffirst/savefirst/savefprefix for ivreg2 (wantFfirst/wantSavefirst,
// w2d_uncovered.log ivreg2_ffirst_savefirst_068). A census row like kVocabPredict:
// no rejectUnknownOptions call reads it.
const piet::eval::OptVocab& kVocabIvreg2 = piet::eval::registerOptVocab(
    {"ivreg2", "xtivreg2"}, {{"r", "robust", "cl", "cluster", "gmm2s", "fe", "small",
                              "vce", "vcov", "gmm", "absorb", "noconstant", "nocons", "level",
                              "first", "partial", "ffirst", "savefirst", "savefprefix"},
     {"sfirst", "saverfprefix"}});

// estimates/eststo: `estimates table` is the one sub-form carrying options and
// eststo_esttab.cpp:938-940 reads b/se/stats. B4-W3b moved `title` and
// `replace` from "unimplemented"/"absent" into the KNOWN set, because both are
// now read: `estimates store NAME, title("…")` (73 corpus uses) lands on the
// snapshot and prints in `estimates dir`, and `estimates save F, replace` (86
// uses) is the overwrite guard. The net itself is applied PER SUB-VERB at the
// routing site below — a flat union would let `estimates store m1, replace`
// through, which Stata rejects.
const piet::eval::OptVocab& kVocabEstimates = piet::eval::registerOptVocab(
    {"estimates", "eststo"}, {{"b", "se", "stats", "title", "replace"},
     {}});

// W2 eststoprefix (BUG-520): `eststo`'s OWN option list, which is not
// `estimates store`'s. It is a community command, so the package's spelling is
// the contract -- eststo.ado 1.1.0's syntax line reads
//   [name] [, Title() Prefix(name) Refresh Refresh2() ADDscalars() noEsample
//            noCopy MISsing svy] [: command]
// `prefix()` is READ: w2_eststo_prefix.log A7 (`eststo, prefix(zz): regress y w`
// is rc 0 and answers "(zz9 stored)"). The others are real options this build
// does not implement, so they get the "valid Stata option" wording rather than
// "not allowed". Registered under the `eststo` token ONLY -- `estimates store
// m1, prefix(zz)` stays rejected by its own per-sub-verb net below.
const piet::eval::OptVocab& kVocabEststoStore = piet::eval::registerOptVocab(
    {"eststo"}, {{"title", "prefix"},
     {"refresh", "addscalars", "noesample", "nocopy", "missing", "svy"}});


// --- registered, and no net exists to bind ---------------------------------
// Measured 2026-09-09 by driving `<cmd> ..., zzzfake` through --do: these six
// answer rc 0 and silently drop the token. Registering does not change that
// (the W2 brief forbids adding a net where the handler re-parses raw), it only
// makes the surface visible to the census. The missing net is the finding.
//
// collapse: handlers_data_verbs.cpp:1020 pulls `by(...)` out of the RAW line
// with its own regex; nothing reads `fast`. BUG-477/item2: `fast` moves to
// KNOWN -- it was classified unimplemented even though the handler already
// answers rc 0 for it (this build has no "original data" copy to skip
// restoring, so `fast`'s only real effect in Stata is a no-op acknowledgement
// here too). Measured cell-for-cell against `collapse (mean) price,
// by(foreign) fast`: B5REC|collapse|fast|0| / B5VAL|rows_collapse|2|x|0
// (tests/eval/golden/aer_b4/b5_importcase_pins.log) -- this build already
// answers both.
// collapse's syntax line is BY(varlist) CW FAST CLABEL frame(name):
// runCollapse reads `cw` and `clabel` off the option keys the dispatcher
// resolved (BUG-962, BUG-1100), and frame(name) is parsed and then never read
// by collapse_17 -- StataBE 19.5 collapses in memory and makes no frame -- so
// the collapse arm checks the name and drops it. The arm runs this net: an
// unknown word is r(198) (w2au_collapse_family.log cz_c_err_opt).
const piet::eval::OptVocab& kVocabCollapse = piet::eval::registerOptVocab(
    {"collapse"}, {{"by", "cw", "fast", "clabel", "frame"},
     {}});

// file: file_io.cpp:153-160 greps the raw option tail for read/write/append/
// replace and refuses `binary` by name. `text` is known on the import/`clear`
// precedent (dispatcher.cpp's import net): this build's file I/O is
// unconditionally text mode, so the flag's semantic is satisfied, not dropped.
const piet::eval::OptVocab& kVocabFile = piet::eval::registerOptVocab(
    {"file"}, {{"read", "write", "append", "replace", "text"},
     {"binary"}});

// tsfilter: handlers_aer_estim.cpp:1046-1048 reads trend/smooth.
const piet::eval::OptVocab& kVocabTsfilter = piet::eval::registerOptVocab(
    {"tsfilter"}, {{"trend", "smooth"},
     {}});

// Three env/session verbs whose handlers take no option vector at all
// (runNet/runSsc, and `set`'s note arm). Each unimplemented name is the
// spelling the 12 papers actually use, so none of them is recalled:
//   net install ftools, from("https://…")      ssc install `pkg', replace
//   set more off , perm
const piet::eval::OptVocab& kVocabNet = piet::eval::registerOptVocab(
    {"net"}, {{},
     {"from"}});

const piet::eval::OptVocab& kVocabSsc = piet::eval::registerOptVocab(
    {"ssc"}, {{},
     {"replace"}});

// B5-D14 (BUG-454): `preserve` moves to known -- the `restore` arm below now
// reads it (it used to call NO reject net at all and read no options, so
// `restore, preserve` was rc 0 and the option was silently dropped). See the
// arm and runRestore's `rearm` parameter for the re-arm implementation and
// the measured receipts (B5REC|restore|preserve_nopreserve|622| and
// B5REC|restore|preserve_after_preserve|0|, tests/eval/golden/aer_b4/
// b5_misc_pins.log:90-98).
// BUG-922: `not` (cancel the restore; oracle w2_prog_controlflow pc_pr_not).
const piet::eval::OptVocab& kVocabRestore = piet::eval::registerOptVocab(
    {"restore"}, {{"preserve", "not"},
     {}});

const piet::eval::OptVocab& kVocabSet = piet::eval::registerOptVocab(
    {"set"}, {{},
     {"permanently"}});


// --- the BUG-336 GRAPH arm, promoted -------------------------------------
// The 17th and last of the BUG-336 batch. The four sets are the arm's own
// literals, unchanged; only their storage moved. What is new is that the nine
// command tokens the arm serves now CARRY a vocabulary, so the census can say
// what a graph command knows instead of answering `no_vocabulary` for all of
// it. This is the measurement half only — implementing the graph options the
// papers actually use is W4's, and no graph handler is touched here.
//
// The per-token split follows the arm: it assembles its known set from `gc`,
// the resolved PLOT TYPE. For scatter/line/tsline the token IS the plot type;
// for histogram/kdensity/xtline the token is the type and carries that type's
// extras; for `twoway` and `graph` the type is derived from the varlist
// (`twoway histogram x` -> gc "histogram"), so those two tokens carry the
// UNION — anything less would make the registry claim `twoway histogram x,
// width(2)` is unknown when the arm accepts and honours it.
//
// `legend` sits in `known` although nothing reads it. That is not an oversight
// of this sweep: the arm's own note adjudicates it (p5.do:167 uses it, and a
// graph is a visual surface where an unhonoured hint is not a wrong answer —
// the same call already made for `list, noobs`). Promoting a set verbatim is
// the whole contract here, so the adjudication is carried, not re-opened.
const std::unordered_set<std::string> kGraphBaseKnown = {"title", "xtitle", "ytitle", "note",
                                                          "name", "legend"};

// --- AER scope-2 W4 ------------------------------------------------------
// W2 registered the arm's vocabulary so the census could SEE it; W4 makes the
// arm answer for it. Three new sets, each MEASURED from the code that reads it:
//
//   kGraphTwowayGlobals -- the eight globals `runTwowayLayers` honours that no
//     bare handler reads (graph_m4.cpp:1595-1668: axes.xline/yline, the
//     xscale/yscale `noline` flags, the xlabel/ylabel `none` flags, the legend
//     order/cols/size/off block, and `subtitle`). title/xtitle/ytitle/note are
//     honoured by BOTH surfaces and stay in kGraphBaseKnown. The presence of one
//     of THESE eight is what re-routes a single-arm scatter/line/connected into
//     the layer renderer (the arm below): the machinery already exists and only
//     one surface could reach it, which is a wiring gap, not a missing feature.
//
//   kGraphLayerStyle -- every key `twoway::layerStyle` reads
//     (twoway_opts.cpp:248-284). The short spellings are listed because
//     layerStyle reads them ITSELF (`lw`, `lp`, `mc`, `ms`, `mz`, `msi`), so
//     they are canonical names here, not abbreviations.
//
//   kGraphCosmetic -- options this build ACCEPTS and does not apply. They reach
//     no renderer, so silence would be defect class 1 all over again: the arm
//     appends ONE note per block naming exactly the ones the user typed (see
//     graphAcceptedNotApplied below). An option that would change a NUMBER is
//     never in here -- `histogram, discrete` moves the bin edges and `kdensity,
//     gen()` creates variables, so both stay in the unimplemented half and are
//     refused.
const std::unordered_set<std::string> kGraphTwowayGlobals = {
    "xline", "yline", "xscale", "yscale", "xlabel", "ylabel", "legend", "subtitle"};

const std::unordered_set<std::string> kGraphLayerStyle = {
    "color", "fcolor", "lcolor", "lwidth", "lw", "lpattern", "lp",
    "mcolor", "mc", "msymbol", "ms", "msize", "mz", "msi"};

const std::unordered_set<std::string> kGraphCosmetic = {
    "graphregion", "plotregion", "scheme", "saving", "caption",
    "xsize", "ysize", "fxsize", "fysize", "scale", "bgcolor", "lstyle",
    "text", "ttext", "xtick", "ytick", "xmtick", "ymtick",
    "horizontal", "yaxis", "xaxis", "sort", "nodraw", "barwidth",
    "range", "connect", "cmissing", "jitter", "xcommon", "ycommon",
    // serset-twoway VOC: the twoway plot and axis options the w2al corpus
    // writes and Stata answers at rc 0 (marker labels, marker/line/area/bar
    // colours, the t-axis family, minor labels, aspectratio, recast, pstyle).
    // Drawing choices only: nothing here moves a plotted number.
    "mlabposition", "mlabformat", "mlabcolor", "mlabsize", "mlabvposition", "mlabangle",
    "mlabgap", "mlabstyle", "mlabtextstyle", "mfcolor", "mlcolor", "mlwidth", "mlalign",
    "clcolor", "clpattern", "clwidth", "acolor", "alcolor", "afcolor", "alwidth", "alpattern",
    "bcolor", "bfcolor", "blcolor", "blwidth", "fintensity", "tline", "tlabel", "ttick",
    "tmlabel", "ttitle", "tscale", "xmlabel", "ymlabel", "aspectratio", "recast", "pstyle",
    "dropline", "msangle", "base", "mlabel"};

// What is left after W4: `aspectratio` has no renderer concept at all, and
// `by()` is graph FACETING -- a different set of panels, which is a different
// answer, and which `rejectByFacet` (graph_m4.cpp:93) already refuses by name on
// the twoway path. It sits in the unimplemented half so the bare arm says "valid
// Stata option this build does not implement" instead of "not allowed".
// serset-twoway VOC: aspectratio left this set -- Stata draws with it at rc 0
// (w2al tf_wt_077a/b `aspectratio(1)`, tf_tw_072 `aspect(1)`).
const std::unordered_set<std::string> kGraphBaseUnimpl = {"by"};

// The shared known set every graph token starts from. The per-type extras are
// unioned on top of it, by the arm for its own reject and by the registrations
// below for the census.
const std::unordered_set<std::string> kGraphSharedKnown =
    unionOf(unionOf(kGraphBaseKnown, kGraphTwowayGlobals),
            unionOf(kGraphLayerStyle, kGraphCosmetic));

// gfx-histogram-kdensity-family (oracle w2aq_histogram_family): histogram's
// documented options -- discrete, bins()/width()/start(), the four height types, the
// kdensity/normal overlays and their opts(), addlabels, barwidth, horizontal,
// binrescale -- and by() panels are served by runHistogram (graph_m4.cpp).
// addplot()/addlabopts()/barwidth()/blcolor() are carried, not drawn, and so
// stay out of the applied set (graphAcceptedNotApplied names them).
const std::unordered_set<std::string> kGraphHistExtra = {
    "width", "bin", "freq", "frequency", "fraction", "percent", "density", "normal", "normopts",
    "kdensity", "kdenopts", "discrete", "start", "by", "addplot", "addlabels", "addlabopts",
    "blcolor", "binrescale"};

// Item 6 (User ruling 2026-09-09): `gen`/`generate` moved OUT of
// kGraphKdensUnimpl (below) and INTO here. gfx-histogram-kdensity-family:
// at(), nograph, width() (the old bandwidth spelling), the old kernel names
// and normopts() are kdensity's own options ([R] kdensity) and are served too.
const std::unordered_set<std::string> kGraphKdensExtra = {
    "kernel", "bwidth", "n", "normal", "normopts", "gen", "generate", "at", "nograph", "width",
    "biweight", "cosine", "epanechnikov", "gaussian", "parzen", "rectangle", "triangle", "epan2"};

const std::unordered_set<std::string> kGraphXtlineExtra = {"overlay"};

// serset-twoway FIT: twoway_lfit_parse.class / twoway_lfitci_parse.class's own
// options. n(), range(), level() and stdp/stdf/stdr change the fitted numbers
// and are APPLIED (graph_m4.cpp parseFitOptions); the rest are drawing choices.
const std::unordered_set<std::string> kGraphFitExtra = {
    "level", "stdp", "stdf", "stdr", "nofit", "fitplot", "ciplot", "atobs", "estopts", "predopts"};

const std::unordered_set<std::string> kGraphFitApplied = {"n", "range", "level", "stdp", "stdf", "stdr"};

// Per-type refusal, for the option that would change a NUMBER rather than a
// colour: `histogram, discrete` puts one bin on each distinct value
// (different bin edges, different heights). Accepting it quietly would be a
// wrong answer, so it is refused by name instead of noted.
// gfx-histogram-kdensity-family: discrete, addplot and by() left this set when
// runHistogram served them; at() left the kdensity set the same way.
const std::unordered_set<std::string> kGraphHistUnimpl = {};

const std::unordered_set<std::string> kGraphKdensUnimpl =
    unionOf(kGraphBaseUnimpl, {"addplot", "student", "stopts"});

// The umbrella union: for `twoway` and `graph` the plot TYPE is derived from the
// varlist, not from the dispatched token, so those two carry every type's extras.
// `by` is histogram's own (runHistogram's panels); the layer renderer still
// refuses it (rejectByFacet), so it stays in kGraphBaseUnimpl for these two.
const std::unordered_set<std::string> kGraphAllTypesKnown = [] {
  auto u = unionOf(unionOf(kGraphSharedKnown, kGraphHistExtra),
                   unionOf(kGraphKdensExtra, kGraphXtlineExtra));
  u = unionOf(u, kGraphFitExtra);
  u.erase("by");
  return u;
}();

// TWO DECLARED SPELLINGS (BUG-430's mechanism, errors.h). W4 put `xtick`/`ytick`
// in the vocabulary, and that made `xti` a prefix of BOTH `xtitle` and `xtick`
// -- ambiguous, therefore left alone, therefore refused. `xti("...")` /
// `yti("...")` are 6 corpus statements (scatter, graph, binscatter) and THIS
// BUILD ALREADY RESOLVED THEM TO xtitle/ytitle, measured on main's binary before
// this change (`--dump-optvocab --resolve scatter xti` -> in_known). So the map
// records a resolution that already existed and that W4's own additions would
// otherwise have taken away; it is not a new claim about Stata's abbreviation
// table, which was not consulted (no oracle run this stint).
// serset-twoway VOC: `m()` = msymbol and `c()` = connect are Stata's own
// one-letter spellings, below the prefix rule's two-character floor (w2al
// tf_sc_073m, tf_tw_084); `mlabp` is ambiguous by prefix (mlabposition /
// mlabpos...) and is Stata's documented short form; xlabels/ylabels are
// Stata's synonyms of xlabel/ylabel (tf_ts_064).
const std::unordered_map<std::string, std::string> kGraphAliases = {
    {"xti", "xtitle"},      {"yti", "ytitle"},   {"m", "msymbol"},   {"c", "connect"},
    {"mlabp", "mlabposition"}, {"xlabels", "xlabel"}, {"ylabels", "ylabel"}};

const piet::eval::OptVocab& kVocabGraphPlain = piet::eval::registerOptVocab(
    {"scatter", "line", "tsline"}, {kGraphSharedKnown, kGraphBaseUnimpl, kGraphAliases});

// histogram accepts `w()` for width() and `bins()` for bin(): `w` is below the prefix rule's
// two-character floor and `bins` is no prefix of `bin` (hk_h_005w, hk_h_097bins).
const std::unordered_map<std::string, std::string> kGraphHistAliases = {
    {"xti", "xtitle"}, {"yti", "ytitle"}, {"w", "width"}, {"bins", "bin"},
    {"xlabels", "xlabel"}, {"ylabels", "ylabel"}};

const piet::eval::OptVocab& kVocabGraphHist = piet::eval::registerOptVocab(
    {"histogram", "hist"},
    {unionOf(kGraphSharedKnown, kGraphHistExtra), kGraphHistUnimpl, kGraphHistAliases});

// twoway__histogram_gen's options (help twoway__histogram_gen). `w` and `bins` are the
// declared spellings the prefix rule cannot reach (hk_h_005w_tw, hk_h_097bins_tw);
// frac/freq/disc/gen/ret resolve by prefix.
const piet::eval::OptVocab& kVocabTwowayHistGen = piet::eval::registerOptVocab(
    {"twoway__histogram_gen"},
    {{"discrete", "bin", "width", "start", "density", "fraction", "frequency", "percent", "return",
      "generate", "display"},
     {},
     {{"w", "width"}, {"bins", "bin"}}});

const piet::eval::OptVocab& kVocabGraphKdens = piet::eval::registerOptVocab(
    {"kdensity"},
    {unionOf(kGraphSharedKnown, kGraphKdensExtra), kGraphKdensUnimpl, kGraphAliases});

const piet::eval::OptVocab& kVocabGraphXtline = piet::eval::registerOptVocab(
    {"xtline"},
    {unionOf(kGraphSharedKnown, kGraphXtlineExtra), kGraphBaseUnimpl, kGraphAliases});

const piet::eval::OptVocab& kVocabGraphTwoway = piet::eval::registerOptVocab(
    {"twoway"}, {kGraphAllTypesKnown, kGraphBaseUnimpl, kGraphAliases});

// W4: the PLOT-TYPE heads. These are not dispatcher commands -- `connected`,
// `lfit`, `rarea`, `function` and friends are read by twoway_parse.cpp's
// `parseLayerInterior`, which never consults the command registry. They are
// registered anyway for two reasons, both measured:
//   * `runTwowayLayers` now runs an unknown-option net over each layer's own
//     interior (graph_m4.cpp), and it reads its vocabulary back from HERE under
//     the non-command token `twoway_layer` -- the same "a net owns one copy of
//     its set" contract every other registration follows (precedent:
//     `mediate:tvar`, estimation_m4b.cpp:102).
//   * The AER option census attributes a `||` chain segment's options to that
//     SEGMENT's plot type, which is Stata's grammar, so `connected`/`lfit`/
//     `area`/`rarea`/`rcap`/`function` are commands as far as the census is
//     concerned and answered `no_vocabulary` for all 61 of their option rows.
// The set is the umbrella union, not the style keys alone, because Stata allows
// every twoway_option inside an individual plot's option list as well as in the
// global block -- `(scatter y x, xlabel(1 2))` is legal Stata, and a layer net
// carrying only layerStyle's keys would refuse it.
const piet::eval::OptVocab& kVocabGraphLayer = piet::eval::registerOptVocab(
    {"twoway_layer", "connected", "area", "rarea", "rcap", "rcapsym", "rspike",
     "spike", "dropline", "lfit", "qfit", "lfitci", "qfitci", "function",
     "rline", "pcspike", "pcarrow", "bar", "dot"},
    {kGraphAllTypesKnown, kGraphBaseUnimpl, kGraphAliases});

// `graph` is a MULTIPLEX, and this cost a seeded RED before it was a comment.
// The token routes export/save/combine/drop as well as the plot arm, so a
// registration carrying only the plot vocabulary is not merely incomplete — it
// MIS-RESOLVES: `graph export "f", as(png)` has `as` rewritten to
// `aspectratio` (the plot arm's unimplemented list), and the export handler,
// which reads `as` at graph_m4.cpp:2726, stops seeing its format. Six selftest
// legs (BS-012, BUG-103) named it immediately. So the graph token's `known` is
// the union of the plot arm's and the SUBCOMMAND handlers' own reads:
//   graph export   as / name / replace          (graph_m4.cpp:2674,2726,2743)
//   graph save     replace                      (:2504)
//   graph combine  rows / cols / title / subtitle / note / name / imargin /
//                  graphregion                  (:2606,2607,2626,2627,2628,2634,2635,2642)
// `subtitle` and `graphregion` are in the plot arm's UNIMPLEMENTED list and in
// combine's READ list; the registry unions both and `known` wins at every
// decision point, which is the right answer for the token as a whole.
// W4 adds `scale`/`xcommon`/`ycommon` -- three keys `graph combine` is given in
// the corpus (26 uses across the twelve papers) and does not read
// (runGraphCombine's reads are the eight listed above). They arrive through
// kGraphCosmetic, so the combine arm notes them by name rather than dropping
// them. One ABBREVIATION cost is recorded rather than hidden: `cols` and
// `color` now share the prefix `col`, so a bare `col(2)` is ambiguous and left
// alone (i.e. refused) where it used to resolve to `cols`. The full spelling is
// unaffected and no corpus statement or self-test uses the 3-letter form.
const std::unordered_set<std::string> kGraphSubcmdKnown = {
    "as", "name", "replace", "rows", "cols", "title", "subtitle", "note",
    "imargin", "graphregion"};

const piet::eval::OptVocab& kVocabGraphMultiplex = piet::eval::registerOptVocab(
    {"graph"},
    {unionOf(kGraphAllTypesKnown, kGraphSubcmdKnown), kGraphBaseUnimpl, kGraphAliases});


// W4 -- coefplot / marginsplot / binscatter. All three ARE graph commands and
// all three answered `no_vocabulary` for every option row in the census (47 + 5
// + 12). coefplot and marginsplot had no unknown-option net at all, so a typo
// was accepted and dropped exactly like the multi-layer twoway path's.
//
// `known` here is split the same way as the plot arm's: the keys the handler
// READS, plus the keys it accepts and reports. The reads are measured, not
// guessed:
//   coefplot     level / drop / keep / xline      (eststo_esttab.cpp:666,684,689,698)
//                title / xtitle / ytitle          (:817-819)
//                name                             (registerGraphOutput, this file)
//   marginsplot  title / note                     (graph_m4.cpp:1736,1739) + name
//   binscatter   nquantiles                       (handlers_qe.cpp:970 pushOpt)
// Everything else the twelve papers hand these three is a drawing instruction
// -- `recast(bar)`, `ciopts()`, `vertical`, `coeflabels()`, `groups()` -- and a
// drawing instruction is not a number (the §B4(c) call). Those are accepted and
// named in the block's note, never silently dropped.
//
// TWO exceptions, because they are NOT merely cosmetic and are therefore left in
// the unimplemented half so they say so: coefplot's `levels()` and `ci()` set
// WHICH interval is drawn (this build draws `level()`'s, defaulting to 95), and
// accepting them quietly would draw a 95% band for a statement that asked for
// 90%. binscatter's own `by`/`linetype`/`controls`/`absorb` (handlers_qe.cpp:85)
// keep the refusals they already had -- those change the fitted values.
const std::unordered_set<std::string> kCoefplotSpecific = {
    "at",        "base",     "ciopts",   "citop",       "coeflabels", "eqlabels",
    "format",    "groups",   "headings", "mlabel",      "mlabposition", "mlabsize",
    "noeqlabel", "nokey",    "nolabel",  "offset",      "omitted",    "order",
    "recast",    "relocate", "rename",   "rescale",     "swapnames",  "vertical",
    "byopts",    "keep",     "drop",     "level",       "grid",       "aseq"};

const piet::eval::OptVocab& kVocabCoefplot = piet::eval::registerOptVocab(
    {"coefplot"},
    {unionOf(kGraphSharedKnown, kCoefplotSpecific),
     unionOf(kGraphBaseUnimpl, {"levels", "ci"}), kGraphAliases});

const piet::eval::OptVocab& kVocabMarginsplot = piet::eval::registerOptVocab(
    {"marginsplot"}, {unionOf(kGraphSharedKnown, {"recast", "recastci", "plotopts",
                                                  "ciopts", "plot", "x", "noci"}),
                      kGraphBaseUnimpl, kGraphAliases});

// binscatter already has a net and a vocabulary (handlers_qe.cpp kVocabBinscatter).
// This registration UNIONS the graph drawing keys into the SAME registry slot --
// the documented merge rule (errors.h) -- so `binscatter y x, nq(20)
// xtitle("...") scheme(s1mono)` is accepted, its twoway keys carried into
// e(graphcmd) and noted on the drawn block.
const piet::eval::OptVocab& kVocabBinscatterGraph = piet::eval::registerOptVocab(
    {"binscatter"}, {kGraphSharedKnown, {}, kGraphAliases});

// The keys each of the three actually reads -- the `applied` argument to
// graphAcceptedNotApplied at their three dispatch arms.
const std::unordered_set<std::string> kCoefplotApplied = {
    "level", "drop", "keep", "xline", "title", "xtitle", "ytitle", "name"};

const std::unordered_set<std::string> kMarginsplotApplied = {"title", "note", "name"};

// gfx-binscatter: every option of handlers_qe.cpp's kVocabBinscatter known half
// is read by runBinscatter (bins, residualization, fits, colors and symbols in
// the drawn layers, savedata); the twoway keys stay noted.
const std::unordered_set<std::string> kBinscatterApplied = {
    "by",        "nquantiles", "genxq",   "discrete", "medians", "controls",
    "absorb",    "noaddmean",  "linetype", "reportreg", "colors", "mcolors",
    "lcolors",   "msymbols",   "savedata", "replace"};


// lpoly / lowess (goal gfx-lpoly-lowess). Both commands hand options they do
// not know to twoway only when the graph is BUILT, so a typo is r(198) under
// nodraw and silently ignored under nograph (w2ae lpl_typo_draw /
// lpl_typo_nograph, lpl_lw_typo_draw / lpl_lw_typo_nograph); the handlers run
// the net only on the drawn path. `known` = each command's documented options
// plus the graph vocabulary, plus `ttick` (086 reduced_form_est.do:685, a
// time-axis tick option the shared graph set does not carry). The aliases are
// the documented minimums the >=2-char unique-prefix rule cannot reach:
// GENerate (gen..generat prefix generateci/generatevar as well),
// Kernel, Level, Var, EPanechnikov (`ep`/`epa`/`epan` also prefix epan2), and
// lowess's Adjust / Mean / Sort.
const std::unordered_set<std::string> kLpolyOwn = {
    "ci", "generate", "at", "n", "degree", "bwidth", "pwidth", "var", "se", "level", "kernel",
    "nograph", "noscatter", "biweight", "cosine", "epanechnikov", "gaussian", "parzen",
    "rectangle", "triangle", "epan2", "generateci", "generatevar", "legend", "lineopts",
    "ciopts", "scheme", "ttick"};

const std::unordered_set<std::string> kLowessOwn = {
    "nograph", "generate", "adjust", "logit", "sort", "bwidth", "mean", "noweight",
    "plotopts", "lineopts", "rlopts", "ttick"};

const piet::eval::OptVocab& kVocabLpoly = piet::eval::registerOptVocab(
    {"lpoly"},
    {unionOf(kGraphSharedKnown, kLpolyOwn), unionOf(kGraphBaseUnimpl, {"addplot"}),
     {{"xti", "xtitle"}, {"yti", "ytitle"}, {"gen", "generate"}, {"gene", "generate"},
      {"gener", "generate"}, {"genera", "generate"}, {"generat", "generate"},
      {"k", "kernel"}, {"l", "level"}, {"v", "var"}, {"ep", "epanechnikov"},
      {"epa", "epanechnikov"}, {"epan", "epanechnikov"}}});

const piet::eval::OptVocab& kVocabLowess = piet::eval::registerOptVocab(
    {"lowess"},
    {unionOf(kGraphSharedKnown, kLowessOwn), unionOf(kGraphBaseUnimpl, {"plot", "addplot"}),
     {{"xti", "xtitle"}, {"yti", "ytitle"}, {"a", "adjust"}, {"m", "mean"}, {"s", "sort"}}});

// What the two handlers' graph blocks actually read: the computation options
// (they shape the drawn smooth) plus title/xtitle/ytitle/note. Every other key
// the user typed -- legend, xlabel, graphregion, lwidth, scheme, ... -- is
// named in the block's note by graphAcceptedNotApplied.
const std::unordered_set<std::string> kLpolyApplied = {
    "ci", "generate", "at", "n", "degree", "bwidth", "pwidth", "se", "level", "kernel",
    "nograph", "noscatter", "biweight", "cosine", "epanechnikov", "gaussian", "parzen",
    "rectangle", "triangle", "epan2", "title", "xtitle", "ytitle", "note", "name"};

const std::unordered_set<std::string> kLowessApplied = {
    "nograph", "generate", "adjust", "logit", "bwidth", "mean", "noweight", "sort",
    "title", "xtitle", "ytitle", "note", "name"};
}  // namespace piet::dispatch_detail

namespace piet {
using namespace dispatch_detail;


// ---------------------------------------------------------------------------
// AER scope-2 A1 — THE option-name resolution rule, as one callable function.
//
// This body is dispatchImpl's own step 1a-ter, moved out unchanged so the rule
// has exactly one transcription. dispatchImpl calls it per parsed option key;
// `piet_native --dump-optvocab --resolve` calls the same function, which is the
// entire reason that flag exists — a reporting tool carrying its own copy of
// the rule would answer for a dispatcher that does not exist.
//
// `stage` names the LAST pass that actually rewrote the key. It is not a finer
// breakdown than that on purpose: canonicalizeForCommand's internal
// alias/known-prefix/unimplemented-prefix order is its own, and re-deriving it
// out here would be the second transcription this function exists to prevent.
OptKeyResolution resolveOptionKeyForCommand(const std::string& cmd, const std::string& key) {
  // DELIBERATELY a declared table, not a call to resolveOptionAbbrev over a
  // guessed vocabulary: an over-accepted option spelling is the same fabricated
  // parity the command-name arc rejected. Only rows the oracle measured appear.
  struct OptMinimum {
    const char* full;
    size_t minLen;  // Stata's documented minimum abbreviation length
  };
  // level: estimation_options.sthlp `{opt l:evel(#)}` + live `regress price mpg, l(90)` rc=0.
  // level: estimation_options.sthlp `{opt l:evel(#)}` + live `regress price mpg, l(90)` rc=0.
#include "../generated/opt_minimums_bug264.inc"  // NOLINT — table body, see gen_table.py
  const auto& kOptMinimums = kOptMinimumsGenerated;

  // Generated rows a later oracle overrules (the table must not be hand-edited).
  // egen MISSing: its measured minimum is one function's (_gtotal `Missing`, 1),
  // while every egen function declares its own and runEgen resolves each key
  // against the function's own minimums (egenFnVocab). Applied to all of them it
  // made `mode(k), m` the missing variant (StataBE 19.5: `option m not allowed`
  // r(198), _gmode `MISSing`) and `std(x), m(5)` a refused missing (mean(5));
  // BUG-1111.
  static const std::set<std::pair<std::string, std::string>> kGeneratedOverruled = {
      {"egen", "missing"}};

  OptKeyResolution res;
  res.resolved = key;
  auto mins = kOptMinimums.find(cmd);
  if (mins != kOptMinimums.end()) {
    for (const auto& om : mins->second) {
      const std::string full(om.full);
      if (kGeneratedOverruled.count({cmd, full}) != 0) continue;
      if (res.resolved.size() >= om.minLen && res.resolved.size() < full.size() &&
          full.compare(0, res.resolved.size(), res.resolved) == 0) {
        res.resolved = full;
        res.stage = "minimums";
        break;
      }
    }
  }
  // Minimums measured by a later oracle than the generated BUG-264 table (which
  // must not be hand-edited). Each row is a measured accept AND its measured
  // refusal one character shorter:
  //   append GENerate: `gen(src)` rc 0 (aer_w2 w2_data_io.log app9, w2b_dataio
  //   append_gen(src)); `ge(src)` and `g(src)` r(198) (w2b_dataio append_ge,
  //   append_g).
  //   order ALPHAbetic: `order *, alpha` rc 0 (aer_w2 w2b_dataio order_alpha,
  //   w2b_dataio.log:809-810); the minimum is order.ado's syntax line.
  //   fcollapse (ftools 2.49.0) FREQvar REGister Verbose: StataBE 19.5 runs
  //   `freqv(x)`, `reg(x)` (then Mata's r(3499)), `v` and `ve`; `re(x)` is
  //   `option re() not allowed` r(198), and `fre` is r(198) beside FREQ.
  static const std::map<std::string, std::vector<OptMinimum>> kOptMinimumsLater = {
      {"append", {{"generate", 3}}},
      {"order", {{"alphabetic", 5}}},
      {"fcollapse", {{"freqvar", 5}, {"register", 3}, {"verbose", 1}}},
  };
  // Commands whose whole vocabulary a later oracle adjudicated: kOptMinComplete's
  // statement for the rows above. collapse_17's BY CW FAST CLABEL frame() and
  // every other fcollapse option take no abbreviation (BUG-1100, StataBE 19.5:
  // `cla`, `clab`, `clabe`, `fra()`, `fram()`, `fas`, `c` and `b()` on collapse,
  // `app`, `appen`, `sm`, `smar`, `noc`, `nocomp`, `nocompres`, `mer`, `poo()`,
  // `meth()` and `metho()` on fcollapse are `option ... not allowed` r(198)).
  static const std::set<std::string> kOptMinCompleteLater = {"collapse", "fcollapse"};
  auto later = kOptMinimumsLater.find(cmd);
  if (std::string(res.stage) == "unchanged" && later != kOptMinimumsLater.end()) {
    for (const auto& om : later->second) {
      const std::string full(om.full);
      if (res.resolved.size() >= om.minLen && res.resolved.size() < full.size() &&
          full.compare(0, res.resolved.size(), res.resolved) == 0) {
        res.resolved = full;
        res.stage = "minimums";
        break;
      }
    }
  }

  // -------------------------------------------------------------------------
  // AER scope-2 B4 — THE ABBREVIATION FLOOR: the NARROWING companion to the
  // widening table above, and the fix for a destructive regression.
  //
  // WHY IT EXISTS. eval::resolveOptionAbbrev resolves any >=2-char UNIQUE
  // prefix of a net's vocabulary. That rule is a GUESS about Stata, and on
  // 2026-09-09 the guess was measured wrong in the worst possible direction:
  // the W2 vocabulary sweep (e1faab17) put `replace` into the export/save/graph
  // nets, `re` thereby became a unique prefix of it, and
  //   export delimited using "f.csv", re
  // — which real StataBE answers r(198), "option re not allowed" — started
  // OVERWRITING the file. `graph export "f.png", as(png) re` likewise turned
  // the r(602) "file already exists" guard into a silent success. A guess may
  // never be the reason PIET destroys a file or changes an answer.
  //
  // THE RULE. Each row is one MEASURED StataBE refusal: option `full` on
  // command `cmd` accepts no abbreviation shorter than `floorLen`. A key that
  // is a PROPER prefix of `full` and shorter than `floorLen` is left exactly as
  // the user typed it and marked exactOnly, so the net answers r(198) naming
  // that spelling — which is what Stata prints. floorLen == strlen(full) means
  // NO ABBREVIATION AT ALL.
  //
  // WHY A FLOOR AND NOT kOptMinComplete. Joining kOptMinComplete asserts that a
  // command's WHOLE vocabulary has been adjudicated. oracle_p probed a handful
  // of OPTIONS, not whole vocabularies, and claiming a completeness we do not
  // have would start rejecting spellings Stata accepts — `regress ..., noc` is
  // rc 0 in Stata (oracle_p.log:140-156) and has to stay rc 0. A floor row is
  // the narrow statement the capture actually makes, about one option, and
  // nothing else moves.
  //
  // ONE-SIDED, in the safe direction, which is the whole safety argument: a
  // floor can only turn an accept into a reject, and only for a (command,
  // option) pair a live capture measured Stata rejecting. The FULL spelling is
  // never touched — only proper prefixes are tested — and neither is any key
  // that is an exact member of the command's own vocabulary (`import
  // delimited, delim(",")` keeps working even though `delim` is a proper prefix
  // of `delimiter`), which is the same "an exact spelling is never an
  // abbreviation" principle eval::canonicalizeForCommand's step 1 already
  // states.
  struct OptFloor {
    const char* cmd;
    const char* full;
    size_t floorLen;  // shortest ACCEPTED abbreviation; == strlen(full) => none
  };
  static const OptFloor kOptFloors[] = {
      // P0-A, the destructive row. pins/oracle_p.log:23-27 `export
      // delimited using "p_a.csv", re` -> r(198) "option re not allowed";
      // :29-33 `, rep` -> r(198); :16-21 the full `, replace` -> rc 0. So
      // `replace` on export takes NO abbreviation.
      {"export", "replace", 7},
      // oracle_p.log:61-65 `save "p_c.dta", re` -> r(198); :54-59 full
      // `replace` -> rc 0. Same word, same verdict, measured separately.
      {"save", "replace", 7},
      // W1a, 2026-09-10. `tabulate ..., no` ran a NOFREQ tabulation here and is
      // r(198) in Stata. The prefix looked unambiguous only because `nolabel`,
      // `nokey` and `nolog` are ABSENT from kVocabTabulate -- a MISSING
      // vocabulary entry is what made an over-acceptance look unique, which is
      // the same shape as the `re` -> `replace` row above with the cause on the
      // other side. Measured, tests/eval/golden/aer_w1/w1a_optmin_pins3.log:
      // `n` r(198), `no` r(198), `nof` rc 0 -- so the floor is 3. The same log
      // records that `nolabel` and `nokey` are rc 0 in Stata (W1a-1 put them in
      // the net, 2026-09-14) while `nolog` is r(198) (it does not). Filed BUG-485.
      {"tabulate", "nofreq", 3},
      // `graph export`/`graph save` share the identical `replace` word. NOT
      // separately captured in oracle_p — the row is carried from the two
      // measured `replace` verdicts above because it is the same option word on
      // the same file-writing duty, and because the direction is REFUSAL, which
      // cannot invent a Stata behaviour. Measured HERE before the fix: with a
      // file already present, `graph export "g.png", as(png) re` returned rc 0
      // and defeated the r(602) file-exists guard.
      {"graph", "replace", 7},

      // ── P0-B, the measured half. oracle_p.log:41-45 `export delimited ...,
      // nol` -> r(198); :47-51 `, nov` -> r(198). Neither is ambiguous, so the
      // ambiguity repair alone would not reach them: export's option names
      // simply take no abbreviation. Between them they also settle `, no`
      // (:35-39, r(198)) from a second direction.
      // w2ar (2026-10-21) measured the accepting side: `nolab` rc 0
      // (dx_exd_nolab, log:659) and `novar` rc 0 (dx_exd_novar, log:515), the
      // NOLABel / NOVARnames minimums of export delimited's syntax line.
      // `nol`/`nov` stay r(198) (oracle_p above). DATAFmt's minimum is `dataf`
      // and KEEPCELLFmt's `keepcellf` (export delimited / export excel).
      {"export", "nolabel", 5},
      {"export", "novarnames", 5},
      {"export", "datafmt", 5},
      {"export", "keepcellfmt", 9},

      // ── P1, MEASURED MINIMUMS. Each pair is a refusal AND the accepting
      // spelling one character longer, which is what makes it a floor rather
      // than a guess at one.
      // oracle_p.log:68-72 `list make in 1/2, no` -> r(198); :74-84 `, noo` ->
      // rc 0. So noobs' minimum is 3.
      {"list", "noobs", 3},
      // oracle_p.log:159-163 `summarize price mpg, se(2)` -> r(198); :165-173
      // `, sep(2)` -> rc 0. separator's minimum is 3, and PIET's generic 2-char
      // floor sat one character below it.
      {"summarize", "separator", 3},
      // oracle_p.log:176-180 `estimates table m1, st(N)` -> r(198); :182-186
      // `, sta(N)` -> r(198) TOO. Both refused, so the floor is at least 4;
      // `stat` was not probed and a floor of 4 does not accept it — it only
      // declines to block it, leaving today's behaviour untouched. Landing 5
      // would be claiming a measurement nobody took.
      {"estimates", "stats", 4},
      // oracle_p.log:109-113 `regress price mpg, vc(robust)` -> r(198);
      // :121-138 `, vce(robust)` -> rc 0. vce takes NO abbreviation.
      {"regress", "vce", 3},
      // StataBE 19.5, 2026-10-29: `replace e = 3.7, nop` rc 0 (byte e stores 3),
      // `, no` and `, n` r(198) "option no not allowed", plain and by-prefixed
      // alike. NOPromote's minimum is 3.
      {"replace", "nopromote", 3},
      // BUG-1163 review: tsset.ado's display options take no abbreviation --
      // `xtset id t, nob` is r(198) "option nob not allowed", `, displayi(2)`
      // "option displayi() not allowed", and PanelSet answers `xtset id, nob`
      // and `, panel(G)` with its r(101) (StataBE 19.5).
      {"xtset", "noblank", 7},
      {"xtset", "panelname", 9},
      {"xtset", "displayindent", 13},
      {"tsset", "noblank", 7},
      {"tsset", "panelname", 9},
      {"tsset", "displayindent", 13},
      {"tset", "noblank", 7},
      {"tset", "panelname", 9},
      {"tset", "displayindent", 13},
      // BUG-1096, each a refusal and the accepted spelling one longer (StataBE
      // 19.5): `decode e, gen(d) max(3)` r(198), `maxl(3)` rc 0; `encode s, ...
      // noex` r(198), `noext` rc 0.
      {"decode", "maxlength", 4},
      {"encode", "noextend", 5},
      // BUG-1141: [D] recode's pre:fix() and copy:rest minimums (`pr()`, `co`,
      // `cop` r(198)).
      {"recode", "prefix", 3},
      {"recode", "copyrest", 4},

      // ── P1, INTERNAL SYNONYMS — a different kind of row, and the reason it is
      // honest. A PIET `known` set is a statement about what the HANDLER READS,
      // not about Stata's vocabulary: it legitimately carries aliases this build
      // uses internally. `vcov` is one — real Stata's regress has no such option
      // at all, which is why StataBE answers `vco(robust)` with r(198), "option
      // vco() not allowed" (oracle_p.log:115-119). An internal alias exists so a
      // handler can read a key; it must not also widen what PIET accepts. So it
      // keeps its exact spelling and contributes NO abbreviation surface.
      {"regress", "vcov", 4},
      // BUG-1500: ivprobit's TWOstep. StataBE 19.5 `ivprobit yb2 x1 (xe = z2), two`
      // rc 0, `, tw` r(198) "option tw not allowed" (scratch-laneA2 feat/i1.do).
      {"ivprobit", "twostep", 3},

      // gtools' gegen: its own total(), sum() and group() take `missing` only
      // in full (StataBE 19.5 with gtools: `gegen q = total(a), mi` and
      // `group(a), mi` are `option mi not allowed` r(198)), while the functions
      // it hands to egen read their ado's minimum (`rowtotal(a x), mi` rc 0).
      // So the key reaches runEgen as typed and is resolved per function there,
      // as egen's is (BUG-1111).
      {"gegen", "missing", 7},
  };
  {
    // An exact member of the command's own vocabulary is a spelling, not an
    // abbreviation, and is never floored (see the `delim` case above).
    const eval::OptVocab* fv = eval::findOptVocab(cmd);
    const bool exactSpelling =
        fv != nullptr &&
        (fv->known.count(res.resolved) != 0 || fv->unimplemented.count(res.resolved) != 0);
    if (!exactSpelling) {
      for (const auto& of : kOptFloors) {
        if (cmd != of.cmd) continue;
        const std::string full(of.full);
        if (res.resolved.size() < full.size() && res.resolved.size() < of.floorLen &&
            full.compare(0, res.resolved.size(), res.resolved) == 0) {
          // Below the measured floor: do NOT resolve, and tell the net not to
          // resolve either (OptValue::exactOnly reaches
          // eval::rejectUnknownOptions, which otherwise prefix-resolves the key
          // itself — BUG-335's decision point). Hand-rolled nets that never
          // call rejectUnknownOptions are covered by the same act of leaving
          // the key raw: they compare against the full spelling and reject.
          res.exactOnly = true;
          res.stage = "floor";
          return res;
        }
      }
    }
  }

  // BUG-233 / FIX:OPTNET-ABBREV-SWEEP — the GENERAL half of the same point.
  //
  // The table above is the WIDENING half: nine oracle-measured minimums,
  // including the two 1-char ones (`l`->level, `r`->robust) that are below
  // resolveOptionAbbrev's deliberate 2-char floor. It can only be a declared
  // table, because widening acceptance without an oracle row is fabricated
  // parity.
  //
  // This call is the DROP half, and it needs no oracle at all, because it
  // widens nothing: eval::rejectUnknownOptions ALREADY accepts every >=2-char
  // unique prefix of every net's `known` set. canonicalizeForCommand applies
  // that same rule against that same vocabulary, one step earlier, so the
  // accepted set is bit-identical and the only change is that the handler now
  // SEES the option it already agreed to accept. Measured accept-then-drop
  // instances this closes: winsor2 `cut(5 95)` (silently reverted to the 1/99
  // default), ttest `uneq` (pooled SE instead of Welch), esttab `nosta`
  // (stars printed anyway), levelsof `loc(m)` (the local never created).
  //
  // Runs AFTER the table so the 1-char rows still win; a no-op on any key the
  // table already canonicalized.
  // BUG-264/BUG-335 — THE NARROWING FLIP. The first attempt skipped only
  // canonicalizeForCommand and measured 13/18 on controlled boundary checks,
  // with all 3 passes being spellings already rejected for ambiguity: it
  // narrowed nothing, because rejectUnknownOptions resolves prefixes ITSELF.
  // The mark below reaches that decision point (OptValue::exactOnly).
  if (kOptMinComplete.count(cmd) != 0 || kOptMinCompleteLater.count(cmd) != 0) {
    // Whole vocabulary adjudicated: the minimums pass above is a COMPLETE
    // statement of the spellings Stata accepts, so mark the key and let the
    // net answer r(198) for anything else — no unique-prefix fallback.
    res.exactOnly = true;
  } else {
    // canonicalizeForCommand rewrites a whole option vector, and treats each
    // entry independently, so handing it a one-entry vector is the same pass
    // dispatchImpl used to run over all of them at once.
    std::vector<std::pair<std::string, eval::OptValue>> one;
    one.emplace_back(res.resolved, eval::OptValue());
    eval::canonicalizeForCommand(one, cmd);
    if (one.front().first != res.resolved) {
      res.resolved = one.front().first;
      res.stage = "vocab";
    }
  }
  return res;
}
}  // namespace piet
