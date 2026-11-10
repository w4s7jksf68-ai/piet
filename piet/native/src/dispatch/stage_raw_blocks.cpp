// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
#include "piet/handlers/io.h"
namespace piet::dispatch_detail {
bool interceptControlBlocks(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd) {


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
      { blocks = out; return true; }
    }
    // P4 wave: Stata's BRACELESS one-line branch, `if COND command ...`. The
    // braced form has always worked; without this, the braceless one reached
    // the command table as a command literally named `if` and was rejected as
    // unsupported. Handled here rather than in doexec's brace aggregator
    // because both the console and do-file paths funnel through dispatch(),
    // and the condition/body semantics are runIf's already — this only has to
    // find the split point and hand it a one-line body. FU-602: in Stata an
    // `else command` line may follow this form (w2_esample_copies.do:39-40,
    // StataBE runs it). A do-file, loop or program body pairs the two in
    // doexec's brace aggregator (collectElse), which turns the pair into a
    // braced if/else block; only an unpaired one-line `if` reaches this
    // intercept. An `else` that reaches dispatch has no `if` in front of it
    // and is refused by name below.
    if (strippedForLoop.size() >= 4 && lower(strippedForLoop.substr(0, 4)) == "else" &&
        (strippedForLoop.size() == 4 || strippedForLoop[4] == '{' ||
         std::isspace(static_cast<unsigned char>(strippedForLoop[4])) != 0)) {
      ojson out = ojson::array();
      out.push_back(echo);
      out.push_back(ojson(eval::errorBlock(
          "syntax_error",
          "`else` must follow an `if` command: the line before this `else` is not a one-line "
          "`if exp command` and does not close an `if` block",
          std::string("else"), std::nullopt, strutil::trim(rawCmd), "r(198)")));
      { blocks = out; return true; }
    }
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
        { blocks = out; return true; }
      }
      // FU-875: the split refuses a condition whose brackets do not pair; that
      // is Stata's r(132), not a line for the command tokenizer (StataBE 19.5:
      // `if (1 == 1 display "in"` "too few ')' or ']'", `if 1 == 1) display
      // "in"` "too many ')' or ']'").
      if (const std::string u = cecon::bracket_imbalance(rest); !u.empty()) {
        ojson out = ojson::array();
        out.push_back(echo);
        out.push_back(ojson(eval::errorBlock("engine_error", u, std::nullopt, std::nullopt,
                                             std::nullopt, std::string("r(132)"))));
        { blocks = out; return true; }
      }
    }
    // BUG-547: the legacy `for` command (foreach/forvalues are its
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
      { blocks = out; return true; }
    }
  }
  return false;
}


bool interceptInputBlock(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd) {


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
    // The head word ends at a blank OR at the fold separator: a bare `input`
    // (no varlist, FU-1251) folds to `input\x1F<row>...` with nothing between
    // (w2ar dx_inp_021_bare, log:4664: rows appended to the data in memory).
    const std::string inputTok0 = inputLine.substr(0, inputLine.find_first_of(" \t\x1F"));
    if (lower(inputTok0) == "input" && rawCmd.find('\x1F') != std::string::npos) {
      ojson r = handlers::runInput(rawCmd, session);
      if (r.is_array()) {
        ojson out = ojson::array({echo});
        for (const auto& b : r) out.push_back(b);
        { blocks = out; return true; }
      }
      { blocks = ojson::array({echo, r}); return true; }
    }
  }
  return false;
}


bool interceptMata(SessionState& session,
    ojson& blocks,
    const std::string& rawCmd) {


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
      return true;
    }
  }
  return false;
}


bool interceptParallelPrefix(SessionState& session,
    ojson& blocks,
    const std::string& rawCmd) {


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
    // BUG-1381: the head is matched as Stata abbreviates it (loc/loca,
    // gl/glo/glob/globa), the spellings tryCaptureDefinition already answers --
    // `loc t : type v` read a never-synced plane and was r(101).
    const std::string headWord = lowered.substr(head, lowered.find_first_of(" \t", head) - head);
    static const std::unordered_set<std::string> kDefHeads{"loc", "loca", "local", "gl", "glo",
                                                            "glob", "globa", "global"};
    if (lowered.find("label") != std::string::npos ||
        lowered.compare(head, 5, "local") == 0 || lowered.compare(head, 6, "global") == 0 ||
        kDefHeads.count(headWord) != 0) {
      session.syncEvalVarLabels();
    }
  }
  return false;
}

}
