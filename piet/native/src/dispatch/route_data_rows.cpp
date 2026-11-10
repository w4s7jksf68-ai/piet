// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "data_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {
bool route_data_rows(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    const std::string& rawCmd) {


  // BUG-1417: drop and keep are r-class. A drop or keep that ran posts
  // r(k_drop), the variables it removed, or for `if`/`in` r(N_drop), the
  // observations, and replaces all of r(); a failed one leaves r() alone
  // (StataBE 19.5: `drop pv` r(k_drop) 1 and r(sum) `.`; `drop if _n > 38`
  // r(N_drop) 2; `keep` posts r(k_drop) too).
  // The counts come off the resident card and the verb's own result block
  // (n_rows_after, varlist_after) before any datastore() open: a filter must
  // not materialize the head to learn one integer (INSPECT-2608).
  const auto dropKeepCount = [&session](bool rows) -> int64_t {
    if (session.tempDropDepth > 0) return -1;
    try {
      if (rows) {
        if (std::optional<int64_t> n = session.residentRowsIfCurrent()) return *n;
      } else if (const std::vector<VarInfo>* card = session.residentReadCard()) {
        return std::count_if(card->begin(), card->end(),
                             [](const VarInfo& v) { return !isHiddenColumn(v.name); });
      }
      Datastore* ds = session.datastore();
      if (ds == nullptr) return -1;
      if (rows) return ds->nRows();
      const std::vector<std::string> names = ds->names();
      return std::count_if(names.begin(), names.end(),
                           [](const std::string& n) { return !isHiddenColumn(n); });
    } catch (const std::exception&) {
      return -1;
    }
  };

  const auto postDropKeepR = [&session, &dropKeepCount, &cmd](const ojson& blk, bool rows,
                                                         int64_t before) {
    if (session.tempDropDepth > 0) return;
    if (before < 0 || !blk.is_object() || blk.value("type", std::string()) == "error") return;
    int64_t after = -1;
    if (rows && blk.contains("n_rows_after") && blk["n_rows_after"].is_number_integer()) {
      after = blk["n_rows_after"].get<int64_t>();
    } else if (!rows && blk.contains("varlist_after") && blk["varlist_after"].is_array()) {
      after = 0;
      for (const auto& v : blk["varlist_after"]) {
        const std::string nm = v.is_object() ? v.value("name", std::string())
                                             : (v.is_string() ? v.get<std::string>() : "");
        if (!isHiddenColumn(nm)) ++after;
      }
    } else {
      after = dropKeepCount(rows);
    }
    if (after < 0) return;
    const std::string key = rows ? "N_drop" : "k_drop";
    eval::replaceRClass(session.eval);
    session.eval.rValues.set(key, static_cast<double>(before - after));
    eval::declareRPosted(session.eval, cmd, {key});
  };


  if (cmd == "drop") {
    // BS-097: `drop varlist` / `drop if COND` never had comma-options to
    // begin with (neither branch below reads parsed.opts) — any leftover
    // key is unconditionally unconsumed, so the known-set is the empty set.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    if (parsed.ifExpr.has_value() || parsed.inRange.has_value()) {
      // BUG-032: a string-referencing condition can't go to cobra's all-f64
      // eval --keepif; filter row-wise. Numeric stays cobra. BS-090 (BETA
      // Phase G): a co-occurring `in` is ANDed into the SAME row-wise mask
      // when the string detour fires (strmutate::filterRowsIf), and
      // composed via piet::handlers::applyIfAndInFilter otherwise.
      const int64_t rowsBefore = dropKeepCount(true);
      ojson res = strmutate::needsStringCount(session, parsed.ifExpr)
                      ? ojson(strmutate::filterRowsIf(session, *parsed.ifExpr, /*keep=*/false,
                                                      parsed.inRange))
                      : ojson(handlers::runDropIf(parsed.ifExpr.value_or(""), session,
                                                  parsed.inRange));
      postDropKeepR(res, true, rowsBefore);
      blocks.push_back(std::move(res));
      return true;
    }
    // Variable-name abbreviation ([U] 11.2): a unique prefix stands for its
    // variable, several are r(111) "ambiguous abbreviation" -- the rule the by
    // keys already use (resolveByKeyNames). StataBE ab.do: `keep
    // number_people_clicked` keeps number_people_clicked_w3 (007's Wave 3).
    {
      std::vector<std::string> names = parsed.varlist;
      if (auto err = handlers::resolveByKeyNames(session, names)) {
        blocks.push_back(*err);
        return true;
      }
      parsed.varlist = names;
    }
    const int64_t varsBefore = dropKeepCount(false);
    ojson dropped = ojson(handlers::runDrop(parsed.varlist, session));
    // BUG-785: dropping EVERY variable (`drop _all`, or a varlist naming them
    // all) leaves Stata with no observations either -- `drop _all` / `set obs 5`
    // reads "Number of observations (_N) was 0, now 5" (w2_esample.log:1205
    // E11). The engine's column drop kept the row count, so `set obs 5` after
    // it was "cannot reduce the number of observations from 61 to 5", an
    // uncaptured stop (w2_esample.do:403, the replay's first stop). The rows go
    // too, but the dataset stays loaded and EMPTY, not unloaded: Stata still
    // runs `generate` on it (selftest BUG-291 `gen double x` / `drop x` / `gen
    // x = 1` answers float, WSN 291a). A hidden column (the e(sample) mask)
    // keeps its name over zero rows, and rows `set obs` adds later hold missing
    // -- outside the sample, as Stata reads them (E11 counts 0).
    if (dropped.is_object() && dropped.value("type", std::string()) == "data_mutation") {
      bool noVarsLeft = false;
      if (dropped.contains("varlist_after") && dropped["varlist_after"].is_array()) {
        noVarsLeft = dropped["varlist_after"].empty();
      } else {
        try {
          Datastore* dds = session.datastore();
          noVarsLeft = dds != nullptr && dds->names().empty();
        } catch (const std::exception&) {
          noVarsLeft = false;  // unreadable head: leave it to the next verb's honest error
        }
      }
      if (noVarsLeft && session.currentHbin().has_value()) {
        const std::string emptied = mintScratchPath(session.workdir(), "drop_all_rows");
        try {
          runCobraJson({"eval", "--data", *session.currentHbin(), "--keepif", "0", "--out",
                        emptied});
          session.load(emptied);
          if (dropped.contains("n_rows_after")) dropped["n_rows_after"] = 0;
        } catch (const CobraError& ce) {
          blocks.push_back(dropped);
          blocks.push_back(ojson(eval::engineError(
              "drop: every variable was dropped, but the observations could not be removed (" +
              std::string(ce.what()) + ")")));
          return true;
        }
      }
    }
    postDropKeepR(dropped, false, varsBefore);
    blocks.push_back(dropped);
    return true;
  }


  if (cmd == "keep") {
    // BS-097: same zero-consumption argument as `drop` immediately above.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    if (parsed.ifExpr.has_value() || parsed.inRange.has_value()) {
      const int64_t rowsBefore = dropKeepCount(true);
      ojson res = strmutate::needsStringCount(session, parsed.ifExpr)
                      ? ojson(strmutate::filterRowsIf(session, *parsed.ifExpr, /*keep=*/true,
                                                      parsed.inRange))
                      : ojson(handlers::runKeepIf(parsed.ifExpr.value_or(""), session,
                                                  parsed.inRange));
      postDropKeepR(res, true, rowsBefore);
      blocks.push_back(std::move(res));
      return true;
    }
    {
      // Abbreviations, as for `drop` just above.
      std::vector<std::string> names = parsed.varlist;
      if (auto err = handlers::resolveByKeyNames(session, names)) {
        blocks.push_back(*err);
        return true;
      }
      parsed.varlist = names;
    }
    const int64_t varsBefore = dropKeepCount(false);
    ojson kept = ojson(handlers::runKeep(parsed.varlist, session));
    postDropKeepR(kept, false, varsBefore);
    blocks.push_back(std::move(kept));
    return true;
  }


  if (cmd == "sort") {
    // BUG-336 (batch 2): known set measured from the handler body -- runSort takes no options.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSort.known, kVocabSort.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    blocks.push_back(ojson(handlers::runSort(parsed.varlist, session)));
    return true;
  }


  if (cmd == "gsort") {
    // BUG-336 (batch 2): known set measured from the handler body -- runGsort takes no options.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabGsort.known, kVocabGsort.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    blocks.push_back(ojson(handlers::runGsort(parsed.varlist, session)));
    return true;
  }


  if (cmd == "order") {
    blocks.push_back(ojson(handlers::runOrder(parsed.varlist, parsed.opts, session)));
    return true;
  }


  if (cmd == "recode") {
    // BUG-336 (batch 2): generate is KNOWN (the debug_all/pangram stanzas use
    // `recode ..., gen(edu3)`). runRecode re-tokenizes the raw line and reads
    // generate()/into()/label() from its canonicalized options, so `g()' (the
    // vocabulary's declared alias) reaches it as generate.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRecode.known, kVocabRecode.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // BS-031(b): parsed.ifExpr was already correctly extracted by the
    // tokenizer but never forwarded — see runRecode's own comment.
    // Under `test` the handler returns the mutation and test's notes.
    ojson r = handlers::runRecode(parsed.raw, session, parsed.ifExpr);
    if (r.is_array()) {
      for (const auto& b : r) blocks.push_back(b);
    } else {
      blocks.push_back(r);
    }
    return true;
  }


  if (cmd == "collapse") {
    const int64_t collapseNBefore = rowsBeforeForRN(session);
    // Step-1 D3: `parsed.weightVar` only survives the tokenizer's guard
    // here when weightType is fw/fweight (collapse is on the fweight
    // allow-list) — any other weight type was already rejected upstream.
    // The flavor the user typed: gcollapse and fcollapse label, format and type
    // their results by their own rules (collapseResultMeta).
    const std::string collapseFlavor =
        (cmdTyped == "gcollapse" || cmdTyped == "fcollapse") ? cmdTyped : std::string("collapse");
    // Each flavor's own option net; an unknown word is r(198) (cz_c_err_opt).
    // collapse_17: by() cw fast clabel frame(). ftools' fcollapse: by() cw fast
    // nocompress append smart verbose here (its alias row refused the rest by
    // name), and `compress`, noCOMPRESS's positive spelling, which asks for the
    // default. gcollapse: what its alias row lets through. clabel and frame()
    // are collapse's alone (BUG-1100: `fcollapse ..., clabel` and `gcollapse
    // ..., clabel` are `option clabel not allowed` r(198) in StataBE 19.5).
    // verbose is read by nothing here; it reaches the net so that the flag
    // check below sees it.
    static const std::unordered_set<std::string> kFcollapseArmOpts = {
        "by", "cw", "fast", "nocompress", "compress", "append", "smart", "verbose"};
    static const std::unordered_set<std::string> kGcollapseArmOpts = {"by", "cw", "fast",
                                                                      "verbose"};
    const std::unordered_set<std::string>& collapseArmOpts =
        collapseFlavor == "fcollapse"   ? kFcollapseArmOpts
        : collapseFlavor == "gcollapse" ? kGcollapseArmOpts
                                        : kVocabCollapse.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, collapseArmOpts,
                                              kVocabCollapse.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // frame(name): syntax's `name` check, then nothing -- collapse_17 never reads
    // it (StataBE 19.5: frame(fz) collapses in memory and leaves no frame fz;
    // frame(1x) is `1x invalid name` r(198), frame(a b) `option frame():  too
    // many names specified` r(103), frame() rc 0). A bare frame or a bare by, or
    // a flag given an argument (clabel(x), append(x), smart(1), fast(1),
    // verbose(1)), is `option ... incorrectly specified` r(198) in all three
    // flavors; by() is no by() at all (rc 0, one observation).
    for (const auto& [k, v] : parsed.opts) {
      if ((k == "frame" || k == "by") == v.isBool) {
        // The option as typed: an abbreviation reached here under its full key,
        // and the alias row rebuilt parsed.raw from the full keys (StataBE 19.5,
        // ftools 2.49.0: fcollapse `v(1)` is `option v() incorrectly specified`).
        std::string typed = k;
        const long long comma = eval::findUnquotedComma(rawCmd);
        if (!v.isBool && comma >= 0) {
          static const std::regex reCalled(R"(([A-Za-z_][A-Za-z0-9_]*)\s*\()");
          const std::string tail = rawCmd.substr(static_cast<size_t>(comma));
          for (std::sregex_iterator it(tail.begin(), tail.end(), reCalled), end; it != end; ++it) {
            const std::string w = (*it)[1].str();
            if (k.compare(0, w.size(), strutil::lower(w)) == 0) {
              typed = w;
              break;
            }
          }
        }
        blocks.push_back(ojson(eval::errorBlock(
            "syntax_error", "option " + typed + (v.isBool ? "" : "()") + " incorrectly specified",
            std::nullopt, std::nullopt, std::nullopt, std::string("r(198)"))));
        return true;
      }
      if (k != "frame") continue;
      std::istringstream names(v.strValue);
      std::string first, more;
      names >> first >> more;
      if (!more.empty()) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                                "option frame():  too many names specified",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                std::string("r(103)"))));
        return true;
      }
      if (!first.empty() && !strutil::isValidStataIdentifier(first)) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", first + " invalid name",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                std::string("r(198)"))));
        return true;
      }
    }
    // noCOMPRESS's two spellings together: StataBE 19.5 `fcollapse ..., compress
    // nocompress` (either order) is `option nocompress not allowed` r(198).
    {
      bool compress = false, nocompress = false;
      for (const auto& kv : parsed.opts) {
        compress = compress || kv.first == "compress";
        nocompress = nocompress || kv.first == "nocompress";
      }
      if (compress && nocompress) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "option nocompress not allowed",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                std::string("r(198)"))));
        return true;
      }
    }
    ojson collapseBlk = ojson(handlers::runCollapse(parsed.raw, parsed.ifExpr, session,
                                                    parsed.opts, parsed.weightVar,
                                                    parsed.inRange, collapseFlavor,
                                                    parsed.weightType));
    if (collapseBlk.value("type", std::string()) != "error" && collapseNBefore >= 0)
      session.eval.rValues.set("N", static_cast<double>(collapseNBefore));
    // e(sample) T2: runCollapse wrote collapse's marker rule (first observation
    // of each by-group, StataBE capture w2_esample_copies); the gtools/ftools
    // spellings reach the same engine verb, but their own rule is unmeasured.
    if (collapseBlk.value("type", std::string()) != "error" &&
        (cmdTyped == "gcollapse" || cmdTyped == "fcollapse") &&
        !handlers::esampleMarkHeadLost(session, EsampleLostBy::kCollapseShim)) {
      collapseBlk = ojson(eval::engineError(
          cmdTyped + ": the data were collapsed, but the estimation-sample marker could not be "
                     "withdrawn from the result; e(sample) is not reliable until the next "
                     "estimation."));
    }
    blocks.push_back(std::move(collapseBlk));
    return true;
  }


  if (cmd == "expand") {
    // AER scope-2 W2: `generate(newvar)` is a real Stata expand option this
    // build does not have, so it now says so instead of "not allowed".
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabExpand.known,
                                              kVocabExpand.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    std::optional<std::string> expandGen;
    for (const auto& [k, v] : parsed.opts)
      if (k == "generate" && !v.isBool) expandGen = strutil::trim(v.strValue);
    // The count is the rest of the line after the verb (a bare number here).
    static const std::regex reExpand(R"(^\s*expand\s+(.+)$)", std::regex::icase);
    std::smatch m;
    std::string arg;
    if (std::regex_search(parsed.raw, m, reExpand)) arg = trimWs(m.str(1));
    // B4-W3b: the regex above takes the whole rest of the line, so with an
    // option present it captured "2, generate(dupflag)" and runExpand rejected
    // it as a per-observation expression. The count is what precedes the option
    // comma.
    {
      size_t comma = arg.find(',');
      if (comma != std::string::npos) arg = trimWs(arg.substr(0, comma));
    }
    // e(sample) T2 (w2_esample.log:1076 E6 `expand 2 if id==3`, rc 0): the
    // qualifiers are the tokenizer's parsed.ifExpr/inRange, so the count is
    // what precedes them (it was refused as a per-observation expression, rc 1).
    if (parsed.ifExpr.has_value() || parsed.inRange.has_value()) {
      static const std::regex reExpandQual(R"(\s+(?:if|in)\s.*$)", std::regex::icase);
      arg = trimWs(std::regex_replace(arg, reExpandQual, ""));
    }
    {
      // The constant form is the only one this build runs (runExpand says so);
      // a non-numeric argument is modelled as x2 for the pre-flight only.
      char* end = nullptr;
      const long long k = arg.empty() ? 0 : std::strtoll(arg.c_str(), &end, 10);
      const int64_t factor = (end && *end == '\0' && k > 0) ? static_cast<int64_t>(k) : 2;
      if (preflight::gate("expand", session, "", factor, blocks) == preflight::Verdict::Cancel) return true;
    }
    blocks.push_back(
        ojson(handlers::runExpandIf(arg, expandGen, parsed.ifExpr, parsed.inRange, session)));
    return true;
  }


  if (cmd == "fillin") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // fillin leaves r() alone (BUG-1451, StataBE: the earlier summarize's r(N) 28
    // and r(mean) survive it on 30 rows).
    blocks.push_back(ojson(handlers::runFillin(parsed.varlist, session)));
    return true;
  }


  if (cmd == "contract") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // contract leaves r() alone too (BUG-1451, measured as for fillin).
    blocks.push_back(ojson(handlers::runContract(parsed.varlist, session)));
    return true;
  }


  if (cmd == "cross") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    static const std::regex reCross(R"(^\s*cross\s+using\s+(.+)$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(parsed.raw, m, reCross)) {
      blocks.push_back(ojson(eval::engineError(
          "cross: missing `using FILE` clause. Usage: cross using \"file\"")));
      return true;
    }
    if (preflight::gate("cross", session, trimWs(m.str(1)), 0, blocks) == preflight::Verdict::Cancel) return true;
    blocks.push_back(ojson(handlers::runCross(trimWs(m.str(1)), session)));
    return true;
  }


  if (cmd == "winsor") {
    blocks.push_back(ojson(handlers::runWinsorCox(parsed, session)));
    return true;
  }


  if (cmd == "isid") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabIsid.known,
                                              kVocabIsid.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    bool missok = false, sort = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "missok" && v.isBool && v.boolValue) missok = true;
      if (k == "sort" && v.isBool && v.boolValue) sort = true;
    }
    // Success is SILENT (Stata prints nothing) — the handler returns an empty
    // array, which must not be pushed as a block (same shape as preserve/
    // restore above); under `sort` it returns the sort and its note.
    ojson r = handlers::runIsid(parsed.varlist, session, missok, sort);
    if (r.is_array()) {
      for (const auto& b : r) blocks.push_back(b);
    } else {
      blocks.push_back(r);
    }
    return true;
  }


  if (cmd == "datasignature") {
    // BUG-1461: set/confirm/clear keep and check the signature (runDatasignatureSub);
    // report and the `using`/saving() file forms are named gaps, and any other
    // word is Stata's r(198) "invalid datasignature subcommand".
    std::string dsSub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
    // confirm abbreviates to conf; set and clear do not abbreviate (StataBE 19.5).
    if (dsSub.size() >= 4 && std::string("confirm").rfind(dsSub, 0) == 0) dsSub = "confirm";
    if (!dsSub.empty()) {
      const bool usingForm =
          std::any_of(parsed.varlist.begin(), parsed.varlist.end(),
                      [](const std::string& t) { return lower(t) == "using"; });
      if (dsSub == "report" || usingForm) {
        blocks.push_back(ojson(eval::unsupportedCommand(
            "datasignature " + dsSub + (usingForm ? " using" : ""), std::nullopt, std::nullopt,
            "piet_gap", eval::UnsupportedCommandKind::kUnknownSubcommand)));
        return true;
      }
      if (dsSub != "set" && dsSub != "confirm" && dsSub != "clear") {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "invalid datasignature subcommand",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(198)")));
        return true;
      }
      if (dsSub == "clear" && !parsed.opts.empty()) {  // StataBE 19.5: r(101), not 198
        blocks.push_back(ojson(eval::errorBlock("engine_error", "options not allowed", std::nullopt,
                                                std::nullopt, std::nullopt, "r(101)")));
        return true;
      }
      std::unordered_set<std::string> known;
      if (dsSub == "set") known = {"reset"};
      if (dsSub == "confirm") known = {"strict"};
      if (auto err = eval::rejectUnknownOptions(parsed.opts, known,
                                                dsSub == "set" ? std::unordered_set<std::string>{"saving"}
                                                               : std::unordered_set<std::string>{})) {
        blocks.push_back(ojson(*err));
        return true;
      }
      if (dsSub != "clear") session.eval.rValues = eval::OrderedFloatDict();
      if (dsSub != "clear") session.eval.rStrings.clear();
      blocks.push_back(ojson(handlers::runDatasignatureSub(dsSub, parsed.opts, session)));
      return true;
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // BUG-373: the signature itself is the whole point of the command, and it
    // was printed and not published — a do-file that compares signatures across
    // a run could not read it back.
    ojson dsBlk = ojson(handlers::runDatasignature(session));
    if (dsBlk.value("type", std::string()) == "display") {
      session.eval.rStrings.set("datasignature", trimWs(dsBlk.value("text", std::string())));
      dsBlk["text"] = "  " + dsBlk.value("text", std::string());  // indented, as Stata prints it
    }
    blocks.push_back(std::move(dsBlk));
    return true;
  }


  if (cmd == "preserve") {
    ojson r = handlers::runPreserve(session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return true;
  }


  if (cmd == "restore") {
    // B5-D14 (BUG-454): this arm used to call NO reject net at all, so ANY
    // option was silently accepted and dropped -- adding the net here is
    // itself a behavior change (an unknown option on `restore` is now an
    // honest r(198) reject instead of rc 0).
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRestore.known,
                                              kVocabRestore.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    bool rearm = false;
    bool cancel = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "preserve" && v.isBool && v.boolValue) rearm = true;
      if (k == "not" && v.isBool && v.boolValue) cancel = true;
    }
    // BUG-922: `restore, not` forgets the preserved copy and leaves the data
    // as it is (oracle pc_pr_not: N stays 3; pc_pr_not_then_restore: the next
    // restore is r(622)). With nothing preserved it is r(622) like `restore`.
    // It was refused r(198) (`not` was not in the vocabulary). A level that
    // preserved nothing cannot cancel its caller's copy either: r(622) and the
    // copy stays (oracle w2ad_prog_preserve_nest pn_d_not_caller,
    // pn_p_not_caller: in1 622:4:30, then the caller's restore brings it back;
    // pn_top_none_not: 622).
    if (cancel) {
      if (rearm) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                                "restore: options not and preserve may not be combined",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                std::string("r(198)"))));
        return true;
      }
      if (!session.ownPreserveFrame()) {
        blocks.push_back(ojson(eval::errorBlock(
            "engine_error", "restore: no previous preserve — use `preserve` before `restore`",
            std::nullopt, std::nullopt, std::nullopt, std::string("r(622)"))));
        return true;
      }
      session.discardTopPreserve();
      return true;
    }
    ojson r = handlers::runRestore(session, rearm);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return true;
  }


  // ------------------------------------------------------------------
  // M4a: output-completion surface (ROADMAP M4a item 1 — data-summary output)

  if (cmd == "winsor2") {
    double cutLo = 1.0, cutHi = 99.0;
    std::string suffix = "_w";
    std::string byStr;
    bool trim = false, replace = false, suffixGiven = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "cuts" && !v.isBool) {
        std::istringstream iss(v.strValue);
        std::vector<std::string> parts;
        std::string tok;
        while (iss >> tok) parts.push_back(tok);
        if (parts.size() == 2) {
          // BUG-112(4): strict-consume each number (mirrors the engine-side collapse
          // pNN fix @9f86577 — C1-F11 literal discipline) — `cuts(5x 95)` must reject,
          // not silently read "5x" as 5.
          size_t consumedLo = 0, consumedHi = 0;
          bool ok = true;
          try {
            cutLo = std::stod(parts[0], &consumedLo);
            cutHi = std::stod(parts[1], &consumedHi);
          } catch (...) {
            ok = false;
          }
          if (!ok || consumedLo != parts[0].size() || consumedHi != parts[1].size()) {
            blocks.push_back(ojson(eval::engineError(
                "winsor2: cuts(" + v.strValue + ") — expected two numbers")));
            return true;
          }
        } else {
          blocks.push_back(ojson(eval::engineError(
              "winsor2: cuts() requires exactly two values, got '" + v.strValue + "'")));
          return true;
        }
      } else if (k == "suffix" && !v.isBool) {
        suffix = v.strValue;
        suffixGiven = true;
      } else if (k == "by" && !v.isBool) {
        byStr = v.strValue;
      } else if (k == "trim" && v.isBool && v.boolValue) {
        trim = true;
      } else if (k == "replace" && v.isBool && v.boolValue) {
        replace = true;
      }
    }
    // winsor2.ado: with no suffix() the new variable is <var>_w, or <var>_tr
    // under `trim` (007's Wave 2 cleaning labels expectcomm_ehai_amount_tr).
    if (!suffixGiven && trim) suffix = "_tr";
    // BS-097: cuts/suffix/by/trim/replace are the only winsor2 options this
    // arm reads (community-package command; real winsor2 also has `label`).
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kWinsor2KnownOpts = kVocabWinsor2.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kWinsor2KnownOpts)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError("winsor2: no variables specified")));
      return true;
    }
    // BUG-373: winsor2 publishes r(r1)/r(r2) — the two CUT VALUES it winsorised
    // at, i.e. the cutLo/cutHi percentiles of the (last) variable. Verified
    // against the live StataBE capture on beertax: r(r1) 0.04766949 and r(r2)
    // 2.41525412, which are exactly its p1 and p99. Computed here, before the
    // mutation, because afterwards the column has already been clipped to them.
    std::vector<double> w2cuts;
    {
      Datastore* w2ds = session.datastore();
      if (w2ds != nullptr && w2ds->has(parsed.varlist.back()) &&
          w2ds->dtype(parsed.varlist.back()) == "f64") {
        const double* col = w2ds->col(parsed.varlist.back()).f64();
        const int64_t wn = w2ds->nRows();
        std::vector<double> v;
        v.reserve(static_cast<size_t>(wn));
        for (int64_t i = 0; i < wn; ++i)
          if (!std::isnan(col[i])) v.push_back(col[i]);
        std::sort(v.begin(), v.end());
        // Stata's percentile rule, the same one Frame::xtile and summarize,detail
        // use: an exact rank boundary averages the two straddling values.
        auto pct = [&v](double p) {
          const int64_t n = static_cast<int64_t>(v.size());
          if (n == 0) return std::numeric_limits<double>::quiet_NaN();
          if (n == 1) return v[0];
          const double Pi = static_cast<double>(n) * p / 100.0;
          const double fl = std::floor(Pi);
          if (fl == Pi && Pi >= 1 && static_cast<int64_t>(Pi) < n)
            return (v[static_cast<size_t>(Pi) - 1] + v[static_cast<size_t>(Pi)]) / 2.0;
          int64_t idx = static_cast<int64_t>(fl) + 1;
          if (idx > n) idx = n;
          return v[static_cast<size_t>(idx) - 1];
        };
        if (!v.empty()) { w2cuts.push_back(pct(cutLo)); w2cuts.push_back(pct(cutHi)); }
      }
    }
    ojson w2Blk = ojson(handlers::runWinsor2(session, parsed.varlist, cutLo, cutHi, trim, byStr,
                                             suffix, replace));
    if (w2Blk.value("type", std::string()) != "error" && w2cuts.size() == 2) {
      session.eval.rValues.set("r1", w2cuts[0]);
      session.eval.rValues.set("r2", w2cuts[1]);
    }
    blocks.push_back(std::move(w2Blk));
    return true;
  }


  if (cmd == "tsfill") {
    blocks.push_back(ojson(handlers::runTsfill(parsed, session)));
    return true;
  }


  if (cmd == "mkspline") {
    // Real Stata's `mkspline stub = varname [if], cubic ...` — same
    // reconstruct-then-split-on-`=` idiom as `sdtest`'s arm above / egen's
    // arm earlier in this function.
    std::string raw = joinVarlist(parsed.varlist);
    size_t eqPos = raw.find('=');
    if (eqPos == std::string::npos) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `mkspline` requires `stub = varname`", "syntax_error")));
      return true;
    }
    std::string stub = trimWs(raw.substr(0, eqPos));
    std::string srcVar = trimWs(raw.substr(eqPos + 1));
    blocks.push_back(
        ojson(handlers::runMkspline(stub, srcVar, parsed.opts, parsed.ifExpr, session)));
    return true;
  }


  // BUG-370 — see registry.cpp for why these three exist and what they refuse.
  // All follow the mkspline arm's `stub = var` split (joinVarlist -> find('=')).
  if (cmd == "xtile") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {"nquantiles", "cutpoints"},
                                              {"altdef"})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    std::string raw = joinVarlist(parsed.varlist);
    size_t eqPos = raw.find('=');
    if (eqPos == std::string::npos) {
      // `xtile if = x`: the `if` opens the qualifier, so no name is left at all.
      if (trimWs(raw).empty()) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "varlist required", std::nullopt,
                                                std::nullopt, std::nullopt, "r(100)")));
        return true;
      }
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `xtile` requires `newvar = varname`", "syntax_error")));
      return true;
    }
    // BUG-1015: `exp` as typed, for the variable label: the text after `=` up to
    // the qualifier, the weight or the comma, a blank before it kept (Stata's
    // syntax `=/exp`; sx_xtile_n_if "3 quantiles of y ").
    std::string expText = trimWs(raw.substr(eqPos + 1));
    {
      const std::string& line = parsed.raw;
      const size_t e = line.find('=');
      if (e != std::string::npos) {
        size_t b = e + 1;
        while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
        size_t stop = line.size();
        int depth = 0;
        bool inStr = false;
        for (size_t i = b; i < line.size(); ++i) {
          const char ch = line[i];
          if (ch == '"') inStr = !inStr;
          if (inStr) continue;
          if (ch == '(') ++depth;
          if (ch == ')') --depth;
          if (depth > 0) continue;
          if (ch == ',' || ch == '[') { stop = i; break; }
          if (i > b && std::isspace(static_cast<unsigned char>(line[i - 1])) &&
              (line.compare(i, 2, "if") == 0 || line.compare(i, 2, "in") == 0) &&
              (i + 2 == line.size() || std::isspace(static_cast<unsigned char>(line[i + 2])))) {
            stop = i;
            break;
          }
        }
        expText = line.substr(b, stop - b);
      }
    }
    std::optional<std::string> xtWeight;
    if (parsed.weightType.has_value() && parsed.weightVar.has_value()) xtWeight = parsed.weightVar;
    blocks.push_back(ojson(handlers::runXtileStata(trimWs(raw.substr(0, eqPos)), expText,
                                                   parsed.opts, parsed.ifExpr, parsed.inRange,
                                                   xtWeight, session, parsed.weightType)));
    return true;
  }


  if (cmd == "ipolate") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {"generate", "epolate", "by"}, {})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    blocks.push_back(
        ojson(handlers::runIpolate(parsed.varlist, parsed.opts, parsed.ifExpr, session)));
    return true;
  }


  if (cmd == "tssmooth") {
    // Two-word command, registered under the first word: the sub-verb arrives as
    // varlist[0] (the duplicates/estat convention).
    std::string sub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
    if (sub != "ma") {
      blocks.push_back(ojson(eval::engineError(
          sub.empty()
              ? std::string("syntax_error: `tssmooth` requires a sub-command; this build "
                            "implements `tssmooth ma newvar = varname, window(#l #c #f)`")
              : "`tssmooth " + sub +
                    "` is not implemented in this build (only `ma`; Stata's exponential, "
                    "dexponential, hwinters, shwinters and nl smoothers are not available).",
          sub.empty() ? "syntax_error" : "unimplemented")));
      return true;
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {"window"}, {"replace"})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    std::string raw =
        joinVarlist(std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end()));
    size_t eqPos = raw.find('=');
    if (eqPos == std::string::npos) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `tssmooth ma` requires `newvar = varname`", "syntax_error")));
      return true;
    }
    blocks.push_back(ojson(handlers::runTssmoothMa(trimWs(raw.substr(0, eqPos)),
                                                   trimWs(raw.substr(eqPos + 1)), parsed.opts,
                                                   session)));
    return true;
  }
  return false;
}

}
