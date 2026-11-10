// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {
bool route_environment(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {


  // ==================== AER-SCOPE1 bundle D (ENV) arms ====================
  // Placed beside erase/mkdir/rmdir/ssc/which, the verbs they are siblings of.
  if (cmd == "shell") {
    pushEnvResult(blocks, handlers::runShell(rawArgsAfterCmd(parsed.raw), session));
    return true;
  }

  if (cmd == "winexec") {
    blocks.push_back(ojson(eval::refusedCommand(cmdTyped, "no_os_process")));
    return true;
  }

  if (cmd == "type") {
    pushEnvResult(blocks, handlers::runType(parsed.raw, parsed.varlist, parsed.opts, session));
    return true;
  }

  if (cmd == "dir" || cmd == "ls") {
    pushEnvResult(blocks, handlers::runDir(parsed.raw, parsed.varlist, parsed.opts, session));
    return true;
  }

  if (cmd == "net") {
    pushEnvResult(blocks, handlers::runNet(parsed.raw, parsed.varlist, parsed.opts, session));
    return true;
  }

  if (cmd == "ado") {
    pushEnvResult(blocks, handlers::runAdo(parsed.raw, parsed.varlist));
    return true;
  }

  if (cmd == "adopath") {
    pushEnvResult(blocks, handlers::runAdopath(parsed.raw, parsed.varlist, session));
    return true;
  }

  if (cmd == "sysdir") {
    pushEnvResult(blocks, handlers::runSysdir(parsed.raw, parsed.varlist, session));
    return true;
  }

  if (cmd == "findfile") {
    pushEnvResult(blocks, handlers::runFindfile(parsed.raw, parsed.varlist, parsed.opts, session));
    return true;
  }

  if (cmd == "sleep") {
    pushEnvResult(blocks, handlers::runSleep(parsed.varlist));
    return true;
  }

  if (cmd == "error") {
    // `error exp`: the code is an expression too, as for exit (BUG-1462) --
    // `capture error 459` / `error _rc` raises r(459) (StataBE). A literal
    // still goes straight through; anything else is evaluated first.
    // StataBE 19.5, 2026-11-03: the code is rounded (`error 3.7` raises r(4)),
    // a code below 0 or above 99999, or a missing one, is r(100000) with no
    // message (`error 12345678`, `error 1e6`), and a bare `error` ("invalid
    // syntax"), a name that is no value ("abc not found") or a string ("type
    // mismatch") is r(197). A leading `=` is read as the start of a name:
    // `error = 3+2` is "=3 invalid name" r(197).
    std::vector<std::string> codeArg = parsed.varlist;
    const std::string arg = trimWs(parsed.argText);
    auto refuse197 = [&](const std::string& msg) {
      blocks.push_back(ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                              std::nullopt, std::string("r(197)"))));
      return blocks;
    };
    if (arg.empty()) { blocks = refuse197("invalid syntax"); return true; }
    if (arg[0] == '=') {
      std::smatch nm;
      const std::string after = trimWs(arg.substr(1));
      std::regex_search(after, nm, std::regex(R"(^[A-Za-z0-9_]*)"));
      { blocks = refuse197("=" + nm.str(0) + " invalid name"); return true; }
    }
    auto clampCode = [](double d) {
      const long long rounded = std::llround(d);
      return std::to_string(rounded < 0 || rounded > 99999 ? 100000LL : rounded);
    };
    if (!std::regex_match(arg, std::regex(R"([+-]?[0-9]+)"))) {
      bool isStr = false;
      std::optional<std::string> v = eval::evalArith(arg, &session.eval, nullptr, &isStr);
      if (!v.has_value()) {
        if (std::regex_match(arg, std::regex(R"([A-Za-z_][A-Za-z0-9_]*)")))
          { blocks = refuse197(arg + " not found"); return true; }
        { blocks = refuse197("invalid syntax"); return true; }
      }
      if (isStr) { blocks = refuse197("type mismatch"); return true; }
      double d = 0.0;
      bool numeric = false;
      try {
        size_t used = 0;
        d = std::stod(*v, &used);
        numeric = used == v->size() && std::isfinite(d);
      } catch (...) {
      }
      if (!v->empty() && (*v)[0] == '.') {
        codeArg = {"100000"};
      } else if (numeric) {
        codeArg = {clampCode(d)};
      } else {
        codeArg = {*v};
      }
    } else {
      codeArg = {clampCode(std::stod(arg))};
    }
    pushEnvResult(blocks, handlers::runErrorCode(codeArg));
    return true;
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
    return true;
  }

  if (cmd == "cls") {
    // Nothing to clear: PIET's Results pane is the block stream itself, and a
    // command that erased it would erase its own evidence. Silent accept.
    return true;
  }

  if (cmd == "gr_edit") {
    ojson note;
    note["type"] = "display";
    note["text"] = "(gr_edit: graph-editor recording, appearance only -- ignored)";
    blocks.push_back(note);
    return true;
  }

  if (cmd == "bcal") {
    // AER scope-2 B2-3 (2026-09-09): served (it was a refusal). Same
    // array-or-object shape rule as the H1 arms.
    ojson r_bc = handlers::runBcal(parsed, session);
    if (r_bc.is_array()) {
      for (const auto& b : r_bc) blocks.push_back(b);
    } else if (!r_bc.is_null()) {
      blocks.push_back(r_bc);
    }
    return true;
  }

  if (cmd == "twoway__histogram_gen") {
    // gfx-histogram-kdensity-family: served (it was refused as a Stata
    // internal). An array of blocks on success, one error object otherwise.
    ojson r_hg = handlers::runTwowayHistogramGen(parsed, session);
    if (r_hg.is_array()) {
      for (const auto& b : r_hg) blocks.push_back(b);
    } else if (!r_hg.is_null()) {
      blocks.push_back(r_hg);
    }
    return true;
  }

  if (cmd == "frames") {
    std::string sub = parsed.varlist.empty() ? "" : lower(parsed.varlist[0]);
    if (sub == "reset") {
      pushEnvResult(blocks, handlers::runFramesReset(session));
      return true;
    }
    // Every other sub-command is `frame` under its plural spelling. Rewriting
    // and re-dispatching keeps ONE implementation of the frame family rather
    // than a second copy that can drift from it.
    std::string rewritten = parsed.raw;
    size_t at = rewritten.find("frames");
    if (at != std::string::npos) rewritten.replace(at, 6, "frame");
    { blocks = piet::dispatch(rewritten, session); return true; }
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
    return true;
  }

  if (cmd == "beep") {
    // Accept-note, not silence: `beep` is a real built-in that makes a sound we
    // have no way to make, and a do-file that beeps between steps must keep
    // running. Saying so once beats a user believing something happened.
    ojson note;
    note["type"] = "display";
    note["text"] = "(beep: this build makes no sound — accepted and ignored)";
    blocks.push_back(note);
    return true;
  }

  if (cmd == "window") {
    // Same shape. The corpus form is `window manage close graph _all`, a GUI
    // instruction with no headless meaning.
    ojson note;
    note["type"] = "display";
    note["text"] = "(window: GUI window management does not apply in this build — accepted and ignored)";
    blocks.push_back(note);
    return true;
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
      { blocks = note("(parallel clean: this build runs sequentially and leaves no cluster "
                  "scratch files -- accepted and ignored)"); return true; }
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
      { blocks = note("(parallel " + head + " " + n +
                  ": recorded. This build runs the work sequentially in this process, so the "
                  "cluster count changes no answer)"); return true; }
    }
    if (head == "break" || head == "version" || head == "printlog" || head == "viewlog" ||
        head == "bs" || head == "sim") {
      { blocks = refuse("parallel " + head +
                        " needs a running cluster, and this build has none: the package cannot "
                        "start a child process here (`parallel setclusters` answers r(601) even "
                        "with statapath()). Refused rather than approximated.",
                    199); return true; }
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
      { blocks = refuse("-" + head + "- invalid parallel subcommand (help parallel)", 198); return true; }
    }
    if (head == "append") {
      { blocks = refuse("parallel append reads a list of .dta files, runs a command over each and "
                    "pastes the results together. Its file-list expansion is implemented and "
                    "measured, but the per-file run goes through the cluster machinery this "
                    "build cannot start, so the whole verb is refused rather than half-done.",
                    199); return true; }
    }

    const std::string* clusters = session.eval.globals.find("PLL_CLUSTERS");
    if (clusters == nullptr || clusters->empty()) {
      { blocks = refuse("You haven't set the number of clusters. Please set it with: parallel "
                    "setclusters #",
                    198); return true; }
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
        { blocks = refuse(std::string("parallel: option ") + bad +
                          " changes what each cluster receives, and this build runs the work "
                          "sequentially in one process. Refused rather than accepted-and-ignored.",
                      198); return true; }
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
      { blocks = refuse("parallel: without `nodata` the package SPLITS the rows across " +
                        std::to_string(nClusters) +
                        " clusters, runs the file on each slice and pastes the results back. "
                        "What each run sees depends on the split, and the package cannot be run "
                        "here to check a sequential reproduction against. Use `nodata`, or "
                        "processors(1), or run the file directly.",
                    199); return true; }
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
    //
    // BUG-922: the snapshot sits one preserve level below the caller, so a
    // caller that holds its own preserve does not make it r(621) (the restore
    // below would then have popped the CALLER's copy), and the child runs one
    // level deeper still: it is a separate Stata session that may preserve,
    // and whatever it leaves preserved is restored when it ends. Every frame
    // below the child's level -- the snapshot and the caller's copies -- is
    // out of its reach (SessionState::ownPreserveFrame): its `restore` in any
    // form is r(622) and touches neither.
    bool snapped = false;
    std::optional<DoFileLevelGuard> snapLevel;
    if (noData && hasRows) {
      snapLevel.emplace(session);
      ojson pres = handlers::runPreserve(session);
      if (pres.is_object() && pres.value("type", std::string()) == "error") {
        // No snapshot, no run: clearing the data now would lose it.
        blocks.push_back(std::move(pres));
        return true;
      }
      snapped = true;
      (void)dispatch("clear", session);
    }
    doexec::ExecuteResult r;
    {
      const int parentPreserveLevel = session.preserveLevel();
      {
        DoFileLevelGuard childLevel(session);
        program::LoopFloorGuard loopFloor(session);  // BUG-920: a separate session
        r = isDo ? doexec::executeDo(body, session, dispatch).result
                 : doexec::executeDoSource(body, session, dispatch);
      }
      program::concludeLevel(parentPreserveLevel, session, r.blocks);
    }
    for (const auto& b : r.blocks) blocks.push_back(b);
    if (snapped) {
      // BUG-922: a failed snapshot restore is reported, never dropped.
      ojson res = handlers::runRestore(session);
      if (res.is_object() && res.value("type", std::string()) == "error")
        blocks.push_back(std::move(res));
    }
    return true;
  }
  return false;
}

}
