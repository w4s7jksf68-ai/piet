// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "data_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "datamgmt/dataset_shape_support.h"
#include "options.h"
#include "handler_declarations.h"
#include "estimation/estimation_internal.h"  // runExpandIf
#include "piet/handlers/data_values.h"
#include "piet/handlers/gtools_aliases.h"
#include "piet/handlers/observations.h"
#include "piet/handlers/session_commands.h"
#include "piet/handlers/time_panel.h"
#include "piet/handlers/variables.h"
namespace piet::dispatch_detail {
namespace {
// Everything r() is made of, so that a command which leaves r() as it found it (gsort holds
// the results and restores them) can put it back after the helper commands it runs.
struct RClassSnapshot {
  eval::OrderedFloatDict values;
  std::vector<std::pair<std::string, std::uint64_t>> hidden, hiddenMacros, hiddenMatrices;
  eval::OrderedDict strings;
  std::vector<std::pair<std::string, eval::EvalMatrix>> matrices;
  std::uint64_t tableGen, tableRefusalGen;
  std::uintptr_t tableEid, tableRefusalEid;
  std::string tableModel, tableRefusal, classCmd, declaredCmd;
  std::vector<std::string> unposted;
  bool classSetUnknown;
  std::optional<std::vector<std::string>> declaredNames;
  explicit RClassSnapshot(const eval::EvalSession& e)
      : values(e.rValues), hidden(e.rHidden), hiddenMacros(e.rHiddenMacros),
        hiddenMatrices(e.rHiddenMatrices), strings(e.rStrings), matrices(e.rMatrices),
        tableGen(e.rTableGen), tableRefusalGen(e.rTableRefusalGen), tableEid(e.rTableEid),
        tableRefusalEid(e.rTableRefusalEid), tableModel(e.rTableModel),
        tableRefusal(e.rTableRefusal), classCmd(e.rClassCmd), declaredCmd(e.rDeclaredCmd),
        unposted(e.rUnposted), classSetUnknown(e.rClassSetUnknown),
        declaredNames(e.rDeclaredNames) {}
  void restore(eval::EvalSession& e) const {
    e.rValues = values;
    e.rHidden = hidden;
    e.rHiddenMacros = hiddenMacros;
    e.rHiddenMatrices = hiddenMatrices;
    e.rStrings = strings;
    e.rMatrices = matrices;
    e.rTableGen = tableGen;
    e.rTableRefusalGen = tableRefusalGen;
    e.rTableEid = tableEid;
    e.rTableRefusalEid = tableRefusalEid;
    e.rTableModel = tableModel;
    e.rTableRefusal = tableRefusal;
    e.rClassCmd = classCmd;
    e.rDeclaredCmd = declaredCmd;
    e.rUnposted = unposted;
    e.rClassSetUnknown = classSetUnknown;
    e.rDeclaredNames = declaredNames;
  }
};
}  // namespace
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
    // `drop varlist` and `drop if|in` are two forms; a varlist with a qualifier is
    // neither (StataBE: `keep price mpg weight in 1/6` -> invalid syntax r(198)).
    if (!parsed.varlist.empty() && (parsed.ifExpr.has_value() || parsed.inRange.has_value())) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax", std::nullopt,
                                              std::nullopt, std::nullopt, std::string("r(198)"))));
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
    bool droppedEveryVariable = false;
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
      droppedEveryVariable = noVarsLeft;
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
    // c(changed): a dataset with no variables left is not a changed one
    // (vs-stata bank: `drop _all` / `drop *`, then describe, r(changed) 0).
    if (droppedEveryVariable) session.datasetChanged = false;
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
    // `keep varlist` and `keep if|in` are two forms; a varlist with a qualifier is
    // neither (StataBE: `keep price mpg weight in 1/6` -> invalid syntax r(198)).
    if (!parsed.varlist.empty() && (parsed.ifExpr.has_value() || parsed.inRange.has_value())) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax", std::nullopt,
                                              std::nullopt, std::nullopt, std::string("r(198)"))));
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
    // `sort` with no varlist is r(100) "varlist required" in StataBE 19.5, and
    // the varlist is checked before the options (truth bank: `so`, `so, d`,
    // `so, bogus`, all r(100) -- 85 syntaxopt cells); it used to reach the
    // engine as an empty key list and come back as its usage line, r(1).
    // The varlist is also checked before the qualifiers, which this arm answers
    // itself (registry.cpp kQualifiersRefusedByArm): `so in 5`, `so [fw=w]`,
    // `so = price*2` and `so using "f"` are r(100) too -- a leading `=` or the
    // word `using` is no variable name -- and with a varlist, `if` and weights
    // are r(101) (`so price mpg if foreign [aw=weight] in 1/50` "if not
    // allowed").
    const bool noVarlist =
        parsed.varlist.empty() || parsed.varlist[0] == "using" ||
        !(std::isalpha(static_cast<unsigned char>(parsed.varlist[0][0])) != 0 ||
          parsed.varlist[0][0] == '_' || static_cast<unsigned char>(parsed.varlist[0][0]) >= 0x80);
    // A leading `-` (gsort's descending sign) is r(198) "- invalid name"
    // (vs-stata bank cc968117 `sort -price`).
    if (!parsed.varlist.empty() && parsed.varlist[0][0] == '-') {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "- invalid name", std::nullopt,
                                              std::nullopt, std::nullopt, "r(198)")));
      return true;
    }
    if (noVarlist) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "varlist required", std::nullopt,
                                              std::nullopt, std::nullopt, "r(100)")));
      return true;
    }
    if (parsed.ifExpr.has_value()) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "if not allowed", std::nullopt,
                                              std::nullopt, std::nullopt, "r(101)")));
      return true;
    }
    if (parsed.weightType.has_value()) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "weights not allowed", std::nullopt,
                                              std::nullopt, std::nullopt, "r(101)")));
      return true;
    }
    if (parsed.inRange.has_value()) {
      blocks.push_back(ojson(eval::unsupportedOption(
          "in " + *parsed.inRange,
          "`sort` does not support the `in` row-range qualifier in this engine.")));
      return true;
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSort.known, kVocabSort.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    {
      // wildcards, ranges and `_all` name the variables sorted on
      std::vector<std::string> sortKeys = parsed.varlist;
      if (auto err = handlers::resolveByKeyNames(session, sortKeys)) {
        blocks.push_back(*err);
        return true;
      }
      blocks.push_back(ojson(handlers::runSort(sortKeys, session)));
    }
    return true;
  }


  if (cmd == "gsort") {
    // BUG-336 (batch 2): known set measured from the handler body -- runGsort takes no options.
    // gsort takes no qualifier: StataBE 19.5 answers a line with `if`, `in` or a
    // weight r(100) "varlist required" (vs-stata bank 970b955e62d11fdb `gsort
    // -price if foreign==1`; the in/weight twins). The `if` reached this arm
    // and was dropped.
    if (parsed.ifExpr.has_value() || parsed.inRange.has_value() || parsed.weightType.has_value()) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "varlist required", std::nullopt,
                                              std::nullopt, std::nullopt, "r(100)")));
      return true;
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabGsort.known, kVocabGsort.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // generate(newvar): a group number, 1, 2, ... in the sorted order, shared by the observations
    // that tie on every key; mfirst: the missing values of a DESCENDING numeric key come first
    // instead of last (an ascending key always sorts them last)
    // (vs-stata bank: `gsort -price, generate(ord)`, `gsort -union, mfirst`).
    const RClassSnapshot gsortRBefore(session.eval);  // gsort leaves r() as it was
    std::optional<std::string> gsortGen;
    bool gsortMfirst = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "generate" && !v.isBool) gsortGen = trimWs(v.strValue);
      if (k == "mfirst" && (v.isBool ? v.boolValue : true)) gsortMfirst = true;
    }
    if (gsortGen.has_value() && session.hasData()) {
      if (Datastore* gds = session.datastore(); gds != nullptr && gds->has(*gsortGen)) {
        blocks.push_back(ojson(eval::errorBlock("engine_error",
                                                "variable " + *gsortGen + " already defined",
                                                std::nullopt, std::nullopt, std::nullopt, "r(110)")));
        return true;
      }
    }
    // A key that is a wildcard stands for the variables it names, each with the key's sign.
    std::vector<std::string> gsortKeys;
    for (const auto& tok : parsed.varlist) {
      const bool signd = !tok.empty() && (tok[0] == '+' || tok[0] == '-');
      const std::string nm = signd ? tok.substr(1) : tok;
      if (session.hasData() && nm.find_first_of("*?~") != std::string::npos) {
        auto vr = piet::loops::expandVarlist(nm, session);
        if (vr.isError) {
          blocks.push_back(vr.error);
          return true;
        }
        for (const auto& v : vr.tokens) gsortKeys.push_back((signd ? tok.substr(0, 1) : std::string()) + v);
      } else {
        gsortKeys.push_back(tok);
      }
    }
    std::vector<std::string> mfFlags;
    if (gsortMfirst && session.hasData()) {
      std::vector<std::string> numericKey;
      {
        Datastore* gds = session.datastore();
        for (const auto& tok : gsortKeys) {
          const bool signd = !tok.empty() && (tok[0] == '+' || tok[0] == '-');
          const std::string nm = signd ? tok.substr(1) : tok;
          if (gds != nullptr && gds->has(nm) && gds->dtype(nm) == "f64") numericKey.push_back(nm);
        }
      }
      std::vector<std::string> withFlags;
      for (const auto& tok : gsortKeys) {
        const bool signd = !tok.empty() && (tok[0] == '+' || tok[0] == '-');
        const std::string nm = signd ? tok.substr(1) : tok;
        if (signd && tok[0] == '-' &&
            std::find(numericKey.begin(), numericKey.end(), nm) != numericKey.end()) {
          const std::string flag = "__piet_gs_mf" + std::to_string(mfFlags.size());
          (void)dispatch("quietly generate byte " + flag + " = missing(" + nm + ")", session);
          mfFlags.push_back(flag);
          withFlags.push_back("-" + flag);
        }
        withFlags.push_back(tok);
      }
      gsortKeys = std::move(withFlags);
    }
    ojson gsorted = ojson(handlers::runGsort(gsortKeys, session));
    for (const auto& f : mfFlags) (void)dispatch("quietly drop " + f, session);
    blocks.push_back(std::move(gsorted));
    const bool gsortFailed = blocks.back().is_object() &&
                             blocks.back().value("type", std::string()) == "error";
    if (!gsortFailed && gsortGen.has_value() && !gsortGen->empty()) {
      // The group number: it steps up where any key's value differs from the row above (an
      // extended missing differs from `.`), is stored in the smallest integer type that holds
      // it, and carries gsort's own label.
      std::string differs = "_n == 1";
      for (const auto& tok : gsortKeys) {
        const bool signd = !tok.empty() && (tok[0] == '+' || tok[0] == '-');
        const std::string nm = signd ? tok.substr(1) : tok;
        if (nm.rfind("__piet_gs_mf", 0) == 0) continue;
        differs += " | " + nm + " != " + nm + "[_n-1]";
      }
      std::vector<std::string> lines = {
          "generate long " + *gsortGen + " = " + differs,
          "quietly replace " + *gsortGen + " = sum(" + *gsortGen + ")",
          "quietly compress " + *gsortGen,
          "quietly label variable " + *gsortGen + " \"Unique group ID from gsort\""};
      for (const auto& line : lines) {
        ojson made = dispatch(line, session);
        if (made.is_array()) {
          for (const auto& b : made)
            if (!(b.is_object() && b.value("type", std::string()) == "echo")) blocks.push_back(b);
        } else {
          blocks.push_back(std::move(made));
        }
      }
    }
    if (!gsortFailed) gsortRBefore.restore(session.eval);
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
    // gcollapse's labelformat() labels the results (runCollapse reads it).
    static const std::unordered_set<std::string> kGcollapseArmOpts = {"by", "cw", "fast",
                                                                      "verbose", "labelformat"};
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
      if ((k == "frame" || k == "by" || k == "labelformat") == v.isBool) {
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
      // The names are read in order, a comma standing as a token of its own: the first that is not
      // a name is `<token> invalid name` r(198) (vs-stata bank: `frame(cf, replace)` -> ", invalid
      // name"), and only then do two or more names count as too many.
      std::vector<std::string> nameTokens;
      {
        std::istringstream names(v.strValue);
        for (std::string w; names >> w;) {
          std::string cur;
          for (char ch : w) {
            if (ch == ',') {
              if (!cur.empty()) nameTokens.push_back(cur);
              nameTokens.push_back(",");
              cur.clear();
            } else {
              cur += ch;
            }
          }
          if (!cur.empty()) nameTokens.push_back(cur);
        }
      }
      for (const auto& tok : nameTokens) {
        if (!strutil::isValidStataIdentifier(tok)) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", tok + " invalid name",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  std::string("r(198)"))));
          return true;
        }
      }
      if (nameTokens.size() > 1) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                                "option frame():  too many names specified",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                std::string("r(103)"))));
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
    // A dataset with no observations has nothing to collapse: r(2000) "no
    // observations" (vs-stata bank 6bfc2871de4549b1, 7be0166c70099e29, after
    // `set obs 0`).
    if (collapseFlavor == "collapse" && collapseNBefore == 0) {
      blocks.push_back(ojson(eval::errorBlock("engine_error", "no observations", std::nullopt,
                                              std::nullopt, std::nullopt, std::string("r(2000)"))));
      return true;
    }
    ojson collapseBlk = ojson(handlers::runCollapse(parsed.raw, parsed.ifExpr, session,
                                                    parsed.opts, parsed.weightVar,
                                                    parsed.inRange, collapseFlavor,
                                                    parsed.weightType));
    if (collapseBlk.value("type", std::string()) != "error" && collapseNBefore >= 0) {
      // r(N): the observations collapsed (after if/in, cw and a missing or zero
      // weight), which runCollapse reports when it filtered.
      const int64_t nUsed = collapseBlk.value("n_used", collapseNBefore);
      session.eval.rValues.set("N", static_cast<double>(nUsed));
      if (collapseBlk.contains("gtools_J")) {
        session.eval.rValues.set("J", collapseBlk.value("gtools_J", 0.0));
        session.eval.rValues.set("minJ", collapseBlk.value("gtools_minJ", 0.0));
        session.eval.rValues.set("maxJ", collapseBlk.value("gtools_maxJ", 0.0));
      }
    }
    // fcollapse.ado builds a helper variable when the run is restricted (if, in, a weight, cw)
    // and drops it at the end: r(k_drop) is 1 then (vs-stata bank).
    if (collapseBlk.value("type", std::string()) != "error" && cmdTyped == "fcollapse") {
      bool restricted = parsed.ifExpr.has_value() || parsed.inRange.has_value() ||
                        parsed.weightType.has_value();
      for (const auto& kv : parsed.opts)
        if (kv.first == "cw") restricted = true;
      if (restricted) {
        session.eval.rValues.set("k_drop", 1.0);
        eval::declareRPosted(session.eval, "fcollapse", {"k_drop"});
      }
    }
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


  // `expandcl [=]exp [if] [in], generate(newvar) cluster(varlist)` ([R]
  // expandcl): expand each cluster exp times (exp must be constant within a
  // cluster, else r(198)), the copies numbered as new clusters 1, 2, ... in
  // generate() ("New cluster ID from expandcl"); the order of the data is kept
  // as a sortpreserve command keeps it (vs-stata bank 9d7ded1700877074).
  if (cmd == "expandcl") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {"generate", "cluster"}, {})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    auto refuse = [&](const std::string& msg, const char* rc) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", msg, std::nullopt, std::nullopt,
                                              std::nullopt, rc)));
      return true;
    };
    std::string gen, clus;
    // GENerate() and CLuster() abbreviate to gen / cl.
    auto isAbbrev = [](const std::string& k, const char* full, size_t minLen) {
      const std::string f(full);
      return k.size() >= minLen && k.size() <= f.size() && f.compare(0, k.size(), k) == 0;
    };
    for (const auto& [k, v] : parsed.opts) {
      if (isAbbrev(k, "generate", 3) && !v.isBool) gen = strutil::trim(v.strValue);
      if (isAbbrev(k, "cluster", 2) && !v.isBool) clus = strutil::trim(v.strValue);
    }
    if (gen.empty()) return refuse("option generate() required", "r(198)");
    if (clus.empty()) return refuse("option cluster() required", "r(198)");
    if (!session.hasData()) {
      blocks.push_back(ojson(eval::noDataLoaded()));
      return true;
    }
    std::string ex;
    for (const auto& t : parsed.varlist) ex += (ex.empty() ? "" : " ") + t;
    ex = strutil::trim(ex);
    if (!ex.empty() && ex[0] == '=') ex = strutil::trim(ex.substr(1));
    if (ex.empty()) return refuse("=exp required", "r(100)");
    if (Datastore* eds = session.datastore())
      if (eds->has(gen)) return refuse("variable " + gen + " already defined", "r(110)");
    std::string qual;
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) qual += " if " + *parsed.ifExpr;
    if (parsed.inRange.has_value() && !parsed.inRange->empty()) qual += " in " + *parsed.inRange;
    const std::string t = "__expandcl_t", v = "__expandcl_v", o = "__expandcl_o",
                      e = "__expandcl_e", f = "__expandcl_f", s = "__expandcl_s";
    auto run = [&](const std::string& line, bool keep) -> bool {
      ojson r = dispatch(line, session);
      for (const auto& b : r) {
        if (!b.is_object()) continue;
        const std::string ty = b.value("type", std::string());
        if (ty == "error") {
          blocks.push_back(b);
          return false;
        }
        if (keep && ty == "display") blocks.push_back(b);
      }
      return true;
    };
    auto cleanup = [&]() {
      (void)dispatch("capture drop " + t + " " + v + " " + o + " " + e + " " + f, session);
      (void)dispatch("capture sort " + s + ", stable", session);
      (void)dispatch("capture drop " + s, session);
    };
    if (!run("quietly generate long " + s + " = _n", false)) return true;
    const std::vector<std::string> pre = {
        "quietly generate byte " + t + " = 0",
        "quietly replace " + t + " = 1" + qual,
        "quietly generate long " + v + " = " + ex,
        "quietly sort " + t + " " + clus + ", stable",
    };
    for (const auto& line : pre)
      if (!run(line, false)) {
        cleanup();
        return true;
      }
    {
      // exp must be constant within each cluster of the sample.
      bool bad = !run("quietly by " + t + " " + clus + ": generate byte " + f + " = " + v +
                          " != " + v + "[1] if " + t,
                      false);
      if (!bad) bad = !run("quietly count if " + f + " == 1", false);
      if (!bad) {
        const double* nbad = session.eval.rValues.find("N");
        bad = nbad == nullptr || *nbad != 0;
        (void)dispatch("quietly drop " + f, session);
      }
      if (bad) {
        cleanup();
        return refuse("expression is not constant within clusters", "r(198)");
      }
    }
    const std::vector<std::string> body = {
        "quietly generate long " + o + " = _n",
        "quietly drop " + v,
    };
    for (const auto& line : body)
      if (!run(line, false)) {
        cleanup();
        return true;
      }
    if (!run("expand " + ex + " if " + t, true)) {
      cleanup();
      return true;
    }
    const std::vector<std::string> post = {
        "quietly sort " + t + " " + clus + " " + o + ", stable",
        "quietly by " + t + " " + clus + " " + o + ": generate long " + e + " = _n",
        "quietly sort " + t + " " + clus + " " + e + ", stable",
        "quietly drop " + o,
        "quietly by " + t + " " + clus + " " + e + ": generate " + o + " = _n==1 if " + t,
        "quietly replace " + e + " = sum(" + o + ") if " + t,
        "quietly rename " + e + " " + gen,
        "quietly label variable " + gen + " \"New cluster ID from expandcl\"",
        "quietly drop " + t + " " + o,
        "quietly sort " + s + ", stable",
        "quietly drop " + s,
    };
    for (const auto& line : post)
      if (!run(line, false)) {
        cleanup();
        return true;
      }
    blocks.push_back(ojson(handlers::mutationResult(session, "expandcl")));
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
    // `expand =exp` is `expand exp`.
    if (!arg.empty() && arg[0] == '=') arg = trimWs(arg.substr(1));
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
    {
      ojson er = ojson(handlers::runExpandIf(arg, expandGen, parsed.ifExpr, parsed.inRange, session));
      if (er.is_array()) {
        for (const auto& b : er) blocks.push_back(b);
      } else {
        blocks.push_back(std::move(er));
      }
    }
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
    if (auto err = eval::rejectUnknownOptions(
            parsed.opts, {"freq", "cfreq", "percent", "cpercent", "format", "float", "zero",
                          "nomiss"})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // contract leaves r() alone too (BUG-1451, measured as for fillin), unless it works out
    // percentages (then the summarize it reads the total from is what is left).
    const bool plainContract = parsed.opts.empty() && !parsed.ifExpr.has_value() &&
                               !parsed.inRange.has_value() && !parsed.weightVar.has_value();
    if (plainContract) {
      blocks.push_back(ojson(handlers::runContract(parsed.varlist, session)));
    } else {
      ojson cr = ojson(handlers::runContractFull(parsed, session));
      if (cr.is_array()) {
        for (const auto& b : cr) blocks.push_back(b);
      } else {
        blocks.push_back(std::move(cr));
      }
    }
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
    // `isid VARS using FILE` checks the file.
    std::optional<size_t> usingAt;
    for (size_t i = 0; i < parsed.varlist.size(); ++i)
      if (lower(parsed.varlist[i]) == "using") {
        usingAt = i;
        break;
      }
    if (usingAt.has_value() && *usingAt + 1 < parsed.varlist.size()) {
      std::vector<std::string> keys(parsed.varlist.begin(),
                                    parsed.varlist.begin() + static_cast<long>(*usingAt));
      std::string file = parsed.varlist[*usingAt + 1];
      if (file.size() >= 2 && file.front() == '"' && file.back() == '"')
        file = file.substr(1, file.size() - 2);
      ojson ru = handlers::runIsidUsing(keys, file, session, missok, sort);
      if (ru.is_array()) {
        for (const auto& b : ru) blocks.push_back(b);
      } else {
        blocks.push_back(ru);
      }
      return true;
    }
    // isid.ado leaves r(N) (CheckUniq's count of the keys that are not missing) or, under
    // `sort`, r(sorted) (CheckAlreadySorted: whether the data already led with the varlist).
    bool sortedBefore = false;
    {
      std::istringstream in(session.eval.sortedBy ? session.eval.sortedBy() : std::string());
      std::vector<std::string> marker;
      for (std::string k; in >> k;) marker.push_back(k);
      sortedBefore = !parsed.varlist.empty() && parsed.varlist.size() <= marker.size() &&
                     std::equal(parsed.varlist.begin(), parsed.varlist.end(), marker.begin());
    }
    const bool isGisid = lower(cmdTyped) == "gisid";
    const bool qualified = (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
                           (parsed.inRange.has_value() && !parsed.inRange->empty());
    ojson r = isGisid && qualified ? handlers::runGisidSample(parsed, session, missok)
                                   : handlers::runIsid(parsed.varlist, session, missok, sort);
    if (r.is_array()) {
      for (const auto& b : r) blocks.push_back(b);
    } else {
      blocks.push_back(r);
    }
    const bool isidFailed = r.is_object() && r.value("type", std::string()) == "error";
    if (isGisid) {
      if (!isidFailed) handlers::gisidPublishResults(parsed, session);
      return true;
    }
    if (!isidFailed && (sort || !missok)) {
      eval::replaceRClass(session.eval);
      if (sort) {
        session.eval.rValues.set("sorted", sortedBefore ? 1.0 : 0.0);
        eval::declareRPosted(session.eval, "isid", {"sorted"});
      } else {
        Datastore* nds = session.datastore();
        session.eval.rValues.set("N", nds != nullptr ? static_cast<double>(nds->nRows()) : 0.0);
        eval::declareRPosted(session.eval, "isid", {"N"});
      }
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
    // `datasignature if ...` / `in ...`: the word after the command is the subcommand, and `if` is not one.
    if (dsSub.empty() && ((parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
                          (parsed.inRange.has_value() && !parsed.inRange->empty()))) {
      blocks.push_back(ojson(eval::errorBlock("engine_error", "invalid datasignature subcommand",
                                              std::nullopt, std::nullopt, std::nullopt, "r(198)")));
      return true;
    }
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
    // No data in memory (a cleared dataset, a file of zero variables): the
    // signature of nothing, 0:0(0):0:0 (bank DSIG of every empty cell).
    if (!session.hasData()) {
      session.eval.rValues = eval::OrderedFloatDict();
      session.eval.rStrings.clear();
      session.eval.rStrings.set("datasignature", "0:0(0):0:0");
      ojson empty;
      empty["type"] = "display";
      empty["text"] = "  0:0(0):0:0";
      blocks.push_back(std::move(empty));
      return true;
    }
    ojson dsBlk = ojson(handlers::runDatasignature(session));
    if (dsBlk.value("type", std::string()) == "display") {
      session.eval.rStrings.set("datasignature", trimWs(dsBlk.value("text", std::string())));
      dsBlk["text"] = "  " + dsBlk.value("text", std::string());  // indented, as Stata prints it
    }
    blocks.push_back(std::move(dsBlk));
    return true;
  }


  if (cmd == "preserve") {
    // `preserve [, changed]`: any other option is r(198) "option X not
    // allowed" (vs-stata bank fee7d08734dd0d8d, 2a1f897a0efbc661).
    for (const auto& kv : parsed.opts) {
      const std::string k = lower(kv.first);
      if (k.empty() || std::string("changed").compare(0, k.size(), k) != 0 || !kv.second.isBool) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "option " + kv.first + " not allowed",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(198)")));
        return true;
      }
    }
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
      session.discardOwnPreserve();
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
    bool trim = false, replace = false, suffixGiven = false, w2label = false;
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
          // cuts(numlist max=2 min=2): fewer than two is r(122), more r(123)
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error",
              std::string("cuts() invalid -- invalid numlist has too ") +
                  (parts.size() < 2 ? "few" : "many") + " elements",
              std::nullopt, std::nullopt, std::nullopt, parts.size() < 2 ? "r(122)" : "r(123)")));
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
      } else if (k == "label" && v.isBool && v.boolValue) {
        w2label = true;
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
    // winsor2.ado's `syntax varlist(min=1)`: wildcards and ranges name the variables, a name
    // that is no variable is r(111) and a string variable r(109); then the checks of its own
    // body, in its order -- cuts() given low above high (the ado swaps them through a tempname
    // it then cannot read back: r(111)), and a result name that already exists.
    std::vector<std::string> w2vars = parsed.varlist;
    if (auto err = handlers::resolveByKeyNames(session, w2vars)) {
      blocks.push_back(*err);
      return true;
    }
    {
      Datastore* wds = session.datastore();
      auto rcBlock = [&](const std::string& msg, const char* rc) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                                std::nullopt, rc)));
      };
      if (wds != nullptr) {
        for (const auto& nm : w2vars) {
          if (!wds->has(nm)) { rcBlock("variable " + nm + " not found", "r(111)"); return true; }
        }
        for (const auto& nm : w2vars) {
          if (wds->dtype(nm) == "factor") { rcBlock("type mismatch", "r(109)"); return true; }
        }
        if (cutLo > cutHi) { rcBlock("__000001 not found", "r(111)"); return true; }
        if (!replace) {
          for (const auto& nm : w2vars) {
            if (wds->has(nm + suffix)) {
              blocks.push_back(ojson(eval::errorBlock(
                  "engine_error",
                  "variable " + nm + suffix + " already exist, re-specify option -suffix()-\n"
                      "Suffix " + suffix + " is invalid for " + nm,
                  std::nullopt, std::nullopt, std::nullopt, "r(111)")));
              return true;
            }
          }
        }
      }
    }
    // cuts(0 100) is no cut at all: the ado says so and does nothing.
    if (cutLo == 0 && cutHi == 100) {
      ojson note;
      note["type"] = "display";
      note["text"] = "option cuts(0 100) is incorrect, no action taken.";
      blocks.push_back(note);
      return true;
    }
    // Each variable's label before it is touched (winsor2 labels its result
    // `labk`, the source's label or name, and with `label` adds the cut).
    std::map<std::string, std::string> w2labk;
    for (const auto& nm : w2vars) {
      std::string lab = nm;
      auto it = session.varMeta.find(nm);
      if (it != session.varMeta.end() && it->second.label.has_value() && !it->second.label->empty())
        lab = *it->second.label;
      w2labk[nm] = lab;
    }
    // A cut at 0 or 100 takes that bound from `summarize`, whose r() is then what the
    // command leaves: the last variable's, over every observation, before it is changed
    // (bank 495cfe130aabe5de `cuts(10 100)`, 6e44ce3717080237 `cuts(0 90) trim`).
    std::optional<std::vector<std::pair<std::string, double>>> w2sumR;
    if (cutLo == 0 || cutHi == 100) {
      ojson sb = dispatch("quietly summarize " + w2vars.back(), session);
      w2sumR.emplace();
      for (const char* key : {"N", "sum_w", "mean", "Var", "sd", "min", "max", "sum"}) {
        const double* val = session.eval.rValues.find(key);
        w2sumR->emplace_back(key, val != nullptr ? *val : std::nan(""));
      }
    }
    ojson w2res;
    std::optional<double> w2EgenN;
    if (!byStr.empty()) {
      // by(): the ado's own steps, per variable -- egen's pctile() (or min()/max() at a 0/100
      // cut) by the groups, stored in the variable's type, then rowmax/rowmin (winsorize) or
      // two cond() replaces (trim), over `if` & !missing(k) and `in`.
      std::string qualIf = parsed.ifExpr.has_value() && !parsed.ifExpr->empty() ? *parsed.ifExpr : "";
      std::string qualIn = parsed.inRange.has_value() && !parsed.inRange->empty()
                               ? " in " + *parsed.inRange
                               : std::string();
      auto fmtCut = [](double x) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.15g", x);
        return std::string(buf);
      };
      auto run = [&](const std::string& line) -> bool {
        ojson out = dispatch("quietly " + line, session);
        for (const auto& b : out)
          if (b.is_object() && b.value("type", std::string()) == "error") {
            blocks.push_back(b);
            return false;
          }
        return true;
      };
      std::vector<std::string> made;
      for (const auto& k : w2vars) {
        const std::string vtype = storage::resolveReplaceType(session, k);
        const std::string if2 =
            " if " + (qualIf.empty() ? std::string() : "(" + qualIf + ") & ") + "!missing(" + k + ")";
        const std::string pL = "__w2pL", pH = "__w2pH", tL = "__w2tL", kw = "__w2kw";
        const std::string lowE = cutLo == 0 ? "min(" + k + ")" : "pctile(" + k + ")";
        const std::string highE = cutHi == 100 ? "max(" + k + ")" : "pctile(" + k + ")";
        const std::string lowP = cutLo == 0 ? "" : ", p(" + fmtCut(cutLo) + ") by(" + byStr + ")";
        const std::string highP = cutHi == 100 ? "" : ", p(" + fmtCut(cutHi) + ") by(" + byStr + ")";
        if (!run("egen " + vtype + " " + pL + " = " + lowE + if2 + qualIn +
                 (lowP.empty() ? ", by(" + byStr + ")" : lowP)))
          return true;
        if (!run("egen " + vtype + " " + pH + " = " + highE + if2 + qualIn +
                 (highP.empty() ? ", by(" + byStr + ")" : highP)))
          return true;
        // egen's rowmax()/rowmin() of two variables is max()/min() of them (a missing one
        // ignored), stored in the type asked for.
        if (!run("gen " + vtype + " " + tL + " = max(" + pL + ", " + k + ")" + if2 + qualIn))
          return true;
        const std::string out = replace ? k : k + suffix;
        if (!trim) {
          if (replace) {
            if (!run("gen " + vtype + " " + kw + " = min(" + pH + ", " + tL + ")" + if2 + qualIn) ||
                !run("replace " + k + " = " + kw))
              return true;
          } else if (!run("gen " + out + " = min(" + pH + ", " + tL + ")" + if2 + qualIn)) {
            return true;
          }
        } else {
          const std::string first = replace ? "replace " + k + " = cond(" + k + "<" + pL + ",.," + k + ")"
                                            : "gen " + vtype + " " + out + " = cond(" + k + "<" + pL +
                                                  ",.," + k + ")";
          if (!run(first + if2 + qualIn) ||
              !run("replace " + out + " = cond(" + k + ">" + pH + ",.," + out + ")" + if2 + qualIn))
            return true;
        }
        // egen ends by counting the missing values it generated: r(N) is that count for the
        // last egen result (rowmin's, or under trim rowmax's; bank 5efb88985f92e9fb r(N) 0).
        {
          const std::string lastEgen = trim ? tL : (replace ? kw : out);
          w2EgenN = 0;
          if (Datastore* cds = session.datastore())
            if (cds->has(lastEgen)) {
              const double* d = cds->col(lastEgen).f64();
              for (int64_t i = 0; i < cds->nRows(); ++i)
                if (std::isnan(d[i])) *w2EgenN += 1;
            }
        }
        if (!run("drop " + pL + " " + pH + " " + tL + (replace && !trim ? " " + kw : std::string())))
          return true;
        made.push_back(out);
      }
      w2res = handlers::mutationResult(session, "winsor2",
                                       replace ? std::vector<std::string>{} : made);
    } else {
      w2res = handlers::runWinsor2(session, w2vars, cutLo, cutHi, trim, byStr, suffix, replace,
                                   &parsed);
    }
    if (w2res.is_object() && w2res.value("type", std::string()) != "error") {
      // labels: `labk`, or with `label` `labk-Winsor(p<lo>,p<hi>)` / `-Trim(...)`, a cut
      // below 1 written with a leading 0 (the ado prefixes "0" to the macro text).
      auto macroNum = [](double x) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.15g", x);
        std::string t = buf;
        if (t.rfind("0.", 0) == 0) t.erase(0, 1);
        return t;
      };
      std::string lo = macroNum(cutLo);
      if (cutLo < 1) lo = "0" + lo;
      const std::string tag = std::string(trim ? "-Trim(p" : "-Winsor(p") + lo + ",p" + macroNum(cutHi) + ")";
      for (const auto& nm : w2vars) {
        const std::string out = replace ? nm : nm + suffix;
        session.setVarLabel(out, w2labk[nm] + (w2label ? tag : std::string()));
      }
      if (w2EgenN) {
        eval::replaceRClass(session.eval);
        session.eval.rValues.set("N", *w2EgenN);
        eval::declareRPosted(session.eval, "count", {"N"});
      } else if (w2sumR) {
        eval::replaceRClass(session.eval);
        std::vector<std::string> posted;
        for (const auto& [key, val] : *w2sumR) {
          session.eval.rValues.set(key, val);
          posted.push_back(key);
        }
        eval::declareRPosted(session.eval, "summarize", posted);
      }
    }
    blocks.push_back(w2res);
    return true;
  }


  if (cmd == "tsreport") {
    blocks.push_back(ojson(handlers::runTsreport(parsed, session)));
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
    if (stub.find_first_of(" \t") != std::string::npos) {
      // the linear forms: the if/in sample is read here, the spline built by the handler
      std::vector<char> inSample;
      if (auto err = handlers::dataset_shape_detail::ifInMask(session, parsed, &inSample)) {
        blocks.push_back(*err);
        return true;
      }
      pushEnvResult(blocks, handlers::runMksplineSegments(session, stub, srcVar, parsed.opts, inSample));
      return true;
    }
    // The cubic form computes over every observation (see runMkspline), so an `in` range
    // is refused by name rather than read and dropped, as its `if` is.
    if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
      blocks.push_back(ojson(eval::unsupportedOption(
          "in " + *parsed.inRange,
          "`mkspline, cubic` computes over every observation. Restrict first (`preserve` + "
          "`keep in ...`), then run mkspline.")));
      return true;
    }
    pushEnvResult(blocks, handlers::runMkspline(stub, srcVar, parsed.opts, parsed.ifExpr, session));
    return true;
  }


  // BUG-370 — see registry.cpp for why these three exist and what they refuse.
  // All follow the mkspline arm's `stub = var` split (joinVarlist -> find('=')).
  if (cmd == "xtile") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {"nquantiles", "cutpoints", "altdef"},
                                              {})) {
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
    // cmdTyped is gquantiles when the gtools alias translated the line here.
    blocks.push_back(ojson(handlers::runXtileStata(trimWs(raw.substr(0, eqPos)), expText,
                                                   parsed.opts, parsed.ifExpr, parsed.inRange,
                                                   xtWeight, session, parsed.weightType,
                                                   lower(cmdTyped) == "xtile")));
    if (lower(cmdTyped) == "gquantiles" && blocks.back().is_object()) {
      if (blocks.back().value("type", std::string()) != "error") {
        handlers::gquantilesPublishXtile(parsed, session, trimWs(expText));
      } else {
        // gquantiles reads `newvar = exp` itself: an expression it cannot evaluate (a name
        // that is not there, a string variable) is a syntax error, not "not found".
        const std::string reason = blocks.back().value("reason", std::string());
        const std::string what = blocks.back().value("message", std::string());
        if (reason == "r(111)" || reason == "r(109)" ||
            (what.size() > 10 && what.compare(what.size() - 10, 10, " not found") == 0))
          blocks.back() = ojson(eval::errorBlock("syntax_error",
                                                 "Invalid syntax. Requried: newvarname = exp",
                                                 std::nullopt, std::nullopt, std::nullopt, "r(198)"));
      }
    }
    return true;
  }


  if (cmd == "ipolate") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {"generate", "epolate", "by"}, {})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    blocks.push_back(
        ojson(handlers::runIpolate(parsed.varlist, parsed.opts, parsed.ifExpr, session)));
    // ipolate.ado ends with `count if newvar >= .`: r(N) is the missing values it left
    // (vs-stata bank).
    if (!(blocks.back().is_object() && blocks.back().value("type", std::string()) == "error")) {
      std::string newvar;
      for (const auto& [k, v] : parsed.opts)
        if (k == "generate" && !v.isBool) newvar = trimWs(v.strValue);
      if (!newvar.empty()) {
        const std::string savedLeaf = session.leafCmdExpanded;
        (void)dispatch("quietly count if missing(" + newvar + ")", session);
        session.leafCmdExpanded = savedLeaf;
        eval::declareRPosted(session.eval, "ipolate", {"N"});
      }
    }
    return true;
  }


  if (cmd == "tssmooth") {
    // Two-word command, registered under the first word: the sub-verb arrives as
    // varlist[0] (the duplicates/estat convention).
    std::string sub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
    // tssmooth's smoothers ([TS] tssmooth): ma, exponential, dexponential,
    // hwinters, shwinters, nl, each with its documented minimum (e, d, h, s).
    // Any other word is r(198) "<word> is not a valid smoother" (vs-stata bank
    // 9a81954fe3a6ba87 `tssmooth zzmethod sm = gnp96`).
    if (!sub.empty()) {
      static const std::pair<const char*, size_t> kSmoothers[] = {
          {"ma", 2}, {"exponential", 1}, {"dexponential", 1}, {"hwinters", 1},
          {"shwinters", 1}, {"nl", 2}};
      bool known = false;
      for (const auto& [full, minLen] : kSmoothers) {
        const std::string f(full);
        known = known || (sub.size() >= minLen && sub.size() <= f.size() &&
                          f.compare(0, sub.size(), sub) == 0);
      }
      if (!known) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", sub + " is not a valid smoother",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(198)")));
        return true;
      }
    }
    // The new variable is checked before the options, as Stata's syntax reads a
    // line: `tssmooth ma gnp96 = gnp96, window(2 1 2) re` .. `replac` (and `r`)
    // are r(110) "variable gnp96 already defined" (vs-stata bank b37db5600da94ae7,
    // bad362d2be2a88c6) -- replace takes no abbreviation.
    if (sub == "ma" && session.hasData()) {
      std::string rawMa =
          joinVarlist(std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end()));
      const size_t eq = rawMa.find('=');
      const std::string nv = eq == std::string::npos ? std::string() : trimWs(rawMa.substr(0, eq));
      bool fullReplace = false;
      for (const auto& kv : parsed.opts) fullReplace = fullReplace || kv.first == "replace";
      if (!nv.empty() && !fullReplace && targetVarExists(session, nv).value_or(false)) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "variable " + nv + " already defined",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(110)")));
        return true;
      }
    }
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
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
      // The smoother's window over a restricted sample (do rows outside it count as missing
      // in the window?) is not something this build computes; running it over every
      // observation would answer a different question at rc 0.
      blocks.push_back(ojson(eval::unsupportedOption(
          "if " + *parsed.ifExpr,
          "`tssmooth ma` in this build smooths over every observation. Restrict first "
          "(`preserve` + `keep if ...`), then run tssmooth.")));
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
