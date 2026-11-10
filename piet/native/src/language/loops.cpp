// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/loops.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "piet/cclass.h"
#include "piet/counters.h"  // BUG-350 coverage: the loop-capture drop site counts too
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/tokenizer.h"
#include "piet/parallel.h"
#include "piet/stata_format.h"  // BUG-1395: numlist element text is %15.0g
#include "piet/strutil.h"
#include "core/expr_blanks.h"  // FU-875: Stata's blank squeeze and r(132) pairing rule
#include "core/ident.h"  // Unicode names: the shared identifier byte tests
#include "data/missing.h"  // BUG-921: the extended-missing kind of a cell read by the if condition
#include "dispatch/dispatch_profile.h"  // EXPRLAYER: expression-zone attribution (opt-in, stderr)
#include "loops_internal.h"

// BUG-920: Stata's macro real-to-text rule (%18.0g text, no leading 0), defined
// in eval/macros.cpp beside the local/global store that applies it.
namespace piet::eval {
std::string stataMacroRealText(double d);
}  // namespace piet::eval

namespace piet::loops {

using namespace loops_detail;

namespace {

constexpr int kWhileCap = 1000;

// P7 (console cap, COUNCIL-20260710 D5/F6): kWhileCap/kNumlistMaxElements
// above already bound how many times a loop can ITERATE, but not how many
// blocks its body emits per iteration — a 3-command body over 10,000
// numlist-bounded iterations is already 30,000+ blocks, well past what a
// single bridge payload should ever carry. This bounds allBlocks itself,
// independent of (and in addition to) the iteration-count caps. Mirrors
// doexec.cpp's kMaxRunBlocks in magnitude — generous / classroom-day-safe,
// and (same reasoning as there) it only stops STORING blocks: the loop still
// runs to its normal completion/break/error so dataset/scalar side effects
// stay correct, only the excess console text is elided.
constexpr size_t kMaxLoopBlocks = 20000;


ojson loopTruncatedBlock() {
  ojson b;
  b["type"] = "display";
  b["text"] = "(output truncated: this loop exceeded " + std::to_string(kMaxLoopBlocks) +
              " blocks -- remaining iterations run, but their output is suppressed)";
  return b;
}

}  // namespace

namespace loops_detail {

// Appends `b` to allBlocks unless the per-loop cap already tripped; the
// first block that would cross kMaxLoopBlocks becomes a single elision
// marker instead, and every push after that is a no-op. `truncated` is
// owned by the caller (one flag per runForeach/runWhile invocation).
void pushLoopBlock(ojson& allBlocks, bool& truncated, const ojson& b) {
  if (truncated) return;
  if (allBlocks.size() >= kMaxLoopBlocks) {
    allBlocks.push_back(loopTruncatedBlock());
    truncated = true;
    return;
  }
  allBlocks.push_back(b);
}

}  // namespace loops_detail

namespace {


std::string rstripBrace(std::string s) {
  s = trim(s);
  if (!s.empty() && s.back() == '{') {
    s.pop_back();
    s = trim(s);
  }
  return s;
}

// ---------------------------------------------------------------------------
// Header keyword detection (loops.py's _LOOP_IF_KEYWORDS)

// BS-015: also covers `capture` and its abbreviations (cap/capt/captu/
// captur/capture — same spelling family as BS-016's fix to dispatcher.cpp's
// single-line capture stripping). `capture` is not a loop/branch construct,
// but like foreach/forvalues/while/if it CAN head a brace-aggregated `{ }`
// block (`capture { ... }`), and doexec.cpp's aggregateBraces()/
// collectBody() key EVERY brace-block recognition off this one set. Without
// this, a multi-line `capture { ... }` block's body lines were aggregated
// as independent top-level statements — never wrapped by capture at all:
// each line ran uncaptured (so `_rc` reflected whatever unrelated command
// ran last, not the block's real outcome), and an error inside the block
// would incorrectly halt the entire do-file instead of being swallowed the
// way `capture` promises. See executeLoop's dedicated "capture" branch
// below (and runCaptureBlock) for the actual block semantics.
const std::unordered_set<std::string>& loopIfKeywords() {
  static const std::unordered_set<std::string> kw = {
      // AER-SCOPE1 bundle A: forval/forv/forvalue are Stata's own accepted
      // truncations of forvalues, and the corpus writes them 946 times across
      // 40 packages -- more often than the full spelling in some files. They
      // must be recognised HERE, not just in the registry: a block opener is
      // detected before any registry lookup happens, so an abbreviation the
      // registry knows and this set does not would still fail to open a block.
      "forval", "forv", "forvalue",
      "foreach", "forvalues", "while", "if",
      "capture", "captur", "captu", "capt", "cap",
      // BUG-132: quietly/noisily ALSO head brace-aggregated blocks
      // (`quietly { ... }` / `noisily { ... }`) — the same omission BS-015
      // fixed for capture. Spellings mirror the dispatcher's single-line
      // prefix stripper (kQuietlyPrefixes, dispatcher.cpp:668-670) exactly.
      // See executeLoop's dedicated quietly/noisily branch (runQuietlyBlock)
      // for the block semantics. These only ever gate block recognition when a
      // `{` is actually present — every isLoopHeader call site requires a brace
      // on the line or a look-ahead to a lone `{` line — so a plain single-line
      // `quietly cmd` / `noisily cmd` still routes to the flat dispatch path
      // (dispatcher.cpp's Pre-0 prefix stripper) unchanged.
      "quietly", "qui", "quie", "quiet",
      "noisily", "noisil", "noisi", "nois", "noi", "no", "n",
      // PLEVEL-IMPL-WAVE arc ①: `parallel[, workers(N)] { … }` — the design's
      // §2.3 option B, a real block command rather than a comment directive.
      // Registered here for the same reason capture/quietly are: this set is
      // the ONE source of brace-block recognition (doexec.cpp's
      // aggregateBraces/collectBody key off it), so without the entry the body
      // lines would run as independent top-level statements and the block head
      // would error as an unknown command. NO abbreviations: unlike
      // capture/quietly (whose spellings mirror Stata's own prefix family),
      // `parallel` is a NEW PIET-only keyword with no Stata abbreviation
      // heritage to honour — and a short form like `par` would shadow user
      // programs. Recognition still requires an actual `{` on the line (or a
      // lone `{` look-ahead), so a bare `parallel` line is untouched by this
      // and keeps whatever the dispatcher does with it today.
      "parallel"};
  return kw;
}

std::string firstWord(const std::string& line) {
  std::string s = trim(line);
  size_t sp = s.find_first_of(" \t");
  return sp == std::string::npos ? s : s.substr(0, sp);
}

// The lowered head keyword of a block line, for the loopIfKeywords() lookup.
//
// `parallel, workers(2) {` is the FIRST block head in this codebase whose
// keyword is immediately followed by an option comma, so firstWord() returns
// "parallel," and the keyword lookup misses — the body lines then run as
// independent top-level statements and the head errors as an unknown command
// (measured, not theorised: that is exactly what the first build did). Strip
// that one trailing comma.
//
// Scoped to `parallel` ON PURPOSE. Stripping it for every keyword would
// silently start treating `capture, {` / `quietly, {` — today unknown-command
// errors — as brace blocks, which is a real behaviour change to lines this arc
// was not asked to touch. If that widening is ever wanted it should arrive as
// its own change with its own fixture, not as a side effect of adding a
// keyword.
std::string headKeyword(const std::string& line) {
  std::string fw = lower(firstWord(line));
  if (fw.size() > 1 && fw.back() == ',') {
    std::string stripped = fw.substr(0, fw.size() - 1);
    if (stripped == "parallel") return stripped;
  }
  return fw;
}



// ---------------------------------------------------------------------------
// foreach-in token split (quote-aware) — loops.py's _split_foreach_tokens

std::vector<std::string> splitForeachTokens(const std::string& itemsStr) {
  std::vector<std::string> tokens;
  std::string s = trim(itemsStr);
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '"') {
      size_t j = s.find('"', i + 1);
      if (j == std::string::npos) {
        tokens.push_back(s.substr(i + 1));
        break;
      }
      tokens.push_back(s.substr(i + 1, j - i - 1));
      i = j + 1;
    } else if (std::isspace(static_cast<unsigned char>(s[i]))) {
      ++i;
    } else {
      size_t j = i;
      while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
      tokens.push_back(s.substr(i, j - i));
      i = j;
    }
  }
  return tokens;
}

// ---------------------------------------------------------------------------
// Body runner — pipes body_lines through doexec::executeDoSource (handles
// nested blocks via the SAME brace-aggregation entry point).

// BUG-483: how many LOOP bodies deep the current dispatch is. The three loop
// runners below open a scope; `if`/`capture`/`quietly`/`parallel` deliberately
// do not (see loops.h). The accessor and the scope's members are defined after
// this anonymous namespace closes, because they are declared in piet::loops.
thread_local int g_loopBodyDepth = 0;

}  // namespace

namespace loops_detail {

BodyRunResult runBody(const std::vector<std::string>& bodyLines, SessionState& session,
                       const DispatchFn& dispatchFn) {
  std::string source;
  for (size_t i = 0; i < bodyLines.size(); ++i) {
    if (i) source += "\n";
    source += bodyLines[i];
  }
  doexec::ExecuteResult result = doexec::executeDoSource(source, session, dispatchFn);
  BodyRunResult out;
  out.blocks = result.blocks;
  // F3: a body aborts its enclosing loop ONLY when execution actually HALTED on
  // an uncaught error (or a cancel) — precisely the condition executeDoSource
  // records in stoppedAtLine. An error consumed by `capture` (BS-127: the
  // dispatcher tags it captured:true; it is display-only, sets _rc, and does
  // NOT halt) leaves stoppedAtLine unset, so the loop must keep iterating. The
  // old "any type==error block present" scan wrongly treated a captured error
  // as fatal, aborting foreach/forvalues/while (and nested loops) after the
  // first captured body error. Keying off stoppedAtLine mirrors doexec.cpp's
  // own top-level halt rule (type==error && !captured) exactly, so runBody can
  // never disagree with it; an UNcaptured error still sets stoppedAtLine and
  // still aborts the loop (Stata semantics for uncaught errors preserved).
  out.hasError = result.stoppedAtLine.has_value();
  // BUG-154: a CLEAN `exit [N]` halt inside this body is invisible to the
  // caller's caller. The halt rides the error channel as a sentinel block
  // ("exit"/"exit N"), and the INNER executeDoSource above both consumed and
  // ERASED it (doexec.cpp handleUncapturedError removes the sentinel so a
  // clean exit never paints a red card). An ordinary uncaptured error stays
  // in out.blocks and re-halts the outer executeDoSource when this body's
  // blocks bubble up through runIf/runForeach/runWhile — but the erased exit
  // sentinel left NOTHING to re-halt on, so `if (cond) { exit 7 }` fell
  // through and the do-file kept running (guards were dead). Re-emit the
  // sentinel here: the outer handleUncapturedError recognizes the message
  // shape, halts with the SAME code, and erases it again — cascading through
  // any nesting depth. `capture { ... exit N }` is unaffected: its body runs
  // under the capture arm, which traps the error channel and sets _rc=N by
  // design (the sentinel we append here is exactly what capture expects to
  // trap).
  if (result.stoppedAtLine.has_value() && result.stoppedAtExitCode.has_value()) {
    int code = *result.stoppedAtExitCode;
    out.exitCode = code;
    ojson halt;
    halt["type"] = "error";
    halt["message"] = "exit" + (code != 0 ? (" " + std::to_string(code)) : std::string());
    out.blocks.push_back(halt);
  }
  if (result.control.has_value()) out.control = result.control;
  return out;
}

}  // namespace loops_detail

namespace {

// ---------------------------------------------------------------------------
// Runners

ojson runForeach(const std::string& varname, const std::vector<std::string>& tokens,
                  const std::vector<std::string>& bodyLines, SessionState& session,
                  const DispatchFn& dispatchFn) {
  LoopBodyScope __loopBody;  // BUG-483: `continue` asks how deep we are
  ojson allBlocks = ojson::array();
  bool truncated = false;   // P7: allBlocks storage cap (see kMaxLoopBlocks)
  for (const auto& tok : tokens) {
    session.eval.locals.set(varname, tok);
    BodyRunResult result = runBody(bodyLines, session, dispatchFn);
    for (auto& b : result.blocks) pushLoopBlock(allBlocks, truncated, b);
    if (result.hasError) break;
    if (result.control == "break") break;
    // control == "continue": fall through to next token.
  }
  // BUG-920: StataBE 19.5 leaves the loop macro EMPTY once the loop is over --
  // not the last element, not the value it held before (oracle
  // w2_prog_controlflow pc_fe_after: `local m "before"` then
  // `foreach m in p1 p2 p3` leaves `m' as []). On EVERY exit: after
  // `continue, break`, an error, zero iterations and inside an outer loop alike
  // (w2ad_prog_preserve_nest pn_lm_fe_break, pn_lm_fe_err, pn_lm_fe_zero,
  // pn_lm_nested_done, pn_lm_nested_break: all []).
  session.eval.locals.erase(varname);
  return allBlocks;
}

// FULL FUNCTIONAL CHECK (2026-08-05): `forvalues` iterates its range LAZILY.
//
// It used to materialise every value into a vector<string> first
// (genForvaluesTokens) and hand that to runForeach, which is why a 10,000-
// iteration cap existed at all: the cap was a MEMORY guard on that vector, not
// a language limit. Stata has no such limit, so a legal do-file with a longer
// loop was refused — and simply raising the number would have moved the failure
// from an honest reject to an out-of-memory one. Generating each value in place
// removes the reason for the cap instead of arguing with it.
//
// The values accumulate `v += step` (drift and all), as Stata's do. BUG-920:
// StataBE 19.5 tests the accumulated value against the end with NO tolerance
// and writes it as macro text by the local/global rule (%18.0g, no leading
// 0). Oracle w2_prog_controlflow: pc_fv_frac (0(0.1)1 assigns
// 0 .1 .2 .3 ... .7999999999999999 .8999999999999999 .9999999999999999, 11
// values) and pc_fv_frac_small (0(0.01)0.31 runs 31 times and ends at
// .3000000000000001: the 32nd accumulated value 0.3100000000000001 is past
// 0.31, which the old 1e-9 tolerance let through). The old rendering was
// Python's repr (0.30000000000000004). A `foreach`-style enumerated list
// still goes through runForeach; only the numeric range is lazy.
ojson runForvaluesRange(const std::string& varname, double start, double step, double end,
                        const std::vector<std::string>& bodyLines, SessionState& session,
                        const DispatchFn& dispatchFn) {
  LoopBodyScope __loopBody;  // BUG-483: `continue` asks how deep we are
  ojson allBlocks = ojson::array();
  bool truncated = false;   // P7: allBlocks storage cap (see kMaxLoopBlocks)
  for (double v = start; (step > 0 && v <= end) || (step < 0 && v >= end); v += step) {
    session.eval.locals.set(varname, eval::stataMacroRealText(v));
    BodyRunResult result = runBody(bodyLines, session, dispatchFn);
    for (auto& b : result.blocks) pushLoopBlock(allBlocks, truncated, b);
    if (result.hasError) break;
    if (result.control == "break") break;
    // control == "continue": fall through to the next value.
  }
  // BUG-920: the loop macro is empty after the loop (oracle pc_fv_ab: `i' is
  // [] after `forvalues i = 1/5`, though it held "before" on entry), on every
  // exit (w2ad pn_lm_fv_break, pn_lm_fv_err, pn_lm_fv_zero: []).
  session.eval.locals.erase(varname);
  return allBlocks;
}

ojson runWhile(const std::string& condition, const std::vector<std::string>& bodyLines,
               SessionState& session, const DispatchFn& dispatchFn) {
  LoopBodyScope __loopBody;  // BUG-483: `continue` asks how deep we are
  ojson allBlocks = ojson::array();
  bool truncated = false;   // P7: allBlocks storage cap (see kMaxLoopBlocks)
  int nIters = 0;
  while (true) {
    std::string condExpanded = condition;
    if (eval::hasMacroRefs(condition)) {
      // unsetRAsMissing: a bare/guarded r() that isn't set evaluates to Stata
      // missing (`.`) in a while condition, not an error (FIX-RESTIMATE).
      eval::ExpandResult exp = eval::expandMacros(condition, session.eval,
                                                  /*unsetRAsMissing=*/true);
      if (exp.isError) {
        allBlocks.push_back(exp.error);
        return allBlocks;
      }
      condExpanded = exp.line;
    }
    BoolEvalResult truth = evalBoolExpr(condExpanded, session);
    if (truth.isError) {
      allBlocks.push_back(truth.error);
      return allBlocks;
    }
    if (!truth.value) break;

    if (++nIters > kWhileCap) {
      allBlocks.push_back(errBlock(
          "while: exceeded " + std::to_string(kWhileCap) +
          " iterations — possible runaway loop. Check your condition to ensure it "
          "eventually becomes false."));
      return allBlocks;
    }

    BodyRunResult result = runBody(bodyLines, session, dispatchFn);
    for (auto& b : result.blocks) pushLoopBlock(allBlocks, truncated, b);
    if (result.hasError) break;
    if (result.control == "break") break;
    // control == "continue": loop back to condition re-eval.
  }
  return allBlocks;
}

ojson runIf(const std::string& condition, const std::vector<std::string>& bodyLines,
            const std::vector<std::string>& elseBodyLines, SessionState& session,
            const DispatchFn& dispatchFn) {
  std::string condExpanded = condition;
  if (eval::hasMacroRefs(condition)) {
    // unsetRAsMissing: `if !_rc & !missing(r(estimate))` after a failed
    // capture must NOT error on the unset r() — Stata evaluates it as missing
    // (`.`). Only the control-flow condition path opts in (FIX-RESTIMATE).
    eval::ExpandResult exp = eval::expandMacros(condition, session.eval,
                                                /*unsetRAsMissing=*/true);
    if (exp.isError) return ojson::array({exp.error});
    condExpanded = exp.line;
  }
  BoolEvalResult truth = evalBoolExpr(condExpanded, session);
  if (truth.isError) return ojson::array({truth.error});

  BodyRunResult result = truth.value ? runBody(bodyLines, session, dispatchFn)
                                      : runBody(elseBodyLines, session, dispatchFn);
  ojson out = result.blocks;
  if (result.control.has_value()) {
    ojson ctrl;
    ctrl["type"] = "_control";
    ctrl["action"] = *result.control;
    out.push_back(ctrl);
  }
  return out;
}

// BS-015: `capture { line1 \n line2 \n ... }` multi-line block. Real Stata
// semantics: run the body sequentially; if a line errors, ITS error message
// is suppressed and the rest of the block does NOT run (mirrors how an
// uncaptured error normally halts execution — capture just catches this
// halt locally instead of letting it propagate out of the block), but
// lines BEFORE the failure keep whatever output they already produced
// (capture does not retroactively hide output that already happened — it
// is not `quietly`). `_rc` mirrors the existing single-line `capture cmd`
// convention (dispatcher.cpp's Pre-0 block): "1" on any error inside the
// block, "0" otherwise — the same coarse boolean this codebase uses
// everywhere today (an exact numeric _rc taxonomy is BS-100/A4, a later
// phase, not this fix). Reuses runBody (the SAME helper runForeach/runWhile/
// runIf already use) so nested blocks/control-flow inside the capture body
// work exactly like any other brace body, for free.
//
// BUG-1434: `showError` is a noisily after the capture (`capture noisily { …
// }`, `capture quietly noisily { … }`) -- the halting error is kept and shown
// (tagged captured, so it does not halt the caller), which is what StataBE
// prints ("variable nosuchvar not found" inside the block). `quiet` is the
// block's output visibility, which the last of the chain's capture/quietly/
// noisily decides: a bare `capture { … }` and `capture noisily quietly { … }`
// drop ordinary output (keptWhenQuiet), `quietly capture noisily { … }` shows
// it.
ojson runCaptureBlock(const std::vector<std::string>& bodyLines, SessionState& session,
                      const DispatchFn& dispatchFn, bool showError, bool quiet) {
  BodyRunResult result;
  {
    std::optional<SessionState::LogVerbosityScope> lv;
    if (quiet) lv.emplace(session, true);
    result = runBody(bodyLines, session, dispatchFn);
  }
  ojson out = ojson::array();
  // BUG-116/F3: the old scan below treated ANY type=="error" block as fatal
  // to this capture block, including one already caught by a NESTED
  // `capture` (tagged captured:true by the dispatcher's capture arm, BS-127
  // — display-only, does not halt). That wrongly truncated the block's
  // output at the nested capture's line and set the OUTER _rc from an error
  // that never actually escaped to this scope. Key off result.hasError
  // instead — runBody already computes it from doexec's own halt authority
  // (stoppedAtLine), the SAME signal runForeach/runWhile/runIf rely on — so
  // a captured nested error passes through untouched and the block keeps
  // running exactly like the enclosing do-file already does around it.
  //
  // BUG-546: the halting block's own coarse rc (eval::coarseRc) is captured
  // HERE, from the same block the loop is about to suppress, rather than
  // hardcoding "1" below — a `capture noisily { summarize nosuchcol }` gave
  // _rc 1 where the single-line `capture noisily summarize nosuchcol` (which
  // already used coarseRc, dispatcher.cpp) gave the real 111. Oracle:
  // tests/eval/golden/aer_w2/w2_openbugs.log fe_unknown/fe_mixed (the SAME
  // capture-BLOCK shape wraps the foreach arms there).
  std::string haltedRc = "1";
  for (const auto& b : result.blocks) {
    const std::string t = b.value("type", "");
    if (result.hasError && t == "error" && !b.value("captured", false)) {
      haltedRc = eval::coarseRc(b);
      // `exit` with code 0 is no error, and capture does not hold it: StataBE
      // `capture noisily { … exit 0 }` ends the do-file ("end of do-file").
      if (result.exitCode == 0) {
        out.push_back(b);
        break;
      }
      if (showError) {
        ojson tagged = b;
        tagged["captured"] = true;
        out.push_back(std::move(tagged));
      }
      break;  // suppress the error itself (and anything after — the block stops here)
    }
    // BUG-1338: a failed inline `: fn'/`=exp' in the body prints nothing under
    // the capture (StataBE 19.5: `capture { display "`: type nosuch'" }`), and
    // its message under `capture noisily { }`, as the halting error's.
    if (b.value("inline_note", false) && !showError) continue;
    if (quiet && !keptWhenQuiet(b)) continue;
    out.push_back(b);
  }
  settleNoisily(out, session);
  if (result.exitCode == 0) return out;
  // BUG-154: a clean `exit N` inside the block carries its EXACT code to _rc
  // (Stata 19.5: `capture { exit 5 }` → _rc 5, block stops, file continues).
  // Every other error carries the exact coarse code (BUG-546, above).
  session.eval.locals.set("_rc", result.exitCode.has_value()
                                     ? std::to_string(*result.exitCode)
                                     : (result.hasError ? haltedRc : "0"));
  if (result.control.has_value()) {
    ojson ctrl;
    ctrl["type"] = "_control";
    ctrl["action"] = *result.control;
    out.push_back(ctrl);
  }
  return out;
}

// BUG-132: `quietly { … }` / `noisily { … }` multi-line block. Like
// `capture { … }` (runCaptureBlock above), quietly/noisily are prefix-class
// keywords that can head a brace-aggregated block — but their semantics differ
// from capture on TWO axes, so they get their own handler rather than reusing
// the capture path:
//   (1) OUTPUT visibility. `quietly` SUPPRESSES the body's normal output
//       (display/result/data_mutation cards) while still showing command
//       echoes and any error message — exactly the filter dispatcher.cpp's
//       single-line quietly branch applies (keep type=="echo"||"error";
//       dispatcher.cpp:769-773). `noisily` is the transparent case: it shows
//       everything the body produced (its purpose is to un-quiet a region
//       nested inside an enclosing quietly block; at top level it behaves like
//       a plain block).
//   (2) ERROR propagation. UNLIKE capture, neither quietly nor noisily
//       SWALLOWS an uncaught error — an error inside the block still HALTS the
//       do-file. runBody leaves the error block untagged (captured stays
//       false) and reports hasError, so passing that error block through
//       unmodified lets doexec.cpp's top-level halt rule (type=="error" &&
//       !captured) fire exactly as it does for a bare uncaught error. This
//       matches the single-line `quietly cmd` behaviour BUG-132's repro
//       confirmed correct (quietly is output-only; it never catches errors the
//       way capture does).
// No `_rc` is set here, on purpose: only `capture` owns `_rc` in this codebase,
// and real Stata's quietly/noisily are transparent to `_rc` — they neither set
// nor clear it — so leaving it untouched is the correct semantics, not an
// omission. Reuses runBody (the SAME helper runForeach/runWhile/runIf/
// runCaptureBlock use) so nested blocks/control-flow inside the body work for
// free.
ojson runQuietlyBlock(const std::vector<std::string>& bodyLines, SessionState& session,
                      const DispatchFn& dispatchFn, bool quiet) {
  // BUG-189: the filter below cannot reach `log using` — dispatch() tees each
  // body line to the log before this function ever sees the block. Declaring the
  // scope makes the tee itself aware of it.
  BodyRunResult result;
  {
    SessionState::LogVerbosityScope __lv(session, quiet);
    result = runBody(bodyLines, session, dispatchFn);
  }
  ojson out = ojson::array();
  for (const auto& b : result.blocks) {
    if (quiet) {
      // quietly: keep only command echoes + error messages (and a nested
      // noisily's output, BUG-1434); suppress the body's normal output.
      // Mirrors the single-line quietly filter.
      std::string t = b.value("type", "");
      if (t != "_control" && keptWhenQuiet(b)) out.push_back(b);
    } else {
      // noisily: fully transparent — show every block the body produced.
      out.push_back(b);
    }
  }
  if (quiet) {
    settleNoisily(out, session);
  } else {
    markNoisily(out, session);
  }
  if (result.control.has_value()) {
    ojson ctrl;
    ctrl["type"] = "_control";
    ctrl["action"] = *result.control;
    out.push_back(ctrl);
  }
  return out;
}

}  // namespace

// BUG-483: declared in loops.h, defined here — outside the anonymous namespace
// above, which is where the counter lives (still visible for the rest of this
// translation unit).
int loopBodyDepth() { return g_loopBodyDepth; }
LoopBodyScope::LoopBodyScope() { ++g_loopBodyDepth; }
LoopBodyScope::~LoopBodyScope() { --g_loopBodyDepth; }

void markNoisily(ojson& blocks, const SessionState& session) {
  if (!session.logEchoSuppressed() || !blocks.is_array()) return;
  for (auto& b : blocks) {
    if (b.is_object() && b.value("type", std::string()) != "echo") b["noisily"] = true;
  }
}

// BUG-1338: the message of a failed inline `: fn'/`=exp' (inline_note) shows
// under quietly as an error's does (StataBE 19.5: `quietly { display "`: type
// nosuch'" }` prints it).
bool keptWhenQuiet(const ojson& b) {
  const std::string t = b.value("type", std::string());
  return t == "echo" || t == "error" || t == "_control" || b.value("noisily", false) ||
         b.value("inline_note", false);
}

void settleNoisily(ojson& blocks, const SessionState& session, bool force) {
  if ((!force && session.logEchoSuppressed()) || !blocks.is_array()) return;
  for (auto& b : blocks) {
    if (b.is_object()) b.erase("noisily");
  }
}

bool isLoopHeader(const std::string& line) {
  return loopIfKeywords().count(headKeyword(line)) != 0;
}

bool isParallelHeader(const std::string& line) { return headKeyword(line) == "parallel"; }

// BUG-167 — see the contract in loops.h. The prefix-class spellings mirror
// loopIfKeywords() exactly (capture/quietly/noisily + their abbreviations);
// foreach/forvalues/while/if are the CONTROL class, whose header legitimately
// carries a list or condition ahead of the brace.
bool opensBraceBlock(const std::string& line) {
  if (!isLoopHeader(line)) return false;
  size_t brace = line.find('{');
  if (brace == std::string::npos) return false;

  static const std::unordered_set<std::string> kControl = {"foreach", "forvalues", "forval",
                                                          "forv",    "forvalue",  "while",
                                                          "if"};
  if (kControl.count(lower(firstWord(line))) != 0) return true;

  // Prefix class: everything before the brace must itself be prefix keywords.
  std::istringstream iss(line.substr(0, brace));
  std::string word;
  while (iss >> word) {
    std::string lw = lower(word);
    if (kControl.count(lw) != 0) return true;  // `capture if ... {` — control wins.
    if (loopIfKeywords().count(lw) == 0) return false;
  }
  return true;
}

// P4 wave — see loops.h. Splits `COND command ...` by walking whitespace-
// separated tokens at paren/quote depth 0 with a tiny operand/operator state
// machine: the command begins at the first token that arrives where an
// OPERATOR was expected. Returning false (rather than a best guess) whenever
// the walk cannot reach that state is the whole safety property — the caller
// then reports its ordinary "unsupported" error, and a malformed line can
// never silently execute a command the user did not write.
bool splitOneLineIf(const std::string& rest, std::string& cond, std::string& body) {
  static const std::vector<std::string> kBinOps = {
      "==", "!=", "~=", ">=", "<=", "&&", "||", "&", "|", "<", ">", "+", "-", "*", "/", "^"};
  auto isBinOpToken = [&](const std::string& t) {
    return std::find(kBinOps.begin(), kBinOps.end(), t) != kBinOps.end();
  };
  // A token ENDING in a binary operator (e.g. `x'!=`) still expects an operand;
  // one that merely contains a complete comparison (`x'!=0`) does not.
  auto endsWithBinOp = [&](const std::string& t) {
    for (const auto& op : kBinOps) {
      if (t.size() >= op.size() && t.compare(t.size() - op.size(), op.size(), op) == 0) return true;
    }
    return false;
  };

  size_t i = 0, n = rest.size();
  int depth = 0;
  bool inQuote = false;
  bool expectOperand = true;
  size_t splitAt = std::string::npos;
  while (i < n) {
    while (i < n && std::isspace(static_cast<unsigned char>(rest[i]))) ++i;
    if (i >= n) break;
    const size_t tokStart = i;
    while (i < n) {
      const char c = rest[i];
      if (c == '"') inQuote = !inQuote;
      else if (!inQuote && (c == '(' || c == '[')) ++depth;
      else if (!inQuote && (c == ')' || c == ']')) --depth;
      else if (!inQuote && depth == 0 && std::isspace(static_cast<unsigned char>(c))) break;
      ++i;
    }
    const std::string tok = rest.substr(tokStart, i - tokStart);
    if (inQuote || depth != 0) return false;  // unbalanced — refuse to guess
    if (expectOperand) {
      // A lone `!`/`~` is a unary prefix; anything else completes an operand
      // unless it trails a binary operator awaiting its right-hand side.
      if (tok != "!" && tok != "~") expectOperand = endsWithBinOp(tok);
    } else if (isBinOpToken(tok)) {
      expectOperand = true;
    } else {
      splitAt = tokStart;  // an operand where an operator belongs = the command
      break;
    }
  }
  if (splitAt == std::string::npos || splitAt == 0) return false;
  cond = rest.substr(0, splitAt);
  body = rest.substr(splitAt);
  // trim
  auto trimBoth = [](std::string& s2) {
    size_t a = s2.find_first_not_of(" \t");
    size_t b = s2.find_last_not_of(" \t");
    s2 = (a == std::string::npos) ? std::string() : s2.substr(a, b - a + 1);
  };
  trimBoth(cond);
  trimBoth(body);
  return !cond.empty() && !body.empty();
}

ojson runOneLineIf(const std::string& condition, const std::string& bodyLine,
                   SessionState& session, const DispatchFn& dispatchFn) {
  return runIf(condition, {bodyLine}, {}, session, dispatchFn);
}

namespace {

// FULL FUNCTIONAL CHECK (2026-08-05). Peel leading PREFIX keywords off a block
// header while what remains still starts with a CONTROL keyword, e.g.
// `quietly forvalues i = 1/10 {` -> ("quietly", "forvalues i = 1/10").
//
// Why this exists: executeLoop dispatches on the header's FIRST WORD, so a
// prefixed loop header matched the prefix branch and ran the body as a bare
// `quietly { … }` block — the `forvalues i = 1/10` part was discarded outright.
// The body therefore executed ONCE with the loop macro never defined, which
// surfaces as "Local macro `i' is not defined". Stata accepts a prefix on any
// command, loop headers included. Returns false when the header is not a
// prefixed control header, leaving every existing dispatch untouched.
bool splitPrefixedControlHeader(const std::string& hdr, std::vector<std::string>& prefixes,
                                std::string& inner) {
  static const std::unordered_set<std::string> kControl = {"foreach", "forvalues", "forval",
                                                          "forv",    "forvalue",  "while",
                                                          "if"};
  std::string rest = trim(hdr);
  prefixes.clear();
  while (true) {
    std::string fw = lower(firstWord(rest));
    if (fw.empty()) return false;
    if (kControl.count(fw) != 0) {
      // `if` is BOTH a control keyword and a legal prefix-free header; only
      // treat what we peeled as prefixes if we actually peeled something.
      if (prefixes.empty()) return false;
      inner = rest;
      return true;
    }
    if (loopIfKeywords().count(fw) == 0) return false;  // not a prefix either
    prefixes.push_back(fw);
    size_t sp = rest.find_first_of(" \t");
    if (sp == std::string::npos) return false;          // prefix with no inner command
    rest = trim(rest.substr(sp));
  }
}

bool isCapturePrefix(const std::string& w) {
  return w == "capture" || w == "captur" || w == "captu" || w == "capt" || w == "cap";
}
bool isQuietlyPrefix(const std::string& w) {
  return w == "quietly" || w == "qui" || w == "quie" || w == "quiet";
}
// The noisily-class spellings splitPrefixedControlHeader accepts. Everything
// that is neither capture- nor quietly-class in a prefix chain is this, but the
// BUG-189 log scope has to say WHICH it is (noisily pushes a visible scope,
// capture pushes none), so the set is named rather than inferred by elimination.
bool isNoisilyPrefix(const std::string& w) {
  return w == "noisily" || w == "noisil" || w == "noisi" || w == "nois" ||
         w == "noi" || w == "no" || w == "n";
}

}  // namespace

ojson executeLoop(const std::string& header, const std::vector<std::string>& bodyLinesIn,
                   const std::vector<std::string>& elseBodyLines, SessionState& session,
                   const DispatchFn& dispatchFn) {
  std::string hdr = rstripBrace(header);
  // AER-SCOPE1 bundle A: fold the accepted truncations onto the canonical head
  // ONCE, here, rather than widening the five `fw == "forvalues"` compares and
  // three `^forvalues\s+` regexes downstream. Five spellings times eight sites
  // is forty places for one of them to be forgotten; this is one place, and the
  // prefixed path (`quietly forval i=1/3 {`) reaches it too because that path
  // recurses back into executeLoop with the inner header.
  {
    size_t sp = hdr.find_first_of(" \t");
    std::string head = lower(sp == std::string::npos ? hdr : hdr.substr(0, sp));
    if (head == "forval" || head == "forv" || head == "forvalue") {
      hdr = "forvalues" + (sp == std::string::npos ? std::string() : hdr.substr(sp));
    }
  }

  // A prefixed control header runs the CONTROL construct and then applies the
  // prefix's own semantics to what it produced — innermost prefix first, so
  // `capture quietly forvalues …` quiets the loop and then captures it, the
  // same order the single-line prefix stripper composes in.
  {
    std::vector<std::string> prefixes;
    std::string inner;
    if (splitPrefixedControlHeader(hdr, prefixes, inner)) {
      // BUG-189: `quietly forvalues … {` must hide its body from the log too.
      // The stack is innermost-wins, so pushing the prefixes outermost-first
      // leaves the innermost keyword on top — the same rule the result filters
      // below implement by folding in reverse.
      // BUG-1434: a capture hides output as quietly does, and a noisily after
      // a capture or quietly turns output back on (the last word decides,
      // StataBE 19.5) -- so either one filters only when no noisily follows.
      auto noisilyAfter = [&](size_t at) {
        for (size_t i = at + 1; i < prefixes.size(); ++i)
          if (isNoisilyPrefix(prefixes[i])) return true;
        return false;
      };
      std::vector<std::unique_ptr<SessionState::LogVerbosityScope>> lvScopes;
      for (size_t i = 0; i < prefixes.size(); ++i) {
        const std::string& p = prefixes[i];
        if (isQuietlyPrefix(p) || (isCapturePrefix(p) && !noisilyAfter(i))) {
          lvScopes.push_back(std::make_unique<SessionState::LogVerbosityScope>(session, true));
        } else if (isNoisilyPrefix(p)) {
          lvScopes.push_back(std::make_unique<SessionState::LogVerbosityScope>(session, false));
        }
      }
      ojson result = executeLoop(inner, bodyLinesIn, elseBodyLines, session, dispatchFn);
      lvScopes.clear();  // close the scopes before the result filters run
      for (auto it = prefixes.rbegin(); it != prefixes.rend(); ++it) {
        const size_t at = static_cast<size_t>(prefixes.rend() - it) - 1;
        if ((isQuietlyPrefix(*it) || isCapturePrefix(*it)) && !noisilyAfter(at)) {
          // quietly: keep command echoes + errors, drop ordinary output —
          // the same filter runQuietlyBlock applies to a braced body.
          ojson kept = ojson::array();
          for (const auto& b : result)
            if (keptWhenQuiet(b)) kept.push_back(b);
          result = kept;
        }
        if (isCapturePrefix(*it)) {
          // capture: swallow an uncaught error and publish _rc, matching
          // runCaptureBlock. A loop that errored has already stopped itself.
          // BUG-1434: under `capture noisily` the error is SHOWN (tagged
          // captured, so it does not halt) -- StataBE prints the inner
          // command's message, as the single-line prefix does.
          const bool showError = noisilyAfter(at);
          ojson kept = ojson::array();
          bool errored = false;
          // BUG-546: the exact coarse rc of the FIRST uncaptured error block,
          // not a hardcoded "1" — mirrors runCaptureBlock's own fix above, and
          // the pre-existing single-line `capture` prefix in dispatcher.cpp,
          // which already reads eval::coarseRc. Oracle: w2_openbugs.log
          // fe_unknown/fe_mixed via `capture noisily foreach ... { }`.
          std::string errRc;
          for (const auto& b : result) {
            if (b.value("type", std::string()) == "error" && !b.value("captured", false)) {
              errored = true;
              if (errRc.empty()) errRc = eval::coarseRc(b);
              // BUG-350 coverage gap, found 2026-09-01 by the leg-1D probe-2
              // audit: this is the SECOND place an error block is dropped where
              // the user cannot see it, and it was not counted. The counter's
              // whole claim is "a run that hid something says so", and a
              // `capture` on a LOOP prefix hid it silently. Counted here for the
              // same reason and by the same call as dispatcher.cpp's site.
              //
              // Under `capture noisily` the block is kept and shown instead.
              if (showError) {
                ojson tagged = b;
                tagged["captured"] = true;
                kept.push_back(std::move(tagged));
                continue;
              }
              piet::counters::noteSwallowedError(b.value("message", std::string()));
              continue;
            }
            // BUG-1338: the messages of failed inline `: fn'/`=exp' in the body
            // are silent under a capture (StataBE: `capture foreach v in a {
            // display "`: type nosuch'" }` prints nothing), shown under capture
            // noisily.
            if (b.value("inline_note", false) && !showError) continue;
            kept.push_back(b);
          }
          session.eval.locals.set("_rc", errored ? errRc : "0");
          result = kept;
        }
        // noisily: transparent; inside an enclosing quiet scope its output is
        // the nested noisily's, which that scope keeps.
        if (isNoisilyPrefix(*it)) markNoisily(result, session);
      }
      settleNoisily(result, session);
      return result;
    }
  }

  // Macro expansion of forvalues/foreach headers ONLY (while/if re-expand
  // per-iteration inside their own runners) — loops.py's has_macro_refs guard.
  std::string fw = headKeyword(hdr);
  // wt_x01_L4/L5 (w2at_table_writers.do:834, log): Stata stores a loop body
  // with each unquoted blank run collapsed to one blank, before macro
  // expansion (eval::collapseUnquotedBlanks; `tex `lab' &  ``lab'_m' \\`
  // writes `x1 & .125 \\`). The body lines are the flattened block, so an
  // `if {}` nested in the loop is collapsed with it; a top-level `if {}` body
  // reaches here as `if` and keeps its blanks, as Stata's does.
  std::vector<std::string> collapsedBody;
  const bool storesCollapsed = fw == "foreach" || fw == "forvalues" || fw == "while";
  if (storesCollapsed) {
    collapsedBody.reserve(bodyLinesIn.size());
    for (const auto& l : bodyLinesIn) collapsedBody.push_back(eval::collapseUnquotedBlanks(l));
  }
  const std::vector<std::string>& bodyLines = storesCollapsed ? collapsedBody : bodyLinesIn;
  if ((fw == "forvalues" || fw == "foreach") && eval::hasMacroRefs(hdr)) {
    eval::ExpandResult exp = eval::expandMacros(hdr, session.eval);
    if (exp.isError) return ojson::array({exp.error});
    hdr = exp.line;
  }

  // foreach VAR of varlist VARLIST
  //
  // BUG-545: the keyword accepts any >=3-char prefix of `varlist` (`var`,
  // `varl`, `vari`, `varli`, `varlis`, `varlist`), not the literal word only.
  // Corpus-measured: 19 lines spell `of var`, 1 spells `of varl`, 20 lines in
  // 8 papers total (open-bugs.json use_count_measured); oracle
  // fe_of_var (w2_openbugs.log) is rc 0 for `of var`.
  {
    static const std::regex re(R"(^foreach\s+([\w\x80-\xff]+)\s+of\s+var(?:l(?:i(?:s(?:t)?)?)?)?\s+(.*))",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      VarlistExpandResult vr = expandVarlistImpl(trim(m.str(2)), session);
      if (vr.isError) return ojson::array({vr.error});
      // BUG-509 (corrected)/BUG-544: `foreach v of varlist nosuchcol` is
      // r(111) "variable nosuchcol not found", caught BEFORE any iteration --
      // the entry's original r(199) pin was an artifact of the oracle's
      // `capture noisily foreach ... {` spelling (the 199 was the orphaned
      // `}`, not the foreach's own rc). Also fires for a MIXED list (oracle
      // fe_mixed: `alpha nosuchcol` never iterates `alpha`). KNOWN DISCLOSED
      // GAP: unique abbreviations (`alph` for `alpha`) are refused r(111)
      // here, while Stata resolves them (oracle fe_abbrev: rc 0, iterates
      // `alpha`) -- resolved now by checkKnownVarTokens (selftest arm
      // W2 loops/BUG-544 fe_abbrev).
      if (auto err = checkKnownVarTokens(vr.tokens, session)) {
        return ojson::array({*err});
      }
      return runForeach(m.str(1), vr.tokens, bodyLines, session, dispatchFn);
    }
  }
  // foreach VAR of numlist NUMLIST
  {
    static const std::regex re(R"(^foreach\s+([\w\x80-\xff]+)\s+of\s+numlist\s+(.*))",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      NumlistResult nr = parseNumlist(trim(m.str(2)));
      if (nr.isError) return ojson::array({nr.error});
      return runForeach(m.str(1), nr.tokens, bodyLines, session, dispatchFn);
    }
  }
  // foreach VAR of local NAME
  {
    static const std::regex re(R"(^foreach\s+([\w\x80-\xff]+)\s+of\s+local\s+([\w\x80-\xff]+)\s*$)",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      std::vector<std::string> tokens = splitForeachTokens(session.eval.locals.get(m.str(2), ""));
      return runForeach(m.str(1), tokens, bodyLines, session, dispatchFn);
    }
  }
  // foreach VAR of global NAME
  {
    static const std::regex re(R"(^foreach\s+([\w\x80-\xff]+)\s+of\s+global\s+([\w\x80-\xff]+)\s*$)",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      std::vector<std::string> tokens = splitForeachTokens(session.eval.globals.get(m.str(2), ""));
      return runForeach(m.str(1), tokens, bodyLines, session, dispatchFn);
    }
  }
  // foreach VAR of newlist TOKENS (BS-034a, BETA campaign C3) — newlist is
  // a list of NEW variable NAMES (e.g. names not yet created as columns,
  // often built from `tempvar`); unlike `of varlist` above, there is NO
  // existence check against the current dataset — iteration is simply over
  // the tokens as given, same split rule as the plain "in" form (oracle:
  // `foreach v of newlist na nb nc` -> "newvar na" / "newvar nb" /
  // "newvar nc"). Must come before the plain "in" form for the SAME reason
  // the other `of ...` forms do (though there's no actual overlap risk
  // here — "newlist" the literal keyword vs "in" — ordering just mirrors
  // the existing convention).
  {
    static const std::regex re(R"(^foreach\s+([\w\x80-\xff]+)\s+of\s+newlist\s+(.*))",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      std::vector<std::string> tokens = splitForeachTokens(trim(m.str(2)));
      return runForeach(m.str(1), tokens, bodyLines, session, dispatchFn);
    }
  }
  // foreach VAR in TOKENS  (must come AFTER the `of` forms)
  {
    // BUG-920: an empty list (`foreach v in {`, the header left as
    // `foreach v in` once the brace is stripped) runs zero times at rc 0
    // (oracle pc_fe_empty_in); `\s+in\s+` needed a token after `in` and sent
    // the bare header to the unrecognised-header error (rc 1).
    static const std::regex re(R"(^foreach\s+([\w\x80-\xff]+)\s+in(?:\s+(.*))?$)", std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      std::vector<std::string> tokens = splitForeachTokens(trim(m.str(2)));
      return runForeach(m.str(1), tokens, bodyLines, session, dispatchFn);
    }
  }
  // forvalues i = #1 #2 to #3  /  forvalues i = #1 #2 : #3 (BS-034b/c, BETA
  // campaign C3) — Stata's "explicit first two values" numlist form: start
  // = #1, the step is IMPLIED by the gap between the first two values (#2 -
  // #1), end = #3; `to`/`:` are interchangeable separators (oracle:
  // `forvalues i = 1 3 to 9` == `forvalues i = 1 3 : 9` -> 1 3 5 7 9). Must
  // be checked AFTER the `(STEP)` and `/` forms below it structurally
  // cannot ambiguously match this (neither uses bare whitespace as its
  // start/second-number separator) — placed here regardless, matching
  // this file's existing top-to-bottom convention. The already-working
  // `1(3)9` form is untouched — this is a syntactically distinct header.
  {
    static const std::regex re(
        R"(^forvalues\s+([\w\x80-\xff]+)\s*=\s*([+-]?\d+(?:\.\d+)?)\s+([+-]?\d+(?:\.\d+)?)\s*(?:to|:)\s*([+-]?\d+(?:\.\d+)?))",
        std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      double first = std::stod(m.str(2)), second = std::stod(m.str(3)), end = std::stod(m.str(4));
      double step = second - first;
      if (step == 0.0) {
        return ojson::array({errBlock("forvalues: step cannot be zero")});
      }
      return runForvaluesRange(m.str(1), first, step, end, bodyLines, session,
                               dispatchFn);
    }
  }
  // forvalues i=START(STEP)END
  {
    static const std::regex re(
        R"(^forvalues\s+([\w\x80-\xff]+)\s*=\s*([+-]?\d+(?:\.\d+)?)\s*\(([+-]?\d+(?:\.\d+)?)\)\s*([+-]?\d+(?:\.\d+)?))",
        std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      double start = std::stod(m.str(2)), step = std::stod(m.str(3)), end = std::stod(m.str(4));
      if (step == 0.0) {
        return ojson::array({errBlock("forvalues: step cannot be zero")});
      }
      return runForvaluesRange(m.str(1), start, step, end, bodyLines, session,
                               dispatchFn);
    }
  }
  // forvalues i=START/END  (step=1)
  {
    static const std::regex re(
        R"(^forvalues\s+([\w\x80-\xff]+)\s*=\s*([+-]?\d+(?:\.\d+)?)\s*/\s*([+-]?\d+(?:\.\d+)?))",
        std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      // BUG-920: #1/#2 always steps by +1, so `forvalues i = 3/1` runs zero
      // times (oracle pc_fv_empty n1 0; [P] forvalues: "#1/#2 ... in steps
      // of 1"). It used to count down 3 2 1. A numlist's 3/1 does count down
      // (parseNumlist), which is a different rule.
      double start = std::stod(m.str(2)), end = std::stod(m.str(3));
      const double step = 1.0;
      return runForvaluesRange(m.str(1), start, step, end, bodyLines, session,
                               dispatchFn);
    }
  }
  // while EXPR
  {
    static const std::regex re(R"(^while\s+(.*))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      return runWhile(trim(m.str(1)), bodyLines, session, dispatchFn);
    }
  }
  // if EXPR [else { ... }]
  {
    static const std::regex re(R"(^if\s+(.*))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      return runIf(trim(m.str(1)), bodyLines, elseBodyLines, session, dispatchFn);
    }
  }
  // capture { ... } (BS-015) — not a loop/branch; `fw` (computed above) is
  // already the lowered first word of the header, so no re-parsing needed.
  // A prefix chain that holds `capture` (`capture {`, `capture noisily
  // [quietly] {`, `noisily capture {`) is a capture block. BUG-1434, StataBE
  // 19.5 2026-11-03: a noisily anywhere after the capture shows the error
  // (`capture quietly noisily {` and `capture noisily quietly {` both print
  // it), and the LAST of the chain's words decides whether ordinary output
  // shows -- capture and quietly hide it, noisily shows it (`quietly capture
  // noisily {` prints, `noisily capture {` and `capture {` print nothing). A
  // chain without capture is a quietly/noisily block by the same last word.
  {
    std::vector<std::string> words;
    std::string rest = trim(hdr);
    while (!rest.empty()) {
      const size_t sp = rest.find_first_of(" \t");
      words.push_back(lower(sp == std::string::npos ? rest : rest.substr(0, sp)));
      rest = sp == std::string::npos ? std::string() : trim(rest.substr(sp));
    }
    size_t capAt = words.size();
    bool allPrefixes = !words.empty();
    for (size_t i = 0; i < words.size(); ++i) {
      if (isCapturePrefix(words[i]) && capAt == words.size()) capAt = i;
      allPrefixes = allPrefixes && (isCapturePrefix(words[i]) || isQuietlyPrefix(words[i]) ||
                                    isNoisilyPrefix(words[i]));
    }
    if (allPrefixes && capAt < words.size()) {
      bool showError = false;
      for (size_t i = capAt + 1; i < words.size(); ++i) showError = showError || isNoisilyPrefix(words[i]);
      const bool quiet = !isNoisilyPrefix(words.back());
      return runCaptureBlock(bodyLines, session, dispatchFn, showError, quiet);
    }
    if (allPrefixes && words.size() > 1) {
      return runQuietlyBlock(bodyLines, session, dispatchFn, /*quiet=*/!isNoisilyPrefix(words.back()));
    }
  }
  // BUG-132: quietly/noisily { ... } block — not a loop/branch; suppress
  // (quietly) or pass through (noisily) the body's output, but (unlike
  // capture) never swallow an uncaught error. `fw` is the lowered first word;
  // spellings mirror the dispatcher's kQuietlyPrefixes split.
  if (fw == "quietly" || fw == "qui" || fw == "quie" || fw == "quiet") {
    return runQuietlyBlock(bodyLines, session, dispatchFn, /*quiet=*/true);
  }
  if (fw == "noisily" || fw == "noisil" || fw == "noisi" || fw == "nois" || fw == "noi" ||
      fw == "no" || fw == "n") {
    return runQuietlyBlock(bodyLines, session, dispatchFn, /*quiet=*/false);
  }
  // PLEVEL-IMPL-WAVE arc ①: `parallel[, workers(N)] { … }`.
  if (fw == "parallel") {
    return runParallelBlock(hdr, bodyLines, session, dispatchFn);
  }

  return ojson::array({errBlock("loops: unrecognised loop header: '" + hdr + "'")});
}

}  // namespace piet::loops
