// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "data_support.h"
#include "expression_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
#include "piet/handlers/data_values.h"
namespace piet::dispatch_detail {
bool route_data_generate(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    QualifierColumnGuard& conditionMask) {


  if (cmd == "generate" || cmd == "gen" || cmd == "g") {
    // BUG-336 (batch 2): known set measured from the handler body -- runGen/runBysortGen take no options.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabGenerate.known, kVocabGenerate.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    std::string raw = genExprText(parsed);
    StorageTypeInfo info = stripStorageType(raw);
    // BUG-112(1) PIET-half: the exists-guard fires BEFORE this verb splits into
    // any of its four sinks (string / standalone _n-_N / `in`-range / plain), so
    // all four reject identically. See genTargetNameError's comment for why
    // the guard is at the verb and not in the handlers.
    if (genTargetName(info.expr).empty() && info.expr.find('=') == std::string::npos) {
      if (auto err = conditionAsNameError(parsed.ifExpr)) {
        blocks.push_back(*err);
        return true;
      }
    }
    if (genTargetName(info.expr) == "if") {
      // `generate if=1`, which the tokenizer kept whole: the same refusal.
      if (auto err = conditionAsNameError(info.expr.substr(info.expr.find('=')))) {
        blocks.push_back(*err);
        return true;
      }
    }
    // `generate = 1` names no variable either (StataBE 19.5: r(102)).
    if (info.typeToken.empty() && info.expr.find('=') != std::string::npos &&
        genTargetName(info.expr).empty()) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "too few variables specified",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              "r(102)")));
      return true;
    }
    if (auto genExists =
            genTargetNameError(session, genTargetName(info.expr), strippedTypeWord(raw, info))) {
      blocks.push_back(*genExists);
      return true;
    }
    // BUG-032: string-valued / string-gated gen must be evaluated row-wise in
    // PIET (cobra is all-f64, cannot do string fns or hold string columns) —
    // mirrors dispatcher.py's `if needs_string_eval(...): return
    // run_string_gen(...)` FIRST, before the numeric cobra path. varname/rhs
    // split matches the oracle's `expr.split("=", 1)`.
    {
      std::string varname, rhs;
      size_t eq = info.expr.find('=');
      if (eq != std::string::npos) {
        varname = trimWs(info.expr.substr(0, eq));
        rhs = trimWs(info.expr.substr(eq + 1));
      } else {
        varname = trimWs(info.expr);
      }
      // BUG-456: the est-ref resolve used to run AFTER this detour decided, so
      // `gen z = (e(cmd)=="regress")` took the string path with a raw e(cmd)
      // in hand and failed at obs 1. Resolve (and fold) first; the detour then
      // sees `("regress"=="regress")` -> `(1)` and routes it numeric.
      if (auto err = preResolveGenExpr(info.expr, rhs, session)) {
        blocks.push_back(*err);
        return true;
      }
      if (strmutate::needsStringEval(session, varname, rhs, parsed.ifExpr, info.stringTyped,
                                     /*isReplace=*/false)) {
        // BS-031(d): fold `in` into the ifExpr this path was already
        // missing it from — see foldStringInRange's own comment above.
        StringInRangeFold fold = foldStringInRange(parsed.ifExpr, parsed.inRange);
        if (!fold.ok) {
          blocks.push_back(stringInRangeError(fold, *parsed.inRange));
          return true;
        }
        std::optional<std::string> combinedIf =
            fold.combinedIf.empty() ? std::nullopt : std::optional<std::string>(fold.combinedIf);
        // A declared `strN` is on varMeta BEFORE the row write, which truncates
        // to it (strmutate writeNew); a failed generate restores what was there.
        const bool strWidthDeclared = info.typeToken.size() > 3 &&
                                      info.typeToken.compare(0, 3, "str") == 0 &&
                                      info.typeToken != "strl" && !varname.empty();
        std::optional<VarMeta> priorMeta;
        bool hadMeta = false;
        if (strWidthDeclared) {
          auto it = session.varMeta.find(varname);
          hadMeta = it != session.varMeta.end();
          if (hadMeta) priorMeta = it->second;
          session.varMeta[varname].stataType = info.typeToken;
        }
        // BUG-1122: a declared numeric type with a string expression is r(109).
        const bool numTyped = !info.typeToken.empty() && !info.stringTyped;
        blocks.push_back(ojson(strmutate::runStringGen(
            session, info.expr, combinedIf, info.stringTyped,
            storage::resolveCreateType(session, info.typeToken), numTyped, parsed.inRange)));
        if (strWidthDeclared && blocks.back().is_object() &&
            blocks.back().value("type", std::string()) == "error") {
          if (hadMeta) session.varMeta[varname] = *priorMeta;
          else session.varMeta.erase(varname);
        }
        // BUG-250: `gen str8 s = ...` records its declared width like the
        // numeric sinks below record theirs.
        recordDeclaredStataType(session, varname, info, blocks.back());
        appendGenMissingNote(blocks, varname);  // BUG-1123
        return true;
      }
    }
    // _b[]/_se[]/e()/scalar() refs resolved to literals BEFORE the expression
    // ships to cobra (which has no estimation state) — the same resolver the
    // scalar/local/if paths use, so gen sees the same value display does. Since
    // BUG-456 this happens in preResolveGenExpr ABOVE the string detour (the
    // `if` was resolved at the parse point); the model gate lives there too.
    // BUG-432: a stored SCALAR does not depend on a model being in memory, and
    // the gate above meant `gen x = y * scalar(s)` was rc 1 in a session with no
    // estimation — while `display s` printed the value. Matrix elements are the
    // same story (BUG-428). The `if` was already substituted at the parse point;
    // this is gen's own expression, which reaches cobra rather than the row
    // evaluator and needs the literals just as much.
    // BUG-456 follow-up (review finding 3): this pass -- the one that
    // substitutes STORED SCALARS, which preResolveGenExpr above does not --
    // discarded its error block, so a mismatch that only appears after scalar
    // substitution (`scalar one = 1` then `gen z = (e(cmd)==one)`) came back as
    // the coarse "generate: expression evaluation failed at obs 1" with no r()
    // code, while the identical `count if e(cmd)==one` reported r(109).
    // name-binding-display-literals: the right-hand side only, as the bysort
    // path above does. The target is a NEW name: neither a stored scalar of
    // that name nor an existing variable it abbreviates (`gen x = 3` beside x2)
    // may rewrite it.
    {
      std::optional<ojson> genExprErr;
      const size_t eq = info.expr.find('=');
      info.expr = eq != std::string::npos
                      ? info.expr.substr(0, eq + 1) + " " +
                            substituteRowExprRefs(trimWs(info.expr.substr(eq + 1)), session,
                                                  &genExprErr)
                      : substituteRowExprRefs(info.expr, session, &genExprErr);
      if (genExprErr.has_value()) {
        blocks.push_back(*genExprErr);
        return true;
      }
    }
    // BS-091/BS-101 (BETA A4): c(name) refs ALSO resolve to a literal BEFORE
    // cobra sees this expression — unlike the est-scalar pass above, c()
    // values never depend on a model being in memory, so this is unGATED.
    info.expr = substituteCRefs(info.expr, session);
    if (parsed.ifExpr.has_value()) {
      parsed.ifExpr = substituteCRefs(*parsed.ifExpr, session);
    }
    // BUG-471 T2: the declared type, else c(type) — stored by the engine, not
    // only claimed (gen_untyped_13 :96 float, gen_byte_27 :222 truncated).
    const std::string genStoreType = storage::resolveCreateType(session, info.typeToken);
    if (hasNorCapN(info.expr) || (parsed.inRange.has_value() && !parsed.inRange->empty())) {
      // bysort.py's run_gen_with_n_N (standalone _n/_N, no bysort prefix) —
      // global 1..N context via cobra eval --bygen with by_vars=[]. BUG-077:
      // `in a/b` also routes here (same empty-by-list global _n/_N context)
      // so the range can fold into the same cond() the if-expr builds.
      blocks.push_back(ojson(handlers::runBysortGen(session, info.expr, {}, parsed.ifExpr, false,
                                                     false, parsed.inRange, std::nullopt,
                                                     genStoreType)));
      recordDeclaredStataType(session, genTargetName(info.expr),
                              withSessionDefaultType(session, info), blocks.back());  // BUG-250
      appendGenMissingNote(blocks, genTargetName(info.expr));  // BUG-1123
      return true;
    }
    blocks.push_back(ojson(handlers::runGen(info.expr, parsed.ifExpr, session, genStoreType)));
    recordDeclaredStataType(session, genTargetName(info.expr),
                            withSessionDefaultType(session, info), blocks.back());  // BUG-250
    appendGenMissingNote(blocks, genTargetName(info.expr));  // BUG-1123
    return true;
  }


  if (cmd == "replace") {
    std::string raw = genExprText(parsed);
    StorageTypeInfo info = stripStorageType(raw);
    // BUG-1142: the target must already exist (an abbreviation resolves; an
    // ambiguous one is r(111)) -- see replaceTargetError. It is read before the
    // options: `replace qq = 1, foo` is r(111) "variable qq not found" on
    // StataBE 19.5, r(198) "option foo not allowed" only for an existing target
    // (measured 2026-10-29).
    if (auto err = replaceTargetError(session, info)) {
      blocks.push_back(*err);
      return true;
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabReplace.known,
                                              kVocabReplace.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    bool noPromote = false;
    if (auto err = replaceNoPromote(parsed.opts, noPromote)) {
      blocks.push_back(*err);
      return true;
    }
    {
      std::string varname, rhs;
      size_t eq = info.expr.find('=');
      if (eq != std::string::npos) {
        varname = trimWs(info.expr.substr(0, eq));
        rhs = trimWs(info.expr.substr(eq + 1));
      } else {
        varname = trimWs(info.expr);
      }
      if (auto err = preResolveGenExpr(info.expr, rhs, session)) {  // BUG-456, as in gen
        blocks.push_back(*err);
        return true;
      }
      if (strmutate::needsStringEval(session, varname, rhs, parsed.ifExpr, info.stringTyped,
                                     /*isReplace=*/true)) {
        // BS-031(e): fold `in` into the ifExpr this path was already
        // missing it from — see foldStringInRange's own comment above (gen
        // branch).
        StringInRangeFold fold = foldStringInRange(parsed.ifExpr, parsed.inRange);
        if (!fold.ok) {
          blocks.push_back(stringInRangeError(fold, *parsed.inRange));
          return true;
        }
        std::optional<std::string> combinedIf =
            fold.combinedIf.empty() ? std::nullopt : std::optional<std::string>(fold.combinedIf);
        std::vector<std::string> notes;
        blocks.push_back(ojson(strmutate::runStringReplace(session, info.expr, combinedIf,
                                                           info.stringTyped, noPromote, &notes,
                                                           parsed.inRange)));
        // BUG-1143: Stata's notes on the values `nopromote` truncated or made
        // missing, command output like compress's lines.
        for (const std::string& text : notes) {
          ojson b;
          b["type"] = "display";
          b["text"] = text;
          blocks.push_back(std::move(b));
        }
        return true;
      }
    }
    // Est-scalar pre-resolution: done in preResolveGenExpr above (BUG-456).
    // Same c(name) pre-resolution as gen (BS-091/BS-101, see gen branch above).
    info.expr = substituteCRefs(info.expr, session);
    if (parsed.ifExpr.has_value()) {
      parsed.ifExpr = substituteCRefs(*parsed.ifExpr, session);
    }
    // BUG-471 T2: replace stores into the variable's own type (the handler
    // widens an integer type by Stata's lattice, or truncates under nopromote).
    const std::string replaceStoreType =
        storage::resolveReplaceType(session, genTargetName(info.expr));
    if (hasNorCapN(info.expr) || (parsed.inRange.has_value() && !parsed.inRange->empty())) {
      // BUG-077: `in a/b` also routes here — see the gen branch above.
      blocks.push_back(ojson(handlers::runBysortGen(session, info.expr, {}, parsed.ifExpr, false,
                                                     true, parsed.inRange, std::nullopt,
                                                     replaceStoreType, noPromote)));
      return true;
    }
    blocks.push_back(ojson(
        handlers::runReplace(info.expr, parsed.ifExpr, session, replaceStoreType, noPromote)));
    return true;
  }


  if (cmd == "egen") {
    // BUG-1433's class: a literal keeps its run of blanks, as in generate.
    std::string raw = genExprText(parsed);
    StorageTypeInfo info = stripStorageType(raw);
    // BUG-1120 review: `egen byte = mean(x)` names no variable after the type this
    // strip took, and egen refuses the word as a name, "byte invalid varname"
    // r(198) (StataBE 19.5); runEgen refuses a reserved NEWVAR the same way. In
    // `egen if = mean(x)` the tokenizer took `if` for the qualifier: "if invalid
    // varname" too.
    if (trimWs(raw).empty() && parsed.ifExpr.has_value() &&
        trimWs(*parsed.ifExpr).rfind('=', 0) == 0) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "if invalid varname", std::nullopt,
                                              std::nullopt, std::nullopt, "r(198)")));
      return true;
    }
    if (genTargetName(info.expr).empty() && info.expr.find('=') != std::string::npos) {
      if (auto r = strutil::newVarNameRefusal(strippedTypeWord(raw, info))) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", r->message, std::nullopt,
                                                std::nullopt, std::nullopt,
                                                "r(" + std::to_string(r->rc) + ")")));
        return true;
      }
    }
    std::string byVal;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "by" && !v.isBool) {
        byVal = v.strValue;
        break;
      }
    }
    // BS-032 (BETA campaign C2): real Stata rejects `egen ... = group(...)` /
    // `tag(...)` combined with a by-context, `bysort state: egen g1=group(year)`
    // (which injects `by(state)` into the recursed-into sub-command below) as
    // much as an explicit `egen ..., by(state)`. runEgen answers it with the row
    // functions, as _egennoby does: r(190) (BUG-1106; it was r(198) here).
    // BUG-1420: c(NAME) in an egen argument is its value, as in generate
    // (`egen t = total(c(N))`), and a name that is no c() key is r(133).
    if (info.expr.find("c(") != std::string::npos) {
      info.expr = substituteCRefs(info.expr, session);
      if (auto undef = eval::undefinedCRef(info.expr)) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", *undef, std::nullopt,
                                                std::nullopt, std::nullopt, "r(133)")));
        return true;
      }
    }
    // BUG-471 T2: the declared type, else c(type) (egen_mean_untyped :953);
    // runEgen withholds it from tag()/seq()/mode(), whose ados ignore it.
    const std::string egenStoreType = storage::resolveCreateType(session, info.typeToken);
    // BUG-464: an egen function of an EXPRESSION (_gmax.ado and its siblings
    // generate the expression into a temporary first) hands it to the engine's
    // f64 evaluator, which has no string function and no bofd()/dofb():
    // `egen mx = max(bofd("cal", bd))` and `egen m = max(strlen(s))` were rc 1
    // "function ... is not supported". Such an argument -- the condition route's
    // own test, strmutate::needsConditionMask -- is evaluated by the row
    // evaluator into a hidden column (strmutate::installRowExpression), which
    // the function then reads; the column leaves with this dispatch, as a
    // condition mask does. The target and info.expr stay as typed. An argument
    // the row evaluator cannot read either (`total(x[_n-1] < x & s == "")`)
    // keeps the engine's answer, as before.
    // An argument naming _n or _N takes the same route under a by-list or
    // if/in: the ados generate it over the whole frame and only then restrict
    // to if/in, where the engine read it on the if/in-filtered copy (StataBE
    // `total(_n) if x > 2` 12, PIET 6). Under by(), total/sum, mean, max, min,
    // sd, std, pc, count, mdev, skew and kurt generate it by the groups, so
    // _n and _N count within each (`egen t = total(x * _n), by(g)` 5 and 26
    // where the engine's frame-wide read gave 5 and 50); median, pctile, iqr,
    // mad and rank generate it once over the whole frame and sort within the
    // groups afterwards, so theirs stay the frame's (`median(x * _n), by(g)` 4
    // and 25 on g = 1 1 1 2 2 2, x = 1..6; `rank(mod(_n,3)), by(g)` ranks the
    // frame's positions). Such an argument the row evaluator cannot read stays
    // the engine's.
    // count() of a string expression counts the values that are not "", which
    // the engine's f64 evaluator cannot read either: `egen c = count(s + "x")`
    // is 5 in StataBE.
    std::string egenExpr = info.expr;
    {
      static const std::regex reExpFn(
          R"(^([^=]*=\s*)(count|iqr|kurt|mad|max|mdev|mean|median|min|pc|pctile|rank|sd|skew|std|sum|total)\s*\((.*)\)\s*$)");
      std::smatch fm;
      if (std::regex_match(info.expr, fm, reExpFn) && session.hasData()) {
        const std::string fn = fm.str(2);
        const std::string arg = fm.str(3);
        const bool isCount = fn == "count";
        const bool engineCannot = strmutate::needsConditionMask(session, arg) ||
                                  (isCount && strmutate::needsStringCount(session, arg));
        const bool groupPositions = !(fn == "median" || fn == "pctile" || fn == "iqr" ||
                                      fn == "mad" || fn == "rank");
        const bool restricted = (groupPositions && !byVal.empty()) ||
                                (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
                                (parsed.inRange.has_value() && !parsed.inRange->empty());
        if (engineCannot || (restricted && strmutate::namesObsPosition(arg))) {
          strmutate::ConditionMaskInstall xi = strmutate::installRowExpression(
              session, arg, groupPositions ? byVal : std::string(), isCount);
          if (xi.ok) {
            conditionMask.columns.push_back(xi.column);
            egenExpr = fm.str(1) + fm.str(2) + "(" + xi.column + ")";
          } else if (engineCannot && !xi.declined) {
            blocks.push_back(xi.error);
            return true;
          }
        }
      }
    }
    std::string egenFixedType;  // F3: seq()'s widened type; mode()'s str#; an ado's own
    blocks.push_back(ojson(handlers::runEgen(egenExpr, byVal, parsed.ifExpr, parsed.opts, session,
                                             parsed.inRange, egenStoreType, &egenFixedType,
                                             cmdTyped == "gegen")));
    // BUG-250: `egen double m = mean(x)` — same declaration carriage as gen
    // (oracle rows J|egen-double -> double, J|egen-default -> float).
    // BUG-471 T2: tag/seq are byte and mode takes its source's type whatever was
    // declared (egen_tag_type, egen_seq_type, egen mode()).
    {
      const std::string egenTarget = genTargetName(info.expr);
      ojson& egenBlk = blocks.back();
      const bool egenFailed =
          egenBlk.is_object() && egenBlk.value("type", std::string()) == "error";
      const size_t egenEq = info.expr.find('=');
      const std::string egenRhs =
          egenEq == std::string::npos ? std::string() : info.expr.substr(egenEq + 1);
      if (egenFailed ||
          !storage::recordEgenFixedType(session, egenTarget, egenRhs, egenFixedType))
        recordDeclaredStataType(session, egenTarget, withSessionDefaultType(session, info),
                                egenBlk);
      else
        refreshCardFormat(session, egenBlk, egenTarget);  // tag() is byte: %8.0g
    }
    return true;
  }


  if (cmd == "encode") {
    std::string strvar = parsed.varlist.empty() ? std::string() : parsed.varlist[0];
    if (strvar.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "encode: syntax error — usage: encode STRVAR, gen(NEWVAR) [label(LBLNAME)]")));
      return true;
    }
    // BS-097: `generate`/`label` are the encode options this arm reads, and
    // `noextend` since BUG-1096 (runEncode).
    //
    // BUG-177 (P2-PREP): the canonical name registered here used to be the
    // ABBREVIATION `gen`, so Stata's own full spelling `encode grp,
    // generate(newv)` was rejected outright ("option `generate' not
    // allowed"). eval::resolveOptionAbbrev is one-sided by construction — it
    // resolves abbrev -> canonical only — so registering an abbreviation as
    // the canonical name makes the FULL spelling permanently unreachable,
    // the exact inverse of what the helper is for. Registering `generate`
    // and canonicalizing first fixes both directions at once: `gen`, `gene`,
    // ... `generate` all resolve, and the read below sees one spelling.
    static const std::unordered_set<std::string> kEncodeKnownOpts = {"generate", "label",
                                                                     "noextend"};
    auto encodeOpts = parsed.opts;
    eval::canonicalizeOptionKeys(encodeOpts, kEncodeKnownOpts);
    std::optional<std::string> genVal, lblVal;
    bool noExtend = false;
    for (const auto& [k, v] : encodeOpts) {
      if (k == "generate" && !v.isBool) genVal = v.strValue;
      if (k == "label" && !v.isBool) lblVal = v.strValue;
      if (k == "noextend" && v.isBool && v.boolValue) noExtend = true;
    }
    if (auto err = eval::rejectUnknownOptions(encodeOpts, kVocabEncode.known, kVocabEncode.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    if (!genVal.has_value() || genVal->empty()) {
      // Stata's rc 198 (w2ah_data_verbs.log ah_encode_nogen); main answered 1.
      blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                              "option generate() required", std::nullopt,
                                              std::nullopt, std::nullopt, std::string("r(198)"))));
      return true;
    }
    blocks.push_back(
        ojson(handlers::runEncode(strvar, *genVal, lblVal, session, parsed.ifExpr, noExtend)));
    return true;
  }


  // BS-003/D4 (BETA B-data): `destring`/`tostring` — PIET-NATIVE (no cobra
  // verb, no hbin-v2 dependency; datamgmt/destring.cpp's runDestring and
  // datamgmt/tostring.cpp's runTostring ride the existing factor-column machinery).
  //
  // BUG-664/665 (closes BUG-440): both now take a VARLIST, as Stata's destring
  // and tostring do -- the handler converts one variable and this arm loops
  // over the varlist. A variable the handler declines (nonnumeric content,
  // irreversible rendering, already the target type) yields its note block and
  // the loop moves on at rc 0; a real error ends the command. generate() takes
  // one new name per source variable, checked up front (a count mismatch is
  // r(198) before any variable is converted, as in Stata).
  if (cmd == "destring" || cmd == "tostring") {
    const bool isDestring = cmd == "destring";
    const std::unordered_set<std::string>& knownOpts =
        isDestring ? kVocabDestring.known : kVocabTostring.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, knownOpts)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    bool doReplace = false, force = false, useDisplayFormat = false;
    handlers::DestringOpts dOpts;
    std::optional<std::string> genVal, ignoreChars, fmtOpt;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "replace" && v.isBool && v.boolValue) doReplace = true;
      if ((k == "gen" || k == "generate") && !v.isBool) genVal = v.strValue;
      if (k == "ignore" && !v.isBool) ignoreChars = v.strValue;
      if (k == "force" && v.isBool && v.boolValue) force = true;
      if (k == "float" && v.isBool && v.boolValue) dOpts.asFloat = true;
      if (k == "dpcomma" && v.isBool && v.boolValue) dOpts.dpcomma = true;
      if (k == "percent" && v.isBool && v.boolValue) dOpts.percent = true;
      if (k == "usedisplayformat" && v.isBool && v.boolValue) useDisplayFormat = true;
      if (k == "format" && !v.isBool) fmtOpt = strutil::trim(v.strValue);
    }
    // Stata's user-error messages and return codes for the option checks.
    if (genVal.has_value() && doReplace) {
      blocks.push_back(ojson(eval::engineError(
          cmd + ": options generate and replace are mutually exclusive", "syntax_error")));
      return true;
    }
    if (!genVal.has_value() && !doReplace) {
      blocks.push_back(ojson(eval::engineError(
          cmd + ": must specify either generate or replace option", "syntax_error")));
      return true;
    }
    if (!isDestring && fmtOpt.has_value()) {
      if (useDisplayFormat) {
        blocks.push_back(ojson(eval::engineError(
            "tostring: must choose between usedisplayformat and format() options",
            "syntax_error")));
        return true;
      }
      if (fmtOpt->find('s') != std::string::npos) {
        blocks.push_back(
            ojson(eval::engineError("tostring: use numeric format in format() option",
                                    "syntax_error")));
        return true;
      }
      // BUG-919: the ado's test is `capture di FMT 12345.67890`, so a format
      // `display` cannot use is r(198) before any variable is touched
      // (w2z_tostring tz_fmt_invalid: `format(%5.2z)` is 198, vb stays byte).
      // Only the leading `%` was checked. The families display renders a
      // number with: %[-][0]W(.|,)D(e|f|g)[c], %Wx, %WH/%WL, and the date
      // families %t... / %d... (their details are left to the renderer).
      static const std::regex reTostringFmt(
          R"(^%-?0?\d*[.,]\d+[efg]c?$|^%-?\d*x$|^%\d*[HL]$|^%-?\d*t[A-Za-z].*$|^%-?d.*$)");
      if (fmtOpt->empty() || !std::regex_match(*fmtOpt, reTostringFmt)) {
        blocks.push_back(
            ojson(eval::engineError("tostring: format() option invalid", "syntax_error")));
        return true;
      }
    }
    // parseOpts() captures a paren-value VERBATIM (it only tracks paren
    // depth, not Stata's own string-quoting) — real Stata's ignore() syntax
    // is always double-quoted (`ignore(",")`), so the raw strValue here is
    // literally `","` including the quote characters; strip them (same
    // convention the merge/joinby `using "FILE"` arms already use above) so
    // the character SET destring strips from each value is just `,`, not
    // `,` and `"`.
    if (ignoreChars.has_value() && ignoreChars->size() >= 2 && ignoreChars->front() == '"' &&
        ignoreChars->back() == '"') {
      ignoreChars = ignoreChars->substr(1, ignoreChars->size() - 2);
    }
    // destring's varlist is optional (`syntax [varlist]` = every variable);
    // tostring's is required.
    std::vector<std::string> vars;
    if (parsed.varlist.empty()) {
      if (!isDestring) {
        blocks.push_back(ojson(eval::engineError("tostring: varlist required", "syntax_error")));
        return true;
      }
      auto vr = piet::loops::expandVarlist("_all", session);
      if (vr.isError) {
        blocks.push_back(vr.error);
        return true;
      }
      vars = vr.tokens;
    } else if (parsed.varlist.size() == 1 &&
               parsed.varlist[0].find_first_of("*?-~") == std::string::npos) {
      // A single plain name goes straight to the handler exactly as before
      // (its own not-found message, and no name-list read that could
      // materialize a dirty resident head -- the C4-L4 chain gates).
      vars = parsed.varlist;
    } else {
      auto vr = piet::loops::expandVarlist(joinVarlist(parsed.varlist), session);
      if (vr.isError) {
        blocks.push_back(vr.error);
        return true;
      }
      vars = vr.tokens;
    }
    std::vector<std::string> newNames;
    if (genVal.has_value()) {
      std::istringstream gs(*genVal);
      std::string tok;
      while (gs >> tok) newNames.push_back(tok);
      if (newNames.size() != vars.size()) {
        blocks.push_back(ojson(eval::engineError(
            cmd + ": number of variables in varlist must equal number of variables in "
                  "generate(newvarlist)",
            "syntax_error")));
        return true;
      }
      // `syntax newvarlist` validates every name before any variable is
      // converted; with one name the handler's own checks are that test.
      if (newNames.size() > 1) {
        std::unordered_set<std::string> seen;
        Datastore* gds = session.datastore();
        for (const auto& nm : newNames) {
          // BUG-1120: the handlers' own reserved-name refusal (runDestring /
          // runTostring), for every name before any variable is converted.
          if (strutil::isReservedVarName(nm) || nm == "_skip" || nm == "_all") {
            blocks.push_back(ojson(eval::errorBlock(
                "syntax_error",
                isDestring ? "generate() contains existing variable(s) and/or illegal variable "
                             "name(s)"
                           : "generate(newvarlist) invalid",
                std::nullopt, std::nullopt, std::nullopt,
                strutil::isReservedVarName(nm) ? "r(198)" : "r(101)")));
            return true;
          }
          if (!seen.insert(nm).second || (gds != nullptr && gds->has(nm))) {
            blocks.push_back(ojson(
                eval::engineError(cmd + ": variable '" + nm + "' already defined")));
            return true;
          }
          if (!strutil::isValidStataIdentifier(nm)) {
            blocks.push_back(ojson(eval::engineError(
                cmd + ": invalid variable name `" + nm + "' — " +
                    strutil::stataIdentifierRuleText(),
                "syntax_error")));
            return true;
          }
        }
      }
    }
    handlers::TostringOpts tOpts;
    tOpts.stataGate = true;
    tOpts.format = fmtOpt;
    tOpts.force = force;
    for (size_t i = 0; i < vars.size(); ++i) {
      std::optional<std::string> target;
      if (genVal.has_value()) target = newNames[i];
      // BUG-1160 (FU-24, FU-32): tostring prints each conversion, "x was
      // double now str3" under replace and "y generated as str3" under
      // generate() (StataBE 19.5, measured 2026-11-03; w2b_dataio.log:77 `big
      // was double now str11`). The old type is the one `: type` answers
      // before the conversion: the recorded claim, else float.
      std::string fromType = "float";
      if (!isDestring) {
        auto mit = session.varMeta.find(vars[i]);
        if (mit != session.varMeta.end() && mit->second.stataType.has_value())
          fromType = *mit->second.stataType;
      }
      // Under `force` a conversion some value does not survive adds "VAR was
      // forced to string; some loss of information", VAR the variable it
      // wrote (StataBE: `gen y = _n + 0.1` / `tostring y, replace force` gives
      // "y was float now str11" and then that line; gen(ys) names ys).
      bool forcedLoss = false;
      ojson blk = isDestring ? ojson(handlers::runDestring(vars[i], doReplace, target,
                                                           ignoreChars, force, session, dOpts))
                             : ojson(handlers::runTostring(vars[i], doReplace, target,
                                                           useDisplayFormat, session, tOpts,
                                                           &forcedLoss));
      const std::string blkType = blk.is_object() ? blk.value("type", std::string()) : "";
      blocks.push_back(std::move(blk));
      if (blkType == "error") return true;
      if (!isDestring && blkType == "data_mutation") {
        const std::string out = target.value_or(vars[i]);
        auto mit = session.varMeta.find(out);
        auto note = [&blocks](const std::string& text) {
          ojson b;
          b["type"] = "display";
          b["text"] = text;
          blocks.push_back(std::move(b));
        };
        if (mit != session.varMeta.end() && mit->second.stataType.has_value())
          note(doReplace ? vars[i] + " was " + fromType + " now " + *mit->second.stataType
                         : out + " generated as " + *mit->second.stataType);
        if (forcedLoss) note(out + " was forced to string; some loss of information");
      }
    }
    return true;
  }


  // BUG-371 — decode / inspect / ds / unab.
  if (cmd == "decode") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabDecode.known,
                                              kVocabDecode.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    if (parsed.varlist.size() != 1) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `decode` requires `decode varname, generate(newvar)`", "syntax_error")));
      return true;
    }
    std::optional<std::string> genVar;
    // BUG-1096: maxlength(#) keeps the whole characters of each label that fit in
    // # bytes, 1 to 32000 (the default), and caps the new str# (runDecode). StataBE
    // 19.5: `maxlength(3)` makes str3 "alp"; `maxlength(0)`/`(32001)` are
    // "maxlength must be between 1 and 32000", a non-integer, an empty one or the
    // bare word "option maxlength() incorrectly specified" (bare: "option
    // maxlength incorrectly specified"), and a second one "option maxlength() not
    // allowed", each r(198).
    size_t maxLength = 32000;
    bool sawMaxLength = false;
    auto r198 = [&](const std::string& msg) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", msg, std::nullopt, std::nullopt,
                                              std::nullopt, "r(198)")));
      return blocks;
    };
    for (const auto& [k, v] : parsed.opts) {
      if (k == "generate" && !v.isBool) genVar = v.strValue;
      if (k != "maxlength") continue;
      if (sawMaxLength) { blocks = r198("option maxlength() not allowed"); return true; }
      sawMaxLength = true;
      if (v.isBool) { blocks = r198("option maxlength incorrectly specified"); return true; }
      const std::optional<double> n = eval::pythonFloat(trimWs(v.strValue));
      if (!n.has_value() || std::isnan(*n) || *n != std::floor(*n))
        { blocks = r198("option maxlength() incorrectly specified"); return true; }
      if (*n < 1 || *n > 32000) { blocks = r198("maxlength must be between 1 and 32000"); return true; }
      maxLength = static_cast<size_t>(*n);
    }
    blocks.push_back(ojson(handlers::runDecode(parsed.varlist[0], genVar, session, maxLength)));
    return true;
  }
  return false;
}

}
