// SPDX-License-Identifier: GPL-3.0-or-later
// Test selection and ordered execution. Cases live in functional selftest files.
#include "selftest_support.h"
#include "selftest_cases.h"

namespace piet::selftest {
const char* selftestSourceFile() { return __FILE__; }
}

// W2 scaffold: the per-lane selftest TUs (selftest_w2_*.cpp) report through this.
void w2check(bool cond, const std::string& label) { check(cond, label); }
void w2skip(const std::string& label, const std::string& why) { skip(label, why); }

namespace {

// A selected suite reuses the same test functions as the full run. Each
// case creates its own work directory and SessionState; none needs a prior
// full-suite case. Keep the full run below in its established order (the final
// float-store fallback test intentionally observes the whole run).
struct SuiteTest {
  const char* name;
  void (*run)();
};

const SuiteTest kTableTests[] = {
    {"testAerTables", testAerTables},
    {"testBS020EststoColonPrefix", testBS020EststoColonPrefix},
    {"testW1aTableOptionMinimums", testW1aTableOptionMinimums},
    {"testBUG160TableNestedMultistat", testBUG160TableNestedMultistat},
    {"testW2EsttabNames", testW2EsttabNames},
};

const SuiteTest kConversionTests[] = {
    {"testBS003DestringTostring", testBS003DestringTostring},
    {"testW2bDestring", testW2bDestring},
    {"testW2zTostring", testW2zTostring},
    {"testW2AhDataVerbs", testW2AhDataVerbs},
};

const SuiteTest kStoredEstimateTests[] = {
    {"testB4W3bEstimatesAndDataVerbs", testB4W3bEstimatesAndDataVerbs},
    {"testW2EststoPrefix", testW2EststoPrefix},
    {"testW2Esample", testW2Esample},
    {"testW2EsttabNames", testW2EsttabNames},
};

const SuiteTest kTimeDeclarationTests[] = {
    {"testC2BS030TssetDelta", testC2BS030TssetDelta},
    {"testXtsetPhysicalSortAndN", testXtsetPhysicalSortAndN},
    {"testW2XtsetPersist", testW2XtsetPersist},
    {"testW2Tsset", testW2Tsset},
};

struct SelftestSuite {
  const char* name;
  const SuiteTest* tests;
  size_t count;
};

const SelftestSuite kSelectedSuites[] = {
    {"tables", kTableTests, sizeof(kTableTests) / sizeof(kTableTests[0])},
    {"data-conversion", kConversionTests, sizeof(kConversionTests) / sizeof(kConversionTests[0])},
    {"stored-estimates", kStoredEstimateTests,
     sizeof(kStoredEstimateTests) / sizeof(kStoredEstimateTests[0])},
    {"time-declarations", kTimeDeclarationTests,
     sizeof(kTimeDeclarationTests) / sizeof(kTimeDeclarationTests[0])},
};

int finishSelftest() {
  std::printf("[selftest] ---------------------------------------------\n");
  if (g_skipped > 0) {
    std::printf("[selftest] %d CHECK(S) SKIPPED\n", g_skipped);
  }
  if (g_failures == 0) {
    std::printf("[selftest] ALL CHECKS PASSED\n");
    return 0;
  }
  std::printf("[selftest] %d CHECK(S) FAILED\n", g_failures);
  return 1;
}

}  // namespace

void listSelftestSuites() {
  std::printf("all (default): complete regression suite\n");
  for (const auto& suite : kSelectedSuites) {
    std::printf("%s: %zu test functions\n", suite.name, suite.count);
    for (size_t i = 0; i < suite.count; ++i) {
      std::printf("  %s\n", suite.tests[i].name);
    }
  }
}

int runSelftestSuite(const std::string& name) {
  if (name == "all") return runSelftest();
  for (const auto& suite : kSelectedSuites) {
    if (name != suite.name) continue;
    // The full run's preceding gtools/compatibility tests leave community
    // names enabled. Prepare the same process-only prerequisite here instead
    // of inheriting a user's persisted extcompat=false setting.
    piet::settings::setExtcompatEnabled(true);
    std::printf("[selftest] suite %s: %zu test functions\n", suite.name, suite.count);
    for (size_t i = 0; i < suite.count; ++i) {
      std::printf("[selftest] RUN %s\n", suite.tests[i].name);
      suite.tests[i].run();
    }
    return finishSelftest();
  }
  std::fprintf(stderr, "Unknown selftest suite '%s'; use --list-suites.\n", name.c_str());
  return 2;
}

int runSelftest() {
  std::printf("[selftest] PIET 2.0 native M0+M1+M4b self-test starting\n");

  testBridgeEcho();
  testWindowsFileDropBridgeSeam();
  testProgressStream();
  testCobraRoundtrip();
  testEvaluatorSmoke();
  testBridgeRoutesM4b();
  testEstScalarRefsInExpressions();
  testAsyncRouteShimContract();
  testSplitByteRouteAndRowRange();
  testGetcolsProjection();
  testVirtualHeadSurvivesForeignMutation();
  testVersionSingleSource();
  testLegalNoticesRoute();
  testResolveUserPath();
  testCwdSeededAtStartup();
  testFileReadRoute();
  testCommandLayerStabilizationW3();
  testFuzzCampaignBugfixes();
  testW3FinalFixerBugfixes();
  // 2.2.0 UX/Settings wave (items B/D/E).
  testSettingsPersistence();
  testMemoryPreflight();
  // P8.1 Wave 2 (contract_B 2.4.0) native lane: file.save / var_detail /
  // ui_state / default_export_dir / live-block streaming / BUG-082.
  testP81Wave2NativeLane();
  testScratchSupersededDelete();
  testWaveRegressions20260902();
  testRunSelectionRoute();
  // COUNCIL-20260710 pre-wave sprint P2/P3/P4 (BUG-055/BUG-056/BUG-060).
  testClusterVceAndUnknownOptionFixes();
  // Step 2 fix: BUG-085 tempfile->save->use checkpoint idiom (SE-044/045).
  testTempfileSaveUseRoundtripBug085();
  testTempfileReapedOnProgramReturnBug360();
  testRenameFoldsItsCardPhase1();
  testConfirmFileSeesVirtualSaveBug361();
  // FLOOR C4-L2B: use-render/expandVarlist/describe card plane + escape parity.
  testC4L2BRenderPlaneGates();
  // BETA campaign Phase A0: registry <-> dispatcher route drift guard.
  testRegistryRoutes();
  // AER scope-1: effect-table coverage + no-data route ratchets.
  testAerCampaignRatchets();
  // AER scope-1 bundle D (ENV): shell translation, accept-notes, findfile.
  testAerEnvBundle();
  // AER scope-1 bundle A: abbreviations reach the same handler; ivreg == 2sls;
  // winsor is the order-statistic clamp, not winsor2's interpolated percentile.
  testAerAbbrevBundle();
  // AER scope-1 bundle B: r() goes immediate at the top level and stays staged inside
  // a program; the svmat <-> mkmat round trip; matlist renders stubs, not just numbers.
  // AER scope-1 bundle H PREREQUISITE: the e()/r() matrix layer. Measured 2026-09-05 to
  // be narrower than the plan assumed -- e-matrices were write-only beyond b/V and r-class
  // matrices did not exist -- so pca/sktest would have published returns nothing could read.
  testMatrixClassLayer();
  // AER scope-1 bundle H (ESTIM): the alias estimators, the four closed-form
  // statistics against StataBE oracles, xi both ways, and the eleven E3 refusals.
  testAerEstimBundle();
  testAerResultsBundle();
  // AER scope-1 bundle C: the gtools/ftools aliases return exactly what their native
  // commands return; a speed option is dropped, a spelling option is renamed, an
  // answer-changing option is refused at r(199).
  testAerGtoolsBundle();
  // AER scope-1 bundle E (TABLES): the estadd -> eststo -> estout|esttab-using
  // pipeline leaves a FILE behind whose bytes carry the addition; outreg2's .txt
  // ledger is what `append` re-reads; the two refusals refuse having written nothing.
  testAerTables();
  // AER scope-1 bundle F (RESHAPE): the shape/sample verbs assert VALUES out of the
  // frame they produced -- one percentile rule shared with summarize,detail; a
  // cluster bootstrap that draws whole clusters; a seeded sample that is
  // reproducible and is not the head of the file; a recast that does not move the
  // frame; notes that survive a .dta round trip.
  testAerReshape();
  // WS-B (extcompat): community-name gate + table drift + `set extcompat`.
  testExtcompatGate();
  // P1-W2-0-LABELBOOK-BUG102: `labelbook` direct-read report.
  testLabelbook();
  // P1-W2-0-LABELBOOK-BUG102 (rider): coeftable factor-level value-label sub.
  testBug102CoeftableValueLabels();
  // BUG-105 stopgap: the 5 hand-added command abbreviations (l/su/d/ta/cor).
  testBUG105AbbreviationStopgap();
  // BUG-105 secondary (1): unsupported_command names its own cause honestly.
  testBUG105UnsupportedCommandHonesty();
  testBUG105InQualPrescan();
  // BETA campaign Phase A1: unified prefix engine (BS-002/014/015/016/017/020/040/041).
  testBS016CaptureAbbreviations();
  testBS017ByBysortAbbreviations();
  testBS002BysortParentheticalSort();
  testBS020EststoColonPrefix();
  testBS040QuietlyNoisilyInnerWins();
  testBS041VersionColonPrefix();
  testBS014ProgramBareFormsPrepass();
  testBS015CaptureMultilineBlock();
  testBUG116NestedCaptureBlock();
  testBUG116NestedCaptureSingleLine();
  testBUG132QuietlyNoisilyBlock();
  testBug344SummarizeSum();
  testBug341VarSubscript();
  testBug343FileFamily();
  testP9ParallelBlock();
  testP9AutoRecognizer();
  testBUG167BraceInsidePrefixedArgs();
  testBUG154ExitInBlock();
  testP1XlsxLayer();
  // AER scope-2 W3a: the spreadsheet/delimited option cluster, and the
  // rels-Target seeded-RED that separates "found a worksheet" from "found
  // THE worksheet".
  testW3aSpreadsheetOptions();
  // AER scope-1 bundle G (IO-TEXT): the old text I/O verbs assert BYTES, not return
  // codes -- outsheet's tab default, outfile's absent header and its quotes,
  // infix's 1-based inclusive columns, infile's line-break-blind record wrap,
  // saveold's literal honesty echo -- and putexcel goes out through the cell
  // writer and back in through xlsx::read. Sits beside testP1XlsxLayer because it
  // is the second consumer of that same layer.
  testAerIoText();
  // AER scope-1 bundle I (UTIL): the data-tidying utilities assert hand-computed
  // VALUES off small frames, never a return code -- unique counts missing as a value
  // where distinct does not, missings tag writes a rowmiss COUNT, carryforward resets
  // at the group boundary and lands in ORIGINAL row order, rangestat's window differs
  // from the by-group aggregate on four of six rows, tsspell breaks a run at a time
  // gap, renvarlab validates every target before it moves one, and marksample works
  // only inside a program.
  testAerUtilCounts();
  testAerUtilRenameLabel();
  testAerUtilFillers();
  testAerUtilRefusals();
  // AER scope-1 bundle J (GRAPH-X): every wrong implementation in this bundle returns
  // rc 0 and draws a picture, so no leg stops at a return code -- grc1leg2 keeps exactly
  // ONE legend, geodist's Seoul-Busan distance is pinned to a tolerance that fails a
  // haversine default, the ECDF's last point is exactly 1.0, event_plot's surviving
  // coefficients are asserted in relative-time order off a deliberately scrambled
  // regression, and grstyle's accept-note is pinned byte for byte.
  testAerGraphX();
  // AER scope-2 A1: the two lexical dumps -- the option-vocabulary census stays
  // in step with the live registry, geonear's declared alias survives the
  // serializer, and the function census names all three evaluator layers.
  testAerA1VocabDumps();
  testAerW2OptVocabTwoWay();
  // AER scope-2 B4: the abbreviation FLOOR — the narrowing companion to W2's
  // widening sweep, and the fix for the `re` -> `replace` file-destroying
  // regression W2 shipped. Every leg cites the StataBE line that settles it.
  testW1bVceAbbrev();
  testAerB4AbbrevFloor();
  // AER scope-2 B4 P0-C: ivreg2 certified `robust` and `small` while dropping
  // both. The assertion is the standard ERROR, and the discriminator is that
  // the three arms must differ — an rc-only test passes on the broken build.
  testAerB4Ivreg2Inference();
  // AER scope-2 bundle K1: three renamers whose rules were measured before they
  // were written -- and three legs that are DISCRIMINATORS, not smoke tests:
  // `move a c` separates the direction-dependent implementation from the
  // before()-always one, bare `tolower` over a mixed dataset proves the
  // non-atomic partial rename, and `postsub("_" "")` proves the end-anchored
  // substitution that a substitute-everywhere reading would silently get wrong.
  testAerK1();
  // AER scope-2 bundle B2: strdist. Byte-wise Levenshtein, measured before it
  // was written -- the `a`/`A` leg separates it from a case-folded copy, the
  // maxdist leg proves it BLANKS rather than clamps, and the one-argument leg
  // pins a form the help file does not spell out.
  testAerB2Strdist();
  testAerB2Oaxaca();
  testAerB2Nnmatch();
  testAerB2Bcal();
  // BUG-450: the varlist expansion point. Two of its legs are GUARDS -- the
  // rename-groups form must reach its arm unexpanded, and a pattern-free
  // varlist must take the unchanged path.
  testVarlistExpansionWiring();
  // AER scope-2 K1c: `parallel`, answered sequentially. Its rules come from the
  // package's own source, because the package cannot be RUN here (setclusters
  // is r(601) even with statapath()) -- so the two forms whose answer depends
  // on the data split are refused rather than approximated.
  testAerK1cParallel();
  // BETA campaign Phase A2: option consume-or-reject net (BS-097) + do/run
  // `, nostop` option parse (BS-018).
  testBS097OptionNetAndBS018DoNostop();
  testW1aTableOptionMinimums();
  testSuccessfulReg3TestLincomResetRc();
  // BETA campaign Phase A3: registry-driven qualifier policy table
  // (BS-096) + D7 `in` mapping.
  testBS096QualifierPolicyTable();
  // P3 weights wave: registry WeightPolicy expansion (None/FweightOnly ->
  // +AweightFweight/+PweightOnly/+AllWeights) — regress/reghdfe/feols now
  // AllWeights, summarize AweightFweight, new mean/proportion rows
  // PweightOnly.
  testP3WeightsWave();
  // BETA campaign Phase A4: context spine — lastModel vcov/cluster/fe
  // inheritance (BS-013/098), rc coarse map (BS-100), c()/set registry
  // (BS-046/047/048/091/101).
  testBS013VceInheritancePostEstimation();
  testBS013EstatDiagFeHonestReject();
  testFIXRestimateUnsetRAndLincomFill();
  testBS100RcCoarseMap();
  testBS091CClassRegistryInExpressions();
  testBS046To048SetRegistry();
  // BETA campaign Phase B (B-data cluster): analysis.do musts — sort real
  // dispatch (BS-001), destring/tostring (BS-003/D4), joinby (BS-004/D5),
  // format display-layer attribute (BS-005/D6).
  testBS001SortRealDispatch();
  testBS003DestringTostring();
  testBUG191ResidentRenameKeepsValues();
  testBS004JoinbyInnerKeyJoin();
  // L1b: BUG-127 — `predict <type> newvar` storage-type-prefix misparse.
  testBUG127PredictTypePrefix();
  // P2 wave: BUG-155 — `predict, xb` appended an unrequested second column and
  // silently overwrote a user variable named `resid` (RED cert arm 1).
  testBUG155PredictOnlyRequestedColumn();
  testBUG1939DevTreeRootResolution();
  testBS005FormatDisplayLayer();
  // BETA campaign Phase B (B-log cluster): `log using`/`log close` real
  // file write (BUG-028/BS-006).
  testBS006LogUsingRealFileWrite();
  // BETA campaign Phase B (B-graph part A cluster): title/subtitle/xtitle/
  // ytitle dequote (BS-008), note() wiring (BS-009), bare-form name()
  // registration (BS-010), graph combine name(X, replace) suffix fix (BS-057).
  testBS008To057GraphTitleNoteName();
  // BETA campaign Phase B (B-graph part B cluster): graph combine on-disk
  // .gph file-path resolution (BS-007/D3), coefplot/marginsplot ->
  // lastGraph/namedGraphs registration (BS-011), graph export as(fmt)
  // parse + honest PNG/PDF fallback wording (BS-012/D8).
  testBS007To012GraphCombineExportRegister();
  // BETA campaign Phase C batch C1: silent-wrong sweep — estimation/summary
  // adapters (BS-022/024/025/026/027/028/029) + spine defects (N1/N3).
  testC1N1CaptureNoisilyAbbrev();
  testC1N3CoarseRcGaps();
  testC1BS022LogisticOddsRatio();
  testC1BS024HettestVarlist();
  testC1BS025CodebookIfWiring();
  testC1BS026TabstatAdapter();
  testC1BS027TabulateTwowaySummarize();
  testC1BS028PwcorrObs();
  testC1BS029LevelsofSeparateMissing();
  // BETA campaign Phase C batch C2: data-verb integrity (BS-030 tsset/xtset
  // delta, BS-031 silent-if/in-drop sweep, BS-032 egen group/tag+by reject,
  // corr obs field composing with C1/BS-028).
  testC2BS030TssetDelta();
  testXtsetPhysicalSortAndN();
  testC2BS031SilentIfDropSweep();
  testC2BS032EgenGroupTagByReject();
  testC2CorrObsFlagComposing();
  // BETA campaign Phase C batch C3: macro/program parity (BS-033 gettoken
  // 2-arg, BS-034 loop header forms, BS-035 local postfix literal, BS-036
  // do/run echo, BS-037 tempvar, BS-038 scalar list format, BS-039 ereturn
  // list).
  testC3BS033GettokenTwoArg();
  testC3BS034LoopHeaderForms();
  testC3BS035LocalPostfixLiteral();
  testC3BS036DoRunEcho();
  testC3BS037Tempvar();
  // BUG-176/177 (P2-PREP): the same two matchers, reached through a macro,
  // plus encode's full `generate()` spelling.
  testP2PrepMacroIndirectedDecls();
  // BUG-180 (P2-PREP): `rmdir` — mkdir's missing teardown twin.
  testP2PrepRmdir();
  testC3BS038ScalarListFormat();
  testC3BS039EreturnList();
  // BETA campaign Phase C batch C4: session/other family (BS-042 confirm
  // silent-success family, BS-043 confirm integer number misparse, BS-044
  // clear subcmd scoping, BS-045 rm/mkdir cleanup, BS-124 timer).
  testC4BS042ConfirmFamily();
  testC4BS043ConfirmIntegerNumber();
  testC4BS044ClearScoping();
  testC4BS045RmMkdir();
  testC4BS124Timer();
  // BETA campaign Phase D batch D1 (IV & systems wirings): BS-063 ivregress
  // 2sls/gmm + ivreghdfe/reghdfe-paren-form, BS-064 ivregress liml, BS-065
  // reg3, BS-066 boottest, BS-067 vce(cluster A B) -> twoway, BS-068
  // reghdfe keepsingletons.
  testD1BS063IvRegress();
  testD1BS064Liml();
  testD1BS065Reg3();
  testD1BS067TwowayCluster();
  testD1BS068Keepsingletons();
  testD1BS066Boottest();
  // BETA campaign Phase D batch D2 (MLE-tier verb wirings): BS-069 tobit
  // ll()/ul(), BS-070 heckman two-step, BS-071 qreg quantile()/level(),
  // BS-072 mediate Stata-18 continuous(#L0 #L1) (engine-parity, not
  // Stata-parity — see runMediate's header comment).
  testD2BS069Tobit();
  testD2BS070Heckman();
  testD2BS071Qreg();
  testD2BS072Mediate();
  // BETA campaign Phase D batch D3 (panel/count wirings): BS-073
  // poisson/ppmlhdfe/fepois, BS-074 nbreg (engine convergence blocker
  // disclosed, wiring proven against a synthetic dataset), BS-075
  // xtpoisson/xtlogit ,fe, BS-076 drdid (own handler over the cobra csdid
  // estimator, which it leaves unmodified).
  testD3BS073Poisson();
  testD3BS074Nbreg();
  testD3BS075XtPoisXtLogit();
  testD3BS076Drdid();
  // BETA campaign Phase D batch D4 (time-series wirings): BS-077 newey,
  // BS-078 prais, BS-079 dfuller/pperron, BS-080 wntestq, BS-081 tsfill.
  testD4BS077Newey();
  testD4BS078Prais();
  testD4BS079DfullerPperron();
  testD4BS080Wntestq();
  testD4BS081Tsfill();
  // BETA campaign Phase D batch D5 (post-estimation/diag/misc wirings):
  // BS-085 sdtest, BS-021 estat durbinalt fix (was mis-aliased to
  // bgodfrey), BS-082 estat endogenous/overid/firststage IV diagnostics,
  // BS-086 margins at()/dydx() context forward (discovered + fixed a
  // deeper engine-silent-wrong gap: --at ignores --model entirely), BS-083
  // hausman, BS-084 chow (PIET convenience wrapper), BS-087(part) mkspline
  // (cubic only; nknots() engine bug disclosed, flagged for Auto Patcher).
  testD5BS085Sdtest();
  testD5BS021Durbinalt();
  testD5BS082IvDiag();
  testD5BS086MarginsAt();
  testD5BS083Hausman();
  testD5BS084Chow();
  testD5BS087Mkspline();
  // BETA campaign Phase E (graphics batch, native geometry/options side):
  // BUG-099 lfit/qfit real fits, BS-049 kdensity kernel()/bandwidth/grid,
  // BS-050 multi-y scatter, BS-052 by() honest reject, BS-053 `||`
  // separator, BS-054 twoway kdensity/histogram prefix, BS-055 xtline/
  // tsline SVG export real data, BS-056 graph drop multi-name/_all,
  // BUG-103 TMPDIR-dotted-path as(png) fallback.
  testBetaEGraphicsBatch();
  // Graph-honesty campaign: BUG-129 twoway unsupported-layer reject-before-
  // output (10-kind silent-drop class) + BUG-130 bare-form rarea band builder.
  testBUG129And130GraphHonesty();
  // BETA campaign Phase G: `in a/b` row-range expansion beyond
  // list/browse/gen/replace (BS-090) + real `by:`/`bysort: summarize`
  // grouped dispatch (BS-092).
  testBS090InRangeExpansion();
  testBS092BysortSummarize();
  // FLOOR/A12: buildVarInfo installs a non-owning level-dictionary VIEW instead
  // of deep-copying every factor level on every card build (levelStringCopies
  // mechanism gate + value-label copy-on-write).
  testLevelsViewNoCopy();
  // BETA campaign Phase G, SCOPE.md regression-lock rows (test-only, no
  // functional code change): BS-060/BS-061/BS-062/BS-088/BS-094.
  testG_BS060MeanonlyStrmutateLock();
  testG_BS061LockLegs();
  testG_BS062ConfirmTypeCompoundLock();
  testG_BS088TsOperatorInjectionLock();
  testG_BUG369TsInExpressionLock();
  testG_BUG370ResidualWireLock();
  testG_BUG371FeatureWaveLock();
  testG_BUG373DeclaredReturnsLock();
  testG_BUG374DisplayLayerLock();
  testG_BS094ModNativeEvaluatorLock();
  // BUG-115: if/in/weight qualifier scanning must be quote-aware (a quoted
  // literal's text is never a real qualifier).
  testBUG115QuoteAwareQualifierLock();

  // B4/W1: Stata takes ONE `if`/`in` qualifier AFTER the options, and the two
  // rejects around it carry DIFFERENT messages (that difference is the spec).
  testB4W1TrailingQualifier();

  // B4/W3b: `estimates store/save/use` (BUG-470's round trip) and the small
  // data verbs, each against its captured Stata cell.
  testB4W3bEstimatesAndDataVerbs();

  // BUG-133: SIGTERM/SIGKILL-orphaned scratch dirs are reaped on the next launch
  // (dead owner-pid marker + settled mtime), live/unmarked/fresh dirs preserved.
  testBUG133OrphanScratchReap();

  // BUG-138: `save` over the file `use` loaded must leave that file intact
  // (sibling-temp + rename, `dest` never opened for write).
  testBUG138SaveOverSource();

  // BUG-139: a clean `exit`/`exit N` must report an honest process exit
  // code instead of the same rc=1 a real engine error gets.
  testBUG139CleanExitCode();

  // BUG-146: `merge`'s cardinality-violation hints must name only remedies
  // this build's dispatcher accepts (the old text recommended `m:m`, which it
  // rejects as a syntax error).
  testBUG146MergeCardinalityHintHonesty();

  // BUG-105 secondary: `hbin write failed` internal jargon leak (5 defensive-
  // assert sites) + `collapse`'s parse-error message missing expected syntax.
  testBUG105ErrHonestyLeaks();

  // FLOOR campaign (stint 05a): slot-compiled evalRows — the compile-once,
  // execute-per-row fast path for strmutate's row evaluator (was 42% of PIET
  // main-thread CPU). Mechanism gate: zero per-row map writes on the slot path.
  testFloorSlotCompiledEvalRows();

  // BUG-105 옵션망: merge/reshape/order residual-option honest-reject
  // (keepusing/nogenerate/keep/assert · i/j · first/last/before/after known;
  // any other token is a named reject, not a silent no-op).
  testBUG105OptNetDataVerbs();

  // BUG-112(1) PIET-half: `generate` over an existing variable is a named
  // reject (r(110)) on ALL four sinks — plain, `_n`/`_N`, `in`-range and
  // `bysort v:` — while `replace` keeps overwriting.
  testBUG112PietGenGuard();
  testBUG112WarningsSurface();
  testBUG331Offset();
  testBUG351KeepIfLevels();
  testBUG352CompoundQuotes();
  testBUG353CorrRClass();
  testBUG354TtestTabulateRClass();
  testBUG355TabulateValueLabels();
  testBUG350SwallowedErrors();
  testCardFoldScratchDrop();
  testJoinbyCardEntrance();

  // BUG-112(7): doexec::executeDo's per-run tempdir reap is RAII'd so it
  // fires even when executeDoSource throws (was: plain code after the
  // depth-guarded block, so a throw skipped it and leaked the tempdir).
  testBUG112DoexecRaiiReap();

  // W:P8 / BUG-169: `graph export, name(X)` silently ignored name() and always
  // wrote session.lastGraph. The rc pins in pangram S19 catch the unknown-name
  // half (rc 693 now, rc 0 + a file before); this catches the half no rc can
  // see — that two DIFFERENT named graphs must export to different bytes.
  testP8GraphExportByName();

  // W:P8: `export delimited`'s cell spellings, measured against Stata's own
  // export of the same data — integral values carry no `.0`, a system missing
  // is an EMPTY field, an extended missing keeps its code, and a %td column
  // exports as its formatted date.
  testP8ExportDelimitedSpelling();

  // FULL FUNCTIONAL CHECK: every DECLARED prefix must actually reach a handler.
  // This is what stops the prefix registry from becoming a hand-kept list that
  // silently goes stale — the same compensating control the command table's own
  // route-drift guard provides.
  testPrefixRegistryRoutes();

  // W:P8 / BUG-174: a relative `do FILE` resolves against the SESSION cwd
  // (what `cd` sets), not the process cwd — the split that made a do-file
  // unable to call another do-file sitting beside it in a GUI session.
  testP8DoRelativeToSessionCwd();

  // BUG-195/196: one Stata numeric-format renderer, exercised over BOTH call
  // paths (display AND string()/tostring) against the persisted StataBE
  // capture under internal capture.
  testBUG195FormatOracleBothPaths();

  // BUG-198: a native `.hbin` save carries the session's display-format and
  // variable-label layer, and `use` reads it back (BUG-190's three-check family
  // on the persistence side, plus the absent-!=-empty old-file guard).
  testBUG198HbinMetaCarriage();

  // BUG-203: lincom/nlcom/test answer the model the user ACTUALLY fitted (or
  // honestly refuse), instead of silently serving an OLS refit.
  testBUG203PostestReadsEstimator();

  // BUG-208: `: variable label x` reads the LIVE label store, so it agrees with
  // `describe` by construction instead of answering "" off a map only the evalcase
  // harness ever wrote.
  testBUG208VariableLabelExtMacro();

  // BUG-160: `table`'s nested multi-statistic layout — ONE table_result with
  // Total row/column margins and value-label-aware headers, for one
  // statistic or several alike.
  testBUG160TableNestedMultistat();

  // BUG-216/217: an extended macro function answers or errors — never echoes its
  // own source text; the r(101) fall-through is generic, so this guards every
  // unimplemented `: fn`, not only the ones named in the entry.
  testBUG216ExtMacroFnRejects();

  // BUG-105: the abbreviation arc — ONE command-name resolution point, declared
  // spelling sets measured live against StataBE, and the two removals.
  testBUG105AbbrevArc();

  // BUG-251/252: no site re-derives the command word from the raw line; the
  // one-character line `u` returns an honest error instead of killing the run.
  testBUG251RawCmdRederivation();

  // BUG-202: nbreg/tobit/heckman/xtpois --level wiring, cross-checked directly
  // against the raw cobra CLI (finite ordered CI at 95%, strictly narrower at
  // 90% — rules out a cosmetic no-op).
  testBUG202LevelCiFamily();

  // BUG-233: the residual of BUG-231 — every option net's vocabulary registered
  // at ONE resolution point, bidirectional per file, plus the regrowth guard.
  testBUG233OptNetVocabularySweep();

  // BUG-274 residual + BUG-263: the three level() reject-to-forward conversions
  // (nbreg / xtpoisson,fe / heckman) with xtlogit's kept refusal, and the
  // mediate treatment sub-block's option net (second-source vector, own
  // canonicalization point).
  testBUG274MediateWiringResiduals();

  // BUG-297: predict after logit/probit/poisson writes the model's OWN
  // prediction (range-pinned per BUG-204's blindness note), refusal narrowed.
  testBUG297PredictGlm();

  // BUG-246..250 (WS-V): the expression/macro contract — unevaluable `= expr`
  // errors with Stata's code, `global :` dispatches, applyStataFmt speaks
  // flags and %tc/%td, string()'s residual rows, and the storage-subtype plane.
  testBUG246to250ExprMacroContract();

  // BUG-291/293/294/295 (ONESHOT-ZERO WS-N): the WS-V residue — varname-plane
  // hygiene on drop/keep, the display arm's own 0-flag/%t/quoted-arg/rc gaps,
  // undefined-macro expansion, and the .dta implicit-float claim.
  testBUG291to295WsvResiduals();

  // BUG-296 + BUG-292 (ONESHOT-ZERO WS-R): the comma/grouped format taxonomy,
  // the %t calendars beyond tc/td, detail-coded %td, the `f` family's overflow
  // arm, and the setcol transport's quoted-cell whitespace.
  testBUG296FormatTaxonomy();

  // BUG-272 (closes BUG-200): the sort-state plane — <sortlist>/.hbin "sorted_by"
  // carriage, gsort minting, and the adversarial stale-claim leg.
  testBUG272SortStatePlane();

  // BUG-483/484: a `continue` with no loop around it ended the do-file in
  // silence at exit 0, and `tabulate` on a missing column threw past `capture`
  // to main(), which replaced the whole run's blocks with one error.
  testControlFlowOutOfContext();

  // W2 (2026-09-19): one TU per lane -- see selftest_w2.h.
  testW2MacroN();
  testW2PredictFrame();
  testW2EststoPrefix();
  testW2LabelGlued();
  testW2DoInclude();
  testW2ByKeep();
  testW2EstimOpts();
  testW2Matrix();
  testW2Getmata();
  testR150Ivreg2Surface();
  testR150Xtivreg2Fe();
  testW2Loops();
  testW2ProgCtl();
  testW2MergeAppend();
  testW2EsttabNames();
  testW2BsBootstrap();
  testW2EgenRename();
  testW2Esample();
  testW2bLinearFe();
  testW2bJoinReshape();
  testW2bStrlWriter();
  testW2bDelimited();
  testW2bDataOpts();
  testW2bIvFamily();
  testW2bIv();
  testW2bDescXtset();
  testW2bPredictStats();
  testW2bEsttabScreen();
  testW2bEgenOpts();
  testW2bGlm();
  testW2bCluster3();
  testW2bMargins();
  testW2bMisc();
  testW2cMacroFns();
  testW2cDataFns();
  testW2bDestring();
  testW2XtsetPersist();
  testW2Tsset();
  testW2dInRange();
  testW2cPostest();
  testW2Ml();
  testW2dEreturn();
  testW2E3Swindex();
  testW2E3Qreg2();
  testW2E3Lasso();
  testW2E3Suest();
  testW2E3Ritest();
  testW2E3Acreg();
  testW2T1Zip();
  testW2T1Env();
  testW2T1Meta();
  testW2T1Gen();
  testW2T1Matrix();
  testW2xFnString();
  testW2zTostring();
  testW2Lpoly();
  testW2Ebmat();
  testW2GfxBinscatter();
  testW2GfxBar();
  testW2apEgen();
  testW2aqHistogram();
  testW2T7Lookfor();
  testW2T7Ihstrans();
  testW2T7Sctorezone();
  testW2LbLeebounds();
  testW2T7ClearAll();
  testW2UniCore();
  testW2UniV();
  testW2UniM();
  testW2UniD();
  testW2UniW();
  testW2C1067();
  { void testW2yDatetime(); testW2yDatetime(); }  // selftest_w2y_datetime.cpp (goal ge2-fn-uncovered-datetime)
  testW2DisplayFormat();
  testW2afFormat();
  testW2AhDataVerbs();
  testW2iVarsub();
  { void testW2eFloat(); testW2eFloat(); }  // selftest_w2e_float.cpp (BUG-471 T2); last, so its fallback count covers the suite

  return finishSelftest();
}
