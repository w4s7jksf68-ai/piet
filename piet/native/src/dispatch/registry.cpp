// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "registry.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "piet/dispatcher.h"      // AER scope-2 A1: resolveOptionKeyForCommand (--resolve)
#include "piet/eval/errors.h"     // AER scope-2 A1: the OptVocab registry (--dump-optvocab)
#include "piet/eval/exprfns.h"    // AER scope-2 A1: addStataFns/addScalarFn (--dump-functions)
#include "piet/eval/pyeval.h"     // AER scope-2 A1: mathNamespace (--dump-functions)
#include "vendor/nlohmann/json.hpp"

namespace piet::registry {

std::string familyToString(Family f) {
  switch (f) {
    case Family::DataMgmt: return "data_mgmt";
    case Family::Estimation: return "estimation";
    case Family::Graph: return "graph";
    case Family::IO: return "io";
    case Family::Macro: return "macro";
    case Family::Meta: return "meta";
  }
  return "unknown";
}

std::string weightPolicyToString(WeightPolicy w) {
  switch (w) {
    case WeightPolicy::None: return "none";
    case WeightPolicy::FweightOnly: return "fweight_only";
    case WeightPolicy::AweightFweight: return "aweight_fweight";
    case WeightPolicy::PweightOnly: return "pweight_only";
    case WeightPolicy::AweightPweight: return "aweight_pweight";
    case WeightPolicy::AllWeights: return "all_weights";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// THE TABLE. Hand-transcribed from dispatcher.cpp:809-2203 (main@b830908 ==
// op/piet@b830908 at scouting time — see A0-anchor-map.md). Row order
// mirrors arm order in dispatcher.cpp so this can be diffed against the
// source by eye. Where one if-arm structurally serves several DISTINCT
// Stata command names (not spelling-variants of the same word), each name
// gets its own row, same handlerTag, noted inline — full list also called
// out in the A0 task's return summary for operator review.
//
// Field derivation rules applied uniformly (see registry.h's CommandSpec
// doc for the exact rule text):
//   allowInRange = ORIGINALLY (A0) true iff the name was in tokenizer.cpp's
//                  hardcoded list {list, browse, br, gen, generate, g,
//                  replace} — that hardcode is long gone (A3/BS-096 made
//                  this table the tokenizer's live source of truth). BS-090
//                  (BETA campaign, Phase G) then flipped allowInRange=true
//                  for every command whose row-filtering machinery can
//                  actually apply an `in a/b` restriction: summarize/sum,
//                  count, ttest, corr/correlate/pwcorr, regress/reg/r/
//                  feols/reghdfe, tabulate/tab/tab1/tab2, duplicates
//                  (report/tag/list real; drop honest-rejects `in`, same
//                  disclosed boundary as its existing `if` reject), egen,
//                  drop, keep, collapse — see piet/native/include/piet/
//                  inrange_filter.h for the shared mechanism (materialize a
//                  real numeric row-index column via `--bygen IDXVAR=_n`,
//                  since op-cobra's `_n`/`_N` tokens throw outside a bysort
//                  context, then fold `in a/b` into an ordinary numeric
//                  `--keepif` predicate on that column). Every OTHER row
//                  stays false: still-legal-but-unimplemented Stata forms
//                  (BS-090's own scope cap) or genuinely out of scope.
//   weight       = P3 weights wave (engine work that taught cobra real
//                  weighted estimation): per-command allow-set, no longer a
//                  single blanket list. AllWeights on regress/reg/r/feols/
//                  reghdfe (cobra's feols accepts all four types, honest-
//                  rejecting only weight+--fe together); AweightFweight on
//                  summarize/sum/su (cobra's `eval --summarize` gained
//                  --aweight alongside its existing --fweight); PweightOnly
//                  on the new mean/proportion rows (probability weights
//                  only, matching real Stata's own restriction on both);
//                  FweightOnly unchanged on the rest of the old allow-list
//                  {tabulate, tab, tab1, tab2, collapse}; None everywhere
//                  else (unchanged from pre-P3).
//   allowByPrefix = ORIGINALLY true only for generate/gen/g, replace, egen —
//                  the only sub-commands dispatcher.cpp's by-gate
//                  implemented per-group. BS-092 (BETA campaign, Phase G)
//                  added summarize as a fourth (sort-then-slice-then-
//                  summarizeCoreStats-per-group, dispatcher.cpp's `by:`/
//                  `bysort:` interception block); every other combination
//                  is still an explicit rejection (dispatcher.cpp's
//                  BUG-041 fallback, further split by byIllegalInStata
//                  below), so false everywhere else (never silently
//                  flattened).
//   allowIf      = true unless the arm never reads/forwards parsed.ifExpr
//                  (checked per-arm while reading 809-2203; several arms
//                  forward the whole `parsed` struct to a handlers::runXxx
//                  overload that takes ParsedCommand — those are marked
//                  true as "forwarded", not independently re-verified
//                  inside the handler sources, which are out of this pass's read
//                  scope; flagged in the return summary).
//   knownOptions = populated ONLY for summarize/sum, the one arm in this
//                  range with an explicit "option not allowed" rejection
//                  net (dispatcher.cpp ~941-947); every other arm reads
//                  specific option keys without an else-reject branch, so
//                  stays an empty (growth) field per the task brief.
// BUG-105 (ONESHOT-PREP9 WS-K, 2026-08-07) — the `abbrev` lists below were
// corrected against a LIVE StataBE oracle, not against a rule. The measured
// table is internal note BUG105-ABBREV-SOURCING-20260731.md §3 (30 commands × every
// prefix = 190 `capture noisily` attempts, plus 32 `which` identity calls to
// separate "some command ran" from "THAT command ran"); the oracle scripts are
// kept at internal capture.
//
// Three standing rules this table now encodes, all measurement-derived:
//   1. NO computed prefix rule. Stata's accepted set is non-contiguous
//      (`hist`✓ `histo`✗ `histogram`✓ · `tab`✓ `tabl`✗ `table`✓ · `d`=describe
//      but `dr`=nothing), so any prefix rule would ACCEPT SPELLINGS REAL STATA
//      REJECTS — fabricated parity, the exact defect BUG-105 exists to close.
//   2. Inventing an abbreviation is the same defect class as omitting one.
//      histogram/drop/keep/poisson/codebook/tabstat/ttest/table/preserve/
//      scatter were measured as full-spelling-only and are left EXACTLY so.
//   3. The 89 registry rows the oracle never probed stay exact-match-only
//      (today's behaviour, blast radius 0) until probed by the same two-pass
//      method. `probe.do` is the generator, not a fixed list.
//
// Every widened row below is reachable because dispatcher.cpp normalises the
// routed token through registry::resolveCanonical() exactly once — the arms,
// kNoDataCommands and kFixedArityCommands all see the canonical spelling, so a
// spelling can no longer be routable in the tokenizer's gates and unknown in
// the dispatcher (the asymmetry that made the 07-31 stopgap a two-file edit).
const std::vector<CommandSpec>& commandRegistry() {
  // NOT `const`, because BUG-450's plain-varlist opt-in below marks rows once
  // after construction. The return type still hands callers a const reference,
  // so the table is immutable everywhere outside this function.
  static std::vector<CommandSpec> kRegistry = {
      // ---- BETA campaign C4 (this batch's task brief): `tempvar` is
      // handled BEFORE the 809-2203 routing table this file otherwise
      // transcribes (dispatcher.cpp's pre-tokenizer "U4" intercept step,
      // alongside `tempfile`/`tempname` — see dispatcher.cpp's own U4
      // doc comment), so it's structurally outside this table's normal
      // "one row per 809-2203 arm" scope, unlike every other row below.
      // Backfilled anyway per this task's explicit instruction (confirmed
      // via --dump-registry: `tempvar` — added BS-037/C3 — was the one
      // command in the C2/C3 batch's surface with zero registry
      // footprint). `tempfile`/`tempname` (its siblings, same U4
      // mechanism, pre-dating this table's original A0 scouting pass) are
      // deliberately left unregistered, unchanged by this batch — not
      // asked for, and touching that boundary is out of this item's scope. ----
      {"tempvar", {}, Family::Macro, false, false, WeightPolicy::None, false, {}, "runTempvar"},

      // ---- dispatcher.cpp:814 `use` ----
      // BUG-252 (WS-Q): `use` abbreviates in Stata down to one character.
      // Measured 2026-08-07, StataBE -b, marker-judged: `u <file>, clear` and
      // `us <file>, clear` both answer rc 0. PIET rejected both until now.
      // This is the "one-line registry edit" the BUG-105 arc advertises — and
      // it is the edit that detonated dispatcher.cpp's `use` arm (BUG-251),
      // which located the argument tail by searching the raw line for the
      // literal "use". It is safe to make only because that arm no longer
      // re-derives the command word. Shipped together deliberately: an
      // abbreviation is the only input that can reach the repaired path, so
      // landing the fix without it would leave the fix unfalsifiable.
      {"use", {"u", "us"}, Family::IO, false, false, WeightPolicy::None, false, {}, "runUse"},

      // ---- dispatcher.cpp:872 `sysuse` (bundled fatalities sample only) ----
      {"sysuse", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runUse (bundle-resolved)"},

      // ---- dispatcher.cpp:910 `describe`/`desc`/`codebook` — ONE arm, split
      // into two rows: codebook does NOT get the empty-dataset special case
      // describe/desc gets (cmd=="describe"||cmd=="desc" guard at line 911),
      // a real behavioral divergence inside one shared arm. FLAGGED. ----
      // byIllegalInStata=true (A3/BS-096): audit G3, CONFIRMED live —
      // `describe`/`codebook` both answer "may not be combined with by",
      // r(190), in real Stata.
      // `d` added by the BUG-105 abbreviation stopgap (2026-07-31): an
      // abbrev spelling on an EXISTING row, never a new row — row count
      // stays 119, so the GATES.md "registry-row coupled delta" (freeze
      // re-pin / floors.json rows / pangram tier-1) is not triggered.
      // Measured before landing: zero lines in either freeze corpus
      // (debug_all.do, freeze_extra.do) or pangram.do tokenize to `d`, so
      // no frozen fixture can re-point onto the widened spelling set.
      {"describe", {"desc", "d", "de", "des", "descr", "descri", "describ"}, Family::Meta, false, false, WeightPolicy::None, false, {}, "runDescribe",
       true},
      {"codebook", {}, Family::Meta, false, false, WeightPolicy::None, false, {}, "runDescribe (no empty-data special case)",
       true},

      // ---- dispatcher.cpp:927 `summarize`/`sum` — the ONE explicit
      // unknown-option net in 809-2203. ----
      // BS-090 (BETA campaign, Phase G): allowInRange flipped true — real
      // `in`-range support via piet::handlers::applyIfAndInFilter's
      // materialize-idx-then-keepif fold (see inrange_filter.h's doc
      // comment for why the naive gen/replace-style `_n` fold doesn't
      // generalize to a plain --keepif call).
      // BS-092 (BETA campaign, Phase G): allowByPrefix flipped true —
      // dispatcher.cpp's `by:`/`bysort:` interception now has a genuine
      // per-group summarize implementation (sort-then-slice-then-
      // summarizeCoreStats-per-group), the one OTHER stem besides
      // gen/replace/egen with real by-group dispatch.
      // P3 weights wave: weight flipped FweightOnly -> AweightFweight — cobra's
      // `eval --summarize` now also honours --aweight (dispatcher.cpp's
      // summarize arm picks --aweight vs --fweight off parsed.weightType;
      // descriptive/summarize.cpp's runSummarize/summarizeCoreStats stay fweight-only,
      // unchanged, for freeze-preservation — a sibling runSummarizeAweight
      // carries the new [aw=] path).
      {"summarize", {"sum", "su", "summ", "summa", "summar", "summari", "summariz"}, Family::DataMgmt, true, true, WeightPolicy::AllWeights, true,
       {"detail", "d", "meanonly"}, "runSummarize / runSummarizeAweight"},

      // ---- dispatcher.cpp:968 `save` ----
      {"save", {"sa", "sav"}, Family::IO, false, false, WeightPolicy::None, false, {}, "runSaveData"},

      // ---- dispatcher.cpp:999 `rename`/`ren` ----
      {"rename", {"ren", "rena", "renam"}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runRename"},

      // ---- dispatcher.cpp:1019 `clonevar` ---- `in` since BUG-1096 (runClonevar).
      {"clonevar", {}, Family::DataMgmt, false, true, WeightPolicy::None, false, {}, "runClonevar"},

      // ---- dispatcher.cpp:1040 `generate`/`gen`/`g` — real by-prefix support. ----
      {"generate", {"gen", "g", "ge", "gene", "gener", "genera", "generat"}, Family::DataMgmt, true, true, WeightPolicy::None, true, {},
       "runGen / runBysortGen(_n,in) / strmutate::runStringGen"},

      // ---- dispatcher.cpp:1088 `replace` — real by-prefix support. ----
      {"replace", {}, Family::DataMgmt, true, true, WeightPolicy::None, true, {},
       "runReplace / runBysortGen(_n,in) / strmutate::runStringReplace"},

      // ---- dispatcher.cpp:1124 `egen` — real by-prefix support (via by() injection). ----
      // BS-090: allowInRange flipped true — runEgen's existing if-prefilter
      // idx column (materialized regardless, for the if-merge-back key) now
      // doubles as the `in a/b` predicate target, no separate idx needed
      // (see the .cpp comment). NOTE: this is orthogonal to allowByPrefix
      // (still true here already) — `bysort: egen ... in ...` is STILL
      // illegal, rejected by the tokenizer's OWN outer-token gate on
      // `bysort`/`by` (never registry rows), unaffected by this flip.
      {"egen", {}, Family::DataMgmt, true, true, WeightPolicy::None, true, {}, "runEgen"},

      // ---- dispatcher.cpp:1139 `regress`/`reg`/`r`/`feols`/`reghdfe` — ONE
      // arm serving two user-facing estimator names.
      // WS-B (extcompat, 2026-08-06): CANONICAL FLIPPED. This row used to
      // read {"reghdfe", {"feols"}} — our own spelling filed as a hidden
      // alias of somebody else's package name. `reghdfe` is Sergio Correia's
      // command name; PIET contains none of his code and only accepts the
      // spelling so a migrating do-file keeps working. The row's canonical is
      // now `feols` (the name PIET answers to on its own) and `reghdfe` is
      // the community-package compatibility ALIAS, tagged in communityNames
      // so the extcompat gate can withdraw it. Both spellings still resolve
      // through findByName() exactly as before, and the command ECHO prints
      // whatever the user typed, so no user-visible output moves. What DOES
      // move is the freeze fixture FILENAME (tools/freeze_matrix.py keys
      // captures by canonical): internal capture reghdfe.json is
      // re-pinned to feols.json in this same change, TOLERANCE.md §4.1.
      // BS-090: allowInRange flipped true for regress/reghdfe — the
      // shared runRegress handler now pre-filters if/in into a materialized
      // scratch hbin and points cobra's feols CLI --data at it instead of
      // forwarding a raw --keepif (see runRegress's comment in estimation/linear_estimation.cpp).
      // P3 weights wave: weight flipped None -> AllWeights — cobra's feols now
      // accepts aw/fw/iw/pw, with --fe too (feols_fit demeans with the
      // weights). The combinations Stata refuses — iweight with --fe, pweight
      // with an iid VCE — come back from cobra itself, not a PIET-side check.
      // runRegress forwards parsed.weightVar/weightType as --weight/--wtype.
      {"regress", {"reg", "regr", "regre", "regres"}, Family::Estimation, true, true, WeightPolicy::AllWeights, false, {},
       "runRegress (forwards parsed)"},
      {"feols", {"reghdfe"}, Family::Estimation, true, true, WeightPolicy::AllWeights, false, {},
       "runRegress (forwards parsed; reghdfe = community-package alias)", false, {"reghdfe"}},

      // ---- dispatcher.cpp `ivregress` (BETA D1, BS-063): 2sls/gmm/liml
      // sub-estimator word is parsed.varlist[0] (own dispatch arm, not a
      // registry-modeled distinction — one row covers all three). ----
      // [aw=]/[pw=] are the engine's weighted 2SLS/GMM (BUG-1283: StataBE 19.5
      // `ivregress 2sls y (x1 = z1) x2 [aw=w]` _b[x1] 1.8001190408021581); liml
      // and fweight/iweight stay refused by name in runIvRegress / the gate.
      {"ivregress", {}, Family::Estimation, true, false, WeightPolicy::AweightPweight, false, {},
       "runIvRegress (forwards parsed, method from varlist[0])"},

      // ---- dispatcher.cpp `ivreghdfe` (BETA D1, BS-063) — same handler as
      // `ivregress 2sls` with allowAbsorb=true; also shared with the
      // `reghdfe y (endog=instr), absorb(F)` paren form (routed from the
      // `reghdfe` arm above, not a separate registry row — that form is
      // still literally the command word `reghdfe`, not `ivreghdfe`). ----
      // [aw=]/[pw=] with absorb(): the engine's weighted within transform, as
      // xtivreg2's fe (BUG-1283: StataBE 19.5 `ivreghdfe y (x1 = z1) x2 [aw=w],
      // absorb(id)` _b[x1] 1.7965772864578362, _se .19795319501932024).
      {"ivreghdfe", {}, Family::Estimation, true, false, WeightPolicy::AweightPweight, false, {},
       "runIvRegress (forwards parsed, allowAbsorb=true)", false, {"ivreghdfe"}},

      // ---- dispatcher.cpp `reg3` (BETA D1, BS-065): `(y1 x1 x2) (y2 x3
      // y1) [, sure]` — multiple parenthesised equation groups parsed
      // directly from varlist by the handler itself. ----
      {"reg3", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runReg3 (forwards parsed)"},
      // ---- dispatcher.cpp `sureg`: sureg.ado's `reg3 <eqns>, sure` (e(cmd)
      // `sureg`), the same handler. ----
      {"sureg", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runReg3 (forwards parsed, stataCmd sureg)"},

      // ---- dispatcher.cpp `boottest` (BETA D1, BS-066): post-estimation
      // wild cluster bootstrap, session.lastModel()-driven like test/
      // lincom/nlcom below — no if/in/weight of its own. ----
      {"boottest", {}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "runBoottest (forwards parsed)", false, {"boottest"}},

      // ---- dispatcher.cpp `tobit` (BETA D2, BS-069): censored-normal MLE,
      // ll()/ul() parsed directly by the handler (bare ll/ul computed from
      // the depvar's own observed min/max). ----
      // BUG-1544: tobit takes aweights, fweights, iweights and pweights (tobit.sthlp;
      // the engine fits them all).
      // TOBit (vs-stata bank fd1f148d `tob , zzabbrprobe` r(301): the replay, not r(199))
      {"tobit", {"tob", "tobi"}, Family::Estimation, true, false, WeightPolicy::AllWeights, false, {},
       "runTobit (forwards parsed)"},

      {"var", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runVar (forwards parsed)"},
      {"vargranger", {}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "runVargranger (re-renders the stored fit)"},
      {"irf", {}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "runIrf (materializes the stored IRF cells)"},
      {"mi", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "runMi (subcommand-multiplexed)"},
      {"xtabond", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runXtabond (forwards parsed)"},
      // [aw=]/[pw=]: the engine's weighted NLS (BUG-1283).
      {"nl", {}, Family::Estimation, true, false, WeightPolicy::AweightPweight, false, {},
       "runNlGmm (raw expression)"},
      {"ml", {}, Family::Estimation, true, true, WeightPolicy::PweightOnly, false, {},
       "runMl (compiled lf lifecycle)"},
      {"gmm", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runNlGmm (raw expression)"},
      {"mixed", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runMixed (forwards parsed)"},

      // svyset carries the design's [pweight=] or [iweight=], so its weight policy must
      // admit them; without that the tokenizer rejects the weight before the handler
      // ever sees it (the handler refuses [aweight=] and [fweight=] itself, r(101)). `svy` is the PREFIX row (intercepted ahead of tokenize).
      {"svyset", {}, Family::Meta, false, false, WeightPolicy::AllWeights, false, {},
       "runSvyset (forwards parsed)"},
      // `svy` itself is deliberately NOT a row: it is a PREFIX, intercepted
      // ahead of tokenize, and this codebase's convention is that prefixes carry
      // no registry row (by, bysort, quietly, capture, bootstrap, jackknife,
      // permute and rolling are all absent too). Registering it would also fail
      // the registry-route drift check, which asserts every registered spelling
      // reaches a dispatcher arm — a bare `svy` correctly does not. The
      // structural consequence (prefixes are invisible to the registry-coverage
      // gate) is the FULL FUNCTIONAL CHECK item filed at the P6 close.

      {"ivprobit", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runIvMl (forwards parsed)"},
      {"ivtobit", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runIvMl (forwards parsed)"},

      // ---- P7 wave (dispatcher.cpp): survival. stset mutates the dataset
      // (it materializes _t0/_t/_d/_st); stcox/streg are estimation. ----
      {"stset", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "runStset (forwards parsed)"},
      {"stcox", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runStcox (forwards parsed)"},
      {"streg", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runStreg (forwards parsed)"},

      // ---- P7 wave (dispatcher.cpp): ordered / multinomial / zero-inflated /
      // fractional MLE. Each carries the ML-family vce set its engine verb
      // actually implements; anything else honest-rejects in the handler. ----
      // OLOgit, OPRobit, MLOGit (vs-stata bank 563ee6e0 `olo`, 11f3d58f `opr`, dd4aca9d
      // `mlog` r(301), the replay; `ol`, `op`, `mlo` are r(199))
      {"ologit", {"olo", "olog", "ologi"}, Family::Estimation, true, false, WeightPolicy::AllWeights, false, {},
       "runOrdered (forwards parsed; fw/iw/pw, aw is Stata's r(101))"},
      {"oprobit", {"opr", "opro", "oprob", "oprobi"}, Family::Estimation, true, false, WeightPolicy::AllWeights, false, {},
       "runOrdered (forwards parsed; fw/iw/pw, aw is Stata's r(101))"},
      {"mlogit", {"mlog", "mlogi"}, Family::Estimation, true, false, WeightPolicy::AllWeights, false, {},
       "runMlogit (forwards parsed; fw/iw/pw, aw is Stata's r(101))"},
      {"zip", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runZeroInflated (forwards parsed)"},
      {"zinb", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runZeroInflated (forwards parsed)"},
      {"fracreg", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runFracreg (forwards parsed)"},

      // ---- dispatcher.cpp `heckman` (BETA D2, BS-070): two-step Heckit
      // only (select() parsed directly by the handler; Stata's default
      // full-MLE form honest-rejects — see runHeckman's header comment). ----
      {"heckman", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runHeckman (forwards parsed)"},

      // ---- dispatcher.cpp `qreg` (BETA D2, BS-071): quantile regression,
      // quantile()/level() parsed directly by the handler. ----
      {"qreg", {}, Family::Estimation, true, false, WeightPolicy::AllWeights, false, {},
       "runQreg (forwards parsed)"},

      // ---- dispatcher.cpp `mediate` (BETA D2, BS-072): Stata 18 causal
      // mediation, three parenthesised blocks parsed directly by the
      // handler (only the continuous-treatment, two-level form is in beta
      // scope — see runMediate's header comment for the engine-parity
      // disclosure). ----
      {"mediate", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runMediate (forwards parsed)"},

      // ---- dispatcher.cpp:1175 `xtset` ----
      {"xtset", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runXtset"},

      // ---- dispatcher.cpp:1195 `tsset` ----
      {"tsset", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "runTsDeclare / runTsRedisplay"},

      // ---- dispatcher.cpp:1217 `clear` (arity-capped to `clear [all]` by
      // kFixedArityCommands, not modeled here) ----
      {"clear", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runClear"},

      // ---- dispatcher.cpp:1223/1229/1242 `set` — ONE row covering all three
      // `set` sub-arms (graphics off/on silent-accept, `set obs N` ->
      // runSetObs, bare set/version/compress -> silent no-op). ----
      {"set", {}, Family::Meta, false, false, WeightPolicy::None, false, {},
       "set_multiplex (runSetObs for `set obs`; silent no-op otherwise)"},

      // ---- dispatcher.cpp:1242 `version`/`compress` — share the silent
      // no-op arm with `set`; distinct command names, own rows. ----
      {"version", {}, Family::Meta, false, false, WeightPolicy::None, false, {}, "noop_accept"},
      {"compress", {}, Family::Meta, false, false, WeightPolicy::None, false, {}, "noop_accept"},

      // ---- dispatcher.cpp:1249 `exit` ----
      // BUG-1462: `e` is exit's minimal abbreviation (StataBE: a bare `e` line
      // ends the do-file, rc 0).
      {"exit", {"e", "ex", "exi"}, Family::Meta, false, false, WeightPolicy::None, false, {}, "exit_inline (engineError code=program_exit)"},

      // ---- dispatcher.cpp:1264 `cd` ----
      {"cd", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runCd"},

      // ---- dispatcher.cpp:1273 `pwd` ----
      {"pwd", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runPwd"},

      // ---- dispatcher.cpp:1278 `log` ----
      {"log", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runLog"},

      // ---- dispatcher.cpp:1284 `which` ----
      {"which", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runWhich"},

      // ---- dispatcher.cpp:1289 `ssc` ----
      {"ssc", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runSsc"},

      // ---- dispatcher.cpp:1295 `confirm` ----
      {"confirm", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runConfirm"},

      // ---- dispatcher.cpp:1301 `mkdir` ----
      {"mkdir", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runMkdir"},

      // ---- dispatcher.cpp `rmdir` -- BUG-180 (P2-PREP): NEW. `mkdir` was
      // registered here from the start but its teardown twin never was, so
      // `rmdir` reached no handler and fell through to the generic
      // unsupported_command fallback -- silently, under `capture`, which is
      // how a benchmark harness spent four days blaming `mkdir` for
      // directories that `rmdir` had failed to remove. ----
      {"rmdir", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runRmdir"},

      // ---- dispatcher.cpp:1307 `erase` -- BETA campaign C4 (BS-045):
      // shares its arm with `rm`, a genuine Stata BUILT-IN synonym
      // (live-probed `which rm` -> "built-in command: rm", not an invented
      // alias) -- own row rather than folded into `abbrev` since `rm` isn't
      // a truncation/abbreviation of "erase" (same split rationale as
      // corr/correlate, eststo/estimates elsewhere in this table). ----
      {"erase", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runErase"},
      {"rm", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runErase (rm is a genuine Stata built-in synonym of erase)"},

      // ---- dispatcher.cpp: `timer` (BETA campaign C4, BS-124) -- NEW:
      // previously allow-listed in kNoDataCommands (correctly) but with NO
      // dispatch arm at all, silently falling through to the generic
      // unsupported_command fallback every time. Session bookkeeping (not
      // filesystem/OS I/O), classed Meta like describe/label/version. ----
      {"timer", {}, Family::Meta, false, false, WeightPolicy::None, false, {}, "runTimer"},

      // ---- dispatcher.cpp:1313/1319/1325 `postfile`/`post`/`postclose` ----
      // BUG-343 — two-word verb registered under the first word (the `export`
      // precedent); the handler routes open|write|read|close. Text mode only:
      // `binary`/`seek`/`set`/`query` are refused by name inside the handler.
      {"file", {}, Family::IO, false, false, WeightPolicy::None, false,
       {"read", "write", "text", "replace", "append"}, "runFileCmd (open|write|read|close)"},
      {"postfile", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runPostfile"},
      {"post", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runPost"},
      {"postclose", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runPostclose"},

      // ---- dispatcher.cpp:1332 `drop` ----
      // BS-090: allowInRange flipped true — runDropIf now composes if/in via
      // piet::handlers::applyIfAndInFilter(complement=true); the
      // string-referencing detour (strmutate::filterRowsIf) folds `in` into
      // its own row-wise mask instead.
      {"drop", {}, Family::DataMgmt, true, true, WeightPolicy::None, true, {},
       "runDrop / runDropIf / strmutate::filterRowsIf"},

      // ---- dispatcher.cpp:1347 `keep` ----
      // BS-090: allowInRange flipped true — same mechanism as `drop` above
      // (runKeepIf, complement=false).
      // BUG-532: allowByPrefix flipped true for both rows — `by/bysort KEYS
      // [(SORT)]: keep if` / `drop if` run per group (dispatcher.cpp's by-prefix
      // branch -> handlers::runByKeepIf); the varlist forms stay r(198) there.
      {"keep", {}, Family::DataMgmt, true, true, WeightPolicy::None, true, {},
       "runKeep / runKeepIf / strmutate::filterRowsIf"},

      // ---- dispatcher.cpp:1360 `sort` ----
      {"sort", {"so", "sor"}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runSort"},

      // ---- dispatcher.cpp:1365 `gsort` ----
      {"gsort", {}, Family::DataMgmt, true, true, WeightPolicy::AllWeights, false, {}, "runGsort"},

      // ---- dispatcher.cpp:1370 `order` ----
      {"order", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runOrder"},

      // ---- dispatcher.cpp:1375 `recode` ---- `in` since BUG-1141: runRecode
      // re-reads the line's qualifiers and hands both to its gen/replace/count.
      {"recode", {}, Family::DataMgmt, false, true, WeightPolicy::None, false, {}, "runRecode"},

      // ---- dispatcher.cpp:1380 `collapse` ----
      // BS-090: allowInRange flipped true — runCollapse now pre-filters
      // if/in into a materialized scratch hbin (piet::handlers::
      // applyIfAndInFilter) and points cobra's collapse CLI --data at it
      // instead of forwarding a raw --keepif; the pre-comma raw-text strip
      // that protects the stat-varlist parse from a leaked `if` clause now
      // strips a leaked `in` clause the same way (see the .cpp comment).
      // collapse `[aw fw iw pw]` (w2au_collapse_family.log cz_c_w_*).
      {"collapse", {}, Family::DataMgmt, true, true, WeightPolicy::AllWeights, false, {}, "runCollapse"},

      // ---- dispatcher.cpp:1389 `encode` ----
      {"encode", {}, Family::DataMgmt, true, false, WeightPolicy::None, false, {}, "runEncode"},

      // ---- BETA B-data BS-003/D4: `destring`/`tostring` — PIET-native
      // string(factor)<->numeric conversion (no cobra verb). Real Stata has
      // neither if/in/weight/by on these two (plain variable-management
      // commands); no registry row existed before this phase (total
      // absence — audit-confirmed). ----
      {"destring", {}, Family::DataMgmt, false, false, WeightPolicy::None, false,
       {"replace", "gen", "generate", "ignore", "force", "float", "dpcomma", "percent"},
       "runDestring"},
      {"tostring", {}, Family::DataMgmt, false, false, WeightPolicy::None, false,
       {"replace", "gen", "generate", "usedisplayformat", "format", "force"}, "runTostring"},

      // ---- dispatcher.cpp:1411 `reshape` ----
      {"reshape", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runReshape"},

      // ---- dispatcher.cpp:1416 `append` ----
      {"append", {"ap", "app", "appe", "appen"}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runAppend"},

      // ---- dispatcher.cpp:1433 `merge` ----
      {"merge", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runMerge"},

      // ---- BETA B-data BS-004/D5: `joinby` — PIET-native inner key-join
      // (no cobra verb; audit-confirmed zero implementation at both
      // layers before this phase, the highest-severity total-absence
      // finding). Bare form only: real Stata's own syntax has no if/in/by/
      // weight, and this build implements none of joinby's OWN options
      // either (unmatched()/update/updatemiss/_merge()/nolabel all honest-
      // reject via the empty knownOptions net — D5, post-beta WP-18). ----
      {"joinby", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runJoinby"},
      // P6 wave — frames. `frame` carries its own sub-command/prefix grammar
      // (dispatcher.cpp), so one registry row covers the family.
      {"frame", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runFramePut"},
      {"frlink", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runFrlink"},
      {"frget", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runFrget"},
      // `isid VARLIST` — uniqueness assertion (P1 wave). No qualifiers: Stata's
      // isid asserts over the WHOLE dataset, and no options in this build.
      {"isid", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runIsid"},
      // `assert EXP [if] [in]` — Stata's claim-verifier (P4 wave). Takes if/in
      // (both narrow the rows the claim is checked over) and no options.
      {"assert", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {}, "runAssert"},
      // P1 wave data verbs (engine-backed). `expand` takes a constant count in
      // this build, and `in` since BUG-1115 (runExpandIf already filtered on it);
      // the others take a varlist / `using FILE`.
      {"expand", {}, Family::DataMgmt, false, true, WeightPolicy::None, false, {}, "runExpand"},
      {"fillin", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runFillin"},
      {"cross", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runCross"},
      {"contract", {}, Family::DataMgmt, true, true, WeightPolicy::FweightOnly, false, {}, "runContract"},
      // `export delimited|excel` / `import delimited|excel` — two-word verbs;
      // the dispatcher routes on the sub-verb. Registered under the first word
      // so the command table recognizes the line at all.
      {"export", {}, Family::IO, true, true, WeightPolicy::None, false,
       {"replace", "delimiter", "nolabel", "firstrow", "sheet", "sheetreplace"},
       "runExportDelimited/runExportExcel"},
      {"import", {}, Family::IO, false, false, WeightPolicy::None, false,
       {"clear", "delimiter", "firstrow", "case", "sheet", "cellrange", "allstring", "describe",
        "varnames", "rowrange", "colrange", "numericcols", "encoding"},
       "runImportDelimited/runImportExcel"},

      // ---- dispatcher.cpp:1488 `duplicates` ----
      // BS-090: allowInRange flipped true — report/tag/list now honor `in`
      // (composed with `if` via AND, same shape as their pre-existing `if`
      // support); `drop` honestly rejects `in` for the identical reason it
      // already rejects `if` (see runDuplicatesDrop's .cpp comment — one
      // registry row covers all four sub-verbs, so the flag governs the
      // TOKENIZER gate only; the per-sub-verb legality split lives in the
      // handlers, same as `if` already does).
      {"duplicates", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {}, "runDuplicates"},

      // ---- dispatcher.cpp:1505 `count` ----
      // BS-090: allowInRange flipped true — runCount composes if/in via
      // piet::handlers::applyIfAndInFilter; the string-referencing detour
      // (strmutate::countRowsIf) folds `in` into its own row-wise mask.
      {"count", {"cou", "coun"}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "runCount / strmutate::countRowsIf"},

      // ---- dispatcher.cpp:1526 `display`/`di` ----
      {"display", {"di", "dis", "disp", "displ", "displa"}, Family::Meta, false, false, WeightPolicy::None, false, {}, "handleDisplay"},

      // ---- dispatcher.cpp:1535 `list`/`browse`/`br` — ONE arm; list and
      // browse are DISTINCT commands in real Stata (results-window print vs
      // Data Browser) both mapped identically onto runList here — a
      // deliberate headless-engine simplification, not a bug, but FLAGGED
      // since browse gets NO independent behavior of its own. `br` is a
      // genuine abbreviation of browse. ----
      {"list", {"l", "li", "lis"}, Family::DataMgmt, true, true, WeightPolicy::None, false, {}, "runList"},
      {"browse", {"br", "bro", "brow", "brows"}, Family::DataMgmt, true, true, WeightPolicy::None, false, {}, "runList (identical to `list`)"},

      // ---- BETA B-data BS-005/D6: `format` — a display-layer session
      // attribute (session.varMeta[...].format) honored by list/browse
      // (Datastore::browse()'s conditional "format" column-meta key); NOT
      // persisted into hbin (post-beta P1/hbin-v2). Real Stata's `format`
      // has no if/in/by/weight and no options of its own beyond the
      // trailing %fmt token. ----
      {"format", {}, Family::Meta, false, true, WeightPolicy::None, false, {}, "runFormat"},

      // ---- dispatcher.cpp:1542 `scalar list`/`scalar dir` — this row ONLY
      // covers that display sub-arm. Plain `scalar NAME = value` DEFINITIONS
      // never reach here at all — they are captured upstream at step 0a
      // (tryCaptureDefinition, dispatcher.cpp:554), outside 809-2203
      // entirely. FLAGGED: this registry has no row for the definition
      // form since it isn't part of the routing table this phase transcribes. ----
      // AER-SCOPE1 bundle A (2026-09-03): five abbreviations, each sourced the way this
      // table requires -- the AER corpus EXECUTES the spelling, in the package counts
      // recorded in internal fixture gaps.csv (tw 14, gr 10, sca 2, eret 2, levels 2).
      // Measured, not inferred: Stata's accepted set is non-contiguous, so a plausible
      // prefix is not evidence and none was added on that basis. `levels` is not a
      // truncation at all -- it is levelsof's own pre-Stata-11 name.
      {"scalar", {"sca"}, Family::Macro, false, false, WeightPolicy::None, false, {},
       "runScalarList (list/dir sub-arm only; `scalar x=..` handled pre-809, not modeled here)"},

      // ---- BETA campaign C3 (BS-039): `ereturn list` — bare display-only
      // e() dump. A real 809-2203 routing-table arm (unlike `tempvar`
      // above), landed in the C3 batch alongside BS-038's `scalar list`/
      // `dir` but never got a registry row — confirmed missing via
      // --dump-registry; backfilled this batch (BETA campaign C4) per this
      // task's explicit instruction. W2D-T01: `clear`/`post`/`local` route to
      // runEreturn (results/ereturn_commands.cpp); every other sub-verb is refused by
      // name there. `ereturn scalar NAME = EXP` inside an eclass program is
      // still program.cpp's handleReturnStmt. ----
      {"ereturn", {"eret"}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "runEreturnList (list) + runEreturn (clear|post|local)"},

      // ---- dispatcher.cpp:1549 `matrix`/`mat` ----
      {"matrix", {"mat", "matr", "matri"}, Family::Macro, false, false, WeightPolicy::None, false, {}, "runMatrixDef"},

      // ---- dispatcher.cpp:1555 `label` (variable/var, values, define, list/lst, dir, drop) ----
      {"label", {"la", "lab", "labe"}, Family::Meta, false, false, WeightPolicy::None, false, {},
       "label_multiplex (runLabelVariable/Values/Define/List/Dir/Drop)"},

      // ---- new `labelbook [LBLNAME...]` (P1-W2-0-LABELBOOK-BUG102): direct-
      // read report over session.labelDefs/valueLabels/varLblname — no
      // engine change (the data already arrives via `use`'s --out-meta
      // sidecar, session_state.h:1054-1067; browse already consumes the same
      // map via valueLabelMapForBrowse()). Scope = label set + definition +
      // attached variables, NOT Stata's full diagnostic block (Range/Gaps/
      // Unique/Missing — those need a data scan a direct-read report does
      // not perform; verified against a live StataBE labelbook oracle run).
      // Placed immediately after `label` to mirror dispatcher.cpp arm order
      // (the new arm sits right before `preserve`, same as in this table). ----
      {"labelbook", {}, Family::Meta, false, false, WeightPolicy::None, false, {}, "runLabelbook"},
      // `datasignature` — content fingerprint forwarded to the engine verb of
      // the same name. No qualifiers (it fingerprints the WHOLE dataset), no
      // options in this build.
      {"datasignature", {}, Family::Meta, false, false, WeightPolicy::None, false, {}, "runDatasignature"},

      // ---- dispatcher.cpp:1622 `preserve` ----
      {"preserve", {}, Family::Meta, false, false, WeightPolicy::None, false, {}, "runPreserve"},

      // ---- dispatcher.cpp:1628 `restore` ----
      {"restore", {}, Family::Meta, false, false, WeightPolicy::None, false, {}, "runRestore"},

      // ---- dispatcher.cpp:1634 `levelsof` ----
      // byIllegalInStata=true (A3/BS-096): audit G3, CONFIRMED live —
      // `levelsof may not be combined with by`, r(190).
      // allowInRange: runLevelsofCommand filters if AND in (applyIfAndInFilter).
      {"levelsof", {"levels"}, Family::DataMgmt, true, true, WeightPolicy::None, false, {}, "runLevelsof",
       true},

      // ---- dispatcher.cpp:1653 `winsor2` (community-package name; no abbrev) ----
      {"winsor2", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runWinsor2",
       false, {"winsor2"}},

      // ---- BUG-370 `xtile` / `ipolate` / `tssmooth` — the 08-31 wiring audit found
      // the ENGINE half of all three already shipping (`cobra eval --xtile/
      // --ipolate/--tsma`, each with a Stata-parity unit test) and no command table
      // row pointing at it, so no user surface existed anywhere. allowIf=true means
      // "the arm receives it", not "it is honoured": xtile and ipolate both
      // honest-reject `if` in the handler, because with a restricted sample Stata
      // derives the cutpoints / interpolation anchors from that sample and this
      // engine would derive them from every row — a wrong number, not a slow one.
      // `tssmooth` is registered under its FIRST WORD; the arm sub-dispatches on
      // `ma` (duplicates/estat convention). ----
      // BUG-1015: xtile takes `if`, `in` and its weights -- runXtileStata
      // evaluates exp over the marked sample (w2as sx_xtile_n_if, sx_xtile_n_aw).
      // The weights are xtile's "aweight fweight pweight": the
      // qualifier wave (f8413c27) served [pw=] as well, and the integration of the
      // two branches keeps it; [iw=] is Stata's r(101), refused first by the
      // registry's kStataQualifiers row, so AllWeights here admits exactly aw/fw/pw.
      {"xtile", {}, Family::DataMgmt, true, true, WeightPolicy::AllWeights, false, {},
       "runXtileStata (nquantiles/cutpoints; if/in, aw/fw/pw)"},
      {"ipolate", {}, Family::DataMgmt, true, false, WeightPolicy::None, true, {},
       "runIpolate (generate()+epolate+by(); by: prefix -> by(); if honest-rejected)"},
      {"tssmooth", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "runTssmoothMa (ma only; window(#l[ #c[ #f]]))"},

      // ---- BUG-371 — the four commands the leg-①D idiom probe named as absent and
      // that need no new statistics: `decode` (encode's missing inverse — a user
      // who encoded a string could not get it back), `inspect` (numeric
      // composition of a column), `ds` and `unab` (varlist introspection, the
      // bread and butter of macro-driven do-files). ds/unab reuse the SAME
      // wildcard expander `foreach ... of varlist` uses (piet::loops::
      // expandVarlist), so `x*` cannot mean two different things. ----
      {"decode", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runDecode"},
      {"inspect", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {}, "runInspect"},
      {"ds", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "dispatcher arm (sets r(varlist))"},
      {"unab", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "dispatcher arm (sets a local)"},
      // Package-007 unblock (T7). StataCorp's own ado, so no community tag.
      // byIllegalInStata=true: `bysort g: lookfor x` is r(190) in StataBE
      // (T7 L1 probe), not the r(199) a by-prefix on an unknown name gives.
      {"lookfor", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "runLookfor (describes matches, sets r(varlist))", true},
      {"xtsum", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {}, "runXtsum"},
      {"xttab", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {}, "runXttab"},

      // ---- dispatcher.cpp:1697 `tabulate`/`tab`/`tab1`/`tab2` — tab1/tab2
      // are real Stata's distinct one-way/two-way BATCH tabulation commands
      // (different multi-var semantics than plain tabulate in real Stata);
      // here all three route through the identical runTabulate. Split into
      // 3 rows (tab1/tab2 own rows, not abbrev of tabulate). FLAGGED. ----
      // BS-090: allowInRange flipped true for tabulate/tab1/tab2 —
      // dispatcher.cpp's own if-prefilter block (runTabulate has no
      // ifExpr/inRange param of its own, being pure Datastore computation)
      // now composes if/in via piet::handlers::applyIfAndInFilter.
      // tabulate/tab1 serve [fw=], [aw=] and [iw=] (tabulate.cpp TabWeight;
      // w2aj_qualifiers qf_tb_aw_sp, qf_tb_iw_sp). AllWeights is the policy
      // that admits all three; the registry's kStataQualifiers row answers
      // Stata's r(101) for the pweight it refuses, before this policy is read.
      {"tabulate", {"tab", "ta", "tabu", "tabul", "tabula", "tabulat"}, Family::DataMgmt, true, true, WeightPolicy::AllWeights, false, {}, "runTabulate"},
      {"tab1", {}, Family::DataMgmt, true, true, WeightPolicy::AllWeights, false, {},
       "runTabulate (real Stata tab1 batches per-var; not modeled distinctly here)"},
      {"tab2", {}, Family::DataMgmt, true, true, WeightPolicy::FweightOnly, false, {},
       "runTabulate (real Stata tab2 batches pairwise; not modeled distinctly here)"},

      // ---- dispatcher.cpp:1745 `table` ----
      // byIllegalInStata=true (A3/BS-096): audit G3, CONFIRMED live —
      // `prefix by: not allowed since Stata 17; see help table for updated
      // syntax`, r(190).
      {"table", {}, Family::DataMgmt, true, true, WeightPolicy::AllWeights, false, {}, "runTable",
       true},

      // ---- dispatcher.cpp:1796 `tabstat` (M4a item2 "estimation surface") ----
      // BUG-1014: [aw=]/[fw=] flipped on -- tabstat.ado takes `[aw fw]` and hands
      // them to summarize per group (w2as sx_tabstat_aw, sx_tabstat_fw).
      {"tabstat", {}, Family::Estimation, true, true, WeightPolicy::AweightFweight, false, {}, "runTabstat"},

      // ---- dispatcher.cpp:1810 `corr`/`pwcorr`/`correlate` — ONE arm;
      // `correlate` is a genuine Stata alias of `corr`; `pwcorr` is a
      // DISTINCT real Stata command (pairwise + significance) forced onto
      // the same handler with pairwise=true. Split: corr(+correlate abbrev),
      // pwcorr own row. FLAGGED. ----
      // BS-090: allowInRange flipped true for corr/pwcorr/ttest — all three
      // route through estimation/estimation_support.cpp's shared applyIfFilterM4 (BUG-042's
      // if-prefilter, extended this phase to also accept inRange and
      // delegate to piet::handlers::applyIfAndInFilter). tabstat, sharing
      // the SAME helper, is deliberately left untouched (its call site
      // still passes no inRange argument) — not asked for by this phase.
      // BUG-1014: [aw=]/[fw=] flipped on for both -- correlate takes them
      // (correlate.sthlp), pwcorr.ado's syntax line is `[aw fw]` (w2as
      // sx_corr_aw, sx_corr_cov_aw, sx_corr_covariance_aw, sx_corr_fw).
      {"corr", {"correlate", "cor", "corre", "correl", "correla", "correlat"}, Family::Estimation, true, true, WeightPolicy::AweightFweight, false, {}, "runCorr"},
      {"pwcorr", {}, Family::Estimation, true, true, WeightPolicy::AweightFweight, false, {},
       "runCorr (pairwise forced true)"},

      // ---- dispatcher.cpp:1821 `ttest` ----
      {"ttest", {}, Family::Estimation, true, true, WeightPolicy::None, false, {}, "runTtest (forwards parsed)"},

      // ---- `mean`/`proportion` (P3 weights wave): NEW rows — cobra's `mean`
      // and `proportion` verbs, both real Stata eclass commands (e(b)/_b[...]
      // access after the fact, same as regress). Both are probability-weight
      // ONLY in real Stata (aw/fw/iw are all illegal on mean/proportion —
      // live-verified), so PweightOnly, not AllWeights. No by-prefix support
      // (not implemented here, same boundary as ttest/tabstat above); if/in
      // real (same applyIfAndInFilter pre-materialize path runRegress uses).
      // Dispatcher arm added right after ttest's, mirroring this row order.
      // mean's help: fweights, aweights, iweights and pweights are allowed;
      // the engine serves pw and aw and refuses fw/iw by name.
      {"mean", {}, Family::Estimation, true, true, WeightPolicy::AllWeights, false, {}, "runMean (forwards parsed)"},
      // BUG-1541: proportion takes fweights and iweights too (the engine fits
      // them); its aweight is Stata's r(101), the registry's kStataQualifiers row.
      {"proportion", {}, Family::Estimation, true, true, WeightPolicy::AllWeights, false, {},
       "runProportion (forwards parsed)"},

      // ---- dispatcher.cpp:1826 `xtreg` ----
      // FU-29: AllWeights, although Stata's xtreg takes no iweight. xtreg,fe
      // declares `[aw fw pw]` and xtreg,re declares no weight at all, so the
      // legal set depends on the MODEL, which the tokenizer gate cannot see. The
      // two refusals live in runXtreg with Stata's own text and rc
      // (w2_pweight.log X11_iw_fe r(101) "iweights not allowed", X12_pw_re
      // r(101) "weights not allowed"). No enumerator "aw+fw+pw" exists, and a
      // new one would need a hint string beside the freeze-pinned ones.
      {"xtreg", {}, Family::Estimation, true, false, WeightPolicy::AllWeights, false, {},
       "runXtreg (forwards parsed)"},

      // ---- dispatcher.cpp:1831 `logit`/`logistic`/`probit` — ONE arm, THREE
      // rows. `probit` is a distinct estimator (cobraCmd="probit").
      //
      // BUG-105 (WS-K, 2026-08-07): `logistic` was listed as an ABBREV of
      // `logit` and this comment claimed it was "a genuine Stata synonym".
      // Both were wrong, and the code in this very repo already disagreed:
      // BS-022 gave `logistic` its own odds-ratio display transform in the
      // dispatcher arm (`cmd == "logistic"` -> exp(b), delta-method SE,
      // exp(ci)), pinned by selftest's testC1BS022LogisticOddsRatio and by
      // pangram S05.logistic_rc. It is a DIFFERENT Stata command that shares
      // one arm — exactly the case registry.h's header says gets its OWN row
      // with the same handlerTag (as logit/probit already do).
      //
      // Leaving it in `abbrev` was not merely descriptive drift: once the
      // routed token is normalised through resolveCanonical(), `logistic`
      // would have folded to `logit` and the odds-ratio branch would have gone
      // silently dead — a wrong-number regression behind a right-looking
      // command. Same fields as logit (same handler, same weight policy); the
      // spelling stays routable and keeps its tokenizer qualifier gates. ----
      // W:P5: PweightOnly, not AllWeights. The engine's MLE lane implements
      // the pweight sandwich (Σ wᵢsᵢ score meat) — which is also the only
      // weight kind whose variance semantics Stata defines for these
      // estimators without a further replicate/frequency argument. fw/aw/iw
      // keep their honest reject rather than being silently treated as pw,
      // which would report the wrong standard errors under a right-looking
      // command. Same reasoning for the count models below.
      // FU-585 (margins-corpus-shapes): logit takes every weight past this gate;
      // runLogitProbit answers pweight and fweight (099 tables_share_remote.do:58
      // `logit ... [fweight=w], vce(cl region)`, served under vce(cluster)) and
      // refuses the rest by name (aweight is Stata's own r(101)).
      {"logit", {"logi"}, Family::Estimation, true, false, WeightPolicy::AllWeights, false, {},
       "runLogitProbit(\"logit\") (forwards parsed)"},
      // BUG-1286: logistic and probit take fweights and iweights too (the engine
      // fits them); runLogitProbit answers aweight's r(101) and refuses the
      // cluster forms the engine does not fit, by name.
      {"logistic", {}, Family::Estimation, true, false, WeightPolicy::AllWeights, false, {},
       "runLogitProbit(\"logit\") + BS-022 odds-ratio display transform"},
      {"probit", {"prob", "probi"}, Family::Estimation, true, false, WeightPolicy::AllWeights, false, {},
       "runLogitProbit(\"probit\") (forwards parsed)"},
      // cloglog (BUG-1169/1170): cobra cloglog, the complementary log-log link on
      // the binary ML path (screen, VCEs, fw/iw/pw as probit has them).
      {"cloglog", {}, Family::Estimation, true, false, WeightPolicy::AllWeights, false, {},
       "runLogitProbit(\"cloglog\") (forwards parsed)"},

      // ---- dispatcher.cpp `poisson`/`ppmlhdfe`/`fepois` (BETA D3, BS-073):
      // count-model wiring -> cobra fepois. Bare `poisson` omits --fe (D0
      // finding: fepois-without-fe reproduces Stata's poisson exactly);
      // `ppmlhdfe` and cobra's own `fepois` command name both allow
      // absorb() -> --fe (real Stata's `poisson` itself has no absorb()
      // option). Three rows, one handler, mirroring the regress/feols/
      // reghdfe precedent above. ----
      // BUG-1538/1539: poisson takes fweights, iweights and pweights, ppmlhdfe
      // fweights and pweights (the engine's fepois fits them all); the rest are
      // Stata's own r(101), answered by the registry's kStataQualifiers rows.
      {"poisson", {}, Family::Estimation, true, false, WeightPolicy::AllWeights, false, {},
       "runPoisson (forwards parsed; absorb() honest-rejects here)"},
      {"ppmlhdfe", {}, Family::Estimation, true, false, WeightPolicy::AllWeights, false, {},
       "runPoisson (forwards parsed; absorb() -> --fe)", false, {"ppmlhdfe"}},
      {"fepois", {}, Family::Estimation, true, false, WeightPolicy::PweightOnly, false, {},
       "runPoisson (forwards parsed; absorb() -> --fe; engine-name convenience spelling)"},

      // ---- dispatcher.cpp `nbreg` (BETA D3, BS-074): negative-binomial
      // (NB2) regression -> cobra nbreg. ENGINE FINDING: cobra's nbreg has a
      // live-verified convergence gap for fits whose intercept sits away
      // from zero (see runNbreg's header comment, estimation/count_estimation.cpp) —
      // wiring is complete and correct; the gap is engine-side, out of this
      // batch's edit scope. ----
      // BUG-1537: fweights, iweights and pweights (the engine fits them); the
      // aweight is Stata's r(101), the registry's kStataQualifiers row.
      {"nbreg", {}, Family::Estimation, true, false, WeightPolicy::AllWeights, false, {},
       "runNbreg (forwards parsed)"},

      // ---- dispatcher.cpp `xtpoisson`/`xtlogit` (BETA D3, BS-075):
      // conditional fixed-effects MLE, `,fe` ONLY (the engine has no RE/PA
      // form of either — Stata's own default for both IS random-effects, so
      // the bare/`,re`/`,pa` forms honest-reject naming `,fe` as the only
      // supported form). Distinct real Stata commands, own rows. ----
      // xtpois: xtpois.ado is `xtpoisson `0'' (vs-stata bank 2126d6c2 r(301), the replay)
      {"xtpoisson", {"xtpois"}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runXtPoisson (forwards parsed; ,fe only)"},
      {"xtlogit", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runXtLogitFe (forwards parsed; ,fe only)"},

      // ---- dispatcher.cpp:1837 `csdid`/`drdid` — TWO distinct DID
      // estimators (Callaway-Sant'Anna vs the classic 2x2 doubly-robust
      // DiD) sharing the SAME underlying engine csdid machinery but now
      // (BETA D3, BS-076) via SEPARATE handlers: drdid synthesizes its own
      // gvar column from treatment()/time() rather than requiring the user
      // to supply gvar() directly (real drdid syntax has no gvar() option
      // at all). ----
      {"csdid", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runCsdid (forwards parsed)", false, {"csdid"}},
      {"drdid", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runDrdid (forwards parsed)", false, {"drdid"}},

      // ---- Arc F (P3 wave) — the seven QE/RD commands (estimation/regression_discontinuity.cpp,
      // estimation/eventstudyinteract.cpp, estimation/did_imputation.cpp,
      // estimation/synthetic_did.cpp, graph/binscatter.cpp).
      // allowIf is true for all seven (each forwards parsed.ifExpr as
      // --keepif). binscatter also takes `in` (forwarded as --inrange) and
      // weights: binscatter declares [aweight fweight]; the row admits
      // every type so that runBinscatter answers a pweight/iweight with
      // Stata's own r(101) "pweights not allowed" (w2k_binscatter.log BS_pw)
      // instead of the generic weight refusal. `rdplot` and `binscatter` are
      // classed Graph — their real deliverable is the plot (a sibling arc's
      // GRAPH half); rdplot additionally prints a table. ----
      {"rdrobust", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runRdrobust (forwards parsed)", false, {"rdrobust"}},
      {"rddensity", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runRddensity (forwards parsed)", false, {"rddensity"}},
      {"rdplot", {}, Family::Graph, true, false, WeightPolicy::None, false, {},
       "runRdplot (forwards parsed)", false, {"rdplot"}},
      {"binscatter", {}, Family::Graph, true, true, WeightPolicy::AllWeights, false, {},
       "runBinscatter (forwards parsed)", false, {"binscatter"}},
      // [aw=]/[pw=]: the engine's weighted IW estimator (BUG-1283).
      // AllWeights: the arm answers [iw] with Stata's r(101) and refuses [fw] itself.
      {"eventstudyinteract", {}, Family::Estimation, true, false, WeightPolicy::AllWeights, false, {},
       "runEventstudyinteract (forwards parsed)", false, {"eventstudyinteract"}},
      {"did_imputation", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runDidImputation (forwards parsed)", false, {"did_imputation"}},
      {"sdid", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runSdid (forwards parsed)", false, {"sdid"}},

      // ---- dispatcher.cpp `newey`/`prais`/`dfuller`/`pperron`/`wntestq`
      // (BETA D4, BS-077/078/079/080): time-series estimators/diagnostics,
      // all reading session.timevar (from a prior `tsset`) except wntestq
      // (no --timevar on its own CLI at all). ----
      {"newey", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runNewey (forwards parsed; lag() required)"},
      {"prais", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runPrais (forwards parsed)"},
      {"dfuller", {}, Family::Estimation, true, true, WeightPolicy::None, false, {},
       "runDfullerPperron (forwards parsed)"},
      {"pperron", {}, Family::Estimation, true, true, WeightPolicy::None, false, {},
       "runDfullerPperron (forwards parsed; no drift() form)"},
      {"wntestq", {}, Family::Estimation, true, true, WeightPolicy::None, false, {},
       "runWntestq (forwards parsed)"},

      // ---- dispatcher.cpp `tsfill` (BETA D4, BS-081): data-mutation verb
      // (time/time_fill.cpp — alongside gsort/
      // order/recode above); real Stata's tsfill has no if/in of its own. ----
      {"tsfill", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "runTsfill (opts only, no if)"},

      // ---- dispatcher.cpp `sdtest`/`hausman`/`chow` (BETA D5, BS-085/083/
      // 084): sdtest's `= #` one-sample form and mkspline's `stub = var`
      // below both reconstruct-then-split the varlist on `=` in the
      // dispatcher arm itself (same idiom as egen's arm). hausman has no
      // if/in in real Stata (`hausman NAME1 NAME2` operates on stored
      // estimates, not the live dataset directly) — allowIf=false. chow is
      // a PIET-ONLY convenience command (real Stata has no `chow` at all —
      // see runChow's header comment, postestimation/model_comparison.cpp). ----
      {"sdtest", {}, Family::Estimation, true, true, WeightPolicy::None, false, {},
       "runSdtest (by()/= # exactly one required)"},
      {"hausman", {}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "runHausman (eststo/estimates-store fe/re pair)"},
      {"chow", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runChow (PIET convenience wrapper — real Stata has no `chow` command)"},

      // ---- dispatcher.cpp `mkspline` (BETA D5, BS-087 part): data-mutation
      // verb (datamgmt/data_splines.cpp). `cubic` REQUIRED (honest-reject otherwise — see
      // runMkspline's header comment for the engine's linear-vs-cubic gap);
      // `if` is honest-rejected too (forwarded by the dispatcher arm, then
      // rejected inside the handler — see the SAME header comment for the
      // --keepif row-drop finding). ----
      {"mkspline", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "runMkspline (cubic only; if honest-rejected)"},

      // ---- dispatcher.cpp:1842 `test` ----
      {"test", {"te", "tes"}, Family::Estimation, false, false, WeightPolicy::None, false, {}, "runTestCmd"},

      // ---- `testparm` (BUG-367) — `test`'s varlist form, same handler underneath.
      // No abbreviations: Stata does not abbreviate testparm.
      {"testparm", {}, Family::Estimation, false, false, WeightPolicy::None, false, {}, "runTestparm"},

      // ---- dispatcher.cpp:1847 `lincom` ----
      {"lincom", {}, Family::Estimation, false, false, WeightPolicy::None, false, {}, "runLincom"},

      // ---- dispatcher.cpp:1852 `nlcom` ----
      {"nlcom", {}, Family::Estimation, false, false, WeightPolicy::None, false, {}, "runNlcom"},

      // ---- dispatcher.cpp:1857 `margins` ----
      {"margins", {}, Family::Estimation, false, false, WeightPolicy::None, false, {}, "runMargins"},

      // ---- dispatcher.cpp:1862 `predict` ----
      {"predict", {}, Family::Estimation, false, true, WeightPolicy::None, false, {}, "runPredict (lasso cached path supports in; other paths refuse in explicitly)"},

      // ---- dispatcher.cpp:1867 `estat` (sub-command multiplex: hettest/ovtest/vif/...) ----
      {"estat", {}, Family::Estimation, true, false, WeightPolicy::None, false, {}, "runEstat (sub-command multiplex)"},

      // ---- dispatcher.cpp:1879 `eststo`/`est`/`estimates` — ONE arm;
      // `est` IS a genuine Stata abbreviation of `estimates`; `eststo` is a
      // DISTINCT command (stores the CURRENT model directly vs `estimates`'s
      // store/dir/drop/clear sub-multiplex) sharing the outer if-condition.
      // Split: estimates(+est abbrev), eststo own row. FLAGGED. ----
      {"estimates", {"est", "esti", "estim", "estima", "estimat", "estimate"}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "est_multiplex (runEststo/runEstimatesSave/runEstimatesUse/runEststoDir/"
       "runEststoManage via store/save/use/dir/drop/clear)"},
      {"eststo", {}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "runEststo / runEststoDir / runEststoManage", false, {"eststo"}},

      // ---- dispatcher.cpp:1921 `esttab` ----
      {"esttab", {}, Family::Estimation, false, false, WeightPolicy::None, false, {}, "runEsttab",
       false, {"esttab"}},

      // ---- dispatcher.cpp:1926 `coefplot` ----
      {"coefplot", {}, Family::Graph, false, false, WeightPolicy::None, false, {}, "runCoefplot",
       false, {"coefplot"}},

      // ---- dispatcher.cpp:1931 `marginsplot` ----
      {"marginsplot", {}, Family::Graph, false, false, WeightPolicy::None, false, {}, "runMarginsplot"},

      // ---- dispatcher.cpp:1944-2199 the graph JSON surface — ONE big outer
      // `if`, each plot-type keyword its own row (all real distinct Stata
      // commands, no genuine abbrev overlap between them). All explicitly
      // read parsed.ifExpr and explicitly REJECT `in` (dispatcher.cpp
      // ~2052-2057: "in range is not yet supported for graph commands").
      // BETA Phase E (graphics batch) option-surface changes, none of which
      // add a knownOptions entry (this table's knownOptions field is
      // populated only where an explicit reject-net like
      // eval::rejectUnknownOptions exists — none of these arms have one,
      // unrecognised keys are still silently accepted except the ones now
      // explicitly read below):
      //   scatter: `scatter y1 y2 ... x` multi-y now real (BS-050, last
      //     token is x); `by()` now an honest unsupported_option reject
      //     (BS-052, was silently dropped).
      //   histogram/kdensity/line/xtline/tsline: `by()` same honest reject
      //     added to all five (BS-052).
      //   kdensity ALSO: `kernel()` now actually read (BS-049) — default
      //     (unset) is Epanechnikov (Stata's own default, was silently
      //     always Gaussian regardless of the option); `kernel(gaussian)`
      //     is the other supported spelling; any other kernel name is an
      //     honest unsupported_option reject naming the two supported.
      //   twoway: `by()` on the GLOBAL (post-comma) option block is the
      //     same BS-052 honest reject; `||` layer separator now equivalent
      //     to the parenthesised form (BS-053, dispatcher.cpp
      //     expandDoublePipeLayers, pre-processed before this row's own
      //     dispatch); `twoway kdensity`/`twoway histogram` bare-prefixed
      //     spellings now recognised plot-type tokens routing to the
      //     kdensity/histogram rows below (BS-054); `lfit`/`qfit` inside a
      //     parenthesised layer group are now real closed-form OLS fits
      //     (BUG-099, graph/twoway.cpp) instead of being
      //     silently aliased to a raw connect-the-dots `line` layer. ----
      {"scatter", {}, Family::Graph, true, true, WeightPolicy::AllWeights, false, {}, "runScatter"},
      // gfx-histogram-kdensity-family: AllWeights forwards the clause to the
      // handler, which keeps Stata's own legal set and refuses the rest with
      // r(101) (histogram: fweights; kdensity: fweights, aweights, iweights;
      // w2aq_histogram_family hk_r_h_pw/_aw, hk_r_kd_pw).
      {"histogram", {"hist"}, Family::Graph, true, true, WeightPolicy::AllWeights, false, {},
       "runHistogram"},
      {"kdensity", {}, Family::Graph, true, true, WeightPolicy::AllWeights, false, {}, "runKdensity"},
      // serset-twoway WT/IN (2026-10-21): scatter/line/tsline/twoway/graph take
      // `in` and forward every weight clause; the twoway layer renderer keeps
      // each plot type's own legal set (r(101) `iweights not allowed`, w2al
      // tf_r_iw) and `[w=]` is aweight (tf_tw_072, log:305-307).
      {"line", {}, Family::Graph, true, true, WeightPolicy::AllWeights, false, {}, "runGraphLine"},
      {"xtline", {}, Family::Graph, true, true, WeightPolicy::None, false, {}, "runXtline"},
      {"tsline", {}, Family::Graph, true, true, WeightPolicy::AllWeights, false, {}, "runTsline"},
      {"twoway", {"tw"}, Family::Graph, true, true, WeightPolicy::AllWeights, false, {},
       "twoway_dispatch (runTwowayLayers for parenthesised/`||` multi-layer; else plot-type strip, "
       "now incl. kdensity/histogram)"},
      // `graph` itself: bare `graph varlist` degrades to scatter (dispatcher.cpp
      // ~2042-2044); `graph export`/`save`/`combine`/`drop`/`twoway` are real
      // sub-dispatch inside this ONE row — the most-multiplexed row in the
      // table. FLAGGED for operator review (highest internal complexity).
      // BS-056 (BETA Phase E): `graph drop` now processes the FULL namelist
      // (was first-name-only) and honestly rejects an unknown name (was a
      // silent no-op reporting false success); `_all` unchanged. BUG-103:
      // `graph export ..., as(png|pdf)` on an extensionless path no longer
      // mis-resolves its `.svg` fallback sibling under a TMPDIR/workdir
      // containing a dot in a directory component — export-path handling
      // only, not a namelist/option-surface change.
      {"graph", {"gr"}, Family::Graph, true, true, WeightPolicy::AllWeights, false, {},
       "graph_multiplex (export/save/combine/drop/twoway-recurse/else->scatter)"},
      // serset-twoway SS (2026-10-21): the graph-owned serset store (serset.sthlp):
      // `serset`, `serset [set] #`, `serset use [, clear]`, `serset clear`,
      // `serset drop`, `serset dir` (w2al_twoway_family.log:177-188, :5418-5440).
      {"serset", {}, Family::Graph, true, true, WeightPolicy::None, false, {},
       "serset::runSerset (bare/set #/use/clear/drop/dir)"},

      // ---- AER-SCOPE1 bundle C (GTOOLS, 2026-09-03) -----------------------
      // gtools and ftools are drop-in fast replacements for commands PIET
      // already has, so these rows carry the NATIVE row's policy columns
      // verbatim: an alias that accepted a different qualifier set than the
      // command it forwards to would be a second, quieter grammar.
      // `fastreshape` is deliberately ABSENT: its author is not established
      // here, and this table's community names are attributed in NOTICE.txt as
      // a licence obligation. An unregistered name already answers r(199), so
      // waiting costs nothing while guessing costs an attribution.
      {"gcollapse", {}, Family::DataMgmt, true, true, WeightPolicy::AllWeights, false, {},
       "runCollapse via gtools shim", false, {"gcollapse"}},
      {"fcollapse", {}, Family::DataMgmt, true, true, WeightPolicy::AllWeights, false, {},
       "runCollapse via ftools shim", false, {"fcollapse"}},
      {"gegen", {}, Family::DataMgmt, true, true, WeightPolicy::None, true, {},
       "runEgen via gtools shim", false, {"gegen"}},
      {"gduplicates", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "duplicates multiplex via gtools shim", false, {"gduplicates"}},
      // byIllegalInStata stays FALSE here even though levelsof's is true: the
      // by-prefix gate folds this name to `levelsof` before it looks the flag
      // up (dispatcher.cpp's gtoolsCanonicalName call), so this row's own flag
      // is never read. Setting it would only widen the audited four-stem set
      // that selftest pins, with a stem nobody measured against real Stata.
      {"glevelsof", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "runLevelsof via gtools shim", false, {"glevelsof"}},
      {"gisid", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "runIsid via gtools shim", false, {"gisid"}},
      {"greshape", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "runReshape via gtools shim", false, {"greshape"}},
      {"fmerge", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "runMerge via ftools shim", false, {"fmerge"}},
      {"gquantiles", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "runXtile (xtile form) / runGQuantilesPctile (_pctile|pctile forms)", false,
       {"gquantiles"}},
      {"gstats", {}, Family::DataMgmt, true, false, WeightPolicy::None, false, {},
       "gstats multiplex: winsor -> runWinsor2, sum -> runSummarize", false, {"gstats"}},

      // ---- AER-SCOPE1 bundle B (RESULTS, 2026-09-03) ----------------------
      // `return` has never had a row at all: `return scalar|local` inside a
      // program is intercepted before tokenizing (program.cpp), and every other
      // form fell through to the generic gap. Twenty packages write it, most of
      // them `return list`.
      {"return", {}, Family::Macro, false, false, WeightPolicy::None, false, {},
       "runReturnList (list|clear|add) + program::handleReturnStmt (scalar|local)"},
      // `sreturn list|clear` at any level; `sreturn local` in an sclass program
      // is intercepted before tokenizing (program.cpp), as `return local` is.
      {"sreturn", {}, Family::Macro, false, false, WeightPolicy::None, false, {},
       "runSreturnList (list) + clearSreturn (clear) + program::handleReturnStmt (local)"},
      // The matrix<->dataset round trip. Neither direction existed.
      {"svmat", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runSvmat"},
      // ml-getmata T4. `mata` is a raw-line arm (mata_subset.h): `mata: stmt`
      // and the doexec-folded `mata:` ... `end` block run the native M0 subset;
      // everything else, and `mata` without a colon, is refused by name. It
      // reads and writes only the Mata store, so no data is required.
      {"mata", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "mata::runMataLine (raw pre-pass; M0 subset, block folded at preprocess time)"},
      // `getmata` reads the Mata store and writes variables; an empty session is
      // legal (it implies force and creates the dataset, oracle getmata_nodata).
      {"getmata", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runGetmata"},
      {"mkmat", {}, Family::Macro, true, true, WeightPolicy::None, false, {}, "runMkmat"},
      {"matlist", {}, Family::Macro, false, false, WeightPolicy::None, false, {}, "runMatlist"},

      // ---- AER-SCOPE1 bundle A (2026-09-03) -------------------------------
      // `ivreg` is the pre-Stata-11 spelling of 2SLS, not an abbreviation of
      // anything: it has no estimator sub-command word, so it cannot share
      // `ivregress`'s row. Two AER packages still write it.
      {"ivreg", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runIvRegress(\"2sls\") (pre-Stata-11 spelling)"},
      // Cox's `winsor` is NOT winsor2 under another name and must not be
      // aliased onto it: winsor2 clamps at the `_pctile` value (which averages
      // neighbours when N*p is an integer) while winsor.ado replaces the lowest
      // h observations with the (h+1)-th ORDER STATISTIC. At N=100, p=.01 those
      // are different numbers, so an alias would be a silent wrong answer --
      // the one outcome this campaign exists to prevent. Eight packages.
      {"winsor", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "runWinsorCox (order-statistic replacement; NOT winsor2)", false, {"winsor"}},

      // ---- AER-SCOPE1 bundle D (ENV, 2026-09-03) --------------------------
      // The preamble verbs. Every one of these was measured in two or more of
      // the ninety AER replication packages (internal note ONESHOT-AER-SCOPE1-PLAN.md,
      // internal fixture gaps.csv); none carries an abbreviation, because the
      // corpus only ever spells them in full and this table enumerates
      // measured spellings rather than plausible ones (see the note at the top
      // of this file). Every row here is in kNoDataCommands: an environment
      // verb that answered "No dataset is loaded" would fail on the first line
      // of the do-files that need it (BUG-343's class).
      {"shell", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runShell (allowlist translation onto PIET file verbs; spawns no OS process)"},
      {"winexec", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "refused:no_os_process"},
      {"type", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runType"},
      {"dir", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runDir"},
      // `ls` is a genuine Stata built-in synonym of `dir`, not an invented
      // alias -- same split-row shape as erase/rm above.
      {"ls", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runDir (ls = dir)"},
      {"net", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runNet (accept-note: PIET is offline)"},
      {"ado", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runAdo (accept-note: PIET has no ado directory)"},
      {"adopath", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runAdopath (accept-note + session search list for findfile)"},
      {"sysdir", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runSysdir (accept-note + session directory map)"},
      {"findfile", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runFindfile (searches adopath; sets r(fn))"},
      {"sleep", {}, Family::Meta, false, false, WeightPolicy::None, false, {}, "runSleep"},
      {"pause", {}, Family::Meta, false, false, WeightPolicy::None, false, {},
       "accept-note (PIET is non-interactive)"},
      {"cls", {}, Family::Meta, false, false, WeightPolicy::None, false, {},
       "noop_accept (no Results window to clear)"},
      {"gr_edit", {}, Family::Meta, false, false, WeightPolicy::None, false, {},
       "accept-note (graph-editor recording; appearance only)"},
      {"error", {}, Family::Meta, false, false, WeightPolicy::None, false, {},
       "runErrorCode (engineError carrying reason=r(#))"},
      // AER scope-2 B2-3. NAMED DIVERGENCE 1: Stata resolves a .stbcal over the
      // whole adopath (BASE;SITE;.;PERSONAL;PLUS) and `bcal dir` walks it;
      // PIET models no sysdir, so the search directory is the session cwd
      // alone and `personal` is refused by name.
      // NAMED DIVERGENCE 2 (allowInRange=false below): Stata accepts
      // `bcal create NAME in 1/10, from(v)` (BUG-1872); PIET refuses it r(198)
      // "bcal does not support the
      // `in` row-range qualifier". The refusal is honest and is the engine-wide
      // state for `in` (260 of 299 rows), not a bcal-specific gap.
      // NAMED DIVERGENCE 3: `bcal create NAME if <expr>, from(v) generate(nv)`
      // — the `if`+`generate()` COMBINATION — is refused r(198) where Stata is
      // rc 0 and writes the new column with MISSING on the excluded rows
      // (oracle i2_rc/i2_bz2_missing/i3_rc, bcal-20260908/capture.do:451-462;
      // re-measured rc 0 this review). Filed BUG-465. Plain `if` without
      // generate() IS wired, which is why allowIf is true below (measured:
      // `bcal create ifcal if bd > td(15jan2020), from(bd) replace` rc 0,
      // r(min_date_td)=21930). allowIf was false here until the 2026-09-09
      // review: descriptive-only (BUGLOG:1404 — no enforcement site reads it),
      // but `--dump-registry` does, and that export is what
      // internal capture registry-*.json is built from.
      // NAMED DIVERGENCE 4: `bcal create ..., generate(nv)` writes nv as f64
      // where Stata makes it a `long` (oracle i_type = long, i_fmt = %tbi_gen;
      // re-measured 2026-09-09, cap2.log `describe bz3` -> `long %tbg2`). This
      // is the D1 class widened, NOT a bcal-specific gap: every writeColumn
      // call site through utility/utility_support.cpp's writeColumn writes "f64", because this build has
      // no Stata storage types on the gen path. It is NOT in BUGLOG because
      // there is no honest probe surface for it -- PIET's `confirm long
      // variable x` answers rc 0 for an f64 column, so a probe asserting the
      // type would be a false GREEN.
      // (r(fn) used to be a fifth divergence -- `bcal describe`/`bcal dir` post
      // an ABSOLUTE machine path where Stata posts `./NAME.stbcal`. FIXED in
      // the 2026-09-09 review pass; see setDescribeR.)
      {"bcal", {}, Family::DataMgmt, true, false, WeightPolicy::None, false, {}, "runBcal"},
      // gfx-histogram-kdensity-family: served (was refused:stata_internal).
      // twoway__histogram_gen takes fweights, if and in: AllWeights forwards the
      // clause and the handler refuses every type but fweight with r(101)
      // (hk_r_thg_pw); `in` stays unimplemented.
      {"twoway__histogram_gen", {}, Family::Graph, true, true, WeightPolicy::AllWeights, false, {},
       "runTwowayHistogramGen"},
      // `frames` is Stata's plural spelling of `frame`; `reset` is the one
      // sub-command that only exists in the plural form.
      {"frames", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "runFramesReset + rewrite-to-frame"},
      // `tset` is the pre-Stata-7 spelling of tsset: tset.ado is the one line
      // `tsset `0'`, so the dispatcher runs it through the tsset arm (BUG-1163).
      {"tset", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "runTsDeclare / runTsRedisplay (the tsset arm)"},

      // ---- AER-SCOPE1 bundle E (TABLES, 2026-09-04) -----------------------
      // The table pipeline. Every row here is a community package (see
      // communityNameTable below and NOTICE.txt) except `tex`, which is
      // texdoc's line verb rather than a command of its own -- it gets its own
      // row because it reaches the dispatcher as a head, and a head with no row
      // is a gap no matter what opened the file.
      //
      // `estadd`/`estpost`/`texdoc` are sub-verb heads: the row names the head
      // and the arm multiplexes, the same shape as `frames` and `gstats`.
      // Abbreviations are `{}` throughout because the corpus only ever spells
      // these in full -- this table enumerates measured spellings, not
      // plausible ones.
      {"estadd", {}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "runEstadd (local|scalar|matrix|ysumm sub-verbs)", false, {"estadd"}},
      // allowIf: estpost reads the dataset, so `estpost summarize x if y > 0`
      // is a real filter rather than a silently dropped qualifier.
      {"estpost", {}, Family::Estimation, true, true, WeightPolicy::AllWeights, false, {},
       "runEstpost (summarize|tabstat sub-verbs)", false, {"estpost"}},
      {"estout", {}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "runEstout", false, {"estout"}},
      {"outreg2", {}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "runOutreg2 (.txt ledger is the append source of truth)", false, {"outreg2"}},
      {"texsave", {}, Family::IO, true, true, WeightPolicy::None, false, {}, "runTexsave", false, {"texsave"}},
      {"texdoc", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runTexdoc (init|close)", false, {"texdoc"}},
      {"tex", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runTexdocLine (writes into the texdoc file opened by `texdoc init`)"},
      {"tabout", {}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "refused:tabout_grammar", false, {"tabout"}},
      {"frmttable", {}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "runFrmttable (TeX only; the RTF default is refused by name)", false, {"frmttable"}},
      {"regsave", {}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "runRegsave", false, {"regsave"}},
      {"parmest", {}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "runParmest", false, {"parmest"}},
      {"mat2txt", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runMat2txt", false, {"mat2txt"}},
      {"erepost", {}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "runErepost", false, {"erepost"}},

      // ---- AER-SCOPE1 bundle F (RESHAPE, 2026-09-04) ----------------------
      // Shape and sample verbs. None is a community package -- every one is
      // StataCorp's -- so unlike bundles C and E there is no attribution row
      // and NOTICE.txt is untouched.
      //
      // allowIf is FALSE on the column-creating verbs on purpose: `split`,
      // `cumul`, `separate` and `pctile` build a variable that must span every
      // row, so an `if` would silently produce a column defined on a subset.
      // An honest refusal beats a half-filled variable that reads as data.
      {"split", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runSplit"},
      {"xpose", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runXpose"},
      // Metadata only: recast rewrites the declared Stata storage type and does
      // not touch a single cell.
      {"recast", {}, Family::Meta, false, false, WeightPolicy::None, false, {}, "runRecast"},
      {"insobs", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runInsobs"},
      {"stack", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {}, "runStack"},
      {"separate", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "runSeparate"},
      // allowIf: mvencode.ado marks an `if` sample (007's Wave 2 cleaning uses it).
      {"mvencode", {}, Family::DataMgmt, true, false, WeightPolicy::None, false, {},
       "runMvencode"},
      // The three sampling verbs. Philox keyed on session.seed -- `set seed 7`
      // then the same command twice gives the same rows.
      {"sample", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {}, "runSample"},
      {"splitsample", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "runSplitsample"},
      {"bsample", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runBsample"},
      {"drawnorm", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "runDrawnorm (dense Cholesky; non-PD refused by name)"},
      // `input` reaches the dispatcher as a pre-folded raw line (0x1F-joined by
      // captureInputBlocks), the same shape `program ... end` already uses.
      {"input", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runInput (raw pre-pass; block folded at preprocess time)"},
      // `note` is Stata's own documented synonym ([D] notes), not an invented
      // alias -- the same split-row shape as erase/rm and dir/ls.
      {"notes", {"note"}, Family::Meta, false, true, WeightPolicy::None, false, {}, "runNotes"},
      {"char", {"characteristic"}, Family::Meta, false, false, WeightPolicy::None, false, {}, "runChar"},
      {"compare", {}, Family::Meta, true, true, WeightPolicy::None, false, {}, "runCompare"},
      // BUG-1015: cumul.ado takes `[fw aw/] [if] [in]` and is byable(onecall):
      // if/in, both weights and the by: prefix flipped on (w2as sx_cumul_if,
      // sx_cumul_aw_equal, sx_cumul_fw, sx_cumul_by).
      {"cumul", {}, Family::DataMgmt, true, true, WeightPolicy::AweightFweight, true, {}, "runCumul"},
      // pctile/_pctile take [aw=]/[fw=]/[pw=] (the weighted percentile rule in
      // datamgmt/pctile.cpp); the registry's kStataQualifiers row
      // answers Stata's r(101) for iweight before this policy is read.
      {"pctile", {}, Family::DataMgmt, false, false, WeightPolicy::AllWeights, false, {}, "runPctile"},
      {"_pctile", {}, Family::DataMgmt, false, false, WeightPolicy::AllWeights, false, {},
       "runPctileR (r-class form)"},

      // ---- AER-SCOPE1 bundle G (IO-TEXT, 2026-09-04) ----------------------
      // Older text I/O plus putexcel. All StataCorp commands -- no community
      // package here, so NOTICE.txt is untouched.
      //
      // The four refusals at the end carry a row on purpose: a registered name
      // that refuses BY REASON tells the user what is missing, while an
      // unregistered one answers a generic r(199) that reads like a typo.
      {"insheet", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runInsheet -> runImportDelimited + Stata naming"},
      // allowIf/allowInRange TRUE (2026-10-21): runOutsheet now filters the
      // rows it writes (writeDelimitedFile over applyIfAndInFilter; w2ar
      // dx_outs_if log:838).
      {"outsheet", {}, Family::IO, true, true, WeightPolicy::None, false, {},
       "runOutsheet -> writeDelimitedFile (tab default)"},
      // allowIf/allowInRange TRUE: runInfile keeps the observations the qualifier
      // selects (StataBE 19.5, bank 26988b7fdda8f5c6 `infile a b using in.raw if
      // a>1, clear`, 1258423a4c81b569 `in 2/3`).
      {"infile", {}, Family::IO, true, true, WeightPolicy::None, false, {}, "runInfile"},
      // allowInRange TRUE: runInfix keeps the records `in` names (w2ar dx_rb
      // `infix ... using f in 1`, log:2376).
      {"infix", {}, Family::IO, true, true, WeightPolicy::None, false, {}, "runInfix"},
      // allowIf/allowInRange TRUE: runOutfile writes the rows the qualifier
      // selects (w2ar dx_outf_if `outfile s g if year == 2001 using f`, log:4990).
      {"outfile", {}, Family::IO, true, true, WeightPolicy::None, false, {}, "runOutfile"},
      {"saveold", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runSaveold -> runSaveData (format 118 + honest note)"},
      {"putexcel", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runPutexcel (raw pass-through: `=` is split by the tokenizer)"},
      {"filefilter", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runFilefilter (sets r(occurrences))"},
      {"copy", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runCopy"},
      // AER tier-1 (2026-10-28): the ZIP container code the .xlsx reader/writer
      // always carried is now exposed (zip_io.h), so these two stop refusing.
      {"unzipfile", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runUnzipfile (zipio::readEntries)"},
      {"zipfile", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runZipfile (zipio::build, stored)"},
      {"odbc", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "refused:no_odbc_driver"},
      {"spshape2dta", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "refused:no_shapefile_reader"},

      // ---- AER tier-1 (2026-10-28) ----------------------------------------
      // The difficulty-1 bundle of the AER coverage report: commands whose every
      // piece already existed (a handler, a store, an engine verb) and only
      // needed wiring. Plan: internal note ONESHOT-AER-TIER1-PLAN-20261028.md;
      // answers: tests/eval/golden/aer_w2/w2ax_t1_{official,community}.log.
      // `lookfor`, `ihstrans` and the two mata housekeeping lines are the
      // package-007 branch's and are not here.
      //
      // StataCorp's own (ado or built-in): no community tag.
      {"macro", {}, Family::Macro, false, false, WeightPolicy::None, false, {},
       "tryMacroVerbIntercept (drop|define|shift|list|dir; raw pre-pass)"},
      {"xtdescribe", {"xtdes"}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "runXtdescribe (T_i distribution, sets summarize-detail r())"},
      {"mvdecode", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "runMvdecode (values -> .)"},
      {"lrtest", {}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "runLrtest (two stored fits, or one and the current)"},
      {"fvunab", {}, Family::Macro, false, false, WeightPolicy::None, false, {},
       "runFvunab (ts/fv operator prefixes kept)"},
      // vs-stata bank (2026-11-11): the programmer list parsers and the .dta
      // format probe. StataCorp's own, so no community tag.
      {"numlist", {}, Family::Macro, false, false, WeightPolicy::None, false, {},
       "runNumlist (sets r(numlist))"},
      {"tsunab", {}, Family::Macro, false, false, WeightPolicy::None, false, {},
       "runTsunab (fvunab restricted to ts operators; sets a local)"},
      {"fvexpand", {}, Family::Macro, true, true, WeightPolicy::None, false, {},
       "runFvexpand (sets r(varlist) r(fvops) r(tsops))"},
      {"dtaversion", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runDtaversion (sets r(version))"},
      {"misstable", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "runMisstable (summarize; patterns/tree/nested refused)"},
      {"integ", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "runInteg (trapezoid/cubic, sets r(integral))"},
      {"numlabel", {}, Family::Meta, false, false, WeightPolicy::None, false, {},
       "runNumlabel (add|remove)"},
      {"_dots", {}, Family::Meta, false, false, WeightPolicy::None, false, {},
       "runDots (progress display)"},
      {"cf", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runCf (compare with a using file)"},
      {"renpfix", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "runRenpfix (sequential renames, stops at a clash)"},
      {"markout", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "runMarkout"},
      {"postutil", {}, Family::IO, false, false, WeightPolicy::None, false, {},
       "runPostutil (clear|dir)"},
      // Community packages. Each author was read from the package's own `ssc
      // describe` record on the oracle Mac, 2026-10-28
      // (internal capture t1_ssc_install.log); matselrc is not
      // on SSC and came from STB-56 dm79 (t1_matselrc_install.log). zipuse ships
      // inside the zipsave package.
      {"require", {}, Family::Meta, false, false, WeightPolicy::None, false, {},
       "runRequire (accept-note)", false, {"require"}},
      {"labvars", {}, Family::Meta, false, false, WeightPolicy::None, false, {}, "runLabvars",
       false, {"labvars"}},
      {"astile", {}, Family::DataMgmt, true, true, WeightPolicy::None, true, {},
       "runAstile (quantile groups, by())", false, {"astile"}},
      {"matsort", {}, Family::Macro, false, false, WeightPolicy::None, false, {}, "runMatsort",
       false, {"matsort"}},
      {"matselrc", {}, Family::Macro, false, false, WeightPolicy::None, false, {}, "runMatselrc",
       false, {"matselrc"}},
      {"fillmissing", {}, Family::DataMgmt, true, true, WeightPolicy::None, true, {},
       "runFillmissing (with(); by-prefix injected)", false, {"fillmissing"}},
      {"findname", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "runFindname (sets r(varlist))", false, {"findname"}},
      {"sencode", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "runSencode (order of appearance)", false, {"sencode"}},
      {"zipuse", {}, Family::IO, true, true, WeightPolicy::None, false, {}, "runZipuse",
       false, {"zipuse"}},
      {"zipsave", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runZipsave",
       false, {"zipsave"}},

      // ---- AER-SCOPE1 bundle I (UTIL, 2026-09-04) -------------------------
      // Data-tidying utilities. Every community name below was read from the
      // package's own `*!` line or Authors block via `which` on 2026-09-04 --
      // which corrected the plan's draft twice more: `unique` is Michael Hills
      // AND Tony Brady (the ado says only "MH and TB"; the help file has the
      // names), and `renvarlab` is Joseph Canner, not Nicholas Cox. `labmask`
      // has no SSC package of its own; it ships inside `labutil`.
      //
      // DELIBERATELY ABSENT (2026-09-04 reading): `dropmiss` and `renvars` --
      // neither had a package found under its own name here, so authorship
      // looked unconfirmed. CORRECTION, AER-SCOPE2 K1 (2026-09-07): `renvars`
      // DOES name an author in its ado header (`njc`/`njc/jw`); it ships from
      // the Stata Journal, not SSC, which is why this pass missed it -- see
      // its row near the end of this table. `dropmiss` stays absent (see the
      // K1 refusal row below). `matchit`/`reclink` stay out: record linkage is
      // out of scope, and an unregistered name already answers r(199).
      // CORRECTION, AER-SCOPE2 B2 (2026-09-07): `strdist` was in that sentence
      // and is no longer -- it is the last name blocking paper 100, so the
      // scope moved to it and it has its own row further down.
      // `marksample` is a StataCorp built-in, no community tag.
      {"unique", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {}, "runUnique",
       false, {"unique"}},
      // gtools' spelling, landed here rather than in bundle C because `unique`
      // did not exist yet when C translated the other nine gtools names.
      {"gunique", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "runUnique (gtools spelling)", false, {"gunique"}},
      {"distinct", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {}, "runDistinct",
       false, {"distinct"}},
      {"missings", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "runMissings (report|dropvars|dropobs|tag)", false, {"missings"}},
      {"mdesc", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runMdesc",
       false, {"mdesc"}},
      {"labmask", {}, Family::Meta, false, true, WeightPolicy::None, false, {}, "runLabmask",
       false, {"labmask"}},
      {"renvarlab", {}, Family::Meta, false, false, WeightPolicy::None, false, {}, "runRenvarlab",
       false, {"renvarlab"}},
      {"splitvallabels", {}, Family::Meta, true, true, WeightPolicy::None, false, {},
       "runSplitvallabels", false, {"splitvallabels"}},
      // Stata's own, and program-internal: no community tag.
      {"marksample", {}, Family::Macro, false, false, WeightPolicy::None, false, {},
       "runMarksample (inside a program only)"},
      {"filelist", {}, Family::IO, false, false, WeightPolicy::None, false, {}, "runFilelist",
       false, {"filelist"}},
      // allowByPrefix: `bysort id: carryforward y, gen(f)` is the corpus form,
      // and the dispatcher injects the by() the same way it does for egen.
      {"carryforward", {}, Family::DataMgmt, false, false, WeightPolicy::None, true, {},
       "runCarryforward", false, {"carryforward"}},
      // Package-007 unblock (T7). Community ados: ihstrans (SSC, Jan Helmdag) takes
      // if/in; sctorezone (SSC scto) accepts and ignores them in Stata, so the row
      // does not allow them.
      {"ihstrans", {}, Family::DataMgmt, true, true, WeightPolicy::None, false,
       {"keepusing", "prefix", "casewise"}, "runIhstrans", false, {"ihstrans"}},
      {"sctorezone", {}, Family::DataMgmt, false, false, WeightPolicy::None, false,
       {"force", "only", "exclude"}, "runSctorezone", false, {"sctorezone"}},
      // allowIf/allowInRange FALSE, corrected after implementation: rangestat
      // WRITES a column, and a written column must be frame-length and
      // row-aligned. `applyIfAndInFilter` returns a filtered FRAME, not a mask,
      // so there is no row identity to write back through -- the handler
      // refuses both qualifiers in every form, and the row now says so.
      // (`unique`, `distinct` and `missings` keep true: their arms genuinely
      // read the qualifier in at least one form.)
      {"rangestat", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "runRangestat", false, {"rangestat"}},
      {"tsspell", {}, Family::DataMgmt, true, true, WeightPolicy::None, true, {}, "runTsspell",
       false, {"tsspell"}},

      // ---- AER-SCOPE1 bundle J (GRAPH-X, 2026-09-04) ----------------------
      // Community graph/geo verbs. Every author below was read from the
      // package's own `*!` line on 2026-09-04 -- which corrected the plan once
      // more: `grc1leg2` credits Mead Over alone, not "Mead Over, Alan
      // Feiveson".
      //
      // `grc1leg` was DELIBERATELY ABSENT here as of 2026-09-04 (its ado and
      // help file name no author). AER-SCOPE2 K1 (2026-09-07): the author was
      // found instead in the installer's own `stata.trk` record for
      // `grc1leg.pkg` -- see communityNameTable() below -- so it now gets a
      // row, routed to grc1leg2's same handler. Still absent: cibar,
      // binsreg, spmap, maptile, shp2dta, geoinpoly, heatplot, distplot --
      // refused anyway, and an unregistered name already answers r(199).
      {"grc1leg2", {}, Family::Graph, false, false, WeightPolicy::None, false, {},
       "runGrc1leg -> runGraphCombine (one panel keeps its legend)", false, {"grc1leg2"}},
      {"grc1leg", {}, Family::Graph, false, false, WeightPolicy::None, false, {},
       "runGrc1leg -> runGraphCombine (one panel keeps its legend)", false, {"grc1leg"}},
      {"grstyle", {}, Family::Meta, false, false, WeightPolicy::None, false, {},
       "accept-note (scheme styling is not applied in this build)", false, {"grstyle"}},
      {"geodist", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {}, "runGeodist",
       false, {"geodist"}},
      {"geonear", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {}, "runGeonear",
       false, {"geonear"}},
      {"cdfplot", {}, Family::Graph, true, true, WeightPolicy::AllWeights, false, {}, "runCdfplot",
       false, {"cdfplot"}},
      {"event_plot", {}, Family::Graph, false, false, WeightPolicy::None, false, {},
       "runEventPlot -> runCoefplot", false, {"event_plot"}},

      // ---- AER scope 2, bundle K1 (2026-09-07) ----------------------------
      // The three renamers. `move` is StataCorp's own built-in (`which move`
      // rc 0, `ado/base`), so it carries NO community tag; the other two do,
      // and both authors were read from the package's own header on this
      // machine: tolower.ado's first line is "* Nick Cox Statalist Digest 895",
      // and renvars.ado's is "*! 2.3.0 01feb2001 njc/jw STB-60 dm88".
      //
      // `renvars` was DELIBERATELY ABSENT as of 2026-09-04 with the note that
      // it has "no SSC package under its own name". That is true and was the
      // wrong test: it is on the Stata Journal, `net install dm88_1, from(
      // http://www.stata-journal.com/software/sj5-4)` rc 0. The refusal was
      // spent on an unmeasured axis, so measuring it inverts it.
      //
      // It is NOT an alias for `renvarlab` above, which was the plan. renvarlab
      // does prefix/suffix/upper/lower and lists presub/postsub/subst/postfix
      // in its own `unimplemented` set -- and those four are most of what the
      // corpus actually spells, along with the `\ newnamelist` map form
      // renvarlab has no syntax for at all.
      // ---- AER scope 2, bundle B2 (2026-09-07) ----------------------------
      // `strdist` closes the last name blocking paper 100. It was DELIBERATELY
      // absent as of 2026-09-04 with the note that it is "record linkage, out
      // of scope"; the scope moved, and the author question that would have
      // blocked it does not arise -- strdist.ado's own `*!` line reads
      // "version 1.2 09dec2017 Michael D Barker Felix Poge" (o-umlaut in the
      // source; NOTICE.txt carries the same spelling this table does).
      //
      // if/in are BOTH false, and that is a decision rather than an omission:
      // the package marks rows out with a `touse` and writes only into it, so
      // an excluded row keeps a MISSING. This build's shared qualifier helper
      // hands back a filtered FRAME, which would DROP those rows instead --
      // a different dataset at rc 0. Refusing the qualifier is the honest
      // answer until there is a mask; the twelve-paper corpus uses neither.
      {"strdist", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "runStrdist (byte-wise Levenshtein; default new variable is literally `strdist`)",
       false, {"strdist"}},
      // ---- AER scope 2, bundle K1c (2026-09-07) ---------------------------
      // George Vega Yon's `parallel`, answered SEQUENTIALLY in this process.
      // The handler is a dispatcher arm rather than a row in a handlers file,
      // because `parallel do` re-enters the do-file executor and that recursion
      // already lives in dispatcher.cpp (the `do`/`run` intercept).
      //
      // NOTE the collision, which is real and structural: PIET already owns the
      // word `parallel` for its OWN `parallel { ... }` block. That form needs a
      // brace on the line and is recognised in loops.cpp before tokenizing, so
      // the two never meet. This row is the SSC command.
      {"parallel", {}, Family::Meta, false, false, WeightPolicy::None, false, {},
       "dispatcher arm: setclusters/initialize/clean accept-notes, `do` runs sequentially, "
       "the data-splitting and append forms are named refusals",
       false, {"parallel"}},
      {"tolower", {}, Family::Meta, false, false, WeightPolicy::None, false, {},
       "runTolower -> runRename per variable (NOT atomic: stops at the first r(110))",
       false, {"tolower"}},
      {"renvars", {}, Family::Meta, false, false, WeightPolicy::None, false, {},
       "runRenvars -> runRename (atomic: `confirm new var` over the whole plan first)",
       false, {"renvars"}},
      // StataCorp's own: a registry row, no community tag.
      {"move", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "runMove -> runOrder before()/after() by direction"},
      // ── AER-SCOPE1 bundle H (ESTIM, 2026-09-05) ──────────────────────────
      //
      // TWENTY-THREE rows. The plan's count assumed
      // fourteen non-E3 commands; the enumeration is twelve, because `ivreg`
      // already landed with bundle A (runIvRegress("2sls"), above) and needs no
      // second row. Twelve implemented + eleven E3 refusals = 23.
      //
      // E1 -- aliases onto estimators that already exist. areg copies regress's
      // policy exactly because it IS regress with an absorb(); the two ivreg2
      // spellings copy ivregress's (no `in`, no weights) for the same reason.
      {"areg", {}, Family::Estimation, true, true, WeightPolicy::AllWeights, false, {},
       "runAreg -> runRegress(\"areg\")"},
      // ivreg2: [aw=]/[pw=] (ivreg2; the engine's weighted 2SLS,
      // w2d_uncovered.log:1916 ivreg2_aw_ctl). fweight/iweight are not implemented.
      {"ivreg2", {}, Family::Estimation, true, false, WeightPolicy::AweightPweight, false, {},
       "runIvreg2 -> runIvRegress", false, {"ivreg2"}},
      // xtivreg2, fe: [aw=]/[pw=] weight the within transform's group means and then
      // ivreg2 (xtivreg2; w2d_uncovered_xtivreg2.log:2853
      // xtivreg2_fe_pw_savefirst_034). fweight/iweight are not implemented.
      {"xtivreg2", {}, Family::Estimation, true, false, WeightPolicy::AweightPweight, false, {},
       "runIvreg2 -> runIvRegress (fe -> absorb(panelvar))", false, {"xtivreg2"}},

      // E2 PIET-side -- closed-form statistics computed here, no engine call.
      // allowInRange follows pwcorr (the nearest policy anchor): these read a
      // column pair and honour both qualifiers.
      {"spearman", {}, Family::Estimation, true, true, WeightPolicy::None, false, {},
       "runSpearman (rank correlation + 4-parameter beta exact p)"},
      {"sktest", {}, Family::Estimation, true, true, WeightPolicy::AweightFweight, false, {},
       "runSktest (D'Agostino-Belanger-D'Agostino + Royston adjustment)"},
      {"signrank", {}, Family::Estimation, true, true, WeightPolicy::None, false, {},
       "runSignrank (Wilcoxon matched-pairs, normal + exact p)"},
      {"centile", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "runCentile (binomial-interpolated CI)"},

      // E2 engine-backed -- these forward to cobra subcommands. allowInRange is
      // false on tsfilter/pca: each either writes a frame-length column or
      // consumes the whole sample, and `in` would have to mean a filtered FRAME
      // rather than a mask (the same reason rangestat above refuses both).
      // lpoly and lowess now have that restricted-sample form: the engine takes
      // the if/in sample as a MASK over the full frame (`--keepif`, `--inrange`)
      // and writes generate() back by row (w2ae_lpoly_lowess lpl_in, lpl_lw_in).
      // Their weight rows are AllWeights so that the HANDLER answers with
      // Stata's own rc: lpoly takes [aw=] (lpoly's syntax `[fw aw]`; [fw=] is
      // refused by name, unmeasured), [pw=]/[iw=] are r(101) "pweights not
      // allowed" (lpl_pw); lowess.ado declares no weights at all, so every
      // weight is r(101) "weights not allowed" (lpl_lw_aw). The tokenizer's own
      // weight gate would answer r(198) for both.
      // tsfilter hp honours `in` (runTsfilterHp passes the rows to the engine's
      // full-frame sample mask, as lowess/lpoly do).
      {"tsfilter", {}, Family::DataMgmt, true, true, WeightPolicy::None, false, {},
       "runTsfilterHp (subcommand `hp` only; others -> kUnknownSubcommand)"},
      {"lpoly", {}, Family::Graph, true, true, WeightPolicy::AllWeights, false, {},
       "runLpoly (local polynomial smooth + ROT bandwidth)"},
      {"lowess", {}, Family::Graph, true, true, WeightPolicy::AllWeights, false, {},
       "runLowess (tricube LOWESS, no robustness iterations)"},
      {"pca", {}, Family::Estimation, true, true, WeightPolicy::AweightFweight, false, {},
       "runPca (correlation/covariance principal components)"},

      // X2-GLM: `glm` left E3. Fitted in-process (estimation_glm.cpp over the
      // engine's cecon::glm_fit). `if` and `in` are read (runGlm's frame filter;
      // vs-stata bank cc7bc9b0 `glm ... in 1/60, family(poisson)` rc 0).
      // AweightFweight lets [aw=] through (w2_glm_lifecycle pois_aw_cl_*);
      // an [fw=] that gets past the tokenizer is refused by name in runGlm.
      {"glm", {}, Family::Estimation, true, true, WeightPolicy::AweightFweight, false, {},
       "runGlm (in-process, estimation_glm.cpp)"},

      // xi is BOTH a command (`xi i.region`) and a prefix (`xi: regress ...`);
      // the prefix half is its own row in prefixRegistry() below.
      {"xi", {}, Family::DataMgmt, false, false, WeightPolicy::None, false, {},
       "runXi / runXiPrefix (indicator materialization, always materialized)"},

      // E3 -- registered so the name RESOLVES and the refusal can name the
      // nearest native command, rather than falling through to the generic
      // unknown-command path which can say nothing useful. No communityNames on
      // these rows even where the name is community-authored (rlasso,
      // honestdid, ritest, acreg, swindex, qreg2): communityNames drives the
      // extcompat gate and the NOTICE.txt attribution, and attribution is owed
      // for compatibility we SHIP, not for a name we decline. allowIf and
      // allowInRange are false throughout -- a refusal reads no qualifier.
      // suest left E3 (w2j_e3_builtin S_*): postestimation/suest_estimation.cpp runSuest, stored
      // regress fits (mean + lnvar equations, robust/cluster sandwich).
      {"suest", {}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "runSuest (stored regress fits: mean + lnvar scores, stacked sandwich V)"},
      // ---- AER scope 2, bundle B2-4 (2026-09-09): `teffects` leaves E3 -----
      // ONLY the `nnmatch` estimator: Abadie-Imbens nearest-neighbour matching
      // (StataCorp [CAUSAL]), the last statistical blocker of paper 076. Landed
      // after its match-set rule was measured AND the first candidate rule was
      // refuted: internal notes REPORT, REFUTE-0 and
      // REFUTE-1 (14 + 15 live-Stata runs, every gen() id in order).
      // ra/ipw/ipwra/aipw/psmatch are DIFFERENT estimators and stay named
      // refusals in the handler (`refused:teffects_<sub>_unimplemented`).
      // allowIf: forwarded as a --keepif MASK so the gen() ids stay frame `_n`.
      // allowInRange (BUG-468): `in a/b` joins that mask as `_n` bounds
      // (runTeffects); StataBE `in 1/40` rc 0, e(N) 40 (golden/aer_estim2_oracle.log
      // INFO|nn_level|in_rc, in_N). WeightPolicy::None is the un-measured half:
      // Stata documents no weights on teffects nnmatch that this build has captured.
      {"teffects", {}, Family::Estimation, true, true, WeightPolicy::None, false, {},
       "runTeffects (nnmatch only: ATE/ATET, AI robust V, gen(stub) ids; every other "
       "sub-command = refused:teffects_<sub>_unimplemented)"},
      {"lasso", {}, Family::Estimation, true, true, WeightPolicy::None, false, {},
       "runLasso (linear CV/plugin/BIC, Poisson plugin/BIC; cached selected coefficients)"},
      {"lassocoef", {}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "runLassoCoef (current fit; display(x|coef), sort(none|names))"},
      // rlasso left E3 (w2j_e3_rlasso): estimation/lasso_estimation.cpp runRlasso, lassopack's
      // rigorous lasso (rlasso.ado/MakeData/RLasso) with homoskedastic, robust and
      // cluster penalty loadings, fe/noconstant; if/in are its marksample.
      {"rlasso", {}, Family::Estimation, true, true, WeightPolicy::None, false, {},
       "runRlasso (rlasso.ado: rigorous lasso + post-lasso OLS, e(b)/e(beta*)/e(Psi), "
       "homoskedastic/robust/cluster loadings, fe)"},
      // ---- AER scope 2, bundle B2 (2026-09-08): `oaxaca` leaves E3 ----------
      // Ben Jann's Blinder-Oaxaca decomposition, `pooled` twofold ONLY (the
      // corpus form, paper 034). Landed after -- not before -- its inference was
      // pinned: internal note oaxaca-se-20260908/REPORT.md (12 live-Stata
      // fixtures, all 120 SEs and every e(V)/e(V0) entry to 3.8e-14, 48 wrong
      // constructions rejected). The threefold default is a NAMED refusal in
      // the handler. allowIf: the handler forwards the qualifier as --keepif.
      // Weights: none (Stata allows pw/aw/fw here; unverified, so refused).
      {"oaxaca", {}, Family::Estimation, true, false, WeightPolicy::None, false, {},
       "runOaxaca (twofold `pooled` decomposition, robust suest V; threefold default "
       "= refused:oaxaca_threefold_default)",
       false, {"oaxaca"}},
      {"honestdid", {}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "refuse:kNotImplemented (E3-deferred; nearest native: csdid)"},
      // ritest left E3 (w2j_e3_ritest): dispatcher step 0a-ter hands the raw line
      // to estimation/ritest_inference.cpp runRitestLine (its exp_list is per replicate);
      // its own if/in are parsed there, so the row's qualifier gates never see one.
      {"ritest", {}, Family::Estimation, false, false, WeightPolicy::None, false, {},
       "runRitest (ritest.ado: permutation within strata x cluster cells, r(p)/r(c)/r(b), "
       "saving(), seed(); exp_list evaluated per replicate)"},
      // acreg left E3 (w2j_e3_acreg): estimation/acreg_estimation.cpp runAcreg -- OLS/2SLS with
      // the heteroskedastic / temporal / spatial lat-lon kernels, pfe1()/pfe2(); the
      // other kernels refuse by name. if/in are its marksample (ado).
      {"acreg", {}, Family::Estimation, true, true, WeightPolicy::None, false, {},
       "runAcreg (acreg.ado: OLS/2SLS, Conley spatial + temporal HAC, pfe absorption, KP F)"},
      // swindex left E3 (w2j_e3_swindex): datamgmt/swindex.cpp runSwindex, the
      // ado's syntax/refusals and Anderson index; if/in are its marksample.
      {"swindex", {}, Family::Estimation, true, true, WeightPolicy::None, false, {},
       "runSwindex (swindex.ado: inverse-covariance weighted index, r(wt)/r(pw))"},
      // Package-007 lane LB: estimation/leebounds_estimation.cpp runLeebounds, Lee (2009)
      // bounds without tight() (leebounds.ado's leesbound), analytic V; if/in are its
      // sample. AllWeights so a weight reaches the handler, which refuses pw/fw/iw BY
      // NAME (the ado's own weighted path is not implemented) and gives aweights
      // Stata's r(101) (StataBE p4 AW).
      {"leebounds", {}, Family::Estimation, true, true, WeightPolicy::AllWeights, false, {},
       "runLeebounds (leebounds.ado: Lee (2009) bounds, analytic V; tight/cieffect/"
       "vce(bootstrap)/weights refused by name)",
       false, {"leebounds"}},
      // qreg2 left E3 (w2j_e3_qreg2): estimation/qreg2_estimation.cpp runQreg2 fits it natively
      // -- qreg's LP vertex under version 11 (e(f_r) as _qreg measures it), the
      // Machado-Parente-Santos Silva sandwich, the MSS/PSS tests. FweightOnly is
      // qreg2 `[fweight]` (aw/pw/iw r(101) via the tokenizer's
      // kStataQualifiers row); if/in are its marksample.
      {"qreg2", {}, Family::Estimation, true, true, WeightPolicy::FweightOnly, false, {},
       "runQreg2 (qreg2.ado: qreg vertex, kernel sandwich V (robust/cluster), MSS/PSS tests)"},

      // ---- AER-SCOPE2 bundle K1 (RENAME-UTIL, 2026-09-07) ------------------
      // Rename/reposition utilities + one loop wrapper. `tolower`'s author
      // was read fresh today from this machine's own installer records (ado
      // header, cross-checked against `stata.trk` -- see
      // communityNameTable() below). `move` is Stata's OWN built-in
      // (repositions one variable; no `move.ado` under ado/plus confirms it),
      // so it carries no community tag. `renvars` corrects the 2026-09-04
      // bundle-I reading above: its ado header does name an author, it just
      // ships from the Stata Journal rather than SSC.
      // `parallel` (SSC) is a WRAPPER, not an external runtime -- it spawns
      // extra Stata instances to split `do()`/`append` work in pure Stata
      // syntax (2026-09-07 correction; see the
      // classify-wrapper-vs-external-runtime memory note). Not installed
      // here under its own name (only the unrelated `parallel_map.ado`,
      // bundled inside Sergio Correia's `ftools`), and `stata.trk` has no
      // record for it either -- author left blank rather than guessed.
      // `dropmiss` gets NO ROW AT ALL, and that is the decision, not an omission.
      // Its authorship is still unresolved (neither SSC nor the Stata Journal
      // path has it; the successor `missings`, above, is the Cox package Stata
      // users get today), so the attribution rule denies it a row exactly as it
      // did in scope 1. A `refuse:` row was written here first and removed: an
      // unregistered name ALREADY answers r(199), and this file's own bundle-J
      // note settles what to do about that -- "a second, weaker copy of an
      // answer the registry already gives is not honesty, it is duplication"
      // (graph/graph_extension_support.cpp). The two names above it, `grc1leg` and
      // `renvars`, moved because MEASUREMENT moved -- not because the rule did.
  };
  // BUG-450: the plain-varlist opt-in, applied once, by name.
  //
  // MEASURED on StataNow 19.5 BE over auto.dta before any of them was enabled
  // (`t*` -> trunk turn, `price-trunk` -> five names, `t?rn` -> turn, `zz*` ->
  // r(111), `_all` -> everything). Each name below takes a varlist and nothing
  // else in that position.
  //
  // DELIBERATELY ABSENT, each for a reason that would be a silent wrong answer:
  //   · `rename` — `rename t* T*` is rename-GROUPS. Measured: it renames trunk
  //     to Trunk and turn to Turn. Expanding the pattern first destroys the
  //     pairing and would rename both to the same name.
  //   · every estimation row — `i.x`, `c.x##c.z` and `L.x` share this field.
  //   · `egen`/`collapse` — the varlist sits inside a function call or a
  //     `(stat)` group, so the token that needs expanding is not the whole one.
  //   These are not refusals, they are NOT-YET: each needs its own measurement
  //   pass, and BUG-450's entry says so.
  static const std::set<std::string> kPlainVarlist = {
      "drop",     "keep",   "order",    "summarize", "tabstat", "list",
      "codebook", "describe", "format", "destring", "recode",  "tostring",
      "compress", "isid",
      // `labelbook` is absent on purpose: its words are value-label names, not
      // variables (`labelbook *lbl` is r(198) "* invalid name" in Stata, not a
      // variable pattern that expands to nothing).
      // BUG-525 (W2 labelglued): `corr` and `levelsof` take a varlist and
      // nothing else in that position, and the plain-varlist site is now also
      // where a parenthesised varlist is unwrapped -- oracle
      // tests/eval/golden/aer_w2/w2_cmdtriage.log arms corr_glued (r(rho)
      // equals ctl_corr's) and levelsof_glued (`0 1 2`) pin that half. The
      // wildcard/range half they inherit is Stata's varlist rule, not a
      // separate measurement on these two rows; it is stated here rather than
      // assumed silently. (`correlate` takes no factor-variable operators, so
      // the estimation-row caveat above does not apply to it.)
      "corr",     "levelsof",
      // gfx-binscatter: binscatter `varlist(min=2 numeric)` takes no
      // factor-variable or time-series operators, so `y*` / `a-b` are
      // Stata's varlist rule (w2k_binscatter.log BS_069a: `binscatter y* x`
      // posts e(y1_coefs) and e(y2_coefs), y and y2).
      "binscatter",
      // Package 007 (Stats_for_paper.do `tab edu_category`): tabulate, tab1 and
      // tab2 take a varlist and nothing else in that position, so the shared
      // wildcard/range/abbreviation rule applies (StataBE ec.do, ab3.do).
      "tabulate", "tab1", "tab2",
      // vs-stata bank (s3b_est_other): pwcorr, mean, proportion and pca take a
      // varlist in that position, so Stata's wildcard and range rule applies
      // (f7f455df `pwcorr m* p* t*`, 0f5e5609 `mean price-weight`, 0c8d7547 `pca
      // price-weight, components(2)`).
      "pwcorr", "mean", "proportion", "pca",
  };
  // BUG-1422: StataBE 19.5 `tsfill in 1/3`, `marginsplot in 1/3`, `tsset t in 1/3`,
  // `xtset g t in 1/3` -> "in range not allowed" r(101); the same with [fw=w] or
  // [aw=w] -> "weights not allowed" r(101); `marginsplot x1 in 1/3` -> "varlist
  // not allowed" r(101).
  // `sort` answers its own qualifiers too, in Stata's order: the varlist first
  // (`so if price>5000 in 1/10`, `so in 5`, `so [fw=rep78]`, `so = price*2`,
  // `so using "f"` are all r(100) "varlist required" in StataBE 19.5, truth bank
  // syntaxopt), then `if` r(101), weights r(101); its `in` is Stata syntax this
  // build does not implement and is refused by name in the arm.
  // `reshape` takes no qualifier at all: reshape.ado reads `if`, `in` and `[`
  // as words of its stub list (`reshape wide bp if sex==0, ...` and `... bp in
  // 1/20, ...` are r(111) "variable if/in not found", `... bp [aw=bp], ...`
  // r(198) "[ invalid name"; vs-stata bank 21a1700cb323d079, c64de4384b383b64,
  // d3847b0b1ac554dd), so its arm answers them.
  static const std::set<std::string> kQualifiersRefusedByArm = {"tsfill", "marginsplot", "tsset",
                                                                "xtset", "sort", "reshape"};
  static const bool kMarked = [] {
    for (auto& row : kRegistry) {
      if (kPlainVarlist.count(row.canonical) != 0) row.varlistIsPlain = true;
      if (kQualifiersRefusedByArm.count(row.canonical) != 0) row.qualifiersRefusedByArm = true;
    }
    return true;
  }();
  (void)kMarked;
  return kRegistry;
}

const std::vector<PrefixSpec>& prefixRegistry() {
  static const std::vector<PrefixSpec> kPrefixes = {
      // Grouping. Rowless by design (see registry.h); per-command support is
      // exposed through CommandSpec::allowByPrefix instead.
      {"by", "by grp: summarize y", "dispatcher.cpp parseBysortPrefix"},
      {"bysort", "bysort grp: summarize y", "dispatcher.cpp parseBysortPrefix"},
      // Output/error modifiers. These also head brace blocks (loops.cpp).
      {"capture", "capture summarize y", "dispatcher.cpp prefix stripper + loops.cpp"},
      {"quietly", "quietly summarize y", "dispatcher.cpp prefix stripper + loops.cpp"},
      {"noisily", "noisily summarize y", "dispatcher.cpp prefix stripper + loops.cpp"},
      // Resampling (P6). PIET-side replicate loop; no engine verb.
      {"bootstrap", "bootstrap mean=r(mean), reps(2) seed(1): summarize y", "handlers_resample.cpp"},
      {"jackknife", "jackknife mean=r(mean): summarize y", "handlers_resample.cpp"},
      {"permute", "permute y mean=r(mean), reps(2) seed(1): summarize y", "handlers_resample.cpp"},
      {"rolling", "rolling mean=r(mean), window(5): summarize y", "handlers_resample.cpp"},
      {"statsby", "statsby _b _se, clear: regress y grp", "handlers_resample.cpp"},
      // Survey (P7).
      {"svy", "svy: mean y", "estimation_m4.cpp svy prefix"},
      // Frames (P6).
      {"frame", "frame default: summarize y", "handlers_frames.cpp"},
      // Indicator expansion (AER bundle H). Unlike every prefix above, xi
      // REWRITES the line it prefixes: it materializes the _I* columns first,
      // then re-dispatches the wrapped command with the i. terms replaced by
      // the generated names.
      {"xi", "xi: summarize y", "handlers_aer_estim.cpp runXiPrefix"},
  };
  return kPrefixes;
}

int runDumpPrefixRegistry() {
  nlohmann::ordered_json out = nlohmann::ordered_json::array();
  for (const auto& p : prefixRegistry()) {
    nlohmann::ordered_json row;
    row["name"] = p.name;
    row["probe"] = p.probe;
    row["handler_tag"] = p.handlerTag;
    out.push_back(std::move(row));
  }
  std::printf("%s\n", out.dump().c_str());
  return 0;
}

int runDumpRegistry() {
  nlohmann::ordered_json out = nlohmann::ordered_json::array();
  for (const auto& spec : commandRegistry()) {
    nlohmann::ordered_json row;
    row["canonical"] = spec.canonical;
    row["abbrev"] = spec.abbrev;
    row["family"] = familyToString(spec.family);
    row["allow_if"] = spec.allowIf;
    row["allow_in_range"] = spec.allowInRange;
    row["weight"] = weightPolicyToString(spec.weight);
    row["allow_by_prefix"] = spec.allowByPrefix;
    row["known_options"] = spec.knownOptions;
    row["handler_tag"] = spec.handlerTag;
    row["by_illegal_in_stata"] = spec.byIllegalInStata;
    // WS-B (extcompat): additive key. Every consumer of this dump reads named
    // keys (coverage_check.py, freeze_matrix.py, floor_gate.py,
    // p2perf_bench_compare.py) — none diffs the raw bytes — so an extra field
    // is a pure addition to the drift-audit surface.
    row["community_names"] = spec.communityNames;
    out.push_back(std::move(row));
  }
  std::printf("%s\n", out.dump().c_str());
  return 0;
}

// A3 (BETA campaign, BS-096): linear scan is fine here — commandRegistry()
// is ~90 rows (canonical + abbrev spellings combined), called once per
// qualifier check on the interactive/`--do` path, nowhere near a hot loop.
// Matches canonical first, then any abbrev, so a row's own canonical name
// never needs to be duplicated into its abbrev list for this lookup to find
// it directly.
const CommandSpec* findByName(const std::string& lowerName) {
  for (const auto& spec : commandRegistry()) {
    if (spec.canonical == lowerName) return &spec;
    for (const auto& ab : spec.abbrev) {
      if (ab == lowerName) return &spec;
    }
  }
  return nullptr;
}

// BUG-105 (WS-K) — see registry.h for the full rationale, the falsified
// computed-prefix premise, and why community names are exempt.
std::string resolveCanonical(const std::string& lowerName) {
  const CommandSpec* spec = findByName(lowerName);
  if (spec == nullptr) return lowerName;
  for (const auto& cn : spec->communityNames) {
    if (cn == lowerName) return lowerName;
  }
  return spec->canonical;
}

bool isStataCommandAbbreviation(const std::string& typed) {
  // Compared as typed: Stata's command words are case-sensitive, so a program
  // named `G` is not `g` (generate) and still runs.
  if (typed.empty()) return false;
  // Spellings that are NOT abbreviations inside Stata but ado-files of their
  // own in StataNow's ado/base (r/reg.ado, d/desc.ado, l/levels.ado ...: shims
  // that call the full command). Stata looks for a program already in memory
  // before it searches the ado path, so `program define reg` then `reg price
  // mpg` runs the user's program. Listed from this registry's abbreviation
  // spellings that have a <spelling>.ado under ado/base (StataNow 19.5);
  // `so`, `sa`, `g`, `su` ... have none and stay built-in abbreviations.
  static const std::set<std::string> kAdoShims = {
      "desc",  "d",       "de",     "des",    "descr", "descri", "describ", "ren",
      "rena",  "renam",   "reg",    "regr",   "regre", "regres", "ap",      "app",
      "appe",  "appen",   "la",     "lab",    "labe",  "levels", "logi",    "prob",
      "probi", "te",      "tes",    "est",    "esti",  "estim",  "estima",  "estimat",
      "estimate", "hist", "tw",     "gr",     "note",  "xtdes"};
  if (kAdoShims.count(typed) != 0) return false;
  for (const auto& spec : commandRegistry()) {
    if (spec.canonical == typed) return false;
    for (const auto& ab : spec.abbrev) {
      if (ab != typed) continue;
      for (const auto& cn : spec.communityNames)
        if (cn == typed) return false;
      return spec.canonical.size() > typed.size() &&
             spec.canonical.compare(0, typed.size(), typed) == 0;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Stata's own syntax (registry.h StataQualifiers / StataLead). Every row cites
// its source: the command's shipped ado `syntax` statement (read for WHAT the
// syntax admits, never copied) and/or the StataBE 19.5 answer recorded in the
// truth bank of 2026-11-10 ("StataBE ... r(101)").
namespace {
constexpr StataQualifiers kStataQualifiers[] = {
    // ---- legal weight sets carried over from the tokenizer's former
    // kStataQualifiers (each command takes `if` and `in`) ----------------
    // summarize: its help ("aweights, fweights, and iweights are allowed");
    // oracle tests/eval/golden/aer_w2/w2_pweight.log:764-768 (`pweights not
    // allowed`, r(101)) and aer_b4/esttab-beta.log:338 (W_su_pweight_refused).
    {"summarize", true, true, "aweight fweight iweight"},
    // qreg: its help ("allows fweights, iweights, and pweights"); oracle
    // w2p_estim_ev.log:418-419 (`qreg ... [aw=w]` -> r(101), A_qreg_aw_q50).
    // ...under `version 12` and earlier its ado parses `[fw aw]` (qreg.ado `vc <= 12`).
    {"qreg", true, true, "fweight iweight pweight", 13, "aweight fweight"},
    // qreg2 (SSC): `syntax ... [fweight]`; oracle w2j_e3_qreg2.log:1996-2018.
    {"qreg2", true, true, "fweight"},
    // tabulate/tab1: "fweights, aweights, and iweights are allowed"; tab2:
    // "fweights are allowed by tab2". Oracle w2aj_qualifiers qf_tb_aw_sp/_iw_sp.
    {"tabulate", true, true, "aweight fweight iweight"},
    {"tab1", true, true, "aweight fweight iweight"},
    {"tab2", true, true, "fweight"},
    // pctile/xtile: aweights, fweights and pweights (pctile's help); _pctile
    // also iweights (StataBE `_pctile x [iw=wf]` rc 0, BUG-1444).
    {"pctile", true, true, "aweight fweight pweight"},
    {"_pctile", true, true, "aweight fweight iweight pweight"},
    {"xtile", true, true, "aweight fweight pweight"},
    // BUG-1444 (StataBE 19.5, 2026-10-29): pweights and iweights are r(101) on
    // correlate, pwcorr, tabstat (tabstat.ado `[aw fw]`, BUG-1151) and cumul;
    // aweights on probit and poisson.
    {"corr", true, true, "aweight fweight"},
    {"pwcorr", true, true, "aweight fweight"},
    {"tabstat", true, true, "aweight fweight"},
    {"cumul", true, true, "aweight fweight"},
    {"probit", true, true, "fweight iweight pweight"},
    {"poisson", true, true, "fweight iweight pweight", 7, "aweight fweight iweight pweight"},
    // BUG-1539/1537/1541 (StataBE 19.5, scratch-laneA2/agents/wml/x1.log).
    {"ppmlhdfe", true, true, "fweight pweight"},
    {"nbreg", true, true, "fweight iweight pweight", 7, "aweight fweight iweight pweight"},
    {"proportion", true, true, "fweight iweight pweight"},
    // ---- 2026-11-11 (vs-stata bank) --------------------------------------
    {"_dots", false, false, ""},  // _dots.ado `syntax [anything] [, ...]`; StataBE if/in/aw r(101)
    {"acreg", true, true, "fweight pweight"},  // acreg.ado `[fweight pweight]`; StataBE aw r(101)
    {"areg", true, true, "aweight fweight pweight"},  // areg.ado `[aw fw pw]`; StataBE iw r(101)
    {"assert", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"cdfplot", true, true, "aweight fweight iweight pweight"},  // cdfplot.ado `[aweight fweight iweight pweight]`; bank rc 0 for all four
    {"binscatter", true, true, "aweight fweight"},  // binscatter.ado `[aweight fweight]`; StataBE iw/pw r(101)
    {"centile", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"cf", false, false, ""},  // cf.ado `syntax varlist using/ [, ...]`; StataBE if/in r(101)
    {"clear", false, false, ""},  // clear.ado `syntax [anything]`; StataBE if r(101)
    {"cloglog", true, true, "fweight iweight pweight"},  // cloglog.ado `[fw iw pw]`; StataBE aw r(101)
    {"clonevar", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"codebook", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"compare", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"compress", false, false, ""},  // built-in; StataBE if/in/aw r(101)
    {"contract", true, true, "fweight"},  // contract.ado `[fw]`; StataBE aw/pw r(101)
    {"count", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"cross", false, false, ""},  // cross.ado `syntax using/`; StataBE if r(101)
    {"csdid", true, true, "iweight"},  // csdid.ado `[iw]`; StataBE aw/fw/pw r(101)
    {"describe", false, false, ""},  // describe.ado `syntax [anything] [using] [, ...]`; StataBE if/in/aw r(101)
    {"destring", false, false, ""},  // destring.ado `syntax [varlist], ...`; StataBE if/in r(101)
    {"dfuller", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"did_imputation", true, true, "aweight"},  // StataBE fw/iw/pw r(101), aw rc 0
    {"drawnorm", false, false, ""},  // drawnorm.ado `syntax newvarlist [, ...]`; StataBE if r(101)
    {"drdid", true, true, "iweight"},  // drdid.ado `[iw]`; StataBE aw/fw/pw r(101)
    {"drop", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"ds", false, false, ""},  // built-in `ds [varlist] [, options]`; StataBE if r(101)
    {"duplicates", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"egen", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    // StataBE `error 111 in 1/5` / `[aw=weight]`: r(197) "in range not allowed" /
    // "weights not allowed" (48fd40e7455e6945, 68563bac22df7952); `if` unmeasured.
    {"error", true, false, "", 0, nullptr, 197},
    {"estout", false, false, ""},  // estout.ado `syntax [anything] [using] [, ...]`; StataBE if/in r(101)
    {"esttab", false, false, ""},  // esttab.ado `syntax [anything] [using] [, ...]`; StataBE if/in r(101)
    {"fillin", false, false, ""},  // fillin.ado `syntax varlist(min=2)`; StataBE if r(101)
    {"fmerge", false, false, ""},  // fmerge.ado `syntax [varlist(default=none)] using/ [, ...]`; StataBE if r(101)
    {"format", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"fracreg", true, true, "fweight iweight pweight"},  // StataBE aw r(101); fw/iw/pw rc 0
    {"frget", false, false, ""},  // frget.ado `syntax anything(equalok), ...`; StataBE if/in r(101)
    {"frlink", false, false, ""},  // built-in `frlink 1:1|m:1 varlist, frame()`; StataBE if r(101)
    {"frmttable", false, false, ""},  // frmttable.ado `syntax [using/], [...]`; StataBE if r(101)
    {"gduplicates", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"generate", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"getmata", false, false, ""},  // getmata.ado `syntax [anything] [, ...]`; StataBE if r(101)
    {"gisid", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"gquantiles", true, true, "aweight fweight pweight"},  // StataBE iw r(101); aw/fw/pw rc 0
    {"graph", true, true, "aweight fweight pweight"},  // StataBE `graph twoway ... [iw=]` r(101); aw/fw/pw rc 0
    {"greshape", false, false, ""},  // greshape.ado `syntax anything, ...`; StataBE if r(101)
    {"grstyle", false, false, nullptr},  // StataBE if/in r(101) (its `[aw=]` is read as a name, r(198))
    {"heckman", true, true, "fweight iweight pweight"},  // heckman.ado `[pw fw iw]`; StataBE aw r(101) (vs-stata bank aedc499f)
    {"histogram", true, true, "fweight"},  // histogram.ado `[fw]`; StataBE aw/iw/pw r(101)
    {"insobs", false, false, ""},  // insobs.ado `syntax anything [, ...]`; StataBE if/in r(101)
    {"inspect", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"integ", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"ipolate", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"isid", false, false, ""},  // isid.ado `syntax varlist [, Sort Missok]`; StataBE if/in/aw r(101)
    {"ivprobit", true, true, "fweight iweight pweight"},  // ivprobit.ado `[fw pw iw/]`; StataBE aw r(101)
    {"ivreghdfe", true, true, "aweight fweight pweight"},  // StataBE iw r(101) "iweights not allowed" (bank 83ecde87)
    {"ivtobit", true, true, "fweight iweight pweight"},  // ivtobit.ado `[fw pw iw/]`; StataBE aw r(101)
    {"joinby", false, false, ""},  // joinby.ado `syntax [varlist(default=none)] using/ [, ...]`; StataBE if/in/aw r(101)
    {"kdensity", true, true, "aweight fweight iweight"},  // StataBE pw r(101); aw/fw/iw rc 0
    {"keep", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"label", true, false, nullptr},  // StataBE `label ... in` r(101) (`if`/weights: r(198) invalid syntax)
    {"labelbook", false, false, ""},  // labelbook.ado `syntax [anything] [using/] [, ...]`; StataBE if/in/aw r(101)
    {"labmask", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"labvars", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"leebounds", true, true, "fweight iweight pweight"},  // leebounds.ado `[pweight fweight iweight/]`; StataBE aw r(101)
    {"levelsof", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"line", true, true, "aweight fweight pweight"},  // StataBE iw r(101); aw/fw/pw rc 0
    {"list", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"logistic", true, true, "fweight iweight pweight"},  // StataBE aw r(101); fw/iw/pw rc 0
    {"logit", true, true, "fweight iweight pweight"},  // StataBE aw r(101); fw/iw/pw rc 0
    {"lowess", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"lpoly", true, true, "aweight fweight"},  // StataBE iw/pw r(101); aw/fw rc 0
    {"markout", false, false, ""},  // built-in; StataBE if/in/aw r(101)
    {"matlist", false, false, ""},  // matlist.ado `syntax anything [, *]`; StataBE if/in/aw r(101)
    {"matselrc", false, false, nullptr},  // StataBE if/in r(101)
    {"mdesc", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"mediate", true, true, "fweight iweight pweight"},  // mediate.ado `[pw iw fw]`; StataBE aw r(101)
    {"merge", false, false, ""},  // merge.ado `syntax [varlist(default=none)] using/ [, ...]`; StataBE if/in/aw r(101)
    {"missings", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"mixed", true, true, "fweight pweight"},  // StataBE aw/iw r(101); fw/pw rc 0
    {"mkmat", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"mkspline", true, true, "fweight"},  // mkspline.ado `[fweight]`; StataBE aw/pw r(101)
    {"mlogit", true, true, "fweight iweight pweight", 18, "aweight fweight iweight pweight"},  // StataBE aw r(101); fw/iw/pw rc 0; mlogit.ado allows aw when _caller() < 18
    {"move", false, false, ""},  // built-in `move varname1 varname2`; StataBE if r(101)
    {"mvdecode", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"mvencode", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"newey", true, true, "aweight"},  // newey.ado `[aw/]`; StataBE fw/iw/pw r(101)
    {"notes", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"char", false, false, ""},  // built-in `char`; StataBE if/in/weights r(101)
    {"numlabel", false, false, ""},  // numlabel.ado `syntax [anything] [, ...]`; StataBE in/aw r(101)
    {"ologit", true, true, "fweight iweight pweight", 19, "aweight fweight iweight pweight"},  // StataBE aw r(101); fw/iw/pw rc 0; ologit.ado allows aw when _caller() < 19
    {"oprobit", true, true, "fweight iweight pweight", 19, "aweight fweight iweight pweight"},  // StataBE aw r(101); fw/iw/pw rc 0; oprobit.ado allows aw when _caller() < 19
    {"order", false, false, ""},  // order.ado `syntax varlist [, ...]`; StataBE if r(101)
    {"pca", true, true, "aweight fweight"},  // StataBE iw/pw r(101); aw/fw rc 0
    {"pperron", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"prais", true, true, ""},  // StataBE weights r(101) "weights not allowed" (bank 89ef3eaf [aw=t])
    {"preserve", false, false, ""},  // built-in; StataBE if/in/aw r(101)
    {"rangestat", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"rddensity", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"rdplot", true, false, ""},  // rdplot.ado `syntax anything [if] [, ...]`; StataBE in/aw/fw/iw/pw r(101)
    {"rdrobust", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"recast", false, false, ""},  // recast.ado `syntax varlist [, FORCE DRYRUN]`; StataBE if/in/aw r(101)
    {"recode", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"reghdfe", true, true, "aweight fweight pweight"},  // StataBE iw r(101); aw/fw/pw rc 0
    {"reg3", true, true, "aweight fweight"},  // reg3.ado `[aw fw]`; StataBE iw/pw r(101)
    {"regsave", false, false, ""},  // regsave.ado `syntax [anything] [using/] [, ...]`; StataBE if r(101)
    {"renvarlab", false, false, ""},  // renvarlab.ado `syntax newvarlist [, ...]`; StataBE if/in/aw r(101)
    {"renvars", false, false, ""},  // renvars.ado `syntax newvarlist [, ...]`; StataBE if/in/aw r(101)
    {"replace", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"require", false, false, ""},  // require.ado `syntax anything, [...]`; StataBE if/in/aw r(101)
    {"restore", false, false, ""},  // built-in; StataBE if/in/aw r(101)
    {"sample", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"save", false, false, ""},  // built-in `save [filename] [, options]`; StataBE if r(101)
    {"scalar", false, true, ""},  // StataBE `scalar x = 5 if foreign` / `[aw=]` r(101); `in` rc 0
    {"scatter", true, true, "aweight fweight pweight"},  // StataBE iw r(101); aw/fw/pw rc 0
    {"sctorezone", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"sdid", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"sdtest", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"sencode", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"separate", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"signrank", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"sktest", true, true, "aweight fweight"},  // sktest.ado `[aw fw]`; StataBE pw r(101)
    {"sort", false, true, ""},  // built-in `sort varlist [in] [, stable]`; StataBE if/aw r(101)
    {"spearman", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"split", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"splitsample", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"splitvallabels", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"stack", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"stcox", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"streg", true, true, ""},  // StataBE `streg ... [pweight=]` r(101) "weights must be stset" (vs-stata bank 850cc61d)
    {"strdist", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"stset", true, false, "fweight pweight iweight"},  // stset.ado `syntax varlist [if/] [fw pw iw/]`; StataBE in/aw r(101)
    {"sureg", true, true, "aweight fweight"},  // sureg.ado `[aw fw]`; StataBE iw/pw r(101)
    {"svyset", false, false, "pweight iweight"},  // svyset.ado `[pweight iweight/]`; StataBE if/in/aw/fw r(101)
    {"tabout", true, true, "aweight fweight iweight"},  // tabout.ado `[fweight aweight iweight]`; StataBE pw r(101)
    {"teffects", true, true, "fweight iweight pweight"},  // StataBE aw r(101); fw/iw/pw rc 0
    {"tolower", false, false, ""},  // StataBE if/in/aw r(101)
    {"tostring", false, false, ""},  // tostring.ado `syntax varlist, [...]`; StataBE if/in/aw r(101)
    {"tset", false, false, ""},  // StataBE if/in/aw r(101)
    {"tsline", true, true, "aweight fweight pweight"},  // StataBE iw r(101); aw/fw/pw rc 0
    {"tssmooth", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"tsspell", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"ttest", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"twoway", true, true, "aweight fweight pweight"},  // StataBE iw r(101); aw/fw/pw rc 0
    {"twoway__histogram_gen", true, true, "fweight"},  // twoway__histogram_gen.ado `[fw]`; StataBE aw/iw/pw r(101)
    {"unab", false, false, ""},  // unab.ado `syntax [varlist(...)]`; StataBE if r(101)
    {"unique", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"var", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"winsor", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"winsor2", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"wntestq", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"xi", false, false, ""},  // xi.ado `syntax [anything] [, PREfix() NOOMIT]`; StataBE if/in/aw r(101)
    {"xtabond", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"xtdescribe", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"xtline", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"xtlogit", true, true, "fweight iweight pweight"},  // xtlogit.ado `[iweight fweight pweight]`; StataBE aw r(101)
    {"xtpoisson", true, true, "fweight iweight pweight"},  // xtpoisson.ado `[iweight fweight pweight]`; StataBE aw r(101)
    {"xtsum", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"xttab", true, true, ""},  // StataBE weights r(101) "weights not allowed"
    {"zinb", true, true, "fweight iweight pweight", 7, "aweight fweight iweight pweight"},  // zinb.ado; StataBE aw r(101); aw when _caller() < 7
    {"zip", true, true, "fweight iweight pweight", 7, "aweight fweight iweight pweight"},  // zip.ado; StataBE aw r(101); aw when _caller() < 7
};

// {canonical, bare, bareRc, withOptions, withOptionsRc, replay, noOptions,
//  qualifiersAreLead}. Source of every row: StataBE 19.5 truth bank 2026-11-10,
// the cells `<cmd>` and `<cmd> , <unknown option>` (and `<cmd> <lead>,
// <unknown option>` for noOptions) with a dataset loaded and no estimates in
// memory; cross-checked against the ado `syntax` statement where one ships
// (a required `varlist`/`using/`/`=/exp`, or a replay branch `if replay()`).
constexpr StataLead kStataLead[] = {
    {"_pctile", "varlist required", 100, "varlist required", 100, false, false, false},
    {"append", "using required", 100, "using required", 100, false, false, false},
    {"areg", nullptr, 0, nullptr, 0, true, false, false},
    {"astile", "namelist required", 100, "namelist required", 100, false, false, false},
    {"binscatter", "varlist required", 100, "varlist required", 100, false, false, false},
    {"carryforward", "varlist required", 100, "varlist required", 100, false, false, false},
    {"cdfplot", "varlist required", 100, "varlist required", 100, false, false, false},
    {"cf", "varlist required", 100, "varlist required", 100, false, false, false},
    // clear.ado `syntax [anything]`: `clear, zz` r(101) (vs-stata bank d3045496a568112c).
    {"clear", nullptr, 0, "options not allowed", 101, false, false, false},
    {"cloglog", nullptr, 0, nullptr, 0, true, false, false},
    // clonevar takes no options: `clonevar p2 = price, zznotanoption` r(101)
    // (vs-stata bank e7f86976681cfa70); with nothing before the comma its own
    // usage refusal, r(198) (`clonevar , zzabbrprobe`, 8e5794e8f72d9d5b).
    {"clonevar", nullptr, 0, "syntax is clonevar newvar = varname ...", 198, false, true, false},
    {"compare", "varlist required", 100, "varlist required", 100, false, true, false},
    {"contract", "varlist required", 100, "varlist required", 100, false, false, false},
    {"count", nullptr, 0, "options not allowed", 101, false, true, false},
    {"cross", "using required", 100, "options not allowed", 101, false, true, false},
    {"csdid", nullptr, 0, nullptr, 0, true, false, false},
    {"decode", "varlist required", 100, "varlist required", 100, false, false, false},
    {"dfuller", "varlist required", 100, "varlist required", 100, false, false, false},
    {"did_imputation", "varlist required", 100, "varlist required", 100, false, false, false},
    {"drawnorm", "varlist required", 100, "varlist not allowed", 101, false, false, false},
    {"drdid", nullptr, 0, nullptr, 0, true, false, false},
    {"drop", "varlist, if exp, or in range required", 100, "options not allowed", 101, false, true, true},
    {"encode", "varlist required", 100, "varlist required", 100, false, false, false},
    {"erase", "invalid file specification", 198, nullptr, 0, false, true, false},
    {"estadd", "subcommand required", 100, "subcommand required", 100, false, false, false},
    {"estpost", nullptr, 0, nullptr, 0, true, false, false},
    {"eventstudyinteract", "varlist required", 100, "varlist required", 100, false, false, false},
    {"expand", "=exp required", 100, "=exp required", 100, false, false, false},
    {"fillin", "varlist required", 100, "varlist required", 100, false, true, false},
    {"fillmissing", "varlist required", 100, "varlist required", 100, false, false, false},
    {"findfile", "file \"\" not found", 601, "varlist not allowed", 101, false, false, false},
    {"format", nullptr, 0, "options not allowed", 101, false, false, false},
    {"fracreg", nullptr, 0, nullptr, 0, true, false, false},
    {"frget", "something required", 100, "something required", 100, false, false, false},
    {"gegen", "something required", 100, "something required", 100, false, false, false},
    {"generate", "=exp required", 100, "=exp required", 100, false, false, true},
    {"geodist", "something required", 100, "something required", 100, false, false, false},
    {"geonear", "varlist required", 100, "varlist required", 100, false, false, false},
    {"gisid", "varlist required", 100, "varlist required", 100, false, false, false},
    {"glevelsof", "something required", 100, "something required", 100, false, false, false},
    {"glm", nullptr, 0, nullptr, 0, true, false, false},
    {"gmm", nullptr, 0, nullptr, 0, true, false, false},
    {"graph", nullptr, 0, "varlist not allowed", 101, false, false, false},
    {"gsort", "varlist required", 100, "varlist required", 100, false, false, false},
    {"gunique", "varlist required", 100, "varlist required", 100, false, false, false},
    {"heckman", nullptr, 0, nullptr, 0, true, false, false},
    {"insheet", "using required", 100, nullptr, 0, false, false, false},
    {"insobs", "number of inserted observations required", 100, "number of inserted observations required", 100, false, false, false},
    {"inspect", nullptr, 0, "options not allowed", 101, false, true, false},
    {"integ", "varlist required", 100, "varlist required", 100, false, false, false},
    {"ipolate", "varlist required", 100, "varlist required", 100, false, false, false},
    {"isid", "varlist required", 100, "varlist required", 100, false, false, false},
    {"ivprobit", nullptr, 0, nullptr, 0, true, false, false},
    {"ivreg", nullptr, 0, nullptr, 0, true, false, false},
    {"ivregress", nullptr, 0, nullptr, 0, true, false, false},
    {"ivtobit", nullptr, 0, nullptr, 0, true, false, false},
    {"joinby", "using required", 100, "using required", 100, false, false, false},
    {"kdensity", "varlist required", 100, "varlist required", 100, false, false, false},
    {"keep", "varlist, if exp, or in range required", 100, "options not allowed", 101, false, true, true},
    {"labmask", "varlist required", 100, "varlist required", 100, false, false, false},
    {"lasso", nullptr, 0, nullptr, 0, true, false, false},
    {"leebounds", nullptr, 0, nullptr, 0, true, false, false},
    {"levelsof", "varlist required", 100, "varlist required", 100, false, false, false},
    {"line", nullptr, 0, "varlist required", 100, false, false, false},
    {"logistic", nullptr, 0, nullptr, 0, true, false, false},
    {"logit", nullptr, 0, nullptr, 0, true, false, false},
    {"lowess", "varlist required", 100, "varlist required", 100, false, false, false},
    {"lpoly", "varlist required", 100, "varlist required", 100, false, false, false},
    {"markout", "varlist required", 100, "varlist required", 100, false, false, false},
    {"matlist", "matrix name or matrix expression required", 100, "matrix name or matrix expression required", 100, false, false, false},
    {"mean", nullptr, 0, nullptr, 0, true, false, false},
    {"mediate", nullptr, 0, nullptr, 0, true, false, false},
    {"mixed", nullptr, 0, nullptr, 0, true, false, false},
    {"mkdir", "invalid syntax", 198, "varlist not allowed", 101, false, false, false},
    {"mkmat", "varlist required", 100, "varlist required", 100, false, false, false},
    {"ml", nullptr, 0, nullptr, 0, true, false, false},
    {"mlogit", nullptr, 0, nullptr, 0, true, false, false},
    {"move", "varlist required", 100, "varlist required", 100, false, true, false},
    {"mvdecode", "varlist required", 100, "varlist required", 100, false, false, false},
    {"mvencode", "varlist required", 100, "varlist required", 100, false, false, false},
    {"nbreg", nullptr, 0, nullptr, 0, true, false, false},
    {"newey", nullptr, 0, nullptr, 0, true, false, false},
    {"nl", nullptr, 0, nullptr, 0, true, false, false},
    {"nlcom", nullptr, 0, nullptr, 0, true, false, false},
    {"notes", nullptr, 0, "varlist required", 100, false, false, false},
    {"char", nullptr, 0, nullptr, 0, false, false, false},
    {"ologit", nullptr, 0, nullptr, 0, true, false, false},
    {"oprobit", nullptr, 0, nullptr, 0, true, false, false},
    {"order", "varlist required", 100, "varlist required", 100, false, false, false},
    {"outfile", "using required", 100, "using required", 100, false, false, false},
    {"outsheet", "using required", 100, "using required", 100, false, false, false},
    {"pca", nullptr, 0, nullptr, 0, true, false, false},
    {"pctile", "varlist required", 100, "varlist required", 100, false, false, false},
    {"poisson", nullptr, 0, nullptr, 0, true, false, false},
    {"pperron", "varlist required", 100, "varlist required", 100, false, false, false},
    {"ppmlhdfe", nullptr, 0, nullptr, 0, true, false, false},
    {"prais", nullptr, 0, nullptr, 0, true, false, false},
    {"probit", nullptr, 0, nullptr, 0, true, false, false},
    {"proportion", nullptr, 0, nullptr, 0, true, false, false},
    {"qreg", nullptr, 0, nullptr, 0, true, false, false},
    {"rangestat", "slist required", 100, "slist required", 100, false, false, false},
    {"rddensity", "varlist required", 100, "varlist required", 100, false, false, false},
    {"rdplot", "something required", 100, "something required", 100, false, false, false},
    {"rdrobust", "something required", 100, "something required", 100, false, false, false},
    {"recode", "varlist (rule).. required", 100, "varlist (rule).. required", 100, false, false, false},
    {"reg3", nullptr, 0, nullptr, 0, true, false, false},
    {"renpfix", nullptr, 0, "varlist required", 100, false, false, false},
    {"replace", "varlist required", 100, "varlist required", 100, false, false, false},
    // `require, list` runs (require.ado `cap syntax [using], list [*]`), so only the
    // bare command is a refusal.
    {"require", "something required", 100, nullptr, 0, false, false, false},
    {"rm", "invalid file specification", 198, nullptr, 0, false, true, false},
    {"scatter", nullptr, 0, "varlist required", 100, false, false, false},
    {"sctorezone", "time zone shift or starttime variable required", 100, "time zone shift or starttime variable required", 100, false, false, false},
    {"sdid", "varlist required", 100, "varlist required", 100, false, false, false},
    {"sdtest", "varlist required", 100, "varlist required", 100, false, false, false},
    {"sencode", "varlist required", 100, "varlist required", 100, false, false, false},
    {"separate", "varlist required", 100, "varlist required", 100, false, false, false},
    {"signrank", "varlist required", 100, "varlist required", 100, false, false, false},
    {"sktest", "varlist required", 100, "varlist required", 100, false, false, false},
    {"sort", "varlist required", 100, "varlist required", 100, false, false, false},
    {"split", "varlist required", 100, "varlist required", 100, false, false, false},
    {"splitvallabels", "varlist required", 100, "varlist required", 100, false, false, false},
    {"stack", "varlist required", 100, "varlist required", 100, false, false, false},
    // st estimation commands replay: `stcox , zzabbrprobe` with no results is r(301)
    // "last estimates not found" (vs-stata bank d00dde22, 9bb917af).
    {"stcox", nullptr, 0, nullptr, 0, true, false, false},
    {"streg", nullptr, 0, nullptr, 0, true, false, false},
    {"strdist", "something required", 100, "something required", 100, false, false, false},
    {"sureg", nullptr, 0, nullptr, 0, true, false, false},
    {"tab1", "varlist required", 100, "varlist required", 100, false, false, false},
    {"tab2", "varlist required", 100, "varlist required", 100, false, false, false},
    {"tabstat", "varlist required", 100, "varlist required", 100, false, false, false},
    {"tabulate", "varlist required", 100, "varlist required", 100, false, false, false},
    {"test", nullptr, 0, nullptr, 0, true, false, false},
    {"testparm", "varlist required", 100, "varlist required", 100, false, false, false},
    {"texsave", "using required", 100, "using required", 100, false, false, false},
    {"tobit", nullptr, 0, nullptr, 0, true, false, false},
    {"tolower", nullptr, 0, "options not allowed", 101, false, true, false},
    {"tostring", "varlist required", 100, "varlist required", 100, false, false, false},
    {"tsline", nullptr, 0, "varlist required", 100, false, false, false},
    {"ttest", "varlist required", 100, "varlist required", 100, false, false, false},
    {"unique", "varlist required", 100, "varlist required", 100, false, false, false},
    {"unzipfile", "zipfile required", 100, "zipfile required", 100, false, false, false},
    {"var", nullptr, 0, nullptr, 0, true, false, false},
    {"winsor", "varlist required", 100, "varlist required", 100, false, false, false},
    {"winsor2", "varlist required", 100, "varlist required", 100, false, false, false},
    {"wntestq", "varlist required", 100, "varlist required", 100, false, false, false},
    {"xtabond", nullptr, 0, nullptr, 0, true, false, false},
    {"xtile", "varlist required", 100, "varlist required", 100, false, false, false},
    {"xtline", "varlist required", 100, "varlist required", 100, false, false, false},
    {"xtlogit", nullptr, 0, nullptr, 0, true, false, false},
    {"xtpoisson", nullptr, 0, nullptr, 0, true, false, false},
    {"xtreg", nullptr, 0, nullptr, 0, true, false, false},
    {"xttab", "varlist required", 100, "varlist required", 100, false, false, false},
    {"zinb", nullptr, 0, nullptr, 0, true, false, false},
    {"zip", nullptr, 0, nullptr, 0, true, false, false},
    {"zipfile", "files or directories required", 100, "files or directories required", 100, false, false, false},
};
}  // namespace

const StataQualifiers* findStataQualifiers(const std::string& canonical) {
  for (const auto& row : kStataQualifiers)
    if (canonical == row.canonical) return &row;
  return nullptr;
}

const StataLead* findStataLead(const std::string& canonical) {
  for (const auto& row : kStataLead)
    if (canonical == row.canonical) return &row;
  return nullptr;
}

namespace {
// {canonical, refusesFactor, refusesTimeSeries}. Source of every row: the
// StataBE 19.5 refusal in the truth bank of 2026-11-10 -- "factor-variable and
// time-series operators not allowed" (both) for `keep i.foreign`, `sort
// i.foreign`, `tab i.foreign`, `tabstat D.gnp96 L.gnp96`, `ttest D.gnp96 == 0`,
// `xttab L.jl`, `format price 9.2f`, ...; "factor-variable operators not
// allowed" (time-series taken) for `cor price i.foreign` (and `cor gnp96
// L.gnp96` runs), `scatter price i.foreign`, `xtsum i.dry`.
constexpr StataOperators kStataOperators[] = {
    {"corr", true, false},      {"format", true, true},   {"histogram", true, true},
    {"keep", true, true},       {"mdesc", true, true},    {"pca", true, true},
    {"recode", true, true},     {"scatter", true, false}, {"sort", true, true},
    {"tab1", true, true},       {"tab2", true, true},     {"tabstat", true, true},
    {"tabulate", true, true},   {"ttest", true, true},    {"twoway", true, false},
    {"unab", true, true},       {"xtsum", true, false},   {"xttab", true, true},
    // xtivreg2's varlist(ts): "factor-variable operators not allowed" (bank d5505dc3
    // `xtivreg2 frate unemp i.year (beertax = spirits drinkage), fe cluster(sid)`)
    {"xtivreg2", true, false},
};
}  // namespace

const StataOperators* findStataOperators(const std::string& canonical) {
  for (const auto& row : kStataOperators)
    if (canonical == row.canonical) return &row;
  return nullptr;
}

// ---------------------------------------------------------------------------
// WS-B (extcompat, 2026-08-06) — the community-package attribution record.
//
// `author` is transcribed VERBATIM from NOTICE.txt section 2 at the repo root.
// That file is the single source of truth for the name -> author mapping and
// the only place where the attribution may be edited; a name added here
// without a NOTICE.txt line is an unattributed name, which is the exact defect
// this table exists to prevent. Nothing here is ever printed on the ordinary
// success path — it reaches a user only through the r(199) reject message
// below, the first-run consent dialog, Settings, NOTICE.txt and the docs
// (LEGAL-DISTRIBUTION §2.7(c): never in command output, which the answerbook
// gates byte-compare).
//
// `nativeEquivalent` is filled only where PIET's own command for the same job
// is structurally provable, not merely plausible:
//   reghdfe    -> feols      (the SAME registry row; canonical)
//   ivreghdfe  -> ivregress  (the same handler, runIvRegress)
//   ppmlhdfe   -> poisson    (the same handler, runPoisson)
//   eststo     -> estimates  (the same dispatcher arm — see the eststo row)
// The other thirteen names have no native equivalent in this build, so their
// message drops the middle sentence rather than pointing at a command that
// answers a different question.
const std::vector<CommunityName>& communityNameTable() {
  static const std::vector<CommunityName> kNames = {
      {"reghdfe", "Sergio Correia (+ Paulo Guimaraes, Tom Zylkin)", "feols"},
      {"ivreghdfe", "Sergio Correia (+ Paulo Guimaraes, Tom Zylkin)", "ivregress"},
      {"ppmlhdfe", "Sergio Correia (+ Paulo Guimaraes, Tom Zylkin)", "poisson"},
      {"boottest", "David Roodman", ""},
      {"csdid", "Fernando Rios-Avila; Brantly Callaway, Pedro H. C. Sant'Anna", ""},
      {"drdid", "Fernando Rios-Avila; Brantly Callaway, Pedro H. C. Sant'Anna", ""},
      {"rdrobust",
       "Sebastian Calonico, Matias D. Cattaneo, Max H. Farrell, Rocio Titiunik "
       "(+ Michael Jansson)",
       ""},
      {"rdplot",
       "Sebastian Calonico, Matias D. Cattaneo, Max H. Farrell, Rocio Titiunik "
       "(+ Michael Jansson)",
       ""},
      {"rddensity",
       "Sebastian Calonico, Matias D. Cattaneo, Max H. Farrell, Rocio Titiunik "
       "(+ Michael Jansson)",
       ""},
      {"binscatter", "Michael Stepner", ""},
      {"did_imputation", "Kirill Borusyak", ""},
      {"eventstudyinteract", "Liyang Sun", ""},
      {"sdid", "Damian Clarke, Daniel Pailanir", ""},
      {"winsor2", "Yujun Lian", ""},
      {"winsor", "Nicholas J. Cox", ""},
      // gtools / ftools (AER-SCOPE1 bundle C). Attribution is a licence
      // obligation, not a courtesy, and it is why `fastreshape` has no row:
      // an unattributed name is the defect, so the name waits for its author.
      {"gcollapse", "Mauricio Caceres Bravo", "collapse"},
      {"gegen", "Mauricio Caceres Bravo", "egen"},
      {"gduplicates", "Mauricio Caceres Bravo", "duplicates"},
      {"glevelsof", "Mauricio Caceres Bravo", "levelsof"},
      {"gisid", "Mauricio Caceres Bravo", "isid"},
      {"greshape", "Mauricio Caceres Bravo", "reshape"},
      {"gquantiles", "Mauricio Caceres Bravo", "xtile"},
      {"gstats", "Mauricio Caceres Bravo", ""},
      {"fcollapse", "Sergio Correia", "collapse"},
      {"fmerge", "Sergio Correia", "merge"},
      {"esttab", "Ben Jann", ""},
      {"eststo", "Ben Jann", "estimates"},
      {"coefplot", "Ben Jann", ""},
      // AER-SCOPE1 bundle E (TABLES). Every author below was read from the
      // package's own `*!` line via `which <pkg>` on 2026-09-04, not taken from
      // the plan's prose -- which corrected the plan twice: outreg2 credits
      // Roy Wada AND John Gallup (its header says "based on outreg 3.0.6/4.0.0
      // by john_gallup"), and mat2txt is Ben Jann's 1.1.2 over M Blasnik's
      // 1.1.1. `frmttable` ships inside the `outreg` package rather than under
      // its own SSC name, which is why a bare `ssc install frmttable` answers
      // 601; it is John Luke Gallup's, version 1.32.
      {"estadd", "Ben Jann", ""},
      {"estpost", "Ben Jann", ""},
      {"estout", "Ben Jann", ""},
      {"outreg2", "Roy Wada, based on outreg by John Luke Gallup", ""},
      {"texsave", "Julian Reif", ""},
      {"texdoc", "Ben Jann", ""},
      {"tabout", "Ian Watson", ""},
      {"frmttable", "John Luke Gallup", ""},
      {"regsave", "Julian Reif", ""},
      {"parmest", "Roger Newson", ""},
      {"mat2txt", "Ben Jann, M Blasnik", ""},
      {"erepost", "Ben Jann", ""},
      // AER-SCOPE1 bundle I (UTIL). Read from each package's own `*!` line or
      // Authors block on 2026-09-04. `unique`'s ado gives only initials ("MH
      // and TB"); the full names come from its help file. `distinct`'s ado
      // credits NJC alone, so that is what is recorded -- not the plan's
      // second name, which the package does not claim.
      {"unique", "Michael Hills, Tony Brady", ""},
      {"gunique", "Mauricio Caceres Bravo", "unique"},
      {"distinct", "Nicholas J. Cox", ""},
      {"missings", "Nicholas J. Cox", ""},
      {"mdesc", "Rose Anne Medeiros, Dan Blanchette", ""},
      {"labmask", "Nicholas J. Cox", ""},
      {"renvarlab", "Joseph Canner", ""},
      {"splitvallabels", "Nicholas Winter, Ben Jann", ""},
      {"filelist", "Robert Picard", ""},
      {"carryforward", "David Kantor", ""},
      {"ihstrans", "Jan Helmdag", ""},
      {"sctorezone", "SurveyCTO (William Blackmon, IPA)", ""},
      // Package-007 lane LB. leebounds.ado's `*!` lines: "version 1.5 2013-07-17"
      // and "author Harald Tauchmann". Nearest native: none.
      {"leebounds", "Harald Tauchmann", ""},
      {"rangestat", "Robert Picard, Nicholas J. Cox, Roberto Ferrer", ""},
      {"tsspell", "Nicholas J. Cox", ""},
      // AER-SCOPE1 bundle J (GRAPH-X, 2026-09-04). `grc1leg` was NOT here at
      // that reading: its .ado header carries no author line. AER-SCOPE2 K1
      // (2026-09-07) found the author in the installer's `stata.trk` record
      // instead (package `grc1leg.pkg`, credited to Vince Wiggins,
      // StataCorp) -- see the row below.
      {"grc1leg2", "Mead Over", ""},
      {"grc1leg", "Vince Wiggins (StataCorp)", "grc1leg2"},
      {"grstyle", "Ben Jann", ""},
      {"geodist", "Robert Picard", ""},
      {"geonear", "Robert Picard", ""},
      {"cdfplot", "Adrian Mander", ""},
      {"event_plot", "Kirill Borusyak", ""},
      // AER scope 2, bundle K1 (2026-09-07). Both authors are in the package's
      // own header on this machine. `renvars` names a native equivalent of ""
      // deliberately: `renvarlab` is NOT one -- it cannot spell the map form or
      // presub/postsub/subst -- and pointing there would promise a
      // compatibility that does not exist.
      {"tolower", "Nicholas J. Cox", ""},
      // AER scope 2, bundle B2. strdist.ado's own header line.
      {"strdist", "Michael D Barker, Felix Pöge", ""},
      // oaxaca.ado's `*!` line: "version 4.1.1  24apr2023  Ben Jann". Nearest
      // native: none (regress is the ingredient, not the decomposition).
      {"oaxaca", "Ben Jann", ""},
      // parallel.ado's own header: "by George G. Vega [cre,aut], Brian
      // Quistorff [ctb]". Both are named because the package names both.
      {"parallel", "George G. Vega Yon, Brian Quistorff", ""},
      {"renvars", "Nicholas J. Cox, Jeroen Weesie", ""},
      // Provenance differs between these two and the difference is recorded
      // rather than smoothed over. ivreg2 is VERIFIED against the shipped
      // package on this machine: the help file of ivreg2 (help ivreg2) has its own citation block, which reads
      // "Baum, C.F., Schaffer, M.E., Stillman, S. 2010." and lists all three
      // with affiliations. (ivreg2.ado's header shorthand "authors cfb & mes"
      // is a code comment, not the attribution -- taking it literally would
      // have dropped Stillman.) xtivreg2 1.0.17 has been installed on the box
      // since 2026-10-27 (User decision C); its header reads "author mes"
      // (Mark E. Schaffer), which agrees with the published module citation.
      {"ivreg2", "Christopher F. Baum, Mark E. Schaffer, Steven Stillman", "ivregress"},
      {"xtivreg2", "Mark E. Schaffer", "ivregress"},

      // AER-SCOPE2 bundle K1 (2026-09-07). `renvars` corrects the bundle-I
      // "DELIBERATELY ABSENT" reading above: its ado header does carry an
      // author (`*! 2.4.0 22aug2005 njc` / `*! 2.3.0 01feb2001 njc/jw
      // STB-60 dm88`); it just isn't an SSC package (`ssc install renvars`
      // answers r(601)) -- it installs from the Stata Journal instead
      // (`net install dm88_1, from(http://www.stata-journal.com/software/sj5-4)`).
      // `tolower`'s own ado carries the author as a plain comment rather than
      // a `*!` line (`* Nick Cox Statalist Digest 895`), and this machine's
      // `stata.trk` record for `tolower.pkg` spells the same name out in full
      // ("Author: Nicholas J. Cox, University of Durham") -- both sources
      // agree, so this is not a guess from one weak signal.
      // `parallel` (SSC): checked both required sources and neither
      // resolves it on this machine -- ado/plus has no `parallel.ado` of its
      // own (only the unrelated `parallel_map.ado`, bundled inside Sergio
      // Correia's `ftools` package), and `stata.trk` has no `N parallel*.pkg`
      // install record either. Left blank rather than invented; PIET's row
      // for it still ships (see the K1 CommandSpec block), it just carries no
      // attribution.

      // AER tier-1 (2026-10-28). Read from each package's `ssc describe` record
      // on the oracle Mac (internal capture t1_ssc_install.log);
      // matselrc from the STB-56 dm79 package record (t1_matselrc_install.log);
      // require from the Authors block of its installed help file
      // (ado/plus/r/require.sthlp, version 1.3.1), which names both authors.
      // `astile` names xtile as its native equivalent: astile's own help says its
      // cut points are xtile's, and w2ax_t1_community.log arm astile_vs_xtile10
      // measures 0 disagreements.
      {"require", "Sergio Correia, Matthew P. Seay", ""},
      {"labvars", "Daniel Klein", ""},
      {"astile", "Attaullah Shah", "xtile"},
      {"matsort", "Paul Millar", ""},
      {"matselrc", "Nicholas J. Cox", ""},
      {"fillmissing", "Attaullah Shah", ""},
      {"findname", "Nicholas J. Cox", ""},
      {"sencode", "Roger Newson", ""},
      {"zipsave", "Henrik Stovring", ""},
      {"zipuse", "Henrik Stovring", ""},
  };
  return kNames;
}

const CommunityName* findCommunityName(const std::string& lowerName) {
  for (const auto& cn : communityNameTable()) {
    if (cn.name == lowerName) return &cn;
  }
  return nullptr;
}

std::string communityGateMessage(const CommunityName& cn) {
  std::string msg = "'" + cn.name + "' is a community-package compatibility name (package by " +
                    cn.author + ", not affiliated).";
  if (!cn.nativeEquivalent.empty()) {
    msg += " PIET's native command is '" + cn.nativeEquivalent + "'.";
  }
  msg += " Run: set extcompat on";
  return msg;
}

// ---------------------------------------------------------------------------
// AER scope-2 A1 — the lexical instrumentation dumps (see registry.h).
namespace {

// ASCII lowercase, matching eval/tokenizer.cpp's own toLower(cmd) convention —
// resolveCanonical() requires its argument already lowercased.
std::string lowerAscii(const std::string& in) {
  std::string out = in;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// The sort that makes these dumps pinnable: OptVocab's members are
// unordered_set, whose iteration order is unspecified.
std::vector<std::string> sortedNames(const std::unordered_set<std::string>& in) {
  std::vector<std::string> out(in.begin(), in.end());
  std::sort(out.begin(), out.end());
  return out;
}

// The THIRD expression-function layer. descriptive/data_listing.cpp's CiParser (the
// `count if` fast path) re-implements exactly these three names itself and
// throws CountFastBail on any other call, so it has no map to enumerate: this
// is a DECLARED list, read off CiParser there. A census naming only
// the two map-backed layers below could conclude a function is supported when
// the fast path silently bails on it, which is the blind spot this row exists
// to close.
const char* const kCiParserFns[] = {"inlist", "inrange", "missing"};

}  // namespace

std::string optVocabDumpJson() {
  nlohmann::ordered_json out = nlohmann::ordered_json::array();
  // registeredOptVocabCommands() walks a std::map, so it arrives sorted, and it
  // already drops the empty key registerOptVocab() hands to a net that declares
  // no command token of its own.
  for (const std::string& cmd : piet::eval::registeredOptVocabCommands()) {
    const piet::eval::OptVocab* v = piet::eval::findOptVocab(cmd);
    // Cannot miss — the name came out of the same registry this reads back —
    // but a null deref here would be a crash in a diagnostic tool.
    if (v == nullptr) continue;
    nlohmann::ordered_json row;
    row["command"] = cmd;
    row["known"] = sortedNames(v->known);
    row["unimplemented"] = sortedNames(v->unimplemented);
    std::vector<std::pair<std::string, std::string>> aliases(v->aliases.begin(),
                                                             v->aliases.end());
    std::sort(aliases.begin(), aliases.end());
    nlohmann::ordered_json aliasObj = nlohmann::ordered_json::object();
    for (const auto& kv : aliases) aliasObj[kv.first] = kv.second;
    row["aliases"] = std::move(aliasObj);
    out.push_back(std::move(row));
  }
  return out.dump();
}

int runDumpOptVocab() {
  std::printf("%s\n", optVocabDumpJson().c_str());
  return 0;
}

int runDumpOptVocabResolve(const std::string& commandIn, const std::string& optionIn) {
  // Stage 1 of dispatcher.cpp's sequence: THE command-name resolution point. A
  // community-package name deliberately comes back unchanged (resolveCanonical's
  // own exemption), which is why both spellings are reported.
  const std::string cmd = resolveCanonical(lowerAscii(commandIn));
  const piet::OptKeyResolution res = piet::resolveOptionKeyForCommand(cmd, optionIn);
  const piet::eval::OptVocab* v = piet::eval::findOptVocab(cmd);

  nlohmann::ordered_json out;
  out["command_in"] = commandIn;
  out["command"] = cmd;
  out["option_in"] = optionIn;
  out["resolved"] = res.resolved;
  out["stage"] = std::string(res.stage);
  out["exact_only"] = res.exactOnly;
  out["in_known"] = v != nullptr && v->known.count(res.resolved) != 0;
  out["in_unimplemented"] = v != nullptr && v->unimplemented.count(res.resolved) != 0;
  // vocab_registered=false is the UNMEASURED answer, never a gap: a command with
  // no registered vocabulary has no option census at all, so in_known and
  // in_unimplemented above read false for EVERY spelling, including the real
  // ones its handler goes on to read.
  out["vocab_registered"] = v != nullptr;
  std::printf("%s\n", out.dump().c_str());
  return 0;
}

std::string functionDumpJson() {
  std::vector<std::pair<std::string, std::string>> rows;  // (name, layer)

  // Both map-backed layers are read LIVE, the way the evaluator builds them:
  // mathNamespace() is pyeval's own registry and addStataFns()/addScalarFn() are
  // what exprfns adds on top of it, so diffing the two gives every name its
  // owning layer without transcribing either list. (The in-tree comments count
  // these differently — exprfns.cpp says 36, pyeval.h says 39 — which is exactly
  // why this reads the maps instead.)
  piet::eval::PyNamespace base = piet::eval::mathNamespace(/*withInfNan=*/false);
  for (const auto& kv : base.fns) rows.emplace_back(kv.first, "pyeval");

  piet::eval::PyNamespace full = base;
  piet::eval::addStataFns(full);
  // addScalarFn is a per-session addition because scalar() closes over a lookup
  // function, so stataMathNamespace() does NOT carry it; a census built from
  // that namespace alone under-reports the engine by one real function. The
  // lookup handed in here is never called — only the key is read.
  piet::eval::addScalarFn(full,
                          [](const std::string&) { return std::optional<std::string>(); });
  for (const auto& kv : full.fns) {
    if (base.fns.count(kv.first) == 0) rows.emplace_back(kv.first, "exprfns");
  }

  for (const char* n : kCiParserFns) rows.emplace_back(n, "ciparser");

  // B3(b): the literal pseudo-functions live in the LEXER (pyeval.cpp), not in
  // either map, because their argument is unquoted date text rather than an
  // expression. A census built from the maps alone reports them missing while
  // `local z = tq(2005q1)` answers 180, so they are declared here the way
  // kCiParserFns is -- read off cecon::literal_scale_for in data/stata_dates.h.
  for (const char* n : {"td", "tc", "tC", "tm", "tq", "tw", "th",
                        "d", "m", "q", "w", "h", "y"})
    rows.emplace_back(n, "literal");

  // B3(c): two more layers a map-based census cannot see. colnumb() is resolved
  // by the matrix-reference substitution in macros.cpp, beside colsof/rowsof;
  // I(), J(), nullmat() and vecdiag() are only legal inside a `matrix X = ...`
  // right-hand side, which is its own small grammar (matrix/matrix_expression.cpp). Publishing them with
  // their own layer names is the honest answer to "does the product understand
  // this name" -- it does, and the label says where.
  rows.emplace_back("colnumb", "matrixref");
  for (const char* n : {"I", "J", "nullmat", "vecdiag"}) rows.emplace_back(n, "matrixdef");

  // Sorted by (name, layer). Each map above already iterates in name order, but
  // the three CiParser names are DUPLICATES of exprfns names, and sorting by
  // name puts each duplicate pair adjacent — which is the fact this dump exists
  // to make visible.
  std::sort(rows.begin(), rows.end());

  nlohmann::ordered_json out = nlohmann::ordered_json::array();
  for (const auto& r : rows) {
    nlohmann::ordered_json row;
    row["name"] = r.first;
    row["layer"] = r.second;
    out.push_back(std::move(row));
  }
  return out.dump();
}

int runDumpFunctions() {
  std::printf("%s\n", functionDumpJson().c_str());
  return 0;
}

}  // namespace piet::registry
