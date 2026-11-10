// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Ordered command orchestration and outer result/log lifecycle.
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
#include "piet/handlers/estimation.h"
#include "piet/handlers/results.h"
namespace piet {
using namespace dispatch_detail;
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
  if (interceptControlBlocks(session, echo, blocks, rawCmd)) return blocks;

  if (interceptDoRun(session, echo, blocks, rawCmd)) return blocks;

  if (interceptOutputPrefixes(session, echo, blocks, rawCmd)) return blocks;

  if (interceptEstimatePrefix(session, echo, blocks, rawCmd)) return blocks;

  if (interceptVersionPrefix(session, echo, blocks, rawCmd)) return blocks;

  if (interceptSurveyPrefix(session, blocks, rawCmd)) return blocks;

  if (interceptProgramCompletion(session, echo, blocks, rawCmd)) return blocks;

  if (interceptArguments(session, blocks, rawCmd)) return blocks;

  if (interceptArgumentMacros(session, echo, blocks, rawCmd)) return blocks;

  if (interceptShellEscape(session, blocks, rawCmd)) return blocks;

  if (interceptMacroDeclarations(session, echo, blocks, rawCmd)) return blocks;

  if (interceptProgramReturn(session, blocks, rawCmd)) return blocks;

  if (interceptProgramDefinition(session, echo, blocks, rawCmd)) return blocks;

  if (interceptInputBlock(session, echo, blocks, rawCmd)) return blocks;

  if (interceptMata(session, blocks, rawCmd)) return blocks;

  if (interceptParallelPrefix(session, blocks, rawCmd)) return blocks;

  if (interceptDefinition(session, echo, blocks, rawCmd)) return blocks;

  if (interceptResampling(session, blocks, rawCmd)) return blocks;

  if (interceptRandomization(session, blocks, rawCmd)) return blocks;



  // ------------------------------------------------------------------
  // 0b. Session-state layer: expand macro / r() references in the raw line.
  std::string cmdForDispatch = rawCmd;

  // EM3a: the expanded line with every macval() value still encoded -- what
  // tokenize()'s unexpanded-macro net must read (it decodes right after the net).
  // Every other consumer below reads cmdForDispatch, the real characters.
  std::string cmdVerbatim;

  bool haveVerbatim = false;

  // BUG-1527: the expanded line with its compound quotes as typed, when the fold
  // below changed it ("" = cmdForDispatch): graph describe's r(command) is the
  // command as typed (StataBE 19.5: `title(`"T cq"')` stays compound). BUG-1049:
  // a program's `0' is taken from it too (Stata binds `0' to the unparsed text,
  // so `echo0 `"p"'' sees `"p"', not the "p" every other consumer reads).
  std::string lineAsTyped;
  if (expandCommandMacros(session, echo, blocks, rawCmd, cmdForDispatch, cmdVerbatim, lineAsTyped, haveVerbatim)) return blocks;

  if (finishVerbatimLine(blocks, cmdForDispatch, cmdVerbatim, lineAsTyped, haveVerbatim)) return blocks;



  // ------------------------------------------------------------------
  // 1. Tokenize (on the macro-expanded line).
  eval::ParsedCommand parsed;
  if (interceptXiPrefix(session, blocks, cmdForDispatch)) return blocks;



  std::string cmd;

  std::string cmdTyped;
  // BUG-105/WS-K: the spelling the USER typed, kept for
                         // the extcompat gate, the `display` raw-argument scan
                         // and every message that echoes the command word.
  std::optional<RcHiddenScope> rcHidden;
  if (tokenizeCommand(cmd, cmdTyped, parsed, session, blocks, cmdForDispatch, cmdVerbatim, rcHidden)) return blocks;



  // ATTR-20260812: the one place a command name becomes canonical, so the one
  // place a per-item timing row can learn what it is actually running. The
  // executor bills by the item's leading token, which makes `capture noisily
  // regress` a `capture` row and files `reg`/`regress` apart; prefixes are
  // stripped upstream of here, so the FIRST resolution inside an item is the
  // real verb. No-op unless PIET_EMIT_TIMING >= 2.
  piet::counters::noteResolvedVerb(cmd);

  // e(wexp) as Stata keeps it (session_state.h commandWexp / LastModel::fitWexp).
  session.commandWexp = parsed.weightExp.value_or(std::string());

  // BUG-1527: the statement a graph's describe record keeps (graphMetaFor trims it).
  const std::string& graphLineTyped = lineAsTyped.empty() ? parsed.raw : lineAsTyped;

  QualifierColumnGuard conditionMask{session, {}}
;
  if (foldCommandCondition(cmd, parsed, session, blocks, conditionMask)) return blocks;

  if (noteConditionProfile(cmd, parsed, blocks)) return blocks;

  if (resolveCommandOptions(cmd, parsed, blocks)) return blocks;

  if (checkCommunityCommand(cmdTyped, blocks)) return blocks;


  // Everything from here to whichever `return` fires is the handler zone; the
  // RAII scope closes on any of them.
  dprof::Scope __zHandler(dprof::kHandler);
  if (normalizeGtoolsAlias(cmd, parsed, session, blocks)) return blocks;

  if (executeByPrefix(cmd, parsed, session, blocks, conditionMask)) return blocks;

  if (resolveProgramCommand(cmd, parsed, session, echo, blocks, cmdForDispatch, lineAsTyped)) return blocks;

  bool needsNoData = commandNeedsNoData(cmd);

  if (countWithoutData(cmd, parsed, session, blocks)) return blocks;

  if (assertWithoutData(cmd, parsed, session, blocks)) return blocks;


  // FU-1252: after `clear`, `generate` creates a 0-observation variable in
  // Stata (w2ar `clear` + `generate double a = .` + `save ..., emptyok`, k=2
  // N=0, log:4506-4509; `generate byte z = .`, k=1 N=0, log:4552). The empty
  // dataset is the one `set obs 0` materializes.
  // `save f, emptyok` with nothing in memory writes the same empty dataset as a
  // k=0 N=0 file (dx_save_079_emptyok, log:4520); without emptyok it stays
  // r(111) (dx_save_noemptyok, log:4535).
  bool saveEmptyOk = false;
  if (parseEmptySave(cmd, parsed, blocks, saveEmptyOk)) return blocks;

  if (allowEmptyGenerate(cmd, session, blocks, needsNoData, saveEmptyOk)) return blocks;

  if (validateEmptyDataset(cmd, parsed, session, blocks, needsNoData, saveEmptyOk)) return blocks;

  if (requireCommandData(cmd, parsed, session, blocks, needsNoData)) return blocks;

  if (checkCommandArity(cmd, cmdTyped, parsed, blocks)) return blocks;

  if (route_io(cmd, cmdTyped, parsed, session, blocks)) return blocks;
  if (route_describe(cmd, parsed, session, blocks)) return blocks;
  if (route_data_names(cmd, cmdTyped, parsed, session, blocks)) return blocks;
  if (route_data_generate(cmd, cmdTyped, parsed, session, blocks, conditionMask)) return blocks;
  if (route_estimation(cmd, cmdTyped, parsed, session, blocks, rawCmd, graphLineTyped)) return blocks;
  if (route_time(cmd, parsed, session, blocks)) return blocks;
  if (route_session(cmd, parsed, session, blocks)) return blocks;
  if (route_environment(cmd, cmdTyped, parsed, session, blocks)) return blocks;
  if (route_data_rows(cmd, cmdTyped, parsed, session, blocks, rawCmd)) return blocks;
  if (route_data_combine(cmd, parsed, session, blocks)) return blocks;
  if (route_results(cmd, cmdTyped, parsed, session, blocks, rawCmd)) return blocks;
  if (route_extensions(cmd, cmdTyped, parsed, session, blocks)) return blocks;
  if (route_tabulation(cmd, cmdTyped, parsed, session, blocks)) return blocks;
  if (route_postestimation(cmd, cmdTyped, parsed, session, blocks)) return blocks;
  if (route_graphics(cmd, cmdTyped, parsed, session, blocks, graphLineTyped, conditionMask)) return blocks;
  if (route_table_family(cmd, parsed, session, blocks)) return blocks;
  // Everything else: explicit piet_gap, never a silent wrong answer. This is
  // the arm the BUG-105 abbreviation gaps land on — the token simply never
  // resolved to a command table entry, which is what the message now says.
  blocks.push_back(ojson(eval::unsupportedCommand(
      parsed.cmd, std::nullopt, std::nullopt, "piet_gap",
      eval::UnsupportedCommandKind::kUnknownToken)));
  return blocks;
}

namespace {

// BUG-1497, BUG-1509, BUG-1512: what a command leaves in `_rc`, s() and r()
// besides its own results, one row per command. Stata's estimation and many
// r-class commands are ado files that run internal steps -- `capture` checks,
// sclass option parsers, r-class helpers -- so:
//  - `_rc` after a run without `capture` is what the ado's last internal capture
//    left: 0 after regress, logit, lincom, ...; 100 after every margins. A
//    built-in (summarize, tabulate, generate, ...) or an ado without such a
//    capture (reghdfe, test, predict, estat ic) leaves it;
//  - s() is emptied by the commands whose ado runs an sclass parser;
//  - r() is emptied by a failure that comes after such a step. For regress,
//    tabulate, lincom, test, testparm, ttest, ... every failure does (r(111),
//    r(198), r(2000), r(103), r(109), r(301), r(100) alike); logit, probit and
//    ivregress fail before it on a bad name or option and keep r(), and empty
//    it on an empty sample (r(2000)).
// Measured in StataBE 19.5 (scratch-laneA/agents/file-new-1/i4.do, i4b.do,
// i7.do, i8.do; scratch-laneA2/agents/rlife/side.do, side2.do, and side3.do,
// side4.do, tp.do over other forms and failures of the same commands): each
// command run `quietly` after a failed `capture confirm variable` (_rc 111) and
// an sclass program's `sreturn local foo`, each failure under `capture` after
// `summarize`.
// Not modelled: a failure that leaves an empty-sample count in r() (poisson, areg,
// xtreg fe on r(2000)/r(2001): r(N) 0), the internal leftovers some failures
// leave beside the emptied r() (reghdfe's r(fn), mean's r(is_svysum)), the
// data-dependent `_rc` of centile (7) and signrank (9, its `capture assert` of
// integer ranks), ttest's success, which depends on the form (rlife/side3.do:
// `ttest x1 == 3` _rc 0 keeping s(), `ttest x1 == x2` 0 emptying s(), `ttest x1,
// by(yb)` 198), and s() after a failure (side5.do: a failed logit or predict
// empties it, a failed regress, lincom or test keeps it). A command not listed
// leaves all three alone. A failed margins is its handler's (runMargins empties
// r() for the errors it shares with Stata).
constexpr int kAnyFailure = 0;  // clearsROn: every failure empties r()

struct AdoResidue {
  const char* cmd;             // canonical command; estat/estimates with the subcommand
  int rc;                      // `_rc` after a successful run; -1 leaves it
  bool clearsS;                // a successful run empties s()
  std::vector<int> clearsROn;  // the failure codes after which r() is empty
};

const AdoResidue* findAdoResidue(const std::string& cmd) {
  static const std::vector<AdoResidue> kTable = {
      {"regress", 0, true, {kAnyFailure}},
      {"logit", 0, true, {2000}},
      {"probit", 0, true, {2000}},
      {"poisson", 0, true, {}},
      {"nbreg", 0, true, {}},
      {"areg", 0, true, {111, 198}},
      {"reghdfe", -1, true, {kAnyFailure}},
      {"xtreg", 0, true, {198}},
      {"ivregress", 0, true, {2000}},
      {"ivreghdfe", -1, true, {}},
      {"tobit", 0, true, {}},
      {"qreg", 0, true, {}},
      {"glm", 0, true, {}},
      {"newey", 0, true, {}},
      {"prais", 0, true, {}},
      {"ologit", 0, true, {}},
      {"mlogit", 0, true, {}},
      {"reg3", 0, true, {111}},
      {"sureg", 0, true, {111}},
      {"dfuller", 0, true, {111}},
      {"pperron", 0, true, {111}},
      {"wntestq", -1, true, {111}},
      {"lincom", 0, true, {kAnyFailure}},
      {"nlcom", 0, true, {kAnyFailure}},
      {"test", -1, false, {kAnyFailure}},
      {"testparm", 0, true, {kAnyFailure}},
      {"margins", 100, true, {}},
      {"predict", -1, true, {}},
      {"estat", -1, false, {kAnyFailure}},
      {"estat hettest", 0, true, {}},
      {"estat ovtest", 0, true, {}},
      {"estat vif", 0, true, {}},
      {"estimates store", 0, false, {}},
      {"tabulate", -1, false, {kAnyFailure}},
      {"pwcorr", 0, true, {kAnyFailure}},
      {"ttest", -1, false, {kAnyFailure}},
      {"tabstat", 0, false, {kAnyFailure}},
      {"sktest", -1, true, {111}},
      {"centile", -1, false, {kAnyFailure}},
      {"signrank", -1, false, {111}},
      {"spearman", 0, false, {111}},
      {"mean", 0, true, {kAnyFailure}},
      {"codebook", 0, true, {kAnyFailure}},
      {"ds", -1, false, {kAnyFailure}},
      {"describe", -1, false, {111}},
      {"duplicates", -1, false, {111}},
      {"egen", 0, false, {111}},
      {"levelsof", 0, false, {}},
      {"xtset", 0, false, {}},
      {"tsset", 0, false, {}},
  };
  for (const auto& row : kTable)
    if (cmd == row.cmd) return &row;
  return nullptr;
}

// The s() store's reserved key prefix: byte-identical to program.cpp's and
// eval/macros.cpp's kSClassKeyPrefix (no shared header, see program.cpp).
const char* const kSStoreKeyPrefix = "\x01s:";

// Applied by dispatch() to a leaf, before its r() hook: a failure's emptied r()
// is then that command's (declared empty) r(). `capture` sets `_rc` afterwards.
void applyAdoResidue(SessionState& session, const ojson& result, const std::string& rawCmd) {
  const std::string& line = session.leafCmdExpanded.empty() ? rawCmd : session.leafCmdExpanded;
  std::istringstream words(line);
  std::string w1, w2;
  words >> w1 >> w2;
  auto word = [](const std::string& w) { return lower(w.substr(0, w.find(','))); };
  const std::string cmd = registry::resolveCanonical(word(w1));
  if (cmd.empty()) return;
  std::string sub = word(w2);
  if (cmd == "estimates" && sub.size() >= 3 && std::string("store").compare(0, sub.size(), sub) == 0)
    sub = "store";
  const ojson* err = nullptr;
  if (result.is_array())
    for (const auto& b : result)
      if (b.is_object() && b.value("type", std::string()) == "error" && !b.value("captured", false)) {
        err = &b;
        break;
      }
  if (err != nullptr) {
    const AdoResidue* row = findAdoResidue(cmd);
    if (row == nullptr) return;
    const std::string rc = eval::coarseRc(*err);
    for (int code : row->clearsROn)
      if (code == kAnyFailure || rc == std::to_string(code)) {
        eval::replaceRClass(session.eval);
        eval::declareRPosted(session.eval, cmd, {});
        return;
      }
    return;
  }
  const AdoResidue* row = sub.empty() ? nullptr : findAdoResidue(cmd + " " + sub);
  if (row == nullptr) row = findAdoResidue(cmd);
  if (row == nullptr) return;
  if (row->rc >= 0) session.eval.locals.set("_rc", std::to_string(row->rc));
  if (row->clearsS) {
    std::vector<std::string> sKeys;
    for (const auto& kv : session.eval.rStrings.items())
      if (kv.first.rfind(kSStoreKeyPrefix, 0) == 0) sKeys.push_back(kv.first);
    for (const auto& k : sKeys) session.eval.rStrings.erase(k);
  }
}

}  // namespace

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
// Counts every dispatch() entry on this thread. A frame whose count did not move
// while it ran is a LEAF command: no prefix (`quietly`, `capture`, `eststo:`), no
// do-file and no loop ran another command underneath it. r(table) is posted only at
// leaves (handlers::rTableAfterCommand), so a container never re-posts a table a
// nested r-class command already replaced; a container that replaced e() itself
// only retires a table that no longer belongs to the live e(). thread_local because
// parallel blocks dispatch on their own threads with their own sessions.
thread_local unsigned long long g_dispatchSerial = 0;

nlohmann::ordered_json dispatch(const std::string& rawCmd, SessionState& session) {
  int depth;
  const unsigned long long serialAtEntry = ++g_dispatchSerial;
  const std::uintptr_t eIdAtEntry = handlers::liveEstimationIdentity(session);
  const eval::RStoreMark rMarkAtEntry = session.eval.rMark();
  // An r-class arm's declaration (eval::declareRPosted) belongs to the command
  // that made it: a leaf that threw past the hook must not hand it on (RULING.md
  // D.3.1).
  session.eval.rDeclaredNames.reset();
  session.eval.rDeclaredCmd.clear();
  session.leafCmdExpanded.clear();
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
  // X2-GLM: glm's e() entries outside its estimation block must not outlive
  // the glm (see handlers.h GlmEStores). Snapshot only while a glm is current.
  const std::optional<handlers::GlmEStores> glmBefore = handlers::glmOwnedEStores(session);
  nlohmann::ordered_json result;
  try {
    result = dispatchImpl(rawCmd, session);
    if (glmBefore.has_value()) handlers::dropStaleGlmEStores(session, *glmBefore);
    // name-binding-display-literals: an inline `=exp' that failed as Stata's
    // evaluator fails expanded to nothing at rc 0; its message prints ahead of
    // the line's own output, as StataBE prints `foo not found` before the line
    // (w2av_name_binding bd_dy_ilunb). Repeats of one message (a line expanded
    // twice on its way in) print once. BUG-1338: the expansion runs before any
    // prefix does, so `quietly` keeps these blocks (`inline_note`) wherever they
    // come from, and `capture` keeps those of its own line: StataBE prints
    // `variable nosuch not found` for `capture display "C`: type nosuch'"` and
    // `quietly local q "Q`: type nosuch'"` too.
    std::vector<std::string> notes = eval::takeInlineExpansionNotes();
    if (!notes.empty() && result.is_array()) {
      size_t at = 0;
      while (at < result.size() && result[at].is_object() &&
             result[at].value("type", std::string()) == "echo")
        ++at;
      std::string prev;
      for (const auto& n : notes) {
        if (n == prev) continue;
        prev = n;
        nlohmann::ordered_json nb;
        nb["type"] = "display";
        nb["text"] = n;
        nb["inline_note"] = true;
        nb["inline_note_of"] = rawCmd;  // the line that expanded (capture's filter)
        result.insert(result.begin() + static_cast<std::ptrdiff_t>(at++), std::move(nb));
      }
    }
  } catch (const std::exception& e) {
    // A C++ exception from a command is that command's error, not the run's.
    // Rethrown, it escaped every `capture` above it (capture reads error BLOCKS)
    // and runDoFileHeadless discarded the whole run's output with it: one
    // `import delimited ..., delimiter(tab)` that threw "bad magic" left
    // w2ar_data_io_family with 0 of 1912 arms, every earlier PREC line lost.
    // As a block it is capturable (_rc via coarseRc), and an uncaptured one
    // stops the do-file at its own line like any other error.
    (void)eval::takeInlineExpansionNotes();
    std::string msg = e.what();
    if (const auto* ce = dynamic_cast<const CobraError*>(&e)) msg = cobraErrorMessage(*ce);
    result = nlohmann::ordered_json::array();
    result.push_back(nlohmann::ordered_json(eval::engineError(msg)));
  } catch (...) {
    // BUG-360 follow-up: drain the section-38 projection ledger on the throwing
    // path too. A command that failed has still written its projections.
    if (session.exitDispatch() == 0) {
      session.reapProjectionScratch();
      session.releasePreInstallPins();  // e(sample) T2: the command is over
      session.sweepOrphanScratch("dispatch-exit-throw");  // BUG-389
      session.auditHeadBacked("dispatch-exit-throw");  // BUG-384 invariant
    }
    throw;
  }
  if (g_dispatchSerial == serialAtEntry) applyAdoResidue(session, result, rawCmd);
  // w2c_postest family 1: r(table) after estimation — the one posting site (see
  // postestimation/rtable_rclass_lifecycle.cpp). Before exitDispatch so its column reads run inside
  // this command's own scratch accounting.
  handlers::rTableAfterCommand(session, result, rawCmd, eIdAtEntry,
                               g_dispatchSerial == serialAtEntry, rMarkAtEntry);
  dprof::Scope __zw(dprof::kWrapper);
  if (session.exitDispatch() == 0) {
    session.reapProjectionScratch();
    session.releasePreInstallPins();  // e(sample) T2: the command is over
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
