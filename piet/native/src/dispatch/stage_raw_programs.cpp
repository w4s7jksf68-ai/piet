// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "expression_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {
bool interceptProgramCompletion(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd) {


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
      //
      // BUG-920: a program or nested do/run file run from a loop body starts
      // outside any loop: its `continue` is out of context too (oracle
      // w2ad_prog_preserve_nest pn_lm_cb_prog, pn_lm_cb_do: r(119) at every
      // iteration, and the caller's loop runs on).
      if (loops::loopBodyDepth() <= session.loopBodyFloor) {
        { blocks = ojson::array({echo, ojson(eval::errorBlock("engine_error",
                                                          "statement out of context", std::nullopt,
                                                          std::nullopt, std::nullopt, "r(119)"))}); return true; }
      }
      ojson ctrl;
      ctrl["type"] = "_control";
      ctrl["action"] = (noSpace.find("break") != std::string::npos) ? "break" : "continue";
      { blocks = ojson::array({echo, ctrl}); return true; }
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
      { blocks = ojson::array({echo}); return true; }
    }
  }
  return false;
}


bool interceptArguments(SessionState& session,
    ojson& blocks,
    const std::string& rawCmd) {


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
      { blocks = ojson::array(); return true; }
    }
  }
  return false;
}


bool interceptArgumentMacros(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd) {


  // ------------------------------------------------------------------
  // (c) `gettoken TOK REST : SRC [, options]` (U4) — intercepted before step
  // 0b (mirrors dispatcher.py's step-0 (c): `_u4_first == "gettoken"`); the
  // line's own macros are expanded below, as Stata does.
  // Silent ([]) on success; [echo, error] on a syntax-error failure.
  {
    dprof::Scope __z(dprof::kSmallSteps);
    std::string u4Stripped = trimWs(rawCmd);
    size_t u4Sp = u4Stripped.find_first_of(" \t");
    std::string u4First = lower(u4Sp == std::string::npos ? u4Stripped : u4Stripped.substr(0, u4Sp));
    if (u4First == "gettoken") {
      // BUG-1499: Stata expands the line's macros before gettoken reads it
      // (StataBE 19.5: parse("`pc'") parses on the local's characters and
      // `gettoken `tn' r : s` binds the local `tn' names).
      std::string gtLine = rawCmd;
      if (eval::hasMacroRefs(rawCmd)) {
        eval::ExpandResult ex = eval::expandMacros(rawCmd, session.eval);
        if (ex.isError) { blocks = ojson::array({echo, ex.error}); return true; }
        gtLine = ex.line;
      }
      eval::LocalsStore store;
      store.get = [&session](const std::string& name) -> std::string {
        return session.eval.locals.get(name, "");
      };
      store.set = [&session](const std::string& name, const std::string& value) {
        session.eval.locals.set(name, value);
      };
      store.getGlobal = [&session](const std::string& name) -> std::string {
        return session.eval.globals.get(name, "");
      };
      store.setGlobal = [&session](const std::string& name, const std::string& value) {
        session.eval.globals.set(name, value);
      };
      std::vector<nlohmann::json> gtBlocks = eval::runGettoken(gtLine, store);
      if (!gtBlocks.empty()) {
        ojson out = ojson::array({echo});
        for (const auto& b : gtBlocks) out.push_back(ojson(b));
        { blocks = out; return true; }
      }
      { blocks = ojson::array(); return true; }  // silent success
    }
  }
  return false;
}


bool interceptMacroDeclarations(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd) {


  {
    dprof::Scope __z(dprof::kSmallSteps);
    ojson u4Out;
    if (tryTempDeclIntercept(rawCmd, session, echo, u4Out)) { blocks = u4Out; return true; }
    // AER tier-1: `macro drop|define|shift|list|dir`. `macro` is in the
    // tokenizer's unsupported_macro net, so it is consumed here, before it.
    // A line still carrying `$`/a backtick is left for the expanded site below.
    if (handlers::tryMacroVerbIntercept(rawCmd, session, echo, u4Out, false)) { blocks = u4Out; return true; }
  }
  return false;
}


bool interceptProgramReturn(SessionState& session,
    ojson& blocks,
    const std::string& rawCmd) {


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
        return true;
      }
      { blocks = ojson::array(); return true; }
    }
  }
  return false;
}


bool interceptProgramDefinition(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd) {


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
          // BUG-923: `program drop NAME` for an absent program is r(111), as in
          // Stata (oracle w2_prog_controlflow pc_pg_capdrop_absent). It used
          // to be a benign no-op here (Python parity) that `capture` read as 0.
          { blocks = ojson::array({echo, mgmtBlock}); return true; }
        }
        if (mgmtBlock.contains("text") && progSubLower == "dir") {
          { blocks = ojson::array({echo, mgmtBlock}); return true; }
        }
        { blocks = ojson::array({echo}); return true; }
      }
      // BUG-923: a `program define NAME` that reaches dispatch was not taken
      // by the capture pass (program::captureProgams matches only a line that
      // STARTS with the header, so a prefixed `capture noisily program define
      // NAME` lands here). Defining an existing program without dropping it is
      // r(110) in Stata and leaves the old definition in place (oracle
      // pc_pg_redefine: 110, and the old pc_add still runs). Only that case is
      // answered here; any other define form keeps its old reply below.
      {
        std::string defName;
        if (progSubLower == "define" || progSubLower == "def" || progSubLower == "de") {
          progIss >> defName;
        } else if (!progSubLower.empty() && progSubLower != "list") {
          defName = progSub;
        }
        while (!defName.empty() && defName.back() == ',') defName.pop_back();
        if (const size_t comma = defName.find(','); comma != std::string::npos) {
          defName.erase(comma);
        }
        if (!defName.empty()) {
          if (std::optional<std::string> existing = program::findProgram(session, defName)) {
            { blocks = ojson::array({echo, ojson(eval::errorBlock(
                                           "engine_error", "program " + *existing + " already defined",
                                           std::nullopt, std::nullopt, std::nullopt,
                                           std::string("r(110)")))}); return true; }
          }
        }
      }
    }
  }
  return false;
}

}
