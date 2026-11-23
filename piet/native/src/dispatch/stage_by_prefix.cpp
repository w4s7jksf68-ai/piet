// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include "data_support.h"
#include "expression_support.h"
#include "prefix_support.h"
#include "summary_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
#include "piet/eval/exprfns.h"
#include "piet/handlers/data_values.h"
#include "piet/handlers/descriptive.h"  // ListLayout / buildListing: by-group list
#include "piet/loops.h"                 // expandVarlist: by-group list
#include "piet/handlers/descriptive.h"
#include "piet/handlers/gtools_aliases.h"
#include "piet/handlers/observations.h"
#include "piet/handlers/variables.h"
#include "piet/program.h"
namespace piet::dispatch_detail {
bool executeByPrefix(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    QualifierColumnGuard& conditionMask) {


  // ------------------------------------------------------------------
  // 1b. M3b: bysort/by prefix interception — BEFORE classify()/the program
  // guard, mirroring dispatcher.py's step 1b (bysort.py's run_bysort_cmd).
  // `bysort VARLIST: CMD` / `by VARLIST: CMD`:
  //   - gen/replace (any expr, not just _n/_N) -> cobra eval --bygen with
  //     by()/sort() clauses (run_bysort_gen).
  //   - egen -> inject `, by(VARLIST)` into the sub-command and recurse
  //     through dispatch() (respecting an explicit by() already present).
  //   - anything else -> `bysort` sorts by VARLIST first (in place), `by`
  //     does not, then the sub-command is dispatched normally (recursion).
  // BS-017: bys/byso/bysor are legal Stata abbreviations of `bysort` (`by`
  // has no shorter abbreviation of its own) — normalise to "bysort" before
  // calling parseBysortPrefix so its existing cmdLower=="bysort" checks
  // (sortFirst included) need no changes of their own.
  if (cmd == "bysort" || cmd == "by" || cmd == "bys" || cmd == "byso" || cmd == "bysor") {
    // A subscript in an expression counts within the group here; the row evaluator reads
    // the whole frame and does not read subscripts under a by-prefix.
    const strmutate::GroupRelativeSubscripts groupRelative;
    // BUG-535: `by KEYS, sort: CMD` is Stata's long spelling of `bysort KEYS:
    // CMD` (w2_bykeep.log arm by_sortopt, rc 0). The prefix parser below has no
    // notion of prefix options — the comma sent `sort:` into the option list and
    // the line died "no subcommand found after colon" — so the one option is
    // rewritten here as `bysort KEYS: CMD` and the rewritten line is
    // re-TOKENIZED in place, not re-dispatched. parsed.raw is already
    // macro-expanded; dispatch() would expand it a second time (an expanded
    // value that itself holds `$name` or a backtick would be expanded again).
    // No dispatcher entry point sits below macro expansion (the egen and xi:
    // re-dispatches go through dispatch()/dispatchImpl() too), and this branch
    // reads nothing of the line but its raw text and varlist, so a local
    // re-tokenize is the whole re-entry. Any other prefix option keeps the old
    // path.
    std::string bysortCmdNorm = (cmd == "by") ? cmd : std::string("bysort");
    const eval::ParsedCommand* byParsed = &parsed;
    eval::ParsedCommand sortOptParsed;
    {
      const std::string& raw = parsed.raw;
      const size_t colon = raw.find(':');
      const size_t sp = raw.find_first_of(" \t");
      if (colon != std::string::npos && sp != std::string::npos && sp < colon) {
        auto [keysPart, prefixOpts] = splitTopLevelComma(raw.substr(sp, colon - sp));
        // by's options are sort and rc0; any other word is r(198) "option W
        // not allowed" (vs-stata bank 36a25c35 `bysort foreign, stable:`).
        if (prefixOpts.has_value()) {
          std::istringstream ow(lower(trimWs(*prefixOpts)));
          for (std::string w; ow >> w;) {
            const std::string head = w.substr(0, w.find('('));
            if (head != "sort" && head != "rc0") {
              blocks.push_back(ojson(eval::errorBlock("syntax_error", "option " + head + " not allowed",
                                                      std::nullopt, std::nullopt, std::nullopt,
                                                      "r(198)")));
              return true;
            }
          }
        }
        if (prefixOpts.has_value() && lower(trimWs(*prefixOpts)) == "sort" &&
            !trimWs(keysPart).empty()) {
          try {
            sortOptParsed = eval::tokenize("bysort " + trimWs(keysPart) + ": " +
                                           trimWs(raw.substr(colon + 1)));
          } catch (const eval::TokenizerError& te) {
            blocks.push_back(ojson(te.block));
            return true;
          }
          byParsed = &sortOptParsed;
          bysortCmdNorm = "bysort";
        }
      }
    }
    BysortPrefix bp = parseBysortPrefix(bysortCmdNorm, byParsed->varlist, byParsed->raw);
    if (bp.subCmd.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "bysort/by: no subcommand found after colon in `" + byParsed->raw + "`")));
      return true;
    }
    // A by-variable with a time-series operator is r(101) "time-series
    // operators not allowed" (vs-stata bank 5f227ad3 `bysort L.x:`).
    for (const std::vector<std::string>* names : {&bp.byVars, &bp.sortVars})
      for (const auto& k : *names)
        if (k.find('.') != std::string::npos) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", "time-series operators not allowed",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  "r(101)")));
          return true;
        }
    // BUG-532: abbreviated / wildcard keys resolve before any branch reads them.
    for (std::vector<std::string>* names : {&bp.byVars, &bp.sortVars}) {
      if (std::optional<ojson> err = handlers::resolveByKeyNames(session, *names)) {
        blocks.push_back(*err);
        return true;
      }
    }
    // No observations: `by` has no group to run the command in, so it runs it
    // zero times and succeeds (vs-stata bank: `clear`, `set obs 0`, `gen x=.`,
    // `bysort x: gen n=_n` is rc 0 with k 1 -- n is never created).
    if (session.hasData()) {
      Datastore* bds = session.datastore();
      if (bds != nullptr && bds->nRows() == 0) {
        bool keysExist = true;
        for (const std::vector<std::string>* names : {&bp.byVars, &bp.sortVars})
          for (const auto& k : *names) keysExist = keysExist && bds->has(k);
        if (keysExist) return true;
      }
    }
    // BUG-105 (WS-K) — the by/bysort sibling of the resolution point above, so
    // `bysort region: summ price` reaches the same per-group implementation
    // `bysort region: summarize price` does. The typed spelling is kept for the
    // reject messages further down, which quote what the user wrote.
    const std::string subCmdTyped = lower(rstripColon(bp.subCmd));
    std::string subCmdLower = registry::resolveCanonical(subCmdTyped);
    // Same fold on the by-prefix path: `bysort g: gegen x = mean(y)` has to
    // reach the egen by()-injection branch below, which compares this name. The
    // recursive dispatch of the inner command then passes the main translation
    // point above, so options are still translated exactly once.
    subCmdLower = handlers::gtoolsCanonicalName(subCmdLower);

    // A command that is not byable is refused by the prefix itself, before any
    // sort, as r(190) "NAME may not be combined with by". The names below are
    // the ones the vs-stata bank recorded so (StataBE 19.5): the prefixes
    // capture/quietly/noisily/version (0152d124, faca8032, bf5c3740, 20eb1ff5),
    // the language commands display/local/scalar/matrix/matlist/error/timer/
    // _dots/cls (fc7086e1, 100b0a5b, 8ec6ecfc, 8d922009, ee607b20, 8b733e35,
    // edcc622c, 41b9b968, bc723e9b), and mean/collapse/sort/preserve/restore/
    // notes/numlabel/labvars/tolower/mkmat/matselrc/matsort/xi/require
    // (8c5e1ce2, 397f8cf2, 2fbb2a49, 4ac6ce2a, 3b055348, d1c73b2b, 64ee73a0,
    // 6b40fe96, b95bc804, f6e1c32d, 7cae5922, 11c889d1, 49e12db6, 0d12f1e3).
    // Rows flagged byIllegalInStata (describe, codebook, ...) answer the same.
    // A user program is byable only when its `program define` line says
    // byable(); otherwise it is refused the same way (b6e3e8ab).
    {
      static const std::set<std::string> kNotByable = {
          "capture", "quietly", "noisily", "version", "display", "local", "global",
          "scalar", "matrix", "matlist", "error", "timer", "_dots", "cls", "mean",
          "collapse", "sort", "preserve", "restore", "notes", "numlabel", "labvars",
          "tolower", "mkmat", "matselrc", "matsort", "xi", "require"};
      static const std::map<std::string, std::string> kPrefixAbbrev = {
          {"cap", "capture"}, {"capt", "capture"}, {"captu", "capture"}, {"captur", "capture"},
          {"qui", "quietly"}, {"quie", "quietly"}, {"quiet", "quietly"}, {"quietl", "quietly"},
          {"n", "noisily"}, {"no", "noisily"}, {"noi", "noisily"}, {"nois", "noisily"},
          {"noisi", "noisily"}, {"noisil", "noisily"}, {"vers", "version"},
          {"versi", "version"}, {"versio", "version"}, {"loc", "local"}, {"loca", "local"},
          {"gl", "global"}, {"glo", "global"}, {"glob", "global"}, {"globa", "global"},
          {"note", "notes"}};
      std::string stem = subCmdLower;
      if (auto it = kPrefixAbbrev.find(subCmdTyped); it != kPrefixAbbrev.end()) stem = it->second;
      const registry::CommandSpec* stemSpec = registry::findByName(stem);
      bool refuse = kNotByable.count(stem) > 0 || (stemSpec && stemSpec->byIllegalInStata);
      std::string shownName = stemSpec ? stemSpec->canonical : stem;
      if (!refuse && !registry::isStataCommandAbbreviation(bp.subCmd) && stemSpec == nullptr) {
        if (std::optional<std::string> prog = program::findProgram(session, rstripColon(bp.subCmd))) {
          const ProgramDef& pd = session.programs.at(*prog);
          if (pd.byable.empty()) {
            refuse = true;
            shownName = pd.name;
          }
        }
      }
      if (refuse && !bp.byVars.empty()) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                                shownName + " may not be combined with by",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(190)")));
        return true;
      }
    }

    if (subCmdLower == "gen" || subCmdLower == "generate" || subCmdLower == "g" ||
        subCmdLower == "replace") {
      bool isReplace = (subCmdLower == "replace");
      // A plain by: generate/replace has the same sortedness requirement as
      // by: keep. In w2_bykeep the final rcby field probes this later command,
      // independently of whether the preceding keep/drop succeeded.
      if (!bp.sortFirst) {
        std::vector<std::string> keys = bp.byVars;
        keys.insert(keys.end(), bp.sortVars.begin(), bp.sortVars.end());
        Datastore* ds = session.datastore();
        if (ds == nullptr) { blocks.push_back(ojson(eval::noDataLoaded())); return true; }
        const std::vector<std::string> claim = engineSortClaim(*ds);
        const bool inOrder = claim.empty() ? keysAlreadyGrouped(*ds, keys, true)
                                          : keysLeadClaim(keys, claim);
        if (!inOrder) {
          blocks.push_back(ojson(eval::errorBlock(
              "not_sorted", "not sorted — `by " + joinSpace(bp.byVars) +
                  ":` requires the data to already be sorted by " + joinSpace(keys) + ".",
              std::nullopt, std::nullopt, std::nullopt, "r(5)")));
          return true;
        }
      }
      std::string subJoined = bp.subRaw;
      // strip the sub-command token itself (first word) off subRaw.
      {
        size_t sp = subJoined.find_first_of(" \t");
        subJoined = (sp == std::string::npos) ? std::string() : trimWs(subJoined.substr(sp));
      }
      // BUG-1144 (FU-216, FU-227): the sub-command's own options, behind the
      // first comma outside quotes, parentheses and subscripts. They take the
      // plain arms' nets (kVocabGenerate, kVocabReplace); the tail used to stay
      // inside the expression, so `bysort g: replace b = 2.7, nopromote` stored
      // 2.7 in a byte column promoted to float, where StataBE keeps byte b and
      // stores 2 (measured 2026-10-29).
      bool noPromote = false;
      std::string placeWord, placeVar;  // generate ..., before(v) | after(v)
      // A replace reads its target before its options (replaceTargetError), so
      // the options' refusal waits for it.
      std::optional<ojson> optionError;
      {
        const long long comma = eval::findUnquotedComma(subJoined);
        if (comma >= 0) {
          eval::ParsedCommand optParsed;
          try {
            optParsed = eval::tokenize(bp.subRaw);
          } catch (const eval::TokenizerError& te) {
            blocks.push_back(ojson(te.block));
            return true;
          }
          // The plain arms' keys are resolved at dispatchImpl's resolution
          // point before their nets run; these never pass it, so an accepted
          // abbreviation (`nopro`) reached the `nopromote` compare below raw and
          // was dropped: b promoted to float, where StataBE keeps byte b.
          const char* optCmd = isReplace ? "replace" : "generate";
          for (auto& kv : optParsed.opts) {
            // The same measured spellings as the plain arms (`b(x)` means before(x)): the form
            // the option was typed in selects the row.
            const OptKeyResolution optRes =
                resolveOptionKeyForCommand(optCmd, kv.first, kv.second.isBool ? 1 : 2);
            if (std::strcmp(optRes.stage, "bank-refuse") == 0) {
              blocks.push_back(ojson(eval::engineError(
                  "option `" + kv.first + "` not allowed", "unsupported_option")));
              return true;
            }
            kv.first = optRes.resolved;
            if (optRes.exactOnly) kv.second.exactOnly = true;
          }
          const eval::OptVocab& vocab = isReplace ? kVocabReplace : kVocabGenerate;
          if (auto err =
                  eval::rejectUnknownOptions(optParsed.opts, vocab.known, vocab.unimplemented)) {
            optionError = ojson(*err);
          } else if (isReplace) {
            optionError = replaceNoPromote(optParsed.opts, noPromote);
          }
          if (optionError.has_value() && !isReplace) {
            blocks.push_back(*optionError);
            return true;
          }
          if (!isReplace) {
            if (auto perr = generatePlacementRequest(session, optParsed.opts, placeWord, placeVar)) {
              blocks.push_back(*perr);
              return true;
            }
          }
          subJoined = trimWs(subJoined.substr(0, static_cast<size_t>(comma)));
        }
      }
      // extract trailing `if COND` (first occurrence outside any quoted
      // string — BUG-115 class sweep: `bysort v: gen x = "if you see this"`
      // must not have its quoted literal mis-split as an `if` qualifier).
      // Only the `if` keyword itself must stand outside the quotes: the
      // condition after it may hold a string literal (`by g: replace x = 30 if
      // s != "a"` was r(198) `trailing input` when the whole tail had to be
      // unquoted; StataBE 19.5 makes 6 real changes, measured 2026-10-29).
      std::optional<std::string> ifExpr;
      static const std::regex reIf(R"(\s+if\s+)", std::regex::icase);
      std::smatch ifm;
      if (eval::searchUnquoted(subJoined, reIf, ifm)) {
        const size_t at = static_cast<size_t>(ifm.position(0));
        ifExpr = trimWs(subJoined.substr(at + static_cast<size_t>(ifm.length(0))));
        subJoined = trimWs(subJoined.substr(0, at));
      }
      // storage-type prefix strip (gen byte idx = _n).
      StorageTypeInfo info = stripStorageType(subJoined);
      // BUG-1142: a by-prefixed replace reads its target as the plain arm does,
      // and before its options (`by g: replace qq = 1, foo` is r(111) on StataBE
      // 19.5, measured 2026-10-29).
      if (isReplace) {
        if (auto err = replaceTargetError(session, info)) {
          blocks.push_back(*err);
          return true;
        }
        if (optionError.has_value()) {
          blocks.push_back(*optionError);
          return true;
        }
      }
      // e(sample) T2/T4: this route hands its own `if` and expression to the
      // engine as raw text (runBysortGen), past the whole-line rewrite, so they
      // take the SAME row rewrite the plain gen/replace arms take
      // (substituteRowExprRefs: e(sample) first, then _b[]/_se[]/e()/scalar()/
      // stored scalars) -- else `e()` was rc 1 (w2_esample.log:560 B27 `by id:
      // generate ... = sum(1) if e(sample)` |0|28|6|), and before this round
      // `by g: gen z = 1 if x > scalar(s)` was rc 1 `function scalar() is not
      // supported` and `... if x > _b[x]` r(111) `unknown variable _b` while the
      // same line without `by` ran (review 2026-10-15). Only the right-hand
      // side is rewritten, never the target name: a stored scalar may share
      // the new variable's name. generate/replace do not touch e(), so one
      // reading serves every group.
      {
        std::optional<ojson> rowErr;
        if (ifExpr.has_value()) ifExpr = substituteRowExprRefs(*ifExpr, session, &rowErr);
        const size_t eq = info.expr.find('=');
        if (!rowErr.has_value() && eq != std::string::npos) {
          info.expr = info.expr.substr(0, eq + 1) + " " +
                      substituteRowExprRefs(trimWs(info.expr.substr(eq + 1)), session, &rowErr);
        }
        if (rowErr.has_value()) {
          blocks.push_back(*rowErr);
          return true;
        }
      }
      if (info.stringTyped) {
        blocks.push_back(ojson(eval::unsupportedCommand(
            "bysort gen (string-typed)", std::nullopt, std::nullopt, "piet_gap",
            eval::UnsupportedCommandKind::kNotImplemented)));
        return true;
      }
      // BUG-112(1) PIET-half: `bysort v: gen NEW = ...` is still the `generate`
      // verb, so it takes the identical exists-guard and message. The `replace`
      // sub-command sharing this block keeps overwriting, which is its job.
      if (!isReplace) {
        if (auto genExists = genTargetNameError(session, genTargetName(info.expr),
                                                strippedTypeWord(subJoined, info))) {
          blocks.push_back(*genExists);
          return true;
        }
      }
      // BS-002: an explicit parenthetical sort key (if present) overrides
      // the sortFirst-driven default sort() clause — see makeBygenExpr's
      // by()/sort() split (egen.cpp) and BysortPrefix::sortVars above.
      std::optional<std::vector<std::string>> sortVarsOpt;
      if (!bp.sortVars.empty()) sortVarsOpt = bp.sortVars;
      // BUG-532: under `bysort`, runBysortGen sorts the rows in place by KEYS
      // SORT first (Stata's order and marker), then computes.
      // BUG-471 T2: the same resolver as the plain arms (bysort_gen_untyped :686,
      // bysort_gen_double :700); a by-prefixed replace keeps the variable's type.
      const std::string byStoreType =
          isReplace ? storage::resolveReplaceType(session, genTargetName(info.expr))
                    : storage::resolveCreateType(session, info.typeToken);
      // BUG-1436: a string condition is one the engine's --bygen cannot read
      // (`bysort g: replace x = 10 if strlen(s) > 2` was rc 1 `function strlen()
      // is not supported`; StataBE 19.5 replaces 3 rows). It reads one row at a
      // time, so the whole-frame row mask is every group's answer and follows the
      // rows through the sort, as on the by-driver below. `_n`/`_N` count within
      // the group: the mask reads them numbered per group, over rows already in
      // bysort's order (runBysortGen then finds them sorted).
      if (ifExpr.has_value() && session.hasData() &&
          strmutate::needsConditionMask(session, *ifExpr)) {
        const bool groupObs = std::regex_search(*ifExpr, std::regex(R"(\b_[nN]\b)"));
        if (groupObs && bp.sortFirst) {
          if (auto sortErr = handlers::sortForBysort(session, bp.byVars, sortVarsOpt)) {
            blocks.push_back(*sortErr);
            return true;
          }
        }
        strmutate::ConditionMaskInstall mi = strmutate::installConditionMask(
            session, *ifExpr, groupObs ? bp.byVars : std::vector<std::string>{});
        if (!mi.ok) {
          blocks.push_back(mi.error);
          return true;
        }
        conditionMask.columns.push_back(mi.column);
        ifExpr = mi.condition;
      }
      ojson mutBlock = handlers::runBysortGen(session, info.expr, bp.byVars, ifExpr, bp.sortFirst,
                                              isReplace, /*inRange=*/std::nullopt, sortVarsOpt,
                                              byStoreType, noPromote);
      blocks.push_back(mutBlock);
      // BUG-250: `bysort id: gen double lagy = ...` is still the generate verb
      // (p2perf_bench.do:207-208 is exactly this shape). replace declares no
      // type in Stata, so only the gen half records.
      if (!isReplace) {
        recordDeclaredStataType(session, genTargetName(info.expr),
                                withSessionDefaultType(session, info), blocks.back());
        appendGenMissingNote(blocks, genTargetName(info.expr));  // BUG-1123
        const bool madeFailed = mutBlock.is_object() &&
                                mutBlock.value("type", std::string()) == "error";
        if (!madeFailed && !placeWord.empty())
          placeGeneratedVariable(session, genTargetName(info.expr), placeWord, placeVar, blocks);
      }
      return true;
    }

    if (subCmdLower == "egen" && !bp.byVars.empty()) {
      auto [head, opts] = splitTopLevelComma(bp.subRaw);
      std::string byClause = "by(" + joinSpace(bp.byVars) + ")";
      std::string inner;
      static const std::regex reByOpt(R"(\bby\s*\()", std::regex::icase);
      if (!opts.has_value()) {
        inner = trimWs(head) + ", " + byClause;
      } else if (std::regex_search(*opts, reByOpt)) {
        inner = bp.subRaw;  // explicit by() wins
      } else {
        inner = trimWs(head) + ", " + trimWs(*opts) + " " + byClause;
      }
      // BUG-532: Stata's `bysort KEYS (SORT): egen` sorts the data in place by
      // KEYS SORT first and leaves them sorted (see runBysortGen); egen's own
      // by() then computes on those rows.
      if (bp.sortFirst) {
        std::vector<std::string> sortKeys = bp.byVars;
        sortKeys.insert(sortKeys.end(), bp.sortVars.begin(), bp.sortVars.end());
        if (std::optional<ojson> sortErr = handlers::sortInStataOrder(session, sortKeys)) {
          blocks.push_back(*sortErr);
          return true;
        }
      }
      ojson innerBlocks = dispatch(inner, session);
      for (const auto& b : innerBlocks) {
        if (!(b.is_object() && b.value("type", "") == "echo")) blocks.push_back(b);
      }
      return true;
    }

    // AER-SCOPE1 bundle I (UTIL): `bysort id: carryforward y, gen(f)` and
    // `bysort id: tsspell x` reach their handlers the way egen does -- the
    // by-variables are injected as a `by()` option and the line is
    // re-dispatched. Both handlers are group-aware; the prefix supplies the
    // grouping, not a loop.
    //
    // It sorts for BOTH `bysort` and plain `by:`, which is the same choice the
    // `by: summarize` branch below makes and for a stronger reason. Real Stata
    // refuses a plain `by:` on unsorted data (r(5), "not sorted"); replicating
    // that needs a sortedness oracle PIET does not expose to this layer -- the
    // `sorted_by` claim lives in the .hbin header and has no accessor here. Of
    // the two ways to be wrong, sorting is the safe one PRECISELY BECAUSE these
    // two verbs are order-dependent: carryforward propagates the last
    // non-missing value forward and tsspell walks runs, so running them on an
    // unsorted frame produces a different, entirely plausible answer. Sorting
    // makes them right; refusing would only be more faithful.
    //
    // BUG-623: the sort is on the by-keys AND the parenthesised sort keys.
    // `bysort id (t): carryforward y, gen(f)` sorted by id alone, so the fill
    // walked each id in whatever order the rows happened to be in, not by t
    // (main, t = 4 3 2 1 with y set only at t = 1: f filled 1 row, Stata 4).
    // Stata's `bysort id (t):` sorts by id t, and a plain `by id (t):` needs
    // the data in that order.
    // BUG-1015: cumul is byable(onecall) too -- its by() option IS the prefix
    // (cumul's by() option takes the prefix's variables), so it rides the same injection.
    // ipolate is byable(onecall) the same way (a by() option beside the prefix
    // is r(190)); its own
    // `sort` attribute restores the order, so the bysort order is the one left.
    // w2d_uncovered.log ipolate_by_generate_090 / ipolate_bys_gen_ctl `0|1|3|84|`.
    // AER tier-1: `by id: fillmissing x, with(any)` (023, all 35 lines) and
    // `bysort g: astile q = x, nq(3)` ride the same injection; both handlers
    // read by() as their grouping.
    // tsspell is not byable: it groups by the tsset panel itself, and a by
    // prefix is r(190) "tsspell may not be combined with by" (vs-stata bank
    // 52348252ac3ce36c `bysort up: tsspell, cond(up)`).
    if (subCmdLower == "tsspell" && !bp.byVars.empty()) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                              "tsspell may not be combined with by", std::nullopt,
                                              std::nullopt, std::nullopt, "r(190)")));
      return true;
    }
    if ((subCmdLower == "carryforward" || subCmdLower == "cumul" ||
         subCmdLower == "ipolate" || subCmdLower == "ipol" || subCmdLower == "ipo" ||
         subCmdLower == "fillmissing" || subCmdLower == "astile") &&
        !bp.byVars.empty()) {
      if (subCmdLower.rfind("ipo", 0) == 0) {
        auto [ihead, iopts] = splitTopLevelComma(bp.subRaw);
        (void)ihead;
        static const std::regex reIpoBy(R"((^|[\s,])by\s*\()", std::regex::icase);
        if (iopts.has_value() && std::regex_search(*iopts, reIpoBy)) {
          blocks.push_back(ojson(eval::errorBlock(
              "syntax_error", "option by() may not be combined with by prefix", std::nullopt,
              std::nullopt, std::nullopt, "r(190)")));
          return true;
        }
      }
      std::vector<std::string> cfSortKeys = bp.byVars;
      cfSortKeys.insert(cfSortKeys.end(), bp.sortVars.begin(), bp.sortVars.end());
      ojson sortBlocks = dispatch("sort " + joinSpace(cfSortKeys), session);
      for (const auto& b : sortBlocks) {
        if (b.is_object() && b.value("type", "") == "error") {
          blocks.push_back(b);
          return true;
        }
      }
      auto [head, opts] = splitTopLevelComma(bp.subRaw);
      std::string byClause = "by(" + joinSpace(bp.byVars) + ")";
      std::string inner;
      static const std::regex reByOpt2(R"(\bby\s*\()", std::regex::icase);
      if (!opts.has_value()) {
        inner = trimWs(head) + ", " + byClause;
      } else if (std::regex_search(*opts, reByOpt2)) {
        // fillmissing.ado refuses the pair -- two lines `as error`, then a
        // plain `exit`, so rc 0 and nothing filled (StataBE, review of the
        // tier-1 GEN lane, review-gen/ga.log A5). The data stay sorted, as
        // the bysort prefix left them.
        if (subCmdLower == "fillmissing") {
          for (const char* line : {"Options bysort and by() cannot be combined",
                                   "Use either bysort or by(varlist)"}) {
            ojson d;
            d["type"] = "display";
            d["text"] = line;
            blocks.push_back(d);
          }
          return true;
        }
        inner = bp.subRaw;  // explicit by() wins, same rule as egen
      } else {
        inner = trimWs(head) + ", " + trimWs(*opts) + " " + byClause;
      }
      ojson innerBlocks = dispatch(inner, session);
      for (const auto& b : innerBlocks) {
        if (!(b.is_object() && b.value("type", "") == "echo")) blocks.push_back(b);
      }
      return true;
    }

    // BUG-532: `by/bysort KEYS [(SORT)]: keep if COND` and `drop if COND` — the
    // only by-able forms of keep/drop ([D] drop). COND is evaluated per by-group
    // (group-local _n/_N, x[k], sum()); a missing COND counts as true. Oracle:
    // tests/eval/golden/aer_w2/w2_bykeep.log.
    //
    // Order is established HERE, the way Stata does it: `bysort` sorts the data
    // in place on KEYS++SORT in Stata's order (a string key by its bytes, not by
    // the engine's first-seen dictionary code — arm bys_p_str) and leaves it
    // sorted; plain `by` requires the rows to be in that order already and
    // refuses with r(5) otherwise, data untouched. For plain `by`, "in order" is
    // Stata's sortedby verdict as far as PIET can know it: the engine file's own
    // sort claim (engineSortClaim — arms by_p_after_bysgen, by_two_halfsorted),
    // else the rows themselves, measured under Stata's string order or the
    // engine's (keysAlreadyGrouped; the latter is the order PIET's own `sort` on
    // a string key leaves). The level-string, first-key-only check the summarize
    // branch below uses would call a string key PIET just sorted "not sorted".
    if ((subCmdLower == "keep" || subCmdLower == "drop") && !bp.byVars.empty()) {
      eval::ParsedCommand subParsed;
      try {
        subParsed = eval::tokenize(bp.subRaw);
      } catch (const eval::TokenizerError& te) {
        blocks.push_back(ojson(te.block));
        return true;
      }
      if (auto err = eval::rejectUnknownOptions(subParsed.opts, {})) {
        blocks.push_back(ojson(*err));
        return true;
      }
      if (subParsed.inRange.has_value()) {  // arms by_keep_in / by_keep_if_in
        blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                                "'in' may not be combined with 'by'",
                                                "in " + *subParsed.inRange, std::nullopt,
                                                std::nullopt, "r(190)")));
        return true;
      }
      // `by g: keep VARLIST` / `by g: drop VARLIST` is r(198) invalid syntax in
      // Stata (arms by_keep_varlist / by_drop_varlist): only the `if` form is
      // by-able.
      if (!subParsed.ifExpr.has_value() || trimWs(*subParsed.ifExpr).empty() ||
          !subParsed.varlist.empty()) {
        blocks.push_back(ojson(eval::errorBlock(
            "syntax_error",
            "invalid syntax — under `by:` only `" + subCmdLower + " if EXP` is allowed; `" +
                subCmdLower + " VARLIST` takes no by-prefix",
            subCmdTyped, std::nullopt, std::nullopt, "r(198)")));
        return true;
      }
      std::optional<ojson> rowErr;
      const std::string cond = substituteRowExprRefs(*subParsed.ifExpr, session, &rowErr);
      if (rowErr.has_value()) {
        blocks.push_back(*rowErr);
        return true;
      }
      if (!session.hasData()) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return true;
      }
      Datastore* baseDs = session.datastore();
      if (baseDs == nullptr) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return true;
      }
      std::vector<std::string> keys = bp.byVars;
      keys.insert(keys.end(), bp.sortVars.begin(), bp.sortVars.end());
      for (const auto& k : keys) {
        if (!baseDs->has(k)) {  // arms bys_nosuchvar / bys_p_nosuchkey: r(111)
          blocks.push_back(ojson(eval::engineError("variable " + k + " not found")));
          return true;
        }
      }
      // Stata orders the missing values of a numeric key `. < .a < ... < .z`
      // and groups each kind apart. The engine's gsort/by() collapse every
      // missing kind into one NaN (src/data/frame2.cpp Frame::gsort's BUG-272
      // comment), and so does keysAlreadyGrouped: a key holding an extended
      // missing could be grouped and ordered differently from Stata, so it is
      // refused by name before anything moves (the sort primitive itself
      // belongs to the engine lane).
      for (const auto& k : keys) {
        if (baseDs->dtype(k) != "f64") continue;
        const double* v = baseDs->col(k).f64();
        for (int64_t r = 0; r < baseDs->nRows(); ++r) {
          if (cecon::missing_kind(v[r]) > 0) {
            blocks.push_back(ojson(eval::errorBlock(
                "unsupported_command",
                "`" + std::string(bp.sortFirst ? "bysort" : "by") + " ...: " + subCmdLower +
                    " if` is not supported yet when a key variable holds extended missing "
                    "values (.a-.z): `" + k +
                    "` does, and this engine would order and group every missing kind as "
                    "one value.",
                std::nullopt, std::nullopt, std::nullopt, "piet_gap")));
            return true;
          }
        }
      }
      // `bysort` sorts in place on KEYS++SORT (runByKeepIf, through runSort),
      // and the marker becomes KEYS++SORT (w2e_probes.log bo_sort_prefix_noop:
      // a coarser `sort g` on data sorted by g x leaves "g"). Plain `by`
      // never sorts: it runs when Stata's sortedby marker --
      // the engine file's own claim, which every `bysort` now leaves on PIET's
      // physically sorted rows -- leads with KEYS++SORT (arms by_p_after_bysgen,
      // by_p_sorted), and is r(5) otherwise (by_p_wrongkey, by_two_halfsorted).
      // With no claim at all (a string key -- the engine mints none -- or data
      // from a claimless transform) the rows themselves are measured: the named
      // permissive residual, rc 0 where Stata may answer r(5).
      std::vector<std::string> sortOn;  // empty = the rows are in order already
      if (bp.sortFirst) {
        sortOn = keys;
      } else {
        bool inOrder = false;
        if (const std::vector<std::string> claim = engineSortClaim(*baseDs); !claim.empty()) {
          inOrder = keysLeadClaim(keys, claim);
        } else {
          inOrder = keysAlreadyGrouped(*baseDs, keys, /*stataStringOrder=*/true) ||
                    keysAlreadyGrouped(*baseDs, keys, /*stataStringOrder=*/false);
        }
        if (!inOrder) {  // arms by_unsorted / by_p_wrongkey / by_drop_unsorted / by_two_halfsorted
          blocks.push_back(ojson(eval::errorBlock(
              "not_sorted",
              "not sorted — `by " + joinSpace(bp.byVars) +
                  (bp.sortVars.empty() ? std::string() : " (" + joinSpace(bp.sortVars) + ")") +
                  ":` requires the data to already be sorted by " + joinSpace(keys) +
                  ". Use `bysort` to sort first, or run `sort " + joinSpace(keys) +
                  "` before this command.",
              std::nullopt, std::nullopt, std::nullopt, "r(5)")));
          return true;
        }
      }
      // baseDs is not used past this point: the handler may replace the head.
      const int64_t byRowsBefore = baseDs->nRows();
      ojson kept = ojson(handlers::runByKeepIf(session, cond, bp.byVars, sortOn,
                                               /*drop=*/subCmdLower == "drop"));
      // drop and keep are r-class: r(N_drop), the observations removed (vs-stata bank:
      // `bysort foreign: drop if _n>3` -> 68).
      if (kept.is_object() && kept.value("type", std::string()) != "error" &&
          kept.contains("n_rows_after") && kept["n_rows_after"].is_number_integer()) {
        eval::replaceRClass(session.eval);
        session.eval.rValues.set("N_drop",
                                 static_cast<double>(byRowsBefore -
                                                     kept["n_rows_after"].get<int64_t>()));
        eval::declareRPosted(session.eval, subCmdLower, {"N_drop"});
      }
      blocks.push_back(std::move(kept));
      return true;
    }

    // by/bysort with the tabulate family (tabulate, tab, ta, tab1, tab2). Stata runs the command
    // once for each by-group, each table under a `-> G = value` heading (the value label when the
    // variable has one), and r() is the last group's (bank fea60afe `bysort foreign: tab rep78`,
    // 41a9b22e `bysort foreign: tab2 rep78` then `display r(N)`). The data are in by-order here
    // (bysort sorts them in place; plain by requires it, r(5)), so each group is a contiguous run of
    // observations and the command runs as typed on that run, `in first/last`, through the ordinary
    // tabulate route with all of its options, weights and `if`.
    // xtsum and xttab are byable the same way (bank 5d68d221 `bysort jail: xtsum`).
    if ((subCmdLower == "tabulate" || subCmdLower == "tab" || subCmdLower == "ta" ||
         subCmdLower == "tab1" || subCmdLower == "tab2" || subCmdLower == "xtsum" ||
         subCmdLower == "xttab") &&
        !bp.byVars.empty()) {
      eval::ParsedCommand subParsed;
      try {
        subParsed = eval::tokenize(bp.subRaw);
      } catch (const eval::TokenizerError& te) {
        blocks.push_back(ojson(te.block));
        return true;
      }
      if (subParsed.inRange.has_value()) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "in may not be combined with by",
                                                std::nullopt, std::nullopt, std::nullopt, "r(190)")));
        return true;
      }
      // tabulate is a built-in: its `if` counts `_n`, `_N` and x[k] within the by-group, and its
      // generate() is refused under by before any group runs ("The generate() option may not be
      // used with the by prefix", tabulate oneway's help). tab1, tab2, xtsum and xttab are
      // byable(recall) ados: each group's call marks its sample from the `if` over the whole
      // data, so their `_n` stays the frame's, and an option the ado hands to tabulate is that
      // inner tabulate's, which is not by-prefixed.
      const bool builtinTab =
          subCmdLower == "tabulate" || subCmdLower == "tab" || subCmdLower == "ta";
      if (builtinTab) {
        for (const auto& [k, v] : subParsed.opts) {
          if (resolveOptionKeyForCommand("tabulate", lower(k), v.isBool ? 1 : 2).resolved ==
              "generate") {
            blocks.push_back(ojson(eval::errorBlock(
                "syntax_error", "generate() may not be combined with by", std::nullopt,
                std::nullopt, std::nullopt, "r(190)")));
            return true;
          }
        }
      }
      Datastore* tds = session.datastore();
      if (tds == nullptr) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return true;
      }
      std::vector<std::string> keys = bp.byVars;
      keys.insert(keys.end(), bp.sortVars.begin(), bp.sortVars.end());
      for (const auto& k : keys) {
        if (!tds->has(k)) {
          blocks.push_back(ojson(eval::errorBlock("engine_error", "variable " + k + " not found",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  "r(111)")));
          return true;
        }
      }
      if (bp.sortFirst) {
        if (std::optional<ojson> sortErr = handlers::sortInStataOrder(session, keys)) {
          blocks.push_back(*sortErr);
          return true;
        }
        tds = session.datastore();
      } else {
        const std::vector<std::string> claim = engineSortClaim(*tds);
        const bool inOrder = claim.empty() ? keysAlreadyGrouped(*tds, keys, true)
                                          : keysLeadClaim(keys, claim);
        if (!inOrder) {
          blocks.push_back(ojson(eval::errorBlock(
              "not_sorted", "not sorted — `by " + joinSpace(bp.byVars) +
                  ":` requires the data to already be sorted by " + joinSpace(keys) + ".",
              std::nullopt, std::nullopt, std::nullopt, "r(5)")));
          return true;
        }
      }
      const int64_t nRows = tds->nRows();
      // The group keys and headings, read once: the inner tabulates never move a row.
      std::vector<std::vector<std::string>> keyText(static_cast<size_t>(nRows));
      for (const auto& bv : bp.byVars) {
        const bool isFactor = tds->dtype(bv) != "f64";
        const cecon::Column& c = tds->col(bv);
        const std::vector<std::string> levels =
            isFactor ? tds->varinfo(bv).levels : std::vector<std::string>{};
        const auto vl = session.valueLabels.find(bv);
        const std::string keyFormat = isFactor ? std::string() : session.displayFormat(tds->varinfo(bv));
        for (int64_t r = 0; r < nRows; ++r) {
          std::string text;
          if (isFactor) {
            const int32_t code = c.codes()[r];
            text = code >= 0 && static_cast<size_t>(code) < levels.size()
                       ? levels[static_cast<size_t>(code)] : std::string();
          } else {
            const double x = c.f64()[r];
            if (std::isnan(x)) {
              text = cecon::missing_text(cecon::missing_kind(x));
            } else {
              bool labelled = false;
              if (vl != session.valueLabels.end() && x == std::floor(x) && std::fabs(x) < 2e9) {
                auto it = vl->second.find(static_cast<int>(x));
                if (it != vl->second.end()) {
                  text = it->second;
                  labelled = true;
                }
              }
              // An unlabelled value is shown in the variable's display format (bank
              // c155c25af604378d: float dry under %9.0g is `-> dry = 39.5696`, `.03692`).
              if (!labelled) text = trimWs(eval::applyStataFmt(x, keyFormat));
            }
          }
          keyText[static_cast<size_t>(r)].push_back(bv + " = " + text);
        }
      }
      // Each missing kind and each labelled code is its own group: compare the raw keys.
      auto sameGroup = [&](int64_t a, int64_t b) {
        for (const auto& bv : bp.byVars) {
          const cecon::Column& c = tds->col(bv);
          if (tds->dtype(bv) != "f64") {
            if (c.codes()[a] != c.codes()[b]) return false;
          } else if (cecon::stata_cmp(c.f64()[a], c.f64()[b]) != 0) {
            return false;
          }
        }
        return true;
      };
      const long long comma = eval::findUnquotedComma(bp.subRaw);
      const std::string head =
          comma >= 0 ? bp.subRaw.substr(0, static_cast<size_t>(comma)) : bp.subRaw;
      const std::string tail = comma >= 0 ? bp.subRaw.substr(static_cast<size_t>(comma)) : "";
      // The group boundaries are fixed before the first inner command runs: it may replace the
      // session's datastore (r() posting, generate()), so no column view outlives this scan.
      std::vector<std::pair<int64_t, int64_t>> groups;
      for (int64_t g0 = 0; g0 < nRows;) {
        int64_t g1 = g0 + 1;
        while (g1 < nRows && sameGroup(g0, g1)) ++g1;
        groups.emplace_back(g0, g1);
        g0 = g1;
      }
      tds = nullptr;
      std::string innerHead = trimWs(head);
      if (builtinTab && subParsed.ifExpr.has_value() && !subParsed.ifExpr->empty() &&
          (std::regex_search(*subParsed.ifExpr, std::regex(R"(\b_[nN]\b)")) ||
           subParsed.ifExpr->find('[') != std::string::npos)) {
        // The `if` is evaluated once, numbered within each group, into a hidden 0/1 column the
        // group runs then read in its place (`bysort foreign: tab rep78 if _n <= 5` tabulates
        // each group's first five observations, as `by: summarize` does).
        strmutate::ConditionMaskInstall mi =
            strmutate::installGroupConditionMask(session, *subParsed.ifExpr, bp.byVars);
        if (!mi.ok) {
          blocks.push_back(mi.error);
          return true;
        }
        conditionMask.columns.push_back(mi.column);
        innerHead = subParsed.cmd + " " + trimWs(subParsed.argText) + " if " + mi.condition;
        if (subParsed.weightType.has_value())
          innerHead += " [" + *subParsed.weightType + "=" +
                       subParsed.weightExp.value_or(subParsed.weightVar.value_or("")) + "]";
      }
      for (const auto& [g0, g1] : groups) {
        std::string heading = "-> ";
        for (size_t i = 0; i < keyText[static_cast<size_t>(g0)].size(); ++i)
          heading += (i ? " " : "") + keyText[static_cast<size_t>(g0)][i];
        ojson header;
        header["type"] = "display";
        header["text"] = heading;
        blocks.push_back(header);
        const std::string inner = innerHead + " in " + std::to_string(g0 + 1) + "/" +
                                  std::to_string(g1) + (tail.empty() ? "" : tail);
        ojson innerBlocks = dispatch(inner, session);
        bool failed = false;
        for (const auto& b : innerBlocks) {
          if (b.is_object() && b.value("type", "") == "echo") continue;
          // A group with no observations prints "no observations" and the loop goes on, ending
          // rc 0 (bank 6e19cab3: `bysort sv: xttab jl`, the sv = . group).
          if (b.is_object() && b.value("type", "") == "error" &&
              b.value("message", std::string()) == "no observations") {
            ojson note;
            note["type"] = "display";
            note["text"] = "no observations";
            blocks.push_back(note);
            continue;
          }
          if (b.is_object() && b.value("type", "") == "error") failed = true;
          blocks.push_back(b);
        }
        if (failed) return true;
      }
      return true;
    }

    // BS-092 (BETA campaign, Phase G): `by:`/`bysort: summarize` — the ONE
    // OTHER subcommand (besides gen/replace/egen above) that gets a genuine
    // PIET-side per-group implementation this phase; the other 7
    // Stata-legal-but-unimplemented stems the fallback message below still
    // names (correlate/pwcorr, sdtest, ttest, tabstat, tabulate family,
    // count, list) stay OUT of this batch's scope (BS-107, unchanged).
    //
    // Real Stata's `by G: summarize X` sorts by G (`bysort` re-sorts in
    // place; plain `by` requires the data ALREADY sorted, erroring "not
    // sorted" otherwise) then re-runs `summarize` once per contiguous
    // distinct-G block, printing a `-> G = value` header ahead of each
    // group's own stats table (live-verified this stint, StataBE:
    // `bysort state: summarize beertax` -> "-> state = al" then the usual
    // Variable/Obs/Mean/... table). This sorts UNCONDITIONALLY for both
    // `by` and `bysort` (the only divergence from real Stata is a plain
    // `by:` on genuinely unsorted data, where Stata refuses to run at all —
    // silently sorting instead of replicating that reject is the
    // conservative choice: it always answers the CORRECTLY grouped
    // question rather than risking a wrong one) into a FRESH scratch copy
    // (piet::handlers::sortForGrouping) — summarize is read-only, so the
    // live session dataset's row order must never change as a side effect.
    // (BUG-532, 2026-09-26: that holds for a plain `by:` only. Stata's
    // `bysort` sorts the data in place, and so does this driver now, through
    // handlers::sortInStataOrder below; the copy is then skipped.)
    // BUG-105 abbreviation stopgap: `su` is matched here alongside `sum`
    // so `bysort G: su X` reaches this real per-group implementation. Left
    // out, it would fall through to the honest "by:/bysort: is only
    // implemented for gen/replace/egen/summarize" reject below — true of
    // the token, false of the command it abbreviates.
    // P4 wave: the same driver now also carries `regress`/`reg`. Everything from
    // the sort through the boundary scan and the per-group slice is command-
    // agnostic — only the option net and the one per-group call below differ —
    // so `regress` graduates by joining this condition rather than by growing a
    // second copy of the group machinery. (ROADMAP P4: "generic group driver".)
    // W:P5: `tabstat` joins on the same terms regress did in P4 — the sort,
    // the boundary scan and the per-group slice are command-agnostic, so the
    // only additions are one flag and one per-group call below. Real Stata
    // prints a `-> region = N` header then that group's own table, which is
    // exactly the shape this driver already emits.
    const bool byIsSummarize =
        (subCmdLower == "summarize" || subCmdLower == "sum" || subCmdLower == "su");
    const bool byIsRegress = (subCmdLower == "regress" || subCmdLower == "reg");
    const bool byIsTabstat = (subCmdLower == "tabstat");
    // `count` joins the same driver (FU-163): it takes no varlist and no options,
    // and Stata leaves r(N) = the LAST group's count.
    const bool byIsCount = (subCmdLower == "count");
    // vs-stata stage 4: r-class tests and descriptive estimators that read the
    // data and change nothing but r()/e(). Stata's `by G: CMD` runs CMD once per
    // group on that group's rows (bank: `bysort foreign: ttest mpg == 20` leaves
    // the Foreign group's r(N_1) 22, r(t) 3.386090207522228; `bysort foreign:
    // pca ...` the last group's e()). After the sort each group is a contiguous
    // row range, so each group's run is the ordinary command restricted with
    // `in FIRST/LAST`, dispatched as typed (its own if, weights and options
    // included). A by-line whose `if` counts rows (`_n`, `_N`, a subscript) is
    // within-group in Stata and frame-relative under `in`: those keep the
    // refusal below, as do a line that already carries `in` and one whose text
    // would be macro-expanded a second time by the re-dispatch.
    const bool byIsGeneric = [&] {
      static const char* const kGenericBy[] = {"corr", "pwcorr", "ttest", "sdtest",
                                               "signrank", "spearman", "pca"};
      if (std::find(std::begin(kGenericBy), std::end(kGenericBy), subCmdLower) ==
          std::end(kGenericBy))
        return false;
      if (bp.subRaw.find('`') != std::string::npos || bp.subRaw.find('$') != std::string::npos)
        return false;
      eval::ParsedCommand probe;
      try {
        probe = eval::tokenize(bp.subRaw);
      } catch (const eval::TokenizerError&) {
        return true;  // the per-group dispatch reports the command's own syntax error
      }
      if (probe.inRange.has_value()) return false;
      if (probe.ifExpr.has_value() &&
          std::regex_search(*probe.ifExpr, std::regex(R"(\b_[nN]\b|\[)")))
        return false;
      return true;
    }();
    // `list` is byable too: each group's rows in its own table, numbered within the
    // group (absolute: in the whole data), its `if` seeing the group's `_n` (bank
    // e37d8f995c91d0e5 `bysort foreign: list make price if _n<=3`).
    const bool byIsList = (subCmdLower == "list" || subCmdLower == "l" || subCmdLower == "li" ||
                           subCmdLower == "lis");
    if (byIsSummarize || byIsRegress || byIsTabstat || byIsCount || byIsList || byIsGeneric) {
      if (bp.byVars.empty()) {
        blocks.push_back(ojson(eval::engineError("bysort/by: no group variable(s) given")));
        return true;
      }
      eval::ParsedCommand subParsed;
      try {
        subParsed = eval::tokenize(bp.subRaw);
      } catch (const eval::TokenizerError& te) {
        blocks.push_back(ojson(te.block));
        return true;
      }
      // Stata's own refusal comes first: summarize's [iw=] is illegal with
      // detail, under by: too (StataBE r(101), BUG-1444; the unprefixed rule
      // is QF-SUM-IW below).
      const bool subDetail = std::any_of(
          subParsed.opts.begin(), subParsed.opts.end(), [](const auto& kv) {
            return (kv.first == "detail" || kv.first == "d") && kv.second.isBool &&
                   kv.second.boolValue;
          });
      if (byIsSummarize && subDetail && subParsed.weightType.has_value() &&
          (lower(*subParsed.weightType) == "iw" || lower(*subParsed.weightType) == "iweight")) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "iweights not allowed",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(101)")));
        return true;
      }
      // BUG-1520: summarize's [aw=]/[fw=]/[iw=] and tabstat's [aw=]/[fw=] run in
      // each group with the engine weight flag their unprefixed arms pass
      // (StataBE 19.5: `by g: summarize x [aw=wf]` leaves the last group's r(N)
      // 4, r(sum_w) 6.5). Any other weighted by-prefixed summarize/tabstat/count
      // is refused by name rather than run unweighted (a by-line's weight
      // passes the tokenizer under the sub-command's policy).
      std::string byWeightFlag;
      if (subParsed.weightType.has_value() && subParsed.weightVar.has_value() &&
          (byIsSummarize || byIsTabstat)) {
        const std::string wt = lower(*subParsed.weightType);
        if (wt == "aw" || wt == "aweight") byWeightFlag = "--aweight";
        else if (wt == "fw" || wt == "fweight") byWeightFlag = "--fweight";
        else if (byIsSummarize && (wt == "iw" || wt == "iweight")) byWeightFlag = "--iweight";
      }
      if (!byIsRegress && !byIsGeneric && subParsed.weightType.has_value() &&
          byWeightFlag.empty()) {
        blocks.push_back(ojson(eval::unsupportedOption(
            "[" + *subParsed.weightType + "=" + subParsed.weightVar.value_or("") + "]",
            "a weight on a by-prefixed `" + subCmdTyped +
                "` is not implemented in this version of PIET; run it separately for each group.")));
        return true;
      }
      std::optional<std::string> tabstatIfAfterRows;  // see the `_rc` note below
      // e(sample) T2/T4: the per-group `if` reaches applyIfAndInFilter as raw
      // text (below), past the whole-line rewrite, so it takes the SAME row
      // rewrite every other `if` takes (substituteRowExprRefs: e(sample) first,
      // then _b[]/_se[]/e()/scalar()/stored scalars). Before T4 only e(sample)
      // was rewritten here, and `by id: count if x > scalar(s)` / `... if x >
      // _b[x]` reached the engine as text (rc 1 `function scalar() is not
      // supported`, r(111) `unknown variable _b`). summarize/tabstat/count leave
      // e() alone, so the text resolved once is every group's. A by-prefixed
      // REGRESS replaces e() after each group, so a later group's `if` reads the
      // previous group's fit in Stata; that is not followed here and is refused
      // by name (r(198)) instead of answered with the pre-loop results.
      if (subParsed.ifExpr.has_value() && !byIsGeneric) {
        const std::string ie = *subParsed.ifExpr;  // a copy: the `if` is replaced below
        // BUG-1518: a `_b[`/`_se[` that ends a longer name (`g_b[1]`) is that
        // variable's subscript, not a coefficient.
        auto coefRef = [&ie](const char* tok) {
          for (size_t p = ie.find(tok); p != std::string::npos; p = ie.find(tok, p + 1))
            if (p == 0 || !cecon::ident::isIdentByte(ie[p - 1])) return true;
          return false;
        };
        if (byIsRegress && (eval::hasEsampleToken(ie) || coefRef("_b[") || coefRef("_se[") ||
                            eval::hasBareEParen(ie))) {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error",
              "e(sample), _b[], _se[] and e() in the `if` of a by-prefixed estimation command are "
              "not available in this version of PIET: each group's fit replaces e() before the "
              "next group's `if` is read.",
              std::nullopt, std::nullopt, std::nullopt, "r(198)")));
          return true;
        }
        auto prepareIf = [&](std::string& out) -> bool {
          std::optional<ojson> rowErr;
          out = substituteRowExprRefs(ie, session, &rowErr, !byIsRegress);
          if (rowErr.has_value()) {
            blocks.push_back(*rowErr);
            return false;
          }
          // The string-condition route (see dispatchImpl's QualifierColumnGuard): a
          // string function reads one row at a time, so the whole-frame mask is
          // every group's answer, and it follows the rows through the sort below
          // (qf_if_by_sum_strpos / qf_if_by_count_strpos; FU-501). `_n`/`_N` are
          // group-relative under by: and a whole-frame mask cannot carry them.
          if (strmutate::needsConditionMask(session, out) && session.hasData() &&
              !std::regex_search(out, std::regex(R"(\b_[nN]\b)"))) {
            strmutate::ConditionMaskInstall mi = strmutate::installConditionMask(session, out);
            if (!mi.ok) {
              blocks.push_back(mi.error);
              return false;
            }
            conditionMask.columns.push_back(mi.column);
            out = mi.condition;
          }
          return true;
        };
        std::string prepared;
        if (!prepareIf(prepared)) return true;
        subParsed.ifExpr = prepared;
        // tabstat is an ado command: each group's run leaves `_rc` 0 once it
        // has rows to tabulate (a group with none leaves it as it was), so only
        // the groups up to the first with rows read the caller's `_rc` (StataBE
        // 19.5, 2026-11-03, three groups, caller _rc 111: `by g: tabstat x if
        // _rc == 111` tabulates the first group only, `... if _rc == 0` none,
        // `... if _rc == 0 | g == 0` all three). The `if` the later groups read
        // is the same text with `_rc` 0.
        const std::string callerRc = session.eval.locals.get("_rc", "0");
        if (byIsTabstat && callerRc != "0" && substituteRcValue(ie, session) != ie) {
          session.eval.locals.set("_rc", "0");
          std::string afterRows;
          const bool ok = prepareIf(afterRows);
          session.eval.locals.set("_rc", callerRc);
          if (!ok) return true;
          tabstatIfAfterRows = afterRows;
        }
      }
      bool detail = false;
      bool byMeanonly = false;
      if (byIsCount) {
        // The plain `count` arm's two nets, in the same order: leftover tokens
        // (the BUG-010 fixed-arity guard, stage_validation.cpp) then the empty
        // option vocabulary (BS-097); neither may degrade to rc 1. A by prefix
        // does not change the command's own syntax answer: a word in the
        // varlist position is r(101) "varlist not allowed" as `count price` is
        // (vs-stata bank 6c4f982fed18e7ae), and an option r(101) "options not
        // allowed" as `count, zznotanoption` is (c69d5acda46a3296). A
        // qualifier-shaped word keeps the r(198) below, as in the plain arm.
        if (!subParsed.varlist.empty()) {
          const std::string& w = subParsed.varlist.front();
          const bool qualifierShaped = w == "in" || w == "if" || w.rfind("in(", 0) == 0 ||
                                       w.rfind("if(", 0) == 0;
          if (!qualifierShaped) {
            blocks.push_back(ojson(eval::errorBlock("engine_error", "varlist not allowed",
                                                    std::nullopt, std::nullopt, std::nullopt,
                                                    "r(101)")));
            return true;
          }
          blocks.push_back(ojson(eval::engineError(
              bp.subCmd + ": unexpected extra tokens after `count [if] [in] — takes no variable "
                       "list`: " + joinVarlist(subParsed.varlist) + ".",
              "syntax_error")));
          return true;
        }
        if (auto err = eval::rejectUnknownOptions(subParsed.opts, {})) {
          blocks.push_back(ojson(*err));
          return true;
        }
      }
      long bySummarizeSeparator = -1;
      if (byIsSummarize) {
        for (const auto& [k, v] : subParsed.opts) {
          if ((k == "detail" || k == "d") && v.isBool && v.boolValue) detail = true;
          if (k == "meanonly" && (!v.isBool || v.boolValue)) byMeanonly = true;
        }
        // Mirrors the plain `summarize` arm's own option net (BS-097) exactly
        // — detail/d/meanonly only, anything else an honest r(198)-parity
        // reject naming the token.
        for (const auto& [k, v] : subParsed.opts) {
          if (k == "detail" || k == "d" || k == "meanonly") continue;
          // separator(#): the rule every # variables, in each group's table (bank
          // aeb2486cb3f8544f `by foreign: summarize price mpg weight, separator(1)`).
          if ((k == "separator" || k == "sep" || k == "sepa" || k == "separ" || k == "separa" ||
               k == "separat" || k == "separato") && !v.isBool) {
            try {
              size_t used = 0;
              const long sepv = std::stol(v.strValue, &used);
              if (used == v.strValue.size()) {
                bySummarizeSeparator = sepv < 0 ? 0 : sepv;
                continue;
              }
            } catch (const std::exception&) {
            }
          }
          blocks.push_back(
              ojson(eval::engineError("option `" + k + "` not allowed", "unsupported_option")));
          return true;
        }
      }
      // regress has no option net here on purpose: runRegress owns its own
      // (vce/robust/cluster/level/noconstant/absorb…), and duplicating it would
      // be a second copy to drift out of sync. A bad option surfaces from the
      // per-group call with regress's own wording.
      // tabstat likewise: its own arm below owns the stat/statistics/columns
      // net, and this driver forwards the parsed options to the same handler.
      std::optional<std::string> byTabstatStats;
      bool byTabstatSave = false;
      std::vector<std::string> byTsKeys, byTsRows;
      if (byIsTabstat) {
        for (const auto& [k, v] : subParsed.opts) {
          if ((k == "stat" || k == "stats" || k == "statistics") && !v.isBool) {
            byTabstatStats = v.strValue;
          } else if (k == "columns" && !v.isBool) {
            // Presentation-only in Stata (which axis carries the statistics);
            // PIET renders one fixed layout, so accept and note rather than
            // reject a legal option or silently swallow it.
            continue;
          } else if (k == "save" && v.isBool) {
            byTabstatSave = v.boolValue;
          } else {
            blocks.push_back(
                ojson(eval::engineError("option `" + k + "` not allowed", "unsupported_option")));
            return true;
          }
        }
        // `by G: tabstat X, by(H)` is illegal in Stata (two grouping axes);
        // the by() option cannot reach here anyway — the prefix owns grouping.
        // BUG-1520: the unprefixed arm's statistic words and row names.
        if (const auto bad = parseTabstatStats(byTabstatStats, byTsKeys, byTsRows)) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", "unknown statistic: " + *bad,
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  "r(198)")));
          return true;
        }
        byTabstatStats = std::string();
        for (const auto& k : byTsKeys) *byTabstatStats += (byTabstatStats->empty() ? "" : " ") + k;
      }
      if (!session.hasData()) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return true;
      }
      Datastore* baseDs = session.datastore();
      if (baseDs == nullptr) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return true;
      }
      for (const auto& bv : bp.byVars) {
        if (!baseDs->has(bv)) {
          blocks.push_back(ojson(eval::engineError("variable " + bv + " not found")));
          return true;
        }
      }

      // BUG-157 (P4 wave): plain `by` REQUIRES the data to already be sorted by
      // the by-variables; Stata refuses with r(5) "not sorted" otherwise, and
      // that is exactly what `bysort` exists to avoid. BS-092 sorted
      // unconditionally for both spellings and called the silent sort "the
      // conservative choice"; it is not — it answers a question the user's
      // program did not ask for and hides a real bug in their code. `bysort`
      // still sorts, unchanged.
      //
      // Sortedness is MEASURED here (adjacent-row comparison on the base data),
      // never inferred from a stored flag: a flag can be stale after any verb
      // that reorders rows, and a wrong "already sorted" answer would put rows
      // from two different groups into one fit.
      // The engine file's sort claim, where it has one, is Stata's sort marker
      // (engineSortClaim: never false where present), and Stata's `by:` reads
      // the marker, not the rows: keys that do not lead it are r(5) even on rows
      // that happen to be grouped (bank e74318c52a02c617: `generate g=foreign`
      // on auto, sorted by foreign, then `by g: summarize price`). With no claim
      // the rows are measured, as before.
      std::vector<std::string> byClaim;
      if (!bp.sortFirst) byClaim = engineSortClaim(*baseDs);
      if (!bp.sortFirst && !byClaim.empty()) {
        std::vector<std::string> keys = bp.byVars;
        keys.insert(keys.end(), bp.sortVars.begin(), bp.sortVars.end());
        if (!keysLeadClaim(keys, byClaim)) {
          blocks.push_back(ojson(eval::errorBlock(
              "not_sorted",
              "not sorted — `by " + joinSpace(bp.byVars) + ":` requires the data to already be "
              "sorted by " + joinSpace(keys) + ". Use `bysort " + joinSpace(bp.byVars) +
              ":` to sort first, or run `sort " + joinSpace(keys) + "` before this command.",
              std::nullopt, std::nullopt, std::nullopt, "r(5)")));
          return true;
        }
      } else if (!bp.sortFirst) {
        std::string unsortedOn;
        const int64_t nBase = baseDs->nRows();
        for (const auto& bv : bp.byVars) {
          const cecon::Column& c = baseDs->col(bv);
          const bool isFac = baseDs->dtype(bv) != "f64";
          std::vector<std::string> levels;
          if (isFac) levels = baseDs->varinfo(bv).levels;
          auto keyAt = [&](int64_t r) -> std::string {
            if (!isFac) return std::string();
            int32_t code = c.codes()[r];
            return (code >= 0 && static_cast<size_t>(code) < levels.size())
                       ? levels[static_cast<size_t>(code)]
                       : std::string("\xff");  // missing sorts last, like Stata
          };
          for (int64_t r = 1; r < nBase; ++r) {
            bool decreasing;
            if (isFac) {
              decreasing = keyAt(r) < keyAt(r - 1);
            } else {
              // Stata orders missing AFTER every number, so a NaN following a
              // number is sorted, and a number following a NaN is not.
              // Missing values are ordered by kind too (. < .a < ... < .z).
              double a = c.f64()[r - 1], b = c.f64()[r];
              decreasing = cecon::stata_cmp(b, a) < 0;
            }
            if (decreasing) { unsortedOn = bv; break; }
          }
          if (!unsortedOn.empty()) break;
          // Only the FIRST by-variable can be checked independently; a later one
          // need only be sorted within each block of the earlier ones. Checking
          // it globally would reject legitimately sorted data, so stop here —
          // this catches the filed defect (a `by:` on data never sorted at all)
          // without inventing a stricter rule than Stata's.
          break;
        }
        if (!unsortedOn.empty()) {
          blocks.push_back(ojson(eval::errorBlock(
              "not_sorted",
              "not sorted — `by " + joinSpace(bp.byVars) + ":` requires the data to already be "
              "sorted by " + joinSpace(bp.byVars) + " (`" + unsortedOn + "` is not in order). "
              "Use `bysort " + joinSpace(bp.byVars) + ":` to sort first, or run `sort " +
              joinSpace(bp.byVars) + "` before this command.",
              std::nullopt, std::nullopt, std::nullopt, "r(5)")));
          return true;
        }
      }

      // BUG-532: Stata's `bysort KEYS [(SORT)]:` sorts the data in place, and
      // leaves them marked sorted by KEYS++SORT, before it runs the command. So
      // does this driver (handlers::sortInStataOrder: a string key in byte order;
      // the engine moves no row when the marker already leads with KEYS++SORT).
      // It used to sort only a scratch copy, leaving PIET's rows where Stata's
      // were not.
      // The keys were resolved above (abbreviations, review of d82bdfa6); a
      // parenthesised key that names no variable is r(111), as in Stata.
      if (bp.sortFirst) {
        std::vector<std::string> sortKeys = bp.byVars;
        sortKeys.insert(sortKeys.end(), bp.sortVars.begin(), bp.sortVars.end());
        for (const auto& sv : bp.sortVars) {
          if (!baseDs->has(sv)) {
            blocks.push_back(ojson(eval::engineError("variable " + sv + " not found")));
            return true;
          }
        }
        if (std::optional<ojson> sortErr = handlers::sortInStataOrder(session, sortKeys)) {
          blocks.push_back(*sortErr);
          return true;
        }
        baseDs = session.datastore();
        if (baseDs == nullptr) {
          blocks.push_back(ojson(eval::noDataLoaded()));
          return true;
        }
      }

      // P2-PERF ARC-C C1-③: skip the sort when it is PROVABLY the identity.
      //
      // sortForGrouping exists to make same-group rows physically contiguous,
      // and it pays a full re-sort plus a full --out write of the whole dataset
      // to do it — every time, including the overwhelmingly common case where
      // the user has just run `sort region` themselves. It cannot be skipped on
      // the strength of the `by`-spelling sortedness check a few lines up: that
      // check answers a DIFFERENT question (it compares factor LEVEL STRINGS
      // and inspects only the first by-variable, because its job is to reject a
      // user error, not to certify a permutation), and it does not run at all
      // under `bysort`.
      //
      // So certify it against gsort's OWN key semantics instead (the
      // keysAlreadyGrouped helper above, factored out for BUG-532). If the key
      // tuple is already non-decreasing under exactly that order, the
      // permutation gsort would return is the identity, so `take_rows` would
      // hand back a byte-identical frame and the whole round trip is dead work.
      // Any doubt at all — a key that dips — falls through to the sort, so
      // this can only ever remove a no-op.
      // The generic per-group run reads the session's own rows by `in`, so the
      // groups must be contiguous there (bysort has just sorted them; a plain
      // `by` on data sorted by the first key only is Stata's r(5)). bysort leaves
      // a string key in Stata's byte order, which a dictionary whose codes are
      // not byte-ordered does not follow, so the session's rows are also taken
      // as they stand when they are in that order (`gen s = cond(foreign,"a","b")`
      // + `bysort s: ttest mpg == 20` runs, as `bysort s: summarize` does).
      const bool alreadyGrouped =
          keysAlreadyGrouped(*baseDs, bp.byVars) ||
          (byIsGeneric && keysAlreadyGrouped(*baseDs, bp.byVars, /*stataStringOrder=*/true));
      if (byIsGeneric && !alreadyGrouped) {
        blocks.push_back(ojson(eval::errorBlock(
            "not_sorted", "not sorted", std::nullopt, std::nullopt, std::nullopt, "r(5)")));
        return true;
      }

      std::string sortedPath;
      std::optional<Datastore> sortedOwned;
      Datastore* sortedPtr = nullptr;
      if (alreadyGrouped) {
        sortedPath = baseDs->path();
        sortedPtr = baseDs;
      } else {
        try {
          sortedPath = handlers::sortForGrouping(session, baseDs->path(), bp.byVars);
        } catch (const CobraError& ce) {
          blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
          return true;
        }
        sortedOwned.emplace(sortedPath);
        sortedPtr = &*sortedOwned;
      }
      Datastore& sortedDs = *sortedPtr;
      int64_t nRowsSorted = sortedDs.nRows();
      if (nRowsSorted == 0) {
        blocks.push_back(ojson(eval::engineError("no observations")));
        return true;
      }

      // Raw column views over the SORTED copy for group-boundary detection
      // (adjacent-row key comparison) and header-value formatting — mirrors
      // the raw cecon::Column access pattern used throughout this file
      // (e.g. the describe/summarize-fallback block a few hundred lines up).
      struct ByColView {
        std::string name;
        bool isFactor = false;
        const double* f64 = nullptr;
        const int32_t* codes = nullptr;
        // Owned COPY, not a pointer into Datastore::varinfo()'s return —
        // that accessor returns VarInfo BY VALUE (datastore.h), so taking
        // `&varinfo(...).levels` would dangle the instant the temporary
        // VarInfo is destroyed at the end of the full-expression.
        std::vector<std::string> levels;
      };
      std::vector<ByColView> byCols;
      for (const auto& bv : bp.byVars) {
        ByColView v;
        v.name = bv;
        v.isFactor = sortedDs.dtype(bv) != "f64";
        const cecon::Column& c = sortedDs.col(bv);
        if (v.isFactor) {
          v.codes = c.codes();
          v.levels = sortedDs.varinfo(bv).levels;
        } else {
          v.f64 = c.f64();
        }
        byCols.push_back(std::move(v));
      }
      auto rowKeyEqual = [&](int64_t a, int64_t b) {
        for (const auto& v : byCols) {
          if (v.isFactor) {
            if (v.codes[a] != v.codes[b]) return false;
          } else {
            // Each missing kind is its own group (StataBE: `by x:` over . .a .z
            // gives a group per kind, as the engine's by() does).
            if (cecon::stata_cmp(v.f64[a], v.f64[b]) != 0) return false;
          }
        }
        return true;
      };
      // P4 wave correction. BS-092 rendered numeric group values with
      // eval::pythonFloatRepr ("1.0"), disclosed at the time as a cosmetic
      // simplification on the grounds that there was "no prior art to match".
      // There is now: the P4 answerbook's real-Stata capture prints
      // `-> region = 1`, not `-> region = 1.0`
      // (internal capture p4_stata.log). An
      // integral-valued double therefore renders WITHOUT the trailing ".0",
      // exactly as tabulate.cpp's tableF64Label already does for the same
      // reason; non-integral values keep their decimals.
      auto formatGroupHeader = [&](int64_t row) {
        std::string out;
        for (size_t i = 0; i < byCols.size(); ++i) {
          if (i) out += " ";
          const ByColView& v = byCols[i];
          out += v.name + " = ";
          if (v.isFactor) {
            // A string variable: a row with no level is the empty string, and its
            // header names nothing after `=` (StataBE 19.5 `-> ks = `, BUGLOG BUG-1774).
            int32_t code = v.codes[row];
            out += (code >= 0 && static_cast<size_t>(code) < v.levels.size())
                       ? v.levels[static_cast<size_t>(code)]
                       : "";
          } else {
            double x = v.f64[row];
            if (std::isnan(x)) {
              out += cecon::missing_text(cecon::missing_kind(x));
            } else if (x == std::floor(x) && std::fabs(x) < 1e15) {
              char buf[32];
              std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(x));
              // A value label names the group as Stata's header does (`-> foreign =
              // Domestic`, vs-stata bank b1c9f63b1865f435).
              std::string text = buf;
              auto ln = session.varLblname.find(v.name);
              if (ln != session.varLblname.end()) {
                auto defs = session.labelDefs.find(ln->second);
                if (defs != session.labelDefs.end()) {
                  auto hit = defs->second.find(buf);
                  if (hit != defs->second.end()) text = hit->second;
                }
              }
              out += text;
            } else {
              out += eval::pythonFloatRepr(x);
            }
          }
        }
        return out;
      };

      // P2-PERF ARC-C C1-①: the row-index column every slice below filters on
      // is a property of `sortedPath`, not of the group — build it once.
      handlers::RowIndexCache rowIdx;
      // BUG-1166 (FU-498, FU-530): a regressor list carrying a time-series
      // operator must read the full series. A slice cut with the `if` already
      // applied holds only this group's rows that pass it, so `by id: regress y
      // L.x if t > 2` lost each group's first admitted row (its L.x is the t==2
      // value the slice dropped): e(N) 3 where StataBE fits 4 (measured
      // 2026-10-29). Such a fit gets the group's WHOLE slice and its `if` as a
      // pre-evaluated column, which the engine applies as --keepif after the
      // operators (runRegress's FU-475 route). Stata serves time-series
      // operators under by: only on data sorted by panel and time, so a group
      // that can be fitted is a whole panel: the slice holds its full series.
      std::vector<std::string> byRegressors;
      if (byIsRegress && !subParsed.varlist.empty())
        byRegressors.assign(subParsed.varlist.begin() + 1, subParsed.varlist.end());
      const bool tsRegress = byIsRegress && translate::hasTsToken(byRegressors);
      try {
        if (!byIsGeneric) rowIdx = handlers::buildRowIndexCache(session, sortedPath);
      } catch (const CobraError& ce) {
        blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
        return true;
      }
      // The `if` is read once, over the whole sorted frame. summarize and count
      // are built-ins whose `if` sees the by-group: `_n`, `_N` and x[k] count
      // within it (StataBE 19.5, measured 2026-10-29 on two groups of six: `by
      // id: summarize x if _n > 2` r(N) 4, `by id: count if _n == 1` 1, `by id:
      // count if x[_n-1] < .` 5). regress and tabstat are ado commands that mark
      // their sample over the whole data, so their `_n` is the frame's (`by id:
      // regress y x if _n > 2` e(N) 6, as the slice's --keepif already counts
      // it); they need their `if` as a column only for the time-series fit's
      // engine --keepif, or when it is a string condition naming `_n`/`_N`,
      // which the pre-sort mask above could not take (BUG-1436).
      // Stata reads the `if` in the first group, so a condition that cannot be
      // evaluated is shown under that group's header.
      std::optional<ojson> ifError;
      handlers::RowIndexCache rowIdxAfterRows = rowIdx;  // tabstatIfAfterRows' frame
      std::optional<ojson> ifAfterRowsError;
      if (tabstatIfAfterRows.has_value() && !tabstatIfAfterRows->empty() &&
          strmutate::needsConditionMask(session, *tabstatIfAfterRows)) {
        ifAfterRowsError = handlers::addIfMaskColumn(session, rowIdxAfterRows,
                                                     *tabstatIfAfterRows, {});
        if (!ifAfterRowsError) tabstatIfAfterRows = rowIdxAfterRows.maskVar;
      }
      if (!byIsGeneric && subParsed.ifExpr.has_value() && !subParsed.ifExpr->empty() &&
          (byIsSummarize || byIsCount || byIsList || tsRegress ||
           strmutate::needsConditionMask(session, *subParsed.ifExpr))) {
        const std::vector<std::string> groupKeys =
            (byIsSummarize || byIsCount || byIsList) ? bp.byVars : std::vector<std::string>{};
        ifError = handlers::addIfMaskColumn(session, rowIdx, *subParsed.ifExpr, groupKeys);
        if (!ifError) subParsed.ifExpr = rowIdx.maskVar;
      }

      // P2-PERF ARC-C C1-④: every read the sub-command makes below is against a
      // per-group SLICE, never against the session's head. Routing those to the
      // resident plane evicts the head and resyncs it once per group; say so
      // once, here, for the whole loop.
      // The generic per-group run reads the session's head itself (by `in`).
      std::optional<SessionState::StatelessRead> statelessGroupReads;
      if (!byIsGeneric) statelessGroupReads.emplace(session);

      // The LAST group's results are what Stata leaves in r() when the loop ends.
      std::optional<ojson> lastSummBlock;
      std::optional<int64_t> lastCountN;
      std::optional<ojson> lastTabstatBlock;
      bool tabstatSawRows = false;  // a group's tabstat has run on rows: `_rc` is 0

      if (byIsGeneric) {
        // Group ranges and headers are read before the first run: a group's
        // command may replace the session's frame (e(sample) after pca), which
        // the raw column views above point into.
        std::vector<std::pair<int64_t, int64_t>> groupRanges;
        std::vector<std::string> groupHeaders;
        for (int64_t a = 0; a < nRowsSorted;) {
          int64_t b = a + 1;
          while (b < nRowsSorted && rowKeyEqual(a, b)) ++b;
          groupRanges.emplace_back(a, b);
          groupHeaders.push_back("-> " + formatGroupHeader(a));
          a = b;
        }
        for (size_t gi = 0; gi < groupRanges.size(); ++gi) {
          const int64_t g0 = groupRanges[gi].first, g1 = groupRanges[gi].second;
          ojson header;
          header["type"] = "display";
          header["text"] = groupHeaders[gi];
          blocks.push_back(header);
          // `CMD VARLIST in FIRST/LAST [if ...] [weight] [, options]`: the range
          // goes after the command's own words, before its first top-level
          // `if`, weight bracket or option comma.
          const std::string& raw = bp.subRaw;
          size_t cut = raw.size();
          int depth = 0;
          bool inQuote = false;
          for (size_t i = 0; i < raw.size(); ++i) {
            const char ch = raw[i];
            if (ch == '"') inQuote = !inQuote;
            if (inQuote) continue;
            if (ch == '(') ++depth;
            else if (ch == ')') --depth;
            if (depth != 0) continue;
            if (ch == ',' || ch == '[') { cut = i; break; }
            if (ch == 'i' && raw.compare(i, 2, "if") == 0 && i > 0 &&
                std::isspace(static_cast<unsigned char>(raw[i - 1])) &&
                (i + 2 == raw.size() || !cecon::ident::isIdentByte(raw[i + 2]))) {
              cut = i;
              break;
            }
          }
          std::string head = raw.substr(0, cut);
          while (!head.empty() && std::isspace(static_cast<unsigned char>(head.back())))
            head.pop_back();
          const std::string line = head + " in " + std::to_string(g0 + 1) + "/" +
                                   std::to_string(g1) + (cut < raw.size() ? " " : "") +
                                   raw.substr(cut);
          const ojson groupBlocks = dispatch(line, session);
          bool failed = false;
          for (const auto& b : groupBlocks) {
            if (b.is_object() && b.value("type", std::string()) == "echo") continue;
            blocks.push_back(b);
            if (b.is_object() && b.value("type", std::string()) == "error") failed = true;
          }
          if (failed) return true;  // Stata's by stops at the first group that fails
        }
        return true;
      }
      // list: its layout options and the sorted, indexed frame every group reads.
      handlers::ListLayout byListLayout;
      std::unique_ptr<Datastore> byListData;
      std::vector<std::string> byListVars;
      if (byIsList) {
        eval::canonicalizeForCommand(subParsed.opts, "list");
        if (auto err = eval::rejectUnknownOptions(subParsed.opts, kVocabList.known,
                                                  kVocabList.unimplemented)) {
          blocks.push_back(ojson(*err));
          return true;
        }
        if (auto bad = handlers::parseListLayout(subParsed.opts, subParsed.raw, &byListLayout)) {
          blocks.push_back(*bad);
          return true;
        }
        byListData = std::make_unique<Datastore>(rowIdx.path);
        if (!subParsed.varlist.empty()) {
          std::string spec;
          for (const auto& t : subParsed.varlist) spec += (spec.empty() ? "" : " ") + t;
          auto lvr = piet::loops::expandVarlist(spec, session);
          if (lvr.isError) {
            blocks.push_back(lvr.error);
            return true;
          }
          byListVars = lvr.tokens;
        } else if (Datastore* cur = session.datastore()) {
          byListVars = cur->names();
        }
      }

      int64_t g0 = 0;
      while (g0 < nRowsSorted) {
        int64_t g1 = g0 + 1;
        while (g1 < nRowsSorted && rowKeyEqual(g0, g1)) ++g1;

        ojson header;
        header["type"] = "display";
        header["text"] = "-> " + formatGroupHeader(g0);
        blocks.push_back(header);
        if (ifError) {
          blocks.push_back(*ifError);
          return true;
        }
        if (byIsList) {
          const double* mask =
              rowIdx.maskVar.empty() ? nullptr : byListData->col(rowIdx.maskVar).f64();
          std::vector<int64_t> rows, obs;
          for (int64_t r = g0; r < g1; ++r) {
            if (mask != nullptr && !(mask[r] == 1.0)) continue;
            rows.push_back(r);
            obs.push_back(byListLayout.absolute ? r + 1 : r - g0 + 1);
          }
          if (!rows.empty()) {
            ojson err;
            ojson listing = handlers::buildListing(session, *byListData, rows, obs, byListVars,
                                                   byListLayout, &err);
            if (listing.is_null()) {
              blocks.push_back(err);
              return true;
            }
            listing["omitted"] = 0;
            ojson b;
            b["type"] = "browse";
            b["editable"] = false;
            b["columns"] = ojson::array();
            b["rows"] = ojson::array();
            b["listing"] = std::move(listing);
            blocks.push_back(std::move(b));
          }
          g0 = g1;
          continue;
        }

        // Slice this group's own contiguous [g0+1, g1] row range (1-based,
        // inclusive) out of the sorted copy — reusing BS-090's shared
        // row-filter infra (piet::handlers::applyIfAndInFilter) purely for
        // its mechanical row-range materialization, ANDed with any `if`
        // the sub-command itself carried (`by G: summarize X if COND`
        // restricts EACH group's own sample by COND too, same as real
        // Stata — group membership itself is unaffected since it was
        // already fixed by the sort/boundary-scan above).
        std::string rangeStr = std::to_string(g0 + 1) + "/" + std::to_string(g1);
        if (tsRegress) {
          // BUG-1166: the group's whole slice; subParsed's `if` names the mask
          // column the slice keeps, and runRegress hands it to the engine as
          // --keepif, applied after the operators.
          std::string groupSlice;
          try {
            groupSlice = handlers::sliceKeepingIfMask(session, rowIdx, g0 + 1, g1);
          } catch (const CobraError& ce) {
            blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
            return true;
          }
          blocks.push_back(
              ojson(handlers::runRegress(subParsed, session, subCmdLower, &groupSlice)));
          g0 = g1;
          continue;
        }
        const bool afterRows = tabstatSawRows && tabstatIfAfterRows.has_value();
        if (afterRows && ifAfterRowsError) {
          blocks.push_back(*ifAfterRowsError);
          return true;
        }
        handlers::InRangeFilterResult fr;
        try {
          fr = handlers::applyIfAndInFilter(
              session, sortedPath, afterRows ? tabstatIfAfterRows : subParsed.ifExpr, rangeStr,
              /*complement=*/false, afterRows ? &rowIdxAfterRows : &rowIdx);
        } catch (const CobraError& ce) {
          blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
          return true;
        }
        if (!fr.ok) {
          blocks.push_back(ojson(handlers::inRangeRejectError(fr, rangeStr, "summarize")));
          return true;
        }
        if (byIsSummarize) {
          Datastore groupDs(fr.dataPath);
          std::optional<std::vector<std::string>> vl;
          if (!subParsed.varlist.empty()) vl = subParsed.varlist;
          lastSummBlock = ojson(
              byWeightFlag.empty()
                  ? handlers::summarizeCoreStats(groupDs, vl, detail, std::nullopt)
                  : handlers::summarizeCoreStatsWeighted(groupDs, vl, detail, subParsed.weightVar,
                                                         byWeightFlag.c_str()));
          if (bySummarizeSeparator >= 0 && lastSummBlock->value("type", "") != "error")
            (*lastSummBlock)["separator"] = bySummarizeSeparator;
          blocks.push_back(*lastSummBlock);
        } else if (byIsCount) {
          // The slice already carries this group's rows AND the `if`.
          Datastore groupDs(fr.dataPath);
          lastCountN = groupDs.nRows();
          ojson out;
          out["type"] = "display";
          out["text"] = std::to_string(*lastCountN);
          blocks.push_back(out);
        } else if (byIsTabstat) {
          // The slice IS this group's sample (and already carries the
          // sub-command's own `if`), so pass no ifExpr — re-filtering here
          // would re-apply it against the slice.
          lastTabstatBlock = ojson(
              byWeightFlag.empty()
                  ? handlers::runTabstatOnData(subParsed.varlist, byTabstatStats, session,
                                               fr.dataPath)
                  : handlers::runTabstatOnDataWeighted(subParsed.varlist, byTabstatStats,
                                                       fr.dataPath, byWeightFlag,
                                                       *subParsed.weightVar));
          blocks.push_back(*lastTabstatBlock);
          if (lastTabstatBlock->value("n_obs", int64_t{0}) > 0) tabstatSawRows = true;
        } else {
          // The slice already carries this group's rows AND the sub-command's
          // own `if`, so runRegress must NOT re-filter it — hence the override.
          // Stata leaves the LAST group's fit in e()/`_b[]` when the loop ends,
          // which falls out of calling the ordinary handler once per group.
          blocks.push_back(
              ojson(handlers::runRegress(subParsed, session, subCmdLower, &fr.dataPath)));
        }

        g0 = g1;
      }
      if (lastSummBlock) captureSummarizeR(*lastSummBlock, byMeanonly, detail, session);
      if (lastCountN) session.eval.rValues.set("N", static_cast<double>(*lastCountN));
      // BUG-1520: r() is the last group's tabstat's -- empty, and with `save` its
      // r(StatTotal), as the unprefixed arm posts it (StataBE 19.5, x7.do: `by g:
      // tabstat x [aw=wf], statistics(n mean sd) save` leaves only r(StatTotal),
      // 3x1, rows N Mean SD: 4, 7.1538461538461542, 5.2171296528451023).
      if (tabstatSawRows) session.eval.locals.set("_rc", "0");
      if (lastTabstatBlock) {
        eval::replaceRClass(session.eval);
        std::vector<std::string> posted;
        const ojson& tb = *lastTabstatBlock;
        if (byTabstatSave && tb.value("type", std::string()) != "error" && tb.contains("total") &&
            tb["total"].is_object()) {
          std::vector<std::string> colVars;
          if (tb.contains("vars") && tb["vars"].is_array())
            for (const auto& vn : tb["vars"])
              if (vn.is_string()) colVars.push_back(vn.get<std::string>());
          session.eval.rMatrices.emplace_back(
              "StatTotal", tabstatStatMatrix(tb["total"], byTsKeys, byTsRows, colVars));
          posted.push_back("StatTotal");
        }
        eval::declareRPosted(session.eval, "tabstat", std::move(posted));
      }
      return true;
    }

    // BUG-041: any OTHER subcommand under `by:`/`bysort:` used to sort-then-
    // dispatch-once — a Gate-(1) silent-drop (the sub-command runs ONCE
    // over the whole, optionally re-sorted, dataset, not once per by-group,
    // so e.g. `bysort grp: summarize wage` quietly answered the SAME
    // question as unqualified `summarize wage` instead of producing 5
    // per-group tables). True by-group dispatch (genuinely repeating the
    // sub-command once per distinct by-value and stitching results
    // together) is real feature work, queued for unfreeze — this
    // stabilization fix is the other honest half of Gate (1): raise an
    // explicit error naming the qualifier instead of silently flattening
    // it. Only gen/replace/egen/summarize (handled above — summarize added
    // BS-092, BETA Phase G) have a genuine native by-group implementation
    // today; every other command reaching here falls back to this honest
    // rejection, matching the qualifier-matrix sweep's expectation
    // (REJECTED, not SILENT_DROP) across the board rather than an ad hoc
    // per-command allowlist.
    {
      std::string prefixWord = bp.sortFirst ? "bysort" : "by";
      std::string byVarsJoined = joinSpace(bp.byVars);
      // A3 (BETA campaign, BS-096): distinguish two structurally different
      // reasons a subcommand lands in this fallback — audit G3 finding #10
      // (SCOPE.md BS-096) confirmed today's single message ("not supported
      // yet") is actively misleading for 4 stems (codebook/describe/
      // levelsof/table) that real Stata itself refuses to combine with
      // by:/bysort: at all (CONFIRMED live: StataBE answers "may not be
      // combined with by" / "prefix by: not allowed", r(190)) — those will
      // NEVER be supported here no matter how much by-group dispatch work
      // lands, unlike the other 7 Stata-legal-but-not-yet-implemented stems
      // (correlate/pwcorr, sdtest, ttest, tabstat, tabulate family, count,
      // list — queued at BS-107; summarize graduated out of this list this
      // phase, BS-092). registry's byIllegalInStata field carries that
      // distinction (a lookup, not a second hardcoded list); anything NOT
      // found in the registry (e.g. a genuinely unrecognized token) or
      // found-but-not-flagged keeps the ORIGINAL "not supported yet"
      // wording unchanged.
      const registry::CommandSpec* subSpec = registry::findByName(subCmdLower);
      // The gtools fold above maps e.g. greshape onto reshape; the flag is
      // read on the typed (pre-fold) row too, so a gtools row carries its own
      // measured verdict.
      const registry::CommandSpec* typedSpec =
          registry::findByName(registry::resolveCanonical(subCmdTyped));
      std::string msg;
      // BUG-105/WS-K: the LOOKUP uses the canonical spelling (so an
      // abbreviation gets the right verdict), the MESSAGE quotes what the user
      // typed (so today's bytes are unchanged for every non-abbreviated line).
      // Stata's answer for a non-byable command is the command's own
      // "<cmd> may not be combined with by", r(190), whatever the sort order
      // (vs-stata bank ab684d8dd4e3db69 `by foreign: codebook`,
      // 6df7b0c9ad98ab0b `bysort foreign: glevelsof`, b7971446 esttab, b49d7ab0bf968d9e `bysort foreign:
      // scatter`, 9e1e81f9bba79933 `hist` -- the program is named as typed); table's is its
      // Stata 17 note (8c5b6946 `bysort foreign: table rep78`); c4e0a1066a17b3d4 `bysort
      // foreign: margins`, e7fcf06457bc321e `... mean` (s4_post); 92020bf71d0804da compress,
      // 29f1e114ea34c258 format, 4006519450d75a6b label, 386ab1fe9e41d315 notes (s4_io_misc).
      if ((subSpec && subSpec->byIllegalInStata) || (typedSpec && typedSpec->byIllegalInStata)) {
        const std::string stataMsg =
            subCmdLower == "table"
                ? std::string("prefix by: not allowed since Stata 17; see help table for updated syntax")
                : subCmdTyped + " may not be combined with by";
        blocks.push_back(ojson(eval::errorBlock("syntax_error", stataMsg, std::nullopt,
                                                std::nullopt, std::nullopt, "r(190)")));
        return true;
      } else {
        // A plain `by:` over data not sorted by its keys is r(5) "not sorted"
        // in Stata before the command runs at all (vs-stata bank
        // 8ad2062fa3e4bdd1 `by race: svyset ...` on unsorted data), so that
        // answer does not depend on PIET's by-group support.
        if (!bp.sortFirst && subSpec != nullptr) {
          std::vector<std::string> keys = bp.byVars;
          keys.insert(keys.end(), bp.sortVars.begin(), bp.sortVars.end());
          if (Datastore* ds = session.datastore()) {
            const std::vector<std::string> claim = engineSortClaim(*ds);
            const bool inOrder = claim.empty() ? keysAlreadyGrouped(*ds, keys, true)
                                               : keysLeadClaim(keys, claim);
            if (!inOrder) {
              blocks.push_back(ojson(eval::errorBlock(
                  "not_sorted", "not sorted — `by " + joinSpace(bp.byVars) +
                      ":` requires the data to already be sorted by " + joinSpace(keys) + ".",
                  std::nullopt, std::nullopt, std::nullopt, "r(5)")));
              return true;
            }
          }
        }
        msg = "`" + prefixWord + " " + byVarsJoined + ": " + subCmdTyped +
              "` is not supported yet — `by:`/`bysort:` grouped dispatch is only "
              "implemented for `gen`/`replace`/`egen`/`summarize`/`count`/`regress`/`tabstat`/`keep if`/`drop if` "
              "(and `carryforward`/`tsspell`) in this version of PIET. Run `" +
              subCmdTyped + "` separately for each group instead.";
      }
      // Stata's own refusal is r(190) "<cmd> may not be combined with by" (bank: every
      // `bysort foreign: pctile|xtile|recode|merge|reshape|...` cell, d282ade8b76a830c).
      if (subSpec && subSpec->byIllegalInStata) {
        blocks.push_back(ojson(eval::errorBlock(
            "by_not_allowed", subCmdTyped + " may not be combined with by\n" + msg,
            std::nullopt, std::nullopt, std::nullopt, "r(190)")));
        return true;
      }
      blocks.push_back(ojson(eval::errorBlock("unsupported_command", msg, std::nullopt, std::nullopt,
                                              std::nullopt, "piet_gap")));
      return true;
    }
  }
  return false;
}

}
