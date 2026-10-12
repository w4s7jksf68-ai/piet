#include "piet/dispatcher.h"
#include "piet/translate.h"

#include "piet/bcal.h"  // AER scope-2 B2-3: the calendar search directory follows session.cwd

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

#include "piet/bundle_paths.h"
#include "piet/cclass.h"
#include "piet/cobra_runner.h"
#include "piet/counters.h"
#include "piet/display_hardening.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // BUG-476: kStataMissingSentinel
#include "piet/eval/gettoken.h"
#include "piet/eval/macros.h"
#include "piet/eval/pyvalue.h"
#include "piet/eval/tokenizer.h"
#include "piet/handlers.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"
#include "piet/mata_subset.h"
#include "piet/program.h"
#include "piet/strmutate.h"
#include "piet/storage_type.h"  // BUG-471 T2: the store-type resolver
#include "piet/strutil.h"
#include "dispatch_profile.h"  // WS-D1: per-zone dispatch attribution (stderr, opt-in)
#include "io/win_compat.h"  // cecon::to_native_path / native_unlink (BUG-050 wide open)
#include "data/missing.h"   // BUG-532: cecon::missing_kind, the `.a`-`.z` test on a by-key
#include "piet/twoway_parse.h"
#include "piet/scratch_seq.h"
#include "piet/settings.h"  // WS-B: settings::extcompatEnabled() (the extcompat gate below)
#include "piet/preflight.h"  // SPEC II-4: memory pre-flight before data-size-scaling verbs
#include "registry.h"

// ---------------------------------------------------------------------------
// BUG-233 / FIX:OPTNET-ABBREV-SWEEP — this file's option vocabularies.
//
// Every set below used to be a function-local `static const` sitting next to its
// reject net, which meant nothing upstream of the handler could see it — and the
// handler's own `k == "fullname"` comparisons therefore ran against RAW keys
// while eval::rejectUnknownOptions had already ACCEPTED an abbreviation of them.
// That is BUG-231's mechanism, and it was live in nets across six files.
//
// Registering here (static init, before any dispatch) lets dispatcher.cpp's single
// resolution point canonicalize the keys BEFORE routing. The sets did not get
// copied: each net now binds a reference back to its registered entry, so there is
// still exactly one place each spelling is written.
namespace {
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
// (milf-coord/plans/aer-scope1/lexgaps-12.json, command="use" option="replace",
// every one of the 92 `uses[].text` entries matches `,\s*replace` verbatim).
const piet::eval::OptVocab& kVocabUse = piet::eval::registerOptVocab(
    {"use"}, {{"clear", "replace"},
     {}});
const piet::eval::OptVocab& kVocabXtset = piet::eval::registerOptVocab(
    {"xtset"}, {{"delta"},
     {}});
// B4-W3b: the PERIOD options. Each names a Stata date format and nothing else
// (`tsset t, quarterly` -> r(tsfmt) "%tq" and `format t %tq`), so the table
// below IS the implementation, not a lookup beside one. `generic` is Stata's
// own name for the %tg default; `clock`/`Clock` are the datetime pair.
const std::map<std::string, std::string>& kTssetPeriodFmt() {
  static const std::map<std::string, std::string> m = {
      {"yearly", "%ty"},   {"halfyearly", "%th"}, {"quarterly", "%tq"},
      {"monthly", "%tm"},  {"weekly", "%tw"},     {"daily", "%td"},
      {"generic", "%tg"},  {"clock", "%tc"},      {"Clock", "%tC"}};
  return m;
}
const piet::eval::OptVocab& kVocabTsset = piet::eval::registerOptVocab(
    {"tsset"}, {{"delta", "yearly", "halfyearly", "quarterly", "monthly", "weekly", "daily",
                 "generic", "clock", "format"},
     {}});
const piet::eval::OptVocab& kVocabDestring = piet::eval::registerOptVocab(
    {"destring"}, {{"replace", "gen", "generate",
                                                                        "ignore", "force"},
     {}});
// B5-D1: `force` accepted as a validated no-op, same rationale as append's
// `force` below — StataBE renders 42/./.a with `force` on a double
// (tests/kitchensink/fmt_oracle/tostring_missing_oracle.log:67, rc 0) and
// runTostring's renderer has no reversibility gate to relax, so accepting the
// token changes nothing observable; without this a legal script died at r(198)
// (measured B4REC|tostring|replace_force|0| — tests/eval/golden/aer_b4/
// milfpins_oracle_r.log:29). `g` is Stata's abbreviation of `generate`; it
// cannot resolve via the unique-prefix rule because `gen`/`generate` are BOTH
// already-registered literal spellings that `g` prefixes ambiguously, so it is
// declared instead (BUG-430 aliases mechanism, kVocabPredict precedent above).
const piet::eval::OptVocab& kVocabTostring = piet::eval::registerOptVocab(
    {"tostring"}, {{"replace", "gen", "generate",
                                                                        "usedisplayformat", "force"},
     {},
     {{"g", "generate"}}});
// BUG-370. `nq` is Stata's own abbreviation of nquantiles, and the vocab layer
// resolves prefixes, so registering the long spelling accepts both.
const piet::eval::OptVocab& kVocabDecode = piet::eval::registerOptVocab(
    {"decode"}, {{"generate"},
     {"maxlength"}});
// B5-D7: `not` moves to known -- see the `ds` arm below for the complement
// implementation and the measured receipt. has/alpha/varwidth/detail stay
// unimplemented (refused by name, not silently dropped).
const piet::eval::OptVocab& kVocabDs = piet::eval::registerOptVocab(
    {"ds"}, {{"not"},
     {"detail", "varwidth", "alpha", "has"}});
const piet::eval::OptVocab& kVocabXtile = piet::eval::registerOptVocab(
    {"xtile"}, {{"nquantiles", "cutpoints"},
     {"altdef"}});
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
// `sysuse auto` is rc 0 with _N 74 -> 2 (milfpins_oracle_g.log:250-256), the
// same rows this build's varlist path already keeps. So it is accepted and
// read as the acknowledgement it is, rather than rejected as unknown.
const piet::eval::OptVocab& kVocabDuplicates = piet::eval::registerOptVocab(
    {"duplicates"}, {{"gen", "generate", "force"},
     {}});
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
const piet::eval::OptVocab& kVocabTabulate = piet::eval::registerOptVocab(
    {"tabulate", "tab", "ta", "tab1", "tab2"},
    {{"chi2", "sum", "summarize", "missing", "generate", "sort", "cell", "col", "row", "nofreq",
      "nolabel", "nokey"},
     {}});
const piet::eval::OptVocab& kVocabTable = piet::eval::registerOptVocab(
    {"table"}, {{"statistic", "stat"},
     {}});
// B4-W3b: `nototal` and `save` join the KNOWN set — both change which numbers
// appear (`nototal` drops the overall row, `save` publishes r(Stat#)/r(name#)),
// which is exactly why they were rejected before and exactly why they are
// implemented rather than accepted-and-ignored now.
const piet::eval::OptVocab& kVocabTabstat = piet::eval::registerOptVocab(
    {"tabstat"}, {{"by", "stat", "stats",
                   "statistics", "columns", "nototal", "save"},
     {}});
const piet::eval::OptVocab& kVocabCorr = piet::eval::registerOptVocab(
    {"corr", "cor", "pwcorr", "correlate"}, {{"pairwise", "sig", "obs"},
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
// B4REC|describe|fullname|0| (tests/eval/golden/aer_b4/milfpins_oracle_r.log:
// 130-131) and the in-tree StataBE rendered table at tests/eval/golden/
// f16_tw_transport_oracle.log:43,85 (`describe, fullnames`). short/simple/
// numbers/detail stay unimplemented -- those DO change what prints.
const piet::eval::OptVocab& kVocabDescribe = piet::eval::registerOptVocab(
    {"describe"}, {{"fullnames"},
     {"short", "simple", "numbers", "detail"}});
// rename's 1:1 and wildcard forms read no option at all and Stata's own 1:1
// form has none either (dispatcher.cpp's rename arm). Registering the EMPTY
// vocabulary is not a no-op: it is the difference between "we looked and there
// are none" and "nobody has looked".
// B4-W3b: the GROUP form's case options (29 corpus uses of `rename _all,
// lower` / `rename *, upper`). Measured rc 0 on StataBE 19.5
// (milfpins_oracle_g.log:101-105), which also shows the note Stata prints when
// nothing moves: "(all newnames==oldnames)".
const piet::eval::OptVocab& kVocabRename = piet::eval::registerOptVocab(
    {"rename"}, {{"lower", "upper"},
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
const piet::eval::OptVocab& kVocabIvMl = piet::eval::registerOptVocab(
    {"ivprobit", "ivtobit"}, {{"vce", "vcov", "robust", "cluster", "ll", "ul", "level"},
     {}});
const piet::eval::OptVocab& kVocabFracreg = piet::eval::registerOptVocab(
    {"fracreg"}, {{"vce", "vcov", "robust", "cluster", "level"},
     {}});
const piet::eval::OptVocab& kVocabCompress = piet::eval::registerOptVocab(
    {"compress"}, {{},
     {"nocoalesce"}});
const piet::eval::OptVocab& kVocabSort = piet::eval::registerOptVocab(
    {"sort"}, {{},
     {"stable"}});
const piet::eval::OptVocab& kVocabGsort = piet::eval::registerOptVocab(
    {"gsort"}, {{},
     {"generate", "mfirst"}});
const piet::eval::OptVocab& kVocabRecode = piet::eval::registerOptVocab(
    {"recode"}, {{"generate"},
     {"prefix", "label", "copyrest", "test"}});
// B5-D6: `separator` moves to known -- runList renders a GUI grid (same
// validated-no-op class as noobs/clean just above: the grid draws no
// every-N-rows rule either way, so accepting and discarding the count changes
// nothing observable). Measured B4REC|list|sep(0)|0| (tests/eval/golden/
// aer_b4/milfpins_oracle_r.log:143-144) and the in-tree StataBE rendered
// table at milfpins_oracle_h.log:94 / milfpins_oracle_i.log:126 (`list ...,
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
// milfpins_oracle_r.log:60-66). KNOWN NARROWING: runLabelDefine re-parses
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
    {"label"}, {{"add", "modify", "replace"},
     {"nofix", "new", "rename", "delete"}});
const piet::eval::OptVocab& kVocabLabelbook = piet::eval::registerOptVocab(
    {"labelbook"}, {{},
     {"alpha", "length", "list", "problems", "detail"}});
// import/export: the net is per (command, subcommand), so the four branch sets
// stay the literals the net uses and the REGISTRATION is their union per
// command token. Unioning across subcommands is the registry's documented
// behaviour (errors.h: csdid and its estat sub-net), and the branch net is
// still the authority on what is accepted.
const std::unordered_set<std::string> kExportDelimKnown = {"delimiter", "delim", "nolabel",
                                                           "replace"};
const std::unordered_set<std::string> kExportDelimUnimpl = {"quote", "novarnames", "datafmt"};
// AER scope-2 W3a: `sheet()` and `sheetreplace`/`sheetmodify` land; `cell()`
// (a start cell for the rectangle) and `keepcellfmt` (needs a styles part this
// writer does not author) stay named gaps.
const std::unordered_set<std::string> kExportExcelKnown = {"replace", "nolabel", "firstrow",
                                                           "sheet", "sheetreplace", "sheetmodify"};
// `datestring()`/`missing()` are deliberately NOT listed: adding `datestring`
// would make `da` an ambiguous prefix against export delimited's `datafmt` in
// the per-command union, trading one honest message for a worse one on a
// spelling that already resolved.
const std::unordered_set<std::string> kExportExcelUnimpl = {"cell", "keepcellfmt", "locale"};
// AER scope-2 W3a: `varnames`/`rowrange`/`colrange`/`numericcols`/`encoding`
// land. BUG-477: `case` for DELIMITED lands too -- it is a DIFFERENT rule
// from excel's (delimited's own default is `lower`, not `preserve` --
// b5_importcase_pins.log) but the same three-value grammar, so it moves to
// KNOWN alongside them. `asdouble`/`bindquote`/`stringcols` stay named gaps.
const std::unordered_set<std::string> kImportDelimKnown = {
    "delimiter", "delim",        "clear",    "varnames", "rowrange",
    "colrange",  "numericcols", "encoding", "case"};
const std::unordered_set<std::string> kImportDelimUnimpl = {"asdouble", "bindquote",
                                                            "stringcols"};
// AER scope-2 W3a: this is the highest-frequency option cluster in the whole
// twelve-paper corpus (`import excel` appears 273 times, 243 in paper 015).
const std::unordered_set<std::string> kImportExcelKnown = {
    "firstrow", "clear", "sheet", "cellrange", "allstring", "case", "describe"};
const std::unordered_set<std::string> kImportExcelUnimpl = {"locale"};
std::unordered_set<std::string> unionOf(const std::unordered_set<std::string>& a,
                                        const std::unordered_set<std::string>& b) {
  std::unordered_set<std::string> out = a;
  out.insert(b.begin(), b.end());
  return out;
}
const piet::eval::OptVocab& kVocabImport = piet::eval::registerOptVocab(
    {"import"}, {unionOf(kImportDelimKnown, kImportExcelKnown),
     unionOf(kImportDelimUnimpl, kImportExcelUnimpl)});
const piet::eval::OptVocab& kVocabExport = piet::eval::registerOptVocab(
    {"export"}, {unionOf(kExportDelimKnown, kExportExcelKnown),
     unionOf(kExportDelimUnimpl, kExportExcelUnimpl)});
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
    {"joinby"}, {{"unmatched"},
     {}});
const piet::eval::OptVocab& kVocabEncode = piet::eval::registerOptVocab(
    {"encode"}, {{"generate", "label"},
     {"noextend"}});

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
// absorb. :1303-1310 refuses be/gls/mle/ec1/pa BY NAME as real xtreg
// estimators this build does not have — the definition of unimplemented.
const piet::eval::OptVocab& kVocabXtreg = piet::eval::registerOptVocab(
    {"xtreg"}, {{"fe", "re", "vce", "vcov", "robust", "cluster", "noconstant", "nocons",
                 "level", "absorb"},
     {"be", "gls", "mle", "ec1", "pa"}});
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
// B4REC|save|replace_emptyok|0| (tests/eval/golden/aer_b4/milfpins_oracle_r.log
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
// predict: estimation_m4c.cpp:1240 kSupported = {xb,resid,residuals,fe} is the
// read set; :1241-1246 kUnsupportedStats is that file's own list of real Stata
// predict statistics it refuses by name. e/equation/xbd are the three more the
// 12 papers use (predict ...,e / equation(`i') / xbd) and this build has not.
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
    {"predict"}, {{"xb", "resid", "residuals", "fe"},
     {"cooksd", "leverage", "hat", "stdp", "stdf", "stdr", "rstandard", "rstudent",
      "dfbeta", "pr", "xbu", "dresid", "anscombe", "deviance", "pearson", "response",
      "score", "working", "e", "equation", "xbd"},
     {{"re", "residuals"}, {"res", "residuals"}, {"resi", "residuals"}}});
// ivreg2/xtivreg2: handlers_aer_estim.cpp:326-338 consumes r/robust/cl/cluster/
// gmm2s/fe/small and passes everything else to runIvRegress, whose net is
// kVocabIvRegress (estimation_m4.cpp:141) — so the known set is the union of
// the two. The unimplemented names are the ivreg2 options the 12 papers use
// that neither layer reads (first/sfirst/savefirst/savefprefix/saverfprefix/
// partial); each is corpus-attested Stata that ran, not a recollection.
const piet::eval::OptVocab& kVocabIvreg2 = piet::eval::registerOptVocab(
    {"ivreg2", "xtivreg2"}, {{"r", "robust", "cl", "cluster", "gmm2s", "fe", "small",
                              "vce", "vcov", "gmm", "absorb", "noconstant", "nocons", "level"},
     {"first", "sfirst", "savefirst", "savefprefix", "saverfprefix", "partial"}});
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
const piet::eval::OptVocab& kVocabCollapse = piet::eval::registerOptVocab(
    {"collapse"}, {{"by", "fast"},
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
const piet::eval::OptVocab& kVocabRestore = piet::eval::registerOptVocab(
    {"restore"}, {{"preserve"},
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
    "range", "connect", "cmissing", "jitter", "xcommon", "ycommon"};
// What is left after W4: `aspectratio` has no renderer concept at all, and
// `by()` is graph FACETING -- a different set of panels, which is a different
// answer, and which `rejectByFacet` (graph_m4.cpp:93) already refuses by name on
// the twoway path. It sits in the unimplemented half so the bare arm says "valid
// Stata option this build does not implement" instead of "not allowed".
const std::unordered_set<std::string> kGraphBaseUnimpl = {"aspectratio", "by"};
// The shared known set every graph token starts from. The per-type extras are
// unioned on top of it, by the arm for its own reject and by the registrations
// below for the census.
const std::unordered_set<std::string> kGraphSharedKnown =
    unionOf(unionOf(kGraphBaseKnown, kGraphTwowayGlobals),
            unionOf(kGraphLayerStyle, kGraphCosmetic));
const std::unordered_set<std::string> kGraphHistExtra = {"width", "bin", "freq", "frequency",
                                                          "fraction", "percent", "normal"};
// Item 6 (User ruling 2026-09-09): `gen`/`generate` moved OUT of
// kGraphKdensUnimpl (below) and INTO here — it is now honoured (writes the
// grid/density pair back into the caller's frame, dispatcher.cpp's kdensity
// arm). `at()`/`addplot` stay refused by name; neither is implemented.
const std::unordered_set<std::string> kGraphKdensExtra = {"kernel", "bwidth", "n", "normal",
                                                           "gen", "generate"};
const std::unordered_set<std::string> kGraphXtlineExtra = {"overlay"};
// Per-type refusal, for the option that would change a NUMBER rather than a
// colour: `histogram, discrete` puts one bin on each distinct value
// (different bin edges, different heights). Accepting it quietly would be a
// wrong answer, so it is refused by name instead of noted.
const std::unordered_set<std::string> kGraphHistUnimpl =
    unionOf(kGraphBaseUnimpl, {"discrete", "addplot"});
const std::unordered_set<std::string> kGraphKdensUnimpl =
    unionOf(kGraphBaseUnimpl, {"at", "addplot"});
// The umbrella union: for `twoway` and `graph` the plot TYPE is derived from the
// varlist, not from the dispatched token, so those two carry every type's extras.
const std::unordered_set<std::string> kGraphAllTypesKnown =
    unionOf(unionOf(kGraphSharedKnown, kGraphHistExtra),
            unionOf(kGraphKdensExtra, kGraphXtlineExtra));
// TWO DECLARED SPELLINGS (BUG-430's mechanism, errors.h). W4 put `xtick`/`ytick`
// in the vocabulary, and that made `xti` a prefix of BOTH `xtitle` and `xtick`
// -- ambiguous, therefore left alone, therefore refused. `xti("...")` /
// `yti("...")` are 6 corpus statements (scatter, graph, binscatter) and THIS
// BUILD ALREADY RESOLVED THEM TO xtitle/ytitle, measured on main's binary before
// this change (`--dump-optvocab --resolve scatter xti` -> in_known). So the map
// records a resolution that already existed and that W4's own additions would
// otherwise have taken away; it is not a new claim about Stata's abbreviation
// table, which was not consulted (no oracle run this stint).
const std::unordered_map<std::string, std::string> kGraphAliases = {
    {"xti", "xtitle"}, {"yti", "ytitle"}};
const piet::eval::OptVocab& kVocabGraphPlain = piet::eval::registerOptVocab(
    {"scatter", "line", "tsline"}, {kGraphSharedKnown, kGraphBaseUnimpl, kGraphAliases});
const piet::eval::OptVocab& kVocabGraphHist = piet::eval::registerOptVocab(
    {"histogram", "hist"},
    {unionOf(kGraphSharedKnown, kGraphHistExtra), kGraphHistUnimpl, kGraphAliases});
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
// binscatter already has a net and a vocabulary (handlers_qe.cpp:85). This
// registration UNIONS the graph drawing keys into the SAME registry slot -- the
// documented merge rule (errors.h) -- so `binscatter y x, nq(20) xtitle("...")
// scheme(s1mono)` stops being r(198) while `by()`/`linetype()`/`controls()` keep
// the refusals that arm measured for itself.
const piet::eval::OptVocab& kVocabBinscatterGraph = piet::eval::registerOptVocab(
    {"binscatter"}, {kGraphSharedKnown, {}, kGraphAliases});
// The keys each of the three actually reads -- the `applied` argument to
// graphAcceptedNotApplied at their three dispatch arms.
const std::unordered_set<std::string> kCoefplotApplied = {
    "level", "drop", "keep", "xline", "title", "xtitle", "ytitle", "name"};
const std::unordered_set<std::string> kMarginsplotApplied = {"title", "note", "name"};
const std::unordered_set<std::string> kBinscatterApplied = {"nquantiles"};

// W4, "honesty over silence". A graph arm pushes exactly ONE block, so a note
// cannot be a sibling display block; it is appended to the block's own `note`
// string, which every graph renderer already draws (graph_m4.cpp:75 gives every
// bare block the key, and handlers_aer_graphx.cpp:294 does the same thing for
// grc1leg/event_plot -- this is that helper, at the dispatcher).
//
// `applied` is the set of keys the path that BUILT this block actually read, so
// the note is a statement about the code that ran, not about a hand-maintained
// list: re-routing a `scatter ... , xline(0)` into the layer renderer moves
// `xline` from the noted set to the applied one automatically, because the two
// call sites pass different `applied` sets. Keys the user did not type never
// appear, and a block with nothing to report is left byte-identical -- which is
// what keeps the freeze fixtures (all of them bare commands, no options) pinned.
void graphAcceptedNotApplied(nlohmann::ordered_json& block, const std::string& cmdName,
                             const std::vector<std::pair<std::string, piet::eval::OptValue>>& opts,
                             const std::unordered_set<std::string>& applied) {
  if (!block.is_object() || block.value("type", std::string()) != "graph") return;
  std::vector<std::string> noted;
  for (const auto& kv : opts) {
    if (applied.count(kv.first) != 0) continue;
    if (std::find(noted.begin(), noted.end(), kv.first) != noted.end()) continue;
    noted.push_back(kv.first);
  }
  if (noted.empty()) return;
  std::string joined;
  for (size_t i = 0; i < noted.size(); ++i) {
    if (i != 0) joined += ", ";
    joined += noted[i];
  }
  const std::string text = "(" + cmdName + ": " + joined + " accepted and not applied — this "
                           "build draws the plot without " +
                           (noted.size() == 1 ? std::string("it") : std::string("them")) + ".)";
  std::string cur;
  if (block.contains("note") && block["note"].is_string()) cur = block["note"].get<std::string>();
  block["note"] = cur.empty() ? text : (cur + "  " + text);
}
}  // namespace

namespace piet {

using ojson = nlohmann::ordered_json;

// P3 weights wave: forward-declared here rather than added to
// piet/handlers.h (out of this batch's edit manifest — owned by a different
// stint). Defined in handlers.cpp within the same piet::handlers namespace
// (external linkage, same TU-independent matching every other
// piet::handlers::runXxx symbol already relies on) — this TU can call them
// without a header change. runSummarizeAweight is the [aw=] sibling of the
// already-header-declared runSummarize (picked between at the summarize
// dispatch arm below, off parsed.weightType); runMean/runProportion back
// the two brand-new registry rows.
namespace handlers {
ojson runSummarizeAweight(const std::optional<std::vector<std::string>>& varlist, bool detail,
                          SessionState& session, const std::optional<std::string>& ifExpr,
                          const std::optional<std::string>& weightVar,
                          const std::optional<std::string>& inRange);
ojson runMean(const eval::ParsedCommand& parsed, SessionState& session);
ojson runProportion(const eval::ParsedCommand& parsed, SessionState& session);
// W2 esttabnames (BUG-552): `estimates dir NAMELIST` -- eststo_esttab.cpp,
// declared here on the same pattern (handlers.h is outside that lane).
ojson runEststoDirNames(const std::vector<std::string>& namelist, SessionState& session);
}  // namespace handlers

namespace {

ojson echoBlock(const std::string& cmd) {
  dprof::noteJsonNode();
  ojson e;
  e["type"] = "echo";
  // A folded `mata:` block (mata::captureMataBlocks) is one line joined by a
  // control byte; show it the way Stata's log does, one `: stmt` per line. The
  // byte occurs in no other line, so every other echo is unchanged.
  e["text"] = ".  " + (cmd.find(mata::kMataStmtSep) == std::string::npos &&
                               cmd.find(mata::kMataUnterminated) == std::string::npos
                           ? cmd
                           : mata::renderFoldedMataForEcho(cmd));
  return e;
}

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `lower(...)` call sites
// didn't need touching. Pure dedup, no behavior change.
std::string lower(std::string s) { return strutil::lower(std::move(s)); }

std::string rstripColon(std::string s) {
  while (!s.empty() && s.back() == ':') s.pop_back();
  return s;
}

// BUG-105 (WS-K) — the escape hatch for handlers that re-parse the RAW line
// with their OWN command-word pattern instead of reading the routed token.
//
// The 2026-07-31 design assumed normalising the routed token at the single `cmd`
// formation site subsumed every second copy of a spelling set. Measured: it does
// for the if-chain arms, kNoDataCommands and kFixedArityCommands, but NOT for
// two handlers that hold their own copy —
//   · handlers_m3.cpp's runMatrixDef (five `^\s*mat(?:rix)?\s+…` regexes)
//   · handlers_m3.cpp's runLabelDefine (`^label\s+define\s+`)
// — because they receive `parsed.raw`, never `cmd`. Under the widened sets
// `matr list X` and `labe define L 1 "a"` would have fallen through to those
// handlers' generic "unsupported form" rejects: a WRONG ANSWER wearing a
// plausible message, which is worse than the r(199) they used to get.
//
// Rewriting only the LEADING token keeps the spelling knowledge in the registry
// and leaves those handlers' patterns untouched. Case-insensitive on the token
// (the tokenizer lowercases the routed name; the raw line keeps the user's
// case), and a no-op whenever the typed spelling is already canonical — so
// every non-abbreviated line is byte-for-byte unchanged.
std::string rawWithCanonicalCmd(const std::string& raw, const std::string& typed,
                                const std::string& canonical) {
  if (typed == canonical || typed.empty()) return raw;
  size_t b = raw.find_first_not_of(" \t");
  if (b == std::string::npos) return raw;
  if (raw.size() - b < typed.size()) return raw;
  for (size_t i = 0; i < typed.size(); ++i) {
    if (static_cast<char>(std::tolower(static_cast<unsigned char>(raw[b + i]))) != typed[i]) {
      return raw;
    }
  }
  size_t e = b + typed.size();
  // Must be a whole token, not a prefix of a longer word (`sav` inside `saved`).
  if (e < raw.size() && (std::isalnum(static_cast<unsigned char>(raw[e])) != 0 || raw[e] == '_')) {
    return raw;
  }
  return raw.substr(0, b) + canonical + raw.substr(e);
}

// BUG-251 (WS-Q) — the argument tail of a command line, derived WITHOUT any
// knowledge of the command's spelling.
//
// BUG-232 fixed four sites that located the command word in `parsed.raw` with a
// LITERAL (`find("save")`, `find("label")`), by switching them to the typed
// spelling `cmdTyped`. That closes the reachable crashes, but it leaves the
// mechanism intact: every one of those sites still asks "where in this line is
// the command word?" and still indexes off the answer with no npos guard. The
// answer is only ever "at the front", because eval::tokenize() sets
// `pc.raw = strip(line)` — the raw it hands the arms is the very line the
// command word was tokenized out of, leading whitespace already gone.
//
// So the honest formulation is not "find the word" but "skip the leading
// token", which is what every one of these call sites actually wanted. It
// carries ZERO spelling knowledge, so it is not a second copy of any spelling
// set and cannot drift from the registry; and it cannot throw, whatever the
// line looks like.
//
// The token ends at the first character that is not [A-Za-z0-9_], NOT at the
// first space — deliberately, to stay byte-compatible with the `find(cmd) +
// cmd.size()` arithmetic it replaces: `dis(2+2)` must still yield `(2+2)` and
// `cd/tmp` must still yield `/tmp`, which a whitespace-delimited split would
// swallow whole.
//
// WHY THE OLD SHAPE WAS A CRASH AND NOT MERELY WRONG: `find()` returns npos ==
// SIZE_MAX, and the sites add a small constant to it. That addition WRAPS —
// `npos + 3` is 2, not a huge number — so `substr(npos + 3)` silently reads
// from offset 2 for any line at least 2 long (a wrong answer, quietly), and
// throws std::out_of_range only for a line SHORTER than that. Measured: with
// `use` given its Stata abbreviations, the one-character line `u` threw
// `cpp_exception: basic_string` and killed the whole --do run, while `us` and
// `u , clear` returned plausible output from the wrong offset. A bare
// `raw.substr(labelPos)` with labelPos == npos throws unconditionally.
std::string rawArgsAfterCmd(const std::string& raw) {
  size_t b = raw.find_first_not_of(" \t");
  if (b == std::string::npos) return std::string();
  size_t e = b;
  while (e < raw.size() &&
         (std::isalnum(static_cast<unsigned char>(raw[e])) != 0 || raw[e] == '_')) {
    ++e;
  }
  if (e == b) return std::string();  // no leading identifier token at all
  size_t a = raw.find_first_not_of(" \t", e);
  // strutil::trim, not trimWs() — the latter is this file's thin wrapper over
  // it and is not declared until further down.
  return (a == std::string::npos) ? std::string() : strutil::trim(raw.substr(a));
}

// mutate.py's storage-type prefix strip (`gen byte edu_hi = ...`) — item 3.
// Also detects string storage types (str#, strL) so the caller can route to
// unsupported_command instead of silently mis-generating a numeric column.
struct StorageTypeInfo {
  std::string expr;     // with the leading storage-type token removed
  bool stringTyped = false;
  // BUG-250: the DECLARED token itself (lowercased: "double", "byte", "str8",
  // ...; "" = no declaration). The strip used to be where the user's storage
  // subtype DIED — parsed, used only for the string-typed routing decision,
  // and discarded — so `describe` called every engine-created column "float"
  // and `: type` had nothing to answer. The creation arms below now record it
  // into VarMeta::stataType (the field the .dta import path already fills and
  // describe/`: type`/the save sidecar already read). It records the CLAIM,
  // not the storage: every numeric column is f64 in the datastore and stays
  // f64 — Stata's storage type is a declaration carried in metadata, and
  // whether storage should ever follow the claim is a different question,
  // named in BUG-250's ledger close.
  std::string typeToken;
};

StorageTypeInfo stripStorageType(const std::string& raw) {
  StorageTypeInfo info;
  info.expr = raw;
  std::string beforeEq = raw.substr(0, raw.find('='));
  size_t start = beforeEq.find_first_not_of(" \t");
  if (start == std::string::npos) return info;
  std::string trimmed = beforeEq.substr(start);
  // tokens: byte|int|long|float|double|str#|str#L|strL (case-insensitive),
  // followed by whitespace.
  size_t sp = trimmed.find_first_of(" \t");
  if (sp == std::string::npos) return info;
  std::string tok = lower(trimmed.substr(0, sp));
  bool isNumericType = (tok == "byte" || tok == "int" || tok == "long" || tok == "float" ||
                        tok == "double");
  bool isStrType = false;
  if (tok.rfind("str", 0) == 0) {
    // str, str#, str#L, strL — everything after "str" is digits and/or "l".
    std::string rest = tok.substr(3);
    bool ok = true;
    for (char c : rest) {
      if (!std::isdigit(static_cast<unsigned char>(c)) && c != 'l') {
        ok = false;
        break;
      }
    }
    isStrType = ok;
  }
  if (isNumericType || isStrType) {
    size_t consumed = (start + sp);
    // consume the run of whitespace after the type token too
    size_t after = raw.find_first_not_of(" \t", consumed);
    info.expr = (after == std::string::npos) ? "" : raw.substr(after);
    info.stringTyped = isStrType;
    info.typeToken = tok;  // BUG-250: keep the declaration alive
  }
  return info;
}

// BUG-471 T2: an UNTYPED creation under `set type double` is a double, and its
// label says so (w2e_float_default.log settype_double_gen :1252,
// egen_mean_settype_double :1196). Under the factory default float the record is
// BUG-295's implicit plane exactly as before, so the default path is unchanged.
StorageTypeInfo withSessionDefaultType(const SessionState& session, StorageTypeInfo info) {
  if (info.typeToken.empty() && session.defaultStorageType != "float")
    info.typeToken = session.defaultStorageType;
  return info;
}

// BUG-250: record a declared storage subtype after a creation arm succeeded.
// `blk` is the block the arm is about to return — an "error" block means the
// column was not created and nothing may be recorded (a stale claim on a
// non-column is the BUG-190 defect family). No declaration or no name = no-op,
// so every undeclared `gen` stays byte-identical.
void recordDeclaredStataType(SessionState& session, const std::string& varname,
                             const StorageTypeInfo& info, const ojson& blk) {
  if (varname.empty()) return;
  if (blk.is_object() && blk.value("type", std::string()) == "error") return;
  if (info.typeToken.empty()) {
    // BUG-295: no declaration means Stata's own default, `float` — record it on
    // the implicit plane so the .dta export stops handing this column to the
    // writer's narrowest-lossless scan (which stored `byte` for `gen z = _n`,
    // and made the SAME column answer float before a save and byte after).
    // Not written to varMeta.stataType: `describe`/`: type` already answer
    // float here via BUG-250's fallback, and a declaration this layer never
    // saw must not start looking like one.
    session.implicitFloatVars.insert(varname);
    return;
  }
  session.implicitFloatVars.erase(varname);  // an explicit declaration wins
  session.varMeta[varname].stataType = info.typeToken;
}

// BUG-373: several verbs report r(N) = the number of observations they PROCESSED,
// which for a reshaping verb (collapse/contract/fillin) is the row count BEFORE
// it ran, not after. Captured from live StataBE: all three report 336 on a
// 336-row fixture even though collapse leaves one row behind.
namespace {
int64_t rowsBeforeForRN(SessionState& session) {
  if (std::optional<int64_t> r = session.residentRowsIfCurrent()) return *r;
  Datastore* ds = session.datastore();
  return ds != nullptr ? ds->nRows() : -1;
}
}  // namespace

std::string joinVarlist(const std::vector<std::string>& varlist) {
  std::string s;
  for (size_t i = 0; i < varlist.size(); ++i) {
    if (i) s += " ";
    s += varlist[i];
  }
  return s;
}

// --- sysuse (mission 3, hardening queue) path helpers -----------------------
// `sysuse DATASET` resolves a bundled sample dataset by bare name, no path.
// PIET ships exactly one today — piet/packaging/sample/synthetic_panel.csv, the
// SAME fixture main.cpp's runUiSmoke() already loads bundle-first via
// piet::bundleResourcesDir()+"/sample/synthetic_panel.csv", dev-tree
// piet/packaging/sample/synthetic_panel.csv otherwise (see main.cpp:957-968).
// This mirrors that exact resolution order rather than inventing a second
// convention. main.cpp is out of this workstream's file manifest, so the
// existence check and the dev-tree __FILE__-relative anchor (main.cpp's own
// repoRelative(), main.cpp:212-217) are duplicated locally here — small,
// single-purpose, not worth a shared header for two call sites.
bool fileExistsLocal(const std::string& path) {
  if (path.empty()) return false;
  std::ifstream f(cecon::to_native_path(path));
  return f.good();
}

std::string devTreeSyntheticSampleCsv() {
  std::string thisFile = __FILE__;  // .../piet/native/src/dispatcher.cpp
  auto pos = thisFile.rfind("/piet/native/src/dispatcher.cpp");
  std::string root = (pos == std::string::npos) ? "../.." : thisFile.substr(0, pos);
  return root + "/piet/packaging/sample/synthetic_panel.csv";
}

// bysort.py's needs_bygen: `_n` or `_N` as a whole identifier token anywhere
// in expr (not part of a longer identifier). A conservative scan mirroring
// the oracle's regex intent without pulling in <regex> for two literals.
std::string dispatcherNextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

// Item 6 (User ruling 2026-09-09): `kdensity ..., generate(kx ky)` writes
// the grid/density pair back into the caller's frame. runKdensity
// (graph_m4.cpp) takes a Datastore, not a SessionState, and cannot write
// columns itself — the kdensity dispatch arm does it, once the graph block
// is fully built. mintScratchPath always names a ".hbin"; the setcol
// payload is a CSV — this is the SAME register-the-name-that-actually-
// exists idiom as handlers_aer_abbrev.cpp's, handlers_aer_gtools.cpp's and
// handlers_aer_results.cpp's own `mintCsvPath` copies (a 4th copy of an
// already-3x-duplicated helper), and dispatcherWriteGridColumn below is
// (with those files' estimWriteColumn) the 5th+ copy of the setcol-write
// shape. IDIOMS' Duplication rule names the hoist obligation — KNOWN
// STANDING DEBT, not paid down here to keep this item's blast radius to
// one file.
std::string dispatcherMintCsvPath(const std::string& workdir, const std::string& prefix) {
  const std::string hbin = mintScratchPath(workdir, prefix);
  forgetScratchMint(hbin);
  const std::string csv = hbin.substr(0, hbin.size() - 5) + ".csv";  // 5 == strlen(".hbin")
  registerScratchMint(csv);
  return csv;
}

// Writes one f64 column into the session frame. Missing = an EMPTY line
// (what `cobra setcol` reads as missing; "nan" would land a literal token
// in a numeric column).
bool dispatcherWriteGridColumn(SessionState& session, const std::string& name,
                               const std::vector<double>& vals, std::string* errMsg) {
  const std::string csv = dispatcherMintCsvPath(session.workdir(), "kdensgen");
  {
    std::ofstream f(cecon::to_native_path(csv));
    if (!f) {
      *errMsg = "could not write the values payload";
      return false;
    }
    f.setf(std::ios::fmtflags(0), std::ios::floatfield);
    f.precision(17);
    for (double d : vals) {
      if (!std::isnan(d)) f << d;
      f << "\n";
    }
    if (!f) {
      *errMsg = "could not write the values payload";
      return false;
    }
  }
  const std::string out = mintScratchPath(session.workdir(), "kdensgen");
  const CobraRunResult r = runCobra({"setcol", "--data", *session.currentHbin(), "--var", name,
                                     "--values", csv, "--dtype", "f64", "--new", "--out", out});
  if (!r.ok) {
    *errMsg = "could not create " + name;
    return false;
  }
  // BUG-133-class: the engine's writer creates --out before it can detect a
  // write failure, so a full scratch volume can reach here as a zero exit
  // code. A session.load on a path that is not there records the head
  // anyway and hands the next open a missing mapping.
  if (!cecon::native_exists(out)) {
    *errMsg = "failed writing output dataset (disk full or I/O error)";
    return false;
  }
  session.load(out);
  return true;
}

bool hasNorCapN(const std::string& expr) {
  auto isIdentChar = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  };
  for (size_t i = 0; i + 1 < expr.size() + 1; ++i) {
    for (const char* needle : {"_n", "_N"}) {
      size_t len = 2;
      if (i + len > expr.size()) continue;
      if (expr.compare(i, len, needle) != 0) continue;
      bool leftOk = (i == 0) || !isIdentChar(expr[i - 1]);
      bool rightOk = (i + len >= expr.size()) || !isIdentChar(expr[i + len]);
      if (leftOk && rightOk) return true;
    }
  }
  return false;
}

}  // namespace

namespace {

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — see the
// lower() wrapper above for why this stays a thin pass-through.
std::string trimWs(const std::string& s) { return strutil::trim(s); }

// D1-②: the prefix table, sorted once instead of once per call.
//
// WHY. stripPrefix used to be handed a `std::set<std::string>` and, ON EVERY
// CALL, copy it into a vector (one heap allocation per spelling — 5 for the
// capture set, 11 for the quietly set) and sort that vector, then throw both
// away. It runs twice per dispatched line, so `local q 5` paid ~32 string
// allocations and two sorts to answer "does this line begin with `capture`?".
// The WS-D1 zone profile put the pair at 10.7% of the whole per-line budget,
// second only to the macro-definition regexes; the sampling profiler agreed
// (9.5%, of which 17% was the set->vector copy and 11% more the sort).
//
// The std::set stays the declared source of truth at each call site; the table
// is DERIVED from it in a function-local static, so it is built once and
// cannot drift from the set it came from. `first` is a first-byte bitmap that
// rejects a line which cannot begin with any spelling in the table before a
// single std::string is touched.
struct PrefixTable {
  std::vector<std::string> byLenDesc;  // longest-first (see the loop below)
  bool first[256] = {false};

  explicit PrefixTable(const std::set<std::string>& prefixes)
      : byLenDesc(prefixes.begin(), prefixes.end()) {
    std::sort(byLenDesc.begin(), byLenDesc.end(),
              [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    for (const auto& p : byLenDesc) {
      if (p.empty()) continue;
      unsigned char lo = static_cast<unsigned char>(p[0]);
      first[lo] = true;
      if (lo >= 'a' && lo <= 'z') first[lo - 'a' + 'A'] = true;
    }
  }
};

inline char asciiLower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// dispatcher.py's `_strip_prefix`: if `line` starts with a complete word in
// `prefixes` (optionally followed by ':'), return the remainder; else nullopt.
std::optional<std::string> stripPrefix(const std::string& line, const PrefixTable& table) {
  // First-byte reject, ahead of the trim's allocation. trimWs() is
  // strutil::trim, whose leading edge uses the same isspace class this scan
  // does, so the byte found here is exactly the byte the old
  // `strippedLower.rfind(pfx, 0)` compared first.
  size_t b = 0;
  while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
  if (b >= line.size() || !table.first[static_cast<unsigned char>(line[b])]) return std::nullopt;

  dprof::notePrefixSetSort();
  std::string stripped = trimWs(line);
  // Longest-first so "quiet"/"quietly" don't shadow each other incorrectly
  // (mirrors Python's `sorted(prefixes, key=len, reverse=True)`).
  for (const auto& pfx : table.byLenDesc) {
    if (stripped.size() < pfx.size()) continue;
    bool eq = true;
    for (size_t i = 0; i < pfx.size(); ++i) {
      if (asciiLower(stripped[i]) != pfx[i]) {
        eq = false;
        break;
      }
    }
    if (!eq) continue;
    std::string rest = stripped.substr(pfx.size());
    if (!rest.empty() && rest[0] != ' ' && rest[0] != '\t' && rest[0] != ':') continue;
    // strip a single leading ':' then whitespace
    size_t i = 0;
    if (i < rest.size() && rest[i] == ':') ++i;
    rest = trimWs(rest.substr(i));
    if (!rest.empty()) return rest;
  }
  return std::nullopt;
}

// BUG-112(1) PIET-half: Stata's `generate` REFUSES a name that already exists
// (r(110) "variable X already defined"); `replace` is the verb that overwrites.
// PIET honoured that on exactly ONE of its four gen sinks — strmutate::
// runStringGen guards (strmutate.cpp:977) — while the three NUMERIC sinks
// (handlers::runGen · egen::runBysortGen via the standalone `_n`/`_N` + `in`
// route · the same handler via the `bysort v:` prefixed route) all shipped
// straight to `cobra eval --gen`, whose Frame::generate is create-or-replace.
// Live-measured on this tree before the fix: `gen x = 2` over an existing x==1
// moved summarize's mean 1 -> 2 with NO diagnostic, and likewise `gen z = 77`
// over `gen z = _n` (3 -> 77) and `bysort x: gen y = 99` (3 -> 99).
//
// The guard lives HERE, at the VERB, not in the handlers: runGen/runBysortGen
// are also the internal engine of ~10 deliberately create-or-replace callers
// (reshape/egen/encode/bygen-tag/merge-cardinality/mkspline — and `replace`
// itself, which builds the identical `--gen` call), so a handler-level guard
// would reject all of them; selftest's direct piet::handlers::runGen() legs
// bypass dispatch() and stay untouched for the same reason. Verb-level also
// keeps the fix engine-INDEPENDENT — it takes no dependency on the cobra-half's
// `--gen-strict` flag (BUG-112(1) engine-half), so the Art-7 archive-pin arm
// behaves identically to a fresh build instead of failing on an unknown flag.
//
// Message spelling is strmutate's VERBATIM: errors.cpp::coarseRc keys r(110)
// off the "already defined" substring, so a `capture`-wrapped gen sets the same
// _rc Stata does, and the two sinks cannot drift apart in wording.
std::optional<ojson> genTargetExistsError(SessionState& session, const std::string& varname) {
  if (varname.empty() || !session.hasData()) return std::nullopt;
  bool exists = false;
  // C4-L2A (D1 card plane): existence is card-answerable — a CURRENT card is
  // authoritative for name presence either way (handlers_m3.cpp::runConfirm's
  // idiom). nullptr means "answer from the datastore", never "assume absent".
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    for (const auto& vi : *card) {
      if (vi.name == varname) {
        exists = true;
        break;
      }
    }
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return std::nullopt;
    exists = ds->has(varname);
  }
  if (!exists) return std::nullopt;
  return ojson(eval::engineError("variable " + varname + " already defined"));
}

// `[type] NEWVAR = expr` (post-stripStorageType text) -> NEWVAR. First `=`
// wins, matching both the gen block's own inline split and handlers.cpp::
// splitAssign. No `=` at all -> empty, i.e. NO exists-guard: that form is a
// syntax error the downstream handler already owns, and Stata likewise reports
// the syntax problem rather than the name one.
std::string genTargetName(const std::string& exprAfterType) {
  size_t eq = exprAfterType.find('=');
  if (eq == std::string::npos) return std::string();
  return trimWs(exprAfterType.substr(0, eq));
}

}  // namespace

// ---------------------------------------------------------------------------
// M3b: bysort/by prefix — C++ port of bysort.py's _parse_bysort_prefix +
// run_bysort_cmd. Split out (not in the anonymous namespace above) only so
// it can stay grouped with its own small helpers; still translation-unit
// local.
namespace {

struct BysortPrefix {
  bool isBysort = false;      // false = not a bysort/by line at all
  bool sortFirst = false;     // true=bysort (re-sort), false=by (no re-sort)
  std::vector<std::string> byVars;
  // BS-002: `bysort byvar (sortvar1 sortvar2):` — vars inside the
  // parenthetical are a SORT-ONLY tiebreak key (Stata: equivalent to `sort
  // byvar sortvar1 sortvar2` before grouping on byvar alone), never part of
  // the by-GROUP key. Empty when no parenthetical was present.
  std::vector<std::string> sortVars;
  std::string subCmd;
  std::string subRaw;
};

// _parse_bysort_prefix: cmd="bysort"/"by", varlist=["foreign:","gen","idx=_n"]
// (or the colon as a standalone token), raw="bysort foreign: gen idx=_n".
BysortPrefix parseBysortPrefix(const std::string& cmdLower, const std::vector<std::string>& varlist,
                               const std::string& raw) {
  BysortPrefix p;
  if (cmdLower != "bysort" && cmdLower != "by") return p;
  p.isBysort = true;
  p.sortFirst = (cmdLower == "bysort");

  // BS-002: byVars-collection loop, now paren-aware. A token opening with
  // '(' (and everything up to the token that closes it with ')') is the
  // sort-only parenthetical — collected into sortVars instead of byVars.
  // The trailing ':' that ends the by-spec may be glued to a plain byVar
  // token, to the token that CLOSES the parenthetical, or stand alone —
  // all three are handled uniformly below.
  std::optional<size_t> colonIdx;
  bool inParen = false;
  for (size_t i = 0; i < varlist.size(); ++i) {
    std::string tok = varlist[i];

    if (tok == ":") {
      colonIdx = i;
      break;
    }

    bool endsWithColon = (!tok.empty() && tok.back() == ':');
    if (endsWithColon) tok.pop_back();

    if (!inParen && !tok.empty() && tok.front() == '(') {
      tok.erase(tok.begin());
      inParen = true;  // may close within this same token, handled just below
    }

    if (inParen) {
      size_t closeParen = tok.find(')');
      if (closeParen != std::string::npos) {
        std::string before = tok.substr(0, closeParen);
        if (!before.empty()) p.sortVars.push_back(before);
        inParen = false;
      } else if (!tok.empty()) {
        p.sortVars.push_back(tok);
      }
    } else if (!tok.empty()) {
      p.byVars.push_back(tok);
    }

    if (endsWithColon) {
      colonIdx = i;
      break;
    }
  }

  if (!colonIdx.has_value()) {
    // Malformed — no colon found; everything is by_vars, no sub_cmd.
    return p;
  }

  std::vector<std::string> subTokens(varlist.begin() + static_cast<long>(*colonIdx) + 1,
                                     varlist.end());
  if (subTokens.empty()) return p;
  p.subCmd = subTokens[0];
  std::vector<std::string> subVarlist(subTokens.begin() + 1, subTokens.end());

  size_t colonPos = raw.find(':');
  if (colonPos == std::string::npos) {
    std::string joined = p.subCmd;
    for (const auto& t : subVarlist) joined += " " + t;
    p.subRaw = joined;
  } else {
    p.subRaw = trimWs(raw.substr(colonPos + 1));
  }
  return p;
}

// egen.py-adjacent: split `s` at the first COMMA outside parens (options
// separator) — mirrors bysort.py's _split_top_level_comma, used to inject a
// by() clause into an egen sub-command's option tail.
std::pair<std::string, std::optional<std::string>> splitTopLevelComma(const std::string& s) {
  int depth = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    char ch = s[i];
    if (ch == '(') {
      ++depth;
    } else if (ch == ')') {
      --depth;
    } else if (ch == ',' && depth == 0) {
      return {s.substr(0, i), s.substr(i + 1)};
    }
  }
  return {s, std::nullopt};
}

std::string joinSpace(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += " ";
    s += v[i];
  }
  return s;
}

// P2-PERF ARC-C C1-③ (factored out for BUG-532): is the key tuple `keys`
// already lexicographically non-decreasing, row by row, under gsort's OWN key
// semantics (src/data/frame.cpp `cols_as_double` + radix_sort.h
// `gsort_radix_key`)? A factor's key is its integer CODE as a double, an f64's
// key is its value, NaN sorts LAST, and the sort is stable — so a `true` here
// means a `sort KEYS` would be the identity permutation. The by-summarize
// driver asks exactly that (skip a no-op sort). It is deliberately NOT the
// level-string check the `by:` summarize branch uses for its r(5): that one
// reads factor LEVEL STRINGS, so it calls data PIET itself just sorted on a
// string key "not sorted". Every key must exist in `ds` (callers check first).
//
// `stataStringOrder` (BUG-532, the by-keep/drop branch): a string key is read
// in STATA's order instead — its level's byte-order rank, a blank first
// (handlers::stringKeyOrder) — so `true` means the rows are in the order a
// Stata `sort KEYS` leaves them. The two answers differ only on a string key
// whose dictionary is not byte-ordered.
bool keysAlreadyGrouped(Datastore& ds, const std::vector<std::string>& keyNames,
                        bool stataStringOrder = false) {
  const int64_t nBase = ds.nRows();
  // gsort's key ordering, as a plain comparison: NaN above every number.
  auto keyLess = [](double a, double b) {
    bool an = std::isnan(a), bn = std::isnan(b);
    if (an || bn) return !an && bn;  // NaN last; NaN vs NaN is neither
    return a < b;
  };
  struct GsortKeyView {
    bool isFactor = false;
    const double* f64 = nullptr;
    const int32_t* codes = nullptr;
    std::vector<double> rank;  // stataStringOrder: per code; blankRank for -1
    double blankRank = 0;
    double at(int64_t r) const {
      if (!isFactor) return f64[r];
      if (rank.empty()) return static_cast<double>(codes[r]);
      return codes[r] < 0 ? blankRank : rank[static_cast<size_t>(codes[r])];
    }
  };
  std::vector<GsortKeyView> keys;
  keys.reserve(keyNames.size());
  for (const auto& bv : keyNames) {
    GsortKeyView v;
    v.isFactor = ds.dtype(bv) != "f64";
    const cecon::Column& c = ds.col(bv);
    if (v.isFactor) {
      v.codes = c.codes();
      if (stataStringOrder) {
        handlers::StringKeyOrder o = handlers::stringKeyOrder(c.levels);
        if (!o.codeOrderIsStata) {
          v.rank = std::move(o.rank);
          v.blankRank = o.blankRank;
        }
      }
    } else {
      v.f64 = c.f64();
    }
    keys.push_back(std::move(v));
  }
  for (int64_t r = 1; r < nBase; ++r) {
    for (const auto& k : keys) {
      double prev = k.at(r - 1), cur = k.at(r);
      if (keyLess(cur, prev)) return false;  // strictly decreasing on this key
      if (keyLess(prev, cur)) break;         // strictly increasing -> row pair ok
      // equal on this key -> the next key decides
    }
  }
  return true;
}

// BUG-532: Stata's sortedby marker, as far as PIET can know it, for a plain
// `by KEYS:` (which Stata refuses r(5) unless KEYS lead the marker).
//
// Every `bysort KEYS [(SORT)]: CMD` sorts PIET's rows in place into the order
// Stata's `bysort` leaves (handlers::sortInStataOrder: runBysortGen, the egen
// and summarize/regress/tabstat branches, runByKeepIf), so after any bysort
// PIET's rows ARE Stata's rows and the engine file's own sort claim is the
// marker (below). No side ledger of row orders is kept: it was tried
// (task/w2-bykeep-v2 rounds 3-4) and each review found a new way for a
// ledger mark to disagree with the rows (a macro-spelled gsort, a fresh `use`,
// a re-used file), because a ledger describes lines, not rows.

// (2) The engine's own claim: the head file's `sorted_by` header key (BUG-272).
// Never false where present — Frame::gsort mints it only for ascending f64 keys,
// and every mutation that could void it clears it — and minted exactly where
// Stata's `sort` sets its marker, so a key tuple that does not lead it is one
// Stata calls "not sorted" even when the rows happen to be grouped (arm
// by_two_halfsorted: `sort g b` then `by g h:`). Empty = no claim (factor keys,
// a claimless transform, a file from before BUG-272): the caller falls back to
// the rows themselves. Read off the head's open Datastore (Datastore::sortedBy),
// never by reopening the file.
std::vector<std::string> engineSortClaim(const Datastore& ds) {
  std::vector<std::string> keys;
  std::istringstream in(ds.sortedBy());
  for (std::string k; in >> k;) keys.push_back(k);
  return keys;
}

bool keysLeadClaim(const std::vector<std::string>& keys, const std::vector<std::string>& claim) {
  if (keys.size() > claim.size()) return false;
  for (size_t i = 0; i < keys.size(); ++i)
    if (keys[i] != claim[i]) return false;
  return true;
}

// BUG-532 (review of d82bdfa6, finding F): the by-keys and the parenthesised
// sort keys of a `by/bysort` prefix are a varlist, and Stata resolves them as
// one: a wildcard or range expands (loops::expandVarlist, the BUG-450 varlist
// expander every plain data verb uses), and a name that is not a variable but
// is the unique prefix of one stands for it (varname abbreviation; several
// candidates is r(111) "ambiguous abbreviation"). expandVarlist does not
// resolve abbreviations (a literal token passes through unchanged, and the
// plain `sort` verb answers `sort xv` r(111) on main too), so the prefix step
// is this function's own -- a design decision, no captured arm; [U] 11.2
// "Abbreviation rules". A token that matches nothing is left as typed: each
// branch below refuses it by its own rule (r(111) for keep/drop, the
// summarize driver's own check).
std::optional<ojson> resolveByKeyNames(SessionState& session, std::vector<std::string>& names) {
  if (names.empty() || !session.hasData()) return std::nullopt;
  bool patterned = false;
  for (const auto& t : names)
    if (t.find_first_of("*?-") != std::string::npos || lower(t) == "_all") patterned = true;
  std::vector<std::string> all;
  if (const std::vector<VarInfo>* card = session.shadowVarlistIfCurrent()) {
    for (const auto& v : *card) all.push_back(v.name);
  } else if (Datastore* ds = session.datastore()) {
    all = ds->names();
  }
  if (patterned) {
    loops::VarlistExpandResult ex = loops::expandVarlist(joinSpace(names), session);
    if (ex.isError) {
      // Review of e19291ad (REFUSAL rc): expandVarlist's range branch
      // (`zz-yy` with an endpoint that is no variable) returns a block with no
      // rc, which `capture` reads as 1. Stata answers r(111) "variable zz not
      // found". A block that already carries its rc passes through unchanged.
      const std::string reason = ex.error.value("reason", std::string());
      if (reason.compare(0, 2, "r(") == 0) return ex.error;
      std::string missing;
      for (const auto& t : names) {
        const size_t dash = t.find('-');
        if (dash == std::string::npos || t.find_first_of("*?") != std::string::npos) continue;
        for (const std::string& end : {t.substr(0, dash), t.substr(dash + 1)}) {
          if (missing.empty() && std::find(all.begin(), all.end(), end) == all.end()) missing = end;
        }
      }
      if (missing.empty()) missing = joinSpace(names);
      return ojson(eval::errorBlock("engine_error", "variable " + missing + " not found",
                                    std::nullopt, std::nullopt, std::nullopt, "r(111)"));
    }
    names = ex.tokens;
  }
  for (auto& t : names) {
    if (std::find(all.begin(), all.end(), t) != all.end()) continue;
    std::vector<std::string> hits;
    for (const auto& v : all)
      if (v.size() > t.size() && v.compare(0, t.size(), t) == 0) hits.push_back(v);
    if (hits.size() == 1) {
      t = hits.front();
    } else if (hits.size() > 1) {
      return ojson(eval::errorBlock("engine_error", t + " ambiguous abbreviation", std::nullopt,
                                    std::nullopt, std::nullopt, "r(111)"));
    }
  }
  return std::nullopt;
}

// BS-031(d)/(e) (BETA campaign C2, silent-if-drop sweep): the STRING gen/
// replace path (strmutate::runStringGen/runStringReplace, routed from the
// `generate`/`replace` arms below when needsStringEval() fires) only ever
// received `ifExpr` — never `parsed.inRange` — so `gen str8 sv = "x" in
// 1/10` silently set ALL 336 rows instead of just the 10 in-range ones
// (live-verified this stint: count if sv=="x" -> 10, count if sv=="" ->
// 326; `replace sv = "y" in 5/8` similarly: 4 changed, 332 unchanged). The
// numeric gen/replace path already folds `in a/b` into the same cond() the
// if-expr builds (BUG-077, egen.cpp's runBysortGen) — this is the
// IDENTICAL MVP grammar (bare INT, INT/INT, f/INT, INT/l, f/l; negative
// bounds rejected) reproduced here as a file-local helper so the STRING
// path (strmutate.cpp, a different TU with its own row-wise `_n`/`_N`
// evaluator — see strmutate.cpp's evalRows) can fold `in` into a combined
// RAW (un-rewritten) Stata-syntax ifExpr string BEFORE calling
// runStringGen/runStringReplace, which each internally rewrite_expr() the
// ifExpr they're given exactly like they already do for a real `if`. Not a
// new shared abstraction (matches this codebase's own established per-TU
// duplication convention for small helpers like this).
struct StringInRangeFold {
  bool ok = false;
  bool negative = false;
  // "" means neither `if` nor `in` was present (a genuinely absent cond).
  std::string combinedIf;
};

StringInRangeFold foldStringInRange(const std::optional<std::string>& ifExpr,
                                    const std::optional<std::string>& inRange) {
  StringInRangeFold res;
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveRange = inRange.has_value() && !inRange->empty();
  if (!haveRange) {
    res.ok = true;
    res.combinedIf = haveIf ? *ifExpr : std::string();
    return res;
  }
  auto parseBound = [](const std::string& raw) -> std::optional<std::string> {
    std::string t = trimWs(raw);
    std::string tl = lower(t);
    if (tl == "f") return std::string("1");
    if (tl == "l") return std::string("_N");
    if (t.empty()) return std::nullopt;
    if (t[0] == '-') return std::nullopt;  // negative — caller checks this case separately
    for (char c : t) {
      if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    return t;
  };
  std::string t = trimWs(*inRange);
  size_t slash = t.find('/');
  std::string p1 = slash == std::string::npos ? t : t.substr(0, slash);
  std::string p2 = slash == std::string::npos ? t : t.substr(slash + 1);
  if ((!p1.empty() && p1[0] == '-') || (!p2.empty() && p2[0] == '-')) {
    res.negative = true;
    return res;
  }
  auto a = parseBound(p1);
  auto b = parseBound(p2);
  if (!a.has_value() || !b.has_value()) {
    return res;  // ok=false, negative=false -> generic "unsupported form"
  }
  std::string rangePred = "_n>=" + *a + " & _n<=" + *b;
  res.combinedIf = haveIf ? ("(" + *ifExpr + ") & (" + rangePred + ")") : rangePred;
  res.ok = true;
  return res;
}

// Turns a failed foldStringInRange() into the same honest-reject shape
// egen.cpp's runBysortGen already uses for the identical grammar, so a
// string-typed `gen`/`replace ... in ...` gets byte-identical wording to
// its numeric sibling.
ojson stringInRangeError(const StringInRangeFold& fold, const std::string& inRangeRaw) {
  if (fold.negative) {
    return ojson(eval::unsupportedOption(
        "in " + inRangeRaw,
        "Negative (from-the-end) `in` bounds are not supported yet. Use a positive 1-based "
        "row range, e.g. `in 1/10`, `in f/10`, `in 5/l`, `in f/l`."));
  }
  return ojson(eval::unsupportedOption(
      "in " + inRangeRaw,
      "This `in` row-range form is not supported for gen/replace. Supported forms: INT, "
      "INT/INT, f/INT, INT/l, f/l (f = first row, l = last row)."));
}

// BS-011 (BETA B-graph-B): coefplot/marginsplot are graph-family results
// but their dispatch branches (below) return immediately after computing
// the block, never reaching the big scatter/histogram/twoway/... block's
// own tail that caches `session.lastGraph` and captures a bare `name()`
// into `session.namedGraphs`. Without that, `graph export`/`graph save`/
// `graph combine` unconditionally failed with "no graph in memory"
// immediately after a coefplot/marginsplot (BS-011 evidence). This is a
// literal mirror of that existing tail (same name-capture regex/logic,
// copied — not a refactor of the scatter/twoway block's own copy, and not
// a new shared abstraction either; unifying all call sites onto one
// consumer is BS-099's Phase E job, out of this stint's scope).
void registerGraphOutput(ojson& result, SessionState& session,
                          const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  if (!(result.is_object() && result.value("type", std::string()) == "graph")) return;
  session.lastGraph = result;
  std::string nameOpt;
  for (const auto& [k, v] : opts) {
    if (k == "name" && !v.isBool) nameOpt = v.strValue;
  }
  if (nameOpt.empty()) return;
  static const std::regex reNameSplit(R"([,\s]+)");
  std::sregex_token_iterator it(nameOpt.begin(), nameOpt.end(), reNameSplit, -1), endIt;
  std::string nm = (it != endIt) ? it->str() : nameOpt;
  if (!nm.empty()) {
    session.namedGraphs[nm] = result;
    result["name"] = nm;
  }
}

// BS-053: `twoway scatter y x || lfit y x` -- Stata's `||` double-pipe
// layer separator, an alternate spelling of the parenthesised
// `twoway (scatter y x) (lfit y x)` form (same layer semantics, just
// without the parens). twoway_parse.cpp's parser only ever recognises
// TOP-LEVEL `(...)` groups as layer boundaries; a bare `||`-separated
// string has none, so it fell into the "no top-level parens" bare
// single-plot branch, which only ever looks at the FIRST plot-type token
// and silently drops everything from the first `||` onward (SCOPE's
// "silently truncates to first plot").
//
// Fix strategy: don't touch twoway_parse.cpp's parenthesised-layer grammar
// at all -- instead, when the raw twoway argument string has NO top-level
// parens but DOES have a top-level `||`, wrap each `||`-delimited segment
// in its own parens (`"scatter y x || lfit y x"` -> `"(scatter y x) (lfit
// y x)"`) and hand the synthesized string to the EXISTING, already-correct
// parenthesised-layer parser unchanged. `||` occurrences inside a quoted
// string (e.g. a `title("a||b")`) or inside a nested `(...)` option value
// are never split, mirroring twoway_parse.cpp's own paren/quote-depth
// scanning convention (scanToplevelParens) so the two stay consistent in
// spirit without sharing code.
std::string expandDoublePipeLayers(const std::string& rawArgs) {
  std::vector<size_t> splitPositions;
  int depth = 0;
  bool inQuote = false;
  size_t n = rawArgs.size();
  for (size_t i = 0; i + 1 < n;) {
    char c = rawArgs[i];
    if (inQuote) {
      if (c == '"') inQuote = false;
      ++i;
      continue;
    }
    if (c == '"') {
      inQuote = true;
      ++i;
      continue;
    }
    if (c == '(') {
      ++depth;
      ++i;
      continue;
    }
    if (c == ')') {
      if (depth > 0) --depth;
      ++i;
      continue;
    }
    if (depth == 0 && c == '|' && rawArgs[i + 1] == '|') {
      splitPositions.push_back(i);
      i += 2;
      continue;
    }
    ++i;
  }
  if (splitPositions.empty()) return rawArgs;

  std::vector<std::string> segments;
  size_t prev = 0;
  for (size_t pos : splitPositions) {
    segments.push_back(rawArgs.substr(prev, pos - prev));
    prev = pos + 2;
  }
  segments.push_back(rawArgs.substr(prev));

  std::string out;
  for (auto& seg : segments) {
    std::string trimmed = trimWs(seg);
    if (trimmed.empty()) continue;
    out += "(" + trimmed + ") ";
  }
  if (!out.empty()) out.pop_back();
  return out;
}

// BUG-176 (P2-PREP): the `tempfile`/`tempname`/`tempvar` intercept body,
// lifted verbatim out of dispatchImpl's step (c)/(d) so the SAME matcher can
// run twice — once on the raw line (the literal path, unchanged) and once on
// the macro-EXPANDED line at step 0b-bis. Returns true when it consumed the
// line, writing the reply into `out`; false leaves `out` untouched and the
// caller falls through exactly as before.
//
// Duplicating the body instead would have been the third instance of this
// codebase's recurring one-sided-edit failure (BUG-158/BUG-161): two copies
// of one matcher drift, and the macro-indirected path silently stops
// agreeing with the literal one. One body, two call sites.
bool tryTempDeclIntercept(const std::string& line, SessionState& session,
                          const nlohmann::ordered_json& echo, nlohmann::ordered_json& out) {
  std::string u4Stripped = trimWs(line);
  size_t u4Sp = u4Stripped.find_first_of(" \t");
  std::string u4First = lower(u4Sp == std::string::npos ? u4Stripped : u4Stripped.substr(0, u4Sp));
  if (u4First != "tempfile" && u4First != "tempname" && u4First != "tempvar") return false;
  std::vector<std::string> names;
  if (u4Sp != std::string::npos) {
    std::istringstream namesIss(u4Stripped.substr(u4Sp));
    std::string nm;
    while (namesIss >> nm) names.push_back(nm);
  }
  nlohmann::ordered_json r = (u4First == "tempfile")   ? handlers::runTempfile(names, session)
                             : (u4First == "tempname") ? handlers::runTempname(names, session)
                                                       : handlers::runTempvar(names, session);
  if (r.is_array() && r.empty()) {
    out = nlohmann::ordered_json::array();
    return true;
  }
  out = nlohmann::ordered_json::array({echo, r});
  return true;
}

// BUG-513: `local NAME : display <display-args>` -- the extended macro function
// that stores what `display` would have printed (`local m : display _N` -> 12,
// tests/eval/golden/aer_w2/w2_smalls.log arm ext_display_N). It was the generic
// BUG-216 refusal, "display not allowed" r(101): honest, but it is how a do-file
// formats a number into a macro (`local b : display %5.3f _b[x]`), so every such
// line stopped its file.
//
// Answered HERE and not in eval::tryExtendedMacroFn because the answer IS
// handleDisplay's -- token resolution, %fmt, _b[]/e()/c()/scalar reads -- and
// that needs the full SessionState the eval layer deliberately does not see
// (same reason substituteCRefs runs at this call site). A second, thinner
// display inside the eval layer is the two-layer drift this file keeps paying
// for. One body, two call sites, like tryTempDeclIntercept above.
//
// The line is macro-expanded first (step 0a sees the RAW line), exactly what
// step 0b would have done to an ordinary `display` line. A display ERROR is
// returned as the block it is, reason and all, and the macro stays UNSET --
// BUG-216/217's rule for a failed colon form.
//
// The abbreviations are the display command's own (`di` .. `display`); only
// the full word is in the oracle.
bool tryExtDisplayCapture(const std::string& lineIn, SessionState& session,
                          nlohmann::ordered_json& out) {
  if (lineIn.find(':') == std::string::npos) return false;  // memchr-class gate
  {
    // Second gate, still no regex: only a `local`/`global` line can be this
    // form, and `by g:` / `bysort g:` prefixes are the colon lines that matter.
    const size_t h = lineIn.find_first_not_of(" \t");
    if (h == std::string::npos) return false;
    const std::string head = lower(lineIn.substr(h, 6));
    if (head.compare(0, 5, "local") != 0 && head != "global") return false;
  }
  static const std::regex reLoose(
      R"(^\s*(?:local|global)\s+\S+?\s*:\s*(?:display|displa|displ|disp|dis|di)(?![A-Za-z0-9_]))",
      std::regex::icase);
  if (!std::regex_search(lineIn, reLoose)) return false;
  std::string line = lineIn;
  if (eval::hasMacroRefs(line)) {
    eval::ExpandResult er = eval::expandMacros(line, session.eval);
    if (er.isError) {
      out = nlohmann::ordered_json::array({er.error});
      return true;
    }
    line = er.line;
  }
  static const std::regex reStrict(
      R"(^\s*(local|global)\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(?:display|displa|displ|disp|dis|di)(?![A-Za-z0-9_])(.*)$)",
      std::regex::icase);
  std::smatch m;
  if (!std::regex_match(line, m, reStrict)) return false;  // not this form after all
  // reStrict has THREE capture groups (kind, name, display args); the display
  // arguments are group 3. Group 4 does not exist and std::smatch answers ""
  // for it, which is how the first build stored an EMPTY macro at rc 0.
  const nlohmann::ordered_json shown = handleDisplay(trimWs(m.str(3)), session);
  if (!shown.is_object() || shown.value("type", std::string()) != "display" ||
      !shown.contains("text") || !shown.at("text").is_string()) {
    out = nlohmann::ordered_json::array({shown});
    return true;
  }
  const std::string text = shown.at("text").get<std::string>();
  if (lower(m.str(1)) == "local") {
    session.eval.locals.set(m.str(2), text);
  } else {
    session.eval.globals.set(m.str(2), text);
  }
  out = nlohmann::ordered_json::array();
  return true;
}

}  // namespace

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
#include "generated/opt_minimums_bug264.inc"  // NOLINT — table body, see gen_table.py
  const auto& kOptMinimums = kOptMinimumsGenerated;

  OptKeyResolution res;
  res.resolved = key;
  auto mins = kOptMinimums.find(cmd);
  if (mins != kOptMinimums.end()) {
    for (const auto& om : mins->second) {
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
      // P0-A, the destructive row. milf-pins/oracle_p.log:23-27 `export
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
      {"export", "nolabel", 7},
      {"export", "novarnames", 10},

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

      // ── P1, INTERNAL SYNONYMS — a different kind of row, and the reason it is
      // honest. A PIET `known` set is a statement about what the HANDLER READS,
      // not about Stata's vocabulary: it legitimately carries aliases this build
      // uses internally. `vcov` is one — real Stata's regress has no such option
      // at all, which is why StataBE answers `vco(robust)` with r(198), "option
      // vco() not allowed" (oracle_p.log:115-119). An internal alias exists so a
      // handler can read a key; it must not also widen what PIET accepts. So it
      // keeps its exact spelling and contributes NO abbreviation surface.
      {"regress", "vcov", 4},
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
  if (kOptMinComplete.count(cmd) != 0) {
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

// M2 scope (ROADMAP "M2 — glue + vertical slice") ... M3/M4/BETA extension
// point comment continues to apply unchanged (see this file's top-of-file
// doc comment) — this function is exactly what used to be named
// `dispatch()` before BS-006 (BUG-028's real log write) split it in two.
// The public `dispatch()` (defined at the very bottom of this file, after
// this function's closing brace) is now a thin log-teeing wrapper; this is
// still the ~2470-line M2/M3/M4/BETA routing body, completely unchanged
// otherwise. Every recursive self-call inside this function's own body
// still literally says `dispatch(...)` (not `dispatchImpl(...)`) — quite
// deliberately, so nested calls (capture/quietly/noisily/by/bysort prefix-
// stripping; the eststo/version bare-command forwards; and, via the
// `dispatch` function pointer handed to doexec::executeDoSource/executeDo
// and program::runProgram below, every line of a replayed do-file/loop/
// program body) all flow through the SAME wrapper as any other call —
// see dispatch()'s own doc comment for why that matters.
// ---------------------------------------------------------------------------
// BUG-428 / BUG-432 — THE ROW-PREDICATE SUBSTITUTION POINT.
//
// `local a = M[1,1]` reads a matrix element and `display s` reads a stored
// scalar, because the macro/expression layer substitutes both before evaluating.
// The ROW layer — `assert`, `count if`, `keep if`, the `if` of a `gen`, and
// gen's own expression — never got that pass, so `assert Q[1,1] == 1` came back
// "unknown VARIABLE Q" for a matrix defined on the line above, and
// `gen x = y * scalar(s)` was rc 1 while `display s` printed 3. Two ids, one
// missing pass ([[two-evaluator-layers-predicate-split]]).
//
// Both halves are no-ops on text that names no defined matrix and no defined
// scalar, which keeps the blast radius to expressions that fail today: nothing
// that already works can start substituting.
//
// A VARIABLE always wins over a same-named scalar — Stata's rule, and the reason
// this cannot simply loop `substituteBareName` over the scalar store the way
// evalArith does: evalArith has no frame in hand, and here we do.
// BUG-456 (2026-09-08) — the row plane's CONSTANT STRING COMPARE fold.
//
// The engine's data plane has no string type: its `--keepif` accepts
// `factorvar ==/!= "literal"` and nothing else (src/data/frame.cpp), and
// CiParser bails on any string operand. Stata's row plane takes a constant
// string compare everywhere -- `assert e(cmd)=="regress"` rc 0, `gen z =
// (e(cmd)=="regress")` mean 1, `count if e(cmd)=="nope"` 0 (capture.log
// A1/G1/G10) -- and after the resolver above has turned e(cmd) into `"regress"`
// the claim is a constant. So the constant is decided HERE, before the engine
// sees it: `"lit" op "lit"` folds to 1/0 for ==, !=, ~=, <, <=, >, >= (Stata
// orders strings bytewise, refute3 Q1-Q3), and `"lit" op <number|.>` (either
// order) is r(109) "type mismatch" (A9/A10/D10/N1/M1-M7), never a silent 0.
// Anything with a variable, a function call or a non-constant operand on
// either side is left exactly as it was -- CiParser / the engine own it.
//
// Quote-aware by construction: the operator is found with eval::searchUnquoted
// (tokenizer.h), never a quote-blind regex ([[piet-quote-aware-qualifier]]),
// and each operand is classified from the characters beside the operator.
// NAMED GAP (2026-09-09, review finding 11): the operand scanner below bounds a
// literal with a bare rfind('"')/find('"') and does NOT understand the `\"`
// escaping resolveERef (macros.cpp) and resolveEStringLiteral
// (display_hardening.cpp) emit. Unreachable today -- measured: PIET refuses
// compound double quotes, so no e-string can carry a `"` -- so this is a pin
// owed whenever that gap closes, not a live defect.
// String FUNCTIONS on a constant (`inlist(e(cmd),"a")`, `strpos(e(cmdline),
// "x")`) are NOT folded here; they take the strmutate detour when it can
// evaluate them and are refused honestly by it when it cannot -- named scope,
// not claimed by this pass.
namespace {

bool rowConstNumberToken(const std::string& t) {
  static const std::regex reNum(R"(^([0-9]+\.?[0-9]*|\.[0-9]+|\.)([eE][+-]?[0-9]+)?$)");
  return !t.empty() && std::regex_match(t, reNum);
}

// Operand kinds: 0 = not a constant we fold, 1 = "literal", 2 = number/missing.
struct RowConstOperand {
  int kind = 0;
  size_t begin = 0;  // [begin, end) of the operand text incl. quotes
  size_t end = 0;
  std::string text;  // literal contents (kind 1) or the number token (kind 2)
};

bool rowOperandBoundaryLeft(char c) {  // char just BEFORE a constant operand
  return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '"' || c == ')' ||
           c == ']' || c == '.');
}
bool rowOperandBoundaryRight(char c) {  // char just AFTER a constant operand
  return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '"' || c == '(' ||
           c == '[' || c == '.');
}

RowConstOperand rowLeftOperand(const std::string& e, size_t opPos) {
  RowConstOperand o;
  size_t l = opPos;
  while (l > 0 && std::isspace(static_cast<unsigned char>(e[l - 1]))) --l;
  if (l == 0) return o;
  if (e[l - 1] == '"') {
    if (l < 2) return o;
    const size_t q = e.rfind('"', l - 2);
    if (q == std::string::npos) return o;
    o.kind = 1;
    o.begin = q;
    o.end = l;
    o.text = e.substr(q + 1, l - 2 - q);
  } else {
    size_t k = l;
    auto isTokChar = [](char c) {
      return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
    };
    while (k > 0 && isTokChar(e[k - 1])) --k;
    // exponent sign: `1e+307` scanned back to `307`; extend over the `e+`.
    if (k >= 2 && (e[k - 1] == '+' || e[k - 1] == '-') && (e[k - 2] == 'e' || e[k - 2] == 'E')) {
      k -= 2;
      while (k > 0 && isTokChar(e[k - 1])) --k;
    }
    const std::string tok = e.substr(k, l - k);
    if (!rowConstNumberToken(tok)) return o;
    o.kind = 2;
    o.begin = k;
    o.end = l;
    o.text = tok;
  }
  if (o.begin > 0 && !rowOperandBoundaryLeft(e[o.begin - 1])) o.kind = 0;
  return o;
}

RowConstOperand rowRightOperand(const std::string& e, size_t opEnd) {
  RowConstOperand o;
  size_t r = opEnd;
  while (r < e.size() && std::isspace(static_cast<unsigned char>(e[r]))) ++r;
  if (r >= e.size()) return o;
  if (e[r] == '"') {
    const size_t q = e.find('"', r + 1);
    if (q == std::string::npos) return o;
    o.kind = 1;
    o.begin = r;
    o.end = q + 1;
    o.text = e.substr(r + 1, q - r - 1);
  } else {
    static const std::regex reNumHead(R"(^([0-9]+\.?[0-9]*|\.[0-9]+|\.)([eE][+-]?[0-9]+)?)");
    std::smatch m;
    const std::string tail = e.substr(r);
    if (!std::regex_search(tail, m, reNumHead)) return o;
    o.kind = 2;
    o.begin = r;
    o.end = r + static_cast<size_t>(m.length(0));
    o.text = m.str(0);
  }
  if (o.end < e.size() && !rowOperandBoundaryRight(e[o.end])) o.kind = 0;
  return o;
}

// Folds in place. Returns false (and fills *mismatch) on a string-vs-number
// compare -- the caller turns that into the r(109) block.
bool foldRowConstStringCompares(std::string& e, std::string* mismatch) {
  if (e.find('"') == std::string::npos) return true;  // nothing constant-string to fold
  static const std::regex reCmp(R"(==|!=|~=|<=|>=|<|>)");
  size_t from = 0;
  while (from < e.size()) {
    std::smatch m;
    const std::string tail = e.substr(from);
    if (!eval::searchUnquoted(tail, reCmp, m)) break;
    const size_t opPos = from + static_cast<size_t>(m.position(0));
    const std::string op = m.str(0);
    const size_t opEnd = opPos + op.size();
    const RowConstOperand L = rowLeftOperand(e, opPos);
    const RowConstOperand R = rowRightOperand(e, opEnd);
    if (L.kind == 0 || R.kind == 0) {  // a variable / call / non-constant: not ours
      from = opEnd;
      continue;
    }
    if (L.kind != R.kind) {
      *mismatch = e.substr(L.begin, R.end - L.begin);
      return false;
    }
    if (L.kind == 2) {  // number op number: the engine's arithmetic, untouched
      from = opEnd;
      continue;
    }
    const int c = L.text.compare(R.text);
    bool v = false;
    if (op == "==") v = (c == 0);
    else if (op == "!=" || op == "~=") v = (c != 0);
    else if (op == "<") v = (c < 0);
    else if (op == "<=") v = (c <= 0);
    else if (op == ">") v = (c > 0);
    else v = (c >= 0);
    e.replace(L.begin, R.end - L.begin, v ? "1" : "0");
    from = L.begin + 1;
  }
  return true;
}

// ---------------------------------------------------------------------------
// B4-W3b: `tabulate VAR, generate(stub)`.
//
// Returns a null json on success, or the error block to emit. The dummies are
// built with piet::handlers::runGen — the SAME generate path the user's own
// `gen` uses — rather than a private column writer, so the storage plane, the
// resident-frame opt-in and the identifier validation are the ones already in
// service and cannot drift from them.
nlohmann::ordered_json runTabulateGenerate(const std::string& stub,
                                           const std::vector<std::string>& varlist,
                                           const std::vector<std::string>& rawRows,
                                           SessionState& session) {
  using ojson = nlohmann::ordered_json;
  if (varlist.size() != 1) {
    // Stata's `generate()` is a ONE-WAY tabulate option; the two-way syntax
    // does not carry it. Refusing by name rather than inventing a meaning for
    // the cross-tabulated case.
    return ojson(eval::errorBlock(
        "unsupported_option",
        "tabulate: generate() is a one-way option — it makes one indicator per level of a "
        "single variable, and a two-way table has no single level list to make them from."));
  }
  if (!strutil::isValidStataIdentifier(stub)) {
    return ojson(eval::errorBlock(
        "syntax_error",
        "tabulate: invalid variable name `" + stub + "' — " + strutil::stataIdentifierRuleText()));
  }
  if (rawRows.empty()) {
    return ojson(eval::errorBlock("bad_input",
                                  "tabulate: generate() has no levels to make indicators from."));
  }
  // Stata's own guard, and it fires BEFORE any column is written: a collision
  // half-way through would leave the dataset holding stub1..stub3 of a set of
  // five, which is worse than the error.
  Datastore* ds = session.datastore();
  for (size_t i = 0; i < rawRows.size(); ++i) {
    const std::string nm = stub + std::to_string(i + 1);
    bool exists = false;
    if (const auto* card = session.shadowVarlistIfCurrent()) {
      for (const auto& info : *card)
        if (info.name == nm) exists = true;
    } else if (ds != nullptr) {
      exists = ds->has(nm);
    }
    if (exists) {
      return ojson(eval::errorBlock("bad_input", "variable " + nm + " already defined"));
    }
  }

  const std::string& srcVar = varlist[0];
  for (size_t i = 0; i < rawRows.size(); ++i) {
    const std::string nm = stub + std::to_string(i + 1);
    const std::string& lvl = rawRows[i];
    // A level key that parses as a number compares numerically; anything else
    // is a string level and compares as one. runTabulate renders numeric
    // levels through the same formatter either way, so this is a property of
    // the KEY, not a guess about the column.
    bool numericLevel = false;
    try {
      size_t pos = 0;
      (void)std::stod(lvl, &pos);
      numericLevel = (pos == lvl.size());
    } catch (...) {
      numericLevel = false;
    }
    const std::string cmp =
        numericLevel ? (srcVar + "==" + lvl) : (srcVar + "==\"" + lvl + "\"");
    // The missing rule (oracle milfpins_oracle_g.log:170-176) lives here.
    const std::string expr =
        nm + " = cond(missing(" + srcVar + "), ., " + cmp + ")";
    ojson r = piet::handlers::runGen(expr, std::nullopt, session);
    if (r.is_object() && r.value("type", std::string()) == "error") return r;
    // Stata makes these `byte`, formats them %8.0g, and labels them with the
    // literal spelling `rep78== 1.0000` (milfpins_shapes.log:31-36 — the
    // level rendered `%7.4f` — "1.0000" is 6 characters right-aligned into a
    // width-7 field, which is the single leading space the oracle shows). The
    // label is carried so `describe` and
    // `local : variable label` agree with it; the width rule is Stata's own
    // and is reproduced from the measured example, not invented.
    session.varMeta[nm].stataType = "byte";
    std::string shown = lvl;
    if (numericLevel) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.4f", std::stod(lvl));
      shown = buf;
    }
    session.setVarLabel(nm, srcVar + "==" + std::string(shown.size() < 7 ? 7 - shown.size() : 0, ' ') +
                                shown);
  }
  return ojson();  // null => no error
}

}  // namespace

// The stored-SCALAR substitution the row plane has always done, lifted out of
// substituteRowExprRefs unchanged (BUG-456 follow-up, 2026-09-09) so gen's
// pre-resolve can run the same substitution on a COPY, purely to DECIDE the
// type of an expression it must not otherwise rewrite. A VARIABLE always wins
// over a same-named scalar -- Stata's rule, and the reason this asks the read
// card before substituting.
static std::string substituteStoredScalars(const std::string& in, SessionState& session) {
  std::string e = in;
  if (!session.eval.scalars.items().empty()) {
    const std::vector<VarInfo>* card = session.residentReadCard();
    for (const auto& [sn, sv] : session.eval.scalars.items()) {
      if (e.find(sn) == std::string::npos) continue;
      bool isVar = false;
      if (card != nullptr) {
        for (const auto& vi : *card)
          if (vi.name == sn) { isVar = true; break; }
      } else if (session.currentHbin().has_value()) {
        Datastore* ds0 = session.datastore();
        isVar = ds0 != nullptr && ds0->has(sn);
      }
      if (!isVar) e = eval::substituteBareName(e, sn, sv);
    }
  }
  return e;
}

// `errOut` (BUG-456): when non-null, receives the error block for the two
// shapes this pass can now refuse -- an unknown coefficient name (r(111)) and a
// string-vs-number constant compare (r(109)). A null errOut keeps the text as
// substituted so far; the engine then refuses it in its own words.
std::string substituteRowExprRefs(const std::string& in, SessionState& session,
                                  std::optional<ojson>* errOut = nullptr) {
  std::string e = in;
  if (e.find('[') != std::string::npos) {
    e = eval::substituteMatrixRefs(e, session.eval);
  }
  // BUG-456: this used to run ONLY when the text contained `scalar(` -- so the
  // `_b[]`/`_se[]`/e() halves of the resolver never ran on a row predicate
  // without one, and `assert _se[mpg]>0` reached the engine as a variable
  // subscript ("unknown variable _se") while `assert _se[mpg]*scalar(one)>0`
  // passed (capture A2 vs A13, the proof). The trigger is now the union of the
  // resolver's own prefilters. BUG-432's model gate is kept for the
  // coefficient half: with no model in memory `_b[`/`_se[` are left as text so
  // the engine refuses them honestly (Stata: r(111) "_b not allowed when e(b)
  // is not present"), instead of the resolver's silent `.`.
  const bool hasBRef = e.find("_b[") != std::string::npos || e.find("_se[") != std::string::npos;
  const bool wantsEst =
      hasBRef || e.find("scalar(") != std::string::npos || eval::hasBareEParen(e);
  if (wantsEst && (!hasBRef || session.eval.lastModelResult.has_value())) {
    std::string bErr;
    e = eval::resolveEstScalarRefs(e, session.eval, &bErr);
    if (!bErr.empty()) {
      if (errOut != nullptr) {
        // BUG-456 follow-up: r(111) or r(109) -- the resolver says which.
        const std::string code = eval::splitErrCode(&bErr);
        *errOut = ojson(eval::errorBlock("engine_error", bErr, std::nullopt, std::nullopt,
                                         std::nullopt, code));
      }
      return e;
    }
  }
  e = substituteStoredScalars(e, session);
  // BUG-456 follow-up (2026-09-09): a NUMBER in a STRING-typed argument slot is
  // r(109) here too, and this test must run whether or not the resolver above
  // did -- the macro layer substitutes a PRESENT e-scalar upstream, so
  // `count if strpos(e(N),"2")` reaches this point as `strpos(20.0,"2")` with
  // `wantsEst` false. A stored scalar (`scalar k = 5` -> `strpos(5,"x")`) is
  // the same shape and is caught by the same pass, after its substitution.
  if (errOut != nullptr) {
    std::string badCall;
    if (eval::stringFnNumericArg(e, &badCall)) {
      std::string msg = eval::stringSlotMismatchError(badCall);
      const std::string code = eval::splitErrCode(&msg);
      *errOut = ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                       std::nullopt, code));
      return e;
    }
  }
  // BUG-456: decide the constant string compares the engine cannot (see the
  // fold's own comment above). A string-vs-number compare is r(109).
  {
    std::string mismatch;
    if (!foldRowConstStringCompares(e, &mismatch) && errOut != nullptr) {
      *errOut = ojson(eval::errorBlock(
          "engine_error",
          "type mismatch: `" + mismatch + "` compares a string with a number",
          std::nullopt, std::nullopt, std::nullopt, "r(109)"));
    }
  }
  return e;
}

// BUG-456: gen/replace's own expression, resolved BEFORE strmutate::needsStringEval
// decides the route. `_b[]`/`_se[]`/e()/scalar() go to literals (model-gated
// for the coefficient half, BUG-432), then the constant string compares fold
// (`(e(cmd)=="regress")` -> `(1)`). `rhs` is re-derived so the detour test that
// follows sees the resolved text. Returns the error block for r(111)/r(109).
static std::optional<ojson> preResolveGenExpr(std::string& expr, std::string& rhs,
                                              SessionState& session) {
  const bool hasBRef =
      expr.find("_b[") != std::string::npos || expr.find("_se[") != std::string::npos;
  const bool wantsEst =
      hasBRef || expr.find("scalar(") != std::string::npos || eval::hasBareEParen(expr);
  if (wantsEst && (!hasBRef || session.eval.lastModelResult.has_value())) {
    std::string bErr;
    expr = eval::resolveEstScalarRefs(expr, session.eval, &bErr);
    if (!bErr.empty()) {
      // BUG-456 follow-up: r(111) or r(109) -- the resolver says which.
      const std::string code = eval::splitErrCode(&bErr);
      return ojson(eval::errorBlock("engine_error", bErr, std::nullopt, std::nullopt,
                                    std::nullopt, code));
    }
  }
  // BUG-456 follow-up (review finding 3, 2026-09-09): gen's TYPE decision is
  // made on a COPY with the stored scalars substituted, because the real
  // substitution happens further down (after the strmutate detour has already
  // claimed anything holding a string literal) and gen must not be re-routed by
  // a scalar. Measured: `scalar one = 1` then `gen z = (e(cmd)==one)` was rc 1
  // "generate: expression evaluation failed at obs 1" with NO r() code, where
  // the identical `count if e(cmd)==one` reported r(109) -- the same claim,
  // two codes. Only the ERROR comes from this copy; `expr` is untouched.
  {
    std::string probe = substituteStoredScalars(expr, session);
    std::string badCall, probeMismatch;
    if (eval::stringFnNumericArg(probe, &badCall)) {
      std::string msg = eval::stringSlotMismatchError(badCall);
      const std::string code = eval::splitErrCode(&msg);
      return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                    std::nullopt, code));
    }
    if (!foldRowConstStringCompares(probe, &probeMismatch)) {
      return ojson(eval::errorBlock(
          "engine_error",
          "type mismatch: `" + probeMismatch + "` compares a string with a number",
          std::nullopt, std::nullopt, std::nullopt, "r(109)"));
    }
  }
  {  // BUG-456 follow-up: the string-slot type test, ungated (see the row plane).
    std::string badCall;
    if (eval::stringFnNumericArg(expr, &badCall)) {
      std::string msg = eval::stringSlotMismatchError(badCall);
      const std::string code = eval::splitErrCode(&msg);
      return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                    std::nullopt, code));
    }
  }
  std::string mismatch;
  if (!foldRowConstStringCompares(expr, &mismatch)) {
    return ojson(eval::errorBlock("engine_error",
                                  "type mismatch: `" + mismatch + "` compares a string with a number",
                                  std::nullopt, std::nullopt, std::nullopt, "r(109)"));
  }
  const size_t eq = expr.find('=');
  rhs = (eq == std::string::npos) ? std::string() : trimWs(expr.substr(eq + 1));
  return std::nullopt;
}

namespace {

// `title("A title")` arrives with its quote characters still on the value (the
// tokenizer keeps them, exactly as the file-part handling has to strip them
// too). One matching pair, so a title that is itself quoted survives.
std::string dequoteEstTitle(std::string t) {
  if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') ||
                        (t.front() == '\'' && t.back() == '\''))) {
    t = t.substr(1, t.size() - 2);
  }
  return t;
}

// esample T0 (BUG-636): the post-estimation verbs that re-fit the ACTIVE model
// from its data file -- test/testparm/lincom/nlcom (runPostEstimationM4's
// lastModelBaseArgv fallback, their only path) -- replay `fitDataPath` else
// `dataPath` as `--data`. In-session those files are pinned (the live model by
// reclaimSupersededScratch exclusion (7), a stored one by pinStoredModel), but
// `estimates use` reads back the path the .ster recorded at `estimates save`,
// and nothing pins that: a later fit plus a few mutations in the SAME session
// reclaim it, and a .ster saved by another session names that session's
// scratch -- as does the same model after `estimates store` + `estimates
// restore`. The verbs then failed with
// `[data_io] cannot open .../piet_native_XXXX/...hbin` at rc 1. predict already
// refuses this shape by name (estimation_m4c.cpp, the BUG-518 remainder); this
// is its sibling for the verbs whose ONLY path is that replay. NOT margins:
// `margins, dydx()` after a linear fit answers from the coefficient table
// (buildLinearAmeRawM4) without the file, so a guard here would refuse a
// working form; its replay branches need the check inside runMargins. It fires only
// when the replay can NOT work: no engine handle to answer from, the file is
// not the live head (a dirty resident head is legitimately not on disk) and not
// a virtual save, and nothing exists at the path.
std::optional<ojson> refuseIfFitDataGone(const std::string& verb, SessionState& session) {
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value() || !lmOpt->estHandle.empty()) return std::nullopt;
  const LastModel& lm = *lmOpt;
  const std::string& fitFile = lm.fitDataPath.empty() ? lm.dataPath : lm.fitDataPath;
  if (fitFile.empty() || session.isVirtualSave(fitFile)) return std::nullopt;
  if (session.currentHbin().has_value() && *session.currentHbin() == fitFile) return std::nullopt;
  std::error_code ec;
  if (std::filesystem::exists(cecon::to_native_path(fitFile), ec)) return std::nullopt;
  return ojson(eval::engineError(
      verb + ": the data `" + (lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd) +
          "` was fitted on are no longer available to re-fit from in this version of PIET "
          "(the working frame it was fitted on has been released -- e.g. results loaded back "
          "by `estimates use`). Re-run the estimation command, then " +
          verb + ".",
      "unsupported_option"));
}

// W2 eststoprefix (BUG-520). `eststo`'s own options, read in ONE place so the
// prefix arm (Pre-0b) and the flat arm cannot drift: the prefix arm never
// reaches the tokenizer with its head, so a net that lived only in the flat arm
// would leave `eststo m1, bogus: regress y x` accepted-and-ignored.
struct EststoStoreOpts {
  std::optional<std::string> title;
  std::string autoPrefix = "est";  // estout's default; w2_eststo_prefix.log A2/A11
};

std::optional<ojson> readEststoStoreOpts(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts, EststoStoreOpts* out) {
  // NOT kVocabEststoStore.known: that slot is the UNION with `estimates`' own
  // vocabulary (b/se/stats/replace), none of which `eststo` takes.
  static const std::unordered_set<std::string> kRead = {"title", "prefix"};
  if (auto err = eval::rejectUnknownOptions(opts, kRead, kVocabEststoStore.unimplemented)) {
    return ojson(*err);
  }
  for (const auto& [k, v] : opts) {
    if (k == "title" && !v.isBool) out->title = dequoteEstTitle(v.strValue);
    if (k == "prefix") {
      // eststo.ado declares Prefix(name): one Stata name, nothing else. A bare
      // `prefix` or a non-name would otherwise mint a key no `esttab` can spell.
      static const std::regex reName(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
      const std::string p = v.isBool ? std::string() : trimWs(v.strValue);
      if (!std::regex_match(p, reName)) {
        return ojson(eval::engineError(
            "eststo: option prefix() takes one name, e.g. `eststo, prefix(m): regress y x`",
            "syntax_error"));
      }
      out->autoPrefix = p;
    }
  }
  return std::nullopt;
}

// The colon that ends an `eststo [name] [, options] :` head: the first one that
// is outside a quoted span (w2_eststo_prefix.log A6 -- `eststo t1, title("a: b"):
// regress y w` is rc 0 and stores t1) AND outside a `...' macro reference, so an
// inline extended function in the name (`eststo m`: word 1 of `l'': ...`) does
// not split the line inside the macro. npos when there is none -- which is what
// keeps the flat `eststo m1, title("a: b")` flat. A colon BEHIND a trailing
// ` // comment` is not one either: tokenize() strips that comment (`\s+//.*$`)
// for a line that reaches it raw, so `eststo m1 // note: keep` is the flat store
// it has always been, not a prefix with the head `m1 // note`.
size_t findEststoPrefixColon(const std::string& s) {
  const std::vector<bool> quoted = eval::quotedSpanMask(s);
  int tick = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (quoted[i]) continue;
    const char ch = s[i];
    if (ch == '/' && tick == 0 && i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t') &&
        i + 1 < s.size() && s[i + 1] == '/') {
      return std::string::npos;
    }
    if (ch == '`') {
      ++tick;
    } else if (ch == '\'' && tick > 0) {
      --tick;
    } else if (ch == ':' && tick == 0) {
      return i;
    }
  }
  return std::string::npos;
}

}  // namespace


// BUG-525 (W2 labelglued; oracle w2_cmdtriage.log arms di_glued, corr_glued,
// levelsof_glued, erase_glued): Stata ends the command word at the first
// character that cannot be part of a name, so `di("glued")`, `corr(a c)`,
// `levelsof(g), local(lv)` and `erase"ct_tmp.dta"` all run (rc 0 each; the
// glued corr's r(rho) equals the spaced control's). PIET cut the word at
// whitespace, so each was looked up as a command literally named
// `di("glued")` -> r(199). Normalising the RAW line once, at the top of
// dispatch, is what makes every downstream reader agree: the prefix
// strippers, the tokenizer, and the handlers that re-parse parsed.raw against
// their own `^cmd\s+` anchors (runErase, handleDisplay, runLabelDefine ...).
// Only the leading name is touched, and only when `(` or `"` is glued to it;
// a line that does not begin with a name (`*`, `//`, a macro reference, a
// quote, a digit) comes back byte-identically. The echo block is built from
// the line as typed, before this runs, so the log shows what the user wrote.
std::string splitGluedHead(const std::string& line) {
  size_t b = 0;
  while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
  if (b >= line.size()) return line;
  if (std::isalpha(static_cast<unsigned char>(line[b])) == 0 && line[b] != '_') return line;
  size_t e = b + 1;
  while (e < line.size() &&
         (std::isalnum(static_cast<unsigned char>(line[e])) != 0 || line[e] == '_')) {
    ++e;
  }
  if (e >= line.size() || (line[e] != '(' && line[e] != '"')) return line;
  return line.substr(0, e) + " " + line.substr(e);
}

// BUG-524 (W2 labelglued; oracle w2_cmdtriage.log section B): Stata's `label`
// subcommands abbreviate down to a documented minimum each, and BELOW that
// minimum the line is r(198) "invalid syntax" -- not an unknown subcommand.
// Measured (each full spelling is its abbreviation's control):
//   label define / label def / la de        rc 0    (label_define_full, label_def, la_de)
//   label d                                 r(198)  (label_d_too_short)
//   label values / label val / label value  rc 0    (label_values_full, label_val, label_value)
//   label va                                r(198)  (label_va_too_short; f keeps yn3)
// The remaining floors are the syntax block of `help label`
// (ado/base/l/label.sthlp: `{opt da:ta}` `{opt var:iable}` `{opt di:r}`
// `{opt l:ist}` `{opt lang:uage}`; copy, drop, rename and save carry no colon
// and do not abbreviate) -- the published definition, not an oracle arm, and
// said so here. The command word's own floor (`la`) is the registry's row.
// PIET compared the token to the full spelling only, so about 1,070 corpus
// statements (`label val` 519, `label def` 453, `label de` 79 ...) were
// r(199) hard stops.
struct LabelSubFloor {
  const char* full;
  size_t minLen;
};
constexpr LabelSubFloor kLabelSubFloors[] = {
    {"data", 2}, {"variable", 3}, {"define", 2}, {"values", 3},   {"dir", 2},      {"list", 1},
    {"copy", 4}, {"drop", 4},     {"rename", 6}, {"save", 4},     {"language", 4},
};
enum class LabelSubMatch { kResolved, kTooShort, kUnknown };
// `sub` is the typed token, lowercased. The floors are chosen so that at its
// minimum every subcommand's prefix is unique (`de`/`di`/`da` at 2, `l` at 1 --
// `language` needs `lang`), so the first floor-satisfying hit is the only one.
LabelSubMatch resolveLabelSub(const std::string& sub, std::string* canonical) {
  bool sawPrefix = false;
  for (const auto& f : kLabelSubFloors) {
    const std::string full(f.full);
    if (sub.empty() || sub.size() > full.size() || full.compare(0, sub.size(), sub) != 0) continue;
    sawPrefix = true;
    if (sub.size() >= f.minLen) {
      *canonical = full;
      return LabelSubMatch::kResolved;
    }
  }
  return sawPrefix ? LabelSubMatch::kTooShort : LabelSubMatch::kUnknown;
}

// BUG-524: rawWithCanonicalCmd's idea one token further in. runLabelDefine
// anchors on `^label\s+define\s+` in the raw line, so a subcommand typed as an
// abbreviation is rewritten to its full spelling there (after the command word
// has been). Whole-token match only; anything unexpected returns the line
// unchanged, exactly as the command-word helper does.
std::string rawWithCanonicalLabelSub(const std::string& raw, const std::string& typedSub,
                                     const std::string& canonicalSub) {
  if (typedSub == canonicalSub || typedSub.empty()) return raw;
  size_t b = raw.find_first_not_of(" \t");
  if (b == std::string::npos) return raw;
  size_t e1 = raw.find_first_of(" \t", b);
  if (e1 == std::string::npos) return raw;
  size_t s = raw.find_first_not_of(" \t", e1);
  if (s == std::string::npos) return raw;
  size_t e = raw.find_first_of(" \t", s);
  if (e == std::string::npos) e = raw.size();
  if (lower(raw.substr(s, e - s)) != typedSub) return raw;
  return raw.substr(0, s) + canonicalSub + raw.substr(e);
}

nlohmann::ordered_json dispatchImpl(const std::string& rawCmdTyped, SessionState& session) {
  // BUG-525: the line as TYPED feeds the echo; everything else in this
  // function reads the glued-head-normalised copy (splitGluedHead above).
  const std::string rawCmd = splitGluedHead(rawCmdTyped);
  // C4-L1 window-trace attribution (report-only): remember this command's verb
  // token so an EMH window closure can name its closer. Nested dispatches
  // (capture/quietly strips, do-file replay) re-enter here and overwrite, so
  // the innermost — semantically executing — verb wins. No-op env-off.
  dprof::noteLine();
  ojson echo;
  ojson blocks;
  {
    dprof::Scope __z(dprof::kEntry);
    session.noteC4TraceVerb(rawCmd);
    echo = echoBlock(rawCmdTyped);
    blocks = ojson::array();
    blocks.push_back(echo);
  }

  // ------------------------------------------------------------------
  // Pre-(-1). Loop/if block detection for the console path (M3a, mirrors
  // dispatcher.py's step Pre-(-1) — the tokenizer rejects bare loop
  // keywords, so this must run before tokenize()). Only fires when the raw
  // line ITSELF carries a brace block (single-line `foreach v in ... { ... }`
  // form) — do-file replay routes through doexec::executeDoSource directly,
  // never through here.
  {
    dprof::Scope __z(dprof::kLoopIf);
    std::string strippedForLoop = trimWs(rawCmd);
    if (loops::opensBraceBlock(strippedForLoop)) {
      doexec::ExecuteResult result = doexec::executeDoSource(strippedForLoop, session, dispatch);
      ojson out = ojson::array();
      out.push_back(echo);
      for (const auto& b : result.blocks) out.push_back(b);
      return out;
    }
    // P4 wave: Stata's BRACELESS one-line branch, `if COND command ...`. The
    // braced form has always worked; without this, the braceless one reached
    // the command table as a command literally named `if` and was rejected as
    // unsupported. Handled here rather than in doexec's brace aggregator
    // because both the console and do-file paths funnel through dispatch(),
    // and the condition/body semantics are runIf's already — this only has to
    // find the split point and hand it a one-line body. `else` on its own line
    // is NOT part of this form in Stata and is deliberately not synthesized.
    if (strippedForLoop.size() > 3 && lower(strippedForLoop.substr(0, 3)) == "if " &&
        strippedForLoop.find('{') == std::string::npos) {
      // Stata ends the condition at the first token that starts a command. The
      // condition is an expression, so it cannot contain a bare space-separated
      // word that is not part of an operator — scan for the first top-level
      // (unquoted, unparenthesised) space that follows a complete comparison.
      // In practice the corpus form is `if EXPR <cmd> ...`; splitting on the
      // last top-level relational operand keeps `if !_rc exit 1` and
      // `if `x' != 0 exit `x'` both correct.
      const std::string rest = trimWs(strippedForLoop.substr(3));
      std::string cond, body;
      if (loops::splitOneLineIf(rest, cond, body) && !body.empty()) {
        ojson out = ojson::array();
        out.push_back(echo);
        ojson r = loops::runOneLineIf(cond, body, session, dispatch);
        if (r.is_array()) { for (const auto& b : r) out.push_back(b); }
        else out.push_back(r);
        return out;
      }
    }
    // BUG-547: the legacy `for` command (for.ado -- foreach/forvalues are its
    // modern replacements, but `for` is still valid Stata and 34 corpus lines
    // in 5 papers use it). Handled here, next to the one-line-if intercept
    // above, for the same reason: the tokenizer rejects a bare `for` as an
    // unrecognized command word, so this must run before tokenize(). A
    // `capture noisily for ...`/`quietly for ...` prefix already works
    // without any special-casing here: Pre-0 below strips the prefix and
    // RE-ENTERS dispatchImpl with the bare `for ...` remainder, which hits
    // this same branch on the recursive call.
    if (loops::isLegacyForLine(strippedForLoop)) {
      ojson out = ojson::array();
      out.push_back(echo);
      ojson r = loops::runLegacyFor(strippedForLoop, session, dispatch);
      if (r.is_array()) { for (const auto& b : r) out.push_back(b); }
      else out.push_back(r);
      return out;
    }
  }

  // ------------------------------------------------------------------
  // Pre-(-0.5). `do FILE` / `run FILE` console command (M3a port of
  // dispatcher.py's same-named step). Reads a .do file from disk and
  // replays it through the SAME executor as a future /api/do route.
  {
    dprof::Scope __z(dprof::kDoRun);
    std::string strippedForLoop = trimWs(rawCmd);
    size_t sp = strippedForLoop.find_first_of(" \t");
    std::string firstTok = sp == std::string::npos ? strippedForLoop : strippedForLoop.substr(0, sp);
    std::string firstTokLower = lower(firstTok);
    // W2 doinclude (BUG-528): `include FILE` is the third spelling of this arm.
    // It runs the file through the same executor but SHARES the caller's locals
    // both ways (no private scope, no positional args) -- w2_include_scope.log
    // include_basic / include_nested / include_in_loop / include_in_program.
    const bool isInclude = firstTokLower == "include";
    if (isInclude && sp == std::string::npos) {
      // Bare `include`: a filename is required. Not oracle-covered; answered as
      // invalid syntax rather than falling through to "command not supported".
      ojson out = ojson::array();
      out.push_back(echo);
      out.push_back(ojson(eval::errorBlock("syntax_error", "include: a filename is required",
                                           std::string("include"))));
      return out;
    }
    if ((firstTokLower == "do" || firstTokLower == "run" || isInclude) &&
        sp != std::string::npos) {
      // BUG-089: split the remainder into a filename + positional args instead
      // of treating the whole line as one path. The filename is the first
      // quote-aware token (bind strips a surrounding quote layer so a quoted
      // path with spaces still opens); each further token seeds `1'..`N' via
      // executeDo's args vector — the same mechanism the JSON /api/do bridge uses.
      //
      // BS-018: `do FILE [args] [, nostop]` — split off a trailing options
      // tail at the first UNQUOTED comma (same boundary every other Stata
      // command uses, eval::findUnquotedComma) BEFORE any of the above
      // filename/positional-arg tokenizing. Previously there was no comma
      // handling here at all: `do file.do, nostop` let the comma glue onto
      // the end of the filename token (scanOneToken stops at whitespace, not
      // ','), so the path never resolved; `do file.do , nostop` (space before
      // the comma) instead shredded ","/"nostop" into the positional-args
      // vector, corrupting `1'`/`2'` for any do-file that uses them. Splitting
      // here first means the filename/positional-arg extraction below never
      // sees the options tail at all — behaviorally a no-op for the common
      // no-comma case (findUnquotedComma returns -1, argsAndFileTail ==
      // remainder unchanged).
      std::string remainder = trimWs(strippedForLoop.substr(sp + 1));
      // W2 doinclude (BUG-529): this arm runs BEFORE step 0b's macro expansion,
      // so `do "$DIR/x.do"` used to look for a file literally named `$DIR/x.do`
      // (r(601)) -- 1,766 of 2,786 corpus do/run lines build the path from a
      // macro. Expand the tail here, in the CALLER's scope, with 0b's own call
      // pair and error passthrough (w2_include_scope.log do_gpath /
      // include_gpath / include_gbrace_noext / include_lpath answer rc 0). The
      // args are expanded here too, which is what Stata does: `do f `x'` hands
      // the callee x's value, not the text of a reference its private scope
      // could no longer resolve. Expanding before the comma split matches
      // Stata's expand-then-parse order (a macro holding a comma moves it).
      if (eval::hasMacroRefs(remainder)) {
        eval::ExpandResult er = eval::expandMacros(remainder, session.eval);
        if (er.isError) {
          ojson out = ojson::array();
          out.push_back(echo);
          out.push_back(er.error);
          return out;
        }
        remainder = trimWs(er.line);
      }
      std::string argsAndFileTail = remainder;
      std::string doOptsTail;
      {
        long long commaPos = eval::findUnquotedComma(remainder);
        if (commaPos >= 0) {
          argsAndFileTail = trimWs(remainder.substr(0, static_cast<size_t>(commaPos)));
          doOptsTail = trimWs(remainder.substr(static_cast<size_t>(commaPos) + 1));
        }
      }
      if (!doOptsTail.empty()) {
        // `nostop` is the ONLY real Stata do/run option (syntax diagram:
        // `do filename [args] [, nostop]`) — consumed/recognized here so it
        // no longer gets mangled into doArgs above; its own "continue past
        // an error instead of stopping the do-file" semantic is a SEPARATE
        // follow-up (doexec::executeDoSource's stop-on-first-error loop is
        // unchanged by this fix — out of BS-018's parsing-bug scope). Any
        // OTHER token here is genuinely unsupported -> the same honest
        // r(198)-parity reject every other arm's option net uses (BS-097),
        // rather than silently swallowing it.
        static const std::unordered_set<std::string> kDoRunKnownOpts = {"nostop"};
        // W2 doinclude: `include filename [, adopath]` has its own option set
        // ([P] include); `nostop` is refused there (w2_include_scope.log
        // include_opt_nostop: "option nostop not allowed", r(198)). `adopath`
        // is accepted without its search: no target-paper line uses it.
        static const std::unordered_set<std::string> kIncludeKnownOpts = {"adopath"};
        auto doOpts = eval::parseOpts(doOptsTail);
        if (auto err = eval::rejectUnknownOptions(
                doOpts, isInclude ? kIncludeKnownOpts : kDoRunKnownOpts)) {
          ojson out = ojson::array();
          out.push_back(echo);
          out.push_back(ojson(*err));
          return out;
        }
      }
      auto [fnRaw, argsRest] = eval::scanOneToken(argsAndFileTail, /*bind=*/true);
      std::vector<std::string> doArgs;
      std::string rest = argsRest;
      while (!trimWs(rest).empty()) {
        std::string prev = rest;
        auto [tok, newRest] = eval::scanOneToken(rest, /*bind=*/true);
        rest = newRest;
        if (rest == prev) break;  // defensive: no progress → avoid infinite loop
        doArgs.push_back(tok);
      }
      if (!fnRaw.empty() && isInclude && !doArgs.empty()) {
        // `include` takes no arguments (w2_include_scope.log include_extra_token:
        // "invalid syntax", r(198)).
        ojson out = ojson::array();
        out.push_back(echo);
        out.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax",
                                             std::string("include"))));
        return out;
      }
      if (!fnRaw.empty()) {
        doexec::ExecuteFileResult fres;
        {
          // W2 doinclude (BUG-530): do/run get a PRIVATE local scope; include
          // does not. The guard lives HERE, not in executeDo, so the top-level
          // `--do`/GUI routes and `parallel do` keep today's session scope. The
          // args are seeded by executeDoSource into the fresh scope, so the
          // caller's `0'/`1'.. are no longer clobbered (do_args_private).
          std::optional<doexec::PrivateLocalScope> privateScope;
          if (!isInclude) privateScope.emplace(session);
          fres = doexec::executeDo(doexec::withDefaultDoExtension(fnRaw), session, dispatch,
                                   doArgs);
        }
        ojson out = ojson::array();
        ojson echoWithPath = echo;
        echoWithPath["resolved_path"] = fres.resolvedPath;
        echoWithPath["source"] = fres.source;
        out.push_back(echoWithPath);
        // BS-036 (BETA campaign C3): real Stata's ONLY documented difference
        // between `do` and `run` is that `run` does not echo the file's own
        // commands as they execute (their OUTPUT — display/summarize/error/
        // etc — still shows exactly as `do` would; only the per-line ". cmd
        // text" echo is suppressed). Previously both were byte-identical.
        // Reuses the SAME block-filtering idiom the quietly/noisily prefix
        // handling above already applies (`b.value("type","") != "echo"`).
        bool suppressEcho = (firstTokLower == "run");
        for (const auto& b : fres.result.blocks) {
          if (suppressEcho && b.is_object() && b.value("type", std::string()) == "echo") continue;
          out.push_back(b);
        }
        // W2 doinclude (BUG-531): `exit N` (N != 0) inside the file ends it AND
        // hands the caller r(N) -- w2_include_scope.log do_exit5 / include_exit5:
        // "exit 5 / r(5);", _rc 5 under capture. The inner run consumed and
        // erased its halt sentinel (doexec.cpp handleUncapturedError), so the
        // code was lost and the caller saw rc 0. A bare `exit` still ends only
        // the file (do_exit / include_exit: rc 0, caller continues). The block
        // carries the exact rc in `reason`, like `error N`, and its message is
        // deliberately NOT the "exit N" sentinel shape: the caller is not
        // exiting cleanly, it is receiving an error.
        if (fres.result.stoppedAtExitCode.has_value() && *fres.result.stoppedAtExitCode != 0) {
          const int code = *fres.result.stoppedAtExitCode;
          ojson exitErr = ojson(eval::engineError("do-file ended with exit " + std::to_string(code)));
          exitErr["reason"] = "r(" + std::to_string(code) + ")";
          out.push_back(exitErr);
        }
        if (fres.result.stoppedAtReason.has_value()) {
          std::string linepart;
          if (fres.result.stoppedAtLine.has_value()) {
            linepart = " at line " + std::to_string(*fres.result.stoppedAtLine);
          }
          ojson stopNotice;
          stopNotice["type"] = "echo";
          stopNotice["text"] = "do-file stopped" + linepart + " — " + *fres.result.stoppedAtReason;
          out.push_back(stopNotice);
        }
        return out;
      }
    }
  }

  // ------------------------------------------------------------------
  // Pre-0. `capture`/`quietly`/`noisily` prefix stripping (dispatcher.py's
  // same-named step, items 4-5). Must run BEFORE step (a) continue-sentinel
  // and 0a try_capture_definition so a prefixed line's remainder is what
  // those steps see.
  {
    // BS-016: cap/capt/captu/captur/capture are all legal Stata
    // abbreviations of `capture` (previously only cap/capture were
    // recognised, silently rejecting captu/captur/capt as unknown commands).
    static const std::set<std::string> kCapturePrefixes = {"capture", "captur", "captu", "capt",
                                                             "cap"};
    // N1(a) (BETA Phase C batch C1): Stata accepts n/no/noi/nois/noisi/
    // noisil/noisily as legal abbreviations of `noisily` — this set
    // previously had only {noisily, nois, n}, missing no/noi/noisi/noisil.
    // `noi` is by far the single most common spelling in practice
    // (`cap noi <cmd>`); its absence meant `cap noi X` dispatched "noi" as
    // an unknown command, whose error capture then swallowed X's entire
    // output (rc=199, zero output) — exactly the "cap noi suppresses
    // everything" symptom the C1 probes showed. The isQuietly classifier
    // just below still only lists the QUIETLY-class spellings explicitly;
    // every newly-added noisily-class spelling correctly falls to its
    // `else` (noisily) branch by construction, so no change needed there.
    static const std::set<std::string> kQuietlyPrefixes = {"quietly", "qui",    "quie",   "quiet",
                                                             "noisily", "noisil", "noisi",  "nois",
                                                             "noi",     "no",     "n"};

    // N1(b): is `remainder`'s OWN leading prefix (if any) noisily-class?
    // Real Stata: bare `capture cmd` suppresses ALL output — success or
    // error; only `capture noisily cmd` shows it (both on success and on
    // error). Only the FIRST token of `remainder` is inspected — a
    // pathological `capture quietly noisily X` triple-nest therefore
    // resolves as quietly (documented, acceptable known deviation).
    auto isNoisilyRooted = [](const std::string& remainder) {
      std::string fw = trimWs(remainder);
      size_t sp = fw.find_first_of(" \t");
      std::string firstTok = lower(rstripColon(sp == std::string::npos ? fw : fw.substr(0, sp)));
      static const std::set<std::string> kNoisilyClass = {"noisily", "noisil", "noisi", "nois",
                                                            "noi",     "no",     "n"};
      return kNoisilyClass.count(firstTok) != 0;
    };

    // BUG-526 (W2 labelglued; oracle w2_cmdtriage.log section D): a prefix
    // delivered THROUGH a macro. `local q quietly` + `` `q' summarize a `` is
    // rc 0 with r(N)=12 (macro_quietly); `global qui "qui"` + `${qui} generate
    // double mq = 1` is rc 0 (global_qui); `local cp capture` + `` `cp' drop
    // nosuchvar `` is rc 0 where the bare `drop nosuchvar` is r(111)
    // (macro_capture against ctl_drop_missing). Stata expands macros before it
    // looks for the prefix; this block ran on the RAW line, missed, and step
    // 0b's expansion then handed the tokenizer a command literally named
    // `quietly` -> r(199), an uncaptured stop (paper 073 alone carries 2,981
    // `${qui} cmd` lines).
    //
    // Only the HEAD token is expanded here, and the line re-enters dispatch
    // only when that head became a capture/quietly/noisily word. The rest of
    // the line stays raw and is expanded exactly once, by the inner dispatch's
    // own step 0b -- so a macro in the remainder is not expanded twice, and a
    // head that expands to an ordinary command, or to nothing, keeps today's
    // path (0b expands the whole line and the command runs). The inner echo
    // is dropped: Stata logs the line as typed, and `blocks` already holds it.
    {
      const std::string rawTrim = trimWs(rawCmd);
      if (!rawTrim.empty() && (rawTrim[0] == '`' || rawTrim[0] == '$')) {
        const size_t sp = rawTrim.find_first_of(" \t");
        const std::string head = sp == std::string::npos ? rawTrim : rawTrim.substr(0, sp);
        const std::string rest = sp == std::string::npos ? std::string() : rawTrim.substr(sp);
        if (eval::hasMacroRefs(head)) {
          eval::ExpandResult er = eval::expandMacros(head, session.eval);
          if (!er.isError) {
            const std::string expHead = trimWs(er.line);
            const size_t hs = expHead.find_first_of(" \t");
            const std::string word =
                lower(rstripColon(hs == std::string::npos ? expHead : expHead.substr(0, hs)));
            if (!word.empty() &&
                (kCapturePrefixes.count(word) != 0 || kQuietlyPrefixes.count(word) != 0)) {
              ojson inner = dispatchImpl(expHead + rest, session);
              if (inner.is_array()) {
                for (const auto& b : inner) {
                  if (b.is_object() && b.value("type", std::string()) == "echo") continue;
                  blocks.push_back(b);
                }
              } else if (!inner.is_null()) {
                blocks.push_back(inner);
              }
              return blocks;
            }
          }
        }
      }
    }

    std::optional<std::string> capRemainder;
    {
      dprof::Scope __z(dprof::kPrefixStrip);
      static const PrefixTable kCaptureTable(kCapturePrefixes);
      capRemainder = stripPrefix(rawCmd, kCaptureTable);
    }
    if (capRemainder.has_value()) {
      ojson innerBlocks = dispatch(*capRemainder, session);
      // BUG-116 class (A11 sweep, oracle-verified): a NESTED `capture` that
      // already failed tags its error block captured:true (BS-127) — that
      // statement, AS A COMMAND, still completed without escaping an error
      // (capture never propagates one), so it must not read as THIS
      // capture's own wrapped-command error. Confirmed against real Stata
      // (StataBE 19.5): `capture capture noisily regress y nosuchvar`
      // leaves _rc==0, not the inner regress's rc — the same
      // !captured guard doexec.cpp/loops.cpp's runCaptureBlock use.
      const ojson* errBlock = nullptr;
      for (const auto& b : innerBlocks) {
        if (b.value("type", "") == "error" && !b.value("captured", false)) {
          errBlock = &b;
          break;
        }
      }
      // BS-100: coarse Stata rc map (piet::eval::coarseRc) replaces the old
      // boolean-collapsed "_rc=1" hardcode — see errors.h's doc comment for
      // the mapping + BS-118's post-beta exact-code scope.
      session.eval.locals.set("_rc", errBlock != nullptr ? eval::coarseRc(*errBlock) : "0");

      // N1(b): bare `capture cmd` suppresses ALL output regardless of
      // success/failure; `capture noisily cmd` shows it regardless of
      // success/failure. Previously the success path always forwarded
      // output (ignoring noisily) and the error path always suppressed it
      // (also ignoring noisily) — backwards for `capture noisily cmd`,
      // which must show output on BOTH outcomes.
      if (isNoisilyRooted(*capRemainder)) {
        ojson out = ojson::array({echo});
        for (const auto& b : innerBlocks) {
          if (b.value("type", "") != "echo") {
            // BS-127: an error block that reaches the caller under a
            // `capture` prefix is SHOWN (noisily) but must not HALT a
            // running do-file — real Stata shows the message, sets _rc,
            // and continues. doexec's stop-on-first-error loop skips
            // blocks carrying this tag.
            if (b.value("type", "") == "error") {
              ojson tagged = b;
              tagged["captured"] = true;
              out.push_back(tagged);
              continue;
            }
            out.push_back(b);
          }
        }
        return out;
      }
      // BUG-350: a plain `capture` drops the error block entirely — that is
      // correct Stata behaviour and stays. What was missing is that NOTHING
      // recorded it, so a run could hide a disk-full and still look complete.
      // Count it here, at the one place it is dropped, and keep the first
      // message for the end-of-run summary.
      for (const auto& b : innerBlocks) {
        if (b.is_object() && b.value("type", std::string()) == "error") {
          piet::counters::noteSwallowedError(b.value("message", std::string()));
        }
      }
      return ojson::array({echo});
    }

    std::optional<std::string> quiRemainder;
    {
      dprof::Scope __z(dprof::kPrefixStrip);
      static const PrefixTable kQuietlyTable(kQuietlyPrefixes);
      quiRemainder = stripPrefix(rawCmd, kQuietlyTable);
    }
    if (quiRemainder.has_value()) {
      std::string firstWord = trimWs(rawCmd);
      size_t sp2 = firstWord.find_first_of(" \t");
      std::string fw = lower(rstripColon(sp2 == std::string::npos ? firstWord : firstWord.substr(0, sp2)));
      // N1(a): this classifier only needs to list the QUIETLY-class
      // spellings explicitly — every noisily-class spelling (including the
      // ones added to kQuietlyPrefixes above: no/noi/noisi/noisil) already
      // falls to the `else` (noisily) branch below by construction, so the
      // expanded abbreviation set needs no change here.
      bool isQuietly = (fw == "quietly" || fw == "qui" || fw == "quie" || fw == "quiet");
      // BUG-189: same reason as the braced form — the filters below run after
      // the inner dispatch has already teed to `log using`, so the scope must be
      // declared around the dispatch itself, not applied to its result.
      ojson innerBlocks;
      {
        SessionState::LogVerbosityScope __lv(session, isQuietly);
        innerBlocks = dispatch(*quiRemainder, session);
      }
      ojson out = ojson::array({echo});

      // BS-040: if the remainder is ITSELF another quietly/noisily-prefixed
      // command, the recursive dispatch() call above has ALREADY resolved
      // its effective visibility — Stata nests these prefixes with the
      // INNERMOST keyword winning (`quietly noisily cmd` must show cmd's
      // output; the reverse must hide it). Re-applying THIS layer's own
      // filter on top of an already-resolved inner result would let an
      // outer `quietly` re-hide what an inner `noisily` correctly chose to
      // show (the exact bug). Detect that case and pass the inner result
      // through untouched instead — only the INNERMOST layer in a chain
      // (whose own remainder is NOT itself quiet/noisy-prefixed) actually
      // applies a filter, via the unchanged isQuietly branch below.
      static const PrefixTable kQuietlyTable2(kQuietlyPrefixes);
      if (stripPrefix(*quiRemainder, kQuietlyTable2).has_value()) {
        for (const auto& b : innerBlocks) out.push_back(b);
        return out;
      }

      // BUG-189: hand this layer's verdict to the log tee one frame out. Only
      // the innermost layer reaches here (the chained case returned above
      // without touching the flag), so what the tee sees is the keyword that
      // actually decided visibility.
      session.setLineVisibility(isQuietly ? SessionState::LineVis::Quiet
                                          : SessionState::LineVis::Noisy);
      if (isQuietly) {
        for (const auto& b : innerBlocks) {
          std::string t = b.value("type", "");
          if (t == "error" || t == "echo") out.push_back(b);
        }
      } else {
        for (const auto& b : innerBlocks) {
          if (b.value("type", "") != "echo") out.push_back(b);
        }
      }
      return out;
    }
  }

  // ------------------------------------------------------------------
  // Pre-0b. `eststo [NAME] [, options]: command` (BS-020/BS-095) — same
  // unified-prefix philosophy as capture/quietly/noisily above (recognise on
  // the raw line, dispatch the remainder exactly once, apply this prefix's own
  // post-processing): run the remainder FIRST so handlers::runEststo
  // (eststo_esttab.cpp) snapshots the FRESH result, then store it.
  //
  // W2 eststoprefix (BUG-520). This arm used to be the regex
  // `^eststo\s+(\w+)\s*:` on the RAW line, which is blind to everything but a
  // literal name. Three shapes fell past it to the flat `eststo` arm, which
  // never dispatches a remainder:
  //   eststo: regress y w              bare -- one of the commonest corpus lines
  //   eststo mac`i': regress y w       a macro in NAME (unexpanded here: `\w+`
  //                                    stops at the backtick)
  //   eststo t0, title("x"): regress   options before the colon
  // With inner options the flat arm refused the INNER command's first option,
  // r(198). Without them it stored the STALE previous model under a corrupted
  // key (`regress`, `mac3:`) at rc 0 and never ran the command -- silent wrong.
  // Oracle, tests/eval/golden/aer_w2/w2_eststo_prefix.log: A2 bare, A3/A4 local
  // in NAME (A4 in a loop, inner `vce(cluster id)`), A5 global in NAME, A6
  // options before the colon incl. a QUOTED colon, A7 prefix(), A8 a failing
  // inner command stores nothing and returns its own rc.
  //
  // So the split is structural now: first word `eststo`, then the first colon
  // outside quotes and outside a macro reference. Macros are expanded in the
  // HEAD ONLY. The remainder goes to dispatch() raw, because dispatch expands it
  // itself (step 0b) and expanding twice is a bug of its own (`\`'-deferred
  // macros, `++i'). Head first, remainder second is also Stata's left-to-right
  // order, which `eststo m`++i': regress y x`i'` can observe.
  //
  // The no-colon forms -- `eststo`, `eststo NAME`, `eststo dir`, `eststo
  // drop|clear [NAME]` -- never enter here, and dir/drop/clear in the NAME
  // position are still left to the flat arm (which now refuses the colon shape
  // by name instead of storing under it).
  {
    dprof::Scope __z(dprof::kColonPrefix);
    const std::string trimmedRaw = trimWs(rawCmd);
    // A3: all three colon-prefix forms require a literal ':'.
    const size_t wordEnd = trimmedRaw.find(':') == std::string::npos
                               ? std::string::npos
                               : trimmedRaw.find_first_of(" \t,:");
    if (wordEnd != std::string::npos && lower(trimmedRaw.substr(0, wordEnd)) == "eststo") {
      const std::string rest = trimmedRaw.substr(wordEnd);
      const size_t colonPos = findEststoPrefixColon(rest);
      std::string headRaw =
          colonPos == std::string::npos ? std::string() : trimWs(rest.substr(0, colonPos));
      // Checked on the RAW head, before any expansion, so a line this arm hands
      // back to the flat arm has had no macro side effect from here.
      const size_t subEnd = headRaw.find_first_of(" \t,");
      const std::string sub0 = lower(headRaw.substr(0, subEnd));
      if (colonPos != std::string::npos && sub0 != "dir" && sub0 != "drop" && sub0 != "clear") {
        const std::string eststoRemainder = trimWs(rest.substr(colonPos + 1));
        // The community-name gate (WS-B) sits after tokenize, which this arm
        // returns before -- so it is asked here. Without this, `set extcompat
        // off` gated `eststo m1` and let `eststo m1: regress y x` through (it
        // always had, for the literal-name form; bare `eststo:` would have made
        // that the commonest line in the corpus). The INNER command meets the
        // gate on its own, inside its own dispatch.
        if (!settings::extcompatEnabled()) {
          if (const registry::CommunityName* cn = registry::findCommunityName("eststo")) {
            blocks.push_back(ojson(eval::errorBlock(
                "unsupported_command", registry::communityGateMessage(*cn), std::string("eststo"))));
            return blocks;
          }
        }
        if (eval::hasMacroRefs(headRaw)) {
          eval::ExpandResult er = eval::expandMacros(headRaw, session.eval);
          if (er.isError) {
            blocks.push_back(er.error);
            return blocks;
          }
          headRaw = er.line;
        }
        // The head is parsed and validated BEFORE the command runs, as
        // eststo.ado's `syntax` does: a bad head is r(198) and nothing ran.
        auto [namePart, optsPart] = splitTopLevelComma(headRaw);
        std::vector<std::string> nameToks;
        {
          std::istringstream ns(namePart);
          std::string tk;
          while (ns >> tk) nameToks.push_back(tk);
        }
        // `\w+`, exactly what the regex this replaces accepted -- no oracle arm
        // measures an invalid name, so the accepted set is neither widened nor
        // narrowed here. An EMPTY name part is the bare form (A2). A macro that
        // expands to nothing mid-name (`est`undefined'` -> `est`) is a legal name.
        static const std::regex reEststoName(R"(^\w+$)");
        if (nameToks.size() > 1 ||
            (nameToks.size() == 1 && !std::regex_match(nameToks[0], reEststoName))) {
          blocks.push_back(ojson(eval::engineError(
              "eststo: `" + trimWs(namePart) + "` is not one name -- the prefix form is "
              "`eststo [name] [, options]: command`",
              "syntax_error")));
          return blocks;
        }
        EststoStoreOpts storeOpts;
        if (optsPart.has_value()) {
          std::vector<std::pair<std::string, eval::OptValue>> headOpts;
          try {
            headOpts = eval::parseOpts(trimWs(*optsPart));
          } catch (const eval::TokenizerError& te) {
            blocks.push_back(ojson(te.block));
            return blocks;
          }
          // The head never reaches the tokenizer's resolution point, so it gets
          // the same canonicalization here -- otherwise `ti("x")` would pass the
          // net below and then miss readEststoStoreOpts' `k == "title"` (BUG-231's
          // mechanism), and the BUG-233 late-resolution guard would count it.
          eval::canonicalizeForCommand(headOpts, "eststo");
          if (auto err = readEststoStoreOpts(headOpts, &storeOpts)) {
            blocks.push_back(*err);
            return blocks;
          }
        }
        ojson out = ojson::array({echo});
        bool hasError = false;
        // `eststo NAME:` with nothing after the colon is the flat store --
        // eststo.ado only runs a command when `s(after)` is non-empty.
        if (!eststoRemainder.empty()) {
          ojson innerBlocks = dispatch(eststoRemainder, session);
          for (const auto& b : innerBlocks) {
            // is_object() first: value() on anything else throws type_error.306
            // and takes the whole process with it.
            const std::string t = b.is_object() ? b.value("type", std::string()) : std::string();
            if (t == "error") hasError = true;
            if (t != "echo") out.push_back(b);
          }
        }
        // A8: a failing inner command stores NOTHING (`estimates table bad1` is
        // r(111) afterwards) and the line's rc is the inner command's own. The
        // auto-name counter is not consumed either: the bare store after two
        // failures is est10, the very next number.
        if (!hasError) {
          std::optional<std::string> eststoNameOpt;
          if (!nameToks.empty()) eststoNameOpt = nameToks[0];
          out.push_back(ojson(handlers::runEststo(eststoNameOpt, storeOpts.title, session,
                                                  storeOpts.autoPrefix)));
        }
        return out;
      }
    }
  }

  // ------------------------------------------------------------------
  // Pre-0c. `version #: command` (BS-041/BS-095) — same unification as
  // eststo NAME: above. Bare `version #` (no colon) is UNCHANGED — it stays
  // the existing silent accept-and-ignore no-op (flat if-chain's
  // `cmd=="set"||cmd=="version"||cmd=="compress"` arm). This block only
  // fires when a colon+remainder is actually present, which today silently
  // vanishes in its entirety (the whole line no-ops, remainder included —
  // e.g. `version 14: regress y x` runs nothing and reports nothing).
  // `version` has no suppression semantics of its own, so on success this
  // is a transparent passthrough (mirrors `noisily`'s shape: drop the
  // remainder's own echo, keep everything else, including any error).
  {
    dprof::Scope __z(dprof::kColonPrefix);
    static const std::regex reVersionColon(R"(^version\s+(\S+)\s*:\s*(.+)$)", std::regex::icase);
    std::smatch vm;
    std::string trimmedRawV = trimWs(rawCmd);
    // A3: all three colon-prefix forms require a literal ':'.
    if (trimmedRawV.find(':') != std::string::npos &&
        std::regex_search(trimmedRawV, vm, reVersionColon)) {
      std::string versionRemainder = trimWs(vm.str(2));
      if (!versionRemainder.empty()) {
        ojson innerBlocks = dispatch(versionRemainder, session);
        ojson out = ojson::array({echo});
        for (const auto& b : innerBlocks) {
          if (b.value("type", "") != "echo") out.push_back(b);
        }
        return out;
      }
    }
  }

  // ------------------------------------------------------------------
  // P7 — the `svy:` prefix. Intercepted here, AFTER macro expansion would have
  // happened for the resampling prefixes but structurally alongside the other
  // simple `KEYWORD: command` forms (eststo/version): svy carries no exp_list,
  // so nothing has to be deferred per-replicate and the ordinary expansion
  // order is correct. The design itself lives in SessionState (svyset).
  {
    dprof::Scope __z(dprof::kColonPrefix);
    static const std::regex reSvyColon(R"(^\s*svy\s*(?:,[^:]*)?:\s*(.+)$)",
                                        std::regex::icase);
    std::smatch sm;
    std::string trimmedRawS = trimWs(rawCmd);
    // A3: all three colon-prefix forms require a literal ':'.
    if (trimmedRawS.find(':') != std::string::npos &&
        std::regex_search(trimmedRawS, sm, reSvyColon)) {
      blocks.push_back(ojson(handlers::runSvyPrefix(trimWs(sm.str(1)), session)));
      return blocks;
    }
  }

  // ------------------------------------------------------------------
  // (a) `continue` / `continue, break` (U7/S11) — loop-control bubble,
  // intercepted before any keyword handler. Normalise by removing ALL
  // whitespace so "continue, break" / "continue ,break" / "continue,break"
  // collapse to the same token.
  {
    dprof::Scope __z(dprof::kSmallSteps);
    std::string noSpace;
    for (char c : lower(trimWs(rawCmd))) {
      if (c != ' ' && c != '\t') noSpace += c;
    }
    if (noSpace == "continue" || noSpace == "continue,break") {
      // BUG-483. Inside a loop this is the iteration control it has always
      // been. OUTSIDE every loop it used to return the same sentinel, which
      // doexec turned into `out.control` and an immediate return — the silent
      // end of the do-file, at rc 0, with no stopped_at_line. doexec.h's own
      // contract already said a top-level run "never returns this to its
      // caller"; the code did.
      //
      // Stata, measured (tests/eval/golden/bug483_484_control_oracle.log):
      // `continue` and `continue, break` outside a loop print "statement out of
      // context", set r(119), and the next statement runs. The rc travels in
      // `reason` because coarseRc has no rule that would reach 119 from this
      // message (BUG-187: reason is the only channel carrying an exact rc).
      if (loops::loopBodyDepth() == 0) {
        return ojson::array({echo, ojson(eval::errorBlock("engine_error",
                                                          "statement out of context", std::nullopt,
                                                          std::nullopt, std::nullopt, "r(119)"))});
      }
      ojson ctrl;
      ctrl["type"] = "_control";
      ctrl["action"] = (noSpace.find("break") != std::string::npos) ? "break" : "continue";
      return ojson::array({echo, ctrl});
    }
    // BUG-483, the other half. A bare `break` never matched here, so it fell
    // through to the generic arms and came back "No dataset is loaded…" — not
    // silent, so not the headline, but not Stata either. `break` IS a real
    // command (`which break` is rc 0 while `brea` is r(199), so it is also not
    // abbreviable) and it is a NO-OP: rc 0 and nothing printed, measured both
    // at top level and inside forvalues/while/foreach, where it does NOT exit
    // the loop (5 of 5 iterations; `continue, break` is the exit and gives 3).
    // Treating the two keywords as one case would therefore be wrong in one
    // direction whichever case was picked.
    if (noSpace == "break") {
      return ojson::array({echo});
    }
  }

  // ------------------------------------------------------------------
  // (b) `args [name1 name2 …]` (S3/U3-args) — program bodies open with this.
  // Stata: `args a b c` == `local a `1'` / `local b `2'` / `local c `3'`.
  // Silent (no console output), Stata-faithful.
  {
    dprof::Scope __z(dprof::kSmallSteps);
    std::string argsStripped = trimWs(rawCmd);
    if (lower(argsStripped).rfind("args", 0) == 0 &&
        (argsStripped.size() == 4 || argsStripped[4] == ' ' || argsStripped[4] == '\t')) {
      std::istringstream namesIss(argsStripped.substr(4));
      std::string name;
      int idx = 1;
      while (namesIss >> name) {
        session.eval.locals.set(name, session.eval.locals.get(std::to_string(idx), ""));
        ++idx;
      }
      return ojson::array();
    }
  }

  // ------------------------------------------------------------------
  // (c) `gettoken TOK REST : SRC [, bind]` (U4) — run on the RAW
  // (pre-expansion) line so TOK/REST/SRC are literal identifiers, not macro
  // refs (mirrors dispatcher.py's step-0 (c): `_u4_first == "gettoken"`).
  // Silent ([]) on success; [echo, error] on a syntax-error failure.
  {
    dprof::Scope __z(dprof::kSmallSteps);
    std::string u4Stripped = trimWs(rawCmd);
    size_t u4Sp = u4Stripped.find_first_of(" \t");
    std::string u4First = lower(u4Sp == std::string::npos ? u4Stripped : u4Stripped.substr(0, u4Sp));
    if (u4First == "gettoken") {
      eval::LocalsStore store;
      store.get = [&session](const std::string& name) -> std::string {
        return session.eval.locals.get(name, "");
      };
      store.set = [&session](const std::string& name, const std::string& value) {
        session.eval.locals.set(name, value);
      };
      std::vector<nlohmann::json> gtBlocks = eval::runGettoken(rawCmd, store);
      if (!gtBlocks.empty()) {
        ojson out = ojson::array({echo});
        for (const auto& b : gtBlocks) out.push_back(ojson(b));
        return out;
      }
      return ojson::array();  // silent success
    }
  }

  // ------------------------------------------------------------------
  // (c)/(d) `tempfile NAME...` / `tempname NAME...` / `tempvar NAME...`
  // (U4; tempvar added BS-037, BETA campaign C3) — run on the RAW
  // (pre-expansion) line so the names they bind are literal identifiers, not
  // macro refs. Silent ([]) on success; [echo, ...errorBlocks] on failure —
  // mirrors dispatcher.py's `if _gt_blocks: blocks.extend(...); return blocks`
  // / `return []` split. Running BEFORE tokenize() (below) is exactly what
  // makes `tempvar` safe here: eval/tokenizer.cpp's early macro-statement
  // keyword net (Step 1c) still lists "tempvar" among its unconditional
  // "unsupported_macro" rejects — this U4 block intercepts the line first,
  // so tokenize() never sees it, identical to how tempfile/tempname already
  // bypass that same net today.
  // AER-SCOPE1 bundle D: `!cmd` is Stata's shorthand for `shell cmd`. Rewritten
  // here rather than given its own arm because `!` is not a word the tokenizer
  // can carry to a registry lookup, and because doing it before step 0b means
  // the macros inside (`!rm -f $path/`pattern'`, the corpus's actual spelling)
  // expand exactly as they would after `shell`.
  {
    static const std::regex reBangShell(R"(^\s*!\s*(\S.*)$)");
    std::smatch bm;
    if (std::regex_match(rawCmd, bm, reBangShell)) {
      return piet::dispatch("shell " + bm[1].str(), session);
    }
  }

  {
    dprof::Scope __z(dprof::kSmallSteps);
    ojson u4Out;
    if (tryTempDeclIntercept(rawCmd, session, echo, u4Out)) return u4Out;
  }

  // ------------------------------------------------------------------
  // (e) `return scalar`/`return local` (U5/S7) — must run BEFORE
  // try_capture_definition so `return scalar n = ...` isn't consumed as a
  // macro definition. Staged into session.returnStaging — silent on success.
  {
    dprof::Scope __z(dprof::kSmallSteps);
    ojson returnErr;
    if (program::handleReturnStmt(rawCmd, session, returnErr)) {
      if (!returnErr.empty()) {
        blocks.push_back(returnErr);
        return blocks;
      }
      return ojson::array();
    }
  }

  // ------------------------------------------------------------------
  // (f) `program drop NAME` / `program dir` ("0a-bis") — begins with the
  // `program` keyword the tokenizer rejects, so intercepted here on the raw
  // line, pre-expansion. `program define … end` is captured pre-tokenizer
  // by doexec's program::captureProgams and never reaches dispatch as a
  // bare line.
  {
    dprof::Scope __z(dprof::kSmallSteps);
    std::string progLine = trimWs(rawCmd);
    std::istringstream progIss(progLine);
    std::string progTok0;
    progIss >> progTok0;
    // BUG-434: `prog drop NAME` is `program drop NAME`. The abbreviation is not
    // in the registry — `program` has no row there (it is intercepted before
    // tokenizing), so registry::resolveCanonical cannot help — it lives in
    // program.cpp's own `^prog(?:ram)?` head regexes, which is why `prog def`
    // worked and this arm, matching the full spelling only, answered rc 199 for
    // the drop. One sub-verb honoured the abbreviation and its sibling did not,
    // and the failure said nothing about the spelling. Unguarded it halts the
    // do-file, which is where a corpus package stopped.
    //
    // The accepted set is program.cpp's, deliberately: two spellings of the
    // same head in two files is how they drift apart.
    const std::string progHead = lower(progTok0);
    if (progHead == "program" || progHead == "prog") {
      std::string progSub;
      progIss >> progSub;
      std::string progSubLower = lower(progSub);
      if (progSubLower == "drop" || progSubLower == "dir") {
        std::vector<std::string> progNames;
        std::string nm;
        while (progIss >> nm) progNames.push_back(nm);
        ojson mgmtBlock = program::handleProgramManage(progSubLower, progNames, session);
        if (mgmtBlock.value("type", "") == "error") {
          // `program drop NAME` for an absent program is a benign no-op at
          // the dispatch layer (Python parity — see program-port's
          // handleProgramManage doc), does NOT trip `capture`.
          if (progSubLower == "drop") {
            return ojson::array({echo});
          }
          return ojson::array({echo, mgmtBlock});
        }
        if (mgmtBlock.contains("text") && progSubLower == "dir") {
          return ojson::array({echo, mgmtBlock});
        }
        return ojson::array({echo});
      }
    }
  }

  // ------------------------------------------------------------------
  // (f-bis) AER-SCOPE1 bundle F: `input varlist` ... `end`. Like `program`,
  // its body is DATA rather than statements, so doexec folds the whole block
  // into ONE line at preprocess time (handlers::captureInputBlocks, 0x1F
  // separators -- a byte no Stata source contains). It is intercepted here on
  // the raw line for the same reason `program` is: the tokenizer would split
  // the folded rows on their commas and whitespace.
  //
  // The 0x1F test is what distinguishes a folded block from a bare `input`
  // typed with no block; the latter falls through to the registry arm below,
  // which answers honestly instead of silently doing nothing.
  {
    dprof::Scope __z(dprof::kSmallSteps);
    std::string inputLine = trimWs(rawCmd);
    std::istringstream inputIss(inputLine);
    std::string inputTok0;
    inputIss >> inputTok0;
    if (lower(inputTok0) == "input" && rawCmd.find('\x1F') != std::string::npos) {
      ojson r = handlers::runInput(rawCmd, session);
      if (r.is_array()) {
        ojson out = ojson::array({echo});
        for (const auto& b : r) out.push_back(b);
        return out;
      }
      return ojson::array({echo, r});
    }
  }

  // ------------------------------------------------------------------
  // (f-ter) ml-getmata T4: Mata. `mata: stmt` one-liners and the `mata:` ...
  // `end` block doexec folds into one line (mata::captureMataBlocks) run the
  // native M0 subset against the SEPARATE Mata store; `mata` without a colon,
  // and any statement outside M0, is refused by name. Intercepted on the RAW
  // line, before macro expansion and the tokenizer, for the reason `input` is:
  // Mata's `\`, `:-`, `[.,1]` and `"NAME"` are not Stata tokens. The handler
  // expands macros per statement itself (030's `(code \ `i')`). Silent on
  // success, so the empty array is FILTERED, never pushed (defect class 1).
  {
    dprof::Scope __z(dprof::kSmallSteps);
    if (mata::isMataLine(rawCmd)) {
      ojson r = mata::runMataLine(rawCmd, session);
      if (!(r.is_array() && r.empty())) blocks.push_back(r);
      return blocks;
    }
  }

  // ------------------------------------------------------------------
  // 0a. Session-state layer: capture macro definitions (local/global/scalar/
  // increment forms). A capture produces NO console output (Stata semantics)
  // — mirrors dispatcher.py's `if captured is not None: return []`.
  // BS-091/BS-101 (BETA A4): c(name) refs resolve to a literal BEFORE
  // tryCaptureDefinition parses this line — `scalar a = c(N)` / `local x =
  // c(N)` need the SAME single-source c()-registry gen/replace/if use
  // (piet::substituteCRefs, cclass.h). tryCaptureDefinition only sees the
  // narrower EvalSession (not full SessionState), so the substitution runs
  // HERE, at the one call site with full session access — rawCmd itself is
  // untouched (the substituted copy is used for THIS call only; every other
  // downstream use of rawCmd, e.g. macro expansion below, sees the original).
  // BUG-208: `local l : variable label x` is answered inside
  // tryCaptureDefinition, in the eval layer, off session.eval.varLabels — a map
  // whose only writer used to be the evalcase harness, so it read "" in real use
  // for a label `describe` was displaying at the same moment. That store is now a
  // PROJECTION of SessionState's live varMeta/card (see SessionState::
  // syncEvalVarLabels), and this is the one place it is refreshed: immediately
  // before the capture layer that consumes it, at the only call site with full
  // session access — the same reason substituteCRefs runs here.
  //
  // Gated on a deliberately cheap textual precondition, NOT run per command. The
  // sync resolves a varlist, and the MAXPERF verb-overhead selftest gates assert
  // that ordinary verbs construct ZERO Datastores; paying it on every dispatched
  // line would move them. The gate is a conservative superset (any line carrying
  // both a colon and "label"), so it can only ever do harmless extra work — it
  // cannot cause a miss, which is the direction that matters.
  // (`:` is tested first so the lowered copy is only ever made for the rare line
  // that carries one at all.)
  // BUG-216/217 widen the gate: `local f : format x` and `local t : value label x`
  // carry a colon but NOT the word "label" in the `: format` case, so the old
  // precondition could never fire for them — and without the sync the projection
  // is not authoritative, which is exactly the condition under which macros.cpp
  // refuses to decide r(111). Still a conservative SUPERSET of the old gate (the
  // "label" clause is kept verbatim and OR-ed), so it cannot cause a miss; the
  // added arm is narrow on purpose. It fires only for a line that BEGINS with
  // `local`/`global` and also carries a colon — i.e. precisely the extended-macro-
  // function definition shape — which deliberately excludes `by group:` and
  // `bysort x:` prefixes, the common colon-bearing lines that would otherwise pay
  // a projection rebuild per command and move the MAXPERF verb-overhead gates.
  if (rawCmd.find(':') != std::string::npos) {
    const std::string lowered = lower(rawCmd);
    const size_t h = lowered.find_first_not_of(" \t");
    const size_t head = (h == std::string::npos) ? 0 : h;
    if (lowered.find("label") != std::string::npos ||
        lowered.compare(head, 5, "local") == 0 || lowered.compare(head, 6, "global") == 0) {
      session.syncEvalVarLabels();
    }
  }
  {
    dprof::Scope __z(dprof::kCaptureDef);
    {
      ojson extDisplayOut;  // BUG-513: `local NAME : display ...`
      if (tryExtDisplayCapture(rawCmd, session, extDisplayOut)) return extDisplayOut;
    }
    eval::MacroFnError mfe;
    if (eval::tryCaptureDefinition(substituteCRefs(rawCmd, session), session.eval, &mfe)) {
      // BUG-216/217: a consumed-but-FAILED extended macro function. `reason` is the
      // exact-code channel eval::coarseRc() honours (its BUG-187 note), so `_rc`
      // comes back as Stata's own 101/111 instead of the generic 1.
      if (mfe.failed()) {
        // BUG-339: carry the offending line as `context`. This return builds a
        // FRESH array, so the entry echo is dropped — and under `capture
        // noisily` the wrapper strips inner echoes too, which is exactly how
        // 160 of these per corpus run showed up in a log with nothing naming
        // their source. `context` costs nothing and is contract-legal.
        return ojson::array({ojson(eval::errorBlock("engine_error", mfe.message, std::nullopt,
                                                    std::nullopt, strutil::trim(rawCmd),
                                                    "r(" + mfe.rc + ")"))});
      }
      return ojson::array();
    }
  }

  // ------------------------------------------------------------------
  // 0a-bis. Resampling prefixes (P6) — intercepted BEFORE macro expansion.
  //
  // `bootstrap mean=r(mean), reps(30) seed(...): summarize y` carries an
  // expression list that must be evaluated ONCE PER REPLICATE, after that
  // replicate's inner command has posted its r()/e()/_b[]. Step 0b below would
  // substitute those references here and now — against whatever the PREVIOUS
  // command left, or erroring as unset — which is a different question from the
  // one the user asked. So the exp_list is carried verbatim into the driver;
  // the options and the inner command ARE expanded (they legitimately hold
  // locals, e.g. `reps(`R')` or `: regress `depvar' x`).
  {
    dprof::Scope __z(dprof::kResample);
    static const std::regex reResamplePrefix(
        R"(^\s*(bootstrap|jackknife|permute|rolling)\b(.*)$)", std::regex::icase);
    std::smatch rm;
    if (std::regex_match(rawCmd, rm, reResamplePrefix)) {
      const std::string method = lower(trimWs(rm.str(1)));
      std::string rest = rm.str(2);
      static const std::regex reColon(R"(:)");
      std::smatch cm;
      // A bare `bootstrap` (no colon) is not the prefix form — let it fall
      // through to the unsupported-command path, which is what Stata's own
      // "command bootstrap is unrecognized" corresponds to here.
      if (eval::searchUnquoted(rest, reColon, cm)) {
        std::string headPart = trimWs(rest.substr(0, static_cast<size_t>(cm.position(0))));
        std::string innerRaw =
            trimWs(rest.substr(static_cast<size_t>(cm.position(0) + cm.length(0))));
        auto [expAndVar, optsOpt] = splitTopLevelComma(headPart);
        std::string expListRaw = trimWs(expAndVar);
        std::string optsRaw = optsOpt.has_value() ? trimWs(*optsOpt) : std::string();
        // `permute VAR explist` — the permutation variable sits between the
        // command word and the expression list.
        std::string permuteVar;
        if (method == "permute") {
          size_t sp = expListRaw.find_first_of(" \t");
          if (sp == std::string::npos) {
            blocks.push_back(ojson(eval::engineError(
                "permute: usage is `permute PERMVAR name=exp [...] [, options]: command`")));
            return blocks;
          }
          permuteVar = trimWs(expListRaw.substr(0, sp));
          expListRaw = trimWs(expListRaw.substr(sp));
        }
        // Expand the halves that MAY hold macros; the exp_list stays raw.
        auto expandHalf = [&](const std::string& in, bool* failed) -> std::string {
          if (in.empty() || !eval::hasMacroRefs(in)) return in;
          eval::ExpandResult er = eval::expandMacros(in, session.eval);
          if (er.isError) {
            blocks.push_back(er.error);
            *failed = true;
            return in;
          }
          return er.line;
        };
        bool failed = false;
        optsRaw = expandHalf(optsRaw, &failed);
        if (failed) return blocks;
        std::string innerCmd = expandHalf(innerRaw, &failed);
        if (failed) return blocks;

        ojson res = (method == "rolling")
                        ? handlers::runRolling(expListRaw, optsRaw, innerCmd, session)
                        : handlers::runResample(method, permuteVar, expListRaw, optsRaw, innerCmd,
                                                session);
        if (res.is_array()) {
          for (const auto& b : res) blocks.push_back(b);
        } else {
          blocks.push_back(res);
        }
        return blocks;
      }
    }
  }

  // ------------------------------------------------------------------
  // 0b. Session-state layer: expand macro / r() references in the raw line.
  std::string cmdForDispatch = rawCmd;
  {
  dprof::Scope __z(dprof::kExpand);
  if (eval::hasMacroRefs(rawCmd)) {
    eval::ExpandResult r = eval::expandMacros(rawCmd, session.eval);
    if (r.isError) {
      blocks.push_back(r.error);
      return blocks;
    }
    cmdForDispatch = r.line;
  }

  // ------------------------------------------------------------------
  // 0b-bis. BUG-176 (P2-PREP): re-apply the two PRE-expansion matchers above
  // to the line macro expansion just produced.
  //
  // Steps (c)/(d) (tempfile/tempname/tempvar) and 0a (tryCaptureDefinition)
  // both pattern-match the RAW line — deliberately, so the names they bind
  // stay literal identifiers (see those steps' own comments). But that makes
  // the whole matcher blind whenever the COMMAND WORD itself arrives through
  // a macro: `global L "tempvar t1"` + `capture quietly $L` reached step (c)
  // as the literal token `$L`, missed, and fell through to
  // eval/tokenizer.cpp's macro-statement keyword net, which rejects
  // "tempvar" unconditionally -> `unsupported_macro`, rc 198. The `scalar`
  // twin fell past the list/dir-only `scalar` arm further down and returned
  // rc 199. Both are the same defect: expansion happens AFTER the matchers
  // that need to see what it produced. Measured on a do-file that dispatches
  // `capture quietly $BENCH_LINE`.
  //
  // Guarded on `cmdForDispatch != rawCmd`, so the literal path is untouched:
  // a line with no macro refs never re-enters here at all, and one whose
  // expansion is a no-op is byte-identically excluded. Nor can this
  // double-fire — every arm above RETURNS on a match, so reaching this point
  // proves the raw line matched none of them. The only lines that can be
  // consumed here are precisely those whose leading keyword came out of a
  // macro, which is the class being fixed.
  //
  // Deliberately NOT re-applied: steps (e) `return scalar`/`return local`,
  // (f) `program drop|dir`, and 0a-bis's resampling prefixes. Each has
  // ordering semantics of its own (0a-bis in particular MUST see its
  // exp_list unexpanded), and none was implicated by a measurement — left
  // for a follow-up that can measure them.
  if (cmdForDispatch != rawCmd) {
    ojson u4Out;
    if (tryTempDeclIntercept(cmdForDispatch, session, echo, u4Out)) return u4Out;
    // BUG-216/217: same error channel as step 0a's site. This is the arm that sees
    // a colon form whose COMMAND WORD arrived through a macro, so it needs the
    // same rc translation or that shape would still store literal text.
    {
      ojson extDisplayOut;  // BUG-513, the macro-delivered command word (BUG-176's class)
      if (tryExtDisplayCapture(cmdForDispatch, session, extDisplayOut)) return extDisplayOut;
    }
    eval::MacroFnError mfe2;
    if (eval::tryCaptureDefinition(substituteCRefs(cmdForDispatch, session), session.eval,
                                   &mfe2)) {
      if (mfe2.failed()) {
        // BUG-339 twin of the step-0a site above: name the line that failed.
        return ojson::array({ojson(eval::errorBlock("engine_error", mfe2.message, std::nullopt,
                                                    std::nullopt, strutil::trim(rawCmd),
                                                    "r(" + mfe2.rc + ")"))});
      }
      return ojson::array();
    }
  }
  }  // dprof kExpand

  // ------------------------------------------------------------------
  // 1. Tokenize (on the macro-expanded line).
  eval::ParsedCommand parsed;
  // `xi [, opts] : <command>` (AER bundle H). Intercepted BEFORE tokenize
  // because the tokenizer would read `xi:` as the command word and the wrapped
  // line as its varlist. Materializes the _I* columns, then re-dispatches the
  // wrapped command with each i. term replaced by the names it produced -- which
  // is what makes `xi: regress y i.region` estimate on real columns and report
  // `_Iregion_2` in its coefficient table, exactly as Stata does.
  {
    static const std::regex reXiPrefix(R"(^\s*xi\s*(?:,\s*([^:]*?))?\s*:\s*(\S.*)$)",
                                       std::regex::icase);
    std::smatch mxi;
    if (std::regex_match(cmdForDispatch, mxi, reXiPrefix)) {
      const std::string optText = mxi.str(1);
      const std::string wrapped = mxi.str(2);
      std::vector<std::pair<std::string, eval::OptValue>> xiOpts;
      if (optText.find("noomit") != std::string::npos) {
        eval::OptValue b;
        b.isBool = true;
        b.boolValue = true;
        xiOpts.emplace_back("noomit", b);
      }
      static const std::regex rePfx(R"(prefix\(\s*([A-Za-z_]\w*)\s*\))", std::regex::icase);
      std::smatch mp;
      if (std::regex_search(optText, mp, rePfx)) {
        eval::OptValue p;
        p.strValue = mp.str(1);
        xiOpts.emplace_back("prefix", p);
      }
      std::vector<std::string> terms;
      {
        std::istringstream ts(wrapped);
        std::string tk;
        while (ts >> tk)
          if (tk.compare(0, 2, "i.") == 0) terms.push_back(tk);
      }
      if (terms.empty()) {
        // No i. term to expand: Stata still runs the command. Forward it
        // unchanged rather than inventing an error.
        return dispatchImpl(wrapped, session);
      }
      std::string rewritten, xiDisplay;
      ojson xerr =
          handlers::runXiPrefix(terms, xiOpts, wrapped, session, &rewritten, &xiDisplay);
      if (!xerr.is_null()) {
        blocks.push_back(xerr);
        return blocks;
      }
      if (!xiDisplay.empty()) {
        ojson db;
        db["type"] = "display";
        db["text"] = xiDisplay;
        blocks.push_back(db);
      }
      ojson inner = dispatchImpl(rewritten, session);
      if (inner.is_array())
        for (const auto& b : inner) blocks.push_back(b);
      else if (!inner.is_null())
        blocks.push_back(inner);
      return blocks;
    }
  }

  std::string cmd;
  std::string cmdTyped;  // BUG-105/WS-K: the spelling the USER typed, kept for
                         // the extcompat gate, the `display` raw-argument scan
                         // and every message that echoes the command word.
  {
    dprof::Scope __z(dprof::kTokenize);
    try {
      parsed = eval::tokenize(cmdForDispatch);
    } catch (const eval::TokenizerError& te) {
      if (te.block.value("code", "") == "syntax_error" &&
          te.block.value("message", "").find("comment") != std::string::npos) {
        return ojson::array();  // pure comment lines produce nothing
      }
      // BUG-535: under a by-prefix, `in` is Stata's own refusal — "'in' may not
      // be combined with 'by'", r(190) — for every sub-command
      // (w2_bykeep.log arms by_keep_in / by_keep_if_in), not PIET's generic
      // "qualifier unsupported" r(198). The tokenizer rejects `in` on a
      // by-line because `by` has no registry row; only the rc and the words
      // change here, the line still does not run.
      if (te.block.value("code", "") == "unsupported_option" &&
          te.block.value("token", "").rfind("in ", 0) == 0) {
        const std::string head = lower(trimWs(cmdForDispatch).substr(
            0, trimWs(cmdForDispatch).find_first_of(" \t")));
        if (head == "by" || head == "bys" || head == "byso" || head == "bysor" ||
            head == "bysort") {
          blocks.push_back(ojson(eval::errorBlock(
              "syntax_error", "'in' may not be combined with 'by'", te.block.value("token", ""),
              std::nullopt, std::nullopt, "r(190)")));
          return blocks;
        }
      }
      blocks.push_back(ojson(te.block));
      return blocks;
    }
    // BUG-428/432: the row layer gets the matrix/scalar substitution the macro
    // layer has always had. One call site per expression that reaches the row
    // evaluator — the `if` here, `assert`'s claim and gen/replace's expression.
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
      std::optional<ojson> rowErr;
      parsed.ifExpr = substituteRowExprRefs(*parsed.ifExpr, session, &rowErr);
      if (rowErr.has_value()) {  // BUG-456: r(111) / r(109), never a silent `.`/0
        blocks.push_back(*rowErr);
        return blocks;
      }
    }
    cmdTyped = rstripColon(lower(parsed.cmd));
    // BUG-105 (WS-K) — THE ONE COMMAND-NAME RESOLUTION POINT. Every arm below,
    // plus kNoDataCommands and kFixedArityCommands, now compares the CANONICAL
    // spelling, so adding a Stata abbreviation is a one-line registry edit
    // instead of the two-file (registry + if-chain literal) edit the 07-31
    // BUG-105 stopgap needed. Non-registry tokens (by/bysort, the pre-tokenize
    // keywords) come back unchanged, and a community-package name comes back
    // unchanged on purpose — see registry.h's resolveCanonical() comment.
    // Reverting this one line is the seeded-RED leg selftest's
    // testWsKAbbrevResolutionIsLoadBearing() asserts against.
    cmd = registry::resolveCanonical(cmdTyped);

    // BUG-450 — THE VARLIST EXPANSION POINT, the third sibling of the command
    // and option resolution points above and below it.
    //
    // `loops::expandVarlist` calls itself "the ONE varlist wildcard/range
    // expander", and until now its callers were the loop header, `ds`/`unab`
    // and the handful of AER handlers written against it directly. The plain
    // data verbs were not among them: `drop t*` answered `variable t* not
    // found` r(111) while `foreach v of varlist t*` expanded correctly in the
    // same session on the same data. In the ninety-package AER corpus that is
    // 1,214 `drop` lines and 989 `keep` lines carrying a wildcard, plus the
    // range forms — the ordinary way that corpus subsets a dataset.
    //
    // Two guards keep this from being a blanket change:
    //   1. The ROW must opt in (`varlistIsPlain`). `rename t* T*` is Stata's
    //      rename-GROUPS syntax and must reach its arm unexpanded; estimation
    //      rows carry `i.x`/`L.x` in the same field. See registry.cpp's list.
    //   2. The TOKEN must actually look like a pattern. A varlist with no `*`,
    //      `?`, `-` or `_all` is passed through byte-identically, so the
    //      common case is untouched and cannot regress.
    // A no-match answers r(111), which is what Stata answers.
    if (!parsed.varlist.empty()) {
      const registry::CommandSpec* spec = registry::findByName(cmd);
      if (spec != nullptr && spec->varlistIsPlain) {
        // BUG-525, second half (oracle corr_glued == ctl_corr: r(rho)
        // .0171895075182102 both ways; levelsof_glued -> `0 1 2`): Stata's
        // varlist parser accepts ONE pair of parentheses around a plain
        // varlist, and the glued heads deliver exactly that shape once the
        // word is split (`corr (a c)`, `levelsof (g)`). PIET forwarded `(a`
        // and `c)` as names and answered "column '(a' not found". Unwrapped
        // here, at the one varlist site -- but gated on its OWN allow-list
        // (kParenVarlistUnwrap), not on `varlistIsPlain`: the oracle has an
        // arm for exactly corr_glued and levelsof_glued, not for the other
        // thirteen plain-varlist rows (drop/keep/summarize/destring/...),
        // and reusing the broad flag would have changed their answer for
        // `(x1 x2)`-shaped input with no oracle arm behind it (review
        // finding, W2 labelglued fix round). The whole field must be the
        // one group -- an inner paren, `=` or quote leaves it alone -- so
        // `test (a = b)` and `(y1 x) (y2 x)` shapes are untouched even on a
        // plain row.
        static const std::unordered_set<std::string> kParenVarlistUnwrap = {"corr", "levelsof"};
        if (kParenVarlistUnwrap.count(cmd) != 0 &&
            parsed.varlist.front().front() == '(' && parsed.varlist.back().back() == ')') {
          std::string joined;
          for (size_t i = 0; i < parsed.varlist.size(); ++i) {
            if (i != 0) joined += ' ';
            joined += parsed.varlist[i];
          }
          const std::string inner =
              joined.size() >= 2 ? joined.substr(1, joined.size() - 2) : std::string();
          if (!inner.empty() && inner.find_first_of("()=\"`'") == std::string::npos) {
            std::vector<std::string> unwrapped;
            std::istringstream is(inner);
            for (std::string tk; is >> tk;) unwrapped.push_back(tk);
            if (!unwrapped.empty()) parsed.varlist = unwrapped;
          }
        }
        bool patterned = false;
        for (const auto& t : parsed.varlist) {
          if (t.find('*') != std::string::npos || t.find('?') != std::string::npos ||
              t.find('-') != std::string::npos || lower(t) == "_all") {
            patterned = true;
            break;
          }
        }
        if (patterned && session.hasData()) {
          std::string spec_str;
          for (size_t i = 0; i < parsed.varlist.size(); ++i) {
            if (i != 0) spec_str += ' ';
            spec_str += parsed.varlist[i];
          }
          loops::VarlistExpandResult ex = loops::expandVarlist(spec_str, session);
          if (ex.isError) {
            blocks.push_back(ex.error);
            return blocks;
          }
          parsed.varlist = ex.tokens;
        }
      }
    }
  }

  // ATTR-20260812: the one place a command name becomes canonical, so the one
  // place a per-item timing row can learn what it is actually running. The
  // executor bills by the item's leading token, which makes `capture noisily
  // regress` a `capture` row and files `reg`/`regress` apart; prefixes are
  // stripped upstream of here, so the FIRST resolution inside an item is the
  // real verb. No-op unless PIET_EMIT_TIMING >= 2.
  piet::counters::noteResolvedVerb(cmd);

  // INSPECT-2608: the per-FORM key, built at the same seam and for the same
  // reason — this is where the command is canonical AND fully parsed. A form is
  // the verb plus PRESENCE flags for the qualifiers that change how much work
  // the command does; values never enter the key, so `if x > .5` and `if x > .9`
  // are one row and the table stays bounded. Per-verb alone would average a
  // filtered `summarize` together with a whole-frame one, which is exactly the
  // distinction a per-command efficiency census exists to see.
  // No-op below PIET_EMIT_TIMING=4.
  if (piet::counters::timingLevel() >= 4) {
    std::string form = cmd;
    if (parsed.ifExpr.has_value()) form += "|if";
    if (parsed.inRange.has_value()) form += "|in";
    if (parsed.weightType.has_value()) form += "|w";
    piet::counters::noteResolvedForm(form);
  }

  // ------------------------------------------------------------------
  // 1a-ter. BUG-105 DoD ③ / FIX:OPTNET-ABBREV-SWEEP — THE OPTION-NAME
  // RESOLUTION POINT, the sibling of the command one above.
  //
  // Implements plans/BUG105-OPTABBREV-AUDIT-20260731.md §2's nine measured
  // minimums. Unlike command names, OPTIONS really do follow a prefix rule in
  // Stata: each option documents its own minimum via `{opt l:evel(#)}` markup
  // and every prefix at-or-longer-than that minimum is accepted. So the table
  // below is (option, minimum length) and every prefix in that range is
  // generated — which is also what makes it a FIX and not just a widening:
  //
  // BUG-231 (measured this stint, not predicted by the audit): eval::
  // rejectUnknownOptions already resolves a >=2-char unique prefix against the
  // net's `known` set, so an abbreviated option ALREADY PASSED the gate — while
  // the handler's own `k == "offset"` comparisons still saw the raw key and
  // skipped it. The option was accepted and then silently dropped:
  //   `poisson fatal beertax, offset(pop)` -> b = -8861267.42121
  //   `poisson fatal beertax, offs(pop)`   -> b =        6.78117  (== no option)
  //   `poisson fatal beertax, exp(pop)`    -> runs; `exposure(pop)` rejects
  // A wrong number behind a right-looking command, with no diagnostic. Rewriting
  // the key HERE — before any handler compares it — is what closes that, because
  // it is the only point that is ordering-correct for every net regardless of
  // where in the handler its reject net happens to sit.
  //
  // AER scope-2 A1: the three passes themselves now live in ONE function,
  // resolveOptionKeyForCommand() (this file, immediately above dispatchImpl),
  // so `piet_native --dump-optvocab --resolve` can report the resolution this
  // loop performs instead of carrying a second copy of the rule. Nothing about
  // the rule moved with it: each pass was already independent per key, so one
  // per-key call produces the same rewrite the two whole-vector passes did.
  for (auto& kv : parsed.opts) {
    const OptKeyResolution optRes = resolveOptionKeyForCommand(cmd, kv.first);
    kv.first = optRes.resolved;
    if (optRes.exactOnly) kv.second.exactOnly = true;
  }

  // ------------------------------------------------------------------
  // 1a-bis. WS-B (extcompat, 2026-08-06) — THE community-name gate. One
  // point, immediately after name resolution, so every route into a command
  // passes it exactly once: a `capture`/`quietly`/`noisily` prefix strips and
  // re-enters dispatch() (Pre-0 above), and `bysort G: reghdfe ...` recurses
  // through dispatch() with the sub-command, so both are covered here without
  // a second check anywhere.
  //
  // ORDER MATTERS for gate ②. The setting is checked FIRST — one relaxed
  // atomic load — and the registry lookup happens only in the off case, which
  // is not the default. On the default path (and therefore in every gate,
  // wave, freeze and answerbook run) this costs one predictable branch per
  // line and touches no table.
  //
  // The message is a REJECT, not a notice: it is only ever produced on the
  // failure path, so no successful command's output can carry an attribution
  // string (LEGAL-DISTRIBUTION §2.7(c) — the display surface is what the
  // answerbook gates byte-compare). Code `unsupported_command` maps to Stata
  // r(199) via eval::coarseRc, so `capture reghdfe ...` sets _rc = 199.
  {
    dprof::Scope __z(dprof::kNameGate);
    if (!settings::extcompatEnabled()) {
      // BUG-105/WS-K: this gate reads cmdTyped, not cmd. resolveCanonical()
      // already exempts community names so the two are equal here today, but
      // the gate's question is literally "did the user type a community name",
      // and it must stay answerable from the typed token alone.
      if (const registry::CommunityName* cn = registry::findCommunityName(cmdTyped)) {
        blocks.push_back(ojson(eval::errorBlock("unsupported_command",
                                                registry::communityGateMessage(*cn), cmdTyped)));
        return blocks;
      }
    }
  }
  // Everything from here to whichever `return` fires is the handler zone; the
  // RAII scope closes on any of them.
  dprof::Scope __zHandler(dprof::kHandler);

  // AER-SCOPE1 bundle C: gtools/ftools translation. Placed AFTER the extcompat
  // community gate (so `set extcompat off` still refuses these names) and
  // BEFORE the by/bysort interception and the no-data guard (so the rewritten
  // head is what those see -- `gegen` must be `egen` by the time the by-prefix
  // branch compares sub-command names, and by the time kNoDataCommands is
  // consulted). One step for ten commands, rather than ten reimplementations.
  if (handlers::isGtoolsAlias(cmd)) {
    handlers::GtoolsTranslation tr = handlers::translateGtoolsAlias(parsed, cmd, session);
    if (!tr.error.empty()) {
      blocks.push_back(tr.error);
      return blocks;
    }
    parsed = tr.parsed;
    cmd = tr.canonical;  // cmdTyped keeps the user's spelling, for messages
  }

  // ------------------------------------------------------------------
  // 1b. M3b: bysort/by prefix interception — BEFORE classify()/the program
  // guard, mirroring dispatcher.py's step 1b (bysort.py's run_bysort_cmd).
  // `bysort VARLIST: CMD` / `by VARLIST: CMD`:
  //   - gen/replace (any expr, not just _n/_N) -> cobra eval --bygen with
  //     by()/sort() clauses (run_bysort_gen).
  //   - egen -> inject `, by(VARLIST)` into the sub-command and recurse
  //     through dispatch() (respecting an explicit by() already present).
  //   - anything else -> `bysort` sorts by VARLIST first (in place), `by`
  //     does not, then the sub-command is dispatched normally (recursion).
  // BS-017: bys/byso/bysor are legal Stata abbreviations of `bysort` (`by`
  // has no shorter abbreviation of its own) — normalise to "bysort" before
  // calling parseBysortPrefix so its existing cmdLower=="bysort" checks
  // (sortFirst included) need no changes of their own.
  if (cmd == "bysort" || cmd == "by" || cmd == "bys" || cmd == "byso" || cmd == "bysor") {
    // BUG-535: `by KEYS, sort: CMD` is Stata's long spelling of `bysort KEYS:
    // CMD` (w2_bykeep.log arm by_sortopt, rc 0). The prefix parser below has no
    // notion of prefix options — the comma sent `sort:` into the option list and
    // the line died "no subcommand found after colon" — so the one option is
    // rewritten here as `bysort KEYS: CMD` and the rewritten line is
    // re-TOKENIZED in place, not re-dispatched. parsed.raw is already
    // macro-expanded; dispatch() would expand it a second time (an expanded
    // value that itself holds `$name` or a backtick would be expanded again).
    // No dispatcher entry point sits below macro expansion (the egen and xi:
    // re-dispatches go through dispatch()/dispatchImpl() too), and this branch
    // reads nothing of the line but its raw text and varlist, so a local
    // re-tokenize is the whole re-entry. Any other prefix option keeps the old
    // path.
    std::string bysortCmdNorm = (cmd == "by") ? cmd : std::string("bysort");
    const eval::ParsedCommand* byParsed = &parsed;
    eval::ParsedCommand sortOptParsed;
    {
      const std::string& raw = parsed.raw;
      const size_t colon = raw.find(':');
      const size_t sp = raw.find_first_of(" \t");
      if (colon != std::string::npos && sp != std::string::npos && sp < colon) {
        auto [keysPart, prefixOpts] = splitTopLevelComma(raw.substr(sp, colon - sp));
        if (prefixOpts.has_value() && lower(trimWs(*prefixOpts)) == "sort" &&
            !trimWs(keysPart).empty()) {
          try {
            sortOptParsed = eval::tokenize("bysort " + trimWs(keysPart) + ": " +
                                           trimWs(raw.substr(colon + 1)));
          } catch (const eval::TokenizerError& te) {
            blocks.push_back(ojson(te.block));
            return blocks;
          }
          byParsed = &sortOptParsed;
          bysortCmdNorm = "bysort";
        }
      }
    }
    BysortPrefix bp = parseBysortPrefix(bysortCmdNorm, byParsed->varlist, byParsed->raw);
    if (bp.subCmd.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "bysort/by: no subcommand found after colon in `" + byParsed->raw + "`")));
      return blocks;
    }
    // BUG-532: abbreviated / wildcard keys resolve before any branch reads them.
    for (std::vector<std::string>* names : {&bp.byVars, &bp.sortVars}) {
      if (std::optional<ojson> err = resolveByKeyNames(session, *names)) {
        blocks.push_back(*err);
        return blocks;
      }
    }
    // BUG-105 (WS-K) — the by/bysort sibling of the resolution point above, so
    // `bysort region: summ price` reaches the same per-group implementation
    // `bysort region: summarize price` does. The typed spelling is kept for the
    // reject messages further down, which quote what the user wrote.
    const std::string subCmdTyped = lower(rstripColon(bp.subCmd));
    std::string subCmdLower = registry::resolveCanonical(subCmdTyped);
    // Same fold on the by-prefix path: `bysort g: gegen x = mean(y)` has to
    // reach the egen by()-injection branch below, which compares this name. The
    // recursive dispatch of the inner command then passes the main translation
    // point above, so options are still translated exactly once.
    subCmdLower = handlers::gtoolsCanonicalName(subCmdLower);

    if (subCmdLower == "gen" || subCmdLower == "generate" || subCmdLower == "g" ||
        subCmdLower == "replace") {
      bool isReplace = (subCmdLower == "replace");
      std::string subJoined = bp.subRaw;
      // strip the sub-command token itself (first word) off subRaw.
      {
        size_t sp = subJoined.find_first_of(" \t");
        subJoined = (sp == std::string::npos) ? std::string() : trimWs(subJoined.substr(sp));
      }
      // extract trailing `if COND` (first occurrence outside any quoted
      // string — BUG-115 class sweep: `bysort v: gen x = "if you see this"`
      // must not have its quoted literal mis-split as an `if` qualifier).
      std::optional<std::string> ifExpr;
      static const std::regex reIf(R"(\s+if\s+(.+)$)", std::regex::icase);
      std::smatch ifm;
      if (eval::searchUnquoted(subJoined, reIf, ifm)) {
        ifExpr = trimWs(ifm.str(1));
        subJoined = trimWs(subJoined.substr(0, static_cast<size_t>(ifm.position(0))));
      }
      // storage-type prefix strip (gen byte idx = _n).
      StorageTypeInfo info = stripStorageType(subJoined);
      if (info.stringTyped) {
        blocks.push_back(ojson(eval::unsupportedCommand(
            "bysort gen (string-typed)", std::nullopt, std::nullopt, "piet_gap",
            eval::UnsupportedCommandKind::kNotImplemented)));
        return blocks;
      }
      // BUG-112(1) PIET-half: `bysort v: gen NEW = ...` is still the `generate`
      // verb, so it takes the identical exists-guard and message. The `replace`
      // sub-command sharing this block keeps overwriting, which is its job.
      if (!isReplace) {
        if (auto genExists = genTargetExistsError(session, genTargetName(info.expr))) {
          blocks.push_back(*genExists);
          return blocks;
        }
      }
      // BS-002: an explicit parenthetical sort key (if present) overrides
      // the sortFirst-driven default sort() clause — see makeBygenExpr's
      // by()/sort() split (egen.cpp) and BysortPrefix::sortVars above.
      std::optional<std::vector<std::string>> sortVarsOpt;
      if (!bp.sortVars.empty()) sortVarsOpt = bp.sortVars;
      // BUG-532: under `bysort`, runBysortGen sorts the rows in place by KEYS
      // SORT first (Stata's order and marker), then computes.
      // BUG-471 T2: the same resolver as the plain arms (bysort_gen_untyped :686,
      // bysort_gen_double :700); a by-prefixed replace keeps the variable's type.
      const std::string byStoreType =
          isReplace ? storage::resolveReplaceType(session, genTargetName(info.expr))
                    : storage::resolveCreateType(session, info.typeToken);
      ojson mutBlock = handlers::runBysortGen(session, info.expr, bp.byVars, ifExpr, bp.sortFirst,
                                              isReplace, /*inRange=*/std::nullopt, sortVarsOpt,
                                              byStoreType);
      blocks.push_back(mutBlock);
      // BUG-250: `bysort id: gen double lagy = ...` is still the generate verb
      // (p2perf_bench.do:207-208 is exactly this shape). replace declares no
      // type in Stata, so only the gen half records.
      if (!isReplace) {
        recordDeclaredStataType(session, genTargetName(info.expr),
                                withSessionDefaultType(session, info), blocks.back());
      }
      return blocks;
    }

    if (subCmdLower == "egen" && !bp.byVars.empty()) {
      auto [head, opts] = splitTopLevelComma(bp.subRaw);
      std::string byClause = "by(" + joinSpace(bp.byVars) + ")";
      std::string inner;
      static const std::regex reByOpt(R"(\bby\s*\()", std::regex::icase);
      if (!opts.has_value()) {
        inner = trimWs(head) + ", " + byClause;
      } else if (std::regex_search(*opts, reByOpt)) {
        inner = bp.subRaw;  // explicit by() wins
      } else {
        inner = trimWs(head) + ", " + trimWs(*opts) + " " + byClause;
      }
      // BUG-532: Stata's `bysort KEYS (SORT): egen` sorts the data in place by
      // KEYS SORT first and leaves them sorted (see runBysortGen); egen's own
      // by() then computes on those rows.
      if (bp.sortFirst) {
        std::vector<std::string> sortKeys = bp.byVars;
        sortKeys.insert(sortKeys.end(), bp.sortVars.begin(), bp.sortVars.end());
        if (std::optional<ojson> sortErr = handlers::sortInStataOrder(session, sortKeys)) {
          blocks.push_back(*sortErr);
          return blocks;
        }
      }
      ojson innerBlocks = dispatch(inner, session);
      for (const auto& b : innerBlocks) {
        if (!(b.is_object() && b.value("type", "") == "echo")) blocks.push_back(b);
      }
      return blocks;
    }

    // AER-SCOPE1 bundle I (UTIL): `bysort id: carryforward y, gen(f)` and
    // `bysort id: tsspell x` reach their handlers the way egen does -- the
    // by-variables are injected as a `by()` option and the line is
    // re-dispatched. Both handlers are group-aware; the prefix supplies the
    // grouping, not a loop.
    //
    // It sorts for BOTH `bysort` and plain `by:`, which is the same choice the
    // `by: summarize` branch below makes and for a stronger reason. Real Stata
    // refuses a plain `by:` on unsorted data (r(5), "not sorted"); replicating
    // that needs a sortedness oracle PIET does not expose to this layer -- the
    // `sorted_by` claim lives in the .hbin header and has no accessor here. Of
    // the two ways to be wrong, sorting is the safe one PRECISELY BECAUSE these
    // two verbs are order-dependent: carryforward propagates the last
    // non-missing value forward and tsspell walks runs, so running them on an
    // unsorted frame produces a different, entirely plausible answer. Sorting
    // makes them right; refusing would only be more faithful.
    //
    // BUG-623: the sort is on the by-keys AND the parenthesised sort keys.
    // `bysort id (t): carryforward y, gen(f)` sorted by id alone, so the fill
    // walked each id in whatever order the rows happened to be in, not by t
    // (main, t = 4 3 2 1 with y set only at t = 1: f filled 1 row, Stata 4).
    // Stata's `bysort id (t):` sorts by id t, and a plain `by id (t):` needs
    // the data in that order.
    if ((subCmdLower == "carryforward" || subCmdLower == "tsspell") && !bp.byVars.empty()) {
      std::vector<std::string> cfSortKeys = bp.byVars;
      cfSortKeys.insert(cfSortKeys.end(), bp.sortVars.begin(), bp.sortVars.end());
      ojson sortBlocks = dispatch("sort " + joinSpace(cfSortKeys), session);
      for (const auto& b : sortBlocks) {
        if (b.is_object() && b.value("type", "") == "error") {
          blocks.push_back(b);
          return blocks;
        }
      }
      auto [head, opts] = splitTopLevelComma(bp.subRaw);
      std::string byClause = "by(" + joinSpace(bp.byVars) + ")";
      std::string inner;
      static const std::regex reByOpt2(R"(\bby\s*\()", std::regex::icase);
      if (!opts.has_value()) {
        inner = trimWs(head) + ", " + byClause;
      } else if (std::regex_search(*opts, reByOpt2)) {
        inner = bp.subRaw;  // explicit by() wins, same rule as egen
      } else {
        inner = trimWs(head) + ", " + trimWs(*opts) + " " + byClause;
      }
      ojson innerBlocks = dispatch(inner, session);
      for (const auto& b : innerBlocks) {
        if (!(b.is_object() && b.value("type", "") == "echo")) blocks.push_back(b);
      }
      return blocks;
    }

    // BUG-532: `by/bysort KEYS [(SORT)]: keep if COND` and `drop if COND` — the
    // only by-able forms of keep/drop ([D] drop). COND is evaluated per by-group
    // (group-local _n/_N, x[k], sum()); a missing COND counts as true. Oracle:
    // tests/eval/golden/aer_w2/w2_bykeep.log.
    //
    // Order is established HERE, the way Stata does it: `bysort` sorts the data
    // in place on KEYS++SORT in Stata's order (a string key by its bytes, not by
    // the engine's first-seen dictionary code — arm bys_p_str) and leaves it
    // sorted; plain `by` requires the rows to be in that order already and
    // refuses with r(5) otherwise, data untouched. For plain `by`, "in order" is
    // Stata's sortedby verdict as far as PIET can know it: the engine file's own
    // sort claim (engineSortClaim — arms by_p_after_bysgen, by_two_halfsorted),
    // else the rows themselves, measured under Stata's string order or the
    // engine's (keysAlreadyGrouped; the latter is the order PIET's own `sort` on
    // a string key leaves). The level-string, first-key-only check the summarize
    // branch below uses would call a string key PIET just sorted "not sorted".
    if ((subCmdLower == "keep" || subCmdLower == "drop") && !bp.byVars.empty()) {
      eval::ParsedCommand subParsed;
      try {
        subParsed = eval::tokenize(bp.subRaw);
      } catch (const eval::TokenizerError& te) {
        blocks.push_back(ojson(te.block));
        return blocks;
      }
      if (auto err = eval::rejectUnknownOptions(subParsed.opts, {})) {
        blocks.push_back(ojson(*err));
        return blocks;
      }
      if (subParsed.inRange.has_value()) {  // arms by_keep_in / by_keep_if_in
        blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                                "'in' may not be combined with 'by'",
                                                "in " + *subParsed.inRange, std::nullopt,
                                                std::nullopt, "r(190)")));
        return blocks;
      }
      // `by g: keep VARLIST` / `by g: drop VARLIST` is r(198) invalid syntax in
      // Stata (arms by_keep_varlist / by_drop_varlist): only the `if` form is
      // by-able.
      if (!subParsed.ifExpr.has_value() || trimWs(*subParsed.ifExpr).empty() ||
          !subParsed.varlist.empty()) {
        blocks.push_back(ojson(eval::errorBlock(
            "syntax_error",
            "invalid syntax — under `by:` only `" + subCmdLower + " if EXP` is allowed; `" +
                subCmdLower + " VARLIST` takes no by-prefix",
            subCmdTyped, std::nullopt, std::nullopt, "r(198)")));
        return blocks;
      }
      std::optional<ojson> rowErr;
      const std::string cond = substituteRowExprRefs(*subParsed.ifExpr, session, &rowErr);
      if (rowErr.has_value()) {
        blocks.push_back(*rowErr);
        return blocks;
      }
      if (!session.hasData()) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return blocks;
      }
      Datastore* baseDs = session.datastore();
      if (baseDs == nullptr) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return blocks;
      }
      std::vector<std::string> keys = bp.byVars;
      keys.insert(keys.end(), bp.sortVars.begin(), bp.sortVars.end());
      for (const auto& k : keys) {
        if (!baseDs->has(k)) {  // arms bys_nosuchvar / bys_p_nosuchkey: r(111)
          blocks.push_back(ojson(eval::engineError("variable " + k + " not found")));
          return blocks;
        }
      }
      // Stata orders the missing values of a numeric key `. < .a < ... < .z`
      // and groups each kind apart. The engine's gsort/by() collapse every
      // missing kind into one NaN (src/data/frame2.cpp Frame::gsort's BUG-272
      // comment), and so does keysAlreadyGrouped: a key holding an extended
      // missing could be grouped and ordered differently from Stata, so it is
      // refused by name before anything moves (the sort primitive itself
      // belongs to the engine lane).
      for (const auto& k : keys) {
        if (baseDs->dtype(k) != "f64") continue;
        const double* v = baseDs->col(k).f64();
        for (int64_t r = 0; r < baseDs->nRows(); ++r) {
          if (cecon::missing_kind(v[r]) > 0) {
            blocks.push_back(ojson(eval::errorBlock(
                "unsupported_command",
                "`" + std::string(bp.sortFirst ? "bysort" : "by") + " ...: " + subCmdLower +
                    " if` is not supported yet when a key variable holds extended missing "
                    "values (.a-.z): `" + k +
                    "` does, and this engine would order and group every missing kind as "
                    "one value.",
                std::nullopt, std::nullopt, std::nullopt, "piet_gap")));
            return blocks;
          }
        }
      }
      // `bysort` sorts in place on KEYS++SORT (runByKeepIf, through runSort),
      // and the marker becomes KEYS++SORT (w2e_probes.log bo_sort_prefix_noop:
      // a coarser `sort g` on data sorted by g x leaves "g"). Plain `by`
      // never sorts: it runs when Stata's sortedby marker --
      // the engine file's own claim, which every `bysort` now leaves on PIET's
      // physically sorted rows -- leads with KEYS++SORT (arms by_p_after_bysgen,
      // by_p_sorted), and is r(5) otherwise (by_p_wrongkey, by_two_halfsorted).
      // With no claim at all (a string key -- the engine mints none -- or data
      // from a claimless transform) the rows themselves are measured: the named
      // permissive residual, rc 0 where Stata may answer r(5).
      std::vector<std::string> sortOn;  // empty = the rows are in order already
      if (bp.sortFirst) {
        sortOn = keys;
      } else {
        bool inOrder = false;
        if (const std::vector<std::string> claim = engineSortClaim(*baseDs); !claim.empty()) {
          inOrder = keysLeadClaim(keys, claim);
        } else {
          inOrder = keysAlreadyGrouped(*baseDs, keys, /*stataStringOrder=*/true) ||
                    keysAlreadyGrouped(*baseDs, keys, /*stataStringOrder=*/false);
        }
        if (!inOrder) {  // arms by_unsorted / by_p_wrongkey / by_drop_unsorted / by_two_halfsorted
          blocks.push_back(ojson(eval::errorBlock(
              "not_sorted",
              "not sorted — `by " + joinSpace(bp.byVars) +
                  (bp.sortVars.empty() ? std::string() : " (" + joinSpace(bp.sortVars) + ")") +
                  ":` requires the data to already be sorted by " + joinSpace(keys) +
                  ". Use `bysort` to sort first, or run `sort " + joinSpace(keys) +
                  "` before this command.",
              std::nullopt, std::nullopt, std::nullopt, "r(5)")));
          return blocks;
        }
      }
      // baseDs is not used past this point: the handler may replace the head.
      blocks.push_back(ojson(handlers::runByKeepIf(session, cond, bp.byVars, sortOn,
                                                   /*drop=*/subCmdLower == "drop")));
      return blocks;
    }

    // BS-092 (BETA campaign, Phase G): `by:`/`bysort: summarize` — the ONE
    // OTHER subcommand (besides gen/replace/egen above) that gets a genuine
    // PIET-side per-group implementation this phase; the other 7
    // Stata-legal-but-unimplemented stems the fallback message below still
    // names (correlate/pwcorr, sdtest, ttest, tabstat, tabulate family,
    // count, list) stay OUT of this batch's scope (BS-107, unchanged).
    //
    // Real Stata's `by G: summarize X` sorts by G (`bysort` re-sorts in
    // place; plain `by` requires the data ALREADY sorted, erroring "not
    // sorted" otherwise) then re-runs `summarize` once per contiguous
    // distinct-G block, printing a `-> G = value` header ahead of each
    // group's own stats table (live-verified this stint, StataBE:
    // `bysort state: summarize beertax` -> "-> state = al" then the usual
    // Variable/Obs/Mean/... table). This sorts UNCONDITIONALLY for both
    // `by` and `bysort` (the only divergence from real Stata is a plain
    // `by:` on genuinely unsorted data, where Stata refuses to run at all —
    // silently sorting instead of replicating that reject is the
    // conservative choice: it always answers the CORRECTLY grouped
    // question rather than risking a wrong one) into a FRESH scratch copy
    // (piet::handlers::sortForGrouping) — summarize is read-only, so the
    // live session dataset's row order must never change as a side effect.
    // (BUG-532, 2026-09-26: that holds for a plain `by:` only. Stata's
    // `bysort` sorts the data in place, and so does this driver now, through
    // handlers::sortInStataOrder below; the copy is then skipped.)
    // BUG-105 abbreviation stopgap: `su` is matched here alongside `sum`
    // so `bysort G: su X` reaches this real per-group implementation. Left
    // out, it would fall through to the honest "by:/bysort: is only
    // implemented for gen/replace/egen/summarize" reject below — true of
    // the token, false of the command it abbreviates.
    // P4 wave: the same driver now also carries `regress`/`reg`. Everything from
    // the sort through the boundary scan and the per-group slice is command-
    // agnostic — only the option net and the one per-group call below differ —
    // so `regress` graduates by joining this condition rather than by growing a
    // second copy of the group machinery. (ROADMAP P4: "generic group driver".)
    // W:P5: `tabstat` joins on the same terms regress did in P4 — the sort,
    // the boundary scan and the per-group slice are command-agnostic, so the
    // only additions are one flag and one per-group call below. Real Stata
    // prints a `-> region = N` header then that group's own table, which is
    // exactly the shape this driver already emits.
    const bool byIsSummarize =
        (subCmdLower == "summarize" || subCmdLower == "sum" || subCmdLower == "su");
    const bool byIsRegress = (subCmdLower == "regress" || subCmdLower == "reg");
    const bool byIsTabstat = (subCmdLower == "tabstat");
    if (byIsSummarize || byIsRegress || byIsTabstat) {
      if (bp.byVars.empty()) {
        blocks.push_back(ojson(eval::engineError("bysort/by: no group variable(s) given")));
        return blocks;
      }
      eval::ParsedCommand subParsed;
      try {
        subParsed = eval::tokenize(bp.subRaw);
      } catch (const eval::TokenizerError& te) {
        blocks.push_back(ojson(te.block));
        return blocks;
      }
      bool detail = false;
      if (byIsSummarize) {
        for (const auto& [k, v] : subParsed.opts) {
          if ((k == "detail" || k == "d") && v.isBool && v.boolValue) detail = true;
        }
        // Mirrors the plain `summarize` arm's own option net (BS-097) exactly
        // — detail/d/meanonly only, anything else an honest r(198)-parity
        // reject naming the token.
        for (const auto& [k, v] : subParsed.opts) {
          (void)v;
          if (k == "detail" || k == "d" || k == "meanonly") continue;
          blocks.push_back(
              ojson(eval::engineError("option `" + k + "` not allowed", "unsupported_option")));
          return blocks;
        }
      }
      // regress has no option net here on purpose: runRegress owns its own
      // (vce/robust/cluster/level/noconstant/absorb…), and duplicating it would
      // be a second copy to drift out of sync. A bad option surfaces from the
      // per-group call with regress's own wording.
      // tabstat likewise: its own arm below owns the stat/statistics/columns
      // net, and this driver forwards the parsed options to the same handler.
      std::optional<std::string> byTabstatStats;
      if (byIsTabstat) {
        for (const auto& [k, v] : subParsed.opts) {
          if ((k == "stat" || k == "stats" || k == "statistics") && !v.isBool) {
            byTabstatStats = v.strValue;
          } else if (k == "columns" && !v.isBool) {
            // Presentation-only in Stata (which axis carries the statistics);
            // PIET renders one fixed layout, so accept and note rather than
            // reject a legal option or silently swallow it.
            continue;
          } else {
            blocks.push_back(
                ojson(eval::engineError("option `" + k + "` not allowed", "unsupported_option")));
            return blocks;
          }
        }
        // `by G: tabstat X, by(H)` is illegal in Stata (two grouping axes);
        // the by() option cannot reach here anyway — the prefix owns grouping.
      }
      if (!session.hasData()) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return blocks;
      }
      Datastore* baseDs = session.datastore();
      if (baseDs == nullptr) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return blocks;
      }
      for (const auto& bv : bp.byVars) {
        if (!baseDs->has(bv)) {
          blocks.push_back(ojson(eval::engineError("variable " + bv + " not found")));
          return blocks;
        }
      }

      // BUG-157 (P4 wave): plain `by` REQUIRES the data to already be sorted by
      // the by-variables; Stata refuses with r(5) "not sorted" otherwise, and
      // that is exactly what `bysort` exists to avoid. BS-092 sorted
      // unconditionally for both spellings and called the silent sort "the
      // conservative choice"; it is not — it answers a question the user's
      // program did not ask for and hides a real bug in their code. `bysort`
      // still sorts, unchanged.
      //
      // Sortedness is MEASURED here (adjacent-row comparison on the base data),
      // never inferred from a stored flag: a flag can be stale after any verb
      // that reorders rows, and a wrong "already sorted" answer would put rows
      // from two different groups into one fit.
      if (!bp.sortFirst) {
        std::string unsortedOn;
        const int64_t nBase = baseDs->nRows();
        for (const auto& bv : bp.byVars) {
          const cecon::Column& c = baseDs->col(bv);
          const bool isFac = baseDs->dtype(bv) != "f64";
          std::vector<std::string> levels;
          if (isFac) levels = baseDs->varinfo(bv).levels;
          auto keyAt = [&](int64_t r) -> std::string {
            if (!isFac) return std::string();
            int32_t code = c.codes()[r];
            return (code >= 0 && static_cast<size_t>(code) < levels.size())
                       ? levels[static_cast<size_t>(code)]
                       : std::string("\xff");  // missing sorts last, like Stata
          };
          for (int64_t r = 1; r < nBase; ++r) {
            bool decreasing;
            if (isFac) {
              decreasing = keyAt(r) < keyAt(r - 1);
            } else {
              // Stata orders missing AFTER every number, so a NaN following a
              // number is sorted, and a number following a NaN is not.
              double a = c.f64()[r - 1], b = c.f64()[r];
              bool an = std::isnan(a), bn = std::isnan(b);
              decreasing = (an && !bn) ? true : ((!an && !bn) ? (b < a) : false);
            }
            if (decreasing) { unsortedOn = bv; break; }
          }
          if (!unsortedOn.empty()) break;
          // Only the FIRST by-variable can be checked independently; a later one
          // need only be sorted within each block of the earlier ones. Checking
          // it globally would reject legitimately sorted data, so stop here —
          // this catches the filed defect (a `by:` on data never sorted at all)
          // without inventing a stricter rule than Stata's.
          break;
        }
        if (!unsortedOn.empty()) {
          blocks.push_back(ojson(eval::errorBlock(
              "not_sorted",
              "not sorted — `by " + joinSpace(bp.byVars) + ":` requires the data to already be "
              "sorted by " + joinSpace(bp.byVars) + " (`" + unsortedOn + "` is not in order). "
              "Use `bysort " + joinSpace(bp.byVars) + ":` to sort first, or run `sort " +
              joinSpace(bp.byVars) + "` before this command.",
              std::nullopt, std::nullopt, std::nullopt, "r(5)")));
          return blocks;
        }
      }

      // BUG-532: Stata's `bysort KEYS [(SORT)]:` sorts the data in place, and
      // leaves them marked sorted by KEYS++SORT, before it runs the command. So
      // does this driver (handlers::sortInStataOrder: a string key in byte order;
      // the engine moves no row when the marker already leads with KEYS++SORT).
      // It used to sort only a scratch copy, leaving PIET's rows where Stata's
      // were not.
      // The keys were resolved above (abbreviations, review of d82bdfa6); a
      // parenthesised key that names no variable is r(111), as in Stata.
      if (bp.sortFirst) {
        std::vector<std::string> sortKeys = bp.byVars;
        sortKeys.insert(sortKeys.end(), bp.sortVars.begin(), bp.sortVars.end());
        for (const auto& sv : bp.sortVars) {
          if (!baseDs->has(sv)) {
            blocks.push_back(ojson(eval::engineError("variable " + sv + " not found")));
            return blocks;
          }
        }
        if (std::optional<ojson> sortErr = handlers::sortInStataOrder(session, sortKeys)) {
          blocks.push_back(*sortErr);
          return blocks;
        }
        baseDs = session.datastore();
        if (baseDs == nullptr) {
          blocks.push_back(ojson(eval::noDataLoaded()));
          return blocks;
        }
      }

      // P2-PERF ARC-C C1-③: skip the sort when it is PROVABLY the identity.
      //
      // sortForGrouping exists to make same-group rows physically contiguous,
      // and it pays a full re-sort plus a full --out write of the whole dataset
      // to do it — every time, including the overwhelmingly common case where
      // the user has just run `sort region` themselves. It cannot be skipped on
      // the strength of the `by`-spelling sortedness check a few lines up: that
      // check answers a DIFFERENT question (it compares factor LEVEL STRINGS
      // and inspects only the first by-variable, because its job is to reject a
      // user error, not to certify a permutation), and it does not run at all
      // under `bysort`.
      //
      // So certify it against gsort's OWN key semantics instead (the
      // keysAlreadyGrouped helper above, factored out for BUG-532). If the key
      // tuple is already non-decreasing under exactly that order, the
      // permutation gsort would return is the identity, so `take_rows` would
      // hand back a byte-identical frame and the whole round trip is dead work.
      // Any doubt at all — a key that dips — falls through to the sort, so
      // this can only ever remove a no-op.
      const bool alreadyGrouped = keysAlreadyGrouped(*baseDs, bp.byVars);

      std::string sortedPath;
      std::optional<Datastore> sortedOwned;
      Datastore* sortedPtr = nullptr;
      if (alreadyGrouped) {
        sortedPath = baseDs->path();
        sortedPtr = baseDs;
      } else {
        try {
          sortedPath = handlers::sortForGrouping(session, baseDs->path(), bp.byVars);
        } catch (const CobraError& ce) {
          blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
          return blocks;
        }
        sortedOwned.emplace(sortedPath);
        sortedPtr = &*sortedOwned;
      }
      Datastore& sortedDs = *sortedPtr;
      int64_t nRowsSorted = sortedDs.nRows();
      if (nRowsSorted == 0) {
        blocks.push_back(ojson(eval::engineError("no observations")));
        return blocks;
      }

      // Raw column views over the SORTED copy for group-boundary detection
      // (adjacent-row key comparison) and header-value formatting — mirrors
      // the raw cecon::Column access pattern used throughout this file
      // (e.g. the describe/summarize-fallback block a few hundred lines up).
      struct ByColView {
        std::string name;
        bool isFactor = false;
        const double* f64 = nullptr;
        const int32_t* codes = nullptr;
        // Owned COPY, not a pointer into Datastore::varinfo()'s return —
        // that accessor returns VarInfo BY VALUE (datastore.h), so taking
        // `&varinfo(...).levels` would dangle the instant the temporary
        // VarInfo is destroyed at the end of the full-expression.
        std::vector<std::string> levels;
      };
      std::vector<ByColView> byCols;
      for (const auto& bv : bp.byVars) {
        ByColView v;
        v.name = bv;
        v.isFactor = sortedDs.dtype(bv) != "f64";
        const cecon::Column& c = sortedDs.col(bv);
        if (v.isFactor) {
          v.codes = c.codes();
          v.levels = sortedDs.varinfo(bv).levels;
        } else {
          v.f64 = c.f64();
        }
        byCols.push_back(std::move(v));
      }
      auto rowKeyEqual = [&](int64_t a, int64_t b) {
        for (const auto& v : byCols) {
          if (v.isFactor) {
            if (v.codes[a] != v.codes[b]) return false;
          } else {
            double x = v.f64[a], y = v.f64[b];
            bool xn = std::isnan(x), yn = std::isnan(y);
            if (xn != yn) return false;
            if (!xn && x != y) return false;
          }
        }
        return true;
      };
      // P4 wave correction. BS-092 rendered numeric group values with
      // eval::pythonFloatRepr ("1.0"), disclosed at the time as a cosmetic
      // simplification on the grounds that there was "no prior art to match".
      // There is now: the P4 answerbook's real-Stata capture prints
      // `-> region = 1`, not `-> region = 1.0`
      // (tests/kitchensink/pangram/waves/_oracle/p4_stata.log). An
      // integral-valued double therefore renders WITHOUT the trailing ".0",
      // exactly as tabulate.cpp's tableF64Label already does for the same
      // reason; non-integral values keep their decimals.
      auto formatGroupHeader = [&](int64_t row) {
        std::string out;
        for (size_t i = 0; i < byCols.size(); ++i) {
          if (i) out += " ";
          const ByColView& v = byCols[i];
          out += v.name + " = ";
          if (v.isFactor) {
            int32_t code = v.codes[row];
            out += (code >= 0 && static_cast<size_t>(code) < v.levels.size())
                       ? v.levels[static_cast<size_t>(code)]
                       : ".";
          } else {
            double x = v.f64[row];
            if (std::isnan(x)) {
              out += ".";
            } else if (x == std::floor(x) && std::fabs(x) < 1e15) {
              char buf[32];
              std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(x));
              out += buf;
            } else {
              out += eval::pythonFloatRepr(x);
            }
          }
        }
        return out;
      };

      // P2-PERF ARC-C C1-①: the row-index column every slice below filters on
      // is a property of `sortedPath`, not of the group — build it once.
      handlers::RowIndexCache rowIdx;
      try {
        rowIdx = handlers::buildRowIndexCache(session, sortedPath);
      } catch (const CobraError& ce) {
        blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
        return blocks;
      }

      // P2-PERF ARC-C C1-④: every read the sub-command makes below is against a
      // per-group SLICE, never against the session's head. Routing those to the
      // resident plane evicts the head and resyncs it once per group; say so
      // once, here, for the whole loop.
      SessionState::StatelessRead statelessGroupReads(session);

      int64_t g0 = 0;
      while (g0 < nRowsSorted) {
        int64_t g1 = g0 + 1;
        while (g1 < nRowsSorted && rowKeyEqual(g0, g1)) ++g1;

        ojson header;
        header["type"] = "display";
        header["text"] = "-> " + formatGroupHeader(g0);
        blocks.push_back(header);

        // Slice this group's own contiguous [g0+1, g1] row range (1-based,
        // inclusive) out of the sorted copy — reusing BS-090's shared
        // row-filter infra (piet::handlers::applyIfAndInFilter) purely for
        // its mechanical row-range materialization, ANDed with any `if`
        // the sub-command itself carried (`by G: summarize X if COND`
        // restricts EACH group's own sample by COND too, same as real
        // Stata — group membership itself is unaffected since it was
        // already fixed by the sort/boundary-scan above).
        std::string rangeStr = std::to_string(g0 + 1) + "/" + std::to_string(g1);
        handlers::InRangeFilterResult fr;
        try {
          fr = handlers::applyIfAndInFilter(session, sortedPath, subParsed.ifExpr, rangeStr,
                                            /*complement=*/false, &rowIdx);
        } catch (const CobraError& ce) {
          blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
          return blocks;
        }
        if (!fr.ok) {
          blocks.push_back(ojson(handlers::inRangeRejectError(fr, rangeStr, "summarize")));
          return blocks;
        }
        if (byIsSummarize) {
          Datastore groupDs(fr.dataPath);
          std::optional<std::vector<std::string>> vl;
          if (!subParsed.varlist.empty()) vl = subParsed.varlist;
          blocks.push_back(ojson(handlers::summarizeCoreStats(groupDs, vl, detail, std::nullopt)));
        } else if (byIsTabstat) {
          // The slice IS this group's sample (and already carries the
          // sub-command's own `if`), so pass no ifExpr — re-filtering here
          // would re-apply it against the slice.
          blocks.push_back(ojson(handlers::runTabstatOnData(
              subParsed.varlist, byTabstatStats, session, fr.dataPath)));
        } else {
          // The slice already carries this group's rows AND the sub-command's
          // own `if`, so runRegress must NOT re-filter it — hence the override.
          // Stata leaves the LAST group's fit in e()/`_b[]` when the loop ends,
          // which falls out of calling the ordinary handler once per group.
          blocks.push_back(
              ojson(handlers::runRegress(subParsed, session, subCmdLower, &fr.dataPath)));
        }

        g0 = g1;
      }
      return blocks;
    }

    // BUG-041: any OTHER subcommand under `by:`/`bysort:` used to sort-then-
    // dispatch-once — a Gate-(1) silent-drop (the sub-command runs ONCE
    // over the whole, optionally re-sorted, dataset, not once per by-group,
    // so e.g. `bysort grp: summarize wage` quietly answered the SAME
    // question as unqualified `summarize wage` instead of producing 5
    // per-group tables). True by-group dispatch (genuinely repeating the
    // sub-command once per distinct by-value and stitching results
    // together) is real feature work, queued for unfreeze — this
    // stabilization fix is the other honest half of Gate (1): raise an
    // explicit error naming the qualifier instead of silently flattening
    // it. Only gen/replace/egen/summarize (handled above — summarize added
    // BS-092, BETA Phase G) have a genuine native by-group implementation
    // today; every other command reaching here falls back to this honest
    // rejection, matching the qualifier-matrix sweep's expectation
    // (REJECTED, not SILENT_DROP) across the board rather than an ad hoc
    // per-command allowlist.
    {
      std::string prefixWord = bp.sortFirst ? "bysort" : "by";
      std::string byVarsJoined = joinSpace(bp.byVars);
      // A3 (BETA campaign, BS-096): distinguish two structurally different
      // reasons a subcommand lands in this fallback — audit G3 finding #10
      // (SCOPE.md BS-096) confirmed today's single message ("not supported
      // yet") is actively misleading for 4 stems (codebook/describe/
      // levelsof/table) that real Stata itself refuses to combine with
      // by:/bysort: at all (CONFIRMED live: StataBE answers "may not be
      // combined with by" / "prefix by: not allowed", r(190)) — those will
      // NEVER be supported here no matter how much by-group dispatch work
      // lands, unlike the other 7 Stata-legal-but-not-yet-implemented stems
      // (correlate/pwcorr, sdtest, ttest, tabstat, tabulate family, count,
      // list — queued at BS-107; summarize graduated out of this list this
      // phase, BS-092). registry's byIllegalInStata field carries that
      // distinction (a lookup, not a second hardcoded list); anything NOT
      // found in the registry (e.g. a genuinely unrecognized token) or
      // found-but-not-flagged keeps the ORIGINAL "not supported yet"
      // wording unchanged.
      const registry::CommandSpec* subSpec = registry::findByName(subCmdLower);
      std::string msg;
      // BUG-105/WS-K: the LOOKUP uses the canonical spelling (so an
      // abbreviation gets the right verdict), the MESSAGE quotes what the user
      // typed (so today's bytes are unchanged for every non-abbreviated line).
      if (subSpec && subSpec->byIllegalInStata) {
        msg = "`" + prefixWord + " " + byVarsJoined + ": " + subCmdTyped +
              "` is not allowed — `" + subCmdTyped + "` may not be combined with a `by`/`bysort` "
              "prefix. This is a structural restriction in Stata itself, not a PIET version "
              "limitation, and will not become available in a later milestone. Remove the `" +
              prefixWord + "` prefix and run `" + subCmdTyped + "` on its own.";
      } else {
        msg = "`" + prefixWord + " " + byVarsJoined + ": " + subCmdTyped +
              "` is not supported yet — `by:`/`bysort:` grouped dispatch is only "
              "implemented for `gen`/`replace`/`egen`/`summarize`/`regress`/`tabstat`/`keep if`/`drop if` "
              "(and `carryforward`/`tsspell`) in this version of PIET. Run `" +
              subCmdTyped + "` separately for each group instead.";
      }
      blocks.push_back(ojson(eval::errorBlock("unsupported_command", msg, std::nullopt, std::nullopt,
                                              std::nullopt, "piet_gap")));
      return blocks;
    }
  }

  // ------------------------------------------------------------------
  // 1c. U5 (S6): user-program invocation — BEFORE the data guard (a program
  // may itself `use`/`sysuse` before touching data). Uses the tokenized
  // parsed.cmd (ORIGINAL case) for case-insensitive program-name lookup,
  // mirroring dispatcher.py's `_find_program(session, parsed.cmd)`.
  {
    std::optional<std::string> progMatch = program::findProgram(session, parsed.cmd);
    if (progMatch.has_value()) {
      const ProgramDef& progDef = session.programs.at(*progMatch);
      // Argument string: everything after the program-name token in the
      // macro-expanded line (cmdForDispatch), mirroring
      // raw_cmd_for_dispatch[find(parsed.cmd)+len:].strip().
      std::string afterCmd;
      size_t pos = cmdForDispatch.find(parsed.cmd);
      if (pos != std::string::npos) {
        afterCmd = trimWs(cmdForDispatch.substr(pos + parsed.cmd.size()));
      }
      ojson progBlocks;
      try {
        progBlocks = program::runProgram(progDef, afterCmd, session, dispatch);
      } catch (const std::exception& exc) {
        return ojson::array({echo, eval::engineError("program " + parsed.cmd + ": " + exc.what())});
      }
      ojson out = ojson::array({echo});
      for (const auto& b : progBlocks) out.push_back(b);
      return out;
    }
  }

  // ------------------------------------------------------------------
  // 2. Guard: data must be loaded for most commands (dispatcher.py §2's
  // allow-list). M3a widens the M2 slice's `use`-only list to every
  // data-free command this milestone's handler surface actually routes
  // (xtset/tsset/display/di/scalar/label/timer/cd/pwd/preserve/restore
  // /tempfile/tempname); the FULL oracle allow-list (sysuse/webuse/clear/
  // set/import/version/compress/graph/eststo/…) stays M3b until those
  // commands themselves are ported — listing a command here that has no
  // handler arm below would silently bypass the no-data guard with nothing
  // to route it to, so this set is intentionally scoped to M3a's surface.
  static const std::set<std::string> kNoDataCommands = {
      "use",       "sysuse", "xtset",     "tsset",   "display", "di",     "cd",
      "pwd",       "timer",  "label",     "preserve", "restore", "tempfile",
      "tempname",  "scalar",
      // M3b: foundational commands that must work with zero data loaded
      // (clear/set-obs are the `clear -> set obs N -> gen` data-construction
      // idiom's first two steps; version/compress/set are pure no-ops;
      // egen/bysort/postfile/label define et al. can run before `use` too).
      "clear",     "set",    "version",   "compress", "exit",   "egen",
      "bysort",    "by",     "postfile",  "post",     "postclose",
      // BUG-028 + U7/S9 offline shims: `log using`/`log close|off|on`, plus
      // which/ssc/confirm/mkdir/erase, must all work before any `use`
      // (analysis.do's line 52-108 preamble opens a log, checks/installs
      // packages, and confirms the input file exists, all before `use`).
      // BUG-180 (P2-PREP): "rmdir" joins its mkdir/erase neighbours here —
      // scratch-directory teardown must work before any `use`, exactly like
      // the setup half.
      "log",       "which",  "ssc",       "confirm",  "mkdir",  "erase",  "rm",  "rmdir",
      // M4a: stored-estimates commands operate on session state (the
      // estimates store / lastModel), not the live dataset (dispatcher.py
      // §2's own no-data allow-list: "eststo", "est", "estimates",
      // "marginsplot", "graph", "coefplot"). "graph" itself (export/save/
      // combine/drop sub-commands read session.last_graph, not live data;
      // `graph twoway`/plot-type forms DO need data — the graph handler
      // arm re-checks hasData() for those internally, matching
      // dispatcher.py's own `if ds is None: return no_data_loaded()` INSIDE
      // the "graph" handler body, not at this outer guard).
      "eststo",    "est",    "estimates", "marginsplot", "coefplot", "graph",
      // Mission 5 (cosmetic no_data_loaded divergence, native-paths
      // investigation): real Stata's `describe`/`desc` with no dataset in
      // memory answers an EMPTY description (0 vars, 0 obs), not an error.
      // Deliberately does NOT include "codebook" — real Stata's codebook
      // needs actual variable statistics and errors with no data, so it
      // stays on the outer no-data guard, unaffected. The "describe"/"desc"
      // arm below special-cases !session.hasData() itself rather than
      // calling handlers::runDescribe() (handlers.cpp, out of this
      // workstream's manifest), whose own internal guard still returns
      // noDataLoaded() unconditionally.
      // BUG-105 abbreviation stopgap: `d` is listed here for the same
      // reason `desc` is — an alias that routes to the describe arm but
      // dies on the outer no-data guard would answer no_data_loaded where
      // its own canonical answers an empty description, i.e. it would be
      // an alias in name only.
      "describe",  "desc",   "d",
      // P1 wave: `import delimited|excel` LOADS a dataset, so it must run with
      // nothing in memory — the same reason `use`/`sysuse` are here. (`export`
      // genuinely needs data and stays on the outer guard.)
      "import",
      // BUG-343: `file open|write|read|close` is text I/O against the filesystem
      // and never touches the dataset. Registering the row without listing it
      // here would have made every `file` line answer "No dataset is loaded" —
      // the same wrong-class error BUG-342's second layer produced, and the
      // reason a registry row is never the whole of adding a command.
      "file",
      // AER-SCOPE1 bundle D (ENV): every environment verb runs with nothing in
      // memory -- most of them ARE the first line of the do-file, before any
      // `use`. Omitting a row here is BUG-343 exactly: the registry row and the
      // arm are both correct and the command still answers "No dataset is
      // loaded". testAerCampaignRatchets() probes this axis directly.
      "shell",   "winexec", "type",    "dir",      "ls",      "net",
      "ado",     "adopath", "sysdir",  "findfile", "sleep",   "pause",
      "cls",     "gr_edit", "error",   "bcal",     "twoway__histogram_gen",
      "frames",
      // AER-SCOPE1 bundle B: `return`, `svmat` and `matlist` read the session's
      // r()/matrix stores, never the dataset -- `svmat A` on an empty session is
      // how Stata builds a dataset FROM a matrix. `mkmat` is deliberately absent
      // for the mirror reason: it reads variables, so no data is a real error.
      "return",  "svmat",   "matlist",
      // ml-getmata T4: `getmata` reads the Mata store, and on an empty session
      // it is how the dataset comes into existence (getmata.ado: _N==0 & k==0
      // implies force; oracle arm getmata_nodata).
      "getmata",
      // `matrix`/`mat` join them: a matrix is session state, not data. Stata
      // accepts `matrix A = (1,2\\3,4)` with nothing in memory, and `svmat A`
      // on an empty session is the documented way to build a dataset FROM a
      // matrix -- refusing the definition made that whole path unreachable.
      "matrix",  "mat",
      // `tset` is deliberately ABSENT: like the `tsset`/`xtset` it re-spells, it
      // validates its variables against the data, so running it with nothing in
      // memory is a genuine no-data error rather than a route that was blocked.
      // AER-SCOPE1 bundle E (TABLES): the table verbs that render STORED
      // ESTIMATES or a session MATRIX and write a file. `eststo`/`esttab` are
      // already above for the same reason. The question this list answers is
      // "does this verb READ THE DATA?" (bundle D's lesson), and for these the
      // answer is no -- an estimate outlives the `clear` that follows it, so
      // `regress ... ; clear ; estout using t.tex` is a real corpus shape.
      "estadd",  "estout",   "outreg2",  "frmttable", "mat2txt", "erepost",
      "regsave", "parmest",
      // texdoc holds an open FILE; `tex` writes a literal line into it. Neither
      // has anything to do with the dataset, and `texdoc init` is routinely the
      // first line of the do-file.
      "texdoc",  "tex",
      // Deliberately ABSENT, all three for the same reason `tset` is: they read
      // the dataset. `estpost summarize` summarises variables, `texsave` renders
      // the dataset as a table, and `tabout` cross-tabulates it -- with nothing
      // in memory each is a genuine no-data error, not a blocked route.
      // AER-SCOPE1 bundle F (RESHAPE): `input` and `drawnorm` CREATE the data
      // they operate on, exactly like `use` -- `clear` then `input x y` is the
      // documented way to type a dataset in, and `drawnorm u v, n(50) clear` is
      // how a simulation starts. Every OTHER verb in bundle F reshapes or
      // samples an existing frame and is deliberately absent from this list.
      "input",   "drawnorm",
      // AER-SCOPE1 bundle G (IO-TEXT): the three readers LOAD data, and the
      // file/environment verbs never touch the dataset at all -- `copy`,
      // `filefilter` and `putexcel set` all routinely appear in a preamble
      // before any `use`. Deliberately ABSENT: `outsheet`, `outfile` and
      // `saveold`, which WRITE the frame out, so running them with nothing in
      // memory is a genuine no-data error rather than a blocked route.
      "insheet", "infile",   "infix",    "putexcel", "filefilter", "copy",
      "unzipfile", "zipfile", "odbc",    "spshape2dta",
      // AER-SCOPE1 bundle I (UTIL): `filelist` builds its dataset FROM the
      // filesystem, so like `use` it must run with nothing in memory. Every
      // other verb in bundle I reads or rewrites an existing frame.
      "filelist",
      // AER-SCOPE1 bundle J (GRAPH-X): `grstyle` touches nothing at all, and
      // `grc1leg2`/`event_plot` read named graphs and stored estimates rather
      // than the frame -- all three routinely appear before any `use`.
      // `geodist`, `geonear` and `cdfplot` read the data and stay on the guard.
      "grstyle", "grc1leg2", "event_plot",
      // AER-SCOPE2 bundle K1: `beep`/`window` touch nothing at all, `grc1leg`
      // reads named graphs like its grc1leg2 sibling above, and `parallel`'s
      // initialize/clean subcommands are process bookkeeping. All four appear in
      // do-file preambles before any `use`.
      "beep", "window", "grc1leg",
      // AER scope 2 K1c: `parallel initialize`/`setclusters`/`clean` are
      // process bookkeeping and appear in a driver's preamble, before any
      // `use`. The `do` forms below them load their own data (or run with
      // none), so the whole name is on the guard.
      "parallel",
  };
  bool needsNoData = kNoDataCommands.count(cmd) != 0;
  if (!session.hasData() && !needsNoData) {
    blocks.push_back(ojson(eval::noDataLoaded()));
    return blocks;
  }

  // ------------------------------------------------------------------
  // 2b. BUG-010 residue — generalized trailing-token guard. Most handler
  // arms below read only the specific tokens they expect off
  // parsed.varlist and silently ignore anything left over, e.g.
  // `count gen zzz9 = 1` tokenizes to cmd="count",
  // varlist=["gen","zzz9","=","1"]; runCount only ever consults
  // parsed.ifExpr, so the entire "gen zzz9 = 1" tail vanishes and the line
  // silently degrades to a bare `count` (960, not the intended `gen`).
  // Real Stata rejects this at parse time. xtset/tsset already got
  // bespoke arm-local guards for this (see above); rather than writing N
  // more bespoke blocks (or a full parser rewrite that would validate
  // every arm's grammar), this is one small allow-list of the commands
  // whose OWN Stata grammar takes zero or one fixed token — so ANY
  // leftover varlist token is unambiguously a mistake, not a legitimate
  // variable name. Kept deliberately narrow (the "worst offenders" swept
  // from the M3a/M3b arms above): commands with genuinely variable-arity
  // varlists (describe, summarize, regress, list, sort, ...) are correctly
  // left alone since a trailing token there IS the next variable name.
  {
    struct FixedArity {
      size_t maxTokens;  // max allowed parsed.varlist.size() for this cmd
      const char* usage;
    };
    static const std::map<std::string, FixedArity> kFixedArityCommands = {
        {"count", {0, "count [if] [in] — takes no variable list"}},
        {"pwd", {0, "pwd — takes no arguments"}},
        {"clear", {1, "clear [all]"}},
        {"preserve", {0, "preserve — takes no arguments"}},
        {"restore", {0, "restore — takes no arguments"}},
        {"exit", {1, "exit [exit-code]"}},
        {"sysuse", {1, "sysuse DATASET | sysuse dir"}},
    };
    auto arityIt = kFixedArityCommands.find(cmd);
    if (arityIt != kFixedArityCommands.end() && parsed.varlist.size() > arityIt->second.maxTokens) {
      std::vector<std::string> extra(
          parsed.varlist.begin() + static_cast<long>(arityIt->second.maxTokens), parsed.varlist.end());
      blocks.push_back(ojson(eval::engineError(
          cmdTyped + ": unexpected extra tokens after `" + arityIt->second.usage +
              "`: " + joinVarlist(extra) + ". Put each command on its own line.",
          "syntax_error")));
      return blocks;
    }
  }

  // ------------------------------------------------------------------
  // 3. Classify + dispatch — the M2 vertical slice's routing table.
  // TODO(M3) EXTENSION POINT: add new `if (cmd == "...")` arms here (mirroring
  // classify()'s Dispatch objects) without touching steps 0a/0b/1/2 above —
  // that pipeline is the frozen step-0a/0b contract this milestone's
  // dispatcher skeleton was built to hold stable for M3.
  if (cmd == "use") {
    // Audit fix: option detection must go through the tokenizer's own
    // parsed.opts (the shared, quote/paren-aware grammar every other
    // command's option handling already uses) rather than the raw-string
    // re-scan below — that raw scan only recognizes a trailing EXACT
    // literal ", clear"/", replace" and otherwise leaves the option text
    // glued onto `afterUse`, so any other option (a typo, or a real Stata
    // `use` option like `nolabel` this version of PIET does not implement)
    // got silently folded into the filepath string, producing a "file not
    // found" error that blamed a corrupted path instead of naming the real
    // problem. `clear`/`replace` are the only options handlers::runUse
    // implements (PIET always replaces the in-memory dataset on `use`, so
    // both are accepted as documented no-ops that reach the SAME runUse —
    // B5-D-USE-REPLACE, see kVocabUse's own comment for the discriminating
    // oracle pair that pins `, replace` to discard, not merge) — reject
    // anything else honestly, before the raw filepath scan below can
    // mis-parse it.
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kUseKnownOpts = kVocabUse.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kUseKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    std::string raw = parsed.raw;
    // BUG-251/WS-Q: was `lowerRaw.find("use")` + `substr(pos + 3)` — a LITERAL
    // spelling, unguarded. `use` really is abbreviable in Stata (measured
    // 2026-08-07 on StataBE: `u` and `us` both answer rc 0), so the moment the
    // registry row gained the abbreviations this threw on the line `u`.
    std::string afterUse = rawArgsAfterCmd(raw);
    // strip a trailing `, clear` or `, replace` (case-insensitive) option —
    // both discard the in-memory dataset identically (runUse never branches
    // on which spelling was given), so either is simply removed before the
    // filepath is taken from what's left.
    size_t comma = afterUse.find(',');
    if (comma != std::string::npos) {
      std::string opt = afterUse.substr(comma + 1);
      std::string optLower = lower(opt);
      size_t oa = optLower.find_first_not_of(" \t");
      size_t ob = optLower.find_last_not_of(" \t");
      if (oa != std::string::npos) {
        std::string trimmedOpt = optLower.substr(oa, ob - oa + 1);
        if (trimmedOpt == "clear" || trimmedOpt == "replace") {
          afterUse = afterUse.substr(0, comma);
        }
      }
    }
    size_t b = afterUse.find_last_not_of(" \t");
    afterUse = (b == std::string::npos) ? "" : afterUse.substr(0, b + 1);
    std::string filepath = !afterUse.empty() ? afterUse
                           : (!parsed.varlist.empty() ? parsed.varlist[0] : "");
    if (filepath.empty()) {
      blocks.push_back(ojson(eval::engineError("syntax_error: `use` requires a filename", "syntax_error")));
      return blocks;
    }
    ojson useResult = handlers::runUse(filepath, session);
    // BUG A / hbin-sniff honesty note: runUse() attaches a "note" field to
    // its own block when it had to load a mismatched-extension file by
    // sniffing hbin content — lift it into its OWN prepended, UNPREFIXED
    // echo block (same pattern xtset/tsset/cd/pwd/clear already use for a
    // second informational echo — see fuzz_sequences.py's own doc-comment:
    // this is NOT a new command boundary, only a ".  "-prefixed echo is).
    if (useResult.contains("note")) {
      ojson noteBlock;
      noteBlock["type"] = "echo";
      noteBlock["text"] = useResult["note"].get<std::string>();
      blocks.push_back(noteBlock);
      useResult.erase("note");
    }
    blocks.push_back(useResult);
    return blocks;
  }

  // ------------------------------------------------------------------
  // Mission 3 (hardening-queue item, not in BUGLOG): `sysuse DATASET[,
  // clear]` / `sysuse dir`. Real Stata's `sysuse` loads one of a whole
  // library of named sample datasets by bare name (no path); PIET ships
  // exactly one today — piet/packaging/sample/synthetic_panel.csv, the SAME
  // fixture main.cpp's runUiSmoke() already loads (bundle-first via
  // piet::bundleResourcesDir()+"/sample/synthetic_panel.csv", falling back to
  // the dev-tree piet/packaging/sample/synthetic_panel.csv — see main.cpp
  // ~957-968 and its resolveFrontendIndexPath() sibling ~252-269). This
  // arm mirrors that exact bundle-first/dev-tree-fallback order via the
  // fileExistsLocal()/devTreeSyntheticSampleCsv() helpers above rather
  // than inventing a second convention. Trailing-token arity (1 token —
  // DATASET or `dir`) is enforced by mission 1's shared kFixedArityCommands
  // table above, not re-checked here.
  if (cmd == "sysuse") {
    if (!parsed.varlist.empty() && lower(parsed.varlist[0]) == "dir") {
      ojson dirOut;
      dirOut["type"] = "display";
      dirOut["text"] = "synthetic_panel";
      blocks.push_back(dirOut);
      return blocks;
    }
    std::string dsArg = parsed.varlist.empty() ? std::string() : parsed.varlist[0];
    if (dsArg.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `sysuse` requires a dataset name (or `sysuse dir` to list what's available)",
          "syntax_error")));
      return blocks;
    }
    std::string dsLower = lower(dsArg);
    static const std::string kDtaSuffix = ".dta";
    if (dsLower.size() > kDtaSuffix.size() &&
        dsLower.compare(dsLower.size() - kDtaSuffix.size(), kDtaSuffix.size(), kDtaSuffix) == 0) {
      dsLower = dsLower.substr(0, dsLower.size() - kDtaSuffix.size());
    }
    if (dsLower != "synthetic_panel") {
      blocks.push_back(ojson(eval::engineError(
          "sysuse: unknown dataset `" + dsArg + "` — the only dataset bundled with this version of "
              "PIET is `synthetic_panel` (run `sysuse dir` to list what's available).",
          "engine_error")));
      return blocks;
    }
    std::string sampleCsv;
    if (std::optional<std::string> resources = bundleResourcesDir()) {
      std::string bundled = *resources + "/sample/synthetic_panel.csv";
      if (fileExistsLocal(bundled)) sampleCsv = bundled;
    }
    if (sampleCsv.empty()) sampleCsv = devTreeSyntheticSampleCsv();
    blocks.push_back(handlers::runUse(sampleCsv, session));
    return blocks;
  }

  if (cmd == "codebook") {
    // BUG-336 (batch 2): known set measured from the handler body -- codebook reads no options (delegates to runSummarize per-var).
    // BUG-371: `compact` moves from the honest-reject set into the implemented one.
    // Nothing new is computed for it — this arm already gathers obs/unique/mean/
    // min/max/label for every variable; compact only decides which get printed.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabCodebook.known, kVocabCodebook.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // BS-025 (BETA Phase C batch C1): codebook used to be a bare alias of
    // `describe` (same shared arm as below) — so `codebook income if
    // year==1982` silently ignored the `if` entirely (returned the SAME
    // unfiltered stats as plain `codebook income`), and the payload was
    // only describe's type/format/label/n_missing/n_unique, never
    // codebook's own range/mean/sd/percentiles. This arm gives codebook
    // its own implementation: honour `if` via the same keepif-into-
    // temp-hbin idiom already used by the tabulate/table `if` arms above
    // (dispatcherNextPath + `cobra eval --keepif` + a fresh Datastore over
    // the filtered copy), compute per-variable type/format/label/
    // n_missing/n_unique directly off that (filtered) Datastore — mirroring
    // handlers::runDescribe's own numeric-unique-count scan — and pull
    // mean/sd/min/max/p10/p25/p50/p75/p90 from handlers::runSummarize(...,
    // detail=true, ...): cobra's `eval --summarize --detail` raw output
    // already carries every Stata percentile (verified live: p1/p5/p10/
    // p25/p50/p75/p90/p95/p99), and runSummarize forwards that raw per-var
    // object VERBATIM into its "stats" — summarize's own renderer just
    // never displays them. No handlers.cpp changes needed: both
    // runSummarize and runDescribe's per-variable logic are reused/mirrored
    // as-is.
    if (!session.hasData()) {
      blocks.push_back(ojson(eval::noDataLoaded()));
      return blocks;
    }
    Datastore* ds = session.datastore();
    std::unique_ptr<Datastore> codebookIfDs;
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
      std::string codebookIfPath = dispatcherNextPath(session.workdir(), "codebookif");
      // BUG-369 follow-up: `codebook x if L.y>0` — same injection every other
      // user-if surface carries; no-op unless the predicate names a TS operator.
      std::vector<std::string> ifArgs = translate::injectTsContextForExpr(
          {"eval", "--data", ds->path(), "--keepif", *parsed.ifExpr, "--out", codebookIfPath},
          *parsed.ifExpr, session);
      try {
        runCobraJson(ifArgs);
      } catch (const CobraError& ce) {
        blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
        return blocks;
      }
      codebookIfDs = std::make_unique<Datastore>(codebookIfPath);
      ds = codebookIfDs.get();
    }
    std::vector<std::string> names = parsed.varlist.empty() ? ds->names() : parsed.varlist;
    for (const auto& name : names) {
      if (!ds->has(name)) {
        blocks.push_back(ojson(eval::engineError("variable " + name + " not found")));
        return blocks;
      }
    }
    // Batch every numeric var into ONE cobra summarize,detail call (not one
    // per variable) — its raw per-var object already carries mean/sd/min/
    // max/p10/p25/p50/p75/p90, Stata-exact.
    std::vector<std::string> f64Names;
    for (const auto& name : names) {
      if (ds->dtype(name) == "f64") f64Names.push_back(name);
    }
    ojson f64Stats = ojson::object();
    if (!f64Names.empty()) {
      ojson sumBlock = ojson(handlers::runSummarize(f64Names, /*detail=*/true, session,
                                                     parsed.ifExpr, std::nullopt));
      if (sumBlock.value("type", "") == "summarize" && sumBlock.contains("stats")) {
        f64Stats = sumBlock["stats"];
      }
    }

    ojson variables = ojson::array();
    for (const auto& name : names) {
      VarInfo vi = ds->varinfo(name);
      auto metaIt = session.varMeta.find(name);
      std::string stataType;
      std::string fmt;
      if (metaIt != session.varMeta.end() && metaIt->second.stataType.has_value()) {
        stataType = *metaIt->second.stataType;
      } else {
        stataType = (vi.dtype == "factor") ? "str#" : "float";
      }
      if (metaIt != session.varMeta.end() && metaIt->second.format.has_value()) {
        fmt = *metaIt->second.format;
      } else {
        fmt = (vi.dtype == "factor") ? "%9s" : "%9.0g";
      }
      std::string label;
      if (metaIt != session.varMeta.end() && metaIt->second.label.has_value()) {
        label = *metaIt->second.label;
      }

      ojson entry;
      entry["name"] = name;
      entry["dtype"] = stataType;
      entry["format"] = fmt;
      entry["label"] = label;
      entry["n_missing"] = ds->nMissing(name);
      entry["n_obs"] = ds->nRows();

      if (vi.dtype == "factor") {
        entry["n_unique"] = static_cast<int64_t>(vi.levels.size());
      } else {
        const cecon::Column& col = ds->col(name);
        const double* d = col.f64();
        int64_t count = col.nbytes / static_cast<int64_t>(sizeof(double));
        std::vector<double> valid;
        valid.reserve(count);
        for (int64_t i = 0; i < count; ++i) {
          if (!std::isnan(d[i])) valid.push_back(d[i]);
        }
        std::sort(valid.begin(), valid.end());
        valid.erase(std::unique(valid.begin(), valid.end()), valid.end());
        entry["n_unique"] = static_cast<int64_t>(std::min<size_t>(valid.size(), 65536));

        if (f64Stats.contains(name)) {
          const ojson& st = f64Stats[name];
          for (const char* k : {"mean", "sd", "min", "max", "p10", "p25", "p50", "p75", "p90"}) {
            if (st.contains(k)) entry[k] = st[k];
          }
        }
      }
      variables.push_back(entry);
    }
    ojson out;
    // No dedicated frontend renderer exists yet for codebook's full payload
    // (range/unique/mean/sd/percentiles combined) — describe's own switch
    // case already renders type/format/label/n_missing/n_unique from this
    // exact "describe" block shape (console.js's formatDescribe), so reuse
    // it rather than emitting a block type the frontend cannot render at
    // all; the extra numeric fields ride along on each variable entry for
    // when the frontend adds codebook-specific rendering (out of scope for
    // this native-only batch — piet/frontend/** is forbidden here).
    out["type"] = "describe";
    out["variables"] = variables;
    // BUG-371: the compact flag rides on the same block. The frontend switches on
    // it (console.js formatDescribe) to print Stata's one-line-per-variable table
    // instead of the storage/format/label layout; a consumer that ignores the flag
    // still sees every field it saw before, so this is purely additive.
    for (const auto& [k, v] : parsed.opts)
      if (k == "compact" && (!v.isBool || v.boolValue)) out["compact"] = true;
    blocks.push_back(out);
    return blocks;
  }

  // BUG-105 abbreviation stopgap: `d` is real Stata's minimum-length
  // abbreviation of `describe` (the general prefix resolver is a separate
  // arc — BACKLOG BUG105-112-HARDENING-REMAINDER). Every spelling added
  // here is ALSO added to this command's registry.cpp `abbrev` list, which
  // is what makes the registry-backed qualifier gates (tokenizer's `in`/
  // weight allow-lists) and testRegistryRoutes see the same alias set.
  if (cmd == "describe" || cmd == "desc" || cmd == "d") {
    // BUG-336 (batch 2): known set measured from the handler body -- runDescribe
    // itself takes no options parameter. B5-D5: `fullnames` is now accepted
    // (kVocabDescribe.known) as a validated no-op -- see that registration's
    // comment for the receipt; short/simple/numbers/detail remain refused by
    // name because they change what prints.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabDescribe.known, kVocabDescribe.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (!session.hasData()) {
      // Mission 5: empty description (0 vars), matching real Stata — see
      // the kNoDataCommands comment above for why this is handled here
      // rather than in handlers::runDescribe() itself.
      ojson emptyDescribe;
      emptyDescribe["type"] = "describe";
      emptyDescribe["variables"] = ojson::array();
      blocks.push_back(emptyDescribe);
      return blocks;
    }
    std::optional<std::vector<std::string>> vl;
    if (!parsed.varlist.empty()) vl = parsed.varlist;
    ojson desc = ojson(handlers::runDescribe(vl, session));
    // BUG-159 (P4 wave): `describe` is r-class in Stata — it posts r(N), r(k)
    // and r(width). PIET posted nothing, so a following `r(k)` errored with
    // "run an r-class command first" after the user had just run one. r(N) is
    // the DATASET's row count (not the described-variable count) and r(k) the
    // number of variables described, which is why the two are read from
    // different places.
    //
    // The row count is read off the RESIDENT CARD, never via session.datastore().
    // describe's whole D1 card-plane design is that a factor-only describe
    // constructs ZERO Datastores, and a construct here would have silently
    // undone that optimization for every describe — the C4-L2B MECHANISM gate
    // caught exactly that when this first went in. When the card cannot answer
    // (no resident card / not current), r(N) is left alone rather than forcing
    // the construct: an honest absence beats a hidden cost. r(k) always comes
    // from the describe block already built above, so it costs nothing.
    {
      if (std::optional<int64_t> rows = session.residentRowsIfCurrent()) {
        session.eval.rValues.set("N", static_cast<double>(*rows));
      }
      const size_t nVars = desc.contains("variables") && desc.at("variables").is_array()
                               ? desc.at("variables").size()
                               : 0;
      session.eval.rValues.set("k", static_cast<double>(nVars));
    }
    blocks.push_back(desc);
    return blocks;
  }

  // BUG-105 abbreviation stopgap: `su` (see the `describe`/`d` note above).
  if (cmd == "summarize" || cmd == "sum" || cmd == "su") {
    bool detail = false;
    for (const auto& [k, v] : parsed.opts) {
      if ((k == "detail" || k == "d") && v.isBool && v.boolValue) detail = true;
    }
    // P4 / BUG-060: unknown-option rejection net. `detail`/`d` and `meanonly`
    // are the summarize options this version of PIET accepts. `meanonly`
    // (BUG-090) tells Stata to skip the variance/percentile pass entirely —
    // r(N)/r(mean)/r(min)/r(max)/r(sum)/r(sum_w) are published, but r(sd)
    // (and the derived r(Var)/r(median)/percentiles/skewness/kurtosis) are
    // NOT: BUG-476, measured StataBE 19.5
    // (tests/eval/golden/aer_b4/milfpins_oracle_r.log, batch R,
    // `B4VAL|mean|+1.81541bacf914cX+00c|sd_is_missing|+1.0000000000000X+3ff`
    // — mean published, r(sd) is Stata's `.`). PIET's runSummarize still
    // computes the full stats unconditionally (a superset of what meanonly
    // leaves in memory, and cheaper to keep than to special-case) — the
    // DISPLAY and the rendered block are unaffected by meanonly either way
    // (Stata also suppresses meanonly's display; that half is NOT changed
    // here, see the r()-only prune below) — only r() is pruned to match
    // what Stata actually leaves set. Anything else isn't a real summarize
    // option at all -> r(198)-parity reject naming the token.
    bool meanonly = false;
    // BUG-333: `separator(#)` is a display option — Stata draws a horizontal rule
    // every # variables (0 = no rules, default 5). Measured on StataBE 19.5:
    // separator(0)/(2)/(-1) are all rc 0, separator(x) is r(198), so only a
    // non-numeric argument is refused. This loop is kept hand-rolled (rather than
    // switched to rejectUnknownOptions) because it predates the registry and is
    // the summarize arm's only gate; widening it in place is the smaller change.
    long summarizeSeparator = -1;  // -1 = not given; the renderer defaults to 5
    for (const auto& [k, v] : parsed.opts) {
      if (k == "meanonly" && (!v.isBool || v.boolValue)) {
        meanonly = true;
        continue;
      }
      if (k == "detail" || k == "d") continue;
      if (k == "separator") {
        if (v.isBool) {
          blocks.push_back(ojson(eval::engineError(
              "option `separator` requires a number, e.g. separator(5)", "unsupported_option")));
          return blocks;
        }
        try {
          size_t used = 0;
          summarizeSeparator = std::stol(v.strValue, &used);
          if (used != v.strValue.size()) throw std::invalid_argument("trailing");
        } catch (const std::exception&) {
          blocks.push_back(ojson(eval::engineError(
              "option `separator()` incorrectly specified — invalid number", "unsupported_option")));
          return blocks;
        }
        if (summarizeSeparator < 0) summarizeSeparator = 0;  // Stata takes -1 as "no rules"
        continue;
      }
      blocks.push_back(
          ojson(eval::engineError("option `" + k + "` not allowed", "unsupported_option")));
      return blocks;
    }
    std::optional<std::vector<std::string>> vl;
    if (!parsed.varlist.empty()) vl = parsed.varlist;
    // `summarize ... if COND [in RANGE]` is now wired: the row filter
    // reaches the engine via cobra keepif inside runSummarize (Doctrine ①:
    // qualifier honoured, never dropped). BS-090 (BETA Phase G): `in` is now
    // legal here too (registry.cpp's allowInRange), composed with `if` via
    // AND inside runSummarize/applyIfAndInFilter.
    // MILF Step-1 D3 / P3 weights wave: `parsed.weightVar` only survives the
    // tokenizer's guard for this branch when weightType is fw/fweight/aw/
    // aweight (registry.cpp's summarize row is AweightFweight) — any other
    // weight type on summarize was already rejected upstream. Which cobra
    // flag gets used (--fweight vs --aweight) depends on WHICH of those the
    // user actually typed, so branch on parsed.weightType here rather than
    // baking one flag into runSummarize itself.
    bool summarizeIsAweight = parsed.weightType.has_value() &&
        (lower(*parsed.weightType) == "aw" || lower(*parsed.weightType) == "aweight");
    ojson summBlock = summarizeIsAweight
        ? ojson(handlers::runSummarizeAweight(vl, detail, session, parsed.ifExpr, parsed.weightVar,
                                              parsed.inRange))
        : ojson(handlers::runSummarize(vl, detail, session, parsed.ifExpr, parsed.weightVar,
                                       parsed.inRange));
    // dispatcher.py's post-handler hook: `capture_r_from_block(result_block,
    // session)` — every native-handler result feeds this (summarize is the
    // r-class case the M3a corpus exercises; lincom/nlcom land when ported).
    // Missing entirely through M2 — this is the fix (not new M3a scope).
    // BUG-333: carry the separator to the renderer. Emitted only when the user
    // asked, so every existing summarize fixture stays byte-identical.
    if (summarizeSeparator >= 0 && summBlock.value("type", "") != "error")
      summBlock["separator"] = summarizeSeparator;
    eval::captureRFromBlock(summBlock, session.eval);
    // BUG-476: `meanonly` leaves r(sd) (and everything captureRFromBlock
    // DERIVES from sd — r(Var), computed as sd*sd when sd was present but
    // Var was not; macros.cpp's captureRFromBlock, "Var = sd^2" branch —
    // and r(median), the p50 alias) Stata-missing. ONLY r(sd) is oracle-
    // measured (BUG-476.do's own header: "do not add an arm for it
    // without capturing it first" for r(min)/r(max)); the percentile/
    // skewness/kurtosis keys are pruned here too because they are NEVER
    // computed under Stata's real meanonly (undisputed — meanonly skips
    // the whole variance/percentile pass), not because a cell pins them.
    // NOT erased: Stata's own convention (and the BUG-476 probe's `scalar
    // b476sd = r(sd)` / `assert b476sd >= .`, which must run to rc 0, not
    // error) is that a numeric r()-result the last command did not compute
    // reads back as MISSING, not "not found" — real Stata's `display
    // r(sd)` after `summarize ..., meanonly` succeeds and shows `.`
    // (oracle_r.log:71's `%21x r(sd)` = `+1.0000000000000X+3ff`, exactly
    // 2^1023 = kStataMissingSentinel). Erasing the key instead would make
    // PIET's own command-line r()-read path answer the honest-but-wrong
    // "r(sd) is not set" error (macros.cpp's unsetRAsMissing guard, the
    // FIX-RESTIMATE precedent) — so these are SET to the sentinel, not
    // removed. The RENDERED block (summBlock itself) is untouched — only
    // the r() store captureRFromBlock just built is overwritten, so
    // freeze/summarize.json and every other summarize fixture's displayed
    // table stays byte-identical.
    if (meanonly && summBlock.value("type", std::string()) != "error") {
      for (const char* k : {"sd", "Var", "median", "p1", "p5", "p10", "p25", "p50", "p75",
                             "p90", "p95", "p99", "skewness", "kurtosis"}) {
        session.eval.rValues.set(k, eval::kStataMissingSentinel);
      }
    }
    blocks.push_back(summBlock);
    return blocks;
  }

  if (cmd == "save") {
    std::string raw = parsed.raw;
    // BUG-105/WS-K: search for the TYPED spelling, not the literal "save".
    // `sav "f.dta"` made find("save") return npos and substr(npos + 4) threw
    // std::out_of_range ("basic_string") — a hard crash of the whole run, not a
    // reject. Found by the spot-check, not by reading: two arms re-derive their
    // arguments from the RAW line by locating the command word in it, so
    // normalising the routed token is necessary but NOT sufficient for them.
    // BUG-251/WS-Q: and searching for `cmdTyped` is still a search — correct
    // only by an invariant nothing here checks, and still unguarded against
    // npos. The tail never depended on the spelling at all.
    std::string after = rawArgsAfterCmd(raw);
    bool inQ = false;
    long commaIdx = -1;
    for (size_t i = 0; i < after.size(); ++i) {
      if (after[i] == '"') {
        inQ = !inQ;
      } else if (after[i] == ',' && !inQ) {
        commaIdx = static_cast<long>(i);
        break;
      }
    }
    bool replace = false;
    if (commaIdx >= 0) {
      std::string optsTail = trimWs(lower(after.substr(static_cast<size_t>(commaIdx) + 1)));
      after = trimWs(after.substr(0, static_cast<size_t>(commaIdx)));
      // Every `save` suboption besides `replace`/`emptyok` (nolabel/orphans/
      // version(#)) is unimplemented — reject the first one by name instead
      // of silently discarding the whole tail (BUG-141 class: a dropped
      // option looks identical to an honoured one from the caller's side).
      // B5-D9: `emptyok` is accepted and otherwise ignored -- see kVocabSave's
      // registration comment for why it is a validated no-op on this verb.
      std::istringstream optsStream(optsTail);
      std::string opt;
      while (optsStream >> opt) {
        if (opt == "replace") {
          replace = true;
          continue;
        }
        if (opt == "emptyok") {
          continue;
        }
        blocks.push_back(ojson(eval::unsupportedOption(
            opt, "`save` supports only `replace`/`emptyok` in this build; nolabel/orphans/"
                 "version() are not implemented.")));
        return blocks;
      }
    }
    std::string filepath = !after.empty() ? after
                           : (!parsed.varlist.empty() ? parsed.varlist[0] : "");
    if (filepath.empty()) {
      blocks.push_back(ojson(eval::engineError("syntax_error: `save` requires a filename")));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runSaveData(filepath, replace, session)));
    return blocks;
  }

  if (cmd == "rename" || cmd == "ren") {
    // BUG-336 (batch 2): known set measured from the handler body -- runRename takes no options; Stata rename has none on the 1:1 form either.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRename.known, kVocabRename.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // ------------------------------------------------------------------
    // B4-W3b: the GROUP form `rename _all, lower` / `rename *, upper`.
    //
    // Routed BEFORE the wildcard reject below, because `*` is exactly the
    // spelling the corpus uses and the reject is about the PATTERN form
    // (`rename v* w*`), which this is not: there is no target pattern here at
    // all, only a case rule applied to every name.
    bool renLower = false, renUpper = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "lower" && (!v.isBool || v.boolValue)) renLower = true;
      if (k == "upper" && (!v.isBool || v.boolValue)) renUpper = true;
    }
    if (renLower || renUpper) {
      if (renLower && renUpper) {
        blocks.push_back(ojson(eval::errorBlock(
            "bad_input", "rename: lower and upper are alternatives — specify one.")));
        return blocks;
      }
      const bool wholeSet = parsed.varlist.size() == 1 &&
                            (parsed.varlist[0] == "_all" || parsed.varlist[0] == "*");
      if (!wholeSet) {
        // Stata's group form also takes an explicit varlist; this build
        // implements the whole-dataset spelling the corpus writes and says so
        // for the rest rather than half-applying it.
        blocks.push_back(ojson(eval::errorBlock(
            "unsupported_option",
            "rename: this build implements the case options on the whole-dataset form only "
            "(`rename _all, " + std::string(renLower ? "lower" : "upper") +
                "` or `rename *, ...`); a narrowed varlist is not implemented.")));
        return blocks;
      }
      if (!session.hasData()) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return blocks;
      }
      std::vector<std::string> names;
      if (const auto* card = session.shadowVarlistIfCurrent()) {
        for (const auto& info : *card) names.push_back(info.name);
      } else if (Datastore* rds = session.datastore()) {
        names = rds->names();
      }
      auto recase = [&](std::string t) {
        for (auto& c : t)
          c = renLower ? static_cast<char>(std::tolower(static_cast<unsigned char>(c)))
                       : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return t;
      };
      // The collision check runs over the WHOLE mapping before a single rename
      // is issued: `X` and `x2`->`x2` is fine, but `X` and `x` both mapping to
      // `x` is Stata's r(110), and discovering that half-way through would
      // leave the dataset partly recased with no way back.
      std::map<std::string, std::string> target;  // newname -> the name claiming it
      std::vector<std::pair<std::string, std::string>> moves;
      for (const auto& nm : names) {
        const std::string nn = recase(nm);
        auto it = target.find(nn);
        if (it != target.end()) {
          blocks.push_back(ojson(eval::errorBlock(
              "bad_input", "variable " + nn + " already defined")));
          return blocks;
        }
        target.emplace(nn, nm);
        if (nn != nm) moves.emplace_back(nm, nn);
      }
      if (moves.empty()) {
        ojson note;
        note["type"] = "display";
        note["class"] = "note";
        note["text"] = "(all newnames==oldnames)";
        blocks.push_back(note);
        return blocks;
      }
      // A rename whose TARGET is another variable's current name would be
      // rejected by runRename's own exists-guard even though the mapping as a
      // whole is a valid permutation (`A`,`b` -> `a`,`B` under upper/lower).
      // Two passes through a reserved prefix keeps every intermediate name
      // unique and never collides with a real one (`__b4ren_` is not a legal
      // Stata name a user could hold).
      std::vector<std::string> renamed;
      bool viaTemp = false;
      for (const auto& [from, to] : moves) {
        if (std::find(names.begin(), names.end(), to) != names.end()) viaTemp = true;
      }
      auto doRename = [&](const std::string& from, const std::string& to) -> bool {
        ojson r = handlers::runRename(from, to, session);
        if (r.is_object() && r.value("type", std::string()) == "error") {
          blocks.push_back(r);
          return false;
        }
        return true;
      };
      if (viaTemp) {
        for (size_t i = 0; i < moves.size(); ++i) {
          if (!doRename(moves[i].first, "__b4ren_" + std::to_string(i))) return blocks;
        }
        for (size_t i = 0; i < moves.size(); ++i) {
          if (!doRename("__b4ren_" + std::to_string(i), moves[i].second)) return blocks;
        }
      } else {
        for (const auto& [from, to] : moves) {
          if (!doRename(from, to)) return blocks;
        }
      }
      for (const auto& [from, to] : moves) renamed.push_back(to);
      ojson note;
      note["type"] = "display";
      note["class"] = "note";
      note["text"] = "(" + std::to_string(renamed.size()) + " variable" +
                     (renamed.size() == 1 ? "" : "s") + " renamed)";
      blocks.push_back(note);
      return blocks;
    }
    bool hasWildcard = std::any_of(parsed.varlist.begin(), parsed.varlist.end(),
                                   [](const std::string& t) {
                                     return t.find('*') != std::string::npos ||
                                            t.find('?') != std::string::npos;
                                   });
    if (hasWildcard) {
      blocks.push_back(ojson(eval::engineError(
          "rename pattern form not yet supported in 1.3.1 — rename one variable at a time")));
      return blocks;
    }
    if (parsed.varlist.size() != 2) {
      blocks.push_back(
          ojson(eval::engineError("rename requires exactly two names: `rename old new`")));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runRename(parsed.varlist[0], parsed.varlist[1], session)));
    return blocks;
  }

  if (cmd == "clonevar") {
    // `clonevar new = old` or `clonevar new old` (both forms accepted —
    // strip a literal '=' then split on whitespace, mirrors commands.py's
    // `" ".join(vl).replace("=", " ").split()`).
    // BUG-127 class: an optional storage-type prefix (`clonevar double x = y`)
    // used to make `double` the NEWVAR and silently discard `x` — strip it
    // through the same stripStorageType gen/egen already use.
    StorageTypeInfo cvInfo = stripStorageType(joinVarlist(parsed.varlist));
    if (cvInfo.stringTyped) {
      blocks.push_back(ojson(eval::engineError(
          "clonevar: string storage types are not supported — PIET stores f64/factor")));
      return blocks;
    }
    std::string joined = cvInfo.expr;
    for (auto& c : joined) {
      if (c == '=') c = ' ';
    }
    std::istringstream iss(joined);
    std::vector<std::string> toks;
    std::string tok;
    while (iss >> tok) toks.push_back(tok);
    if (toks.size() < 2) {
      blocks.push_back(
          ojson(eval::engineError("clonevar: syntax is `clonevar NEWVAR = OLDVAR`")));
      return blocks;
    }
    // BS-031(c): parsed.ifExpr was already correctly extracted by the
    // tokenizer (`clonevar inc2 = income if year==1982`) but never forwarded
    // to the handler at all — see runClonevar's own comment for the fix.
    blocks.push_back(ojson(handlers::runClonevar(toks.front(), toks.back(), session, parsed.ifExpr)));
    return blocks;
  }

  if (cmd == "generate" || cmd == "gen" || cmd == "g") {
    // BUG-336 (batch 2): known set measured from the handler body -- runGen/runBysortGen take no options.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabGenerate.known, kVocabGenerate.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    std::string raw = joinVarlist(parsed.varlist);
    StorageTypeInfo info = stripStorageType(raw);
    // BUG-112(1) PIET-half: the exists-guard fires BEFORE this verb splits into
    // any of its four sinks (string / standalone _n-_N / `in`-range / plain), so
    // all four reject identically. See genTargetExistsError's comment for why
    // the guard is at the verb and not in the handlers.
    if (auto genExists = genTargetExistsError(session, genTargetName(info.expr))) {
      blocks.push_back(*genExists);
      return blocks;
    }
    // BUG-032: string-valued / string-gated gen must be evaluated row-wise in
    // PIET (cobra is all-f64, cannot do string fns or hold string columns) —
    // mirrors dispatcher.py's `if needs_string_eval(...): return
    // run_string_gen(...)` FIRST, before the numeric cobra path. varname/rhs
    // split matches the oracle's `expr.split("=", 1)`.
    {
      std::string varname, rhs;
      size_t eq = info.expr.find('=');
      if (eq != std::string::npos) {
        varname = trimWs(info.expr.substr(0, eq));
        rhs = trimWs(info.expr.substr(eq + 1));
      } else {
        varname = trimWs(info.expr);
      }
      // BUG-456: the est-ref resolve used to run AFTER this detour decided, so
      // `gen z = (e(cmd)=="regress")` took the string path with a raw e(cmd)
      // in hand and failed at obs 1. Resolve (and fold) first; the detour then
      // sees `("regress"=="regress")` -> `(1)` and routes it numeric.
      if (auto err = preResolveGenExpr(info.expr, rhs, session)) {
        blocks.push_back(*err);
        return blocks;
      }
      if (strmutate::needsStringEval(session, varname, rhs, parsed.ifExpr, info.stringTyped,
                                     /*isReplace=*/false)) {
        // BS-031(d): fold `in` into the ifExpr this path was already
        // missing it from — see foldStringInRange's own comment above.
        StringInRangeFold fold = foldStringInRange(parsed.ifExpr, parsed.inRange);
        if (!fold.ok) {
          blocks.push_back(stringInRangeError(fold, *parsed.inRange));
          return blocks;
        }
        std::optional<std::string> combinedIf =
            fold.combinedIf.empty() ? std::nullopt : std::optional<std::string>(fold.combinedIf);
        blocks.push_back(
            ojson(strmutate::runStringGen(session, info.expr, combinedIf, info.stringTyped)));
        // BUG-250: `gen str8 s = ...` records its declared width like the
        // numeric sinks below record theirs.
        recordDeclaredStataType(session, varname, info, blocks.back());
        return blocks;
      }
    }
    // _b[]/_se[]/e()/scalar() refs resolved to literals BEFORE the expression
    // ships to cobra (which has no estimation state) — the same resolver the
    // scalar/local/if paths use, so gen sees the same value display does. Since
    // BUG-456 this happens in preResolveGenExpr ABOVE the string detour (the
    // `if` was resolved at the parse point); the model gate lives there too.
    // BUG-432: a stored SCALAR does not depend on a model being in memory, and
    // the gate above meant `gen x = y * scalar(s)` was rc 1 in a session with no
    // estimation — while `display s` printed the value. Matrix elements are the
    // same story (BUG-428). The `if` was already substituted at the parse point;
    // this is gen's own expression, which reaches cobra rather than the row
    // evaluator and needs the literals just as much.
    // BUG-456 follow-up (review finding 3): this pass -- the one that
    // substitutes STORED SCALARS, which preResolveGenExpr above does not --
    // discarded its error block, so a mismatch that only appears after scalar
    // substitution (`scalar one = 1` then `gen z = (e(cmd)==one)`) came back as
    // the coarse "generate: expression evaluation failed at obs 1" with no r()
    // code, while the identical `count if e(cmd)==one` reported r(109).
    {
      std::optional<ojson> genExprErr;
      info.expr = substituteRowExprRefs(info.expr, session, &genExprErr);
      if (genExprErr.has_value()) {
        blocks.push_back(*genExprErr);
        return blocks;
      }
    }
    // BS-091/BS-101 (BETA A4): c(name) refs ALSO resolve to a literal BEFORE
    // cobra sees this expression — unlike the est-scalar pass above, c()
    // values never depend on a model being in memory, so this is unGATED.
    info.expr = substituteCRefs(info.expr, session);
    if (parsed.ifExpr.has_value()) {
      parsed.ifExpr = substituteCRefs(*parsed.ifExpr, session);
    }
    // BUG-471 T2: the declared type, else c(type) — stored by the engine, not
    // only claimed (gen_untyped_13 :96 float, gen_byte_27 :222 truncated).
    const std::string genStoreType = storage::resolveCreateType(session, info.typeToken);
    if (hasNorCapN(info.expr) || (parsed.inRange.has_value() && !parsed.inRange->empty())) {
      // bysort.py's run_gen_with_n_N (standalone _n/_N, no bysort prefix) —
      // global 1..N context via cobra eval --bygen with by_vars=[]. BUG-077:
      // `in a/b` also routes here (same empty-by-list global _n/_N context)
      // so the range can fold into the same cond() the if-expr builds.
      blocks.push_back(ojson(handlers::runBysortGen(session, info.expr, {}, parsed.ifExpr, false,
                                                     false, parsed.inRange, std::nullopt,
                                                     genStoreType)));
      recordDeclaredStataType(session, genTargetName(info.expr),
                              withSessionDefaultType(session, info), blocks.back());  // BUG-250
      return blocks;
    }
    blocks.push_back(ojson(handlers::runGen(info.expr, parsed.ifExpr, session, genStoreType)));
    recordDeclaredStataType(session, genTargetName(info.expr),
                            withSessionDefaultType(session, info), blocks.back());  // BUG-250
    return blocks;
  }

  if (cmd == "replace") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabReplace.known,
                                              kVocabReplace.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    bool noPromote = false;
    for (const auto& [k, v] : parsed.opts)
      if (k == "nopromote" && v.isBool && v.boolValue) noPromote = true;
    std::string raw = joinVarlist(parsed.varlist);
    StorageTypeInfo info = stripStorageType(raw);
    {
      std::string varname, rhs;
      size_t eq = info.expr.find('=');
      if (eq != std::string::npos) {
        varname = trimWs(info.expr.substr(0, eq));
        rhs = trimWs(info.expr.substr(eq + 1));
      } else {
        varname = trimWs(info.expr);
      }
      if (auto err = preResolveGenExpr(info.expr, rhs, session)) {  // BUG-456, as in gen
        blocks.push_back(*err);
        return blocks;
      }
      if (strmutate::needsStringEval(session, varname, rhs, parsed.ifExpr, info.stringTyped,
                                     /*isReplace=*/true)) {
        // BS-031(e): fold `in` into the ifExpr this path was already
        // missing it from — see foldStringInRange's own comment above (gen
        // branch).
        StringInRangeFold fold = foldStringInRange(parsed.ifExpr, parsed.inRange);
        if (!fold.ok) {
          blocks.push_back(stringInRangeError(fold, *parsed.inRange));
          return blocks;
        }
        std::optional<std::string> combinedIf =
            fold.combinedIf.empty() ? std::nullopt : std::optional<std::string>(fold.combinedIf);
        blocks.push_back(ojson(
            strmutate::runStringReplace(session, info.expr, combinedIf, info.stringTyped)));
        return blocks;
      }
    }
    // Est-scalar pre-resolution: done in preResolveGenExpr above (BUG-456).
    // Same c(name) pre-resolution as gen (BS-091/BS-101, see gen branch above).
    info.expr = substituteCRefs(info.expr, session);
    if (parsed.ifExpr.has_value()) {
      parsed.ifExpr = substituteCRefs(*parsed.ifExpr, session);
    }
    // BUG-471 T2: replace stores into the variable's own type (the handler
    // widens an integer type by Stata's lattice, or truncates under nopromote).
    const std::string replaceStoreType =
        storage::resolveReplaceType(session, genTargetName(info.expr));
    if (hasNorCapN(info.expr) || (parsed.inRange.has_value() && !parsed.inRange->empty())) {
      // BUG-077: `in a/b` also routes here — see the gen branch above.
      blocks.push_back(ojson(handlers::runBysortGen(session, info.expr, {}, parsed.ifExpr, false,
                                                     true, parsed.inRange, std::nullopt,
                                                     replaceStoreType, noPromote)));
      return blocks;
    }
    blocks.push_back(ojson(
        handlers::runReplace(info.expr, parsed.ifExpr, session, replaceStoreType, noPromote)));
    return blocks;
  }

  if (cmd == "egen") {
    std::string raw = joinVarlist(parsed.varlist);
    StorageTypeInfo info = stripStorageType(raw);
    std::string byVal;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "by" && !v.isBool) {
        byVal = v.strValue;
        break;
      }
    }
    // BS-032 (BETA campaign C2): real Stata rejects `egen ... = group(...)`
    // / `tag(...)` combined with a by-context — "group() may not be
    // combined with by", r(190) (best-effort wording — not oracle-captured
    // for this item; cited from real Stata's own documented error). This
    // arm previously ran it silently either way: `bysort state: egen
    // g1=group(year)` (which injects `by(state)` into the recursed-into
    // sub-command below, same as an explicit `egen ..., by(state)` would)
    // succeeded and produced a group-within-panel index the real command
    // would refuse to compute at all. Only group()/tag() are affected —
    // every OTHER egen function legitimately combines with by() (e.g.
    // `egen total=sum(income), by(state)`), so this checks the RHS
    // function name, not just "is by present".
    if (!byVal.empty()) {
      size_t eqPos = info.expr.find('=');
      std::string rhs = eqPos == std::string::npos ? info.expr : trimWs(info.expr.substr(eqPos + 1));
      static const std::regex reGroupTagFn(R"(^\s*(group|tag)\s*\()", std::regex::icase);
      std::smatch fnm;
      if (std::regex_search(rhs, fnm, reGroupTagFn)) {
        std::string fn = lower(fnm.str(1));
        blocks.push_back(ojson(eval::engineError(
            fn + "() may not be combined with by", "unsupported_option")));
        return blocks;
      }
    }
    // BUG-471 T2: the declared type, else c(type) (egen_mean_untyped :953);
    // runEgen withholds it from tag()/seq()/mode(), whose ados ignore it.
    const std::string egenStoreType = storage::resolveCreateType(session, info.typeToken);
    std::string egenSeqType;  // F3: seq()'s widened type, from its own call's summary
    blocks.push_back(ojson(handlers::runEgen(info.expr, byVal, parsed.ifExpr, parsed.opts, session,
                                             parsed.inRange, egenStoreType, &egenSeqType)));
    // BUG-250: `egen double m = mean(x)` — same declaration carriage as gen
    // (oracle rows J|egen-double -> double, J|egen-default -> float).
    // BUG-471 T2: tag/seq are byte and mode takes its source's type whatever was
    // declared (egen_tag_type :1123, egen_seq_type :1137, _gmode.ado:19).
    {
      const std::string egenTarget = genTargetName(info.expr);
      const ojson& egenBlk = blocks.back();
      const bool egenFailed =
          egenBlk.is_object() && egenBlk.value("type", std::string()) == "error";
      const size_t egenEq = info.expr.find('=');
      const std::string egenRhs =
          egenEq == std::string::npos ? std::string() : info.expr.substr(egenEq + 1);
      if (egenFailed ||
          !storage::recordEgenFixedType(session, egenTarget, egenRhs, egenSeqType))
        recordDeclaredStataType(session, egenTarget, withSessionDefaultType(session, info),
                                egenBlk);
    }
    return blocks;
  }

  // BUG-105 (WS-K): `r` is GONE from this arm and from the regress registry
  // row. Measured live: `r price mpg` and `re price mpg` are r(199) in real
  // Stata — PIET was fabricating an abbreviation, which is the same defect
  // class as omitting one. `reg`/`regr`/`regre`/`regres` are the measured set
  // and now arrive here already normalised to "regress"; the literals kept
  // below are the two DISTINCT command names this one arm serves.
  if (cmd == "regress" || cmd == "feols" || cmd == "reghdfe") {
    // BS-063 (BETA D1): a varlist token containing '(' means an inline
    // `(endog = instruments)` IV block — real Stata syntax for THIS still
    // splits in two:
    //   - `reghdfe y (d=z), absorb(F)` IS the ivreghdfe shape (the task's
    //     own framing; cobra has no separate ivreghdfe estimator either —
    //     it is `cobra iv --fe`, same as ivreghdfe itself routes to below)
    //     -> route to the real IV handler, labelled "ivreghdfe" either way
    //     the user spelled the command.
    //   - `regress`/`reg`/`r`/`feols` with the SAME inline-paren shape is
    //     the OLD pre-`ivregress` Stata IV syntax, long retired in modern
    //     Stata in favor of `ivregress`/`ivreg2` — the D1 oracle doesn't
    //     probe this reject directly, so this keeps the pre-existing
    //     honest reject, just re-pointing the message at the real command
    //     (`ivregress`) instead of implying regress/feols will ever grow
    //     inline IV support of their own.
    bool hasIvBlock = std::any_of(parsed.varlist.begin(), parsed.varlist.end(),
                                  [](const std::string& t) { return t.find('(') != std::string::npos; });
    if (hasIvBlock) {
      if (cmd == "reghdfe") {
        blocks.push_back(ojson(handlers::runIvRegress("2sls", parsed, /*allowAbsorb=*/true,
                                                       "ivreghdfe", session,
                                                       /*smallDefault=*/true,
                                                       /*forceSmall=*/false)));
        return blocks;
      }
      blocks.push_back(ojson(eval::unsupportedOption(
          "regress (IV block)",
          "Old-style inline IV syntax `regress y (endog=instruments)` is not supported by "
          "`regress`/`feols` in this version of PIET. Use `ivregress 2sls y (endog=instruments)` "
          "instead.")));
      return blocks;
    }
    // M3a: TS-context injection lands in handlers::runRegress itself
    // (translate.cpp's hasTsToken/injectTsContext, MI-083 CONTRACT-engine
    // §16) — an L./F./D./S. varlist token now injects --xtset/--tsset from
    // session.panelvar/timevar (set by the xtset/tsset handlers below)
    // before cobra sees it; with no TS context set, cobra's own
    // `ts_no_tsset` engine error surfaces, re-wrapped with the `[code]`
    // prefix runRegress's catch clause adds (matches the 2 Eval
    // M2-ratification carve-out cells byte-for-byte — see handlers.cpp).
    // `reghdfe` IS `feols --fe` under the hood (cobra has no separate
    // reghdfe estimator — MILF-verified bit-for-bit against the Stata
    // oracle); it is routed through the exact same runRegress path as
    // regress/reg/r/feols, with only the display "estimator" label
    // differing (handlers.cpp's stataCmd param).
    // BUG-539 (W2 estimopts): `reghdfe ..., residuals[(v)]` also creates a
    // column. The handler returns the estimation block alone and hands the
    // data_mutation back through this out-parameter, so it lands as a block
    // of its own (a handler returning an array is the ARRAY-vs-BLOCK abort).
    ojson residMutation;
    blocks.push_back(ojson(handlers::runRegress(parsed, session, cmd, nullptr, &residMutation)));
    if (residMutation.is_object() && !residMutation.empty()) blocks.push_back(residMutation);
    return blocks;
  }

  // ── AER-SCOPE1 bundle H (ESTIM, 2026-09-05) ────────────────────────────────
  // E1: aliases onto estimators PIET already has.
  if (cmd == "areg") {
    blocks.push_back(ojson(handlers::runAreg(parsed, session)));
    return blocks;
  }
  if (cmd == "ivreg2" || cmd == "xtivreg2") {
    blocks.push_back(ojson(handlers::runIvreg2(parsed, session, cmd)));
    return blocks;
  }
  // E2 computed PIET-side.
  if (cmd == "spearman") {
    blocks.push_back(ojson(handlers::runSpearman(parsed, session)));
    return blocks;
  }
  if (cmd == "sktest") {
    blocks.push_back(ojson(handlers::runSktest(parsed, session)));
    return blocks;
  }
  if (cmd == "signrank") {
    blocks.push_back(ojson(handlers::runSignrank(parsed, session)));
    return blocks;
  }
  if (cmd == "centile") {
    blocks.push_back(ojson(handlers::runCentile(parsed, session)));
    return blocks;
  }
  // `tsfilter` takes a REQUIRED filter word (hp, bw, bk, cf) as its first
  // token. Only `hp` is in scope; the others are named as unknown SUBcommands
  // rather than as an unknown command, because the head verb does exist.
  if (cmd == "tsfilter") {
    const std::string sub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
    if (sub != "hp") {
      blocks.push_back(ojson(eval::unsupportedCommand(
          "tsfilter " + (sub.empty() ? std::string("<none>") : sub), std::nullopt, std::nullopt,
          "piet_gap", eval::UnsupportedCommandKind::kUnknownSubcommand)));
      return blocks;
    }
    ojson r_tsfilter = handlers::runTsfilterHp(parsed, session);
    // A handler here returns EITHER one object, an EMPTY array (silent
    // success), or an array of blocks. Pushing an array where an object belongs
    // is json type_error.306 at the first consumer that reads ["type"].
    if (r_tsfilter.is_array()) {
      for (const auto& b : r_tsfilter) blocks.push_back(b);
    } else if (!r_tsfilter.is_null()) {
      blocks.push_back(r_tsfilter);
    }
    return blocks;
  }
  if (cmd == "lpoly") {
    ojson r_lpoly = handlers::runLpoly(parsed, session);
    // A handler here returns EITHER one object, an EMPTY array (silent
    // success), or an array of blocks. Pushing an array where an object belongs
    // is json type_error.306 at the first consumer that reads ["type"].
    if (r_lpoly.is_array()) {
      for (const auto& b : r_lpoly) blocks.push_back(b);
    } else if (!r_lpoly.is_null()) {
      blocks.push_back(r_lpoly);
    }
    return blocks;
  }
  if (cmd == "lowess") {
    ojson r_lowess = handlers::runLowess(parsed, session);
    // A handler here returns EITHER one object, an EMPTY array (silent
    // success), or an array of blocks. Pushing an array where an object belongs
    // is json type_error.306 at the first consumer that reads ["type"].
    if (r_lowess.is_array()) {
      for (const auto& b : r_lowess) blocks.push_back(b);
    } else if (!r_lowess.is_null()) {
      blocks.push_back(r_lowess);
    }
    return blocks;
  }
  if (cmd == "pca") {
    ojson r_pca = handlers::runPca(parsed, session);
    // A handler here returns EITHER one object, an EMPTY array (silent
    // success), or an array of blocks. Pushing an array where an object belongs
    // is json type_error.306 at the first consumer that reads ["type"].
    if (r_pca.is_array()) {
      for (const auto& b : r_pca) blocks.push_back(b);
    } else if (!r_pca.is_null()) {
      blocks.push_back(r_pca);
    }
    return blocks;
  }
  if (cmd == "oaxaca") {
    // AER scope-2 B2 (2026-09-08): left the E3 refusal list above once the
    // pooled twofold inference was verified against live Stata. Same
    // array-or-object shape rule as the H1 arms.
    ojson r_ox = handlers::runOaxaca(parsed, session);
    if (r_ox.is_array()) {
      for (const auto& b : r_ox) blocks.push_back(b);
    } else if (!r_ox.is_null()) {
      blocks.push_back(r_ox);
    }
    return blocks;
  }
  if (cmd == "xi") {
    ojson xr = handlers::runXi(parsed, session);
    if (xr.is_array())
      for (const auto& b : xr) blocks.push_back(b);
    else if (!xr.is_null())
      blocks.push_back(xr);
    return blocks;
  }
  // E3: registered so the NAME resolves and the refusal carries a named cause
  // (`refused:e3_deferred`) that the route drift guard can tell apart from an
  // unrecognised token. The nearest native command for each lives on its
  // registry row's handlerTag, which is where a reader looks for it.
  if (cmd == "teffects") {
    // AER scope-2 B2-4 (2026-09-09): left the E3 refusal list below once
    // `nnmatch`'s match-set rule was measured (and the REPORT's first rule
    // REFUTED) against live Stata. The sub-command word is REQUIRED Stata
    // syntax and lands as parsed.varlist[0], the same shape `ivregress 2sls`
    // handles further down; every OTHER teffects estimator keeps its own named
    // refusal rather than sharing one cause.
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `teffects` requires an estimator: nnmatch, ra, ipw, ipwra, aipw or "
          "psmatch",
          "syntax_error")));
      return blocks;
    }
    eval::ParsedCommand rest = parsed;
    const std::string sub = lower(rest.varlist[0]);
    rest.varlist.erase(rest.varlist.begin());
    ojson r_te = handlers::runTeffects(sub, rest, session);
    if (r_te.is_array()) {
      for (const auto& b : r_te) blocks.push_back(b);
    } else if (!r_te.is_null()) {
      blocks.push_back(r_te);
    }
    return blocks;
  }
  if (cmd == "suest" || cmd == "glm" || cmd == "lasso" || cmd == "rlasso" ||
      cmd == "honestdid" || cmd == "ritest" || cmd == "acreg" ||
      cmd == "swindex" || cmd == "qreg2") {
    blocks.push_back(ojson(eval::refusedCommand(cmdTyped, "e3_deferred")));
    return blocks;
  }

  if (cmd == "ivregress") {
    // BS-063: `ivregress 2sls|gmm|liml depvar [exog...] (endog =
    // instruments) [if] [, vce(...)]` — the estimator word ("2sls"/"gmm"/
    // "liml") is REQUIRED Stata syntax, landing as parsed.varlist[0] (the
    // tokenizer has no concept of a command's own required sub-command
    // token — same shape `estat SUBCMD` already handles elsewhere in this
    // file).
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `ivregress` requires an estimator: 2sls, gmm, or liml", "syntax_error")));
      return blocks;
    }
    std::string method = lower(parsed.varlist[0]);
    if (method != "2sls" && method != "gmm" && method != "liml") {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `ivregress " + parsed.varlist[0] + "` is not a recognized estimator "
          "(supported: 2sls, gmm, liml)",
          "syntax_error")));
      return blocks;
    }
    eval::ParsedCommand rest = parsed;
    rest.varlist.erase(rest.varlist.begin());
    // BUG-474: `ivregress ..., absorb()` is rc 0 in Stata's OWN base
    // ivregress.ado (`which ivregress` -> StataNow's ado/base/i/
    // ivregress.ado, tests/eval/golden/aer_b4/b5_which.log — not a
    // third-party rule), measured
    // tests/eval/golden/aer_b4/b5_absorb_class.log:38-62
    // (`B5REC|ivregress|absorb|0|`). This is the mirror of BUG-473
    // (plain `regress` must REJECT absorb()) — the two point in opposite
    // directions through the same runIvRegress `allowAbsorb` gate, so only
    // THIS call site (the plain `ivregress` command) flips; `ivreg`
    // (pre-Stata-11 spelling, below) is untouched — not measured.
    blocks.push_back(
        ojson(handlers::runIvRegress(method, rest, /*allowAbsorb=*/true, "ivregress", session,
                                     /*smallDefault=*/false, /*forceSmall=*/false)));
    return blocks;
  }

  if (cmd == "ivreg") {
    // AER-SCOPE1 bundle A: `ivreg y x (d = z)` is Stata's PRE-11 spelling of
    // 2SLS. Unlike `ivregress` it carries no estimator word -- the estimator
    // IS 2SLS -- so the varlist arrives without the sub-command token that arm
    // strips, and the whole difference between the two commands is that one
    // token. Routed to the same handler with the method supplied here rather
    // than a second copy of the IV path that could drift from it.
    blocks.push_back(
        ojson(handlers::runIvRegress("2sls", parsed, /*allowAbsorb=*/false, "ivregress", session,
                                     /*smallDefault=*/false, /*forceSmall=*/false)));
    return blocks;
  }

  if (cmd == "ivreghdfe") {
    // BS-063: `ivreghdfe depvar [exog...] (endog=instr), absorb(F)
    // [vce(...)]` — real Stata's ivreghdfe has no 2sls/gmm/liml sub-command
    // word of its own (always the 2SLS-family estimator); `absorb()` is
    // required in practice (that's the entire point of the command over
    // plain `ivregress`) but left to the handler's own absorb() reader to
    // enforce/ignore, matching how `reghdfe` itself doesn't hard-require
    // `absorb()` at the dispatch layer either.
    blocks.push_back(
        ojson(handlers::runIvRegress("2sls", parsed, /*allowAbsorb=*/true, "ivreghdfe", session,
                                     /*smallDefault=*/true, /*forceSmall=*/false)));
    return blocks;
  }

  if (cmd == "reg3") {
    // BS-065: `reg3 (y1 x1 x2) (y2 x3 y1) [, sure]` — multiple parenthesised
    // equation groups directly in the varlist (no leading positional
    // depvar/exog before the first paren, unlike ivregress); runReg3 does
    // its own paren-group parsing from parsed.varlist.
    ojson reg3Block = ojson(handlers::runReg3(parsed, session));
    if (reg3Block.value("type", std::string()) != "error") {
      session.eval.locals.set("_rc", "0");
    }
    blocks.push_back(std::move(reg3Block));
    return blocks;
  }

  if (cmd == "boottest") {
    // BS-066: post-estimation wild cluster bootstrap — reconstructs the
    // cobra wildboot call from session.lastModel() (requires a preceding
    // vce(cluster ...) fit; runBoottest honest-rejects otherwise).
    blocks.push_back(ojson(handlers::runBoottest(parsed, session)));
    return blocks;
  }

  if (cmd == "var") {
    blocks.push_back(ojson(handlers::runVar(parsed, session)));
    return blocks;
  }

  if (cmd == "vargranger") {
    blocks.push_back(ojson(handlers::runVargranger(session)));
    return blocks;
  }

  if (cmd == "irf") {
    blocks.push_back(ojson(handlers::runIrf(parsed, session)));
    return blocks;
  }

  if (cmd == "mi") {
    blocks.push_back(ojson(handlers::runMi(parsed, session)));
    return blocks;
  }

  if (cmd == "xtabond") {
    blocks.push_back(ojson(handlers::runXtabond(parsed, session)));
    return blocks;
  }

  if (cmd == "nl" || cmd == "gmm") {
    // BUG-336: this handler consumed its real options but never rejected UNKNOWN
    // ones, so a typo was accepted and the model ran WITHOUT it. The known set is
    // MEASURED from the handler body (runNlGmm reads instruments/onestep; vce/robust stay known so nl's OWN precise reject fires);
    // an EMPTY set here eats the options the handler honours -- that first attempt
    // broke 14 freeze fixtures, and the corpus was never the problem.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabNlGmm.known, kVocabNlGmm.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runNlGmm(cmd, rawCmd, parsed, session)));
    return blocks;
  }

  if (cmd == "mixed") {
    // BUG-336: this handler consumed its real options but never rejected UNKNOWN
    // ones, so a typo was accepted and the model ran WITHOUT it. The known set is
    // MEASURED from the handler body (runMixed -> buildP7MleArgv: vce/vcov/robust/cluster);
    // an EMPTY set here eats the options the handler honours -- that first attempt
    // broke 14 freeze fixtures, and the corpus was never the problem.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMixed.known, kVocabMixed.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runMixed(parsed, session)));
    return blocks;
  }

  if (cmd == "svyset") {
    blocks.push_back(ojson(handlers::runSvyset(parsed, session)));
    return blocks;
  }

  if (cmd == "ivprobit" || cmd == "ivtobit") {
    // BUG-336: this handler consumed its real options but never rejected UNKNOWN
    // ones, so a typo was accepted and the model ran WITHOUT it. The known set is
    // MEASURED from the handler body (runIvMl reads ll/ul explicitly, then vce/vcov/robust/cluster);
    // an EMPTY set here eats the options the handler honours -- that first attempt
    // broke 14 freeze fixtures, and the corpus was never the problem.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabIvMl.known, kVocabIvMl.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runIvMl(cmd, parsed, session)));
    return blocks;
  }

  // ---- P7 wave: survival ----
  if (cmd == "stset") {
    blocks.push_back(ojson(handlers::runStset(parsed, session)));
    return blocks;
  }

  if (cmd == "stcox") {
    blocks.push_back(ojson(handlers::runStcox(parsed, session)));
    return blocks;
  }

  if (cmd == "streg") {
    blocks.push_back(ojson(handlers::runStreg(parsed, session)));
    return blocks;
  }

  // ---- P7 wave: ordered / multinomial / zero-inflated / fractional ----
  if (cmd == "ologit" || cmd == "oprobit") {
    blocks.push_back(ojson(handlers::runOrdered(cmd, parsed, session)));
    return blocks;
  }

  if (cmd == "mlogit") {
    blocks.push_back(ojson(handlers::runMlogit(parsed, session)));
    return blocks;
  }

  if (cmd == "zip" || cmd == "zinb") {
    blocks.push_back(ojson(handlers::runZeroInflated(cmd, parsed, session)));
    return blocks;
  }

  if (cmd == "fracreg") {
    // BUG-336: this handler consumed its real options but never rejected UNKNOWN
    // ones, so a typo was accepted and the model ran WITHOUT it. The known set is
    // MEASURED from the handler body (runFracreg -> buildP7MleArgv: same four);
    // an EMPTY set here eats the options the handler honours -- that first attempt
    // broke 14 freeze fixtures, and the corpus was never the problem.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabFracreg.known, kVocabFracreg.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runFracreg(parsed, session)));
    return blocks;
  }

  if (cmd == "tobit") {
    // BS-069 (BETA D2): `tobit depvar indepvars [if], ll[(#)] ul[(#)]` —
    // runTobit computes a bare ll/ul (no numeric value) from the depvar's
    // own observed min/max itself (cobra's tobit CLI has no bare-flag
    // equivalent); level() always honest-rejects (no --level surface at
    // all on cobra's tobit).
    blocks.push_back(ojson(handlers::runTobit(parsed, session)));
    return blocks;
  }

  if (cmd == "heckman") {
    // BS-070 (BETA D2): `heckman depvar indepvars [if], select(selvar =
    // selvars) [twostep]` — runHeckman NEVER forwards --twostep to the
    // engine (cobra's own --twostep flag triggers a stdout/exit-code
    // masking bug; the bare/no-flag default already computes the two-step
    // result); Stata's default (no `twostep`, full MLE) honest-rejects.
    blocks.push_back(ojson(handlers::runHeckman(parsed, session)));
    return blocks;
  }

  if (cmd == "qreg") {
    // BS-071 (BETA D2): `qreg depvar indepvars [if] [, quantile(#)
    // level(#)]` — runQreg normalizes quantile()'s fraction-or-percent
    // syntax to cobra's --tau; level() DOES forward here (live-verified:
    // cobra qreg's --level genuinely changes the CI/SE/t/p, unlike
    // tobit/ivregress/reg3's level(), which never forwards).
    blocks.push_back(ojson(handlers::runQreg(parsed, session)));
    return blocks;
  }

  if (cmd == "mediate") {
    // BS-072 (BETA D2): Stata 18 `mediate (Y) (M) (T, continuous(#L0 #L1))`
    // — runMediate parses the three parenthesised blocks itself (depth-
    // aware, since the treatment block nests continuous(...) inside its
    // own parens); only the continuous-treatment, two-explicit-level form
    // is in beta scope. ENGINE-PARITY not Stata-parity — see runMediate's
    // own header comment (estimation_m4.cpp) for the full disclosure.
    blocks.push_back(ojson(handlers::runMediate(parsed, session)));
    return blocks;
  }

  // ------------------------------------------------------------------
  // M3a handler-surface expansion (ROADMAP M3a item 4). Each arm mirrors
  // commands.py's classify() routing for the same command word.

  if (cmd == "xtset") {
    if (parsed.varlist.empty()) {
      blocks.push_back(
          ojson(eval::engineError("syntax_error: `xtset` requires ivar [timevar]", "syntax_error")));
      return blocks;
    }
    if (parsed.varlist.size() > 2) {
      blocks.push_back(ojson(eval::engineError(
          "xtset: unexpected extra tokens after ivar [tvar]: " + joinVarlist(std::vector<std::string>(
              parsed.varlist.begin() + 2, parsed.varlist.end())) +
              ". Put each command on its own line.",
          "syntax_error")));
      return blocks;
    }
    std::optional<std::string> tvar;
    if (parsed.varlist.size() > 1) tvar = parsed.varlist[1];
    // BS-030 (BETA campaign C2): delta() forwards into the ts-context
    // (translate::injectTsContext -> cobra's --tsdelta); every other real
    // Stata xtset/tsset option (9 unitoptions: daily/weekly/monthly/
    // quarterly/halfyearly/yearly/generic/clocktime/business; noquery;
    // clear) has no engine-side representation in this build and is
    // honest-rejected instead of the prior silent no-op (SCOPE BS-030:
    // "all options silently dropped today").
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kXtsetKnownOpts = kVocabXtset.known;
    std::optional<double> xtsetDelta;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "delta" && !v.isBool) {
        auto d = eval::pythonFloat(v.strValue);
        if (!d.has_value() || *d <= 0.0) {
          blocks.push_back(ojson(eval::engineError(
              "xtset: delta() requires a positive numeric value, got `" + v.strValue + "'",
              "syntax_error")));
          return blocks;
        }
        xtsetDelta = d;
      }
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kXtsetKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(handlers::runXtset(parsed.varlist[0], tvar, session, xtsetDelta));
    return blocks;
  }

  if (cmd == "tsset") {
    if (parsed.varlist.empty()) {
      blocks.push_back(
          ojson(eval::engineError("syntax_error: `tsset` requires a time variable", "syntax_error")));
      return blocks;
    }
    if (parsed.varlist.size() > 1) {
      blocks.push_back(ojson(eval::engineError(
          "tsset: unexpected extra tokens after time variable: " +
              joinVarlist(std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end())) +
              ". Put each command on its own line.",
          "syntax_error")));
      return blocks;
    }
    // BS-030 (BETA campaign C2): see the xtset arm above for the shared
    // delta()/honest-reject rationale. runTsset itself validates delta
    // against the actual data (handlers_m3.cpp) and may return an honest
    // reject in place of the usual echo — this arm just forwards whatever
    // it returns, same as before.
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kTssetKnownOpts = kVocabTsset.known;
    std::optional<double> tssetDelta;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "delta" && !v.isBool) {
        auto d = eval::pythonFloat(v.strValue);
        if (!d.has_value() || *d <= 0.0) {
          blocks.push_back(ojson(eval::engineError(
              "tsset: delta() requires a positive numeric value, got `" + v.strValue + "'",
              "syntax_error")));
          return blocks;
        }
        tssetDelta = d;
      }
    }
    // B4-W3b: the period option (and the explicit `format(%fmt)` spelling of
    // the same fact). At most one — two periods would be two different
    // formats for one column, and picking either silently is the wrong half of
    // honest.
    std::string tssetFmt;
    for (const auto& [k, v] : parsed.opts) {
      auto it = kTssetPeriodFmt().find(k);
      std::string cand;
      if (it != kTssetPeriodFmt().end() && (!v.isBool || v.boolValue)) {
        cand = it->second;
      } else if (k == "format" && !v.isBool) {
        cand = strutil::trim(v.strValue);
      }
      if (cand.empty()) continue;
      if (!tssetFmt.empty() && tssetFmt != cand) {
        blocks.push_back(ojson(eval::errorBlock(
            "bad_input", "tsset: `" + tssetFmt + "' and `" + cand +
                             "' are alternatives — specify one period.")));
        return blocks;
      }
      tssetFmt = cand;
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kTssetKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(handlers::runTsset(parsed.varlist[0], session, tssetDelta, tssetFmt));
    return blocks;
  }

  // ------------------------------------------------------------------
  // M3b: clear / set obs / cd / pwd / noop — the foundational surface the
  // `clear -> set obs N -> gen` data-construction idiom (and every do-file
  // header's `set more off` / `version 16`) depends on.
  if (cmd == "clear") {
    // BS-044 (BETA campaign C4): forward the raw subcommand token (if any)
    // — runClear itself now does the full per-subcommand scoping (kept
    // arity-capped to a single token by kFixedArityCommands above).
    std::string clearSub = !parsed.varlist.empty() ? parsed.varlist[0] : std::string();
    blocks.push_back(ojson(handlers::runClear(clearSub, session)));
    return blocks;
  }

  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "graphics") {
    // BS-047 (BETA A4): `set graphics on|off` is now genuinely tracked —
    // graph_m4.cpp's twoway/graph_combine builders read session.graphicsOff
    // to populate the wire-schema `suppressed` field (previously hardcoded
    // false; see graph_m4.cpp's own comment at both call sites).
    std::string arg = parsed.varlist.size() > 1 ? lower(parsed.varlist[1]) : std::string();
    if (arg.empty()) {
      std::smatch m;
      static const std::regex reSetGraphics(R"(\bset\s+graphics\s+(\S+))", std::regex::icase);
      if (std::regex_search(parsed.raw, m, reSetGraphics)) arg = lower(m.str(1));
    }
    if (arg == "off") {
      session.graphicsOff = true;
    } else if (arg == "on") {
      session.graphicsOff = false;
    }
    // Neither on/off (a malformed value) silently no-ops, matching this
    // arm's pre-existing "tracked but tolerant" behavior.
    return blocks;
  }

  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "obs") {
    std::smatch m;
    static const std::regex reSetObs(R"(\bset\s+obs\s+(\S+))", std::regex::icase);
    std::string nArg;
    if (std::regex_search(parsed.raw, m, reSetObs)) {
      nArg = m.str(1);
    } else if (parsed.varlist.size() > 1) {
      nArg = parsed.varlist[1];
    }
    blocks.push_back(ojson(handlers::runSetObs(nArg, session)));
    return blocks;
  }

  if (cmd == "set" && !parsed.varlist.empty() &&
      (lower(parsed.varlist[0]) == "level" || lower(parsed.varlist[0]) == "clevel")) {
    // BS-048 (BETA A4): `set level #` / `set clevel #` — the session
    // default confidence level, consulted ONLY when a command's own
    // `level(#)` option is absent (the existing --level flow:
    // buildEstimationArgvM4 / handlers.cpp::runRegress / runTtest).
    std::smatch m;
    static const std::regex reSetLevel(R"(\bset\s+(?:level|clevel)\s+(\S+))", std::regex::icase);
    std::string nArg = parsed.varlist.size() > 1 ? parsed.varlist[1] : std::string();
    if (std::regex_search(parsed.raw, m, reSetLevel)) nArg = m.str(1);
    bool ok = false;
    int lv = 0;
    try {
      size_t pos = 0;
      lv = std::stoi(nArg, &pos);
      ok = (pos == nArg.size());
    } catch (...) {
      ok = false;
    }
    if (!ok || lv < 1 || lv > 99) {
      blocks.push_back(ojson(eval::engineError(
          "set level(#) must be an integer between 1 and 99, e.g. `set level 90`.",
          "unsupported_option")));
      return blocks;
    }
    session.level = lv;
    return blocks;
  }

  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "seed") {
    // BS-046 (BETA A4): `set seed #` persistence + c(seed) exposure. No
    // cobra command in this build's dispatch surface yet consumes a
    // session-level default seed (wildboot/mediate/bootstrap land in later
    // D-phase waves, per the wildboot fixed-seed+distribution parity
    // strategy) — state-tracking + c(seed) readback only until then.
    std::smatch m;
    static const std::regex reSetSeed(R"(\bset\s+seed\s+(\S+))", std::regex::icase);
    std::string nArg = parsed.varlist.size() > 1 ? parsed.varlist[1] : std::string();
    if (std::regex_search(parsed.raw, m, reSetSeed)) nArg = m.str(1);
    bool ok = false;
    long long sv = 0;
    try {
      size_t pos = 0;
      sv = std::stoll(nArg, &pos);
      ok = (pos == nArg.size());
    } catch (...) {
      ok = false;
    }
    if (!ok) {
      blocks.push_back(ojson(eval::engineError(
          "set seed requires an integer argument, e.g. `set seed 12345`.", "unsupported_option")));
      return blocks;
    }
    session.seed = sv;
    return blocks;
  }

  if (cmd == "set" && !parsed.varlist.empty() &&
      (lower(parsed.varlist[0]) == "ty" || lower(parsed.varlist[0]) == "typ" ||
       lower(parsed.varlist[0]) == "type")) {
    // BUG-471 T2: `set type float|double [, permanently]` — the default storage
    // type of an untyped generate/egen (c(type); w2e_float_default.log
    // settype_double_gen :1252, settype_float_restore_gen :1292,
    // settype_abbrev_ty :1335 for the `ty` spelling). It was the generic
    // "ignored" echo below, so `set type double` changed nothing. `permanently`
    // is accepted and, like `set extcompat`, session-scoped: a do-file line
    // never rewrites the user's configuration.
    static const std::unordered_set<std::string> kSetTypeOpts = {"permanently"};
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kSetTypeOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    const std::string arg = parsed.varlist.size() == 2 ? parsed.varlist[1] : std::string();
    if (arg != "float" && arg != "double") {
      // settype_bad_rc :1314 — `set type int` is r(198) and leaves c(type) as it
      // was (settype_bad_ctype_unchanged :1324).
      blocks.push_back(ojson(eval::engineError(
          "set type: the default storage type must be float or double, e.g. `set type double`.",
          "unsupported_option")));
      return blocks;
    }
    session.defaultStorageType = arg;
    return blocks;
  }

  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "extcompat") {
    // WS-B (extcompat, 2026-08-06): `set extcompat on|off` — the session
    // switch for the 17 community-package compatibility names (the gate at
    // step 1a-bis above). SESSION SCOPE ONLY, deliberately: a headless
    // `--do` run must never rewrite the user's config file as a side effect
    // of a line in a script, so nothing here calls settings::save(). The
    // DURABLE choice is owned by the first-run consent dialog and the
    // Settings panel, both of which go through settings.set. This mirrors
    // real Stata, where a `set` without `, permanently` lasts the session.
    std::string arg = parsed.varlist.size() > 1 ? lower(parsed.varlist[1]) : std::string();
    if (arg.empty()) {
      std::smatch m;
      static const std::regex reSetExt(R"(\bset\s+extcompat\s+(\S+))", std::regex::icase);
      if (std::regex_search(parsed.raw, m, reSetExt)) arg = lower(m.str(1));
    }
    if (arg == "on") {
      settings::setExtcompatEnabled(true);
    } else if (arg == "off") {
      settings::setExtcompatEnabled(false);
    } else {
      // Unlike `set graphics` above, a malformed value here is NOT tolerated:
      // silently leaving the names enabled when a script asked for them off
      // would be the silent-wrong-answer class.
      blocks.push_back(ojson(eval::engineError(
          "set extcompat requires on or off, e.g. `set extcompat off`.", "unsupported_option")));
    }
    return blocks;
  }

  if (cmd == "set" && !parsed.varlist.empty()) {
    // BS-101 (BETA A4): every OTHER `set` subcommand (more, matsize, scheme,
    // trace, linesize, type, ...) is a real, common, harmless Stata idiom —
    // turning it into an error would halt do-files that use it, for no
    // PIET-side benefit. Previously a TOTALLY silent no-op; now an explicit,
    // non-halting note, so at least it's visible this was recognized and
    // deliberately ignored rather than swallowed without a trace.
    ojson note;
    note["type"] = "echo";
    note["text"] = "(set " + parsed.varlist[0] + ": not applicable to PIET — ignored)";
    blocks.push_back(note);
    return blocks;
  }

  if (cmd == "set" || cmd == "version" || cmd == "compress") {
    // BUG-336 (batch 2), compress only: it takes no options (honest no-op engine-side);
    // set/version parse their own subcommand grammar and are not option-shaped.
    if (cmd == "compress") {
      if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabCompress.known, kVocabCompress.unimplemented)) {
        blocks.push_back(ojson(*err));
        return blocks;
      }
    }
    // Bare `set` (no subcommand at all) / `version #` / `compress` — silent
    // accept-and-ignore, UNCHANGED (commands.py's "noop" native_handler) —
    // returns NO block at all, matching the oracle's `return None` handler
    // convention (only the echo block appears in output).
    return blocks;
  }

  if (cmd == "exit") {
    std::smatch m;
    static const std::regex reExit(R"(\bexit\b\s*(-?\d+)?)", std::regex::icase);
    int code = 0;
    if (std::regex_search(parsed.raw, m, reExit) && m[1].matched) {
      try {
        code = std::stoi(m.str(1));
      } catch (...) {
      }
    }
    std::string text = "exit" + (code != 0 ? (" " + std::to_string(code)) : std::string());
    blocks.push_back(ojson(eval::engineError(text, "program_exit")));
    return blocks;
  }

  if (cmd == "cd") {
    // BUG-251/WS-Q: was `rawLower.find("cd")` + `substr(pos + 2)`. `cd` is NOT
    // abbreviable in Stata (measured: `c /tmp` answers r(199)), so this one was
    // latent rather than live — but it is the same unguarded shape, one
    // registry edit away from the crash the `use` arm actually took.
    std::string dirArg = rawArgsAfterCmd(parsed.raw);
    blocks.push_back(ojson(handlers::runCd(dirArg, session)));
    return blocks;
  }

  if (cmd == "pwd") {
    blocks.push_back(ojson(handlers::runPwd(session)));
    return blocks;
  }

  if (cmd == "log") {
    ojson r = handlers::runLog(parsed.raw, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "which") {
    blocks.push_back(ojson(handlers::runWhich(parsed.raw, parsed.varlist)));
    return blocks;
  }

  if (cmd == "ssc") {
    ojson r = handlers::runSsc(parsed.varlist);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "confirm") {
    ojson r = handlers::runConfirm(parsed.raw, parsed.varlist, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "mkdir") {
    ojson r = handlers::runMkdir(parsed.raw, parsed.varlist, parsed.opts, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  // BUG-180 (P2-PREP): `rmdir DIR` had NO arm at all — see runRmdir
  // (handlers_m3.cpp) for the live-probed semantics and the benchmark-harness
  // failure that surfaced it.
  if (cmd == "rmdir") {
    ojson r = handlers::runRmdir(parsed.raw, parsed.varlist, parsed.opts, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "erase" || cmd == "rm") {
    // BS-045 (BETA campaign C4): `rm` is a genuine Stata built-in synonym
    // of `erase` (live-probed `which rm` -> "built-in command: rm"), not
    // an invented alias — same handler, same behavior.
    ojson r = handlers::runErase(parsed.raw, parsed.varlist, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  // AER-SCOPE1 bundle D: a handler may return one block, an EMPTY array (silent
  // success, the runSsc/runConfirm convention), or an array OF blocks. Pushing a
  // non-empty array as a single element is not a cosmetic mistake -- nlohmann
  // throws type_error.306 as soon as any consumer reads b["type"] off it, which
  // aborted the whole selftest binary. Flattened once here rather than per arm,
  // because the per-arm version is exactly the fix that got applied to two
  // handlers and missed the third.
  auto pushEnvResult = [&blocks](const ojson& r) {
    if (r.is_array()) {
      for (const auto& b : r) blocks.push_back(b);
    } else if (!r.is_null()) {
      blocks.push_back(r);
    }
  };

  // ==================== AER-SCOPE1 bundle D (ENV) arms ====================
  // Placed beside erase/mkdir/rmdir/ssc/which, the verbs they are siblings of.
  if (cmd == "shell") {
    pushEnvResult(handlers::runShell(rawArgsAfterCmd(parsed.raw), session));
    return blocks;
  }
  if (cmd == "winexec") {
    blocks.push_back(ojson(eval::refusedCommand(cmdTyped, "no_os_process")));
    return blocks;
  }
  if (cmd == "type") {
    pushEnvResult(handlers::runType(parsed.raw, parsed.varlist, parsed.opts, session));
    return blocks;
  }
  if (cmd == "dir" || cmd == "ls") {
    pushEnvResult(handlers::runDir(parsed.raw, parsed.varlist, parsed.opts, session));
    return blocks;
  }
  if (cmd == "net") {
    pushEnvResult(handlers::runNet(parsed.raw, parsed.varlist));
    return blocks;
  }
  if (cmd == "ado") {
    pushEnvResult(handlers::runAdo(parsed.raw, parsed.varlist));
    return blocks;
  }
  if (cmd == "adopath") {
    pushEnvResult(handlers::runAdopath(parsed.raw, parsed.varlist, session));
    return blocks;
  }
  if (cmd == "sysdir") {
    pushEnvResult(handlers::runSysdir(parsed.raw, parsed.varlist, session));
    return blocks;
  }
  if (cmd == "findfile") {
    pushEnvResult(handlers::runFindfile(parsed.raw, parsed.varlist, parsed.opts, session));
    return blocks;
  }
  if (cmd == "sleep") {
    pushEnvResult(handlers::runSleep(parsed.varlist));
    return blocks;
  }
  if (cmd == "error") {
    pushEnvResult(handlers::runErrorCode(parsed.varlist));
    return blocks;
  }
  if (cmd == "pause") {
    // `pause on|off` toggles Stata's interactive debugger. PIET is never
    // interactive, so the toggle can have no effect -- said out loud rather
    // than swallowed, because a do-file that pauses expects to stop.
    std::string sub = parsed.varlist.empty() ? "" : lower(parsed.varlist[0]);
    ojson note;
    note["type"] = "display";
    note["text"] = "(pause " + (sub.empty() ? std::string("") : sub) +
                   ": PIET is non-interactive -- no effect)";
    blocks.push_back(note);
    return blocks;
  }
  if (cmd == "cls") {
    // Nothing to clear: PIET's Results pane is the block stream itself, and a
    // command that erased it would erase its own evidence. Silent accept.
    return blocks;
  }
  if (cmd == "gr_edit") {
    ojson note;
    note["type"] = "display";
    note["text"] = "(gr_edit: graph-editor recording, appearance only -- ignored)";
    blocks.push_back(note);
    return blocks;
  }
  if (cmd == "bcal") {
    // Calendar reader is available; creation reports a public-beta refusal.
    ojson r_bc = handlers::runBcal(parsed, session);
    if (r_bc.is_array()) {
      for (const auto& b : r_bc) blocks.push_back(b);
    } else if (!r_bc.is_null()) {
      blocks.push_back(r_bc);
    }
    return blocks;
  }
  if (cmd == "twoway__histogram_gen") {
    blocks.push_back(ojson(eval::refusedCommand(cmdTyped, "stata_internal")));
    return blocks;
  }
  if (cmd == "frames") {
    std::string sub = parsed.varlist.empty() ? "" : lower(parsed.varlist[0]);
    if (sub == "reset") {
      pushEnvResult(handlers::runFramesReset(session));
      return blocks;
    }
    // Every other sub-command is `frame` under its plural spelling. Rewriting
    // and re-dispatching keeps ONE implementation of the frame family rather
    // than a second copy that can drift from it.
    std::string rewritten = parsed.raw;
    size_t at = rewritten.find("frames");
    if (at != std::string::npos) rewritten.replace(at, 6, "frame");
    return piet::dispatch(rewritten, session);
  }
  if (cmd == "tset") {
    // Unlike `tsset`, the pre-Stata-7 spelling also takes the PANEL form
    // `tset panelvar timevar` -- which is the only form the corpus uses.
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `tset` requires a time variable (or panelvar timevar)", "syntax_error")));
      return blocks;
    }
    if (parsed.varlist.size() == 1) {
      blocks.push_back(ojson(handlers::runTsset(parsed.varlist[0], session, std::nullopt)));
      return blocks;
    }
    blocks.push_back(
        ojson(handlers::runXtset(parsed.varlist[0], parsed.varlist[1], session, std::nullopt)));
    return blocks;
  }

  if (cmd == "timer") {
    // BS-124 (BETA campaign C4): "timer" was already in kNoDataCommands
    // above (correctly — it must work with zero data loaded) but had NO
    // dispatch arm at all, so every call fell through to the bottom
    // unsupported_command fallback. sub/slot parsing here is deliberately
    // minimal (matches the varlist shape `timer SUB [N]`); runTimer does
    // all real validation (slot range, state-machine rules).
    std::string timerSub = !parsed.varlist.empty() ? lower(parsed.varlist[0]) : std::string();
    std::optional<int> timerSlot;
    if (parsed.varlist.size() > 1) {
      try {
        size_t pos = 0;
        int n = std::stoi(parsed.varlist[1], &pos);
        if (pos == parsed.varlist[1].size()) timerSlot = n;
      } catch (...) {
        // leave timerSlot unset; runTimer treats a missing/garbled slot on
        // "on"/"off" as its own "invalid syntax" reject.
      }
    }
    ojson r = handlers::runTimer(timerSub, timerSlot, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  // BUG-343: `file open|write|read|close`. Registered under the first word with
  // sub-verb routing inside the handler — the same shape `export`/`import` use
  // (registry.cpp), because the command table has to recognize the LINE before
  // anything can look at what follows it. An empty result means "did its work,
  // says nothing", exactly like postfile below.
  if (cmd == "file") {
    ojson r = handlers::runFileCmd(parsed.raw, session);
    if (!(r.is_array() && r.empty()) && !r.is_null()) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "postfile") {
    ojson r = handlers::runPostfile(parsed.raw, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "post") {
    ojson r = handlers::runPost(parsed.raw, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "postclose") {
    std::string handle = parsed.varlist.empty() ? std::string() : parsed.varlist[0];
    ojson r = handlers::runPostclose(handle, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "drop") {
    // BS-097: `drop varlist` / `drop if COND` never had comma-options to
    // begin with (neither branch below reads parsed.opts) — any leftover
    // key is unconditionally unconsumed, so the known-set is the empty set.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (parsed.ifExpr.has_value() || parsed.inRange.has_value()) {
      // BUG-032: a string-referencing condition can't go to cobra's all-f64
      // eval --keepif; filter row-wise. Numeric stays cobra. BS-090 (BETA
      // Phase G): a co-occurring `in` is ANDed into the SAME row-wise mask
      // when the string detour fires (strmutate::filterRowsIf), and
      // composed via piet::handlers::applyIfAndInFilter otherwise.
      if (strmutate::needsStringCount(session, parsed.ifExpr)) {
        blocks.push_back(ojson(
            strmutate::filterRowsIf(session, *parsed.ifExpr, /*keep=*/false, parsed.inRange)));
        return blocks;
      }
      blocks.push_back(
          ojson(handlers::runDropIf(parsed.ifExpr.value_or(""), session, parsed.inRange)));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runDrop(parsed.varlist, session)));
    return blocks;
  }

  if (cmd == "keep") {
    // BS-097: same zero-consumption argument as `drop` immediately above.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (parsed.ifExpr.has_value() || parsed.inRange.has_value()) {
      if (strmutate::needsStringCount(session, parsed.ifExpr)) {
        blocks.push_back(ojson(
            strmutate::filterRowsIf(session, *parsed.ifExpr, /*keep=*/true, parsed.inRange)));
        return blocks;
      }
      blocks.push_back(
          ojson(handlers::runKeepIf(parsed.ifExpr.value_or(""), session, parsed.inRange)));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runKeep(parsed.varlist, session)));
    return blocks;
  }

  if (cmd == "sort") {
    // BUG-336 (batch 2): known set measured from the handler body -- runSort takes no options.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSort.known, kVocabSort.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runSort(parsed.varlist, session)));
    return blocks;
  }

  if (cmd == "gsort") {
    // BUG-336 (batch 2): known set measured from the handler body -- runGsort takes no options.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabGsort.known, kVocabGsort.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runGsort(parsed.varlist, session)));
    return blocks;
  }

  if (cmd == "order") {
    blocks.push_back(ojson(handlers::runOrder(parsed.varlist, parsed.opts, session)));
    return blocks;
  }

  if (cmd == "recode") {
    // BUG-336 (batch 2): runRecode parses gen()/generate() out of the RAW line
    // (reGen regex in handlers_data_verbs.cpp) -- a read invisible to any scan
    // of parsed.opts, which is how the first version of this guard wrongly
    // listed `generate` as unimplemented and broke the debug_all/pangram
    // stanzas that use `recode ..., gen(edu3)`. generate is KNOWN.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRecode.known, kVocabRecode.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // BS-031(b): parsed.ifExpr was already correctly extracted by the
    // tokenizer but never forwarded — see runRecode's own comment.
    blocks.push_back(ojson(handlers::runRecode(parsed.raw, session, parsed.ifExpr)));
    return blocks;
  }

  if (cmd == "collapse") {
    const int64_t collapseNBefore = rowsBeforeForRN(session);
    // MILF Step-1 D3: `parsed.weightVar` only survives the tokenizer's guard
    // here when weightType is fw/fweight (collapse is on the fweight
    // allow-list) — any other weight type was already rejected upstream.
    ojson collapseBlk = ojson(handlers::runCollapse(parsed.raw, parsed.ifExpr, session,
                                                    parsed.weightVar, parsed.inRange));
    if (collapseBlk.value("type", std::string()) != "error" && collapseNBefore >= 0)
      session.eval.rValues.set("N", static_cast<double>(collapseNBefore));
    blocks.push_back(std::move(collapseBlk));
    return blocks;
  }

  if (cmd == "encode") {
    std::string strvar = parsed.varlist.empty() ? std::string() : parsed.varlist[0];
    if (strvar.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "encode: syntax error — usage: encode STRVAR, gen(NEWVAR) [label(LBLNAME)]")));
      return blocks;
    }
    // BS-097: `generate`/`label` are the only encode options this arm ever
    // reads (real Stata also has `noextend`, not implemented here — a
    // leftover key now reads as an honest reject instead of a silent no-op).
    //
    // BUG-177 (P2-PREP): the canonical name registered here used to be the
    // ABBREVIATION `gen`, so Stata's own full spelling `encode grp,
    // generate(newv)` was rejected outright ("option `generate' not
    // allowed"). eval::resolveOptionAbbrev is one-sided by construction — it
    // resolves abbrev -> canonical only — so registering an abbreviation as
    // the canonical name makes the FULL spelling permanently unreachable,
    // the exact inverse of what the helper is for. Registering `generate`
    // and canonicalizing first fixes both directions at once: `gen`, `gene`,
    // ... `generate` all resolve, and the read below sees one spelling.
    static const std::unordered_set<std::string> kEncodeKnownOpts = {"generate", "label"};
    auto encodeOpts = parsed.opts;
    eval::canonicalizeOptionKeys(encodeOpts, kEncodeKnownOpts);
    std::optional<std::string> genVal, lblVal;
    for (const auto& [k, v] : encodeOpts) {
      if (k == "generate" && !v.isBool) genVal = v.strValue;
      if (k == "label" && !v.isBool) lblVal = v.strValue;
    }
    if (auto err = eval::rejectUnknownOptions(encodeOpts, kVocabEncode.known, kVocabEncode.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (!genVal.has_value() || genVal->empty()) {
      blocks.push_back(ojson(eval::engineError(
          "encode: ,gen(NEWVAR) is required — "
          "usage: encode STRVAR, gen(NEWVAR) [label(LBLNAME)]")));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runEncode(strvar, *genVal, lblVal, session, parsed.ifExpr)));
    return blocks;
  }

  // BS-003/D4 (BETA B-data): `destring`/`tostring` — PIET-NATIVE (no cobra
  // verb, no hbin-v2 dependency; handlers_data_verbs.cpp's runDestring/
  // runTostring ride the existing factor-column machinery). Scoped to
  // exactly ONE variable per call (every analysis.do site is single-var) —
  // a multi-var varlist is an honest reject naming the limitation, not a
  // silent partial application.
  if (cmd == "destring") {
    if (parsed.varlist.size() != 1) {
      blocks.push_back(ojson(eval::engineError(
          "destring: this build supports exactly one variable per call (loop `destring v1`, "
          "`destring v2`, ... for multiple variables)")));
      return blocks;
    }
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kDestringKnownOpts = kVocabDestring.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kDestringKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    bool doReplace = false, force = false;
    std::optional<std::string> genVar, ignoreChars;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "replace" && v.isBool && v.boolValue) doReplace = true;
      if ((k == "gen" || k == "generate") && !v.isBool) genVar = v.strValue;
      if (k == "ignore" && !v.isBool) ignoreChars = v.strValue;
      if (k == "force" && v.isBool && v.boolValue) force = true;
    }
    // parseOpts() captures a paren-value VERBATIM (it only tracks paren
    // depth, not Stata's own string-quoting) — real Stata's ignore() syntax
    // is always double-quoted (`ignore(",")`), so the raw strValue here is
    // literally `","` including the quote characters; strip them (same
    // convention the merge/joinby `using "FILE"` arms already use above) so
    // the character SET destring strips from each value is just `,`, not
    // `,` and `"`.
    if (ignoreChars.has_value() && ignoreChars->size() >= 2 && ignoreChars->front() == '"' &&
        ignoreChars->back() == '"') {
      ignoreChars = ignoreChars->substr(1, ignoreChars->size() - 2);
    }
    blocks.push_back(ojson(
        handlers::runDestring(parsed.varlist[0], doReplace, genVar, ignoreChars, force, session)));
    return blocks;
  }

  if (cmd == "tostring") {
    if (parsed.varlist.size() != 1) {
      blocks.push_back(ojson(eval::engineError(
          "tostring: this build supports exactly one variable per call (loop `tostring v1`, "
          "`tostring v2`, ... for multiple variables)")));
      return blocks;
    }
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kTostringKnownOpts = kVocabTostring.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kTostringKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    bool doReplace = false, useDisplayFormat = false;
    std::optional<std::string> genVar;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "replace" && v.isBool && v.boolValue) doReplace = true;
      if ((k == "gen" || k == "generate") && !v.isBool) genVar = v.strValue;
      if (k == "usedisplayformat" && v.isBool && v.boolValue) useDisplayFormat = true;
    }
    blocks.push_back(ojson(
        handlers::runTostring(parsed.varlist[0], doReplace, genVar, useDisplayFormat, session)));
    return blocks;
  }

  if (cmd == "reshape") {
    if (preflight::gate("reshape", session, "", 0, blocks) == preflight::Verdict::Cancel) return blocks;
    blocks.push_back(ojson(handlers::runReshape(parsed.varlist, parsed.opts, session)));
    return blocks;
  }

  if (cmd == "append") {
    // BS-097: this arm never reads parsed.opts at all today (only parsed.raw
    // via regex + parsed.varlist) — real Stata `append` options (nolabel,
    // generate()) are still rejected rather than silently dropped.
    // W:P5: `force` is accepted. In Stata it permits appending when a
    // variable's type differs between master and using (string vs numeric),
    // filling the using side's cells with missing of the master's type.
    // BUG-550: this was a no-op over the engine's promote-both-to-f64 default,
    // which is NOT what force does (string codes became numbers, and without
    // force the clash was rc 0 instead of r(106)). runAppend now passes
    // `--type-clash error|force` (w2_data_io.log app15/app16/app17).
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kAppendKnownOpts = kVocabAppend.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kAppendKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // B5-D10: `generate(NAME)` -- see kVocabAppend's registration comment.
    std::optional<std::string> appendGenVar, appendKeep;
    bool appendForce = false, appendNolabel = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "generate" && !v.isBool) appendGenVar = v.strValue;
      if (k == "keep") appendKeep = v.isBool ? std::string() : v.strValue;  // BUG-549
      if (k == "force") appendForce = true;  // BUG-550
      if (k == "nolabel") appendNolabel = true;  // BUG-551
    }
    static const std::regex reUsing(R"(\busing\s+(.+?)(?:\s*,|$))", std::regex::icase);
    std::smatch m;
    std::string filepath;
    if (std::regex_search(parsed.raw, m, reUsing)) {
      filepath = trimWs(m.str(1));
    } else if (parsed.varlist.size() >= 2 && lower(parsed.varlist[0]) == "using") {
      filepath = parsed.varlist[1];
    } else {
      blocks.push_back(ojson(eval::engineError(
          "append: `using` filename required — syntax: append using <file>")));
      return blocks;
    }
    if (preflight::gate("append", session, filepath, 0, blocks) == preflight::Verdict::Cancel) return blocks;
    blocks.push_back(ojson(handlers::runAppend(filepath, session, appendGenVar, appendKeep, appendForce, appendNolabel)));
    return blocks;
  }

  if (cmd == "merge") {
    static const std::regex reMergeType(R"(^\s*merge\s+(1:1|m:1|1:m)\s+(.+)$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(parsed.raw, m, reMergeType)) {
      blocks.push_back(ojson(eval::engineError(
          "merge: syntax error. Usage: merge 1:1|m:1|1:m KEYLIST using FILE [, opts]")));
      return blocks;
    }
    std::string mergeType = lower(m.str(1));
    std::string rest = m.str(2);
    // strip trailing options (`, opt...`) — split at the first comma
    // followed by a word char.
    static const std::regex reOptSplit(R"(,\s*(?=\w))");
    std::string restNoOpts;
    {
      std::smatch om;
      if (std::regex_search(rest, om, reOptSplit)) {
        restNoOpts = trimWs(rest.substr(0, static_cast<size_t>(om.position(0))));
      } else {
        restNoOpts = trimWs(rest);
      }
    }
    static const std::regex reUsingSplit(R"(\busing\b)", std::regex::icase);
    std::smatch usingM;
    if (!std::regex_search(restNoOpts, usingM, reUsingSplit)) {
      blocks.push_back(ojson(eval::engineError(
          "merge: missing `using FILE` clause. Usage: merge 1:1 id using \"file.hbin\"")));
      return blocks;
    }
    std::string keyPart = trimWs(restNoOpts.substr(0, static_cast<size_t>(usingM.position(0))));
    std::string filePart =
        trimWs(restNoOpts.substr(static_cast<size_t>(usingM.position(0) + usingM.length(0))));
    if (filePart.size() >= 2 && ((filePart.front() == '"' && filePart.back() == '"') ||
                                 (filePart.front() == '\'' && filePart.back() == '\''))) {
      filePart = filePart.substr(1, filePart.size() - 2);
    }
    std::vector<std::string> keylist;
    {
      std::istringstream iss(keyPart);
      std::string tok;
      while (iss >> tok) keylist.push_back(tok);
    }
    if (keylist.empty()) {
      blocks.push_back(
          ojson(eval::engineError("merge: no key variables specified before `using`")));
      return blocks;
    }
    if (filePart.empty()) {
      blocks.push_back(ojson(eval::engineError("merge: empty filename after `using`")));
      return blocks;
    }
    if (preflight::gate("merge", session, filePart, 0, blocks) == preflight::Verdict::Cancel) return blocks;
    blocks.push_back(ojson(handlers::runMerge(mergeType, keylist, filePart, parsed.opts, session)));
    return blocks;
  }

  // ------------------------------------------------------------------
  // frames (P6 wave) — `frame` is BOTH a command family (`frame put/drop`)
  // and a prefix (`frame NAME: CMD`), so the sub-token decides which. See
  // handlers_frames.cpp for the design; the current frame is always `default`
  // in this build and `frame create`/`change`/`pwf` are honest rejects, not
  // silent no-ops.
  if (cmd == "frame") {
    static const std::regex reFrame(R"(^\s*frame\s+(.+)$)", std::regex::icase);
    std::smatch fm;
    if (!std::regex_match(parsed.raw, fm, reFrame)) {
      blocks.push_back(ojson(eval::engineError(
          "frame: syntax error. Usage: frame put VARLIST [if], into(NAME) | frame drop NAME | "
          "frame NAME: COMMAND")));
      return blocks;
    }
    std::string rest = trimWs(fm.str(1));
    // `frame NAME: CMD` — the colon (outside quotes) marks the prefix form.
    // Checked BEFORE the sub-command split so a frame legitimately named
    // `drop` still reaches its own prefix arm rather than being read as the
    // drop sub-command.
    {
      static const std::regex reColon(R"(:)");
      std::smatch cm;
      if (eval::searchUnquoted(rest, reColon, cm)) {
        std::string frameName = trimWs(rest.substr(0, static_cast<size_t>(cm.position(0))));
        std::string innerCmd =
            trimWs(rest.substr(static_cast<size_t>(cm.position(0) + cm.length(0))));
        // A one-token name is the prefix form; anything else (e.g.
        // `frame put x y, into(f)` has no colon at all) never reaches here.
        if (!frameName.empty() && frameName.find_first_of(" \t") == std::string::npos) {
          if (innerCmd.empty()) {
            blocks.push_back(ojson(eval::engineError(
                "frame " + frameName + ": no command found after the colon")));
            return blocks;
          }
          ojson inner = handlers::runFramePrefix(frameName, innerCmd, session);
          if (inner.is_array()) {
            for (const auto& b : inner) blocks.push_back(b);
          } else {
            blocks.push_back(inner);
          }
          return blocks;
        }
      }
    }

    std::string sub;
    std::string subRest;
    {
      size_t sp = rest.find_first_of(" \t");
      if (sp == std::string::npos) {
        sub = lower(rest);
      } else {
        sub = lower(rest.substr(0, sp));
        subRest = trimWs(rest.substr(sp));
      }
    }

    if (sub == "create" || sub == "change" || sub == "cwf" || sub == "pwf" || sub == "rename" ||
        sub == "copy" || sub == "dir") {
      blocks.push_back(ojson(eval::unsupportedCommand(
          "frame " + sub, std::nullopt, std::nullopt, "piet_gap",
          eval::UnsupportedCommandKind::kNotImplemented)));
      return blocks;
    }

    if (sub == "drop") {
      if (subRest.empty()) {
        blocks.push_back(ojson(eval::engineError("frame drop: frame name required")));
        return blocks;
      }
      ojson r = handlers::runFrameDrop(subRest, session);
      if (!(r.is_array() && r.empty())) blocks.push_back(r);
      return blocks;
    }

    if (sub == "put") {
      // `frame put VARLIST [if COND], into(NAME)`
      auto [head, optsPart] = splitTopLevelComma(subRest);
      std::string varsPart = trimWs(head);
      std::optional<std::string> ifExpr;
      {
        static const std::regex reIf(R"(\s+if\s+(.+)$)", std::regex::icase);
        std::smatch im;
        if (eval::searchUnquoted(varsPart, reIf, im)) {
          ifExpr = trimWs(im.str(1));
          varsPart = trimWs(varsPart.substr(0, static_cast<size_t>(im.position(0))));
        }
      }
      std::vector<std::string> varlist;
      {
        std::istringstream iss(varsPart);
        std::string tok;
        while (iss >> tok) varlist.push_back(tok);
      }
      std::string into;
      for (const auto& [k, v] : parsed.opts) {
        if (lower(k) == "into") into = v.strValue;
      }
      // BUG-233: the one copy of this vocabulary now lives in the file-scope
      // registration block (see top of file); this binding keeps every existing
      // use — including the reject net below — reading the same set.
      const std::unordered_set<std::string>& kFramePutKnown = kVocabFramePut.known;
      if (auto err = eval::rejectUnknownOptions(parsed.opts, kFramePutKnown)) {
        blocks.push_back(ojson(*err));
        return blocks;
      }
      (void)optsPart;
      ojson r = handlers::runFramePut(varlist, ifExpr, into, session);
      if (!(r.is_array() && r.empty())) blocks.push_back(r);
      return blocks;
    }

    blocks.push_back(ojson(eval::engineError(
        "frame " + sub +
        ": unrecognized subcommand. This build supports `frame put`, `frame drop` and the "
        "`frame NAME: COMMAND` prefix.")));
    return blocks;
  }

  // `frlink m:1|1:1 KEY [KEY2 ...], frame(F) generate(G)`
  if (cmd == "frlink") {
    static const std::regex reFrlink(R"(^\s*frlink\s+(1:1|m:1)\s+(.+)$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(parsed.raw, m, reFrlink)) {
      blocks.push_back(ojson(eval::engineError(
          "frlink: syntax error. Usage: frlink m:1|1:1 KEY [KEY2 ...], frame(FRAMENAME) "
          "generate(NEWVAR)")));
      return blocks;
    }
    std::string linkType = lower(trimWs(m.str(1)));
    auto [keyHead, keyOpts] = splitTopLevelComma(trimWs(m.str(2)));
    (void)keyOpts;
    std::vector<std::string> keylist;
    {
      std::istringstream iss(trimWs(keyHead));
      std::string tok;
      while (iss >> tok) keylist.push_back(tok);
    }
    std::string frameName, genName;
    for (const auto& [k, v] : parsed.opts) {
      const std::string kl = lower(k);
      if (kl == "frame") frameName = v.strValue;
      if (kl == "generate" || kl == "gen") genName = v.strValue;
    }
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kFrlinkKnown = kVocabFrlink.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kFrlinkKnown)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runFrlink(linkType, keylist, frameName, genName, session)));
    return blocks;
  }

  // `frget NEW=VAR [NEW2=VAR2 ...], from(LINKVAR)`
  if (cmd == "frget") {
    static const std::regex reFrget(R"(^\s*frget\s+(.+)$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(parsed.raw, m, reFrget)) {
      blocks.push_back(ojson(eval::engineError(
          "frget: syntax error. Usage: frget NEWVAR=VAR [...], from(LINKVAR)")));
      return blocks;
    }
    auto [head, optsPart] = splitTopLevelComma(trimWs(m.str(1)));
    (void)optsPart;
    std::vector<std::pair<std::string, std::string>> renames;
    {
      std::istringstream iss(trimWs(head));
      std::string tok;
      while (iss >> tok) {
        size_t eq = tok.find('=');
        if (eq == std::string::npos) {
          // Bare `frget VAR` copies VAR under its own name.
          renames.emplace_back(tok, tok);
        } else {
          renames.emplace_back(trimWs(tok.substr(0, eq)), trimWs(tok.substr(eq + 1)));
        }
      }
    }
    std::string fromLink;
    for (const auto& [k, v] : parsed.opts) {
      if (lower(k) == "from") fromLink = v.strValue;
    }
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kFrgetKnown = kVocabFrget.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kFrgetKnown)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runFrget(renames, fromLink, session)));
    return blocks;
  }

  // BS-004/D5 (BETA B-data): `joinby key1 [key2 ...] using FILE` — bare form
  // (audit-confirmed syntax: `joinby [varlist] using filename [,
  // update replace unmatched(none|both|master|using) _merge(varname)
  // nolabel]`; analysis.do:451's own call uses zero options, the
  // unmatched(none) default). Mirrors the `merge` arm immediately above
  // (same raw-text `using`-split shape), minus merge's type-prefix capture
  // group (joinby has none).
  //
  // item3 (this lane, BUG-477 brief): `unmatched(none)` graduated to a real
  // acceptance below — it is provably the SAME operation as the bare form
  // (Stata's own default), not a new engine capability. `update`/`replace`/
  // `_merge()`/`nolabel`/`unmatched(master|both)` are still an honest reject
  // (D5: -> post-beta WP-18 for the first four; unmatched(master|both) needs
  // engine work this lane's brief scoped out of, see the session report) —
  // enforced by the known-options net below PLUS a value-level check for
  // `unmatched()` specifically, not a per-option allow-list.
  if (cmd == "joinby") {
    static const std::regex reJoinby(R"(^\s*joinby\s+(.+)$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(parsed.raw, m, reJoinby)) {
      blocks.push_back(ojson(eval::engineError(
          "joinby: syntax error. Usage: joinby key1 [key2 ...] using \"file\"")));
      return blocks;
    }
    std::string rest = m.str(1);
    // strip trailing options (`, opt...`) — split at the first comma
    // followed by a word char (same grammar the merge arm above uses).
    static const std::regex reOptSplitJoinby(R"(,\s*(?=\w))");
    std::string restNoOpts;
    {
      std::smatch om;
      if (std::regex_search(rest, om, reOptSplitJoinby)) {
        restNoOpts = trimWs(rest.substr(0, static_cast<size_t>(om.position(0))));
      } else {
        restNoOpts = trimWs(rest);
      }
    }
    static const std::regex reUsingSplitJoinby(R"(\busing\b)", std::regex::icase);
    std::smatch usingM;
    if (!std::regex_search(restNoOpts, usingM, reUsingSplitJoinby)) {
      blocks.push_back(ojson(eval::engineError(
          "joinby: missing `using FILE` clause. Usage: joinby key1 [key2 ...] using \"file\"")));
      return blocks;
    }
    std::string keyPart = trimWs(restNoOpts.substr(0, static_cast<size_t>(usingM.position(0))));
    std::string filePart =
        trimWs(restNoOpts.substr(static_cast<size_t>(usingM.position(0) + usingM.length(0))));
    if (filePart.size() >= 2 && ((filePart.front() == '"' && filePart.back() == '"') ||
                                 (filePart.front() == '\'' && filePart.back() == '\''))) {
      filePart = filePart.substr(1, filePart.size() - 2);
    }
    std::vector<std::string> keylist;
    {
      std::istringstream iss(keyPart);
      std::string tok;
      while (iss >> tok) keylist.push_back(tok);
    }
    if (keylist.empty()) {
      blocks.push_back(
          ojson(eval::engineError("joinby: no key variables specified before `using`")));
      return blocks;
    }
    if (filePart.empty()) {
      blocks.push_back(ojson(eval::engineError("joinby: empty filename after `using`")));
      return blocks;
    }
    // AER scope-2 W2 -> item3 (this lane): `unmatched(both|master|none)` is a
    // real Stata joinby option (32 uses across the 12 papers). The NAME is
    // known now; this build's `cobra joinby` (apps/cmds/cmds_data.cpp
    // cmd_joinby) implements exactly ONE join shape -- "for every master row,
    // emit one row per using match; a master row with zero using matches is
    // DROPPED entirely" (its own doc comment) -- and a using-only row is never
    // visited at all. That IS `unmatched(none)`'s definition (and it is what
    // bare joinby, with no unmatched() at all, already computes), so `none`
    // is accepted and simply proceeds. `unmatched(master)` needs the engine to
    // ALSO keep unmatched MASTER rows (using columns missing) and
    // `unmatched(both)` needs that plus unmatched USING rows and a `_merge`
    // marker (measured, StataBE 19.5: both -> 74 rows + _merge, master/none
    // -> 64 rows on the b5_io_pins.log fixture) -- neither is buildable from
    // this handler alone, so both stay honest, named refusals rather than a
    // silent alias to the wrong join (Doctrine gate ① — "answers a different
    // question").
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabJoinby.known,
                                              kVocabJoinby.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    for (const auto& [k, v] : parsed.opts) {
      if (k != "unmatched") continue;
      const std::string val = v.isBool ? std::string() : strutil::lower(strutil::trim(v.strValue));
      if (val == "none") break;  // this build's bare join already IS unmatched(none)
      if (val == "master" || val == "both") {
        blocks.push_back(ojson(eval::engineError(
            // The phrase "valid Stata option" is load-bearing, not decoration: the W2 net pins
            // it (selftest.cpp, "refused as a real-but-unwired Stata option -- not as a typo")
            // because that is the whole reason `unimplemented` is a separate bucket from "not a
            // member". Naming what is missing is an improvement ON TOP of that classification,
            // not a replacement for it -- dropping the phrase is how this change first went RED.
            "joinby: unmatched(" + val +
                ") is a valid Stata option that this build cannot honour yet: its joinby would "
                "have to keep the rows it currently drops" +
                (val == "both" ? " (plus a _merge marker)" : "") +
                ". unmatched(none) is implemented and is Stata's own default shape.",
            "unsupported_option")));
        return blocks;
      }
      blocks.push_back(ojson(eval::engineError(
          "joinby: unmatched(" + (v.isBool ? std::string() : v.strValue) +
              ") is not one of both, master or none",
          "unsupported_option")));
      return blocks;
    }
    if (preflight::gate("joinby", session, filePart, 0, blocks) == preflight::Verdict::Cancel) return blocks;
    blocks.push_back(ojson(handlers::runJoinby(keylist, filePart, session)));
    return blocks;
  }

  if (cmd == "duplicates") {
    std::string sub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
    std::optional<std::vector<std::string>> subVarlist;
    if (parsed.varlist.size() > 1) {
      subVarlist = std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end());
    }
    std::optional<std::string> genvar;
    for (const auto& [k, v] : parsed.opts) {
      if ((k == "gen" || k == "generate") && !v.isBool) {
        genvar = v.strValue;
        break;
      }
    }
    // BS-097: `gen`/`generate` are the only options this arm reads (real
    // Stata `duplicates drop` also has `force`, not implemented here).
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kDuplicatesKnownOpts = kVocabDuplicates.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kDuplicatesKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runDuplicates(sub, subVarlist, genvar, session, parsed.ifExpr,
                                                   parsed.inRange)));
    return blocks;
  }

  if (cmd == "count") {
    // BS-097: `count [if COND]` has no comma-options at all (neither branch
    // below reads parsed.opts) — known-set is the empty set.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // BUG-032: a condition over a factor (string) column / string fn / literal
    // can't go to cobra's all-f64 eval --keepif; count row-wise. Numeric stays
    // cobra (mirrors dispatcher.py's count handler). BS-090 (BETA Phase G): a
    // co-occurring `in` is ANDed into the SAME row-wise mask when this
    // string detour fires.
    if (parsed.ifExpr.has_value() && strmutate::needsStringCount(session, parsed.ifExpr)) {
      strmutate::CountResult cr = strmutate::countRowsIf(session, *parsed.ifExpr, parsed.inRange);
      if (!cr.ok) {
        blocks.push_back(ojson(eval::engineError(
            cr.errMsg, cr.inRangeError ? "unsupported_option" : "engine_error")));
        return blocks;
      }
      session.eval.rValues.set("N", static_cast<double>(cr.n));
      ojson out;
      out["type"] = "display";
      out["text"] = std::to_string(cr.n);
      blocks.push_back(out);
      return blocks;
    }
    blocks.push_back(ojson(handlers::runCount(parsed.ifExpr, session, parsed.inRange)));
    return blocks;
  }

  if (cmd == "display") {
    // BUG-105/WS-K: cmdTyped, NOT cmd. This finds the command token inside the
    // raw line; with the canonical spelling, `dis 2+2` would search for
    // "display" in "dis 2+2", get npos, and index off the end of the string.
    // BUG-251/WS-Q: stop searching entirely — the args are simply what follows
    // the leading token. `dis(2+2)` still yields `(2+2)`: rawArgsAfterCmd ends
    // the token at the first non-identifier character, exactly as the
    // `find(cmdTyped) + size()` arithmetic did.
    std::string rawArgs = rawArgsAfterCmd(parsed.raw);
    blocks.push_back(ojson(handleDisplay(rawArgs, session)));
    return blocks;
  }

  // BUG-105 abbreviation stopgap: `l` (see the `describe`/`d` note above).
  // `l` joins the `list` REGISTRY ROW's abbrev list, so `l in 1/5` clears
  // the tokenizer's registry-driven `in` gate exactly as `list in 1/5` does
  // (that gate resolves the command token through registry::findByName, so
  // a dispatcher-only alias would route but then be rejected for `in`).
  if (cmd == "list" || cmd == "browse" || cmd == "br" || cmd == "l") {
    // BUG-336 (batch 2): known set measured from the handler body -- runList renders a GUI grid: noobs/clean are presentation flags whose effect the grid already
    // satisfies (output byte-identical, measured) and the p8 wave + maxperf corpus use them.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabList.known, kVocabList.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // B5-D6: `separator`/`sep(#)` is parsed (same strict-numeric idiom as
    // summarize's separator above) rather than blindly discarded, so a
    // non-numeric argument still gets an honest reject. The value itself is
    // then dropped: runList's grid draws no every-N-rows rule (same
    // validated-no-op class as noobs/clean, see kVocabList's comment).
    // W1a-1: `abbreviate(#)` takes the same road -- the number is checked, the
    // grid has no header width to apply it to (kVocabList's comment).
    for (const auto& [k, v] : parsed.opts) {
      if (k != "separator" && k != "abbreviate") continue;
      if (v.isBool) {
        blocks.push_back(ojson(eval::engineError(
            "option `" + k + "` requires a number, e.g. " + k + (k == "separator" ? "(5)" : "(8)"),
            "unsupported_option")));
        return blocks;
      }
      try {
        size_t used = 0;
        (void)std::stol(v.strValue, &used);
        if (used != v.strValue.size()) throw std::invalid_argument("trailing");
      } catch (const std::exception&) {
        blocks.push_back(ojson(eval::engineError(
            "option `" + k + "()` incorrectly specified — invalid number", "unsupported_option")));
        return blocks;
      }
    }
    // BUG-508: a varlist naming a variable this frame does not have used to be
    // DROPPED in silence — `list a nosuchcol` rendered one column at rc 0 and
    // said nothing, which is the failure mode this campaign ranks worst: the
    // run is GREEN and the table is wrong. Stata is r(111) with the name
    // (oracle tests/eval/golden/bug508_varlist_oracle.log: L1/L2 for a bare
    // name, L5 for a wildcard that matches nothing, and rc 0 for `alpha*` and
    // `alpha-beta`, which is why this cannot be a plain has() loop).
    //
    // Shape copied from the `unab` arm below, including its lock: the resident
    // card is the cheap source but it is not always live, and "no card" must
    // NOT mean "skip the check". Wildcards and `a-b` ranges go through
    // piet::loops::expandVarlist so `x*` means here exactly what it means to
    // `foreach ... of varlist` — the expander's own header forbids a second
    // opinion. The expander does not validate a bare unknown name (it passes
    // straight through; unab's lock records the same), so that half is checked
    // against the frame here.
    if (!parsed.varlist.empty()) {
      std::vector<std::string> knownVars;
      if (const std::vector<VarInfo>* card = session.residentReadCard()) {
        for (const auto& vi : *card) knownVars.push_back(vi.name);
      } else if (Datastore* ldsp = session.datastore()) {
        knownVars = ldsp->names();
      } else {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return blocks;
      }
      std::string listSpec;
      for (const auto& t : parsed.varlist) {
        if (!listSpec.empty()) listSpec += " ";
        listSpec += t;
      }
      auto lvr = piet::loops::expandVarlist(listSpec, session);
      if (lvr.isError) {
        // The measured trigger is a wildcard matching nothing, and Stata names
        // the TOKEN the user typed (`variable zz* not found`), not the pattern
        // machinery. Find the one that matched nothing rather than blaming the
        // first wildcard in the list.
        for (const auto& t : parsed.varlist) {
          if (t.empty() || t.back() != '*') continue;
          const std::string pre = t.substr(0, t.size() - 1);
          bool any = false;
          for (const auto& k : knownVars) {
            if (k.rfind(pre, 0) == 0) { any = true; break; }
          }
          if (!any) {
            blocks.push_back(ojson(eval::engineError("variable " + t + " not found")));
            return blocks;
          }
        }
        blocks.push_back(lvr.error);
        return blocks;
      }
      for (const auto& t : lvr.tokens) {
        if (std::find(knownVars.begin(), knownVars.end(), t) == knownVars.end()) {
          blocks.push_back(ojson(eval::engineError("variable " + t + " not found")));
          return blocks;
        }
      }
    }
    std::optional<std::vector<std::string>> vl;
    if (!parsed.varlist.empty()) vl = parsed.varlist;
    blocks.push_back(ojson(handlers::runList(vl, parsed.ifExpr, parsed.inRange, session)));
    return blocks;
  }

  // BS-005/D6 (BETA B-data): `format VARLIST %fmt` — a display-layer session
  // attribute honored by list/browse (runList threads session's format map
  // into Datastore::browse() above); NOT persisted into hbin (post-beta
  // P1/hbin-v2). The tokenizer has no special handling for a `%`-prefixed
  // token, so `format date_order %td` lands as parsed.varlist ==
  // ["date_order", "%td"] — the LAST token is the format spec, everything
  // before it is the varlist (real Stata's own `format varlist %fmt` shape;
  // multiple variables may share one trailing %fmt).
  if (cmd == "format") {
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError("format: syntax is `format varlist %fmt`")));
      return blocks;
    }
    std::string fmt = parsed.varlist.back();
    std::vector<std::string> fmtVarlist(parsed.varlist.begin(), parsed.varlist.end() - 1);
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // runFormat returns an empty array as the silent-success sentinel (Stata's
    // `format` prints nothing); pushing it as a block would make downstream
    // block processing call .value("type") on an array → cpp_exception. Guard
    // it exactly like the other silent handlers (matrix/label/log arms).
    ojson fr = handlers::runFormat(fmtVarlist, fmt, session);
    if (!(fr.is_array() && fr.empty())) blocks.push_back(fr);
    return blocks;
  }

  if (cmd == "scalar" && !parsed.varlist.empty() &&
      (lower(parsed.varlist[0]) == "list" || lower(parsed.varlist[0]) == "dir")) {
    std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
    blocks.push_back(ojson(handlers::runScalarList(names, session)));
    return blocks;
  }

  // `assert EXP [if] [in]` (P4 wave). The claim is the RAW text after the
  // command word, not the tokenized varlist: `assert x1 > 0 & x2 < 5` is one
  // expression, and rebuilding it from tokens would lose the user's spacing
  // inside string comparisons. parsed.raw is the same source the `if`-qualifier
  // scanner already worked from, so the two agree on where the claim ends.
  if (cmd == "assert") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // BUG-428/432: `assert EXPR` carries its claim in the VARLIST, not in an
    // `if`, so the substitution point above never saw it — which is how
    // `assert Q[1,1] == 1` reported an unknown VARIABLE for a defined matrix.
    std::optional<ojson> claimErr;
    std::string claim = substituteRowExprRefs(joinSpace(parsed.varlist), session, &claimErr);
    if (claimErr.has_value()) {  // BUG-456: `assert _b[nosuch]==0` r(111), `assert e(cmd)==1` r(109)
      blocks.push_back(*claimErr);
      return blocks;
    }
    if (claim.empty()) {
      blocks.push_back(ojson(eval::engineError("assert: no expression given", "syntax_error")));
      return blocks;
    }
    ojson ar = handlers::runAssert(claim, session, parsed.ifExpr, parsed.inRange);
    // Silent success returns an empty array; pushing that would put a non-block
    // into the block list (same guard the `matrix` arm above uses).
    if (!(ar.is_array() && ar.empty())) blocks.push_back(ar);
    return blocks;
  }

  // BS-039 (BETA campaign C3): bare `ereturn list` only — display-only e()
  // dump (handlers_m3.cpp::runEreturnList). Any OTHER `ereturn ...` form
  // (scalar/post/eclass creation, BS-109) falls through unchanged to the
  // pre-existing generic unsupported_command reject below.
  if (cmd == "ereturn" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "list") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runEreturnList(session)));
    return blocks;
  }

  // ==================== AER-SCOPE1 bundle C (GTOOLS) arms =================
  // Reached only for the forms the translation step could NOT fold onto a
  // native command: `gstats` is a multiplex over two different natives, and
  // `gquantiles` in its `_pctile`/`pctile` form has no native twin at all
  // (the `xtile` form was already rewritten to `xtile` upstream).
  if (cmd == "gstats") {
    ojson r = handlers::runGstats(parsed, session);
    // `gstats sum` renders summarize's table, so it must also PUBLISH what
    // summarize publishes. r() is filled at the dispatcher call site, not
    // inside the handler (the summarize arm above does exactly this), so a
    // forwarding arm that only pushed the block would draw the same table and
    // leave r(N)/r(mean) empty -- and `gstats sum x` followed by
    // `display r(mean)` is precisely how the corpus uses it.
    if (r.is_object() && r.value("type", std::string()) != "error") {
      eval::captureRFromBlock(r, session.eval);
    }
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }
  if (cmd == "gquantiles") {
    ojson r = handlers::runGQuantilesPctile(parsed, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  // ==================== AER-SCOPE1 bundle B (RESULTS) arms ================
  if (cmd == "return") {
    std::string sub = parsed.varlist.empty() ? "" : lower(parsed.varlist[0]);
    if (sub == "list") {
      if (auto err = eval::rejectUnknownOptions(parsed.opts, {}, {"all"})) {
        blocks.push_back(ojson(*err));
        return blocks;
      }
      blocks.push_back(ojson(handlers::runReturnList(session)));
      return blocks;
    }
    if (sub == "clear") {
      session.eval.rValues.clear();
      // rStrings doubles as the s() store: `sreturn local` writes there under
      // kSClassKeyPrefix. Stata's `return clear` clears r() ONLY (`sreturn
      // clear` is its own command), so wiping the whole dict would silently
      // destroy every value an rclass/sclass program published. runReturnList
      // already filters these rows out when listing; the clear side has to
      // agree with it.
      std::vector<std::string> rKeys;
      for (const auto& kv : session.eval.rStrings.items()) {
        // Tested on the reserved lead byte rather than on a third copy of the
        // "\x01s:" literal: program.cpp:60 and eval/macros.cpp already keep two
        // copies that a comment says must stay byte-identical, and a third would
        // be one more place for them to diverge. Any \x01-prefixed key is a
        // reserved store, never a user r() macro.
        if (!kv.first.empty() && kv.first[0] == '\x01') continue;
        rKeys.push_back(kv.first);
      }
      for (const auto& k : rKeys) session.eval.rStrings.erase(k);
      // r-class matrices go with the scalars: `return clear` clears all of r().
      session.eval.rMatrices.clear();
      return blocks;
    }
    if (sub == "add") {
      // At the top level there is no program to return from, so the values are
      // already r() and `add` is a no-op. Inside one, it copies the CURRENT r()
      // into the staging that the program's return will flush -- which is what
      // makes `foo` then `return add` inside `bar` compose.
      if (session.progCallDepth > 0) {
        for (const auto& kv : session.eval.rValues.items()) {
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%.17g", kv.second);
          session.returnStaging[kv.first] = {"scalar", std::string(buf)};
        }
        for (const auto& kv : session.eval.rStrings.items()) {
          if (!kv.first.empty() && kv.first[0] == '\x01') continue;  // reserved store
          session.returnStaging[kv.first] = {"local", kv.second};
        }
      }
      return blocks;
    }
    // `return scalar|local` reaches program::handleReturnStmt long before this
    // arm (dispatcher step (e), above), so anything still here is a form PIET
    // does not implement -- named as an unknown SUBCOMMAND, because the command
    // itself now exists.
    blocks.push_back(ojson(eval::unsupportedCommand(
        "return " + sub, std::nullopt, std::nullopt, "piet_gap",
        eval::UnsupportedCommandKind::kUnknownSubcommand)));
    return blocks;
  }
  if (cmd == "matlist") {
    blocks.push_back(ojson(handlers::runMatlist(parsed, session)));
    return blocks;
  }
  if (cmd == "svmat") {
    blocks.push_back(ojson(handlers::runSvmat(parsed, session)));
    return blocks;
  }
  if (cmd == "getmata") {
    // runGetmata returns exactly one object (the data_mutation card or one
    // error block), never an array.
    blocks.push_back(ojson(handlers::runGetmata(parsed, session)));
    return blocks;
  }
  if (cmd == "mkmat") {
    // Silent on success, so the empty array must be FILTERED, not pushed: an
    // array reaching the block stream is read with value("type") downstream and
    // throws type_error.306. This is the same defect class that aborted the
    // selftest binary in bundle D, recurring here on a fresh arm -- which is why
    // every silent arm in this file carries this exact guard.
    ojson r = handlers::runMkmat(parsed, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "matrix" || cmd == "mat") {
    // BUG-105/WS-K: runMatrixDef re-parses the raw line with its own
    // `mat(?:rix)?` patterns, so hand it a canonical leading token.
    ojson r = handlers::runMatrixDef(rawWithCanonicalCmd(parsed.raw, cmdTyped, cmd), session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);  // silent on success
    return blocks;
  }

  if (cmd == "label" && !parsed.varlist.empty()) {
    // BUG-524: canonicalise the subcommand ONCE, here, against the floor
    // table (resolveLabelSub above); every branch below compares the full
    // spelling. Below a floor is Stata's "invalid syntax" r(198)
    // (label_d_too_short, label_va_too_short) and the data are untouched; a
    // token that is not a prefix of any documented subcommand keeps the
    // unknown-subcommand refusal (BUG-105 arm 3 pins that wording; Stata's own
    // rc for it is unmeasured, so it stays what it was).
    //
    // This resolution now runs BEFORE the generic option-vocabulary reject
    // below (BUG-527): `label language NAME, new` needs the resolved
    // subcommand to intercept `, new` on its own terms (oracle rc 0), and
    // that decision has to be made before the generic net answers rc 198
    // for it.
    const std::string subTyped = lower(parsed.varlist[0]);
    std::string sub;
    switch (resolveLabelSub(subTyped, &sub)) {
      case LabelSubMatch::kResolved:
        break;
      case LabelSubMatch::kTooShort:
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax",
                                                "label " + subTyped, std::nullopt, std::nullopt,
                                                "r(198)")));
        return blocks;
      case LabelSubMatch::kUnknown:
        blocks.push_back(ojson(eval::unsupportedCommand(
            "label " + subTyped, std::nullopt, std::nullopt, "piet_gap",
            eval::UnsupportedCommandKind::kUnknownSubcommand)));
        return blocks;
    }
    // BUG-527 (W2 labelglued fix round; oracle w2_cmdtriage.log:345-349,
    // arm lang_new): `label language NAME, new` is rc 0 -- "(language NAME
    // now current language)" -- not the generic option-vocabulary refusal.
    // kVocabLabel still lists `new` as unimplemented (below) so every OTHER
    // subcommand keeps answering the honest "valid Stata option, not yet
    // implemented" rc 198 for a stray `, new`; only `label language ..., new`
    // is intercepted here, before that generic net runs, and only for the
    // exact single-option shape the oracle measured. PIET keeps labels in
    // ONE language, so this does not open a second language store -- it
    // only makes the accept-and-name-it-current answer match Stata's rc,
    // the same way `label language default` already does two arms above.
    if (sub == "language" && parsed.varlist.size() >= 2 && parsed.opts.size() == 1 &&
        parsed.opts[0].first == "new") {
      const std::string& langName = parsed.varlist[1];
      ojson d;
      d["type"] = "display";
      d["text"] = "(language " + langName + " now current language)";
      blocks.push_back(d);
      return blocks;
    }
    // BUG-336 (batch 2): known set measured from the handler body -- runLabelDefine reads add/modify from the raw tail; other label subcommands take none.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabLabel.known, kVocabLabel.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (sub == "variable") {
      if (parsed.varlist.size() < 2) {
        blocks.push_back(ojson(eval::engineError("label variable: missing varname")));
        return blocks;
      }
      std::string varname = parsed.varlist[1];
      // Text is everything after the varname token in the raw line, quotes stripped.
      // BUG-105/WS-K: typed spelling, not the literal — see the `save` arm's
      // comment. `labe variable x "t"` threw std::out_of_range here.
      // BUG-251/WS-Q: this one searched for the command word only to slice
      // FROM it — i.e. to strip leading whitespace that eval::tokenize() has
      // already stripped. `raw.substr(npos)` throws unconditionally, with no
      // wrap-around to hide it. trimWs() is the whole of what it wanted.
      std::string rawArgs = trimWs(parsed.raw);
      // strip "label variable VARNAME" prefix (3 tokens) — split on whitespace,
      // keep the 4th+ as text (mirrors commands.py's maxsplit=3).
      size_t p = 0;
      for (int k = 0; k < 3 && p != std::string::npos; ++k) {
        p = rawArgs.find_first_of(" \t", p);
        if (p != std::string::npos) p = rawArgs.find_first_not_of(" \t", p);
      }
      std::string text = (p == std::string::npos) ? "" : trimWs(rawArgs.substr(p));
      if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') ||
                               (text.front() == '\'' && text.back() == '\''))) {
        text = text.substr(1, text.size() - 2);
      }
      ojson lv = handlers::runLabelVariable(varname, text, session);
      if (!(lv.is_array() && lv.empty())) blocks.push_back(lv);
      return blocks;
    }
    if (sub == "values") {
      if (parsed.varlist.size() < 2) {
        blocks.push_back(ojson(eval::engineError("label values: requires varname [lblname]")));
        return blocks;
      }
      std::optional<std::string> lblname;
      if (parsed.varlist.size() >= 3) lblname = parsed.varlist[2];
      ojson lv = handlers::runLabelValues(parsed.varlist[1], lblname, session);
      if (!(lv.is_array() && lv.empty())) blocks.push_back(lv);
      return blocks;
    }
    if (sub == "define") {
      // BUG-105/WS-K: runLabelDefine anchors on `^label\s+define\s+`.
      // BUG-524: ... so the typed subcommand (`def`, `de`) is canonicalised in
      // the raw line too, or `label def yn2 0 "no"` would fail its own anchor.
      ojson lv = handlers::runLabelDefine(
          rawWithCanonicalLabelSub(rawWithCanonicalCmd(parsed.raw, cmdTyped, cmd), subTyped, sub),
          session);
      if (!(lv.is_array() && lv.empty())) blocks.push_back(lv);
      return blocks;
    }
    if (sub == "list") {
      std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
      blocks.push_back(ojson(handlers::runLabelList(names, session)));
      return blocks;
    }
    if (sub == "data") {
      // help label (label.sthlp): "attaches a label (up to 80 characters) to
      // the dataset in memory ... If no label is specified, any existing label
      // is removed." session.dataLabel is the field the .dta writer already
      // carries (handlers_data_verbs_internal.h); this is its first in-session
      // mutator, so `label data` no longer stops a do-file (14 corpus uses in
      // 5 papers). Not an oracle arm -- the definition is the help file's.
      // Text is everything after the subcommand token, quotes stripped (the
      // `label variable` branch's own slicing, one token earlier).
      std::string rawArgs = trimWs(parsed.raw);
      size_t p = 0;
      for (int k = 0; k < 2 && p != std::string::npos; ++k) {
        p = rawArgs.find_first_of(" \t", p);
        if (p != std::string::npos) p = rawArgs.find_first_not_of(" \t", p);
      }
      std::string text = (p == std::string::npos) ? "" : trimWs(rawArgs.substr(p));
      if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
      }
      session.dataLabel = text;
      return blocks;  // silent success, as `label variable` is
    }
    if (sub == "language") {
      // Oracle lang_bare / lang_default / lang_undefined: the bare query
      // answers r(language)=default, r(languages)=default; naming the current
      // language is rc 0 "(default already current language)"; any other name
      // is r(111) "language X not defined". PIET keeps labels in ONE language,
      // so `default` is the only name that exists here. `, new` (lang_new) is
      // rc 0 in Stata and is intercepted ABOVE, before kVocabLabel's generic
      // reject runs, so it reaches neither this branch nor the r(111) below
      // (BUG-527); `, rename` / `, delete` are unmeasured and stay refused by
      // name through kVocabLabel's unimplemented set.
      if (parsed.varlist.size() == 1) {
        session.eval.rValues = eval::OrderedFloatDict();
        session.eval.rStrings.clear();
        session.eval.rStrings.set("language", "default");
        session.eval.rStrings.set("languages", "default");
        ojson d;
        d["type"] = "display";
        d["text"] =
            "Language for variable and value labels\n\n"
            "    In this dataset, value and variable labels have been defined in only one "
            "language:  default\n\n"
            "    To create new language:         . label language <name>, new\n"
            "    To rename current language:     . label language <name>, rename";
        blocks.push_back(d);
        return blocks;
      }
      const std::string& langName = parsed.varlist[1];
      if (langName == "default") {
        ojson d;
        d["type"] = "display";
        d["text"] = "(default already current language)";
        blocks.push_back(d);
        return blocks;
      }
      blocks.push_back(ojson(eval::errorBlock("engine_error", "language " + langName + " not defined",
                                              std::nullopt, std::nullopt, std::nullopt, "r(111)")));
      return blocks;
    }
    if (sub == "dir") {
      blocks.push_back(ojson(handlers::runLabelDir(session)));
      return blocks;
    }
    if (sub == "drop") {
      std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
      if (names.empty()) {
        blocks.push_back(ojson(eval::engineError("label drop: specify at least one label name")));
        return blocks;
      }
      ojson lv = handlers::runLabelDrop(names, session);
      if (!(lv.is_array() && lv.empty())) blocks.push_back(lv);
      return blocks;
    }
    // BUG-524: what is left resolved to a documented subcommand PIET has no
    // store for (copy, rename, save). A recognised feature, refused by NAME --
    // never accepted-and-ignored, which for `label save` would look like a
    // file was written.
    blocks.push_back(ojson(eval::unsupportedCommand(
        "label " + sub, std::nullopt, std::nullopt, "piet_gap",
        eval::UnsupportedCommandKind::kNotImplemented)));
    return blocks;
  }

  // P1-W2-0: `labelbook [LBLNAME...]` — a top-level command (not a `label`
  // subcommand), so parsed.varlist holds the optional label-set names
  // directly (no subcommand-token stripping, unlike the `label list` arm
  // above).
  if (cmd == "labelbook") {
    // BUG-336 (batch 2): known set measured from the handler body -- runLabelbook takes no options.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabLabelbook.known, kVocabLabelbook.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // BUG-373: Stata publishes the label-set names it walked as r(names).
    ojson lbBlk = ojson(handlers::runLabelbook(parsed.varlist, session));
    if (lbBlk.value("type", std::string()) != "error") {
      std::string names;
      for (const auto& kv : session.labelDefs) {
        if (!names.empty()) names += " ";
        names += kv.first;
      }
      session.eval.rStrings.set("names", names);
    }
    blocks.push_back(std::move(lbBlk));
    return blocks;
  }

  if (cmd == "export" || cmd == "import") {
    // Two-word I/O verbs: `export delimited|excel ... using FILE` and
    // `import delimited|excel [using] FILE`. The sub-verb decides the format;
    // anything else is an honest reject naming what this build supports.
    static const std::regex reSub(R"(^\s*(?:export|import)\s+(\w+)\b\s*(.*)$)",
                                  std::regex::icase);
    std::smatch m;
    if (!std::regex_search(parsed.raw, m, reSub)) {
      blocks.push_back(ojson(eval::engineError(
          cmdTyped + ": expected `" + cmdTyped + " delimited ...` or `" + cmdTyped +
          " excel ...`")));
      return blocks;
    }
    std::string sub = lower(m.str(1));
    std::string rest = trimWs(m.str(2));
    if (sub != "delimited" && sub != "excel") {
      blocks.push_back(ojson(eval::engineError(
          cmdTyped + " " + sub + ": not supported — this build implements `" + cmdTyped +
          " delimited` and `" + cmdTyped + " excel`.")));
      return blocks;
    }
    // Split `[varlist] using "FILE"` (export) / `[using] "FILE"` (import). The
    // option comma-tail is already stripped into parsed.opts.
    std::string spec = rest;
    {
      size_t comma = spec.find(',');
      if (comma != std::string::npos) spec = spec.substr(0, comma);
      spec = trimWs(spec);
    }
    std::vector<std::string> words;
    {
      std::istringstream iss(spec);
      std::string w;
      while (iss >> w) words.push_back(w);
    }
    std::string pathArg;
    std::vector<std::string> varlist;
    bool sawUsing = false;
    for (const auto& w : words) {
      if (lower(w) == "using") { sawUsing = true; continue; }
      if (sawUsing || cmd == "import") pathArg = w;
      else varlist.push_back(w);
    }
    if (pathArg.empty() && !varlist.empty() && cmd == "import") {
      pathArg = varlist.back();
      varlist.pop_back();
    }
    if (pathArg.empty()) {
      blocks.push_back(ojson(eval::engineError(
          cmdTyped + " " + sub + ": missing the file. Usage: " + cmdTyped + " " + sub +
          (cmd == "export" ? " [varlist] using \"file\"" : " using \"file\""))));
      return blocks;
    }
    // BUG-336 (batch 2): known sets measured from the four handler bodies
    // (hasFlagOpt/parseDelimiterOpt reads). `clear` is KNOWN on import because
    // PIET's import always replaces the in-memory dataset -- the flag's
    // semantic is satisfied, not dropped.
    {
      // AER scope-2 W2: the four branch sets moved to file scope (one copy
      // each) so kVocabImport/kVocabExport can be their per-command union.
      // The branch net still decides; nothing about the sets changed.
      std::unordered_set<std::string> known, unimpl;
      if (cmd == "export" && sub == "delimited") {
        known = kExportDelimKnown;
        unimpl = kExportDelimUnimpl;
      } else if (cmd == "export") {
        known = kExportExcelKnown;
        unimpl = kExportExcelUnimpl;
      } else if (sub == "delimited") {
        known = kImportDelimKnown;
        unimpl = kImportDelimUnimpl;
      } else {
        known = kImportExcelKnown;
        unimpl = kImportExcelUnimpl;
      }
      if (auto err = eval::rejectUnknownOptions(parsed.opts, known, unimpl)) {
        blocks.push_back(ojson(*err));
        return blocks;
      }
    }
    if (cmd == "export") {
      blocks.push_back(ojson(sub == "delimited"
                                 ? handlers::runExportDelimited(varlist, pathArg, parsed.opts, session)
                                 : handlers::runExportExcel(varlist, pathArg, parsed.opts, session)));
    } else {
      blocks.push_back(ojson(sub == "delimited"
                                 ? handlers::runImportDelimited(pathArg, parsed.opts, session)
                                 : handlers::runImportExcel(pathArg, parsed.opts, session)));
    }
    return blocks;
  }

  if (cmd == "expand") {
    // AER scope-2 W2: `generate(newvar)` is a real Stata expand option this
    // build does not have, so it now says so instead of "not allowed".
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabExpand.known,
                                              kVocabExpand.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    std::optional<std::string> expandGen;
    for (const auto& [k, v] : parsed.opts)
      if (k == "generate" && !v.isBool) expandGen = strutil::trim(v.strValue);
    // The count is the rest of the line after the verb (a bare number here).
    static const std::regex reExpand(R"(^\s*expand\s+(.+)$)", std::regex::icase);
    std::smatch m;
    std::string arg;
    if (std::regex_search(parsed.raw, m, reExpand)) arg = trimWs(m.str(1));
    // B4-W3b: the regex above takes the whole rest of the line, so with an
    // option present it captured "2, generate(dupflag)" and runExpand rejected
    // it as a per-observation expression. The count is what precedes the option
    // comma.
    {
      size_t comma = arg.find(',');
      if (comma != std::string::npos) arg = trimWs(arg.substr(0, comma));
    }
    {
      // The constant form is the only one this build runs (runExpand says so);
      // a non-numeric argument is modelled as x2 for the pre-flight only.
      char* end = nullptr;
      const long long k = arg.empty() ? 0 : std::strtoll(arg.c_str(), &end, 10);
      const int64_t factor = (end && *end == '\0' && k > 0) ? static_cast<int64_t>(k) : 2;
      if (preflight::gate("expand", session, "", factor, blocks) == preflight::Verdict::Cancel) return blocks;
    }
    blocks.push_back(ojson(handlers::runExpand(arg, expandGen, session)));
    return blocks;
  }

  if (cmd == "fillin") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    const int64_t fillinNBefore = rowsBeforeForRN(session);
    ojson fillinBlk = ojson(handlers::runFillin(parsed.varlist, session));
    if (fillinBlk.value("type", std::string()) != "error" && fillinNBefore >= 0)
      session.eval.rValues.set("N", static_cast<double>(fillinNBefore));
    blocks.push_back(std::move(fillinBlk));
    return blocks;
  }

  if (cmd == "contract") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    const int64_t contractNBefore = rowsBeforeForRN(session);
    ojson contractBlk = ojson(handlers::runContract(parsed.varlist, session));
    if (contractBlk.value("type", std::string()) != "error" && contractNBefore >= 0)
      session.eval.rValues.set("N", static_cast<double>(contractNBefore));
    blocks.push_back(std::move(contractBlk));
    return blocks;
  }

  if (cmd == "cross") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    static const std::regex reCross(R"(^\s*cross\s+using\s+(.+)$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(parsed.raw, m, reCross)) {
      blocks.push_back(ojson(eval::engineError(
          "cross: missing `using FILE` clause. Usage: cross using \"file\"")));
      return blocks;
    }
    if (preflight::gate("cross", session, trimWs(m.str(1)), 0, blocks) == preflight::Verdict::Cancel) return blocks;
    blocks.push_back(ojson(handlers::runCross(trimWs(m.str(1)), session)));
    return blocks;
  }

  if (cmd == "winsor") {
    blocks.push_back(ojson(handlers::runWinsorCox(parsed, session)));
    return blocks;
  }

  if (cmd == "isid") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // Success is SILENT (Stata prints nothing) — the handler returns an empty
    // array, which must not be pushed as a block (same shape as preserve/
    // restore above).
    ojson r = handlers::runIsid(parsed.varlist, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "datasignature") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // BUG-373: the signature itself is the whole point of the command, and it
    // was printed and not published — a do-file that compares signatures across
    // a run could not read it back.
    ojson dsBlk = ojson(handlers::runDatasignature(session));
    if (dsBlk.value("type", std::string()) == "display")
      session.eval.rStrings.set("datasignature", trimWs(dsBlk.value("text", std::string())));
    blocks.push_back(std::move(dsBlk));
    return blocks;
  }

  if (cmd == "preserve") {
    ojson r = handlers::runPreserve(session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "restore") {
    // B5-D14 (BUG-454): this arm used to call NO reject net at all, so ANY
    // option was silently accepted and dropped -- adding the net here is
    // itself a behavior change (an unknown option on `restore` is now an
    // honest r(198) reject instead of rc 0).
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRestore.known,
                                              kVocabRestore.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    bool rearm = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "preserve" && v.isBool && v.boolValue) rearm = true;
    }
    ojson r = handlers::runRestore(session, rearm);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "levelsof") {
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError("levelsof: variable name required")));
      return blocks;
    }
    std::optional<std::string> localName;
    bool clean = false;
    // BS-029 (BETA Phase C batch C1): `missing`/`separate()` are now
    // implemented (previously honest-rejected as unknown options).
    bool missing = false;
    std::optional<std::string> separateSep;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "local" && !v.isBool) localName = v.strValue;
      if (k == "clean" && v.isBool && v.boolValue) clean = true;
      if (k == "missing" && v.isBool && v.boolValue) missing = true;
      if (k == "separate" && !v.isBool) separateSep = v.strValue;
    }
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kLevelsofKnownOpts = kVocabLevelsof.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kLevelsofKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    ojson result = ojson(
        handlers::runLevelsof(parsed.varlist[0], localName, clean, parsed.ifExpr, session));
    if (result.value("type", "") == "levelsof" && result.contains("levels") &&
        result["levels"].is_array()) {
      // handlers::runLevelsof (handlers_m3.cpp — outside this batch's file
      // scope) has no missing/separate support at all: its numeric/factor
      // scan explicitly drops NaN/negative codes (missing) and always
      // space-joins. Both are applied here as a post-process over its
      // `levels` array instead of duplicating that scan. `missing` appends
      // the dataset's own missing marker (".") after the ordinary
      // (non-missing) level list — Stata sorts missing last, so simple
      // append-at-end is correct; `separate(SEP)` re-joins whatever level
      // list results with SEP instead of the default space.
      std::vector<std::string> levelList;
      for (const auto& l : result["levels"]) levelList.push_back(l.get<std::string>());
      if (missing) {
        // NOTE: checks the dataset's UNFILTERED missingness — combining
        // `missing` with an `if` that itself changes whether the variable
        // still has missing rows is a known, unexercised edge case (the
        // oracle's `missing` probe carries no `if`).
        Datastore* ds = session.datastore();
        if (ds != nullptr && ds->has(parsed.varlist[0]) && ds->nMissing(parsed.varlist[0]) > 0) {
          levelList.push_back(".");
        }
      }
      std::string sep = separateSep.has_value() ? *separateSep : " ";
      std::string joined;
      for (size_t i = 0; i < levelList.size(); ++i) {
        if (i) joined += sep;
        joined += levelList[i];
      }
      result["levels"] = ojson(levelList);
      result["text"] = joined;
      // r(levels): Stata stores levelsof's level list as a STRING r-class
      // result, read back via the `` `r(levels)' `` macro-delimited form
      // (eval/macros.cpp's rStrings lookup) — this codebase never actually
      // wired that up (runLevelsof only ever sets the OPTIONAL local()
      // name, never r(levels) itself); wire it here since this is the only
      // site that knows the FINAL (missing/separate-adjusted) joined text.
      session.eval.rStrings.set("levels", joined);
      // BUG-373: Stata also publishes the counts. r(r) is the number of DISTINCT
      // levels and r(N) the number of observations they were drawn from — the two
      // a macro-driven do-file actually loops on.
      session.eval.rValues.set("r", static_cast<double>(levelList.size()));
      if (std::optional<int64_t> lvRows = session.residentRowsIfCurrent())
        session.eval.rValues.set("N", static_cast<double>(*lvRows));
      else if (Datastore* lvDs = session.datastore())
        session.eval.rValues.set("N", static_cast<double>(lvDs->nRows()));
      // Re-set local(name) too, in case runLevelsof already set it above
      // with the PRE-adjustment (missing/separate-unaware) text.
      if (localName.has_value() && !localName->empty()) {
        session.eval.locals.set(*localName, joined);
      }
    }
    blocks.push_back(result);
    return blocks;
  }

  // ------------------------------------------------------------------
  // M4a: output-completion surface (ROADMAP M4a item 1 — data-summary output)

  if (cmd == "winsor2") {
    double cutLo = 1.0, cutHi = 99.0;
    std::string suffix = "_w";
    std::string byStr;
    bool trim = false, replace = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "cuts" && !v.isBool) {
        std::istringstream iss(v.strValue);
        std::vector<std::string> parts;
        std::string tok;
        while (iss >> tok) parts.push_back(tok);
        if (parts.size() == 2) {
          // BUG-112(4): strict-consume each number (mirrors the engine-side collapse
          // pNN fix @9f86577 — C1-F11 literal discipline) — `cuts(5x 95)` must reject,
          // not silently read "5x" as 5.
          size_t consumedLo = 0, consumedHi = 0;
          bool ok = true;
          try {
            cutLo = std::stod(parts[0], &consumedLo);
            cutHi = std::stod(parts[1], &consumedHi);
          } catch (...) {
            ok = false;
          }
          if (!ok || consumedLo != parts[0].size() || consumedHi != parts[1].size()) {
            blocks.push_back(ojson(eval::engineError(
                "winsor2: cuts(" + v.strValue + ") — expected two numbers")));
            return blocks;
          }
        } else {
          blocks.push_back(ojson(eval::engineError(
              "winsor2: cuts() requires exactly two values, got '" + v.strValue + "'")));
          return blocks;
        }
      } else if (k == "suffix" && !v.isBool) {
        suffix = v.strValue;
      } else if (k == "by" && !v.isBool) {
        byStr = v.strValue;
      } else if (k == "trim" && v.isBool && v.boolValue) {
        trim = true;
      } else if (k == "replace" && v.isBool && v.boolValue) {
        replace = true;
      }
    }
    // BS-097: cuts/suffix/by/trim/replace are the only winsor2 options this
    // arm reads (community-package command; real winsor2 also has `label`).
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kWinsor2KnownOpts = kVocabWinsor2.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kWinsor2KnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError("winsor2: no variables specified")));
      return blocks;
    }
    // BUG-373: winsor2 publishes r(r1)/r(r2) — the two CUT VALUES it winsorised
    // at, i.e. the cutLo/cutHi percentiles of the (last) variable. Verified
    // against the live StataBE capture on beertax: r(r1) 0.04766949 and r(r2)
    // 2.41525412, which are exactly its p1 and p99. Computed here, before the
    // mutation, because afterwards the column has already been clipped to them.
    std::vector<double> w2cuts;
    {
      Datastore* w2ds = session.datastore();
      if (w2ds != nullptr && w2ds->has(parsed.varlist.back()) &&
          w2ds->dtype(parsed.varlist.back()) == "f64") {
        const double* col = w2ds->col(parsed.varlist.back()).f64();
        const int64_t wn = w2ds->nRows();
        std::vector<double> v;
        v.reserve(static_cast<size_t>(wn));
        for (int64_t i = 0; i < wn; ++i)
          if (!std::isnan(col[i])) v.push_back(col[i]);
        std::sort(v.begin(), v.end());
        // Stata's percentile rule, the same one Frame::xtile and summarize,detail
        // use: an exact rank boundary averages the two straddling values.
        auto pct = [&v](double p) {
          const int64_t n = static_cast<int64_t>(v.size());
          if (n == 0) return std::numeric_limits<double>::quiet_NaN();
          if (n == 1) return v[0];
          const double Pi = static_cast<double>(n) * p / 100.0;
          const double fl = std::floor(Pi);
          if (fl == Pi && Pi >= 1 && static_cast<int64_t>(Pi) < n)
            return (v[static_cast<size_t>(Pi) - 1] + v[static_cast<size_t>(Pi)]) / 2.0;
          int64_t idx = static_cast<int64_t>(fl) + 1;
          if (idx > n) idx = n;
          return v[static_cast<size_t>(idx) - 1];
        };
        if (!v.empty()) { w2cuts.push_back(pct(cutLo)); w2cuts.push_back(pct(cutHi)); }
      }
    }
    ojson w2Blk = ojson(handlers::runWinsor2(session, parsed.varlist, cutLo, cutHi, trim, byStr,
                                             suffix, replace));
    if (w2Blk.value("type", std::string()) != "error" && w2cuts.size() == 2) {
      session.eval.rValues.set("r1", w2cuts[0]);
      session.eval.rValues.set("r2", w2cuts[1]);
    }
    blocks.push_back(std::move(w2Blk));
    return blocks;
  }

  // BUG-105 abbreviation stopgap: `ta` (see the `describe`/`d` note above).
  if (cmd == "tabulate" || cmd == "tab" || cmd == "ta" || cmd == "tab1" || cmd == "tab2") {
    if (!session.hasData()) {
      blocks.push_back(ojson(eval::noDataLoaded()));
      return blocks;
    }
    bool chi2 = false;
    std::optional<std::string> sumvar;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "chi2" && v.isBool && v.boolValue) chi2 = true;
      if ((k == "sum" || k == "summarize") && !v.isBool) sumvar = v.strValue;
    }
    // BS-097: chi2/sum/summarize/missing/generate/sort are the tabulate/tab1/
    // tab2 options this arm actually READS (real Stata also has nolabel/
    // matcell()/plot/exact/gamma/taub/V, not implemented here). `cell`/`col`/
    // `row`/`nofreq` are in kVocabTabulate's KNOWN set (rc 0, matching the
    // oracle) but are validated no-ops here on purpose — see kVocabTabulate's
    // own comment for the measured/unmeasured boundary that decision rests on.
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kTabulateKnownOpts = kVocabTabulate.known;
    // BUG-371: `missing` graduates from "option not allowed" to implemented — it
    // makes missing an ordinary category instead of a dropped row.
    bool tabMissing = false;
    for (const auto& [k, v] : parsed.opts)
      if (k == "missing" && (!v.isBool || v.boolValue)) tabMissing = true;
    // B5-D-TAB-SORT: `sort` (one-way only; see kVocabTabulate's own comment
    // and runTabulate/tabulateOne's `sortByFreq` param for the implementation
    // and its cited oracle cell).
    bool tabSort = false;
    for (const auto& [k, v] : parsed.opts)
      if (k == "sort" && (!v.isBool || v.boolValue)) tabSort = true;
    // W1a-1: `nolabel` -- "display numeric codes rather than value labels".
    // Read here, applied at the BUG-355 substitution seam below, which is the
    // only place this arm consults session.valueLabels: one flag, one consumer.
    bool tabNoLabel = false;
    for (const auto& [k, v] : parsed.opts)
      if (k == "nolabel" && (!v.isBool || v.boolValue)) tabNoLabel = true;
    // B4-W3b: `generate(stub)`. Read BEFORE the reject net so the net still
    // owns the accept/reject decision (the vocabulary above carries the name).
    std::string tabGenStub;
    for (const auto& [k, v] : parsed.opts)
      if (k == "generate" && !v.isBool) tabGenStub = strutil::trim(v.strValue);
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kTabulateKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError("tabulate requires at least one variable")));
      return blocks;
    }
    Datastore* ds = session.datastore();
    // BUG-484: a column this frame does not have used to reach runTabulate,
    // where Columnar::col() throws EngineError("column not found: X") — and
    // that throw crossed the whole dispatch stack to main.cpp:382, which
    // replaced the ENTIRE run's blocks with one `run failed:` error. Not a
    // truncation: everything the run had already produced was discarded, at
    // exit 0, with stopped_at_line unset, and `capture` could not contain it.
    //
    // Nothing exotic is needed — `codebook` a few thousand lines up already
    // does exactly this, and the oracle says tabulate is not a special case:
    // `summarize nosuchcol` and `list nosuchcol` are r(111) in Stata too, and
    // so is `tabulate nosuchcol` (one-way, two-way, and with `, missing`).
    // "variable X not found" is the wording eval::coarseRc maps to 111.
    for (const auto& tvar : parsed.varlist) {
      if (!ds->has(tvar)) {
        blocks.push_back(ojson(eval::engineError("variable " + tvar + " not found")));
        return blocks;
      }
    }
    if (sumvar.has_value() && !sumvar->empty() && !ds->has(*sumvar)) {
      blocks.push_back(ojson(eval::engineError("variable " + *sumvar + " not found")));
      return blocks;
    }
    // BUG-042: `tabulate ... if COND` — runTabulate is pure computation over
    // a Datastore reference with no cobra call at all, so there's no
    // `--keepif` to forward; pre-filter into a temp hbin instead and hand
    // THAT Datastore to the (otherwise untouched) handler, mirroring the
    // identical if-prefilter already used for `twoway`/graph commands a few
    // hundred lines up in this same file (dispatcherNextPath + `cobra eval
    // --keepif` + a fresh Datastore over the filtered copy). BS-090 (BETA
    // Phase G): `in` now composes with `if` via AND here too
    // (piet::handlers::applyIfAndInFilter).
    std::unique_ptr<Datastore> tabIfDs;
    if ((parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
        (parsed.inRange.has_value() && !parsed.inRange->empty())) {
      handlers::InRangeFilterResult fr;
      try {
        fr = handlers::applyIfAndInFilter(session, ds->path(), parsed.ifExpr, parsed.inRange);
      } catch (const CobraError& ce) {
        blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
        return blocks;
      }
      if (!fr.ok) {
        blocks.push_back(ojson(handlers::inRangeRejectError(fr, *parsed.inRange, cmdTyped)));
        return blocks;
      }
      tabIfDs = std::make_unique<Datastore>(fr.dataPath);
      ds = tabIfDs.get();
    }
    // MILF Step-1 D3: `parsed.weightVar` only survives the tokenizer's guard
    // here when weightType is fw/fweight (tabulate/tab/tab1/tab2 are on the
    // fweight allow-list) — any other weight type was already rejected
    // upstream. The `if`-prefilter above (when present) already carries the
    // weight column through unchanged since it filters ROWS of the whole
    // dataset, not a chosen subset of columns.
    ojson tabBlock =
        handlers::runTabulate(*ds, parsed.varlist, chi2, cmd, sumvar, parsed.weightVar,
                              tabMissing, tabSort);
    // B4-W3b: the RAW level keys, snapshotted here — the value-label
    // substitution a few lines down rewrites tabBlock["table"]["rows"] in
    // place, and a dummy built from a LABEL ("Domestic") instead of its code
    // (0) would compare against a string the column does not hold.
    std::vector<std::string> tabRawRows;
    if (!tabGenStub.empty() && tabBlock.is_object() && tabBlock.contains("table") &&
        tabBlock["table"].is_object() && tabBlock["table"].contains("rows") &&
        tabBlock["table"]["rows"].is_array()) {
      for (const auto& r : tabBlock["table"]["rows"])
        if (r.is_string()) tabRawRows.push_back(r.get<std::string>());
    }
    // BUG-354 (W1 lens L3): `tabulate` is r-class and published nothing —
    // `di r(r)` answered "r(r) is not set". Stata (StataBE 19.5, `tabulate
    // rep78` on auto) sets r(r)=5 (the number of ROWS, i.e. distinct levels
    // shown) and r(N)=69 (the N of the tabulated sample, which EXCLUDES missing
    // — 69 not 74 on that fixture). Both are already in the block this handler
    // just built, so this is carriage at the seam that has them, not a re-count.
    // BUG-355 (W1 lens L4): `tabulate` ignored value labels while the
    // coeftable path applied them from the SAME session store — measured, one
    // session: `regress fatal i.g3` renders `mid.g3`/`high.g3` while
    // `tabulate g3` renders `1.0`/`2.0`/`3.0`. Stata labels the tabulate rows
    // (StataBE 19.5, `label values rep78 replab` -> rows read `three`/`four`
    // while the UNLABELED codes stay numeric). runTabulate takes a Datastore and
    // never sees the session, so the substitution belongs here, at the seam that
    // has both — the same arrangement `applyFactorValueLabels` uses for
    // coefficient rows.
    if (tabBlock.is_object() && tabBlock.contains("table") && tabBlock["table"].is_object() &&
        parsed.varlist.size() == 1 && !session.valueLabels.empty() && !tabNoLabel) {
      auto vlIt = session.valueLabels.find(parsed.varlist[0]);
      if (vlIt != session.valueLabels.end()) {
        auto relabel = [&](const std::string& raw) {
          try {
            const double d = std::stod(raw);
            const double r = std::nearbyint(d);
            // Only an INTEGRAL value can carry a value label; 1.5 has no code.
            if (std::fabs(d - r) > 1e-9) return raw;
            auto it = vlIt->second.find(static_cast<int>(r));
            // An unlabeled code keeps its numeric rendering — Stata's own mix.
            return it == vlIt->second.end() ? raw : it->second;
          } catch (const std::exception&) {
            return raw;  // non-numeric row label (a factor's level string)
          }
        };
        ojson& tbl = tabBlock["table"];
        if (tbl.contains("rows") && tbl["rows"].is_array()) {
          ojson newRows = ojson::array();
          ojson newVals = ojson::object();
          const bool haveVals = tbl.contains("values") && tbl["values"].is_object();
          for (const auto& r : tbl["rows"]) {
            const std::string oldKey = r.get<std::string>();
            const std::string newKey = relabel(oldKey);
            newRows.push_back(newKey);
            if (haveVals && tbl["values"].contains(oldKey)) newVals[newKey] = tbl["values"][oldKey];
          }
          tbl["rows"] = newRows;
          if (haveVals) tbl["values"] = newVals;
        }
      }
    }
    // BUG-373: tab1/tab2 emit `mode:"multi"` with a `tables` ARRAY, so the
    // single-table r-fill below never fired for them and they published nothing.
    // Stata reports the LAST table's counts, which is what a following
    // `display r(r)` reads.
    if (tabBlock.is_object() && tabBlock.contains("tables") && tabBlock["tables"].is_array() &&
        !tabBlock["tables"].empty()) {
      const auto& last = tabBlock["tables"].back();
      if (last.is_object() && last.contains("table") && last["table"].is_object()) {
        const auto& tbl = last["table"];
        if (tbl.contains("rows") && tbl["rows"].is_array())
          session.eval.rValues.set("r", static_cast<double>(tbl["rows"].size()));
        if (tbl.contains("values") && tbl["values"].is_object()) {
          double total = 0.0;
          for (auto it = tbl["values"].begin(); it != tbl["values"].end(); ++it)
            if (it.value().is_number()) total += it.value().get<double>();
          session.eval.rValues.set("N", total);
        }
      }
    }
    if (tabBlock.is_object() && tabBlock.contains("table") && tabBlock["table"].is_object()) {
      const auto& tbl = tabBlock["table"];
      if (tbl.contains("rows") && tbl["rows"].is_array()) {
        session.eval.rValues.set("r", static_cast<double>(tbl["rows"].size()));
      }
      // r(N) is the tabulated sample's own N — the SUM of the cell counts, which
      // is why it excludes missing (Stata: 69, not 74, on `tabulate rep78`).
      if (tbl.contains("values") && tbl["values"].is_object()) {
        double total = 0.0;
        for (auto it = tbl["values"].begin(); it != tbl["values"].end(); ++it)
          if (it.value().is_number()) total += it.value().get<double>();
        session.eval.rValues.set("N", total);
      }
    }
    // ------------------------------------------------------------------
    // B4-W3b: `tabulate VAR, generate(stub)` — one byte dummy per level,
    // named stub1..stubK in the table's own (sorted) level order.
    //
    // THE RULE THAT DECIDES THIS, and the reason an rc-only test cannot see
    // it: the dummies are MISSING where the source is missing, not 0. Measured
    // (StataBE 19.5, tests/eval/golden/aer_b4/milfpins_oracle_g.log:170-176,
    // `sysuse auto` + `tabulate rep78, generate(rs)`):
    //     summarize rs1, meanonly  ->  r(N) 69, NOT 74
    //     r(mean) = +1.dae6076b981dbX-006  (= 2/69, the 5 missing rep78 rows
    //                                       are OUT of the denominator)
    // Zero-filling instead would pass every count and every "did the variables
    // appear" check, and would move the coefficient of every regression that
    // uses one of them. So the expression is cond(missing(v), ., v==level) and
    // never a bare comparison.
    if (!tabGenStub.empty()) {
      ojson genErr = runTabulateGenerate(tabGenStub, parsed.varlist, tabRawRows, session);
      if (!genErr.is_null()) {
        blocks.push_back(genErr);
        return blocks;
      }
    }
    blocks.push_back(ojson(tabBlock));
    return blocks;
  }

  if (cmd == "table") {
    if (!session.hasData()) {
      blocks.push_back(ojson(eval::noDataLoaded()));
      return blocks;
    }
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError("table: requires at least one row variable")));
      return blocks;
    }
    // Stata 17 spells the dimensions `table (rowspec) (colspec)`. The tokenizer
    // hands them over with the parentheses attached, so a bare lookup reported
    // "variable not found: '(region)'". Strip one enclosing pair per token; a
    // token without them is the pre-17 spelling and passes through untouched.
    auto stripDimParens = [](const std::string& t) {
      return (t.size() >= 2 && t.front() == '(' && t.back() == ')') ? t.substr(1, t.size() - 2) : t;
    };
    std::string rowvar = stripDimParens(parsed.varlist[0]);
    std::optional<std::string> colvar;
    if (parsed.varlist.size() >= 2) colvar = stripDimParens(parsed.varlist[1]);
    // `statistic()` REPEATS in Stata 17 (`statistic(mean y) statistic(count y)`).
    // The old loop overwrote its single slot each pass, so a two-statistic
    // request silently answered with the last one only — the caller asked two
    // questions and got one answer with no indication the other was dropped.
    struct StatSpec { std::string stat; std::optional<std::string> var; };
    std::vector<StatSpec> statSpecs;
    for (const auto& [k, v] : parsed.opts) {
      if ((k == "statistic" || k == "stat") && !v.isBool) {
        std::istringstream iss(v.strValue);
        std::vector<std::string> parts;
        std::string tok;
        while (iss >> tok) parts.push_back(tok);
        if (!parts.empty()) {
          StatSpec sp;
          sp.stat = lower(parts[0]);
          if (parts.size() >= 2) sp.var = parts[1];
          statSpecs.push_back(std::move(sp));
        }
      }
    }
    if (statSpecs.empty()) statSpecs.push_back({"freq", std::nullopt});
    // BS-097: statistic/stat are the only `table` options this arm reads.
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kTableKnownOpts = kVocabTable.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kTableKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    Datastore* ds = session.datastore();
    // BUG-042: same if-prefilter as tabulate above (runTable is likewise
    // pure Datastore computation with no cobra --keepif to forward to).
    std::unique_ptr<Datastore> tableIfDs;
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
      std::string tableIfPath = dispatcherNextPath(session.workdir(), "tableif");
      // BUG-369 follow-up: `table x if L.y>0` — see the codebook arm above.
      std::vector<std::string> ifArgs = translate::injectTsContextForExpr(
          {"eval", "--data", ds->path(), "--keepif", *parsed.ifExpr, "--out", tableIfPath},
          *parsed.ifExpr, session);
      try {
        runCobraJson(ifArgs);
      } catch (const CobraError& ce) {
        blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
        return blocks;
      }
      tableIfDs = std::make_unique<Datastore>(tableIfPath);
      ds = tableIfDs.get();
    }
    // BUG-160 CLOSURE: ONE table, not one runTable() call (and one block) per
    // statistic. The single- and two-or-more-statistic requests share the
    // SAME underlying computation (runTableMulti, tabulate.cpp) — Total row/
    // column margins and value-label-aware headers, absent from both paths
    // before this fix — but keep their pre-existing JSON SHAPES: a lone
    // statistic() still renders through runTable's flat `stat`/`cells[row]=
    // value` reshape (every existing one-statistic consumer keeps reading
    // that shape, now with a "Total" entry and real labels), while 2+
    // statistics render through runTableMulti's own nested `stats[]`/
    // `cells[row][stat]=...` shape (previously TWO separate table_result
    // blocks with no shape at all to preserve).
    std::vector<std::pair<std::string, std::optional<std::string>>> multiSpecs;
    multiSpecs.reserve(statSpecs.size());
    for (const auto& sp : statSpecs) multiSpecs.emplace_back(sp.stat, sp.var);
    ojson tableBlk = (multiSpecs.size() == 1)
                         ? ojson(handlers::runTable(*ds, rowvar, colvar, multiSpecs[0].first,
                                                    multiSpecs[0].second, &session))
                         : ojson(handlers::runTableMulti(*ds, rowvar, colvar, multiSpecs,
                                                         &session));
    // BUG-373: table is r-class in Stata and published nothing. r(N) is the
    // tabulated sample's own count, which the block already carries.
    if (tableBlk.value("type", std::string()) != "error" && tableBlk.contains("n_obs") &&
        tableBlk["n_obs"].is_number())
      session.eval.rValues.set("N", tableBlk["n_obs"].get<double>());
    blocks.push_back(std::move(tableBlk));
    return blocks;
  }

  // ------------------------------------------------------------------
  // M4a: output-completion surface (ROADMAP M4a item 2 — estimation surface)

  if (cmd == "tabstat") {
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError("tabstat requires at least one variable")));
      return blocks;
    }
    std::optional<std::string> byVal, statsVal;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "by" && !v.isBool) byVal = v.strValue;
      // `stat` is real Stata's own abbreviation of `statistics()` (freeze
      // corpus proof: tests/kitchensink/debug_all.do:92 / class_demo.do:246
      // both use `stat(mean sd n)`) — previously unrecognized here, so it
      // silently fell through to the default stat list instead of being
      // honored OR named-rejected; recognizing it is required BEFORE adding
      // the reject net below (else this exact frozen invocation would flip
      // from silently-defaulting to a fresh reject, an over-rejection
      // regression against a captured freeze fixture).
      if ((k == "stat" || k == "stats" || k == "statistics") && !v.isBool) statsVal = v.strValue;
    }
    // BS-097: by/stat/stats/statistics are the only tabstat options this arm
    // reads (real Stata also has columns()/casewise/format/longstub/missing/
    // nototal/save/varwidth(), not implemented here) — mirrors the
    // corr/pwcorr/correlate arm immediately below.
    // W:P5: `columns(statistics|variables)` chooses which axis carries the
    // statistics in Stata's rendering. PIET emits one fixed layout, so this is
    // presentation-only here — accepted rather than rejected, because
    // rejecting a legal option that changes no number would fail a working
    // script for nothing. (The other unimplemented tabstat options —
    // casewise/longstub/nototal/save/varwidth — stay rejected: those DO change
    // which numbers appear.)
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kTabstatKnownOpts = kVocabTabstat.known;
    // B4-W3b: nototal / save.
    bool tsNoTotal = false, tsSave = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "nototal" && (!v.isBool || v.boolValue)) tsNoTotal = true;
      if (k == "save" && (!v.isBool || v.boolValue)) tsSave = true;
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kTabstatKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    ojson tsBlock = ojson(handlers::runTabstat(parsed.varlist, byVal, statsVal, session,
                                               parsed.ifExpr, tsSave || tsNoTotal, tsNoTotal));
    // ------------------------------------------------------------------
    // B4-W3b `save`: the r()-class matrices. Measured (StataBE 19.5,
    // milfpins_oracle_g.log:189-220, `sysuse auto` +
    // `tabstat price mpg, by(foreign) stat(mean sd n) nototal save`):
    //   r(Stat1)[3,2] and r(Stat2)[3,2] — one matrix PER GROUP, in the by-
    //     variable's own ascending order;
    //   rows are the STATISTICS in the order asked for, spelled Stata's way
    //     (Mean / SD / N), columns are the VARLIST in the order written
    //     (price, mpg);
    //   r(Stat1)[1,1] = +1.7b86c4ec4ec4fX+00c (Domestic mean price),
    //   r(Stat1)[3,1] = +1.a000000000000X+005 (= 52, the group N);
    //   r(name1) = "Domestic" — the group's LABEL, not its code.
    if (tsSave && tsBlock.is_object() && tsBlock.contains("groups") &&
        tsBlock["groups"].is_array()) {
      // Stata's row-stub spellings. A statistic with no entry here keeps the
      // engine's own key rather than being renamed to a guess.
      static const std::map<std::string, std::string> kStatStub = {
          {"mean", "Mean"}, {"sd", "SD"},   {"n", "N"},         {"count", "N"},
          {"min", "Min"},   {"max", "Max"}, {"sum", "Sum"},     {"p50", "p50"},
          {"median", "p50"}, {"var", "Variance"}, {"range", "Range"}};
      std::vector<std::string> statKeys;
      if (tsBlock.contains("stats") && tsBlock["stats"].is_array()) {
        for (const auto& st : tsBlock["stats"])
          if (st.is_string()) statKeys.push_back(st.get<std::string>());
      }
      std::vector<std::string> colVars;
      if (tsBlock.contains("vars") && tsBlock["vars"].is_array()) {
        for (const auto& vn : tsBlock["vars"])
          if (vn.is_string()) colVars.push_back(vn.get<std::string>());
      }
      auto buildStatMatrix = [&](const ojson& statsObj) {
        eval::EvalMatrix m;
        for (const auto& sk : statKeys) {
          auto it = kStatStub.find(sk);
          m.rownames.push_back(it == kStatStub.end() ? sk : it->second);
        }
        m.colnames = colVars;
        for (const auto& sk : statKeys) {
          std::vector<double> row;
          for (const auto& vn : colVars) {
            double cell = std::numeric_limits<double>::quiet_NaN();
            if (statsObj.contains(vn) && statsObj[vn].is_object() && statsObj[vn].contains(sk) &&
                statsObj[vn][sk].is_number()) {
              cell = statsObj[vn][sk].get<double>();
            }
            row.push_back(cell);
          }
          m.data.push_back(std::move(row));
        }
        return m;
      };
      // r() is replaced wholesale by each r-class command, so the Stat/name
      // family is cleared here rather than merged into whatever the previous
      // command left — a stale r(Stat3) from a three-group run would otherwise
      // outlive a two-group one and read as this command's answer.
      session.eval.rMatrices.clear();
      int gi = 0;
      for (const auto& g : tsBlock["groups"]) {
        if (!g.is_object() || !g.contains("stats")) continue;
        ++gi;
        session.eval.rMatrices.emplace_back("Stat" + std::to_string(gi),
                                            buildStatMatrix(g["stats"]));
        // The group's NAME: its value label when the session holds one for
        // this by-variable, and otherwise the value itself with no trailing
        // ".0" (Stata prints `1982`, not `1982.0`).
        std::string nm;
        if (g.contains("value")) {
          if (g["value"].is_string()) {
            nm = g["value"].get<std::string>();
          } else if (g["value"].is_number()) {
            const double gv = g["value"].get<double>();
            bool labelled = false;
            if (byVal.has_value()) {
              auto vlIt = session.valueLabels.find(*byVal);
              if (vlIt != session.valueLabels.end() &&
                  std::fabs(gv - std::nearbyint(gv)) < 1e-9) {
                auto lit = vlIt->second.find(static_cast<int>(std::nearbyint(gv)));
                if (lit != vlIt->second.end()) {
                  nm = lit->second;
                  labelled = true;
                }
              }
            }
            if (!labelled) {
              std::ostringstream os;
              if (std::fabs(gv - std::nearbyint(gv)) < 1e-9) {
                os << static_cast<long long>(std::nearbyint(gv));
              } else {
                os << gv;
              }
              nm = os.str();
            }
          }
        }
        session.eval.rStrings.set("name" + std::to_string(gi), nm);
      }
      // Stata also returns the overall matrix as r(StatTotal) whenever the
      // total row is present; `nototal` is precisely the option that removes
      // it, so this follows the row rather than the option name.
      if (!tsNoTotal && tsBlock.contains("total") && tsBlock["total"].is_object()) {
        session.eval.rMatrices.emplace_back("StatTotal", buildStatMatrix(tsBlock["total"]));
      }
      session.eval.rValues.set("nvars", static_cast<double>(colVars.size()));
    }
    blocks.push_back(tsBlock);
    return blocks;
  }

  // BUG-105 abbreviation stopgap: `cor` (see the `describe`/`d` note above).
  // Deliberately NOT `c`: real Stata's minimum abbreviation for `correlate`
  // is `cor` (`c` alone is ambiguous), and the BUGLOG finding names the
  // 3-letter spelling specifically.
  if (cmd == "corr" || cmd == "cor" || cmd == "pwcorr" || cmd == "correlate") {
    bool pairwise = false, sig = false, obs = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "pairwise" && v.isBool && v.boolValue) pairwise = true;
      if (k == "sig" && v.isBool && v.boolValue) sig = true;
      if (k == "obs" && v.isBool && v.boolValue) obs = true;
    }
    // BS-097: pairwise/sig/obs are the only corr/pwcorr/correlate options
    // this arm reads (real Stata also has means/covariance/kendall/star()/
    // bonferroni/sidak/print()/listwise/casewise, not implemented here).
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kCorrKnownOpts = kVocabCorr.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kCorrKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (cmd == "pwcorr") pairwise = true;

    // BS-028 (BETA Phase C batch C1): `obs` was previously mismapped
    // straight into `sig` — `correlate, obs` silently became `correlate,
    // sig`, which then hard-errors cobra's own "corr --sig requires
    // --pairwise" instead of real Stata's actual "option obs not allowed"
    // (oracle-pinned: rc=198); `pwcorr, obs` silently became `pwcorr, sig`
    // (showing p-values the user never asked for instead of the per-pair N
    // Stata actually displays). Real Stata: `obs` is a pwcorr-only option —
    // correlate (listwise) already prints its "(obs=N)" banner
    // unconditionally, so an explicit `obs` there is not valid syntax at
    // all, regardless of `pairwise`.
    if (obs && cmd != "pwcorr") {
      blocks.push_back(ojson(eval::engineError("option obs not allowed", "unsupported_option")));
      return blocks;
    }
    // `pwcorr, obs`: cobra's own `corr --pairwise` output already carries a
    // full nobs[i][j] matrix REGARDLESS of --sig (verified against a live
    // cobra run) and cobraToEstimationM4's passthrough already forwards
    // "nobs" — so requesting --pairwise (pwcorr always does, above) is
    // already enough for the block to carry the correct per-pair N data.
    // NOTE (frontend, out of scope — piet/frontend/** is forbidden for this
    // batch): console.js's formatCorrelate() has no code path that reads
    // `block.nobs` to render a per-cell N row for pwcorr (only the
    // `showSig` p-value row exists, gated on `block.sig`) — until a
    // companion frontend change adds one, `pwcorr ..., obs` renders the
    // plain correlation matrix with no extra row. That is still a real
    // improvement over before: `sig` is no longer force-set true by `obs`,
    // so the display no longer shows spurious, un-requested p-values
    // either.
    ojson corrBlock = ojson(
        handlers::runCorr(parsed.varlist, pairwise, sig, session, parsed.ifExpr, parsed.inRange));
    // BS-028 composing 1-liner (BETA campaign C2): record `obs` on the
    // block itself so a future frontend change can decide whether to
    // render the per-pair N matrix (`nobs`, already flowing per the NOTE
    // above) without re-deriving "was `obs` requested" from opts a second
    // time. Only reachable here when cmd=="pwcorr" (the `correlate, obs`
    // case already returned above).
    if (obs && corrBlock.is_object()) {
      corrBlock["obs"] = true;
    }
    blocks.push_back(corrBlock);
    return blocks;
  }

  if (cmd == "ttest") {
    // BUG-373: Stata publishes the confidence bounds, the sample SD and BOTH
    // one-sided p-values alongside the two-sided one. Measured against the live
    // capture: lb_1 .46197733, ub_1 .56453464, sd_1 .47784417, p_l .69428307,
    // p_u .30571693 — and p_l + p_u == 1 by construction, which is what makes
    // publishing only the two-sided p a real loss for a directional test.
    ojson ttestBlk = ojson(handlers::runTtest(parsed, session));
    if (ttestBlk.value("type", std::string()) != "error") {
      auto put = [&](const char* rn, const char* key) {
        if (ttestBlk.contains(key) && ttestBlk[key].is_number())
          session.eval.rValues.set(rn, ttestBlk[key].get<double>());
      };
      put("N_1", "n_obs");
      put("p", "p");
      const auto& ct = ttestBlk.contains("coeftable") ? ttestBlk["coeftable"] : ojson::array();
      if (ct.is_array() && !ct.empty()) {
        const ojson& r0 = ct[0];
        if (r0.contains("ci_lo") && r0["ci_lo"].is_number())
          session.eval.rValues.set("lb_1", r0["ci_lo"].get<double>());
        if (r0.contains("ci_hi") && r0["ci_hi"].is_number())
          session.eval.rValues.set("ub_1", r0["ci_hi"].get<double>());
        if (r0.contains("b") && r0["b"].is_number())
          session.eval.rValues.set("mu_1", r0["b"].get<double>());
        if (r0.contains("t") && r0["t"].is_number()) {
          const double t = r0["t"].get<double>();
          session.eval.rValues.set("t", t);
        }
        // r(sd_1) is the SAMPLE SD, which the block carries only as the standard
        // ERROR: sd = se * sqrt(N). Checked against the capture — se .0260685
        // over N=336 gives .4778442, and Stata reports .4778441714961855.
        if (r0.contains("se") && r0["se"].is_number() && ttestBlk.contains("n_obs") &&
            ttestBlk["n_obs"].is_number()) {
          const double nObs = ttestBlk["n_obs"].get<double>();
          if (nObs > 0)
            session.eval.rValues.set("sd_1", r0["se"].get<double>() * std::sqrt(nObs));
        }
      }
      // One-sided p's from the two-sided one and the sign of t: Stata's p_l is
      // P(T < t) and p_u is P(T > t), so they sum to 1.
      if (ttestBlk.contains("p") && ttestBlk["p"].is_number() && ct.is_array() && !ct.empty() &&
          ct[0].contains("t") && ct[0]["t"].is_number()) {
        const double p2 = ttestBlk["p"].get<double>();
        const double t = ct[0]["t"].get<double>();
        const double pu = (t >= 0) ? p2 / 2.0 : 1.0 - p2 / 2.0;
        session.eval.rValues.set("p_u", pu);
        session.eval.rValues.set("p_l", 1.0 - pu);
      }
    }
    blocks.push_back(std::move(ttestBlk));
    return blocks;
  }

  // P3 weights wave: `mean`/`proportion` — new registry rows (PweightOnly),
  // routed to the new handlers::runMean/runProportion (forward-declared at
  // the top of this file — see that comment for why they're not in
  // piet/handlers.h). Both eclass in real Stata; e(b)/_b[...] wiring happens
  // inside the handler itself (session.setLastModel), same mechanism
  // regress above uses — no dispatcher-side r()/e() plumbing needed here.
  if (cmd == "mean") {
    // BUG-373: r(title) is the table caption Stata publishes for these two.
    ojson meanBlk = ojson(handlers::runMean(parsed, session));
    if (meanBlk.value("type", std::string()) != "error")
      session.eval.rStrings.set("title", "Mean estimation");
    blocks.push_back(std::move(meanBlk));
    return blocks;
  }
  if (cmd == "proportion") {
    ojson propBlk = ojson(handlers::runProportion(parsed, session));
    if (propBlk.value("type", std::string()) != "error")
      session.eval.rStrings.set("title", "Proportion estimation");
    blocks.push_back(std::move(propBlk));
    return blocks;
  }

  if (cmd == "xtreg") {
    blocks.push_back(ojson(handlers::runXtreg(parsed, session)));
    return blocks;
  }

  if (cmd == "logit" || cmd == "logistic" || cmd == "probit") {
    std::string cobraCmd = (cmd == "probit") ? "probit" : "logit";
    ojson block = ojson(handlers::runLogitProbit(cobraCmd, parsed, session));
    // BS-022 (BETA Phase C batch C1): `logistic` previously returned the
    // IDENTICAL block as `logit` (estimator "logit", b = log-odds) — real
    // Stata's `logistic` runs the SAME engine call as `logit` but displays
    // odds ratios: OR = exp(b), SE_OR = OR * se(b) (delta method), CI =
    // exp(ci of b); z/p/t are invariant under this monotonic reparam and
    // stay untouched. Only the DISPLAY coeftable is transformed — `coefs`
    // (feeds e(b)/postestimation, e.g. a later `margins`/`lincom`) is left
    // in the log-odds scale untouched, matching Stata's own internal
    // storage (its display-only OR transform never touches e(b)).
    // NOTE: ci_lo/ci_hi (and p) are currently null in this engine's logit
    // coeftable rows (a separate, pre-existing gap outside this batch's
    // scope) — the exp() transform below is a no-op on a null field, so
    // this stays forward-compatible once that gap is closed.
    // NOTE (frontend, out of scope — piet/frontend/** is forbidden for this
    // batch): console.js's formatOneTable() derives the coefficient
    // column's header purely from `block.estimator === 'margins' ? 'dy/dx'
    // : 'Coefficient'` (no other estimator branch) — with no companion
    // frontend change the column header will still literally read
    // "Coefficient" even though the values underneath are now correct odds
    // ratios. estimatorTitle() already has a `logistic` -> "Logistic
    // regression" row (identical to logit's, matching real Stata, which
    // titles both commands the same), so the header banner itself needs no
    // change.
    // BUG-333: `logit ..., or` reports odds ratios. Stata's `logistic` IS
    // `logit, or`, so the transform this file already performs for `logistic`
    // below is exactly the one `or` needs — generalised here instead of copied.
    // e(b) stays in log-odds for both (Stata does the same: `or` is a display
    // metric, `_b[]` is unchanged), which is why only `coeftable` is touched.
    bool wantOr = false;
    for (const auto& [ok, ov] : parsed.opts)
      if (ok == "or" && ov.isBool && ov.boolValue) wantOr = true;
    if ((cmd == "logistic" || wantOr) && block.value("type", "") == "estimation") {
      if (cmd == "logistic") block["estimator"] = "logistic";
      // A metric marker the renderer can key on without knowing the command —
      // stcox/streg already set this key (applyHazardRatioTableM4).
      block["coef_metric"] = "odds_ratio";
      if (block.contains("coeftable") && block["coeftable"].is_array()) {
        for (auto& row : block["coeftable"]) {
          if (!row.contains("b") || row["b"].is_null()) continue;
          double b = row["b"].get<double>();
          double orv = std::exp(b);
          row["b"] = orv;
          if (row.contains("se") && row["se"].is_number()) {
            row["se"] = orv * row["se"].get<double>();
          }
          if (row.contains("ci_lo") && row["ci_lo"].is_number()) {
            row["ci_lo"] = std::exp(row["ci_lo"].get<double>());
          }
          if (row.contains("ci_hi") && row["ci_hi"].is_number()) {
            row["ci_hi"] = std::exp(row["ci_hi"].get<double>());
          }
          // t/p intentionally unchanged (invariant under a monotonic exp() reparam).
        }
      }
    }
    blocks.push_back(block);
    return blocks;
  }

  if (cmd == "poisson" || cmd == "ppmlhdfe" || cmd == "fepois") {
    // BS-073 (BETA D3a): bare `poisson` -> cobra fepois WITHOUT --fe (D0
    // finding: reproduces Stata's poisson exactly); `ppmlhdfe`/cobra's own
    // `fepois` command name (BOTH accepted as typeable command words here,
    // mirroring the feols/reghdfe precedent above of accepting the engine's
    // own internal estimator name as an additional PIET spelling) allow
    // absorb() -> --fe. Real Stata's plain `poisson` has NO absorb() option
    // — runPoisson honest-rejects it there.
    blocks.push_back(ojson(handlers::runPoisson(cmd, parsed, session)));
    return blocks;
  }

  if (cmd == "nbreg") {
    // BS-074 (BETA D3b): negative-binomial (NB2) regression -> cobra nbreg.
    // NOTE: the engine has a live-verified convergence gap on data whose
    // fitted intercept sits away from zero (including the D3e oracle's own
    // synthetic_panel/cnt data) — see runNbreg's header comment (estimation_m4.cpp)
    // for the full disclosure; this is an engine-side limitation, not a
    // wiring gap.
    blocks.push_back(ojson(handlers::runNbreg(parsed, session)));
    return blocks;
  }

  if (cmd == "xtpoisson") {
    // BS-075 (BETA D3c): conditional fixed-effects Poisson MLE, `,fe` ONLY
    // (the engine has no RE/PA xtpoisson — Stata's own default IS
    // random-effects, so the bare/`,re`/`,pa` forms all honest-reject).
    blocks.push_back(ojson(handlers::runXtPoisson(parsed, session)));
    return blocks;
  }

  if (cmd == "xtlogit") {
    // BS-075 (BETA D3c): conditional fixed-effects logit MLE, `,fe` ONLY
    // (same RE/PA-absence rationale as xtpoisson above).
    blocks.push_back(ojson(handlers::runXtLogitFe(parsed, session)));
    return blocks;
  }

  if (cmd == "csdid") {
    blocks.push_back(ojson(handlers::runCsdid(parsed, session)));
    return blocks;
  }

  // ---- Arc F (P3 wave): the seven QE/RD commands. Unlike every neighbouring
  // estimator arm, these handlers return an ARRAY (one `display` block per
  // printed line + one structured block last — the handlers_resample.cpp
  // shape), because the tables are rendered as TEXT here rather than being
  // handed to the frontend as a coeftable. So the arms splice; they must not
  // push_back, which would nest an array inside the block stream. ----
  if (cmd == "rdrobust" || cmd == "rddensity" || cmd == "rdplot" || cmd == "binscatter" ||
      cmd == "eventstudyinteract" || cmd == "did_imputation" || cmd == "sdid") {
    ojson res;
    if (cmd == "rdrobust") res = handlers::runRdrobust(parsed, session);
    else if (cmd == "rddensity") res = handlers::runRddensity(parsed, session);
    else if (cmd == "rdplot") res = handlers::runRdplot(parsed, session);
    else if (cmd == "binscatter") res = handlers::runBinscatter(parsed, session);
    else if (cmd == "eventstudyinteract") res = handlers::runEventstudyinteract(parsed, session);
    else if (cmd == "did_imputation") res = handlers::runDidImputation(parsed, session);
    else res = handlers::runSdid(parsed, session);
    if (cmd == "binscatter") {
      // W4: binscatter's own net (handlers_qe.cpp) has already run and now
      // ACCEPTS the graph drawing keys (kVocabBinscatterGraph unions them in).
      // Accepting them without saying so would be the silent drop this wave
      // exists to close, so the block carries the note.
      if (res.is_array()) {
        for (auto& b : res) graphAcceptedNotApplied(b, "binscatter", parsed.opts, kBinscatterApplied);
      } else {
        graphAcceptedNotApplied(res, "binscatter", parsed.opts, kBinscatterApplied);
      }
    }
    if (res.is_array()) {
      for (const auto& b : res) blocks.push_back(b);
    } else {
      blocks.push_back(res);
    }
    return blocks;
  }

  if (cmd == "drdid") {
    // BS-076 (BETA D3d): the classic 2-period doubly-robust DiD -> the SAME
    // engine csdid machinery as `csdid` above, ONE (g,t) cell — now its OWN
    // handler (previously shared csdid's arm above and unconditionally
    // rejected on missing gvar(), since real drdid syntax has no gvar()
    // option at all). See runDrdid's header comment (estimation_m4.cpp) for
    // the CSDID_CLEANROOM_WALL compliance note and the live-verified oracle
    // match.
    blocks.push_back(ojson(handlers::runDrdid(parsed, session)));
    return blocks;
  }

  // ------------------------------------------------------------------
  // BETA campaign Phase D batch D4 (time-series wirings): BS-077 newey,
  // BS-078 prais, BS-079 dfuller/pperron, BS-080 wntestq, BS-081 tsfill.
  // Oracle: milf-coord/plans/BETA-20260712-campaign/probes/
  // d45-oracle-stata.log, sections D4a-D4f.

  if (cmd == "newey") {
    blocks.push_back(ojson(handlers::runNewey(parsed, session)));
    return blocks;
  }

  if (cmd == "prais") {
    blocks.push_back(ojson(handlers::runPrais(parsed, session)));
    return blocks;
  }

  // BUG-373: these three computed their statistics and then published NOTHING.
  // A capture gate cannot see that — it compares what a command PRINTS, and the
  // printing was always right. The 2026-08-31 declared-returns sweep is what
  // measured it, against real StataBE: our dfuller numbers match Stata to every
  // digit it reports, and `display r(p)` still errored.
  if (cmd == "dfuller" || cmd == "pperron") {
    ojson blk = ojson(handlers::runDfullerPperron(cmd, parsed, session));
    if (blk.value("type", std::string()) != "error") {
      auto put = [&](const char* rname, const char* key) {
        if (blk.contains(key) && blk[key].is_number())
          session.eval.rValues.set(rname, blk[key].get<double>());
      };
      put("N", "n_obs");
      put("lags", "lags");
      put("Zt", "Zt");
      put("p", "p");
      put("Zrho", "Zrho");   // pperron only; absent on dfuller, so not set there
      // pperron names its lag count `newey_west_lags` in the block and Stata
      // calls it r(lags); and Stata publishes the p-value under BOTH r(p) and
      // r(pval) for this command. Measured, not assumed — the first wiring pass
      // used the block's own key names and the re-sweep caught both misses.
      put("lags", "newey_west_lags");
      put("pval", "p");
      // Stata publishes the critical values under BOTH spellings.
      for (const auto& pr : {std::make_pair("cv_1", "cv_1"), std::make_pair("cv_5", "cv_5"),
                             std::make_pair("cv_10", "cv_10"), std::make_pair("cv1", "cv_1"),
                             std::make_pair("cv5", "cv_5"), std::make_pair("cv10", "cv_10")})
        put(pr.first, pr.second);
      session.eval.locals.set("_rc", "0");
    }
    blocks.push_back(std::move(blk));
    return blocks;
  }

  if (cmd == "wntestq") {
    ojson blk = ojson(handlers::runWntestq(parsed, session));
    if (blk.value("type", std::string()) != "error") {
      if (blk.contains("df") && blk["df"].is_number())
        session.eval.rValues.set("df", blk["df"].get<double>());
      if (blk.contains("p") && blk["p"].is_number())
        session.eval.rValues.set("p", blk["p"].get<double>());
      // Stata calls the Q statistic r(stat).
      if (blk.contains("Q") && blk["Q"].is_number())
        session.eval.rValues.set("stat", blk["Q"].get<double>());
      session.eval.locals.set("_rc", "0");
    }
    blocks.push_back(std::move(blk));
    return blocks;
  }

  if (cmd == "tsfill") {
    blocks.push_back(ojson(handlers::runTsfill(parsed.opts, session)));
    return blocks;
  }

  // ------------------------------------------------------------------
  // BETA campaign Phase D batch D5 (post-estimation/diag/misc wirings):
  // BS-085 sdtest, BS-083 hausman, BS-084 chow, BS-087(part) mkspline.
  // (BS-021 durbinalt fix + BS-082 estat endogenous/overid/firststage live
  // inside runEstat, estimation_m4.cpp — no separate dispatch arm needed,
  // both ride the existing `estat` arm below. BS-086 margins ,at() fix
  // lives inside runMargins, same file — no dispatch change needed either.)

  if (cmd == "sdtest") {
    // Real Stata's `sdtest varname [if], by(groupvar)` / `sdtest varname
    // [if] = #` — the `= #` comparison value rides the varlist as a bare
    // `=` token (same reconstruct-then-split idiom `egen`'s arm above uses
    // for `o = fn(expr)`), NOT a parsed.opts entry.
    std::string raw = joinVarlist(parsed.varlist);
    size_t eqPos = raw.find('=');
    std::string varName = trimWs(eqPos == std::string::npos ? raw : raw.substr(0, eqPos));
    std::optional<std::string> againstVal;
    if (eqPos != std::string::npos) againstVal = trimWs(raw.substr(eqPos + 1));
    blocks.push_back(
        ojson(handlers::runSdtest(varName, againstVal, parsed.opts, parsed.ifExpr, session)));
    return blocks;
  }

  if (cmd == "hausman") {
    if (parsed.varlist.size() != 2) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `hausman` requires exactly two stored estimation names, e.g. `hausman "
          "A B`",
          "syntax_error")));
      return blocks;
    }
    blocks.push_back(
        ojson(handlers::runHausman(parsed.varlist[0], parsed.varlist[1], parsed.opts, session)));
    return blocks;
  }

  if (cmd == "chow") {
    blocks.push_back(ojson(handlers::runChow(parsed, session)));
    return blocks;
  }

  if (cmd == "mkspline") {
    // Real Stata's `mkspline stub = varname [if], cubic ...` — same
    // reconstruct-then-split-on-`=` idiom as `sdtest`'s arm above / egen's
    // arm earlier in this function.
    std::string raw = joinVarlist(parsed.varlist);
    size_t eqPos = raw.find('=');
    if (eqPos == std::string::npos) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `mkspline` requires `stub = varname`", "syntax_error")));
      return blocks;
    }
    std::string stub = trimWs(raw.substr(0, eqPos));
    std::string srcVar = trimWs(raw.substr(eqPos + 1));
    blocks.push_back(
        ojson(handlers::runMkspline(stub, srcVar, parsed.opts, parsed.ifExpr, session)));
    return blocks;
  }

  if (cmd == "test") {
    // BS-097: this arm forwards ONLY the joined varlist string to the
    // handler — parsed.opts is never read (or even passed in) at all, so
    // any post-comma option (e.g. real Stata's `accum`, `mtest`) was already
    // silently discarded before reaching runTestCmd. Known-set is empty.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (auto gone = refuseIfFitDataGone("test", session)) {
      blocks.push_back(std::move(*gone));
      return blocks;
    }
    ojson testBlock = ojson(handlers::runTestCmd(joinVarlist(parsed.varlist), session));
    // BUG-333: Stata's `test` takes accumulate/notest/mtest/coef; this arm forwards
    // only the joined varlist to runTestCmd, and cross-invocation accumulation is
    // a session-state design decision, not a wiring gap. Promoted from a silent
    // drop to an honest unimplemented-reject rather than guessed at.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {},
                                              {"accumulate", "notest", "mtest", "coef"})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // W:P5 — `test` is r-class (captureRFromBlock Branch 3), same wiring the
    // lincom arm below has had since FIX-RESTIMATE. Without this,
    // `test 1.treated` then `display r(p)` errored with "r(p) is not set".
    eval::captureRFromBlock(testBlock, session.eval);
    if (testBlock.value("type", std::string()) != "error") {
      session.eval.locals.set("_rc", "0");
      // BUG-373: Stata publishes r(drop) — whether any constraint was DROPPED as
      // redundant. This engine never drops one silently (a redundant restriction
      // surfaces as an error), so the honest published value is 0 rather than an
      // absence a caller has to special-case.
      session.eval.rValues.set("drop", 0.0);
    }
    blocks.push_back(std::move(testBlock));
    return blocks;
  }

  // BUG-371 — decode / inspect / ds / unab.
  if (cmd == "decode") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {"generate"}, {"maxlength"})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (parsed.varlist.size() != 1) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `decode` requires `decode varname, generate(newvar)`", "syntax_error")));
      return blocks;
    }
    std::optional<std::string> genVar;
    for (const auto& [k, v] : parsed.opts)
      if (k == "generate" && !v.isBool) genVar = v.strValue;
    blocks.push_back(ojson(handlers::runDecode(parsed.varlist[0], genVar, session)));
    return blocks;
  }

  if (cmd == "xtsum") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runXtsum(parsed.varlist, session)));
    return blocks;
  }

  if (cmd == "xttab") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runXttab(parsed.varlist, session)));
    return blocks;
  }

  if (cmd == "inspect") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    ojson blk = ojson(handlers::runInspect(parsed.varlist, session));
    // BUG-373: inspect is r-class in Stata and mine published nothing — a command
    // I wrote the day before, computing every one of these and dropping them.
    // Exactly the class the declared-returns sweep exists to find.
    if (blk.value("type", std::string()) != "error" && blk.contains("inspect")) {
      for (const char* k : {"N", "N_neg", "N_0", "N_pos", "N_negint", "N_posint"})
        if (blk["inspect"].contains(k))
          session.eval.rValues.set(k, blk["inspect"][k].get<double>());
      session.eval.locals.set("_rc", "0");
    }
    blocks.push_back(std::move(blk));
    return blocks;
  }

  if (cmd == "ds") {
    // Stata's ds has a large filter vocabulary (has()/alpha/varwidth/detail);
    // most remain unimplemented and refused BY NAME rather than ignored — a
    // dropped filter would silently list the WRONG variables, which is worse
    // than an error. B5-D7: `not` is now accepted (kVocabDs.known) and
    // implemented as the complement of the varlist against every variable in
    // the dataset — runKeep's own complement idiom (handlers_m3.cpp). Measured
    // B4REC|ds|not|0| (tests/eval/golden/aer_b4/milfpins_oracle_r.log:163-164).
    // Bound to the REGISTERED vocabulary below (kVocabDs), not an inline
    // literal set — this arm used to carry its own duplicate copy of
    // kVocabDs.known/unimplemented, so the census and the arm could drift
    // apart; fixed here.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabDs.known, kVocabDs.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    bool dsNot = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "not" && v.isBool && v.boolValue) dsNot = true;
    }
    auto dsAllNames = [&](ojson* errOut) -> std::optional<std::vector<std::string>> {
      if (const std::vector<VarInfo>* card = session.residentReadCard()) {
        std::vector<std::string> out;
        for (const auto& vi : *card) out.push_back(vi.name);
        return out;
      }
      Datastore* dsp = session.datastore();
      if (dsp == nullptr) {
        *errOut = ojson(eval::noDataLoaded());
        return std::nullopt;
      }
      return dsp->names();
    };
    std::vector<std::string> names;
    if (parsed.varlist.empty()) {
      ojson dsErr;
      auto all = dsAllNames(&dsErr);
      if (!all.has_value()) {
        blocks.push_back(dsErr);
        return blocks;
      }
      // `ds, not` with no varlist has nothing to complement against -- same as
      // plain `ds` (every variable).
      names = *all;
    } else {
      auto vr = piet::loops::expandVarlist(joinVarlist(parsed.varlist), session);
      if (vr.isError) {
        blocks.push_back(vr.error);
        return blocks;
      }
      if (!dsNot) {
        names = vr.tokens;
      } else {
        ojson dsErr;
        auto all = dsAllNames(&dsErr);
        if (!all.has_value()) {
          blocks.push_back(dsErr);
          return blocks;
        }
        // Complement, preserving dataset column order (matches runKeep).
        for (const auto& n : *all) {
          if (std::find(vr.tokens.begin(), vr.tokens.end(), n) == vr.tokens.end()) {
            names.push_back(n);
          }
        }
      }
    }
    std::string joined;
    for (size_t i = 0; i < names.size(); ++i) {
      if (i) joined += " ";
      joined += names[i];
    }
    // r(varlist) is a STRING r-class result. captureRFromBlock has no string
    // branch (it only ever rebuilds the numeric store), so this is set directly —
    // the same one-liner `levelsof` uses for r(levels).
    session.eval.rStrings.set("varlist", joined);
    session.eval.locals.set("_rc", "0");
    ojson dsOut;
    dsOut["type"] = "display";
    dsOut["text"] = joined;
    blocks.push_back(std::move(dsOut));
    return blocks;
  }

  if (cmd == "unab") {
    // `unab lname : varlist` — a colon form, split on the first UNQUOTED colon the
    // same way the `frame NAME: CMD` arm does.
    std::string rest = trimWs(parsed.raw);
    {
      size_t sp = rest.find_first_of(" \t");
      rest = (sp == std::string::npos) ? std::string() : trimWs(rest.substr(sp + 1));
    }
    static const std::regex reUnabColon(R"(:)");
    std::smatch cm;
    if (!eval::searchUnquoted(rest, reUnabColon, cm)) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `unab` requires `unab lname : varlist`", "syntax_error")));
      return blocks;
    }
    std::string lname = trimWs(rest.substr(0, static_cast<size_t>(cm.position())));
    std::string spec = trimWs(rest.substr(static_cast<size_t>(cm.position()) + 1));
    if (lname.empty() || spec.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `unab` requires `unab lname : varlist`", "syntax_error")));
      return blocks;
    }
    auto vr = piet::loops::expandVarlist(spec, session);
    if (vr.isError) {
      blocks.push_back(vr.error);
      return blocks;
    }
    // Stata's unab errors when a name does not exist. expandVarlist only proves
    // that for a WILDCARD (a bare unknown name passes straight through), so the
    // literal names are checked here too rather than defining a local that names
    // a column the dataset does not have.
    //
    // The card is the cheap source but it is not always live, and "no card" must
    // NOT mean "skip the check" — that is how an unchecked name slips through
    // (caught by this lock's own (c) case, which failed until the datastore
    // fallback below was added).
    {
      std::vector<std::string> known;
      if (const std::vector<VarInfo>* card = session.residentReadCard()) {
        for (const auto& vi : *card) known.push_back(vi.name);
      } else if (Datastore* udsp = session.datastore()) {
        known = udsp->names();
      } else {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return blocks;
      }
      for (const auto& t : vr.tokens) {
        if (std::find(known.begin(), known.end(), t) == known.end()) {
          blocks.push_back(
              ojson(eval::engineError("variable " + t + " not found", "unknown_variable")));
          return blocks;
        }
      }
    }
    std::string joined;
    for (size_t i = 0; i < vr.tokens.size(); ++i) {
      if (i) joined += " ";
      joined += vr.tokens[i];
    }
    session.eval.locals.set(lname, joined);
    session.eval.locals.set("_rc", "0");
    return blocks;  // Stata's unab prints nothing; it defines the local.
  }

  // BUG-370 — see registry.cpp for why these three exist and what they refuse.
  // All follow the mkspline arm's `stub = var` split (joinVarlist -> find('=')).
  if (cmd == "xtile") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {"nquantiles", "cutpoints"},
                                              {"altdef"})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    std::string raw = joinVarlist(parsed.varlist);
    size_t eqPos = raw.find('=');
    if (eqPos == std::string::npos) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `xtile` requires `newvar = varname`", "syntax_error")));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runXtile(trimWs(raw.substr(0, eqPos)),
                                              trimWs(raw.substr(eqPos + 1)), parsed.opts,
                                              parsed.ifExpr, session)));
    return blocks;
  }

  if (cmd == "ipolate") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {"generate", "epolate", "by"}, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(
        ojson(handlers::runIpolate(parsed.varlist, parsed.opts, parsed.ifExpr, session)));
    return blocks;
  }

  if (cmd == "tssmooth") {
    // Two-word command, registered under the first word: the sub-verb arrives as
    // varlist[0] (the duplicates/estat convention).
    std::string sub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
    if (sub != "ma") {
      blocks.push_back(ojson(eval::engineError(
          sub.empty()
              ? std::string("syntax_error: `tssmooth` requires a sub-command; this build "
                            "implements `tssmooth ma newvar = varname, window(#l #c #f)`")
              : "`tssmooth " + sub +
                    "` is not implemented in this build (only `ma`; Stata's exponential, "
                    "dexponential, hwinters, shwinters and nl smoothers are not available).",
          sub.empty() ? "syntax_error" : "unimplemented")));
      return blocks;
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {"window"}, {"replace"})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    std::string raw =
        joinVarlist(std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end()));
    size_t eqPos = raw.find('=');
    if (eqPos == std::string::npos) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `tssmooth ma` requires `newvar = varname`", "syntax_error")));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runTssmoothMa(trimWs(raw.substr(0, eqPos)),
                                                   trimWs(raw.substr(eqPos + 1)), parsed.opts,
                                                   session)));
    return blocks;
  }

  if (cmd == "testparm") {
    // BUG-367: `testparm x1 x2` / `testparm i.year` / `testparm x*`. Stata's own
    // options (equal, equation(), nosvyadjust, ...) are NOT implemented, so they
    // are refused by name rather than silently dropped — the same discipline the
    // `test` arm above adopted under BUG-333.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {},
                                              {"equal", "equation", "nosvyadjust", "df"})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (auto gone = refuseIfFitDataGone("testparm", session)) {
      blocks.push_back(std::move(*gone));
      return blocks;
    }
    ojson tpBlock = ojson(handlers::runTestparm(parsed.varlist, session));
    // r-class, exactly as `test` is (W:P5 captureRFromBlock Branch 3) — a
    // `testparm` followed by `display r(p)` has to see the same r() the test
    // arm fills, or the two spellings of one command would disagree.
    eval::captureRFromBlock(tpBlock, session.eval);
    if (tpBlock.value("type", std::string()) != "error") {
      session.eval.locals.set("_rc", "0");
    }
    blocks.push_back(std::move(tpBlock));
    return blocks;
  }

  if (cmd == "lincom") {
    // BUG-333: level() and the eform family are now IMPLEMENTED (engine --level
    // landed this wave; eform is a display metric applied to the block). The
    // BS-097 note below described the pre-fix state, when they were silently
    // discarded and then honestly rejected.
    if (auto err = eval::rejectUnknownOptions(
            parsed.opts, {"level", "eform", "or", "hr", "irr", "rrr", "shr"}, {"post"})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // FIX-RESTIMATE: lincom is r-class — populate r(estimate)/r(se)/r(t)/r(p)/
    // r(lb)/r(ub)/r(df) from its coeftable (captureRFromBlock Branch 2). This
    // was wired only for summarize before, so `lincom` after any estimation
    // (reghdfe/regress/…) left r() empty and every downstream `r(estimate)`
    // read errored. Error blocks (no prior estimation) don't match Branch 2.
    if (auto gone = refuseIfFitDataGone("lincom", session)) {
      blocks.push_back(std::move(*gone));
      return blocks;
    }
    ojson lincomBlock = ojson(handlers::runLincom(joinVarlist(parsed.varlist), session, parsed.opts));
    eval::captureRFromBlock(lincomBlock, session.eval);
    if (lincomBlock.value("type", std::string()) != "error") {
      session.eval.locals.set("_rc", "0");
    }
    blocks.push_back(std::move(lincomBlock));
    return blocks;
  }

  if (cmd == "nlcom") {
    // BUG-333: level() implemented (engine --level landed this wave).
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabNlcom.known, kVocabNlcom.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // FIX-RESTIMATE: nlcom is r-class too (same captureRFromBlock Branch 2).
    if (auto gone = refuseIfFitDataGone("nlcom", session)) {
      blocks.push_back(std::move(*gone));
      return blocks;
    }
    ojson nlcomBlock = ojson(handlers::runNlcom(joinVarlist(parsed.varlist), session, parsed.opts));
    eval::captureRFromBlock(nlcomBlock, session.eval);
    // BUG-373: captureRFromBlock fills the estimate/se/t/p family; r(N) is the
    // fit's own observation count and was not among them.
    if (nlcomBlock.value("type", std::string()) != "error" && nlcomBlock.contains("n_obs") &&
        nlcomBlock["n_obs"].is_number())
      session.eval.rValues.set("N", nlcomBlock["n_obs"].get<double>());
    blocks.push_back(std::move(nlcomBlock));
    return blocks;
  }

  if (cmd == "margins") {
    blocks.push_back(ojson(handlers::runMargins(parsed.opts, session)));
    return blocks;
  }

  if (cmd == "predict") {
    // BUG-517: the `if` qualifier reaches the handler (it was parsed and then
    // dropped here, so `predict p if x` filled every row). c() refs resolve the
    // way gen's `if` does; est refs were already resolved at the parse point.
    std::optional<std::string> predictIf = parsed.ifExpr;
    if (predictIf.has_value()) predictIf = substituteCRefs(*predictIf, session);
    blocks.push_back(
        ojson(handlers::runPredict(parsed.varlist, parsed.opts, session, predictIf)));
    return blocks;
  }

  if (cmd == "estat") {
    std::string sub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
    std::vector<std::string> after(parsed.varlist.empty() ? parsed.varlist.begin()
                                                            : parsed.varlist.begin() + 1,
                                    parsed.varlist.end());
    ojson estatBlk = ojson(handlers::runEstat(sub, after, parsed.opts, parsed.ifExpr, session));
    // BUG-373: `estat vif` is r-class in Stata — r(name_K)/r(vif_K), one pair per
    // regressor, 1-based in coeftable order. The values were computed and printed
    // and never published.
    if (sub == "vif" && estatBlk.value("type", std::string()) != "error" &&
        estatBlk.contains("coeftable") && estatBlk["coeftable"].is_array()) {
      int k = 0;
      for (const auto& row : estatBlk["coeftable"]) {
        ++k;
        const std::string nm = row.value("name", std::string());
        if (!nm.empty()) session.eval.rStrings.set("name_" + std::to_string(k), nm);
        if (row.contains("b") && row["b"].is_number())
          session.eval.rValues.set("vif_" + std::to_string(k), row["b"].get<double>());
      }
    }
    blocks.push_back(std::move(estatBlk));
    return blocks;
  }

  // ------------------------------------------------------------------
  // M4a item 3: stored estimates + tables (ROADMAP M4a)

  if (cmd == "eststo" || cmd == "est" || cmd == "estimates") {
    // `estimates table` (P4 wave) is the ONE sub-form that carries options, so
    // it is routed BEFORE the empty-option net below — that net is what turned
    // `estimates table m1 m2, b se` into "option `b` not allowed".
    // W2 esttabnames: estimates.ado's own floors (`bsubstr(SUB, 1, max(N, len))`)
    // -- `table` 3, `store`/`restore` 3, `dir` 3 (full word). `est sto m1` and
    // `estimates sto m1` were r(199) "subcommand not supported" on main while
    // StataBE answers rc 0 (w2_cmdtriage.log section C est_sto/estimates_sto).
    auto estSubIs = [](const std::string& typed, const std::string& full, size_t floor) {
      return typed.size() >= floor && typed.size() <= full.size() &&
             full.compare(0, typed.size(), typed) == 0;
    };
    if ((cmd == "est" || cmd == "estimates") && !parsed.varlist.empty() &&
        estSubIs(lower(parsed.varlist[0]), "table", 3)) {
      std::vector<std::string> tabNames(parsed.varlist.begin() + 1, parsed.varlist.end());
      blocks.push_back(ojson(handlers::runEstimatesTable(tabNames, parsed.opts, session)));
      return blocks;
    }
    // BS-097 as amended by B4-W3b. The net used to be ONE empty set applied
    // here, above the sub-verb split, on the reasoning that no sub-form read
    // parsed.opts. Two now do — `store`'s `title()` and `save`'s `replace` —
    // and a flat union of their vocabularies would accept `estimates store m1,
    // replace`, which Stata rejects. So the net moved INSIDE each sub-verb and
    // every arm below names its own known set; the sets stay empty where the
    // handler still takes only name/sub + session.
    // (`title("...")`'s quote stripping is dequoteEstTitle, above dispatchImpl --
    // shared with the eststo prefix arm's head.)
    auto rejectFor = [&](const std::unordered_set<std::string>& known) -> bool {
      if (auto err = eval::rejectUnknownOptions(parsed.opts, known)) {
        blocks.push_back(ojson(*err));
        return true;
      }
      return false;
    };
    if (cmd == "est" || cmd == "estimates") {
      std::string sub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
      if (estSubIs(sub, "store", 3)) sub = "store";
      else if (estSubIs(sub, "restore", 3)) sub = "restore";
      // Every operand after the subcommand, space-joined: `estimates drop m1 m2`
      // used to pass `m1` alone and leave `m2` stored at rc 0.
      std::vector<std::string> estOperands(parsed.varlist.size() > 1 ? parsed.varlist.begin() + 1
                                                                      : parsed.varlist.end(),
                                           parsed.varlist.end());
      if (sub == "restore") {
        // W2 esttabnames (BUG-553): `estimates restore NAME` makes a stored
        // model active again (w2_cmdtriage.log section C: rc 0, "(results m1
        // are active now)", then e(df_m)/_b[a] are m1's). estimates.ado Restore:
        // est_expand ..., min(1) max(1) -- a namelist token, so `m*` matching
        // one name works and matching two is r(198) "too many names
        // specified"; a miss is r(111). A BARE `est restore` reaches
        // `est_unhold` with an empty name, which StataBE answers with the
        // literal "estimates __000000 not found" r(111) (est_restore_bare).
        // `.` (the active results, unstored) is a no-op.
        if (rejectFor({})) return blocks;
        auto rcBlock = [](const std::string& msg, int rc) {
          return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                        std::nullopt, "r(" + std::to_string(rc) + ")"));
        };
        if (estOperands.empty()) {
          blocks.push_back(rcBlock("estimates __000000 not found", 111));
          return blocks;
        }
        SessionState::EstNamelist ex = session.expandEstimatesNamelist(estOperands);
        if (ex.rc == 0 && ex.names.size() > 1) {
          ex.rc = 198;
          ex.message = "too many names specified";
        }
        if (ex.rc != 0) {
          blocks.push_back(rcBlock(ex.message, ex.rc));
          return blocks;
        }
        const std::string rname = ex.names.front();
        // Always re-activated from the store, even when NAME is the name the
        // active results carry: identity is decided inside estimatesRestore
        // (a model-less store is a no-op only when its block IS the live one).
        if (rname != "." && !session.estimatesRestore(rname)) {
          blocks.push_back(ojson(eval::errorBlock(
              "unsupported_option",
              "estimates restore: " + rname + " was stored from results this version of PIET "
              "cannot make active again (no fitted model behind them, e.g. `estpost`); "
              "nothing was changed")));
          return blocks;
        }
        ojson note;
        note["type"] = "display";
        note["text"] = "(results " + rname + " are active now)";
        blocks.push_back(std::move(note));
        return blocks;
      }
      if (sub == "store") {
        if (rejectFor({"title"})) return blocks;
        std::optional<std::string> name;
        if (parsed.varlist.size() > 1) name = parsed.varlist[1];
        std::optional<std::string> title;
        for (const auto& [k, v] : parsed.opts) {
          if (k == "title" && !v.isBool) title = dequoteEstTitle(v.strValue);
        }
        // viaEststo=false: `estimates store` is official Stata and never touches
        // estout's auto-name counter ($eststo_counter is written by eststo.ado
        // alone), so it must not move the `est#` the next bare `eststo:` mints.
        blocks.push_back(ojson(handlers::runEststo(name, title, session, "est",
                                                   /*viaEststo=*/false)));
        return blocks;
      }
      if (sub == "save" || sub == "use") {
        if (rejectFor(sub == "save" ? std::unordered_set<std::string>{"replace"}
                                    : std::unordered_set<std::string>{}))
          return blocks;
        if (parsed.varlist.size() < 2) {
          blocks.push_back(ojson(eval::errorBlock(
              "unsupported_option",
              "estimates " + sub + ": a file name is required (`estimates " + sub +
                  " \"myest\"`).")));
          return blocks;
        }
        bool replace = false;
        for (const auto& [k, v] : parsed.opts) {
          if (k == "replace") replace = !v.isBool || v.boolValue;
        }
        blocks.push_back(sub == "save"
                             ? ojson(handlers::runEstimatesSave(parsed.varlist[1], replace, session))
                             : ojson(handlers::runEstimatesUse(parsed.varlist[1], session)));
        return blocks;
      }
      if (sub == "dir") {
        if (rejectFor({})) return blocks;
        blocks.push_back(ojson(handlers::runEststoDirNames(estOperands, session)));
        return blocks;
      }
      if (sub == "drop" || sub == "clear") {
        if (rejectFor({})) return blocks;
        std::optional<std::string> name;
        if (!estOperands.empty()) name = joinSpace(estOperands);
        blocks.push_back(ojson(handlers::runEststoManage(sub, name, session)));
        return blocks;
      }
      if (rejectFor({})) return blocks;
      blocks.push_back(ojson(eval::unsupportedCommand(
          cmdTyped + " " + sub, std::nullopt, std::nullopt, "piet_gap",
          eval::UnsupportedCommandKind::kUnknownSubcommand)));
      return blocks;
    }
    // W2 eststoprefix (BUG-520) -- the flat arm's half. A colon-shaped line that
    // reaches HERE was not recognised by Pre-0b (the command word or the colon
    // itself arrived through a macro, or the NAME position holds dir/drop/clear),
    // and this arm never dispatches a remainder. It used to store whatever model
    // was already in memory under `varlist[0]` -- `regress`, `mac3:` -- at rc 0,
    // and with inner options it blamed the INNER command's first option. Refuse
    // the shape by name instead, and BEFORE the option net so the message is
    // about the line's real problem. Same for a second name: `eststo a b` stored
    // `a` and dropped `b` on the floor.
    {
      const bool managing = !parsed.varlist.empty() &&
                            (lower(parsed.varlist[0]) == "dir" ||
                             lower(parsed.varlist[0]) == "drop" ||
                             lower(parsed.varlist[0]) == "clear");
      bool colonShaped = !parsed.cmd.empty() && parsed.cmd.back() == ':';
      for (const auto& tok : parsed.varlist) {
        if (tok.find(':') != std::string::npos) colonShaped = true;
      }
      if (colonShaped) {
        blocks.push_back(ojson(eval::engineError(
            "eststo: this line has the `eststo [name] [, options]: command` prefix shape, but "
            "PIET recognises that prefix only when the word `eststo` and the colon are written "
            "on the line itself (not delivered through a macro), and never with dir/drop/clear "
            "in the name position. Nothing was run and nothing was stored.",
            "syntax_error")));
        return blocks;
      }
      if (!managing && parsed.varlist.size() > 1) {
        blocks.push_back(ojson(eval::engineError(
            "eststo: `" + joinSpace(parsed.varlist) + "` is not one name -- `eststo [name] "
            "[, options]` stores the active estimates under ONE name",
            "syntax_error")));
        return blocks;
      }
    }
    // eststo [NAME] | eststo dir | eststo drop|clear [NAME]
    if (!parsed.varlist.empty()) {
      std::string sub0 = lower(parsed.varlist[0]);
      if (sub0 == "dir" || sub0 == "drop" || sub0 == "clear") {
        if (rejectFor({})) return blocks;
        if (sub0 == "dir") {
          blocks.push_back(ojson(handlers::runEststoDirNames(
              std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end()),
              session)));
          return blocks;
        }
        std::optional<std::string> name;
        if (parsed.varlist.size() > 1) {
          name = joinSpace(std::vector<std::string>(parsed.varlist.begin() + 1,
                                                    parsed.varlist.end()));
        }
        blocks.push_back(ojson(handlers::runEststoManage(sub0, name, session)));
        return blocks;
      }
    }
    EststoStoreOpts storeOpts;
    if (auto err = readEststoStoreOpts(parsed.opts, &storeOpts)) {
      blocks.push_back(*err);
      return blocks;
    }
    std::optional<std::string> name;
    if (!parsed.varlist.empty()) name = parsed.varlist[0];
    blocks.push_back(
        ojson(handlers::runEststo(name, storeOpts.title, session, storeOpts.autoPrefix)));
    return blocks;
  }

  // ==================== AER-SCOPE1 bundle J (GRAPH-X) arms ==================
  if (cmd == "grc1leg2") {
    ojson result = ojson(handlers::runGrc1leg(parsed, session));
    registerGraphOutput(result, session, parsed.opts);  // BS-011
    if (!(result.is_array() && result.empty())) blocks.push_back(result);
    return blocks;
  }
  // ==================== AER-SCOPE2 bundle K1 arms =========================
  // The corpus's remaining small names. `grc1leg` is an alias, `beep` and
  // `window` are accept-notes, and the three renamers below them have real
  // handlers in handlers_aer_k1.cpp -- every rule in which was measured against
  // StataBE with the packages installed, before it was written. `parallel` is
  // the one still open (K1c).
  if (cmd == "grc1leg") {
    // Vince Wiggins' original (stata.trk: stata.com/users/vwiggins). We answer
    // it with the grc1leg2 handler — Mead Over's enhanced rewrite — which is an
    // ACCEPTED-NAMED substitution, not an equivalence: the two packages have
    // different option defaults, so the registry carries both authors and the
    // probe keeps an arm on a default that differs.
    ojson result = ojson(handlers::runGrc1leg(parsed, session));
    registerGraphOutput(result, session, parsed.opts);  // BS-011
    if (!(result.is_array() && result.empty())) blocks.push_back(result);
    return blocks;
  }
  if (cmd == "beep") {
    // Accept-note, not silence: `beep` is a real built-in that makes a sound we
    // have no way to make, and a do-file that beeps between steps must keep
    // running. Saying so once beats a user believing something happened.
    ojson note;
    note["type"] = "display";
    note["text"] = "(beep: this build makes no sound — accepted and ignored)";
    blocks.push_back(note);
    return blocks;
  }
  if (cmd == "window") {
    // Same shape. The corpus form is `window manage close graph _all`, a GUI
    // instruction with no headless meaning.
    ojson note;
    note["type"] = "display";
    note["text"] = "(window: GUI window management does not apply in this build — accepted and ignored)";
    blocks.push_back(note);
    return blocks;
  }
  if (cmd == "parallel") {
    // ==================== AER scope 2, bundle K1c =========================
    // George Vega Yon's `parallel` (SSC), answered SEQUENTIALLY.
    //
    // WHAT THIS IS NOT. PIET already owns the word `parallel` for its own
    // `parallel { ... }` block (loops.cpp's loopIfKeywords). That form requires
    // a brace ON THE LINE, so it is recognised earlier and never reaches here;
    // every shape below is brace-free and falls through to this arm. Two
    // different commands share a spelling, and the split is structural rather
    // than a heuristic.
    //
    // EVERY RULE BELOW COMES FROM THE PACKAGE'S OWN SOURCE, because the package
    // CANNOT BE RUN on this machine: `parallel setclusters 2` answers r(601)
    // even with an explicit `statapath()`, so $PLL_STATA_PATH never gets set and
    // no child process can start. That is a measurement too, and it is why the
    // data-splitting form below is refused rather than approximated -- there is
    // no oracle to check it against.
    //
    // What parallel.ado does, read from it:
    //   · `parallel do FILE` REQUIRES $PLL_CLUSTERS and exits 198 without it,
    //     with the message reproduced verbatim below.
    //   · With `nodata` (or an empty dataset) the children get NO data and
    //     nothing is pasted back: the parent's data is untouched.
    //   · WITHOUT `nodata` it splits the rows across $PLL_CLUSTERS children and
    //     then `parallel_fusion` PASTES THEIR DATASETS BACK. With one cluster
    //     that is exactly "run the file on the whole dataset in place"; with
    //     more, each child sees a different slice and the answer depends on the
    //     split, which is the form refused here.
    //   · `initialize` is NOT a subcommand of the installed 1.15.8 (measured:
    //     `-initialize- invalid parallel subcommand`, r(198)) -- but it IS what
    //     the corpus writes, from an older release where it set the cluster
    //     count. It is accepted here as a spelling of `setclusters`, which is
    //     the ACCEPTED-NAMED exception for a third-party ado whose own versions
    //     disagree; refusing it would stop a paper's driver on a no-op.
    std::string rest = trimWs(rawArgsAfterCmd(parsed.raw));
    std::string head;
    {
      size_t sp = rest.find_first_of(" \t,:");
      head = lower(sp == std::string::npos ? rest : rest.substr(0, sp));
    }
    auto note = [&](const std::string& text) {
      ojson b;
      b["type"] = "display";
      b["text"] = text;
      blocks.push_back(b);
      return blocks;
    };
    auto refuse = [&](const std::string& text, int rc) {
      blocks.push_back(ojson(eval::errorBlock("engine_error", text, std::nullopt, std::nullopt,
                                              std::nullopt, "r(" + std::to_string(rc) + ")")));
      return blocks;
    };

    if (head == "clean") {
      // Measured rc 0 in Stata even with no cluster set up: it deletes the
      // scratch files a run would have left, and this build leaves none.
      return note("(parallel clean: this build runs sequentially and leaves no cluster "
                  "scratch files -- accepted and ignored)");
    }
    if (head == "setclusters" || head == "initialize") {
      std::string n;
      {
        std::string tail = trimWs(rest.substr(head.size()));
        size_t sp = tail.find_first_of(" \t,");
        n = sp == std::string::npos ? tail : tail.substr(0, sp);
      }
      if (n.empty()) n = "1";
      session.eval.globals.set("PLL_CLUSTERS", n);
      return note("(parallel " + head + " " + n +
                  ": recorded. This build runs the work sequentially in this process, so the "
                  "cluster count changes no answer)");
    }
    if (head == "break" || head == "version" || head == "printlog" || head == "viewlog" ||
        head == "bs" || head == "sim") {
      return refuse("parallel " + head +
                        " needs a running cluster, and this build has none: the package cannot "
                        "start a child process here (`parallel setclusters` answers r(601) even "
                        "with statapath()). Refused rather than approximated.",
                    199);
    }

    // The prefix form, `parallel [, opts] : cmd`, and `parallel do FILE`.
    const bool isDo = (head == "do");
    // The colon of the prefix form, skipping any inside a quoted span --
    // `parallel, by(x): summarize "a:b"` must split at the real one.
    size_t colon = std::string::npos;
    if (!isDo) {
      const std::vector<bool> qmask = eval::quotedSpanMask(rest);
      for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == ':' && (i >= qmask.size() || !qmask[i])) {
          colon = i;
          break;
        }
      }
    }
    if (!isDo && head != "append" && colon == std::string::npos) {
      return refuse("-" + head + "- invalid parallel subcommand (help parallel)", 198);
    }
    if (head == "append") {
      return refuse("parallel append reads a list of .dta files, runs a command over each and "
                    "pastes the results together. Its file-list expansion is implemented and "
                    "measured, but the per-file run goes through the cluster machinery this "
                    "build cannot start, so the whole verb is refused rather than half-done.",
                    199);
    }

    const std::string* clusters = session.eval.globals.find("PLL_CLUSTERS");
    if (clusters == nullptr || clusters->empty()) {
      return refuse("You haven't set the number of clusters. Please set it with: parallel "
                    "setclusters #",
                    198);
    }

    std::string body;    // the file (do form) or the command (prefix form)
    std::string optTail;
    if (isDo) {
      std::string tail = trimWs(rest.substr(2));
      long long c = eval::findUnquotedComma(tail);
      if (c >= 0) {
        optTail = trimWs(tail.substr(static_cast<size_t>(c) + 1));
        tail = trimWs(tail.substr(0, static_cast<size_t>(c)));
      }
      body = tail;
    } else {
      optTail = trimWs(rest.substr(0, colon));
      if (!optTail.empty() && optTail[0] == ',') optTail = trimWs(optTail.substr(1));
      body = trimWs(rest.substr(colon + 1));
    }

    auto optHas = [&](const std::string& key) {
      return lower(optTail).find(key) != std::string::npos;
    };
    // `processors(1)` and `nodata` are the two the corpus writes and the two
    // that decide the shape below. Everything else that would change what a
    // child SEES -- by(), seeds(), randtype(), programs(), noglobals, mata --
    // is refused by name: accepting one silently would promise a scheme this
    // build does not run.
    for (const char* bad : {"by(", "seeds(", "randtype(", "programs(", "noglobals", "mata",
                            "keepusing(", "setparallelid("}) {
      if (optHas(bad)) {
        return refuse(std::string("parallel: option ") + bad +
                          " changes what each cluster receives, and this build runs the work "
                          "sequentially in one process. Refused rather than accepted-and-ignored.",
                      198);
      }
    }
    const bool noData = optHas("nodata");
    auto wholeInt = [](const std::string& t, long long* out) {
      if (t.empty()) return false;
      size_t i = (t[0] == '+' || t[0] == '-') ? 1 : 0;
      if (i >= t.size()) return false;
      for (size_t k = i; k < t.size(); ++k) {
        if (std::isdigit(static_cast<unsigned char>(t[k])) == 0) return false;
      }
      *out = std::stoll(t);
      return true;
    };
    long long nClusters = 1;
    {
      if (!wholeInt(*clusters, &nClusters)) nClusters = 1;
      size_t pp = lower(optTail).find("processors(");
      if (pp != std::string::npos) {
        size_t close = optTail.find(')', pp);
        if (close != std::string::npos) {
          long long p = 0;
          if (wholeInt(trimWs(optTail.substr(pp + 11, close - pp - 11)), &p) && p > 0)
            nClusters = p;
        }
      }
    }

    const bool hasRows = session.hasData();
    if (!noData && hasRows && nClusters > 1) {
      return refuse("parallel: without `nodata` the package SPLITS the rows across " +
                        std::to_string(nClusters) +
                        " clusters, runs the file on each slice and pastes the results back. "
                        "What each run sees depends on the split, and the package cannot be run "
                        "here to check a sequential reproduction against. Use `nodata`, or "
                        "processors(1), or run the file directly.",
                    199);
    }

    // Sequential run. With `nodata` the children start with NO data and nothing
    // comes back, so the parent's dataset is cleared for the run and restored
    // after it. With one cluster and data, the child's dataset IS the result
    // (fusion of a single cluster), so it is left in place.
    //
    // The snapshot goes through `preserve`/`restore` rather than through a
    // saved hbin path plus `clear`: `clear` retires the scratch file the path
    // named, so restoring it afterwards fails with "the working dataset names a
    // scratch file that was never written" -- which is how the first cut of
    // this arm died, on the run rather than on a check.
    bool snapped = false;
    if (noData && hasRows) {
      ojson pres = handlers::runPreserve(session);
      snapped = true;
      (void)pres;
      (void)dispatch("clear", session);
    }
    doexec::ExecuteResult r =
        isDo ? doexec::executeDo(body, session, dispatch).result
             : doexec::executeDoSource(body, session, dispatch);
    for (const auto& b : r.blocks) blocks.push_back(b);
    if (snapped) {
      ojson res = handlers::runRestore(session);
      (void)res;
    }
    return blocks;
  }
  if (cmd == "strdist") {
    // AER scope 2 B2: the last name blocking paper 100.
    pushEnvResult(handlers::runStrdist(parsed, session));
    return blocks;
  }
  if (cmd == "tolower" || cmd == "renvars" || cmd == "move") {
    // pushEnvResult flattens an array and drops nulls, which is the shape all
    // three return: an array of display blocks (possibly empty, which is how
    // the silent path says "nothing to print").
    pushEnvResult(cmd == "tolower"  ? handlers::runTolower(parsed, session)
                  : cmd == "renvars" ? handlers::runRenvars(parsed, session)
                                     : handlers::runMove(parsed, session));
    return blocks;
  }

  if (cmd == "grstyle") {
    // Accept-note, not a silent no-op: this build applies no scheme styling, and
    // saying so once is the difference between "your styling was ignored" and a
    // user believing it took effect. A refusal would stop do-files that open
    // with `grstyle init` before any real work.
    ojson note;
    note["type"] = "display";
    note["text"] = "(grstyle: scheme styling is not applied in this build — accepted and ignored)";
    blocks.push_back(note);
    return blocks;
  }
  if (cmd == "geodist") {
    pushEnvResult(handlers::runGeodist(parsed, session));
    return blocks;
  }
  if (cmd == "geonear") {
    pushEnvResult(handlers::runGeonear(parsed, session));
    return blocks;
  }
  if (cmd == "cdfplot") {
    ojson result = ojson(handlers::runCdfplot(parsed, session));
    registerGraphOutput(result, session, parsed.opts);  // BS-011
    if (!(result.is_array() && result.empty())) blocks.push_back(result);
    return blocks;
  }
  if (cmd == "event_plot") {
    ojson result = ojson(handlers::runEventPlot(parsed, session));
    registerGraphOutput(result, session, parsed.opts);  // BS-011
    if (!(result.is_array() && result.empty())) blocks.push_back(result);
    return blocks;
  }

  // ==================== AER-SCOPE1 bundle I (UTIL) arms =====================
  if (cmd == "unique" || cmd == "gunique") {
    // One arm, two spellings: gtools' `gunique` differs from `unique` only in
    // name, and giving it its own handler would be a second thing to keep in
    // step. The handler reads `parsed.cmd` for the two options that ARE
    // gunique-only and refuses them by name.
    pushEnvResult(handlers::runUnique(parsed, session));
    return blocks;
  }
  if (cmd == "distinct") {
    pushEnvResult(handlers::runDistinct(parsed, session));
    return blocks;
  }
  if (cmd == "missings") {
    pushEnvResult(handlers::runMissings(parsed, session));
    return blocks;
  }
  if (cmd == "mdesc") {
    pushEnvResult(handlers::runMdesc(parsed, session));
    return blocks;
  }
  if (cmd == "labmask") {
    pushEnvResult(handlers::runLabmask(parsed, session));
    return blocks;
  }
  if (cmd == "renvarlab") {
    pushEnvResult(handlers::runRenvarlab(parsed, session));
    return blocks;
  }
  if (cmd == "splitvallabels") {
    pushEnvResult(handlers::runSplitvallabels(parsed, session));
    return blocks;
  }
  if (cmd == "marksample") {
    pushEnvResult(handlers::runMarksample(parsed, session));
    return blocks;
  }
  if (cmd == "filelist") {
    pushEnvResult(handlers::runFilelist(parsed, session));
    return blocks;
  }
  if (cmd == "carryforward") {
    pushEnvResult(handlers::runCarryforward(parsed, session));
    return blocks;
  }
  if (cmd == "rangestat") {
    pushEnvResult(handlers::runRangestat(parsed, session));
    return blocks;
  }
  if (cmd == "tsspell") {
    pushEnvResult(handlers::runTsspell(parsed, session));
    return blocks;
  }

  // ==================== AER-SCOPE1 bundle G (IO-TEXT) arms ==================
  if (cmd == "insheet") {
    pushEnvResult(handlers::runInsheet(parsed, session));
    return blocks;
  }
  if (cmd == "outsheet") {
    pushEnvResult(handlers::runOutsheet(parsed, session));
    return blocks;
  }
  if (cmd == "infile") {
    pushEnvResult(handlers::runInfile(parsed, session));
    return blocks;
  }
  if (cmd == "infix") {
    pushEnvResult(handlers::runInfix(parsed, session));
    return blocks;
  }
  if (cmd == "outfile") {
    pushEnvResult(handlers::runOutfile(parsed, session));
    return blocks;
  }
  if (cmd == "saveold") {
    pushEnvResult(handlers::runSaveold(parsed, session));
    return blocks;
  }
  if (cmd == "putexcel") {
    // RAW line, not `parsed`: putexcel's grammar is `A1 = <value>`, and the
    // tokenizer splits on `=` and on the commas inside `matrix(M), names`, so
    // a parsed varlist cannot reconstruct the assignment. Same reason the
    // `matrix` arm re-parses its own raw line.
    pushEnvResult(handlers::runPutexcel(rawWithCanonicalCmd(parsed.raw, cmdTyped, cmd), session));
    return blocks;
  }
  if (cmd == "filefilter") {
    pushEnvResult(handlers::runFilefilter(parsed, session));
    return blocks;
  }
  if (cmd == "copy") {
    pushEnvResult(handlers::runCopy(parsed, session));
    return blocks;
  }
  if (cmd == "unzipfile" || cmd == "zipfile") {
    blocks.push_back(ojson(eval::refusedCommand(cmdTyped, "no_zip_library")));
    return blocks;
  }
  if (cmd == "odbc") {
    blocks.push_back(ojson(eval::refusedCommand(cmdTyped, "no_odbc_driver")));
    return blocks;
  }
  if (cmd == "spshape2dta") {
    blocks.push_back(ojson(eval::refusedCommand(cmdTyped, "no_shapefile_reader")));
    return blocks;
  }

  // ==================== AER-SCOPE1 bundle F (RESHAPE) arms ==================
  // Shape and sample verbs. Three of them round-trip the whole frame through a
  // CSV and therefore go through the pre-flight gate first -- the same shape
  // the `expand` arm above uses: reject net, then pre-flight, then handler.
  // `pushEnvResult` flattens and drops nulls (defect class 1).
  if (cmd == "split") {
    pushEnvResult(handlers::runSplit(parsed, session));
    return blocks;
  }
  if (cmd == "xpose") {
    if (preflight::gate("xpose", session, "", 1, blocks) == preflight::Verdict::Cancel) return blocks;
    pushEnvResult(handlers::runXpose(parsed, session));
    return blocks;
  }
  if (cmd == "recast") {
    pushEnvResult(handlers::runRecast(parsed, session));
    return blocks;
  }
  if (cmd == "insobs") {
    pushEnvResult(handlers::runInsobs(parsed, session));
    return blocks;
  }
  if (cmd == "stack") {
    {
      // The pre-flight factor is the number of variables being stacked, which
      // is how many copies of the row count the result carries.
      const int64_t k = static_cast<int64_t>(parsed.varlist.size());
      if (preflight::gate("stack", session, "", k > 0 ? k : 1, blocks) == preflight::Verdict::Cancel)
        return blocks;
    }
    pushEnvResult(handlers::runStack(parsed, session));
    return blocks;
  }
  if (cmd == "separate") {
    pushEnvResult(handlers::runSeparate(parsed, session));
    return blocks;
  }
  if (cmd == "mvencode") {
    pushEnvResult(handlers::runMvencode(parsed, session));
    return blocks;
  }
  if (cmd == "sample") {
    pushEnvResult(handlers::runSample(parsed, session));
    return blocks;
  }
  if (cmd == "splitsample") {
    pushEnvResult(handlers::runSplitsample(parsed, session));
    return blocks;
  }
  if (cmd == "bsample") {
    if (preflight::gate("bsample", session, "", 1, blocks) == preflight::Verdict::Cancel)
      return blocks;
    pushEnvResult(handlers::runBsample(parsed, session));
    return blocks;
  }
  if (cmd == "drawnorm") {
    pushEnvResult(handlers::runDrawnorm(parsed, session));
    return blocks;
  }
  if (cmd == "input") {
    // A FOLDED `input` block was intercepted on the raw line far above; only a
    // bare `input` with no block reaches here, and it gets an honest error
    // rather than silently doing nothing.
    pushEnvResult(handlers::runInput(parsed.raw, session));
    return blocks;
  }
  if (cmd == "notes") {
    pushEnvResult(handlers::runNotes(parsed, session));
    return blocks;
  }
  if (cmd == "compare") {
    pushEnvResult(handlers::runCompare(parsed, session));
    return blocks;
  }
  if (cmd == "cumul") {
    pushEnvResult(handlers::runCumul(parsed, session));
    return blocks;
  }
  if (cmd == "pctile") {
    pushEnvResult(handlers::runPctile(parsed, session));
    return blocks;
  }
  if (cmd == "_pctile") {
    // r-class: `_pctile x, p(10 90)` publishes r(r1)/r(r2) and touches no
    // column. The handler fills r() itself here (it is the whole point of the
    // command), so unlike `gstats sum` this arm does not call
    // captureRFromBlock -- there is no table block to capture FROM.
    pushEnvResult(handlers::runPctileR(parsed, session));
    return blocks;
  }

  // ==================== AER-SCOPE1 bundle E (TABLES) arms ====================
  // Placed beside eststo/esttab, the verbs they extend. Every arm routes
  // through `pushEnvResult`, which flattens an array and drops a null: a
  // handler returning a JSON ARRAY where the arm pushes ONE block is the defect
  // class that has now been caught three times in four bundles (twice by
  // review, once by the selftest binary aborting on type_error.306), so it is
  // fixed HERE, at the single call shape, rather than per handler.
  if (cmd == "estadd") {
    pushEnvResult(handlers::runEstadd(parsed, session));
    return blocks;
  }
  if (cmd == "estpost") {
    pushEnvResult(handlers::runEstpost(parsed, session));
    return blocks;
  }
  if (cmd == "estout") {
    pushEnvResult(handlers::runEstout(parsed, session));
    return blocks;
  }
  if (cmd == "outreg2") {
    pushEnvResult(handlers::runOutreg2(parsed, session));
    return blocks;
  }
  if (cmd == "texsave") {
    pushEnvResult(handlers::runTexsave(parsed, session));
    return blocks;
  }
  if (cmd == "texdoc") {
    pushEnvResult(handlers::runTexdoc(parsed, session));
    return blocks;
  }
  if (cmd == "tex") {
    pushEnvResult(handlers::runTexdocLine(parsed, session));
    return blocks;
  }
  if (cmd == "frmttable") {
    pushEnvResult(handlers::runFrmttable(parsed, session));
    return blocks;
  }
  if (cmd == "regsave") {
    pushEnvResult(handlers::runRegsave(parsed, session));
    return blocks;
  }
  if (cmd == "parmest") {
    pushEnvResult(handlers::runParmest(parsed, session));
    return blocks;
  }
  if (cmd == "mat2txt") {
    pushEnvResult(handlers::runMat2txt(parsed, session));
    return blocks;
  }
  if (cmd == "erepost") {
    pushEnvResult(handlers::runErepost(parsed, session));
    return blocks;
  }
  if (cmd == "tabout") {
    // Refused at the COMMAND level, not the option level: tabout's grammar is a
    // panel language of its own, and accepting the head while refusing every
    // option would report a missing option where the truth is a missing
    // command. `refusedCommand` (not `engineError`) so `eval::coarseRc` answers
    // 199 -- the default code answers 1, which is bundle D's shipped defect.
    blocks.push_back(ojson(eval::refusedCommand(cmdTyped, "tabout_grammar")));
    return blocks;
  }

  if (cmd == "esttab") {
    // AER-SCOPE1 bundle E: `esttab ... using f.tex` has to leave a FILE behind.
    // Until now this arm returned a JSON block and wrote nothing, so every
    // corpus line that ended in `using` succeeded and produced no output file.
    // The path is split off HERE so `runEsttab` still receives pure model
    // names, and the block it returns is exactly what gets written -- one
    // renderer, so the on-screen table and the file cannot disagree.
    std::vector<std::string> names = parsed.varlist;
    std::string usingPath;
    for (size_t i = 0; i < names.size(); ++i) {
      if (lower(names[i]) == "using") {
        if (i + 1 < names.size()) usingPath = names[i + 1];
        names.erase(names.begin() + static_cast<long>(i), names.end());
        break;
      }
    }
    // W1a-2 / BUG-407: the cell layer lives in the FILE writer. `runEsttab`
    // builds a JSON estimates_table for the UI and has no cell model at all, so
    // a `cells()`/`main()`/`aux()` on a screen-only `esttab` would be accepted
    // and ignored -- which is the very thing BUG-407 was filed against, just
    // moved to the other surface. Refused BY NAME here, where the `using` half
    // is already known, so the promise "accepted means honoured" holds on both.
    if (usingPath.empty()) {
      for (const auto& [k, v] : parsed.opts) {
        if (k != "cells" && k != "main" && k != "aux") continue;
        blocks.push_back(ojson(eval::unsupportedOption(
            k, "esttab: the cell layer is implemented for `esttab ... using FILE`, which renders "
               "the table; the on-screen block carries no cell spec, so this option is refused "
               "rather than silently ignored")));
        return blocks;
      }
    }
    ojson result = ojson(handlers::runEsttab(names, parsed.opts, session));
    blocks.push_back(result);
    if (!usingPath.empty() && result.value("type", std::string()) != "error") {
      pushEnvResult(handlers::writeEsttabFile(result, usingPath, parsed.opts, session));
    }
    return blocks;
  }

  if (cmd == "coefplot") {
    // W4: the net is HERE, not inside runCoefplot, because `event_plot`
    // (handlers_aer_graphx.cpp) delegates to that handler with options it
    // synthesised itself -- a net inside the handler would judge event_plot's
    // vocabulary by coefplot's. This arm sees only what the user typed.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabCoefplot.known,
                                              kVocabCoefplot.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    ojson result = ojson(handlers::runCoefplot(parsed.varlist, parsed.opts, session));
    graphAcceptedNotApplied(result, "coefplot", parsed.opts, kCoefplotApplied);
    registerGraphOutput(result, session, parsed.opts);  // BS-011
    blocks.push_back(result);
    return blocks;
  }

  if (cmd == "marginsplot") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMarginsplot.known,
                                              kVocabMarginsplot.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    ojson result = ojson(handlers::runMarginsplot(parsed.opts, session));
    graphAcceptedNotApplied(result, "marginsplot", parsed.opts, kMarginsplotApplied);
    registerGraphOutput(result, session, parsed.opts);  // BS-011
    blocks.push_back(result);
    return blocks;
  }

  // ------------------------------------------------------------------
  // M4a item 4 + M4b: graph JSON surface. Scope: scatter/histogram/
  // kdensity/line/xtline/tsline + the bare (non-parenthesised) `twoway
  // PLOTTYPE var...` single-plot form (M4a); `graph export`/`graph save`/
  // `graph combine` (M4b — see graph_m4.cpp's runGraphExport/Save/Combine).
  // The parenthesised multi-layer `twoway (rarea ...) (scatter ...)` form
  // (twoway_opts.py/twoway_parse.py) remains M4a-remaining/out of scope —
  // rejected explicitly below, not silently mis-parsed.
  if (cmd == "scatter" || cmd == "histogram" || cmd == "hist" || cmd == "kdensity" ||
      cmd == "line" || cmd == "xtline" || cmd == "tsline" || cmd == "twoway" || cmd == "graph") {
    std::string gc = cmd;
    std::vector<std::string> vl = parsed.varlist;
    std::optional<std::string> ifExpr = parsed.ifExpr;

    if (cmd == "graph") {
      std::string sub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
      if (sub == "export") {
        // The filename can contain spaces (the Korean Desktop is "바탕 화면", and
        // any "My Folder\..." path), so extract it from the RAW command like
        // `use` does — parsed.varlist[1] is whitespace-tokenized and truncates a
        // spaced path at the first space (-> ext "" -> "unsupported format ''").
        // BUG-251/WS-Q: was `lower(raw).find("export")` + `substr(pos + 6)`.
        // npos-GUARDED, so this pair never crashed — but it is the same
        // re-derivation, with a literal copy of a SUB-command spelling, and its
        // npos path degrades silently to the whitespace-tokenized varlist that
        // the site exists to avoid. Two token-skips is what "after `graph
        // export`" means, and it needs no spelling at all.
        std::string exAfter = rawArgsAfterCmd(rawArgsAfterCmd(parsed.raw));
        size_t exComma = exAfter.find(',');  // drop trailing `, replace`/`width()` options
        if (exComma != std::string::npos) exAfter = trimWs(exAfter.substr(0, exComma));
        if (exAfter.size() >= 2 && ((exAfter.front() == '"' && exAfter.back() == '"') ||
                                    (exAfter.front() == '\'' && exAfter.back() == '\'')))
          exAfter = exAfter.substr(1, exAfter.size() - 2);
        std::string filepath = !exAfter.empty()
                                   ? exAfter
                                   : (parsed.varlist.size() > 1 ? parsed.varlist[1] : std::string());
        if (filepath.empty()) {
          blocks.push_back(ojson(eval::engineError("syntax_error: `graph export` requires a filename")));
          return blocks;
        }
        blocks.push_back(ojson(handlers::runGraphExport(filepath, parsed.opts, session)));
        return blocks;
      }
      if (sub == "save") {
        std::optional<std::string> name;
        std::optional<std::string> filepath;
        // `graph save [NAME] ["file.gph"] [, replace]` — a bare NAME token
        // (no quotes needed) vs a quoted/extension-bearing path are
        // disambiguated the same way dispatcher.py's tokenizer already
        // separates them: varlist[1] is NAME if a second varlist token
        // exists and doesn't look like a path (no '.'/'/'), else it's
        // treated as the save-path.
        // Space-safe: a leading quoted path (possibly with spaces / Korean,
        // e.g. the "바탕 화면" Desktop) is taken whole from the raw text, since
        // the whitespace-tokenized varlist would truncate it at the space
        // (and leave the opening quote in, mangling resolveUserPath).
        // BUG-251/WS-Q: see the `graph export` twin just above.
        std::string svAfter = rawArgsAfterCmd(rawArgsAfterCmd(parsed.raw));
        size_t svComma = svAfter.find(',');
        if (svComma != std::string::npos) svAfter = trimWs(svAfter.substr(0, svComma));
        if (!svAfter.empty() && (svAfter.front() == '"' || svAfter.front() == '\'')) {
          char q = svAfter.front();
          size_t end = svAfter.find(q, 1);
          filepath = (end != std::string::npos) ? svAfter.substr(1, end - 1) : svAfter.substr(1);
        } else if (parsed.varlist.size() > 1) {
          const std::string& tok = parsed.varlist[1];
          if (tok.find('.') != std::string::npos || tok.find('/') != std::string::npos) {
            filepath = tok;
          } else {
            name = tok;
            if (parsed.varlist.size() > 2) filepath = parsed.varlist[2];
          }
        }
        blocks.push_back(ojson(handlers::runGraphSave(name, filepath, parsed.opts, session)));
        return blocks;
      }
      if (sub == "combine") {
        std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
        ojson combined = ojson(handlers::runGraphCombine(names, parsed.opts, session));
        // W4: runGraphCombine reads exactly these eight (graph_m4.cpp:2606-2642).
        // The corpus also hands it `scale()` (25 uses) and `xcommon` (1), which
        // it does not read; those now resolve (kGraphCosmetic) and are named in
        // the note instead of vanishing. No reject net is added here: this
        // sub-command arm never had one, and widening it is W4's neighbour, not
        // W4 -- what the wave owes is that an accepted option is never silent.
        static const std::unordered_set<std::string> kCombineApplied = {
            "rows", "cols", "title", "subtitle", "note", "name", "imargin", "graphregion"};
        graphAcceptedNotApplied(combined, "graph combine", parsed.opts, kCombineApplied);
        blocks.push_back(combined);
        return blocks;
      }
      if (sub == "drop") {
        // BS-056: `graph drop {namelist | _all}` -- only `parsed.varlist[1]`
        // (the FIRST name) was ever read, so `graph drop g1 g2` silently
        // dropped g1 and said nothing about g2; a name that didn't match
        // ANY registered graph (including a literal "all" typed without
        // the leading underscore Stata's own grammar requires) hit the same
        // `erase()` no-op-if-absent call and still reported "(graph
        // dropped)" — a silent false success either way. Now: every name
        // token is processed; `_all` (case-insensitive, anywhere in the
        // list) clears the whole named-graph store; otherwise every name is
        // validated to exist BEFORE any of them are dropped (all-or-nothing,
        // no partial drop on error) and an unknown name is an honest reject
        // naming it, never a silent no-op.
        std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
        bool dropAll = false;
        for (const auto& nm : names) {
          if (lower(nm) == "_all") {
            dropAll = true;
            break;
          }
        }
        if (dropAll) {
          session.namedGraphs.clear();
        } else if (!names.empty()) {
          for (const auto& nm : names) {
            if (session.namedGraphs.count(nm) == 0) {
              blocks.push_back(ojson(eval::engineError("graph drop: no graph named " + nm)));
              return blocks;
            }
          }
          for (const auto& nm : names) session.namedGraphs.erase(nm);
        }
        ojson dropOut;
        dropOut["type"] = "display";
        dropOut["class"] = "note";
        dropOut["text"] = "(graph dropped)";
        blocks.push_back(dropOut);
        return blocks;
      }
      // `graph twoway ...` / other `graph <sub>` forms: strip the leading
      // "graph" token's sub-command and fall through to the plot dispatch
      // below (mirrors commands.py's final `sub_varlist = parsed.varlist[1:]`
      // fallthrough).
      if (sub == "twoway") {
        gc = "twoway";
        vl = std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end());
      } else if (!sub.empty()) {
        gc = sub;
        vl = std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end());
      } else {
        gc = "scatter";
      }
    }

    if (!session.hasData()) {
      blocks.push_back(ojson(eval::noDataLoaded()));
      return blocks;
    }

    if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
      blocks.push_back(ojson(eval::engineError(
          "graph: `in` range is not yet supported for graph commands; use `if`.",
          "syntax_error")));
      return blocks;
    }

    // `twoway (plottype ...) (plottype ...) ...` (parenthesised multi-layer
    // form): route through runTwowayLayers when raw_args contains a
    // top-level paren group — mirrors dispatcher.py's "Stage-4: multi-layer
    // twoway" branch (raw_args extraction regex `^\s*twoway\b\s*(.*)$` /
    // `^\s*graph\s+twoway\b\s*(.*)$`, checked BEFORE the bare single-plot
    // form below so a parenthesised layer group never falls through to the
    // bare-form plot-type stripping).
    if (gc == "twoway") {
      static const std::regex reTwowayRaw(R"(^\s*twoway\b\s*(.*)$)", std::regex::icase);
      static const std::regex reGraphTwowayRaw(R"(^\s*graph\s+twoway\b\s*(.*)$)", std::regex::icase);
      std::string rawArgs;
      std::smatch m;
      // AER-SCOPE1 bundle A: `tw (sc y x)` and `gr twoway ...` resolve to the
      // canonical `cmd` at dispatch, but THESE regexes read the raw line, which
      // still carries what the user typed. rawWithCanonicalCmd rewrites just the
      // leading word (BUG-251's fix, reused here) so the abbreviation reaches
      // the same layer parser as the full spelling.
      const std::string graphRaw = rawWithCanonicalCmd(parsed.raw, cmdTyped, cmd);
      if (cmd == "graph" && std::regex_match(graphRaw, m, reGraphTwowayRaw)) {
        rawArgs = m[1].str();
      } else if (std::regex_match(graphRaw, m, reTwowayRaw)) {
        rawArgs = m[1].str();
      }
      // BS-053: `||`-separated layers, no parens at all -- synthesize the
      // parenthesised equivalent BEFORE the hasToplevelParens gate below so
      // it takes the exact same multi-layer path a hand-parenthesised
      // command would (see expandDoublePipeLayers' doc comment above). A
      // no-op (returns the input unchanged) when there's no top-level `||`,
      // or when the command already used real parens.
      if (!rawArgs.empty() && !twoway::hasToplevelParens(rawArgs)) {
        rawArgs = expandDoublePipeLayers(rawArgs);
      }
      if (!rawArgs.empty() && twoway::hasToplevelParens(rawArgs)) {
        twoway::ParseResult pr = twoway::parseTwoway(rawArgs);
        // W4: the layer parser reads the comma side ITSELF (twoway_parse.cpp's
        // parseLayerInterior / parseTwoway step 1), so these keys never passed
        // through the dispatcher's canonicalizeForCommand and arrived at
        // layerStyle spelled the way the user typed them. That is BUG-233's
        // shape exactly: the net (now) accepts `lc(gs11)` as a prefix of
        // `lcolor`, and layerStyle -- which compares raw keys and reads `lcolor`,
        // `lw`, `lp`, `mc`, `ms`, `msi`, `mz` but NOT `lc` -- then dropped it
        // unread. Corpus-measured: `lc` (4 uses), `msymb` (14), `xti`/`yti`,
        // `xlab`/`ylab`, `xsc`/`ysc`, `leg`, `horiz`. Every one of them is a
        // unique prefix of a member of the registered vocabulary, so the prefix
        // rule reaches all of them and the registration needs no declared-alias
        // table (BUG-430's mechanism is for the ones a prefix CANNOT reach, and
        // this surface has none in the corpus).
        for (auto& lspec : pr.layers) {
          eval::canonicalizeForCommand(lspec.opts, "twoway_layer");
        }
        eval::canonicalizeForCommand(pr.globalOpts, "twoway_layer");
        if (pr.layers.empty()) {
          blocks.push_back(ojson(eval::engineError(
              "twoway: no plot layers found. Check syntax.", "syntax_error")));
          return blocks;
        }
        // Merge global opts: layer-parser opts are the base; tokenizer opts
        // (parsed.opts) fill any gaps (both parse the same comma side) —
        // mirrors dispatcher.py's `merged = dict(g_opts); for k,v in
        // opts.items(): merged.setdefault(k, v)`.
        std::vector<std::pair<std::string, eval::OptValue>> merged = pr.globalOpts;
        for (const auto& kv : parsed.opts) {
          const std::string& k = kv.first;
          bool exists = std::any_of(merged.begin(), merged.end(),
                                     [&k](const auto& p) { return p.first == k; });
          if (!exists) merged.emplace_back(kv.first, kv.second);
        }
        // Use ONLY the layer parser's global if — the tokenizer's if_expr is
        // meaningless here (its greedy `\bif` match swallows paren groups).
        Datastore* dsPtrTw = session.datastore();
        // W4: the unknown-option net for BOTH planes (the global block and each
        // layer's interior) lives inside runTwowayLayers, AFTER its
        // supported-plot-type pre-scan, so `(rcap ...)` still answers "plot type
        // is not supported" rather than complaining about `lstyle`. See
        // graph_m4.cpp. Only the note is assembled here, because only the
        // dispatcher knows which of the two routes produced the block.
        //
        // The note reads the global block AND every layer interior: a cosmetic
        // option written inside a plot's own parentheses is just as silently
        // dropped as one written after the comma, so both are reported.
        std::vector<std::pair<std::string, eval::OptValue>> twNoteOpts = merged;
        for (const auto& lspec : pr.layers) {
          for (const auto& kv : lspec.opts) twNoteOpts.push_back(kv);
        }
        ojson twResult = ojson(handlers::runTwowayLayers(dsPtrTw, session, pr.layers, merged, pr.ifExpr));
        {
          std::unordered_set<std::string> twApplied = {"title", "subtitle", "xtitle", "ytitle",
                                                       "note", "name"};
          twApplied.insert(kGraphTwowayGlobals.begin(), kGraphTwowayGlobals.end());
          twApplied.insert(kGraphLayerStyle.begin(), kGraphLayerStyle.end());
          graphAcceptedNotApplied(twResult, "twoway", twNoteOpts, twApplied);
        }
        if (twResult.is_object() && twResult.value("type", std::string()) == "graph") {
          session.lastGraph = twResult;
          std::string nameOpt;
          for (const auto& [k, v] : merged) {
            if (k == "name" && !v.isBool) nameOpt = v.strValue;
          }
          if (nameOpt.empty()) {
            for (const auto& [k, v] : parsed.opts) {
              if (k == "name" && !v.isBool) nameOpt = v.strValue;
            }
          }
          if (!nameOpt.empty()) {
            static const std::regex reNameSplit(R"([,\s]+)");
            std::sregex_token_iterator it(nameOpt.begin(), nameOpt.end(), reNameSplit, -1), endIt;
            std::string nm = (it != endIt) ? it->str() : nameOpt;
            if (!nm.empty()) {
              session.namedGraphs[nm] = twResult;
              twResult["name"] = nm;
            }
          }
        }
        handlers::decimateGraphBlockForView(twResult);  // L2-FIX-B (BUG-066): live-view only
        blocks.push_back(twResult);
        return blocks;
      }
    }

    // `twoway PLOTTYPE var...` (bare, non-parenthesised form): strip the
    // leading plot-type keyword.
    if (gc == "twoway") {
      // BS-054: `twoway kdensity x` / `twoway histogram x` (the umbrella-
      // prefixed spellings) previously weren't in this set at all, so
      // "kdensity"/"histogram" fell straight through as an ordinary
      // varlist token — `gc` degraded to "scatter" and `vl` still started
      // with the literal word "kdensity"/"histogram", which runScatter then
      // tried to look up AS A VARIABLE ("variable kdensity not found" —
      // exactly the reported symptom). The un-prefixed bare forms
      // (`kdensity x` / `histogram x`, no leading "twoway") already worked
      // fine via their own top-level `cmd` dispatch below; adding these two
      // names here just makes the "twoway "-prefixed spelling strip down to
      // the SAME already-working gc=="kdensity"/gc=="histogram" branch
      // further down this function (no new handler needed).
      static const std::set<std::string> kTwowayPlotTypes = {
          "line",     "scatter", "connected", "area",      "bar",      "spike",
          "dot",      "dropline", "rarea",    "rcap",      "rcapsym",
          "kdensity", "histogram", "hist",
      };
      if (!vl.empty() && kTwowayPlotTypes.count(lower(vl[0]))) {
        gc = lower(vl[0]);
        vl = std::vector<std::string>(vl.begin() + 1, vl.end());
      } else {
        gc = "scatter";
      }
    }

    Datastore* dsPtr = session.datastore();
    std::string tmpGraphHbin;
    std::unique_ptr<Datastore> tmpDs;
    if (ifExpr.has_value() && !ifExpr->empty()) {
      tmpGraphHbin = dispatcherNextPath(session.workdir(), "graphif");
      // BUG-369 follow-up: `twoway ... if L.y>0` — same injection as above.
      std::vector<std::string> args = translate::injectTsContextForExpr(
          {"eval", "--data", dsPtr->path(), "--keepif", *ifExpr, "--out", tmpGraphHbin},
          *ifExpr, session);
      try {
        runCobraJson(args);
      } catch (const CobraError& ce) {
        // BUG-038(a): route through the shared mapping point instead of
        // formatting "[code] message" inline (piet::cobraErrorMessage,
        // cobra_runner.h) — same fix as every other CobraError catch site.
        blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
        return blocks;
      }
      tmpDs = std::make_unique<Datastore>(tmpGraphHbin);
      dsPtr = tmpDs.get();
    }

    ojson result;
    // BUG-034: `graph bar/hbar/pie/box/dot ...` used to fall straight
    // through to the `else` below and get misrouted through runScatter,
    // producing the misleading "scatter requires two variables" error
    // instead of an honest gap (root cause: unrecognized graph
    // sub-commands set gc=sub above with nothing downstream to catch
    // them). Mirrors the parenthesised multi-layer `twoway (plottype ...)`
    // form's reject-before-output guard (runTwowayLayers' supported-plot-type
    // pre-scan, graph_m4.cpp — BUG-129) — applied here to the single-plot
    // dispatch path so these name the gap explicitly (SPEC I-3) rather than
    // silently misrouting to scatter's grammar.
    static const std::set<std::string> kUnsupportedBareGraphTypes = {
        "bar", "hbar", "pie", "box", "dot",
    };
    // BUG-129/BUG-130 (graph-honesty): the bare (non-parenthesised)
    // `twoway rarea ...` / `twoway rcap ...` / `twoway area ...` etc. spellings
    // previously fell through to runScatter below — `rarea` silently
    // misrendered as a 2-var scatter (its band never built; BUG-130) and every
    // unsupported range-plot type drew an empty/wrong plot with rc=0 (BUG-129
    // silent-drop). Route these twoway-specific plot types through the SAME
    // layer machinery the parenthesised form uses (a single synthesised
    // layer): rarea reaches its real band builder, and every unsupported type
    // gets runTwowayLayers' honest reject-before-output. scatter/line/
    // connected/kdensity/histogram keep their dedicated single-plot handlers
    // here (established JSON shapes + selftests unchanged).
    static const std::set<std::string> kBareTwowayLayerRoute = {
        "rarea", "rcap", "rcapsym", "area", "spike", "dropline",
    };
    // BUG-336 (graph arm): this arm had NO unknown-option net at all — nine
    // command tokens accepting any option name and dropping it. The known sets
    // below are MEASURED from graph_m4.cpp's handler bodies (optStr/optBool/
    // optTitleStr reads), not guessed:
    //   every plot: title/xtitle/ytitle/note      (runScatter:271, runGraphLine:849,
    //                                              runXtline:950, runTsline:1054)
    //   histogram : width/bin/freq/frequency/fraction/percent/normal (:387-521)
    //   kdensity  : kernel/bwidth/n/normal                            (:683-794)
    //   xtline    : overlay                                           (:980)
    // `name` is read by the arm itself (namedGraphs capture below), and `legend`
    // is ACCEPTED though nobody reads it: p5.do:167 uses it, so it is corpus-live,
    // and a graph is a visual surface where an unhonoured hint is not a wrong
    // answer — the same accept-visual-hint call already made for `list, noobs`.
    // Options Stata has and PIET does not honour are listed as unimplemented so
    // they say so instead of vanishing.
    // ORDER IS LOAD-BEARING: the unsupported-plot-type and bare-twoway-layer
    // routes below own their own honest rejects, and those are about the COMMAND,
    // not its options. Running this net first made `graph bar y, over(x)` answer
    // "option `over` not allowed" instead of "graph bar is not implemented" —
    // caught by four existing selftest legs (mission 2 / BUG-105(1)), which is
    // exactly what they are for.
    // AER scope-2 W2: the four literal sets below moved to file scope, one
    // copy each, and are now REGISTERED per command token (kVocabGraphBase and
    // friends). This arm is unchanged: it still assembles gknown from gc — the
    // resolved PLOT TYPE, which for `twoway`/`graph` is derived from the
    // varlist and is not the dispatched token — and the registration exists so
    // the census can see the vocabulary, not to decide anything here.
    if (kUnsupportedBareGraphTypes.count(gc) == 0 && kBareTwowayLayerRoute.count(gc) == 0) {
      std::unordered_set<std::string> gknown = kGraphSharedKnown;
      const std::unordered_set<std::string>* gunimpl = &kGraphBaseUnimpl;
      if (gc == "histogram" || gc == "hist") {
        gknown.insert(kGraphHistExtra.begin(), kGraphHistExtra.end());
        gunimpl = &kGraphHistUnimpl;
      } else if (gc == "kdensity") {
        gknown.insert(kGraphKdensExtra.begin(), kGraphKdensExtra.end());
        gunimpl = &kGraphKdensUnimpl;
      } else if (gc == "xtline") {
        gknown.insert(kGraphXtlineExtra.begin(), kGraphXtlineExtra.end());
      }
      if (auto err = eval::rejectUnknownOptions(parsed.opts, gknown, *gunimpl)) {
        blocks.push_back(ojson(*err));
        return blocks;
      }
    }
    // W4 -- THE WIRING GAP, and the reason this is a conditional re-route and
    // not an unconditional one.
    //
    // MEASURED on main today: `scatter y x, xline(0)` was r(198) while
    // `twoway (scatter y x) (line y x), xline(0)` drew the reference line. The
    // renderer for xline/yline/xscale/yscale/xlabel/ylabel/legend/subtitle
    // exists (graph_m4.cpp's `axes`/`legend` block, and graph.js draws both ONLY
    // for kind=="twoway"); the single-plot arm simply could not reach it. So the
    // fix is routing, not a new feature.
    //
    // Unconditional routing is what the plan REFUSED, and the pins say why: the
    // freeze fixtures scatter/graph/line/tsline/histogram/kdensity all pin BARE
    // commands byte for byte, with `"kind":"scatter"` and the flat
    // points/x_var/y_var shape, and two self-test legs (selftest.cpp:3981,
    // :21887) assert kind=="scatter" for a bare `scatter`. A bare or title-only
    // command therefore stays exactly where it is; only a statement that carries
    // one of the eight layer-honoured globals moves.
    //
    // `vl.size() == 2` is load-bearing and was measured too: `scatter y1 y2 x` is
    // Stata's shared-x multi-series form and runScatter builds a `series` array
    // for it (BS-050, graph_m4.cpp:220-260), while `twoway::LayerSpec` carries a
    // single yvar/xvar pair. Re-routing a 3+-token varlist would silently drop
    // every series after the first.
    const bool layerReroutable = (gc == "scatter" || gc == "line" || gc == "connected");
    bool wantsTwowayGlobals = false;
    for (const auto& kv : parsed.opts) {
      if (kGraphTwowayGlobals.count(kv.first) != 0) {
        wantsTwowayGlobals = true;
        break;
      }
    }
    const bool rerouteToLayers = layerReroutable && vl.size() == 2 && wantsTwowayGlobals;
    // The keys the branch taken below actually READS -- the input to the
    // accepted-and-not-applied note. Every bare handler reads these five
    // (runScatter:271 / runGraphLine:849 / runXtline:950 / runTsline:1054 for the
    // four titles, this arm's own name() capture below for `name`).
    std::unordered_set<std::string> gApplied = {"title", "xtitle", "ytitle", "note", "name"};
    if (rerouteToLayers || kBareTwowayLayerRoute.count(gc) != 0) {
      twoway::LayerSpec spec;
      spec.plottype = gc;
      spec.varlist = vl;
      spec.opts = parsed.opts;
      // `dsPtr` is already the (optionally `if`-filtered) datastore from the
      // graphif temp above, so pass no global-if here (avoids a double filter).
      result = ojson(handlers::runTwowayLayers(dsPtr, session, {spec}, parsed.opts,
                                               std::nullopt));
      gApplied.insert(kGraphTwowayGlobals.begin(), kGraphTwowayGlobals.end());
      gApplied.insert(kGraphLayerStyle.begin(), kGraphLayerStyle.end());
    } else if (gc == "histogram" || gc == "hist") {
      result = ojson(handlers::runHistogram(*dsPtr, vl, parsed.opts));
      gApplied.insert(kGraphHistExtra.begin(), kGraphHistExtra.end());
    } else if (gc == "line") {
      result = ojson(handlers::runGraphLine(*dsPtr, vl, parsed.opts));
    } else if (gc == "kdensity") {
      result = ojson(handlers::runKdensity(*dsPtr, vl, parsed.opts));
      gApplied.insert(kGraphKdensExtra.begin(), kGraphKdensExtra.end());
    } else if (gc == "xtline") {
      result = ojson(handlers::runXtline(*dsPtr, vl, session.panelvar, session.timevar, parsed.opts));
      gApplied.insert(kGraphXtlineExtra.begin(), kGraphXtlineExtra.end());
    } else if (gc == "tsline") {
      result = ojson(handlers::runTsline(*dsPtr, vl, session.timevar, parsed.opts));
    } else if (kUnsupportedBareGraphTypes.count(gc) != 0) {
      result = ojson(eval::unsupportedCommand("graph " + gc, std::nullopt, std::nullopt, "piet_gap",
                                              eval::UnsupportedCommandKind::kNotImplemented));
    } else {
      result = ojson(handlers::runScatter(*dsPtr, vl, parsed.opts));
    }
    graphAcceptedNotApplied(result, gc, parsed.opts, gApplied);
    if (!tmpGraphHbin.empty()) cecon::native_unlink(tmpGraphHbin);

    // Item 6 (User ruling 2026-09-09): `kdensity ..., generate(kx ky)`.
    // Runs AFTER dsPtr/tmpGraphHbin are done being read (session.load
    // inside dispatcherWriteGridColumn REPLACES the session head, so any
    // Datastore pointer taken above this point is dangling afterwards) and
    // BEFORE `result["points"]` gets thinned for the console view further
    // down (decimateGraphBlockForView) — this reads the FULL array.
    if (gc == "kdensity" && result.is_object() && result.value("type", std::string()) != "error") {
      std::string genOpt;
      for (const auto& [k, v] : parsed.opts) {
        if ((k == "gen" || k == "generate") && !v.isBool) genOpt = v.strValue;
      }
      if (!genOpt.empty()) {
        // Same idiom as runLpoly's own generate() parse
        // (handlers_aer_estim.cpp): whitespace-split, exactly two names.
        std::istringstream gs(genOpt);
        std::string g1, g2;
        gs >> g1 >> g2;
        if (g1.empty() || g2.empty()) {
          result = ojson(eval::engineError(
              "kdensity generate() requires two names: `generate(gridvar densvar)`",
              "syntax_error"));
        } else if (!result.contains("points") || !result["points"].is_array()) {
          result = ojson(eval::engineError(
              "kdensity: internal error building generate() output", "internal"));
        } else {
          // generate(kx ky): kx = the grid's evaluation points, ky = the
          // density estimates — the SAME order runKdensity's own `points`
          // array carries ([x, density] pairs, graph_m4.cpp).
          std::vector<double> gridVals, densVals;
          gridVals.reserve(result["points"].size());
          densVals.reserve(result["points"].size());
          for (const auto& pt : result["points"]) {
            gridVals.push_back(pt.at(0).get<double>());
            densVals.push_back(pt.at(1).get<double>());
          }
          // Rows beyond the grid are MISSING, never 0 — the same rule
          // runLpoly's own generate() and W3b's tabulate,generate() apply.
          Datastore* liveDs = session.datastore();
          const int64_t nRows =
              liveDs != nullptr ? liveDs->nRows() : static_cast<int64_t>(gridVals.size());
          auto pad = [&](std::vector<double> v) {
            v.resize(static_cast<size_t>(nRows), std::numeric_limits<double>::quiet_NaN());
            return v;
          };
          std::string errMsg;
          if (!dispatcherWriteGridColumn(session, g1, pad(gridVals), &errMsg) ||
              !dispatcherWriteGridColumn(session, g2, pad(densVals), &errMsg)) {
            result = ojson(eval::engineError("kdensity generate(): " + errMsg, "data_io"));
          }
        }
      }
    }

    // M4b: cache the last graph so `graph export`/`graph save` can persist
    // it (mirrors dispatcher.py's `if result.get("type")=="graph": session.
    // last_graph = result`).
    if (result.is_object() && result.value("type", std::string()) == "graph") {
      session.lastGraph = result;
      // BS-010 (U3): the bare (non-parenthesised) plot-type dispatch path
      // above never registered `name()` into namedGraphs — only the
      // parenthesised `twoway (...) (...)` path (~90 lines above) did. This
      // mirrors that same name-capture exactly (first comma/whitespace-
      // delimited token, so a glued `name(G, replace)` suboption is stripped
      // the same way) so `scatter/histogram/kdensity/line/xtline/tsline
      // ..., name(G)` feeds the same named-panels -> `graph combine` idiom
      // the parenthesised twoway form already supports.
      std::string nameOpt;
      for (const auto& [k, v] : parsed.opts) {
        if (k == "name" && !v.isBool) nameOpt = v.strValue;
      }
      if (!nameOpt.empty()) {
        static const std::regex reNameSplit(R"([,\s]+)");
        std::sregex_token_iterator it(nameOpt.begin(), nameOpt.end(), reNameSplit, -1), endIt;
        std::string nm = (it != endIt) ? it->str() : nameOpt;
        if (!nm.empty()) {
          session.namedGraphs[nm] = result;
          result["name"] = nm;
        }
      }
    }

    // L2-FIX-B (BUG-066): thin the LIVE-VIEW copy only. session.lastGraph /
    // namedGraphs were deep-copied full above, so `graph export`/`save` keep
    // every point (golden-pinned); this shrinks the console/bridge block.
    handlers::decimateGraphBlockForView(result);
    blocks.push_back(result);
    return blocks;
  }

  // Everything else: explicit piet_gap, never a silent wrong answer. This is
  // the arm the BUG-105 abbreviation gaps land on — the token simply never
  // resolved to a command table entry, which is what the message now says.
  blocks.push_back(ojson(eval::unsupportedCommand(
      parsed.cmd, std::nullopt, std::nullopt, "piet_gap",
      eval::UnsupportedCommandKind::kUnknownToken)));
  return blocks;
}

// BS-006 (BUG-028): log-tee wrapper around dispatchImpl() above.
//
// dispatchImpl() is heavily self-recursive: capture/quietly/noisily/by/
// bysort prefix-stripping, the eststo/version bare-command forwards, and
// nested do-file/loop/program-body replay (via the `dispatch` function
// pointer handed to doexec::executeDoSource/executeDo and
// program::runProgram from inside dispatchImpl()'s own body) all call BACK
// into `dispatch` from partway through an already-in-flight call. Only the
// TRUE outermost call's finalized return value is "what Stata actually
// showed" for that top-level command — every recursive sub-call's result
// gets folded into (and often filtered/relabeled by — e.g. `quietly`
// dropping non-echo blocks, `capture` swallowing an error down to `_rc`)
// its caller's own return value before THAT reaches its own caller, and so
// on up to the outermost frame. Teeing at every recursion level would
// therefore write the same output to the log multiple times — once raw
// from an inner call, again as part of each enclosing wrapper's own
// combined result.
//
// session.enterDispatch()/exitDispatch() (session_state.h) is a plain
// per-session recursion-depth counter: the FIRST (outermost) call sees
// depth go 0->1; every nested call sees 1->2, 2->3, etc. Only the frame
// that saw the 0->1 transition ever calls session.logTee() — with the
// COMPLETE finalized blocks array (echo + result, already including every
// nested line's own blocks for a `do`/loop/program replay, since those are
// flattened into the outer return value by dispatchImpl()'s existing
// recursive-call sites, not by anything in this wrapper). logTee() itself
// is a silent no-op whenever no log is open (SessionState::logIsOpen()),
// so a session that never calls `log using` — i.e. every pre-existing
// fixture — sees zero behavior change and negligible added cost from this
// wrapper (freeze safety).
//
// The try/catch exists purely so a stray exception from deep inside
// dispatchImpl() still decrements the depth counter and still propagates
// exactly as before this wrapper existed (rethrown unchanged, never
// swallowed) — it does not change dispatchImpl()'s own exception behavior.
// logTee() is likewise wrapped so a logging failure can never affect the
// command's own result; std::ofstream doesn't throw by default (no
// exception mask is ever set here), so this is defense-in-depth, not a
// path this build expects to exercise.
nlohmann::ordered_json dispatch(const std::string& rawCmd, SessionState& session) {
  int depth;
  {
    dprof::Scope __z(dprof::kWrapper);
    depth = session.enterDispatch();
  }
  // AER scope-2 B2-3. bofd()/dofb() resolve `<name>.stbcal` against the
  // session's working directory, but the surfaces that call them do not all
  // carry a session: `display bofd(...)` and `local x = bofd(...)` evaluate in
  // eval::evalArith's PROCESS-WIDE immutable namespace (macros.cpp:1554). One
  // assignment at the outermost dispatch is the whole of that plumbing.
  // It is a RESOLUTION directory only. It used to be part of the cache KEY as
  // well, which made a `cd` silently drop an already-loaded calendar -- Stata's
  // cache is name-keyed and survives the cd (review 2026-09-09; see bcal.h for
  // the measurement table). Two sessions in one process are kept apart by
  // SessionState::resetProcessCaches(), not by this string.
  if (depth == 0) {
    // bcal::searchDirFor, NOT a second copy of the rule: an unset session.cwd
    // must mean the same directory here as it does in the handler's bcalCwd,
    // or install() after `bcal create` keys the cache on one string while
    // lookup() reads it under another (review finding, non-blocking 3).
    bcal::setSearchDir(bcal::searchDirFor(session.cwd.value_or(std::string())));
  }
  nlohmann::ordered_json result;
  try {
    result = dispatchImpl(rawCmd, session);
  } catch (...) {
    // BUG-360 follow-up: drain the section-38 projection ledger on the throwing
    // path too. A command that failed has still written its projections.
    if (session.exitDispatch() == 0) {
      session.reapProjectionScratch();
      session.sweepOrphanScratch("dispatch-exit-throw");  // BUG-389
      session.auditHeadBacked("dispatch-exit-throw");  // BUG-384 invariant
    }
    throw;
  }
  dprof::Scope __zw(dprof::kWrapper);
  if (session.exitDispatch() == 0) {
    session.reapProjectionScratch();
    // BUG-389 (2026-09-02): after the ledgers have had their say, every file left
    // in the workdir that NO ledger owns is dead by definition — the by:/if
    // projection slices, multi-step verb intermediates, fixed-name outputs, the
    // parallel block's protocol files. Measured 173–188 MB per command on a 60 MB
    // frame before this; the acceptance corpus never stepped on the shape.
    session.sweepOrphanScratch("dispatch-exit");
    session.auditHeadBacked("dispatch-exit");  // BUG-384 invariant
  }
  // W:P8: every frame now offers its blocks to logTee, which writes only to
  // the logs whose own depth matches. The unnamed log is still recorded at
  // depth 1 and therefore still teed exactly once per top-level command, so
  // sessions without a `name()`d log are byte-unchanged; a named log opened
  // inside a do-file is recorded at that file's depth and so receives its
  // lines as they run, instead of nothing at all (the outermost tee fires
  // after the file has finished, by which point the named log is closed).
  try {
    // BUG-189: a prefixed line's log view is not its block array. Measured
    // against StataBE 19.5: `quietly display "E"` logs the line as written and
    // nothing it produced (our array legitimately also carries the INNER echo,
    // which the console transcript wants and the log must not have), while
    // `noisily display "F"` inside a quiet scope logs its OUTPUT but still no
    // echo. So the quiet case is teed as a synthesized view rather than
    // filtered, and the noisy case is teed under a visible scope.
    switch (session.consumeLineVisibility()) {
      case SessionState::LineVis::Quiet: {
        nlohmann::ordered_json logView = nlohmann::ordered_json::array();
        logView.push_back(echoBlock(rawCmd));
        for (const auto& b : result) {
          if (b.is_object() && b.value("type", std::string()) == "error") logView.push_back(b);
        }
        session.logTee(logView, depth);
        break;
      }
      case SessionState::LineVis::Noisy: {
        SessionState::LogVerbosityScope __lv(session, /*isQuiet=*/false);
        session.logTee(result, depth);
        break;
      }
      case SessionState::LineVis::None:
      default:
        session.logTee(result, depth);
        break;
    }
  } catch (...) {
    // Logging is a side-effect only; never let it affect the command's
    // own result.
  }
  return result;
}

}  // namespace piet
