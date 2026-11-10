// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {
bool interceptDoRun(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd) {


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
      { blocks = out; return true; }
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
          { blocks = out; return true; }
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
          { blocks = out; return true; }
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
        { blocks = out; return true; }
      }
      if (!fnRaw.empty()) {
        doexec::ExecuteFileResult fres;
        const int callerPreserveLevel = session.preserveLevel();
        {
          // W2 doinclude (BUG-530): do/run get a PRIVATE local scope; include
          // does not. The guard lives HERE, not in executeDo, so the top-level
          // `--do`/GUI routes and `parallel do` keep today's session scope. The
          // args are seeded by executeDoSource into the fresh scope, so the
          // caller's `0'/`1'.. are no longer clobbered (do_args_private).
          std::optional<doexec::PrivateLocalScope> privateScope;
          // BUG-922: do/run also get their own preserve level; include runs
          // as if typed in its caller and shares the caller's (oracle
          // w2ad_prog_preserve_nest pn_i_pres_held: 621, pn_i_not).
          std::optional<DoFileLevelGuard> levelGuard;
          // BUG-920: and start outside any loop (pn_lm_cb_do).
          std::optional<program::LoopFloorGuard> loopFloor;
          // BUG-1459: a `version` the file sets ends with it.
          std::optional<CallerVersionScope> versionScope;
          if (!isInclude) {
            privateScope.emplace(session);
            levelGuard.emplace(session);
            loopFloor.emplace(session);
            versionScope.emplace(session);
          }
          try {
            fres = doexec::executeDo(doexec::withDefaultDoExtension(fnRaw), session, dispatch,
                                     doArgs);
          } catch (...) {
            // BUG-922: the exception path concludes the level too.
            if (!isInclude) program::concludeLevelAfterThrow(callerPreserveLevel, session);
            throw;
          }
        }
        // BUG-922: when a do/run file concludes, the copy it preserved and did
        // not restore is restored ([P] preserve), on error as on success
        // (w2ad_prog_preserve_nest pn_d_leave, pn_r_leave, pn_d_leave_err,
        // pn_d_leave_exit: ret 5:31, then the caller's restore is r(622)), and
        // its tempvars, tempnames and tempfiles die (pn_tv_do_gone: 111).
        ojson levelRestoreBlocks = ojson::array();
        if (!isInclude)
          program::concludeLevel(callerPreserveLevel, session, levelRestoreBlocks);
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
        for (auto& b : levelRestoreBlocks) out.push_back(std::move(b));
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
        { blocks = out; return true; }
      }
    }
  }
  return false;
}


bool interceptShellEscape(SessionState& session,
    ojson& blocks,
    const std::string& rawCmd) {


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
      { blocks = piet::dispatch("shell " + bm[1].str(), session); return true; }
    }
  }
  return false;
}

}
