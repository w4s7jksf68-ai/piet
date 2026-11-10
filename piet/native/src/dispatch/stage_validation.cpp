// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "expression_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {
bool resolveCommandOptions(std::string& cmd,
    eval::ParsedCommand& parsed,
    ojson& blocks) {


  // ------------------------------------------------------------------
  // 1a-ter. BUG-105 DoD ③ / FIX:OPTNET-ABBREV-SWEEP — THE OPTION-NAME
  // RESOLUTION POINT, the sibling of the command one above.
  //
  // Implements internal note BUG105-OPTABBREV-AUDIT-20260731.md §2's nine measured
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
  return false;
}


bool checkCommunityCommand(std::string& cmdTyped,
    ojson& blocks) {


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
        return true;
      }
    }
  }
  return false;
}


bool normalizeGtoolsAlias(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {


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
      return true;
    }
    parsed = tr.parsed;
    cmd = tr.canonical;  // cmdTyped keeps the user's spelling, for messages
  }
  return false;
}


bool countWithoutData(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {

  // BUG-786: with no data in memory Stata's `count if` counts zero observations
  // -- `clear` / `count if e(sample)` is 0 at rc 0 (w2_esample.log:1290 E15
  // |0|0|28|). PIET refused it with r(111) "no dataset". Only a condition that
  // names no variable is served (a constant, or e(sample) already rewritten to
  // `0` above); `count if x` still has no `x` to find, and keeps the refusal
  // (Stata: r(111) variable not found). BUG-1104: a BARE `count` is the same
  // zero (StataBE `clear all` / `count` prints 0, r(N) 0).
  if (!session.hasData() && cmd == "count" && parsed.varlist.empty() && parsed.opts.empty() &&
      !parsed.inRange.has_value()) {
    // A Unicode variable name (`count if قيمة > 2`) names a variable too.
    static const std::regex reIdentChar(R"([A-Za-z_]|[^\x00-\x7F])");
    if (!parsed.ifExpr.has_value() || !std::regex_search(*parsed.ifExpr, reIdentChar)) {
      session.eval.rValues.set("N", 0.0);
      eval::declareRPosted(session.eval, "count", {"N"});  // count's help
      ojson out;
      out["type"] = "display";
      out["text"] = "0";
      blocks.push_back(out);
      return true;
    }
  }
  return false;
}


bool assertWithoutData(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {

  // With no data in memory Stata's `assert` evaluates its claim once, as a
  // constant: `clear` / `assert 1==0` is r(9), `assert 601 == 601` rc 0, and a
  // claim naming a variable is r(111) (StataBE 19.5, checked 2026-10-21). PIET
  // refused every form as "no dataset", which stopped probe_b4 after a failed
  // `import excel ..., clear` had (correctly) emptied the data.
  if (!session.hasData() && cmd == "assert" && parsed.opts.empty() &&
      !parsed.inRange.has_value() && !parsed.ifExpr.has_value() && !parsed.varlist.empty()) {
    const std::string claim = genExprText(parsed);  // BUG-1433: a literal keeps its blanks
    std::string raised;
    bool isStr = false;
    auto v = eval::evalArith(claim, &session.eval, &raised, &isStr);
    if (v && !isStr) {
      if (*v == "0")
        blocks.push_back(ojson(eval::errorBlock("engine_error", "assertion is false", std::nullopt,
                                                std::nullopt, std::nullopt, "r(9)")));
      return true;
    }
  }
  return false;
}


bool parseEmptySave(std::string& cmd,
    eval::ParsedCommand& parsed,
    ojson& blocks,
    bool& saveEmptyOk) {

  if (cmd == "save" || cmd == "sa") {
    for (const auto& kv : parsed.opts)
      if (kv.first == "emptyok") saveEmptyOk = true;
  }
  return false;
}


bool allowEmptyGenerate(std::string& cmd,
    SessionState& session,
    ojson& blocks,
    bool needsNoData,
    bool& saveEmptyOk) {

  if (!session.hasData() && !needsNoData &&
      (cmd == "generate" || cmd == "gen" || cmd == "g" || saveEmptyOk)) {
    ojson so = ojson(handlers::runSetObs("0", session));
    if (so.is_object() && so.value("type", std::string()) == "error") {
      blocks.push_back(std::move(so));
      return true;
    }
  }
  return false;
}


bool validateEmptyDataset(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    bool needsNoData,
    bool& saveEmptyOk) {

  // BUG-1458: with nothing in memory Stata holds an empty dataset, not none.
  // StataBE 19.5, 2026-11-03, after `clear`: `summarize`, `summarize, detail`,
  // `summarize if 1` and `list` are rc 0 and print nothing; `summarize x` and
  // `list x` are r(111) "no variables defined"; `summarize in 1` and `list in
  // 1` are r(198) "observation numbers out of range"; `save f` without
  // emptyok is r(111) "no variables defined". An `if` is read but meets no
  // observation: `summarize if _n==1` and `list if missing(.)` are rc 0, while
  // `if nosuch==1` is r(111) "nosuch not found", `if (` r(132) and `if "a"`
  // r(109), as any expression fails.
  if (!session.hasData() && !needsNoData) {
    auto refuse = [&](const std::string& msg, const std::string& rc) {
      blocks.push_back(ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                              std::nullopt, rc)));
      return blocks;
    };
    if (cmd == "summarize" || cmd == "list") {
      if (!parsed.varlist.empty()) { blocks = refuse("no variables defined", "r(111)"); return true; }
      if (parsed.inRange.has_value()) { blocks = refuse("observation numbers out of range", "r(198)"); return true; }
      if (!parsed.ifExpr.has_value()) return true;
      // `_n` and `_N` are 0 here: no observation is there to number.
      static const std::regex reObsSpecial(R"(\b_[nN]\b)");
      const std::string cond = std::regex_replace(*parsed.ifExpr, reObsSpecial, "0");
      std::string raised;
      bool isStr = false;
      if (std::optional<std::string> v = eval::evalArith(cond, &session.eval, &raised, &isStr)) {
        if (isStr) { blocks = refuse("type mismatch", "r(109)"); return true; }
        return true;
      }
      eval::MacroFnError fe;
      if (eval::classifyFailedExpr(cond, raised, &fe) && fe.failed())
        { blocks = refuse(fe.message, "r(" + fe.rc + ")"); return true; }
      if (std::optional<std::string> name = firstBareName(cond))
        { blocks = refuse(*name + " not found", "r(111)"); return true; }
      { blocks = refuse("invalid syntax", "r(198)"); return true; }
    }
    if (cmd == "save" && !saveEmptyOk) { blocks = refuse("no variables defined", "r(111)"); return true; }
    // StataBE 19.5 after `clear` (with or without `set obs`): a bare `centile`
    // is rc 0 and prints its empty table header; `centile x` is r(111)
    // (measured on StataBE, D4-D6). runCentile answers both.
    if (cmd == "centile") {
      blocks.push_back(ojson(handlers::runCentile(parsed, session)));
      return true;
    }
  }
  return false;
}


bool requireCommandData(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    bool needsNoData) {

  if (!session.hasData() && !needsNoData) {
    // BUG-1437: a name the command table has no row for is Stata's r(199)
    // "unrecognized" whatever is in memory (StataBE `clear all` / `nosuchcmd`, or
    // a program `program drop _all` removed), which the last arm of dispatchImpl
    // answers once data exist. Every arm below this guard is a registry row
    // (beep and window, the two that are not, are in kNoDataCommands).
    if (registry::findByName(cmd) == nullptr) {
      blocks.push_back(ojson(eval::unsupportedCommand(
          parsed.cmd, std::nullopt, std::nullopt, "piet_gap",
          eval::UnsupportedCommandKind::kUnknownToken)));
      return true;
    }
    // BUG-1248: a postestimation verb reads e() before any data. With nothing in
    // memory and no estimates StataBE answers r(301) "last estimates not found"
    // (test, lincom, nlcom, margins, predict, every estat, lassocoef; esttab's
    // "last estimates (.) not found") and boottest its rc-0 "No estimates
    // detected"; each arm answers exactly that first, so with no e() live they
    // go on to it.
    static const std::set<std::string> kEstimatesFirst = {
        "test",  "lincom",   "nlcom",     "margins", "predict",
        "estat", "boottest", "lassocoef", "esttab"};
    if (kEstimatesFirst.count(cmd) == 0 || session.eval.eLive()) {
      blocks.push_back(ojson(eval::noDataLoaded()));
      return true;
    }
  }
  return false;
}


bool checkCommandArity(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    ojson& blocks) {


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
      return true;
    }
  }
  return false;
}

}
