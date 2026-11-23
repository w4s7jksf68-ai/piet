// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include <set>
#include "expression_support.h"
#include "prefix_support.h"
#include "results_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
#include "piet/handlers/descriptive.h"
#include "piet/handlers/estimates_tables.h"
#include "piet/handlers/results.h"
#include "piet/program.h"
namespace piet::dispatch_detail {
bool route_results(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    const std::string& rawCmd) {


  if (cmd == "display") {
    // BUG-105/WS-K: cmdTyped, NOT cmd. This finds the command token inside the
    // raw line; with the canonical spelling, `dis 2+2` would search for
    // "display" in "dis 2+2", get npos, and index off the end of the string.
    // BUG-251/WS-Q: stop searching entirely — the args are simply what follows
    // the leading token. `dis(2+2)` still yields `(2+2)`: rawArgsAfterCmd ends
    // the token at the first non-identifier character, exactly as the
    // `find(cmdTyped) + size()` arithmetic did.
    std::string rawArgs = rawArgsAfterCmd(parsed.raw);
    ojson shown = ojson(handleDisplay(rawArgs, session));
    // What the line shows: its SMCL drawn as text (renderSmclText), unless
    // `_asis` turned SMCL off.
    int linesize = 80;
    if (auto ls = resolveCValue("linesize", session); ls.has_value() && !ls->isString) {
      try {
        linesize = std::stoi(ls->text);
      } catch (...) {
      }
    }
    // The SMCL as typed stays in `smcl` for an SMCL log, which writes it as
    // SMCL (session_logs.cpp); a text log, the console and the app show `text`.
    if (shown.value("type", std::string()) == "display" && shown.contains("text") &&
        shown["text"].is_string() && !shown.value("asis", false)) {
      const std::string typed = shown["text"].get<std::string>();
      const std::string drawn = renderSmclText(typed, linesize);
      if (drawn != typed) {
        shown["text"] = drawn;
        shown["smcl"] = typed;
      }
    }
    // Carry the width to the display and text-log renderers. Keep the
    // original string in the result block for scripts and other consumers.
    if (shown.value("type", std::string()) == "display" &&
        (shown.value("continue", false) || shown.value("text", std::string()).size() >
                                             static_cast<size_t>(linesize)))
      shown["linesize"] = linesize;
    shown.erase("asis");
    if (shown.value("type", std::string()) == "error") {
      // The pieces printed before the failing one (handleDisplay's partial_text).
      if (isDisplayLine(rawCmd)) {
        if (auto partial = displayPartialBlock(rawArgsAfterCmd(rawCmd), shown, session, true)) {
          if (partial->contains("text") && (*partial)["text"].is_string())
            (*partial)["text"] = renderSmclText((*partial)["text"].get<std::string>(), linesize);
          blocks.push_back(*partial);
        }
      }
      shown.erase("partial_text");
    }
    blocks.push_back(std::move(shown));
    return true;
  }


  if (cmd == "scalar" && !parsed.varlist.empty() &&
      (lower(parsed.varlist[0]) == "list" || lower(parsed.varlist[0]) == "dir")) {
    std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
    blocks.push_back(ojson(handlers::runScalarList(names, session)));
    return true;
  }


  // `assert EXP [if] [in]` (P4 wave). The claim is the RAW text after the
  // command word, not the tokenized varlist: `assert x1 > 0 & x2 < 5` is one
  // expression, and rebuilding it from tokens would lose the user's spacing
  // inside string comparisons. parsed.raw is the same source the `if`-qualifier
  // scanner already worked from, so the two agree on where the claim ends.
  if (cmd == "assert") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {"rc0", "null", "fast"})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    bool assertRc0 = false, assertNull = false, assertFast = false;
    for (const auto& [k, v] : parsed.opts) {
      const bool on = v.isBool ? v.boolValue : true;
      if (k == "rc0" && on) assertRc0 = true;
      if (k == "null" && on) assertNull = true;
      if (k == "fast" && on) assertFast = true;
    }
    // EM11: a claim holding a compound-quoted literal (a string with a '"' in it)
    // is a constant the engine's lexer cannot read; the expression layer can.
    // `assert `"`a1'"' != ""` is rc 0 and the == form r(9) (w2af_macro_text_file.log
    // :1366, :1377). Only a whole-raw-claim with no qualifier takes this path.
    if (!parsed.ifExpr && !parsed.inRange && parsed.raw.find("`\"") != std::string::npos) {
      std::string rawClaim = trimWs(parsed.raw);
      const size_t sp = rawClaim.find_first_of(" \t");
      rawClaim = sp == std::string::npos ? std::string() : trimWs(rawClaim.substr(sp));
      std::string raised;
      bool isStr = false;
      auto v = eval::evalArith(rawClaim, &session.eval, &raised, &isStr);
      if (v && !isStr) {
        if (*v == "0") {
          blocks.push_back(ojson(eval::errorBlock("engine_error", "assertion is false",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  "r(9)")));
        }
        return true;
      }
    }
    // BUG-428/432: `assert EXPR` carries its claim in the VARLIST, not in an
    // `if`, so the substitution point above never saw it — which is how
    // `assert Q[1,1] == 1` reported an unknown VARIABLE for a defined matrix.
    // BUG-1433: the claim is genExprText's, as generate/replace's expression is
    // (BUG-904): rebuilt from the blank-split varlist, `assert s == "a  b"`
    // compared s with "a b".
    std::optional<ojson> claimErr;
    std::string claim = substituteRowExprRefs(genExprText(parsed), session, &claimErr);
    if (claimErr.has_value()) {  // BUG-456: `assert _b[nosuch]==0` r(111), `assert e(cmd)==1` r(109)
      blocks.push_back(*claimErr);
      return true;
    }
    claim = substituteStringCells(claim, session);  // BUG-1327 / BUG-1093 (the `if`'s: above)
    // The cells went in after substituteRowExprRefs folded the constant string
    // compares, so the ones they made (`"ab" == "ab"`) are folded here: left as
    // text they sent `assert x[1] == 1 & s[1] == "ab"` -- a subscript, which
    // only the engine reads, beside a string compare, which it cannot -- to rc
    // 1 where StataBE is rc 0.
    for (std::string* part : {&claim, parsed.ifExpr.has_value() ? &*parsed.ifExpr : nullptr}) {
      std::string mismatch;
      if (part != nullptr && !foldRowConstStringCompares(*part, &mismatch)) {
        blocks.push_back(ojson(eval::errorBlock(
            "engine_error", "type mismatch: `" + mismatch + "` compares a string with a number",
            std::nullopt, std::nullopt, std::nullopt, "r(109)")));
        return true;
      }
    }
    if (claim.empty()) {
      blocks.push_back(ojson(eval::engineError("assert: no expression given", "syntax_error")));
      return true;
    }
    // [D] assert: `null` makes an assertion that checks no observation (an if / in that excludes
    // them all) return code 8, where it is 0 by default.
    if (assertNull && (parsed.ifExpr.has_value() || parsed.inRange.has_value())) {
      const std::string savedLeaf = session.leafCmdExpanded;
      std::string countLine = "quietly count";
      if (parsed.ifExpr.has_value()) countLine += " if " + *parsed.ifExpr;
      if (parsed.inRange.has_value()) countLine += " in " + *parsed.inRange;
      const eval::OrderedFloatDict savedR = session.eval.rValues;
      (void)dispatch(countLine, session);
      const double* nSample = session.eval.rValues.find("N");
      const bool emptySample = nSample != nullptr && *nSample == 0;
      session.eval.rValues = savedR;
      session.leafCmdExpanded = savedLeaf;
      if (emptySample) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "assertion is null", std::nullopt,
                                                std::nullopt, std::nullopt, "r(8)")));
        return true;
      }
    }
    ojson ar = handlers::runAssert(claim, session, parsed.ifExpr, parsed.inRange);
    // fast stops at the first contradiction, so it has no count to report; rc0 reports the
    // assertion as false and returns 0.
    if (ar.is_object() && ar.value("type", std::string()) == "error" &&
        ar.value("reason", std::string()) == "r(9)" &&
        ar.value("message", std::string()).find("assertion is false") != std::string::npos) {
      std::string msg = ar.value("message", std::string());
      if (assertFast) msg = "assertion is false";
      if (assertRc0) {
        ojson shown;
        shown["type"] = "display";
        shown["text"] = msg;
        ar = std::move(shown);
      } else if (assertFast) {
        ar["message"] = msg;
      }
    }
    // Silent success returns an empty array; pushing that would put a non-block
    // into the block list (same guard the `matrix` arm above uses).
    if (!(ar.is_array() && ar.empty())) blocks.push_back(ar);
    return true;
  }


  // BS-039 (BETA campaign C3): `ereturn list` — display-only e() dump
  // (results/scalar_results.cpp::runEreturnList).
  // W2D-T01: every other sub-verb goes to handlers::runEreturn
  // (results/ereturn_commands.cpp: clear/post/local, a named refusal for the rest).
  // `ereturn` is on kNoDataCommands because `ereturn clear` is the first line
  // of a do-file helper that runs before anything is loaded
  // (w2d_uncovered.log 211-212); `ereturn list` keeps the no-data answer it
  // had while the whole command sat behind the guard.
  if (cmd == "ereturn") {
    if (!parsed.varlist.empty() && lower(parsed.varlist[0]) == "list") {
      if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
        blocks.push_back(ojson(*err));
        return true;
      }
      // With no data in memory, as with no estimates, there is nothing to list:
      // StataBE prints nothing at rc 0 (runEreturnList answers an empty array).
      ojson listed = ojson(handlers::runEreturnList(session));
      if (!(listed.is_array() && listed.empty())) blocks.push_back(std::move(listed));
      return true;
    }
    // One block or an empty array (silent success) -- never pushed as an array.
    ojson r = handlers::runEreturn(parsed, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return true;
  }


  // `sreturn list` and `sreturn clear` outside an sclass program (`sreturn
  // local` is program.cpp's). Both had no row at all and answered r(199) "not
  // supported", so `capture noisily sreturn list` failed where StataBE lists
  // s() -- or prints nothing when s() is empty -- at rc 0.
  if (cmd == "sreturn") {
    const std::string sub = parsed.varlist.empty() ? "" : lower(parsed.varlist[0]);
    if (sub == "list" || sub == "clear") {
      if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
        blocks.push_back(ojson(*err));
        return true;
      }
      if (parsed.varlist.size() > 1) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax", std::nullopt,
                                                std::nullopt, std::nullopt, "r(198)")));
        return true;
      }
      if (sub == "clear") {
        handlers::clearSreturn(session);
        return true;
      }
      ojson listed = ojson(handlers::runSreturnList(session));
      if (!(listed.is_array() && listed.empty())) blocks.push_back(std::move(listed));
      return true;
    }
    // Any other form is one PIET does not implement, named as an unknown
    // subcommand (the `return` arm's wording below).
    blocks.push_back(ojson(eval::unsupportedCommand(
        "sreturn " + sub, std::nullopt, std::nullopt, "piet_gap",
        eval::UnsupportedCommandKind::kUnknownSubcommand)));
    return true;
  }


  // ==================== AER-SCOPE1 bundle B (RESULTS) arms ================
  if (cmd == "return") {
    // li:st, sca:lar, loc:al, mat:rix are the documented abbreviations ([P] return).
    std::string sub =
        parsed.varlist.empty() ? "" : program::canonicalReturnSubcommand(parsed.varlist[0]);
    if (sub == "list") {
      if (auto err = eval::rejectUnknownOptions(parsed.opts, {"all"}, {})) {
        blocks.push_back(ojson(*err));
        return true;
      }
      ojson listed = ojson(handlers::runReturnList(session, !parsed.opts.empty()));
      if (!(listed.is_array() && listed.empty())) blocks.push_back(std::move(listed));
      return true;
    }
    if (sub == "clear") {
      // `return clear` clears r() ONLY. rStrings doubles as the s() store
      // (`sreturn local` writes there under kSClassKeyPrefix) and `sreturn clear`
      // is its own command, so the reserved \x01 keys stay: eval::replaceRClass
      // is that shape, the one copy every wholesale r() replacement uses.
      // Outside a program there is nothing to return from: `capture return clear`
      // is rc 0 and the previous r() survives (w2o_rnames.log:528 N34 `0|60|60`).
      if (session.progCallDepth == 0) return true;
      eval::replaceRClass(session.eval);
      eval::declareRPosted(session.eval, "return clear", {});  // r() is empty (D.3.13)
      // It also drops what this program has already returned ([P] return:
      // "return clear clears r()", the staged results included): a `return
      // scalar/local/matrix` before it must not reach the caller. e()/s()
      // staging (escalar, slocal) is not r() and stays.
      auto& mats = session.eval.matrices;
      for (auto it = session.returnStaging.begin(); it != session.returnStaging.end();) {
        const std::string& kind = it->second.first;
        if (kind == "scalar" || kind == "local" || kind == "matrix") {
          if (kind == "matrix") {
            const std::string key = it->second.second;
            mats.erase(std::remove_if(mats.begin(), mats.end(),
                                      [&](const auto& kv) { return kv.first == key; }),
                       mats.end());
          }
          it = session.returnStaging.erase(it);
        } else {
          ++it;
        }
      }
      return true;
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
      return true;
    }
    // `return scalar|local` reaches program::handleReturnStmt long before this
    // arm (dispatcher step (e), above), so anything still here is a form PIET
    // does not implement -- named as an unknown SUBCOMMAND, because the command
    // itself now exists. A word that is none of return's subcommands is
    // Stata's r(198) "invalid syntax" (vs-stata bank `return , zz` 30853e37,
    // `return zznotasub a = 1` 5d83cfd6).
    {
      static const std::set<std::string> kReturnSubs = {"list", "clear", "add", "scalar",
                                                        "local", "matrix", "hidden",
                                                        "historical"};
      if (kReturnSubs.count(sub) == 0) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax", std::nullopt,
                                                std::nullopt, std::nullopt, "r(198)")));
        return true;
      }
    }
    blocks.push_back(ojson(eval::unsupportedCommand(
        "return " + sub, std::nullopt, std::nullopt, "piet_gap",
        eval::UnsupportedCommandKind::kUnknownSubcommand)));
    return true;
  }

  if (cmd == "matlist") {
    blocks.push_back(ojson(handlers::runMatlist(parsed, session)));
    return true;
  }

  if (cmd == "svmat") {
    {
      // svmat takes no qualifier or weight, and reads whatever follows the matrix as a matrix
      // name too: `svmat M if ...`, `svmat M in 1/2`, `svmat M [aw=w]`, a bare `svmat` and
      // `svmat , opt` are all r(111) "matrix <word> not found" (vs-stata bank).
      std::string word;
      bool refused = false;
      if (parsed.ifExpr.has_value()) { word = "if"; refused = true; }
      else if (parsed.inRange.has_value()) { word = "in"; refused = true; }
      else if (parsed.weightType.has_value()) {
        word = "[" + *parsed.weightType + "=" + parsed.weightVar.value_or("") + "]";
        refused = true;
      } else if (parsed.varlist.empty()) {
        if (!parsed.opts.empty()) word = parsed.opts.front().first;
        refused = true;
      }
      if (refused) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "matrix " + word + " not found",
                                                std::nullopt, std::nullopt, std::nullopt, "r(111)")));
        return true;
      }
    }
    blocks.push_back(ojson(handlers::runSvmat(parsed, session)));
    return true;
  }

  if (cmd == "getmata") {
    // runGetmata returns exactly one object (the data_mutation card or one
    // error block), never an array.
    blocks.push_back(ojson(handlers::runGetmata(parsed, session)));
    return true;
  }

  if (cmd == "mkmat") {
    // Silent on success, so the empty array must be FILTERED, not pushed: an
    // array reaching the block stream is read with value("type") downstream and
    // throws type_error.306. This is the same defect class that aborted the
    // selftest binary in bundle D, recurring here on a fresh arm -- which is why
    // every silent arm in this file carries this exact guard.
    ojson r = handlers::runMkmat(parsed, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return true;
  }


  if (cmd == "matrix" || cmd == "mat") {
    // BUG-105/WS-K: runMatrixDef re-parses the raw line with its own
    // `mat(?:rix)?` patterns, so hand it a canonical leading token.
    ojson r = handlers::runMatrixDef(rawWithCanonicalCmd(parsed.raw, cmdTyped, cmd), session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);  // silent on success
    return true;
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
      return true;
    }
    // estimates.ado Change: `estimates change [name], title(...)` rewrites a stored model's title
    // (vs-stata bank 76a85d8f: `estimates dir` then lists `renamed`).
    if ((cmd == "est" || cmd == "estimates") && !parsed.varlist.empty() &&
        lower(parsed.varlist[0]) == "change") {
      std::optional<std::string> newTitle;
      for (const auto& [k, v] : parsed.opts) {
        if (k == "title" && !v.isBool) {
          newTitle = dequoteEstTitle(v.strValue);
        } else {
          blocks.push_back(ojson(eval::errorBlock("unsupported_option", "option `" + k + "` not allowed")));
          return true;
        }
      }
      std::vector<std::string> ops(parsed.varlist.begin() + 1, parsed.varlist.end());
      SessionState::EstNamelist ex =
          session.expandEstimatesNamelist(ops.empty() ? std::vector<std::string>{"."} : ops);
      if (ex.rc != 0) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", ex.message, std::nullopt, std::nullopt,
                                                std::nullopt, "r(" + std::to_string(ex.rc) + ")")));
        return true;
      }
      if (ex.names.size() != 1) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "too many names specified", std::nullopt,
                                                std::nullopt, std::nullopt, std::string("r(198)"))));
        return true;
      }
      if (!newTitle.has_value()) return true;  // nothing to change
      if (ex.names[0] == ".") {
        blocks.push_back(ojson(eval::unsupportedOption(
            "change", "estimates change of the active, unstored results is not implemented in this version of PIET")));
        return true;
      }
      if (ojson* sn = session.estimatesGetMutable(ex.names[0])) (*sn)["title"] = *newTitle;
      return true;
    }
    if ((cmd == "est" || cmd == "estimates") && !parsed.varlist.empty() &&
        lower(parsed.varlist[0]) == "stats") {
      std::vector<std::string> statNames(parsed.varlist.begin() + 1, parsed.varlist.end());
      blocks.push_back(ojson(handlers::runEstimatesStats(statNames, parsed.opts, session)));
      return true;
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
        // are active now)", then e(df_m)/_b[a] are m1's). The operand is a
        // namelist that must expand to exactly one name, so `m*` matching
        // one name works and matching two is r(198) "too many names
        // specified"; a miss is r(111). A BARE `est restore` is answered by
        // StataBE with the literal "estimates __000000 not found" r(111)
        // (est_restore_bare).
        // `.` (the active results, unstored) is a no-op.
        if (rejectFor({})) return true;
        auto rcBlock = [](const std::string& msg, int rc) {
          return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                        std::nullopt, "r(" + std::to_string(rc) + ")"));
        };
        if (estOperands.empty()) {
          blocks.push_back(rcBlock("estimates __000000 not found", 111));
          return true;
        }
        SessionState::EstNamelist ex = session.expandEstimatesNamelist(estOperands);
        if (ex.rc == 0 && ex.names.size() > 1) {
          ex.rc = 198;
          ex.message = "too many names specified";
        } else if (ex.rc == 0 && ex.names.empty()) {
          // `_all` over an empty store expands to nothing (StataBE: r(198)).
          ex.rc = 198;
          ex.message = "too few names specified";
        }
        if (ex.rc != 0) {
          blocks.push_back(rcBlock(ex.message, ex.rc));
          return true;
        }
        const std::string rname = ex.names.front();
        // Always re-activated from the store, even when NAME is the name the
        // active results carry: identity is decided inside estimatesRestore
        // (a model-less store is a no-op only when its block IS the live one).
        // eb-order T-P3: an estpost store (a block, no fitted model) is made
        // active again as the posting it was (SessionState::estimatesRestore);
        // only a store holding neither is refused.
        // e(sample) T3: runEstimatesRestore also brings the mask back.
        if (rname != ".") {
          blocks.push_back(ojson(handlers::runEstimatesRestore(rname, session)));
          return true;
        }
        ojson note;
        note["type"] = "display";
        note["text"] = "(results " + rname + " are active now)";
        blocks.push_back(std::move(note));
        return true;
      }
      // e(sample) T3: `estimates esample` / `estimates esample: [varlist] [if]
      // [in] [, replace]` (the full word, no abbreviation;
      // w2_esample.log:1542 G7 rc 0). The colon arrives glued to the word or as
      // its own token.
      if (sub == "esample" || sub == "esample:") {
        if (rejectFor({"replace"})) return true;
        bool colon = sub == "esample:";
        std::vector<std::string> vars;
        for (const auto& t : estOperands) {
          if (!colon && t == ":") {
            colon = true;
            continue;
          }
          if (!colon && !t.empty() && t.front() == ':') {
            colon = true;
            if (t.size() > 1) vars.push_back(t.substr(1));
            continue;
          }
          vars.push_back(t);
        }
        bool replace = false;
        for (const auto& [k, v] : parsed.opts) {
          if (k == "replace") replace = !v.isBool || v.boolValue;
        }
        const bool hasWeight = parsed.weightType.has_value() && parsed.weightVar.has_value();
        blocks.push_back(ojson(handlers::runEstimatesEsample(colon, vars, parsed.ifExpr,
                                                             parsed.inRange, hasWeight, replace,
                                                             session)));
        return true;
      }
      if (sub == "store") {
        if (rejectFor({"title"})) return true;
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
        pushEstStoreMutation(blocks.back(), blocks, session);
        return true;
      }
      if (sub == "save" || sub == "use") {
        if (rejectFor(sub == "save" ? std::unordered_set<std::string>{"replace"}
                                    : std::unordered_set<std::string>{}))
          return true;
        if (parsed.varlist.size() < 2) {
          blocks.push_back(ojson(eval::errorBlock(
              "unsupported_option",
              "estimates " + sub + ": a file name is required (`estimates " + sub +
                  " \"myest\"`).")));
          return true;
        }
        bool replace = false;
        for (const auto& [k, v] : parsed.opts) {
          if (k == "replace") replace = !v.isBool || v.boolValue;
        }
        blocks.push_back(sub == "save"
                             ? ojson(handlers::runEstimatesSave(parsed.varlist[1], replace, session))
                             : ojson(handlers::runEstimatesUse(parsed.varlist[1], session)));
        return true;
      }
      if (sub == "dir") {
        if (rejectFor({})) return true;
        blocks.push_back(ojson(handlers::runEststoDirNames(estOperands, session)));
        return true;
      }
      if (sub == "drop" || sub == "clear") {
        if (rejectFor({})) return true;
        std::optional<std::string> name;
        if (!estOperands.empty()) name = joinSpace(estOperands);
        blocks.push_back(ojson(handlers::runEststoManage(sub, name, session)));
        return true;
      }
      if (rejectFor({})) return true;
      // `estimates` with no subcommand replays the active results when there are any and does
      // nothing otherwise, rc 0 either way (estimates.ado: an empty key runs Replay "." only when
      // e(cmd) is set). The replay is `estimates replay`, which PIET does not serve: it keeps
      // that subcommand's named refusal.
      if (parsed.varlist.empty()) {
        if (session.eval.eStrings.get("cmd", "").empty()) return true;
        blocks.push_back(ojson(eval::unsupportedCommand(
            cmdTyped + " replay", std::nullopt, std::nullopt, "piet_gap",
            eval::UnsupportedCommandKind::kUnknownSubcommand)));
        return true;
      }
      // A word that opens none of estimates' subcommands is Stata's r(198) "subcommand estimates
      // zzz is unrecognized" (vs-stata bank c6aee8b5); the real ones PIET does not serve keep
      // the named refusal below.
      {
        std::string word = sub;
        while (!word.empty() && word.back() == ':') word.pop_back();
        static const char* const kEstimatesSubs[] = {
            "store", "restore", "query", "dir", "drop", "clear", "save", "use", "describe",
            "esample", "table", "stats", "replay", "title", "notes", "for", "change",
            "selected"};
        bool opens = false;
        for (const char* full : kEstimatesSubs)
          if (!word.empty() && std::string(full).rfind(word, 0) == 0) opens = true;
        if (!opens) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                                  "subcommand estimates " + sub + " is unrecognized",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  "r(198)")));
          return true;
        }
      }
      blocks.push_back(ojson(eval::unsupportedCommand(
          cmdTyped + " " + sub, std::nullopt, std::nullopt, "piet_gap",
          eval::UnsupportedCommandKind::kUnknownSubcommand)));
      return true;
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
        return true;
      }
      if (!managing && parsed.varlist.size() > 1) {
        blocks.push_back(ojson(eval::engineError(
            "eststo: `" + joinSpace(parsed.varlist) + "` is not one name -- `eststo [name] "
            "[, options]` stores the active estimates under ONE name",
            "syntax_error")));
        return true;
      }
    }
    // eststo [NAME] | eststo dir | eststo drop|clear [NAME]
    if (!parsed.varlist.empty()) {
      std::string sub0 = lower(parsed.varlist[0]);
      if (sub0 == "dir" || sub0 == "drop" || sub0 == "clear") {
        if (rejectFor({})) return true;
        if (sub0 == "dir") {
          blocks.push_back(ojson(handlers::runEststoDirNames(
              std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end()),
              session)));
          return true;
        }
        std::optional<std::string> name;
        if (parsed.varlist.size() > 1) {
          name = joinSpace(std::vector<std::string>(parsed.varlist.begin() + 1,
                                                    parsed.varlist.end()));
        }
        blocks.push_back(ojson(handlers::runEststoManage(sub0, name, session)));
        return true;
      }
    }
    EststoStoreOpts storeOpts;
    if (auto err = readEststoStoreOpts(parsed.opts, &storeOpts)) {
      blocks.push_back(*err);
      return true;
    }
    std::optional<std::string> name;
    if (!parsed.varlist.empty()) name = parsed.varlist[0];
    std::vector<std::pair<std::string, double>> addedSc;
    if (storeOpts.addscalars.has_value()) {
      std::vector<ojson> notes;
      const auto aerr = applyEststoAddscalars(*storeOpts.addscalars, session, &notes, &addedSc);
      for (auto& nb : notes) blocks.push_back(std::move(nb));
      if (aerr.has_value()) {
        blocks.push_back(*aerr);
        return true;
      }
    }
    blocks.push_back(
        ojson(handlers::runEststo(name, storeOpts.title, session, storeOpts.autoPrefix)));
    pushEstStoreMutation(blocks.back(), blocks, session);
    for (const auto& [k, v] : addedSc) {
      if (ojson* sn = session.estimatesGetMutable(session.activeEstimatesName)) {
        if (sn->contains("escalars") && (*sn)["escalars"].is_object()) (*sn)["escalars"][k] = v;
      }
    }
    return true;
  }

  if (cmd == "tabout") {
    // Refused at the COMMAND level, not the option level: tabout's grammar is a
    // panel language of its own, and accepting the head while refusing every
    // option would report a missing option where the truth is a missing
    // command. `refusedCommand` (not `engineError`) so `eval::coarseRc` answers
    // 199 -- the default code answers 1, which is bundle D's shipped defect.
    blocks.push_back(ojson(eval::refusedCommand(cmdTyped, "tabout_grammar")));
    return true;
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
        if (i + 1 < names.size()) {
          size_t j = i + 1;
          usingPath = strutil::rejoinQuotedWord(names, j);  // a quoted path with blanks
        }
        names.erase(names.begin() + static_cast<long>(i), names.end());
        break;
      }
    }
    // W1a-2 / BUG-407: the cell layer lives in the writer `runEsttab`'s JSON
    // estimates_table has no model for, so on screen a `cells()`/`main()`/`aux()`
    // table is the writer's rendered text (esttabScreen returns it below).
    {
      // W2AK: `esttab matrix(A, fmt()) using f` tabulates a matrix
      // (esttab MatrixMode); the namelist is the matrix spec.
      std::string joined;
      for (const auto& w : names) joined += (joined.empty() ? "" : " ") + w;
      if (joined.rfind("matrix(", 0) == 0 && !joined.empty() && joined.back() == ')') {
        ojson mblock;
        mblock["coef_rows"] = ojson::array();
        mblock["names"] = ojson::array();
        mblock["matrix"] = joined.substr(7, joined.size() - 8);
        if (usingPath.empty()) {
          ojson chk = handlers::esttabScreen(mblock, parsed.opts, session);
          if (!chk.is_null()) pushEnvResult(blocks, chk);
        } else {
          pushEnvResult(blocks, handlers::writeEsttabFile(mblock, usingPath, parsed.opts, session));
        }
        return true;
      }
    }
    ojson result = ojson(handlers::runEsttab(names, parsed.opts, session));
    if (usingPath.empty() && result.value("type", std::string()) != "error") {
      // W2AK: the screen form validates and posts r() through the same
      // translation the file form uses (w2ak ek_c01/c05/c09).
      ojson chk = handlers::esttabScreen(result, parsed.opts, session);
      if (!chk.is_null()) {
        pushEnvResult(blocks, chk);
        return true;
      }
    }
    if (!result.value("writer_selection", false)) blocks.push_back(result);
    if (!usingPath.empty() && result.value("type", std::string()) != "error") {
      pushEnvResult(blocks, handlers::writeEsttabFile(result, usingPath, parsed.opts, session));
    }
    return true;
  }
  return false;
}

}
