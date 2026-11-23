// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/eval/result_order.h"  // orderRResults (tabulate lists r() as Stata does)
#include "results_support.h"
#include "summary_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
#include "piet/handlers/descriptive.h"
#include "data/missing.h"  // cecon::missing_kind (tabstat, missing)
#include <set>
namespace piet::dispatch_detail {
bool route_tabulation(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {


  // BUG-105 abbreviation stopgap: `ta` (see the `describe`/`d` note above).
  if (cmd == "tabulate" || cmd == "tab" || cmd == "ta" || cmd == "tab1" || cmd == "tab2") {
    if (!session.hasData()) {
      blocks.push_back(ojson(eval::noDataLoaded()));
      return true;
    }
    // tab1 and tab2 run the one-way / two-way tabulate for each variable (pair) inside
    // `capture` (tab1.ado, tab2.ado): a refusal the inner tabulate raises is printed
    // under the first table's heading, the loop stops, r() is empty, and the command
    // itself ends rc 0 (bank: `tab1 rep78, zznotanoption`, `tab2 rep78 foreign, n`).
    // Only the refusals of the inner tabulate come this way; a missing varlist or an
    // unknown variable is tab1's own syntax and stays an error.
    const bool tabLoop = cmd == "tab1" || cmd == "tab2";
    auto tabRefuse = [&](ojson err) {
      if (!tabLoop || parsed.varlist.empty() || err.value("type", std::string()) != "error") {
        blocks.push_back(std::move(err));
        return true;
      }
      std::string msg = err.value("message", std::string());
      // What tab1 / tab2 swallow is Stata's own refusal of one table. PIET's "not implemented" answer is
      // not Stata's: swallowing it would end rc 0 with no table and no r(), so it stays the error.
      if (msg.find("does not yet implement") != std::string::npos ||
          msg.find("not supported in this version of PIET") != std::string::npos ||
          msg.find("not implemented") != std::string::npos) {
        blocks.push_back(std::move(err));
        return true;
      }
      msg.erase(std::remove(msg.begin(), msg.end(), '`'), msg.end());
      std::string heading = "-> tabulation of " + parsed.varlist[0];
      if (cmd == "tab2" && parsed.varlist.size() >= 2) heading += " by " + parsed.varlist[1];
      heading += parsed.ifExpr.has_value() && !parsed.ifExpr->empty() ? " if " + *parsed.ifExpr + " " : "  ";
      ojson head, refusal;
      head["type"] = "display";
      head["text"] = heading;
      refusal["type"] = "display";
      refusal["text"] = msg;
      blocks.push_back(std::move(head));
      blocks.push_back(std::move(refusal));
      eval::replaceRClass(session.eval);
      eval::declareRPosted(session.eval, "tabulate", {});
      return true;
    };
    // One- and two-letter spellings Stata reads as the first option of tabulate's syntax line that
    // they prefix, and that no vocabulary row can carry because the same letters also open an
    // option of the other kind (bank: `tabulate rep78, s(price)` and `su(price)` are
    // summarize(); bare `e` is exact; bare `s` / `su` are "option s incorrectly specified";
    // `nol` is nolabel on tab1 and tab2 as it is on tabulate). A bare `l` is read and does
    // nothing at all: the table, r() and the statistics lines are those of the line without it
    // (bank 11e74e34, d9c31dcc), so it is dropped here rather than guessed to be lrchi2.
    for (auto& kv : parsed.opts) {
      std::string& key = kv.first;
      if ((key == "s" || key == "su") && !kv.second.isBool) key = "summarize";
      else if (key == "e" && kv.second.isBool) key = "exact";
      else if (key == "nol" && kv.second.isBool) key = "nolabel";
    }
    parsed.opts.erase(std::remove_if(parsed.opts.begin(), parsed.opts.end(),
                                     [](const auto& kv) { return kv.first == "l" && kv.second.isBool; }),
                      parsed.opts.end());
    for (const auto& kv : parsed.opts) {
      if ((kv.first == "s" || kv.first == "su") && kv.second.isBool)
        return tabRefuse(ojson(eval::errorBlock("syntax_error", "option " + kv.first + " incorrectly specified",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                std::string("r(198)"))));
    }
    double tabExactWork = 1.0;
    bool tabFirstOnly = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "exact" && !v.isBool) {
        try { tabExactWork = std::max(1.0, std::stod(strutil::trim(v.strValue))); } catch (...) {}
      }
      if (k == "firstonly") tabFirstOnly = true;
    }
    if (tabFirstOnly && cmd != "tab2")
      return tabRefuse(ojson(eval::engineError("option `firstonly` not allowed", "unsupported_option")));
    bool chi2 = false;
    std::optional<std::string> sumvar;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "chi2" && v.isBool && v.boolValue) chi2 = true;
      if ((k == "sum" || k == "summarize") && !v.isBool) sumvar = v.strValue;
    }
    // BUG-1514: the two-way statistics lrchi2, V, gamma and taub, `all`, which is
    // chi2 lrchi2 V gamma taub ([R] tabulate twoway; StataBE 19.5 prints and posts
    // the five for `all`, not exact), and Fisher's `exact`. `tabStatWords` keeps
    // the words in the order typed for the refusals below.
    unsigned tabStats = 0;
    std::vector<std::string> tabStatWords;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "exact") {  // `exact` or `exact(#)`
        tabStats |= handlers::kTabExact;
        tabStatWords.push_back(k);
        continue;
      }
      if (!v.isBool || !v.boolValue) continue;
      unsigned bit = 0;
      if (k == "lrchi2") bit = handlers::kTabLrchi2;
      else if (k == "v") bit = handlers::kTabCramersV;
      else if (k == "gamma") bit = handlers::kTabGamma;
      else if (k == "taub") bit = handlers::kTabTaub;
      else if (k == "all")
        bit = handlers::kTabLrchi2 | handlers::kTabCramersV | handlers::kTabGamma | handlers::kTabTaub;
      if (bit == 0 && k != "chi2") continue;
      tabStats |= bit;
      if (k == "all") chi2 = true;
      tabStatWords.push_back(k == "v" ? std::string("V") : k);
    }
    // expected, cchi2 and clrchi2 are two-way cell lines drawn from the counts and margins
    // (cellLines below); plot is a one-way bar column (tabPlot).
    bool tabSubpop = false;
    std::string tabSubpopVar;
    for (const auto& [k, v] : parsed.opts)
      if (k == "subpop" && !v.isBool) { tabSubpop = true; tabSubpopVar = strutil::trim(v.strValue); }
    // BS-097: chi2/sum/summarize/missing/generate/sort are the tabulate/tab1/
    // tab2 options this arm actually READS (with BUG-1514's statistics above).
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kTabulateKnownOpts = kVocabTabulate.known;
    // BUG-371: `missing` graduates from "option not allowed" to implemented — it
    // makes missing an ordinary category instead of a dropped row.
    bool tabMissing = false;
    for (const auto& [k, v] : parsed.opts)
      if (k == "missing" && (!v.isBool || v.boolValue)) tabMissing = true;
    // B5-D-TAB-SORT: `sort` (one-way only; see kVocabTabulate's own comment
    // and runTabulate/tabulateOne's `sortByFreq` param for the implementation
    // and its cited oracle cell).
    bool tabSort = false;
    for (const auto& [k, v] : parsed.opts)
      if (k == "sort" && (!v.isBool || v.boolValue)) tabSort = true;
    // W1a-1: `nolabel` -- "display numeric codes rather than value labels".
    // Read here, applied at the BUG-355 substitution seam below, which is the
    // only place this arm consults session.valueLabels: one flag, one consumer.
    bool tabNoLabel = false;
    for (const auto& [k, v] : parsed.opts)
      if (k == "nolabel" && (!v.isBool || v.boolValue)) tabNoLabel = true;
    // B4-W3b: `generate(stub)`. Read BEFORE the reject net so the net still
    // owns the accept/reject decision (the vocabulary above carries the name).
    std::string tabGenStub;
    for (const auto& [k, v] : parsed.opts)
      if (k == "generate" && !v.isBool) tabGenStub = strutil::trim(v.strValue);
    // BUG-1012: matcell()/matrow()/matcol() -- the frequencies and the row/column
    // values as matrices (tabulate_oneway.sthlp: matcell matrow; tabulate_twoway:
    // + matcol). They are the read-back every display-only option (col, row,
    // nofreq, nolabel, sort) is pinned through in w2as_summary_stats_family.
    std::string tabMatCell, tabMatRow, tabMatCol;
    for (const auto& [k, v] : parsed.opts) {
      if (v.isBool) continue;
      if (k == "matcell") tabMatCell = strutil::trim(v.strValue);
      if (k == "matrow") tabMatRow = strutil::trim(v.strValue);
      if (k == "matcol") tabMatCol = strutil::trim(v.strValue);
    }
    // tab2's syntax is varlist(min=2) (tab2.ado), read before any option: one
    // variable is r(102) "too few variables specified" whatever else is typed
    // (bank: `tab2 rep78`, `tab2 rep78, plot|subvarname|sum|sort`, `tab2 x if ...`).
    if (cmd == "tab2" && parsed.varlist.size() == 1) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "too few variables specified",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              std::string("r(102)"))));
      return true;
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kTabulateKnownOpts,
                                              kVocabTabulate.unimplemented)) {
      return tabRefuse(ojson(*err));
    }
    // `plot` draws a one-way table as its frequencies and a bar of asterisks; a two-way table
    // refuses it before anything else on the line (bank: `tab2 rep78 foreign, plot sort` prints
    // "option plot not allowed" under the first heading, 54ef41c9). With summarize() the table
    // holds means, so no bar is drawn there either: that pairing keeps the named refusal.
    bool tabPlot = false;
    for (const auto& kv : parsed.opts)
      if (kv.first == "plot" && kv.second.isBool && kv.second.boolValue) tabPlot = true;
    if (tabPlot) {
      const bool twoWayLine = cmd == "tab2" || (cmd != "tab1" && parsed.varlist.size() == 2);
      bool withSum = false;
      for (const auto& kv : parsed.opts)
        if ((kv.first == "sum" || kv.first == "summarize") && !kv.second.isBool) withSum = true;
      if (twoWayLine)
        return tabRefuse(ojson(eval::engineError("option `plot` not allowed", "unsupported_option")));
      if (withSum)
        return tabRefuse(ojson(eval::unsupportedOption(
            "plot", "tabulate: plot is not implemented with summarize() in this version of PIET")));
    }
    // `sort` orders a one-way table's rows; a two-way tabulate refuses it (bank:
    // `tabulate rep78 foreign, sort` r(198) "option sort not allowed").
    if ((cmd == "tabulate" || cmd == "tab" || cmd == "ta") && parsed.varlist.size() == 2) {
      for (const auto& kv : parsed.opts) {
        if (kv.first == "sort") {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", "option sort not allowed",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  std::string("r(198)"))));
          return true;
        }
      }
    }
    // tab1 runs one tabulate per variable and leaves its loop at the first one that
    // refuses (tab1.ado captures each): `tab1 rep78 foreign, matcol(c)` prints
    // "option matcol() not allowed" under the first variable's header and ends rc 0
    // with no matrix c (bank: the `matrix list c` after it is r(111)). The one-way
    // tabulate has no matcol().
    if (cmd == "tab1" && !tabMatCol.empty() && !parsed.varlist.empty()) {
      {
        // tab1 leaves its loop at the first refusal: the heading and message of the
        // FIRST variable only (bank: `tab1 rep78 foreign, matcol(c)`).
        ojson head, refusal;
        head["type"] = "display";
        head["text"] = "-> tabulation of " + parsed.varlist[0] + "  ";
        refusal["type"] = "display";
        refusal["text"] = "option matcol() not allowed";
        blocks.push_back(std::move(head));
        blocks.push_back(std::move(refusal));
      }
      eval::replaceRClass(session.eval);
      eval::declareRPosted(session.eval, "tabulate", {});
      return true;
    }
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError("tabulate requires at least one variable")));
      return true;
    }
    // BUG-1012: `tabulate` takes one or two variables; a third is Stata's r(103)
    // "too many variables specified" (sx_tab_ref_three). Before, the third was
    // silently dropped and a two-way table of the first two was answered, rc 0.
    const bool tabPlainCmd = cmd == "tabulate" || cmd == "tab" || cmd == "ta";
    if (tabPlainCmd && parsed.varlist.size() > 2) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "too many variables specified",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              std::string("r(103)"))));
      return true;
    }
    // BUG-1514: Cramér's V is the upper-case option `V`; `tabulate c d, v` is
    // r(198) "option v not allowed" (StataBE 19.5). Option keys arrive lower-cased,
    // so the spelling is read back from the command as typed.
    if (std::find(tabStatWords.begin(), tabStatWords.end(), "V") != tabStatWords.end()) {
      const long long comma = eval::findUnquotedComma(parsed.raw);
      bool upperV = false;
      if (comma >= 0) {
        std::istringstream iss(parsed.raw.substr(static_cast<size_t>(comma) + 1));
        for (std::string w; iss >> w;)
          if (w == "V") upperV = true;
      }
      if (!upperV) return tabRefuse(ojson(eval::engineError("option `v` not allowed", "unsupported_option")));
    }
    // BUG-1514: `nolog` silences exact's enumeration log and is refused without it.
    if (!(tabStats & handlers::kTabExact))
      for (const auto& [k, v] : parsed.opts)
        if (k == "nolog")
          return tabRefuse(ojson(eval::engineError("option `nolog` not allowed", "unsupported_option")));
    // BUG-1514: the statistics are two-way options. One-way, Stata answers r(198)
    // "option lrchi2 not allowed" (likewise gamma, V, all, and chi2 itself, which
    // this arm accepted and ignored; StataBE 19.5 `tabulate c, chi2`). tab1 prints
    // that refusal under its first variable's heading, tabulates nothing, leaves
    // r() empty and ends rc 0 (`tab1 c d, chi2`, `tab1 c d, lrchi2`); this showed
    // both tables with the option ignored and posted the last one's r().
    const bool tabTwoWay = cmd == "tab2" || (cmd != "tab1" && parsed.varlist.size() == 2);
    if (!tabStatWords.empty() && !tabTwoWay) {
      std::string word;
      for (const auto& w : tabStatWords)
        if (w != "chi2" && word.empty()) word = w;
      if (word.empty()) word = "chi2";
      if (cmd == "tab1") {
        ojson head, refusal;
        head["type"] = "display";
        head["text"] = "-> tabulation of " + parsed.varlist[0] + "  ";
        refusal["type"] = "display";
        refusal["text"] = "option " + word + " not allowed";
        blocks.push_back(std::move(head));
        blocks.push_back(std::move(refusal));
        eval::replaceRClass(session.eval);
        eval::declareRPosted(session.eval, "tabulate", {});
        return true;
      }
      blocks.push_back(ojson(eval::engineError("option `" + word + "` not allowed", "unsupported_option")));
      return true;
    }
    // The cell lines are two-way options: a one-way table refuses the first one typed with
    // r(198) "option row not allowed", which tab1 prints under its first heading and ends rc 0
    // (bank: `tab1 rep78 foreign, nofreq row`; `tab1 rep78 foreign, expected cchi2 clrchi2`
    // answers "option expected not allowed", cf904f88, 807a816d).
    if (!tabTwoWay && !sumvar.has_value()) {
      for (const auto& kv : parsed.opts) {
        const std::string& k = kv.first;
        if (k == "row" || k == "col" || k == "cell" || k == "expected" || k == "cchi2" || k == "clrchi2")
          return tabRefuse(ojson(eval::engineError("option `" + k + "` not allowed", "unsupported_option")));
      }
    }
    // With summarize() the table holds means, not counts: no cell line can be drawn from it.
    if (sumvar.has_value()) {
      for (const auto& kv : parsed.opts) {
        const std::string& k = kv.first;
        if (k == "expected" || k == "cchi2" || k == "clrchi2")
          return tabRefuse(ojson(eval::unsupportedOption(
              k, "tabulate: option " + k + " is not implemented with summarize() in this version of PIET")));
      }
    }
    {
      std::string oneWayRefused;
      if (tabTwoWay && tabSubpop) oneWayRefused = "subpop";
      else if (sumvar.has_value() && tabSubpop)
        return tabRefuse(ojson(eval::unsupportedOption(
            "subpop", "tabulate: subpop() is served for plain one-way tables, not with summarize()")));
      else if (!sumvar.has_value()) {
        for (const auto& kv : parsed.opts)
          if (kv.first == "nomeans" || kv.first == "nostandard" || kv.first == "noobs" ||
              kv.first == "means" || kv.first == "standard" || kv.first == "freq" || kv.first == "obs") {
            oneWayRefused = kv.first;
            break;
          }
      }
      if (!oneWayRefused.empty())
        return tabRefuse(ojson(eval::engineError("option `" + oneWayRefused + "` not allowed",
                                                 "unsupported_option")));
    }
    // summarize()'s column options choose which of Mean, Std. dev., Freq. and Obs a table shows;
    // the one-way and the two-way renderers draw the lines the block's `columns` keeps.
    // BUG-1514: the statistics count frequencies. Under [aw=] StataBE 19.5 answers
    // r(406) "lrchi2 not allowed with aweights" (chi2, V, gamma, taub and all alike),
    // under [iw=] r(198) "option lrchi2 not allowed" (chi2 ... all alike).
    if (!tabStatWords.empty() && tabTwoWay && parsed.weightType.has_value()) {
      const std::string wt = strutil::lower(*parsed.weightType);
      const std::string& word = tabStatWords.front();
      if (wt == "aw" || wt == "aweight") {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", word + " not allowed with aweights",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                std::string("r(406)"))));
        return true;
      }
      if (wt == "iw" || wt == "iweight") {
        blocks.push_back(ojson(eval::engineError("option `" + word + "` not allowed", "unsupported_option")));
        return true;
      }
    }
    // matcol() is a two-way option and the matrix options belong to tabulate
    // itself: one-way matcol() is Stata's "option matcol() not allowed", r(198).
    // tab1/tab2 hand the option to every tabulate they run, so each table overwrites
    // the matrix and the LAST table's is what stays (bank: `tab1 rep78 foreign,
    // matcell(m)` leaves m = the foreign counts, `tab2 rep78 foreign, matcol(c)`).
    if (!tabMatCol.empty() && tabPlainCmd && parsed.varlist.size() == 1) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "option matcol() not allowed",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              std::string("r(198)"))));
      return true;
    }
    Datastore* ds = session.datastore();
    // BUG-484: a column this frame does not have used to reach runTabulate,
    // where Columnar::col() throws EngineError("column not found: X") — and
    // that throw crossed the whole dispatch stack to main.cpp:382, which
    // replaced the ENTIRE run's blocks with one `run failed:` error. Not a
    // truncation: everything the run had already produced was discarded, at
    // exit 0, with stopped_at_line unset, and `capture` could not contain it.
    //
    // Nothing exotic is needed — `codebook` a few thousand lines up already
    // does exactly this, and the oracle says tabulate is not a special case:
    // `summarize nosuchcol` and `list nosuchcol` are r(111) in Stata too, and
    // so is `tabulate nosuchcol` (one-way, two-way, and with `, missing`).
    // "variable X not found" is the wording eval::coarseRc maps to 111.
    for (const auto& tvar : parsed.varlist) {
      if (!ds->has(tvar)) {
        blocks.push_back(ojson(eval::engineError("variable " + tvar + " not found")));
        return true;
      }
    }
    if (sumvar.has_value() && !sumvar->empty() && !ds->has(*sumvar)) {
      blocks.push_back(ojson(eval::engineError("variable " + *sumvar + " not found")));
      return true;
    }
    // BUG-042: `tabulate ... if COND` — runTabulate is pure computation over
    // a Datastore reference with no cobra call at all, so there's no
    // `--keepif` to forward; pre-filter into a temp hbin instead and hand
    // THAT Datastore to the (otherwise untouched) handler, mirroring the
    // identical if-prefilter already used for `twoway`/graph commands a few
    // hundred lines up in this same file (dispatcherNextPath + `cobra eval
    // --keepif` + a fresh Datastore over the filtered copy). BS-090 (BETA
    // Phase G): `in` now composes with `if` via AND here too
    // (piet::handlers::applyIfAndInFilter).
    std::unique_ptr<Datastore> tabIfDs;
    if ((parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
        (parsed.inRange.has_value() && !parsed.inRange->empty())) {
      handlers::InRangeFilterResult fr;
      try {
        fr = handlers::applyIfAndInFilter(session, ds->path(), parsed.ifExpr, parsed.inRange);
      } catch (const CobraError& ce) {
        blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
        return true;
      }
      if (!fr.ok) {
        blocks.push_back(ojson(handlers::inRangeRejectError(fr, *parsed.inRange, cmdTyped)));
        return true;
      }
      tabIfDs = std::make_unique<Datastore>(fr.dataPath);
      ds = tabIfDs.get();
    }
    // `parsed.weightVar` survives the tokenizer's guard here as fw/aw/iw on
    // tabulate/tab1 and fw on tab2 (kStataQualifiers answers r(101) for the
    // rest); runTabulate reads the type to choose integer fweight cells or
    // the real-valued aweight/iweight cells. The `if`-prefilter above (when
    // present) already carries the weight column through unchanged since it
    // filters ROWS of the whole dataset, not a chosen subset of columns.
    // BUG-1514: a two-way `nofreq` with nothing else to show -- no cell, col or row
    // line and no statistic -- prints nothing and leaves r() EMPTY, matcell() unmade
    // (StataBE 19.5 `tabulate c d, wrap nofreq`, `, nofreq nokey`, `, nofreq
    // missing`, `, nofreq matcell(mm)` and `tab2 c d e, nofreq`: `return list` is
    // empty, `matrix list mm` r(111)); with `row`, `cell` or a statistic it posts
    // r(N) r(r) r(c) as usual. One-way `nofreq` posts r(N) r(r) (`tabulate c, nofreq`).
    auto tabFlag = [&](const std::string& name) {
      return std::any_of(parsed.opts.begin(), parsed.opts.end(), [&](const auto& opt) {
        return opt.first == name && opt.second.isBool && opt.second.boolValue;
      });
    };
    std::optional<ojson> cellLines;
    if (tabTwoWay && !sumvar.has_value()) {
      const bool nofreq = tabFlag("nofreq");
      ojson requested = ojson::array();
      if (!nofreq) requested.push_back("frequency");
      // Stata stacks the cell lines in this fixed order whatever order they were typed in
      // (bank eb3a65a9: `expected row column cchi2` shows frequency, expected, chi2
      // contribution, row, column; 99a9cf32: `cchi2 clrchi2`).
      for (const char* line : {"expected", "cchi2", "clrchi2", "row", "col", "cell"})
        if (tabFlag(line)) requested.push_back(line);
      if (nofreq && requested.empty() && tabStatWords.empty() && tabGenStub.empty()) {
        eval::replaceRClass(session.eval);
        eval::declareRPosted(session.eval, "tabulate", {});
        return true;
      }
      if (nofreq || requested.size() > 1) cellLines = std::move(requested);
    }
    ojson tabBlock =
        handlers::runTabulate(*ds, parsed.varlist, chi2, cmd, sumvar, parsed.weightVar,
                              tabMissing, tabSort, parsed.weightType, tabStats, tabExactWork,
                              tabFirstOnly, tabSubpopVar);
    if (tabPlot && tabBlock.is_object() && tabBlock.value("type", std::string()) != "error") {
      tabBlock["plot"] = true;
      if (tabBlock.contains("tables"))
        for (auto& panel : tabBlock["tables"]) panel["plot"] = true;
    }
    if (cellLines && tabBlock.is_object() && tabBlock.value("type", std::string()) != "error") {
      const bool noKey = tabFlag("nokey");
      auto setDisplay = [&](ojson& panel) {
        if (!panel.contains("table")) return;
        panel["cell_lines"] = *cellLines;
        if (noKey) panel["no_key"] = true;
      };
      setDisplay(tabBlock);
      if (tabBlock.contains("tables"))
        for (auto& panel : tabBlock["tables"]) setDisplay(panel);
    }
    // B4-W3b: the RAW level keys, snapshotted here — the value-label
    // substitution a few lines down rewrites tabBlock["table"]["rows"] in
    // place, and a dummy built from a LABEL ("Domestic") instead of its code
    // (0) would compare against a string the column does not hold.
    std::vector<std::string> tabRawRows;
    std::optional<ojson> tabRawTable;  // the table before value labels, for the matrices
    // tab1/tab2: the matrices are the last table's (the loop overwrites them).
    if (tabBlock.is_object() && !tabBlock.contains("table") && tabBlock.contains("tables") &&
        tabBlock["tables"].is_array() && !tabBlock["tables"].empty() &&
        tabBlock["tables"].back().is_object() && tabBlock["tables"].back().contains("table"))
      tabRawTable = tabBlock["tables"].back()["table"];
    if (tabBlock.is_object() && tabBlock.contains("table") && tabBlock["table"].is_object()) {
      tabRawTable = tabBlock["table"];
      if (tabBlock["table"].contains("rows") && tabBlock["table"]["rows"].is_array())
        for (const auto& r : tabBlock["table"]["rows"])
          if (r.is_string()) tabRawRows.push_back(r.get<std::string>());
    }
    // BUG-354 (W1 lens L3): `tabulate` is r-class and published nothing —
    // `di r(r)` answered "r(r) is not set". Stata (StataBE 19.5, `tabulate
    // rep78` on auto) sets r(r)=5 (the number of ROWS, i.e. distinct levels
    // shown) and r(N)=69 (the N of the tabulated sample, which EXCLUDES missing
    // — 69 not 74 on that fixture). Both are already in the block this handler
    // just built, so this is carriage at the seam that has them, not a re-count.
    // BUG-355 (W1 lens L4): `tabulate` ignored value labels while the
    // coeftable path applied them from the SAME session store — measured, one
    // session: `regress fatal i.g3` renders `mid.g3`/`high.g3` while
    // `tabulate g3` renders `1.0`/`2.0`/`3.0`. Stata labels the tabulate rows
    // (StataBE 19.5, `label values rep78 replab` -> rows read `three`/`four`
    // while the UNLABELED codes stay numeric). runTabulate takes a Datastore and
    // never sees the session, so the substitution belongs here, at the seam that
    // has both — the same arrangement `applyFactorValueLabels` uses for
    // coefficient rows.
    if (tabBlock.is_object() && tabBlock.contains("table") && tabBlock["table"].is_object() &&
        parsed.varlist.size() == 1 && !session.valueLabels.empty() && !tabNoLabel) {
      auto vlIt = session.valueLabels.find(parsed.varlist[0]);
      if (vlIt != session.valueLabels.end()) {
        auto relabel = [&](const std::string& raw) {
          try {
            const double d = std::stod(raw);
            const double r = std::nearbyint(d);
            // Only an INTEGRAL value can carry a value label; 1.5 has no code.
            if (std::fabs(d - r) > 1e-9) return raw;
            auto it = vlIt->second.find(static_cast<int>(r));
            // An unlabeled code keeps its numeric rendering — Stata's own mix.
            return it == vlIt->second.end() ? raw : it->second;
          } catch (const std::exception&) {
            return raw;  // non-numeric row label (a factor's level string)
          }
        };
        ojson& tbl = tabBlock["table"];
        if (tbl.contains("rows") && tbl["rows"].is_array()) {
          ojson newRows = ojson::array();
          ojson newVals = ojson::object();
          const bool haveVals = tbl.contains("values") && tbl["values"].is_object();
          for (const auto& r : tbl["rows"]) {
            const std::string oldKey = r.get<std::string>();
            const std::string newKey = relabel(oldKey);
            newRows.push_back(newKey);
            if (haveVals && tbl["values"].contains(oldKey)) newVals[newKey] = tbl["values"][oldKey];
          }
          tbl["rows"] = newRows;
          if (haveVals) tbl["values"] = newVals;
        }
      }
    }
    // BUG-373: tab1/tab2 emit `mode:"multi"` with a `tables` ARRAY, so the
    // single-table r-fill below never fired for them and they published nothing.
    // Stata reports the LAST table's counts, which is what a following
    // `display r(r)` reads.
    //
    // r(N) is the tabulated sample's own N, which is why it excludes missing
    // (Stata: 69, not 74, on `tabulate rep78`): the table's grand total,
    // margins.total. BUG-1012 (FU-263): it used to be summed over `values`,
    // whose two-way entries are row OBJECTS, not numbers, so every two-way
    // tabulate posted r(N) = 0 (w2o_rnames N11 N12 N14: Stata 60). Two-way also
    // posts r(c), the column count, and with `chi2` r(chi2) and r(p) (FU-264;
    // tabulate_twoway.sthlp Stored results).
    // BUG-373: tab1/tab2 emit `mode:"multi"` with a `tables` ARRAY; Stata
    // reports the LAST table's counts, which is what a following
    // `display r(r)` reads.
    auto postTabR = [&](const ojson& holder) {
      if (!holder.is_object() || !holder.contains("table") || !holder["table"].is_object()) return;
      const auto& tbl = holder["table"];
      if (tbl.contains("rows") && tbl["rows"].is_array())
        session.eval.rValues.set("r", static_cast<double>(tbl["rows"].size()));
      if (tbl.contains("cols") && tbl["cols"].is_array())
        session.eval.rValues.set("c", static_cast<double>(tbl["cols"].size()));
      if (tbl.contains("margins") && tbl["margins"].is_object() &&
          tbl["margins"].contains("total") && tbl["margins"]["total"].is_number())
        session.eval.rValues.set("N", tbl["margins"]["total"].get<double>());
      if (holder.contains("chi2") && holder["chi2"].is_object()) {
        const auto& c2 = holder["chi2"];
        if (c2.contains("statistic") && c2["statistic"].is_number())
          session.eval.rValues.set("chi2", c2["statistic"].get<double>());
        if (c2.contains("p_value") && c2["p_value"].is_number())
          session.eval.rValues.set("p", c2["p_value"].get<double>());
      }
      // BUG-1514: the other statistics, under Stata's names and in its order
      // (StataBE 19.5 `return list` after `tabulate c d, all`).
      auto setR = [&](const char* name, const ojson& obj, const char* field) {
        if (obj.contains(field) && obj[field].is_number())
          session.eval.rValues.set(name, obj[field].get<double>());
      };
      if (holder.contains("lrchi2") && holder["lrchi2"].is_object()) {
        setR("chi2_lr", holder["lrchi2"], "statistic");
        setR("p_lr", holder["lrchi2"], "p_value");
      }
      if (holder.contains("cramers_v") && holder["cramers_v"].is_number())
        session.eval.rValues.set("CramersV", holder["cramers_v"].get<double>());
      if (holder.contains("gamma") && holder["gamma"].is_object()) {
        setR("gamma", holder["gamma"], "value");
        setR("ase_gam", holder["gamma"], "ase");
      }
      if (holder.contains("taub") && holder["taub"].is_object()) {
        setR("taub", holder["taub"], "value");
        setR("ase_taub", holder["taub"], "ase");
      }
      if (holder.contains("exact") && holder["exact"].is_object()) {
        setR("p_exact", holder["exact"], "p");
        setR("p1_exact", holder["exact"], "p1");
      }
    };
    if (!sumvar.has_value() && tabBlock.is_object() && tabBlock.contains("tables") &&
        tabBlock["tables"].is_array() && !tabBlock["tables"].empty())
      postTabR(tabBlock["tables"].back());
    if (!sumvar.has_value()) postTabR(tabBlock);
    // summarize(): which of Mean, Std. dev., Freq. and Obs the table shows. By default the first
    // three, and Obs too under a weight; naming any of means / standard / freq shows only
    // those named, naming obs ADDS the Obs column to whatever the rest leaves (bank
    // b1a71de16eadd437: `summarize(price) obs` prints Mean, Std. dev., Freq. and Obs, while
    // f613ef85b28c4d6e: `... means` prints Mean alone), and a no-form takes its column
    // away (bank: `summarize(price) nomeans`, `... noobs`). The block
    // carries the list; it is added only when an option or a weight changes the default.
    if (sumvar.has_value() && tabBlock.is_object() && tabBlock.value("type", std::string()) != "error") {
      bool means = true, standard = true, freq = true, obs = parsed.weightVar.has_value();
      bool typed = false, anyPositive = false;
      for (const auto& kv : parsed.opts) {
        const std::string& k = kv.first;
        if (k == "means" || k == "standard" || k == "freq") anyPositive = true;
        if (k == "means" || k == "standard" || k == "freq" || k == "obs" || k == "nomeans" ||
            k == "nostandard" || k == "nofreq" || k == "noobs")
          typed = true;
      }
      if (anyPositive) means = standard = freq = false;
      for (const auto& kv : parsed.opts) {
        const std::string& k = kv.first;
        if (k == "means") means = true;
        else if (k == "standard") standard = true;
        else if (k == "freq") freq = true;
        else if (k == "obs") obs = true;
        else if (k == "nomeans") means = false;
        else if (k == "nostandard") standard = false;
        else if (k == "nofreq") freq = false;
        else if (k == "noobs") obs = false;
      }
      if (typed || parsed.weightVar.has_value()) {
        ojson cols = ojson::array();
        if (means) cols.push_back("means");
        if (standard) cols.push_back("standard");
        if (freq) cols.push_back("freq");
        if (obs) cols.push_back("obs");
        tabBlock["columns"] = cols;
      }
    }
    // BUG-1012: tabulate, summarize() leaves r() EMPTY in Stata (every read of
    // r(N) r(r) r(mean) ... is `.` after it: sx_tab_sum, sx_tab_sum_two). Before,
    // the previous command's r() stayed live and was read back as tabulate's.
    if (sumvar.has_value() && tabBlock.is_object() &&
        tabBlock.value("type", std::string()) != "error") {
      eval::replaceRClass(session.eval);
      eval::declareRPosted(session.eval, "tabulate", {});
    }
    // BUG-1012: the matrices, from the table as built (before value labels; a
    // row key is the value's round-trip rendering, `.`/`.a` for a missing
    // category). matcell = frequencies, matrow = the row values (r x 1), matcol =
    // the column values (1 x c): sx_tab_one_mat, sx_tab_two_mat, sx_tab_sort,
    // sx_tab_missing, sx_tab_m.
    if ((!tabMatCell.empty() || !tabMatRow.empty() || !tabMatCol.empty()) && tabRawTable &&
        tabBlock.value("type", std::string()) != "error") {
      const ojson& tbl = *tabRawTable;
      auto keyValue = [](const std::string& key, double& out) {
        if (!key.empty() && key[0] == '.') {
          if (key.size() == 1) { out = eval::stataMissingWithKind(0); return true; }
          if (key.size() == 2 && key[1] >= 'a' && key[1] <= 'z') {
            out = eval::stataMissingWithKind(key[1] - 'a' + 1);
            return true;
          }
        }
        try {
          size_t pos = 0;
          out = std::stod(key, &pos);
          return pos == key.size();
        } catch (...) {
          return false;
        }
      };
      auto keysOf = [&](const char* field) {
        std::vector<std::string> ks;
        if (tbl.contains(field) && tbl[field].is_array())
          for (const auto& r : tbl[field]) ks.push_back(r.get<std::string>());
        return ks;
      };
      const std::vector<std::string> rowKeys = keysOf("rows");
      const std::vector<std::string> colKeys = keysOf("cols");
      const bool twoWayTbl = tbl.contains("cols");
      auto vectorMatrix = [&](const std::vector<std::string>& ks, bool asRow,
                              const std::string& what, eval::EvalMatrix& m) -> bool {
        std::vector<double> vals;
        for (const auto& k : ks) {
          double v = 0;
          if (!keyValue(k, v)) {
            blocks.push_back(ojson(eval::unsupportedOption(
                what, "tabulate: " + what + "() holds numeric values, and this variable is a string")));
            return false;
          }
          vals.push_back(v);
        }
        if (asRow) {
          m.data = {vals};
          m.rownames = {"r1"};
          for (size_t j = 0; j < vals.size(); ++j) m.colnames.push_back("c" + std::to_string(j + 1));
        } else {
          for (size_t i = 0; i < vals.size(); ++i) {
            m.data.push_back({vals[i]});
            m.rownames.push_back("r" + std::to_string(i + 1));
          }
          m.colnames = {"c1"};
        }
        return true;
      };
      if (!tabMatCell.empty()) {
        eval::EvalMatrix m;
        const ojson& vals = tbl["values"];
        for (size_t i = 0; i < rowKeys.size(); ++i) {
          std::vector<double> row;
          if (twoWayTbl) {
            for (const auto& ck : colKeys) row.push_back(vals[rowKeys[i]][ck].get<double>());
          } else {
            row.push_back(vals[rowKeys[i]].get<double>());
          }
          m.data.push_back(std::move(row));
          m.rownames.push_back("r" + std::to_string(i + 1));
        }
        const size_t nc = twoWayTbl ? colKeys.size() : 1;
        for (size_t j = 0; j < nc; ++j) m.colnames.push_back("c" + std::to_string(j + 1));
        storeUserMatrix(session, tabMatCell, std::move(m));
      }
      if (!tabMatRow.empty()) {
        eval::EvalMatrix m;
        if (!vectorMatrix(rowKeys, false, "matrow", m)) return true;
        storeUserMatrix(session, tabMatRow, std::move(m));
      }
      if (!tabMatCol.empty()) {
        eval::EvalMatrix m;
        if (!vectorMatrix(colKeys, true, "matcol", m)) return true;
        storeUserMatrix(session, tabMatCol, std::move(m));
      }
      if (tbl.contains("cols") && tbl["cols"].is_array()) {
        session.eval.rValues.set("c", static_cast<double>(tbl["cols"].size()));
      }
    }
    // ------------------------------------------------------------------
    // B4-W3b: `tabulate VAR, generate(stub)` — one byte dummy per level,
    // named stub1..stubK in the table's own (sorted) level order.
    //
    // THE RULE THAT DECIDES THIS, and the reason an rc-only test cannot see
    // it: the dummies are MISSING where the source is missing, not 0. Measured
    // (StataBE 19.5, tests/eval/golden/aer_b4/pins_oracle_g.log:170-176,
    // `sysuse auto` + `tabulate rep78, generate(rs)`):
    //     summarize rs1, meanonly  ->  r(N) 69, NOT 74
    //     r(mean) = +1.dae6076b981dbX-006  (= 2/69, the 5 missing rep78 rows
    //                                       are OUT of the denominator)
    // Zero-filling instead would pass every count and every "did the variables
    // appear" check, and would move the coefficient of every regression that
    // uses one of them. So the expression is cond(missing(v), ., v==level) and
    // never a bare comparison.
    if (!tabGenStub.empty()) {
      if (tabTwoWay) {
        // generate() is a one-way option: the two-way tabulate answers r(198)
        // "option gen() not allowed", naming the option as typed (bank: `tab2 rep78
        // foreign, gen(z)` prints it under the first pair's heading and ends rc 0).
        std::string typed = "generate";
        const long long comma = eval::findUnquotedComma(parsed.raw);
        if (comma >= 0) {
          std::istringstream iss(parsed.raw.substr(static_cast<size_t>(comma) + 1));
          for (std::string w; iss >> w;) {
            const size_t paren = w.find('(');
            const std::string word = lower(paren == std::string::npos ? w : w.substr(0, paren));
            if (!word.empty() && std::string("generate").compare(0, word.size(), word) == 0)
              typed = word;
          }
        }
        return tabRefuse(ojson(eval::errorBlock("syntax_error", "option " + typed + "() not allowed",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                std::string("r(198)"))));
      }
      if (cmd == "tab1") {
        // tab1 runs `tabulate v, generate(stub)` per variable: the first makes
        // stub1..stubK, a later one finds stub1 taken and prints the refusal after its
        // own table (bank: `tab1 rep78 foreign, gen(zz)` shows both tables, then
        // "variable zz1 already defined", r() = the second table's, rc 0).
        ojson* tabs = tabBlock.is_object() && tabBlock.contains("tables") && tabBlock["tables"].is_array()
                          ? &tabBlock["tables"] : nullptr;
        for (size_t ti = 0; tabs != nullptr && ti < parsed.varlist.size() && ti < tabs->size(); ++ti) {
          std::vector<std::string> levels;
          const ojson& t = (*tabs)[ti];
          if (t.contains("table") && t["table"].contains("rows") && t["table"]["rows"].is_array())
            for (const auto& r : t["table"]["rows"])
              if (r.is_string()) levels.push_back(r.get<std::string>());
          ojson genErr = runTabulateGenerate(tabGenStub, {parsed.varlist[ti]}, levels, session,
                                             parsed.ifExpr, parsed.inRange, tabMissing);
          if (genErr.is_null()) continue;
          ojson kept = ojson::array();
          for (size_t k = 0; k <= ti && k < tabs->size(); ++k) kept.push_back((*tabs)[k]);
          *tabs = std::move(kept);
          postTabR(tabs->back());
          blocks.push_back(tabBlock);
          ojson refusal;
          refusal["type"] = "display";
          refusal["text"] = genErr.value("message", std::string("generate() failed"));
          blocks.push_back(std::move(refusal));
          eval::declareRPosted(session.eval, "tabulate", {"N", "r"});
          return true;
        }
      } else {
        ojson genErr = runTabulateGenerate(tabGenStub, parsed.varlist, tabRawRows, session,
                                           parsed.ifExpr, parsed.inRange, tabMissing);
        if (!genErr.is_null()) {
          blocks.push_back(genErr);
          return true;
        }
      }
    }
    // The names Stata posts (RULING.md D.3.4): one-way N r
    // (tabulate_oneway's help); two-way N r c, + chi2 p with `chi2`
    // (tabulate_twoway's help). BUG-1514: + chi2_lr p_lr, CramersV, gamma
    // ase_gam, taub ase_taub, p_exact p1_exact with lrchi2, V, gamma, taub, exact,
    // for the statistics the (last) table carries: none on a table with one row or
    // one column. With sum() the set is not declared.
    // A table tab1/tab2 refused ("too many values") ended the loop: the tables before
    // it stay, the refusal prints under its own heading, and r() is empty.
    if (!sumvar.has_value() && tabBlock.is_object() && tabBlock.contains("refused") &&
        tabBlock["refused"].is_object()) {
      const ojson refused = tabBlock["refused"];
      tabBlock.erase("refused");
      if (tabBlock.contains("tables") && tabBlock["tables"].is_array() && !tabBlock["tables"].empty())
        blocks.push_back(ojson(tabBlock));
      ojson head, refusal;
      head["type"] = "display";
      head["text"] = refused.value("header", std::string());
      refusal["type"] = "display";
      refusal["text"] = refused.value("message", std::string());
      blocks.push_back(std::move(head));
      blocks.push_back(std::move(refusal));
      eval::replaceRClass(session.eval);
      eval::declareRPosted(session.eval, "tabulate", {});
      return true;
    }
    if (!sumvar.has_value() && tabBlock.is_object() &&
        tabBlock.value("type", std::string()) != "error") {
      const bool twoWay = cmd == "tab2" || (cmd != "tab1" && parsed.varlist.size() == 2);
      std::vector<std::string> stataNames = {"N", "r"};
      if (twoWay) {
        stataNames.push_back("c");
        const ojson& last = tabBlock.contains("tables") && tabBlock["tables"].is_array() &&
                                    !tabBlock["tables"].empty()
                                ? tabBlock["tables"].back()
                                : tabBlock;
        if (last.contains("chi2")) stataNames.insert(stataNames.end(), {"chi2", "p"});
        if (last.contains("lrchi2")) stataNames.insert(stataNames.end(), {"chi2_lr", "p_lr"});
        if (last.contains("cramers_v")) stataNames.push_back("CramersV");
        if (last.contains("gamma")) stataNames.insert(stataNames.end(), {"gamma", "ase_gam"});
        if (last.contains("taub")) stataNames.insert(stataNames.end(), {"taub", "ase_taub"});
        if (last.contains("exact")) stataNames.insert(stataNames.end(), {"p_exact", "p1_exact"});
      }
      // Stata lists them `N r c chi2 p ... CramersV gamma ase_gam taub ase_taub`
      // (vs-stata truth bank, `: r(scalars)` after tabulate).
      eval::orderRResults(session.eval,
                          {"N", "r", "c", "chi2", "p", "chi2_lr", "p_lr", "CramersV", "gamma",
                           "ase_gam", "taub", "ase_taub", "p_exact", "p1_exact"});
      eval::declareRPosted(session.eval, "tabulate", std::move(stataNames));
    }
    // `exact` on a table too large to enumerate: Stata prints the table and the statistics it
    // already has, then ends r(910) with r() posted (bank: `ta industry race, exact`, N r c
    // stay set). tab2 swallows the refusal like any other the inner tabulate raises.
    {
      ojson* lastTab = tabBlock.is_object() && tabBlock.contains("tables") && tabBlock["tables"].is_array() &&
                               !tabBlock["tables"].empty()
                           ? &tabBlock["tables"].back() : &tabBlock;
      if (lastTab->is_object() && lastTab->contains("exact_refused")) {
        const int64_t ws = (*lastTab)["exact_refused"].get<int64_t>();
        lastTab->erase("exact_refused");
        const std::string msg = "exceeded memory limits using exact(" + std::to_string(ws) +
                                "); try again with larger #; see help tabulate for details";
        blocks.push_back(ojson(tabBlock));
        if (tabLoop) {
          ojson refusal;
          refusal["type"] = "display";
          refusal["text"] = msg;
          blocks.push_back(std::move(refusal));
        } else {
          blocks.push_back(ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                                  std::nullopt, std::string("r(910)"))));
        }
        return true;
      }
    }
    blocks.push_back(ojson(tabBlock));
    return true;
  }


  // `collect` (Stata 17 collections). PIET keeps no collection store; what is served
  // is collect export's file-type rule. With no as(), the document type comes from
  // the file's suffix ([TABLES] collect export: docx html pdf xlsx xls tex smcl txt
  // markdown md), and any other suffix is r(198) "option as() required; could not
  // determine filetype from filename" (vs-stata bank 2ad86a0b64ba1d4f `collect export
  // "t.csv", name(a) replace`). Writing the document, and every other collect
  // subcommand, is refused by name.
  if (cmd == "collect") {
    const std::string args = rawArgsAfterCmd(parsed.raw);
    const size_t s0 = args.find_first_not_of(" \t");
    const size_t s1 = s0 == std::string::npos ? std::string::npos : args.find_first_of(" \t,", s0);
    const std::string sub = s0 == std::string::npos
                                ? std::string()
                                : args.substr(s0, s1 == std::string::npos ? std::string::npos : s1 - s0);
    if (sub == "export") {
      const std::string rest = s1 == std::string::npos ? std::string() : args.substr(s1);
      const size_t f0 = rest.find_first_not_of(" \t");
      std::string fname, opts;
      if (f0 != std::string::npos && rest[f0] == '"') {
        const size_t q = rest.find('"', f0 + 1);
        fname = rest.substr(f0 + 1, (q == std::string::npos ? rest.size() : q) - f0 - 1);
        opts = q == std::string::npos ? std::string() : rest.substr(q + 1);
      } else if (f0 != std::string::npos && rest[f0] != ',') {
        const size_t q = rest.find_first_of(" \t,", f0);
        fname = rest.substr(f0, q == std::string::npos ? std::string::npos : q - f0);
        opts = q == std::string::npos ? std::string() : rest.substr(q);
      }
      bool hasAs = false;
      const size_t comma = opts.find(',');
      for (size_t k = comma == std::string::npos ? opts.size() : comma; k + 1 < opts.size(); ++k) {
        if (opts.compare(k, 2, "as") != 0) continue;
        const char before = k == 0 ? ' ' : opts[k - 1];
        if (!(before == ' ' || before == '\t' || before == ',')) continue;
        size_t p = k + 2;
        while (p < opts.size() && (opts[p] == ' ' || opts[p] == '\t')) ++p;
        if (p < opts.size() && opts[p] == '(') hasAs = true;
      }
      std::string suffix;
      const size_t dot = fname.find_last_of('.');
      const size_t slash = fname.find_last_of("/\\");
      if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        suffix = fname.substr(dot + 1);
      static const std::set<std::string> kSuffixTypes = {"docx", "html", "pdf",  "xlsx",     "xls",
                                                         "tex",  "smcl", "txt", "markdown", "md"};
      if (!fname.empty() && !hasAs && kSuffixTypes.count(suffix) == 0) {
        blocks.push_back(ojson(eval::errorBlock(
            "syntax_error", "option as() required; could not determine filetype from filename",
            std::nullopt, std::nullopt, std::nullopt, "r(198)")));
        return true;
      }
    }
    blocks.push_back(ojson(eval::refusedCommand("collect", "no_collection_store")));
    return true;
  }

  if (cmd == "table") {
    if (!session.hasData()) {
      blocks.push_back(ojson(eval::noDataLoaded()));
      return true;
    }
    // Stata reads the options before it looks at the dimensions: `table, zzabbrprobe` is r(198)
    // "option zzabbrprobe not allowed" (bank).
    if (parsed.varlist.empty()) {
      if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabTable.known, kVocabTable.unimplemented)) {
        blocks.push_back(ojson(*err));
        return true;
      }
    }
    // Stata 17 spells the dimensions `table (rowspec) (colspec)`; the tokenizer hands them over
    // with the parentheses attached (`(foreign`, `rep78)`, `()`). The groups are read back from the
    // tokens: a dimension is one variable (nested dimensions, a `var` / `command` / `result`
    // dimension in the row position and the like are tables this build does not draw), the
    // pre-17 spelling `table rowvar [colvar]` has no parentheses. `(result)` in the column
    // position is the statistics' own dimension, which is where this table puts them.
    std::vector<std::vector<std::string>> dimGroups;
    {
      std::string joined;
      for (const auto& t : parsed.varlist) joined += (joined.empty() ? "" : " ") + t;
      if (joined.find('(') != std::string::npos) {
        size_t i = 0;
        while (i < joined.size()) {
          if (std::isspace(static_cast<unsigned char>(joined[i]))) { ++i; continue; }
          if (joined[i] != '(') {
            blocks.push_back(ojson(eval::engineError("table: dimension list `" + joined + "` is not understood")));
            return true;
          }
          const size_t close = joined.find(')', i);
          if (close == std::string::npos) {
            blocks.push_back(ojson(eval::engineError("table: unbalanced parentheses in the dimension list")));
            return true;
          }
          std::vector<std::string> group;
          std::istringstream gs(joined.substr(i + 1, close - i - 1));
          for (std::string w; gs >> w;) group.push_back(w);
          dimGroups.push_back(std::move(group));
          i = close + 1;
        }
      } else {
        for (const auto& t : parsed.varlist) dimGroups.push_back({t});
      }
    }
    // A third dimension is a page variable: one table per level of it (a third `(result)`
    // dimension puts each statistic on a page of its own).
    std::string pageVarName;
    bool pageByStatistic = false;
    if (dimGroups.size() == 3 && dimGroups[2].size() == 1) {
      if (dimGroups[2][0] == "result") pageByStatistic = true;
      else if (dimGroups[2][0] != "var" && dimGroups[2][0] != "command") pageVarName = dimGroups[2][0];
      if (pageByStatistic || !pageVarName.empty()) dimGroups.pop_back();
    }
    if (dimGroups.size() > 2 ||
        (dimGroups.size() == 2 && dimGroups[1].size() == 1 && dimGroups[1][0] == "result" && dimGroups[0].size() > 1)) {
      blocks.push_back(ojson(eval::unsupportedOption(
          "table", "table: tables with more than two dimensions are not drawn by this build")));
      return true;
    }
    for (size_t gi = 0; gi < dimGroups.size(); ++gi) {
      const auto& g = dimGroups[gi];
      const bool special = std::any_of(g.begin(), g.end(), [](const std::string& w) {
        return w == "result" || w == "var" || w == "command";
      });
      if (g.size() > 1 || (special && !(gi == 1 && g.size() == 1 && g[0] == "result"))) {
        blocks.push_back(ojson(eval::unsupportedOption(
            "table", "table: nested dimensions and the var / command / result dimensions are not "
                     "drawn by this build")));
        return true;
      }
    }
    std::string rowvar = !dimGroups.empty() && !dimGroups[0].empty() ? dimGroups[0][0] : std::string();
    std::optional<std::string> colvar;
    if (dimGroups.size() >= 2 && !dimGroups[1].empty() && dimGroups[1][0] != "result") colvar = dimGroups[1][0];
    // A column dimension without a row dimension (`table () (rep78)`): the one row is the Total.
    // `statistic()` REPEATS in Stata 17 (`statistic(mean y) statistic(count y)`).
    // The old loop overwrote its single slot each pass, so a two-statistic
    // request silently answered with the last one only — the caller asked two
    // questions and got one answer with no indication the other was dropped.
    struct StatSpec { std::string stat; std::optional<std::string> var; };
    std::vector<StatSpec> statSpecs;
    // BUG-1015: the pre-17 `contents(stat var ...)` (c()) form, legal only under
    // `version 16:` or lower (table_16.sthlp; w2as sx_table_c_v16*). Its
    // statistics map onto the same cells statistic() computes (median, p#, iqr and rawsum
    // included), and a word that is no statistic is r(198)
    // (sx_table_ref_stat). The form is output-only: it posts no r().
    const bool tableV16 = session.callerVersion > 0.0 && session.callerVersion < 17.0;
    bool tableContents = false;
    for (const auto& [k, v] : parsed.opts) {
      if (!((k == "contents" || k == "c") && !v.isBool)) continue;
      if (!tableV16) {
        blocks.push_back(ojson(eval::errorBlock(
            "unsupported_option",
            "option contents() not allowed since Stata 17; see help table for updated syntax")));
        return true;
      }
      tableContents = true;
      std::istringstream iss(v.strValue);
      std::vector<std::string> words;
      std::string w;
      while (iss >> w) words.push_back(w);
      static const std::unordered_set<std::string> kServed = {"freq", "mean", "sd", "sum",
                                                              "count", "n", "max", "min"};
      for (size_t i = 0; i < words.size();) {
        const std::string st = lower(words[i]);
        const bool known = kServed.count(st) || st == "rawsum" || st == "median" || st == "iqr" ||
                           (st.size() >= 2 && st[0] == 'p' &&
                            st.find_first_not_of("0123456789", 1) == std::string::npos);
        if (!known) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", words[i] + " invalid statistic",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  std::string("r(198)"))));
          return true;
        }
        if (st == "freq") {
          statSpecs.push_back({"freq", std::nullopt});
          ++i;
        } else {
          if (i + 1 >= words.size()) {
            blocks.push_back(ojson(eval::errorBlock("syntax_error", "varlist required",
                                                    std::nullopt, std::nullopt, std::nullopt,
                                                    std::string("r(100)"))));
            return true;
          }
          statSpecs.push_back({st, words[i + 1]});
          i += 2;
        }
      }
    }
    for (const auto& [k, v] : parsed.opts) {
      if ((k == "statistic" || k == "stat") && !v.isBool) {
        std::istringstream iss(v.strValue);
        std::vector<std::string> parts;
        std::string tok;
        while (iss >> tok) parts.push_back(tok);
        if (!parts.empty()) {
          StatSpec sp;
          sp.stat = lower(parts[0]);
          // statistic(percent, across(v) | total): `total` is the default denominator (the whole
          // table); across(v) divides by the same cell with v collapsed (bank: `table rep78
          // foreign, statistic(percent, across(foreign))` gives 100.00 along each row). The
          // across variable rides on the statistic's name as `percent@v`.
          {
            const size_t comma = v.strValue.find(',');
            if (comma != std::string::npos) {
              std::string head = strutil::trim(v.strValue.substr(0, comma));
              std::string tail = strutil::trim(v.strValue.substr(comma + 1));
              std::istringstream hs(head);
              parts.clear();
              for (std::string w; hs >> w;) parts.push_back(w);
              sp.stat = lower(parts.empty() ? std::string() : parts[0]);
              if (tail.compare(0, 7, "across(") == 0 && !tail.empty() && tail.back() == ')') {
                const std::string av = strutil::trim(tail.substr(7, tail.size() - 8));
                if (av.empty() || av.find(' ') != std::string::npos ||
                    (sp.stat != "percent" && sp.stat != "proportion")) {
                  blocks.push_back(ojson(eval::unsupportedOption(
                      "statistic", "table: across() is served for statistic(percent) and statistic(proportion) "
                                   "with one variable")));
                  return true;
                }
                sp.stat += "@" + av;
              } else if (tail != "total") {
                blocks.push_back(ojson(eval::unsupportedOption(
                    "statistic", "table: statistic(..., " + tail + ") is not implemented in this version of PIET")));
                return true;
              }
            }
          }
          if (parts.size() >= 2) sp.var = parts[1];
          statSpecs.push_back(std::move(sp));
        }
      }
    }
    if (statSpecs.empty()) statSpecs.push_back({"freq", std::nullopt});
    // BS-097: statistic/stat are the only `table` options this arm reads.
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kTableKnownOpts = kVocabTable.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kTableKnownOpts, kVocabTable.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    Datastore* ds = session.datastore();
    for (const auto& sp : statSpecs)
      if (sp.var.has_value() && !ds->has(*sp.var)) {
        blocks.push_back(ojson(eval::engineError("variable " + *sp.var + " not found")));
        return true;
      }
    // The options that shape the result. `missing` makes the dimension variables' missing values
    // categories (and keeps those rows in r(N)); `nototals` and `totals(v)` choose the margins
    // (totals(v) names the dimension a margin is drawn for: under the column variable the Total
    // row, under the row variable the Total column; bank: `table foreign rep78, ... tot(foreign)`
    // draws the Total column); `listwise` keeps only the rows where every statistic's variable
    // is present; `zerocounts` reports 0 in an empty cell of a count or frequency.
    handlers::TableLayout lay;
    bool optNoTotals = false, optListwise = false, legacyRow = false, legacyColumn = false;
    std::vector<std::string> totalsVars;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "missing") lay.includeMissing = true;
      else if (k == "nototals") optNoTotals = true;
      else if (k == "totals" && !v.isBool) {
        std::istringstream ts(v.strValue);
        for (std::string w; ts >> w;) totalsVars.push_back(w);
      } else if (k == "listwise") optListwise = true;
      else if (k == "row") legacyRow = true;
      else if (k == "col" || k == "column") legacyColumn = true;
      else if (k == "zerocounts") lay.zeroCounts = true;
    }
    if ((legacyRow || legacyColumn) && !tableV16) {
      blocks.push_back(ojson(eval::engineError(std::string("option `") + (legacyRow ? "row" : "col") +
                                               "` not allowed", "unsupported_option")));
      return true;
    }
    if (tableContents) {
      // The pre-17 form draws margins only when asked: `row` the Total row, `column` the Total column.
      lay.showRowTotal = legacyRow;
      lay.showColTotal = legacyColumn;
    } else {
      lay.showRowTotal = !optNoTotals;
      lay.showColTotal = !optNoTotals;
      if (!totalsVars.empty() && !optNoTotals) {
        const auto has = [&](const std::string& v) {
          return std::find(totalsVars.begin(), totalsVars.end(), v) != totalsVars.end();
        };
        lay.showRowTotal = colvar.has_value() ? has(*colvar) : false;
        lay.showColTotal = !rowvar.empty() && has(rowvar);
      }
    }
    if (parsed.weightVar.has_value() && parsed.weightType.has_value()) {
      lay.weightVar = *parsed.weightVar;
      const std::string wt = lower(*parsed.weightType);
      lay.weightType = (wt == "fw" || wt == "fweight") ? "fw" : (wt == "aw" || wt == "aweight") ? "aw"
                       : (wt == "pw" || wt == "pweight") ? "pw" : "iw";
    }
    // rows outside `if` / `in` / listwise are not in the table (Stata's `marksample`): one
    // prefilter through the shared if-and-in chain, which also carries time-series operators.
    std::optional<std::string> tableIf = parsed.ifExpr;
    if (optListwise) {
      std::string present;
      for (const auto& sp : statSpecs)
        if (sp.var.has_value()) present += (present.empty() ? "" : ",") + *sp.var;
      if (!present.empty()) {
        const std::string cond = "!missing(" + present + ")";
        tableIf = tableIf.has_value() && !tableIf->empty() ? "(" + *tableIf + ") & " + cond : cond;
      }
    }
    std::unique_ptr<Datastore> tableIfDs;
    if ((tableIf.has_value() && !tableIf->empty()) || (parsed.inRange.has_value() && !parsed.inRange->empty())) {
      handlers::InRangeFilterResult fr;
      try {
        fr = handlers::applyIfAndInFilter(session, ds->path(), tableIf, parsed.inRange);
      } catch (const CobraError& ce) {
        blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
        return true;
      }
      if (!fr.ok) {
        blocks.push_back(ojson(handlers::inRangeRejectError(fr, *parsed.inRange, cmdTyped)));
        return true;
      }
      tableIfDs = std::make_unique<Datastore>(fr.dataPath);
      ds = tableIfDs.get();
      if (ds->nRows() == 0) {  // bank: `table foreign if price>999999` r(2000)
        blocks.push_back(ojson(eval::errorBlock("engine_error", "no observations", std::nullopt,
                                                std::nullopt, std::nullopt, std::string("r(2000)"))));
        return true;
      }
    }
    // BUG-160 CLOSURE: ONE table, not one runTable() call (and one block) per
    // statistic. The single- and two-or-more-statistic requests share the
    // SAME underlying computation (runTableMulti, tabulate.cpp) — Total row/
    // column margins and value-label-aware headers, absent from both paths
    // before this fix — but keep their pre-existing JSON SHAPES: a lone
    // statistic() still renders through runTable's flat `stat`/`cells[row]=
    // value` reshape (every existing one-statistic consumer keeps reading
    // that shape, now with a "Total" entry and real labels), while 2+
    // statistics render through runTableMulti's own nested `stats[]`/
    // `cells[row][stat]=...` shape (previously TWO separate table_result
    // blocks with no shape at all to preserve).
    std::vector<std::pair<std::string, std::optional<std::string>>> multiSpecs;
    multiSpecs.reserve(statSpecs.size());
    for (const auto& sp : statSpecs) multiSpecs.emplace_back(sp.stat, sp.var);
    // Pages: one table per level of the third variable, or one per statistic for a third
    // (result) dimension. r(N) is the rows in the table, the same on every page.
    if (!pageVarName.empty() || (pageByStatistic && multiSpecs.size() > 1)) {
      std::vector<ojson> pages;
      if (!pageVarName.empty()) {
        if (!ds->has(pageVarName)) {
          blocks.push_back(ojson(eval::engineError("variable " + pageVarName + " not found")));
          return true;
        }
        lay.pageVar = pageVarName;
        ojson probe = ojson(handlers::runTableMulti(*ds, rowvar, colvar, multiSpecs, &session, &lay));
        if (probe.value("type", std::string()) == "error") {
          blocks.push_back(std::move(probe));
          return true;
        }
        const ojson levels = probe.contains("page_levels") ? probe["page_levels"] : ojson::array();
        for (const auto& lv : levels) {
          lay.pageLevel = lv["value"].get<double>();
          ojson page = (multiSpecs.size() == 1)
                           ? ojson(handlers::runTable(*ds, rowvar, colvar, multiSpecs[0].first,
                                                      multiSpecs[0].second, &session, &lay))
                           : ojson(handlers::runTableMulti(*ds, rowvar, colvar, multiSpecs, &session, &lay));
          page.erase("page_levels");
          page["page"] = ojson{{"var", pageVarName}, {"label", lv["label"]}};
          pages.push_back(std::move(page));
        }
        if (pages.empty()) pages.push_back(std::move(probe));
      } else {
        for (const auto& sp : multiSpecs) {
          ojson page = ojson(handlers::runTable(*ds, rowvar, colvar, sp.first, sp.second, &session, &lay));
          page["page"] = ojson{{"var", "result"}, {"label", sp.first}};
          pages.push_back(std::move(page));
        }
      }
      if (pages.front().value("type", std::string()) == "error") {
        blocks.push_back(std::move(pages.front()));
        return true;
      }
      if (pages.front().contains("n_obs") && pages.front()["n_obs"].is_number()) {
        eval::replaceRClass(session.eval);
        session.eval.rValues.set("N", pages.front()["n_obs"].get<double>());
        eval::declareRPosted(session.eval, "table", {"N"});
      }
      for (auto& pg : pages) blocks.push_back(std::move(pg));
      return true;
    }
    ojson tableBlk = (multiSpecs.size() == 1)
                         ? ojson(handlers::runTable(*ds, rowvar, colvar, multiSpecs[0].first,
                                                    multiSpecs[0].second, &session, &lay))
                         : ojson(handlers::runTableMulti(*ds, rowvar, colvar, multiSpecs,
                                                         &session, &lay));
    // BUG-373: table is r-class in Stata and published nothing. r(N) is the
    // tabulated sample's own count, which the block already carries.
    // BUG-1015: and r(N) is ALL it posts (table.sthlp Stored results): r() is
    // replaced and declared, so r(mean) after it reads `.` (sx_table_one).
    if (tableBlk.value("type", std::string()) != "error" &&
        tableBlk.contains("n_obs") && tableBlk["n_obs"].is_number()) {
      eval::replaceRClass(session.eval);
      session.eval.rValues.set("N", tableBlk["n_obs"].get<double>());
      eval::declareRPosted(session.eval, "table", {"N"});
    }
    blocks.push_back(std::move(tableBlk));
    return true;
  }


  // ------------------------------------------------------------------
  // M4a: output-completion surface (ROADMAP M4a item 2 — estimation surface)

  if (cmd == "tabstat") {
    // A refused tabstat leaves r() empty. That is judged after the line, by
    // rTableAfterCommand's leaf rule (postestimation/rtable_rclass_lifecycle.cpp),
    // for every tabstat
    // that ended in an error, the refusals raised before this arm included
    // (BUG-1151).
    auto failed = [&](ojson blk) {
      blocks.push_back(std::move(blk));
      return blocks;
    };
    if (parsed.varlist.empty())
      { blocks = failed(ojson(eval::engineError("tabstat requires at least one variable"))); return true; }
    // A dataset with no observations: r(2000) "no observations" (vs-stata bank
    // 4a70f3831ec105d2, `set obs 0` then `tabstat x`).
    int64_t tsRows = -1;
    if (std::optional<int64_t> r = session.residentRowsIfCurrent()) tsRows = *r;
    else if (session.hasData())
      if (Datastore* tds = session.datastore()) tsRows = tds->nRows();
    if (session.hasData() && tsRows == 0) {
      blocks = failed(ojson(eval::errorBlock("engine_error", "no observations", std::nullopt,
                                             std::nullopt, std::nullopt, "r(2000)")));
      return true;
    }
    // BUG-1014: tabstat as tabstat.ado (StataNow 19.5, v2.9.6) runs it -- every
    // statistic is `summarize` on the group's rows, the statistic list is parsed
    // by its Stats subprogram, and r() is replaced by the command whether or not
    // `save` is given (w2as_summary_stats_family sx_tabstat_*).
    std::optional<std::string> byVal, statsVal, statisticsVal, columnsVal;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "by" && !v.isBool) byVal = strutil::trim(v.strValue);
      // `stat`/`s` are abbreviations of statistics() (ado: Statistics(str));
      // `stats()` is its own synonym option (ado: STATS(str)).
      // `s` and `c` take an argument: bare, tabstat.ado's syntax reads them as the
      // abbreviations of Statistics(str) / Columns(str) and refuses (bank:
      // `tabstat price mpg, by(foreign) s`, `tabstat price mpg rep78, c`).
      if ((k == "stat" || k == "statistics" || k == "s") && !v.isBool) statisticsVal = v.strValue;
      if (k == "stats" && !v.isBool) statsVal = v.strValue;
      if ((k == "columns" || k == "c") && !v.isBool) columnsVal = strutil::trim(v.strValue);
    }
    // A bare `s` or `c` is read as typed (the registry resolves a one-letter word
    // to some option): tabstat.ado's Statistics(str) and Columns(str) need an
    // argument, so Stata answers r(198) "option s incorrectly specified" (bank:
    // `tabstat price mpg, by(foreign) s`, `tabstat price mpg rep78, c`).
    {
      const long long comma = eval::findUnquotedComma(parsed.raw);
      if (comma >= 0) {
        // Only a word standing alone at the top level counts: one followed at once by
        // "(" is an option WITH its argument (`s(mean sd)`, `c( s )` -- Stata strips the
        // blanks around a string option's argument), and what is inside the
        // parentheses is that argument.
        const std::string optText = parsed.raw.substr(static_cast<size_t>(comma) + 1);
        std::string topLevel;
        int depth = 0;
        for (size_t i = 0; i < optText.size(); ++i) {
          const char ch = optText[i];
          if (ch == '(') {
            // The word before it names an option that has an argument; blanks may
            // stand between the two (bank b2a03c7e: `tabstat x1, stat (mean) save`).
            if (depth == 0) {
              while (!topLevel.empty() && topLevel.back() == ' ') topLevel.pop_back();
              while (!topLevel.empty() && topLevel.back() != ' ') topLevel.pop_back();
            }
            ++depth;
          } else if (ch == ')') {
            if (depth > 0) --depth;
            if (depth == 0) topLevel += ' ';
          } else if (depth == 0) {
            topLevel += std::isspace(static_cast<unsigned char>(ch)) ? ' ' : ch;
          }
        }
        std::istringstream iss(topLevel);
        for (std::string w; iss >> w;) {
          if (w == "s" || w == "c") {
            blocks = failed(ojson(eval::errorBlock("syntax_error", "option " + w + " incorrectly specified",
                                                   std::nullopt, std::nullopt, std::nullopt,
                                                   std::string("r(198)"))));
            return true;
          }
        }
      }
    }
    const std::unordered_set<std::string>& kTabstatKnownOpts = kVocabTabstat.known;
    bool tsNoTotal = false, tsSave = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "nototal" && (!v.isBool || v.boolValue)) tsNoTotal = true;
      if (k == "save" && (!v.isBool || v.boolValue)) tsSave = true;
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kTabstatKnownOpts))
      { blocks = failed(ojson(*err)); return true; }
    auto refuse = [&](const std::string& msg, const char* rc) {
      return failed(ojson(eval::errorBlock("syntax_error", msg, std::nullopt, std::nullopt,
                                           std::nullopt, std::string(rc))));
    };
    if (statsVal.has_value() && statisticsVal.has_value())
      { blocks = refuse("options statistics() and stats() may not be specified together", "r(198)"); return true; }
    if (statsVal.has_value()) statisticsVal = statsVal;
    // columns(): a prefix of `variables` or `statistics`, or `stats` (ado). Both
    // layouts leave r(Stat#) statistics-by-variables (sx_tabstat_001, _c_v).
    if (columnsVal.has_value() && !columnsVal->empty()) {
      const std::string c = *columnsVal;
      const bool okCol = std::string("variables").compare(0, c.size(), c) == 0 ||
                         std::string("statistics").compare(0, c.size(), c) == 0 || c == "stats";
      if (!okCol)
        { blocks = refuse("option columns(" + c + ") invalid -- specify columns(variables) or "
                      "columns(statistics)", "r(198)"); return true; }
    }
    // syntax varlist(numeric): a string variable is r(109) (sx_tabstat_ref_str).
    if (Datastore* tds = session.datastore()) {
      for (const auto& v : parsed.varlist)
        if (tds->has(v) && tds->dtype(v) == "factor")
          { blocks = refuse("string variables not allowed in varlist; " + v + " is a string variable",
                        "r(109)"); return true; }
    }
    // The Stats subprogram (parseTabstatStats).
    std::vector<std::string> statKeys, statRows;
    if (const auto bad = parseTabstatStats(statisticsVal, statKeys, statRows))
      { blocks = refuse("unknown statistic: " + *bad, "r(198)"); return true; }
    std::string statCsv;
    for (const auto& k : statKeys) statCsv += (statCsv.empty() ? "" : " ") + k;
    // Weights: [aw=] and [fw=] (ado: [aw fw]).
    std::string weightFlag;
    if (parsed.weightType.has_value() && parsed.weightVar.has_value()) {
      const std::string wt = lower(*parsed.weightType);
      weightFlag = (wt == "aw" || wt == "aweight") ? "--aweight" : "--fweight";
    }
    // casewise (same): only the rows where every variable of the list is present.
    std::optional<std::string> tsIf = parsed.ifExpr;
    bool tsMissing = false;
    {
      bool casewise = false;
      for (const auto& [k, v] : parsed.opts) {
        if ((k == "casewise" || k == "same") && (!v.isBool || v.boolValue)) casewise = true;
        if (k == "missing" && (!v.isBool || v.boolValue)) tsMissing = true;
      }
      if (casewise) {
        std::string present = "!missing(";
        for (size_t i = 0; i < parsed.varlist.size(); ++i)
          present += (i == 0 ? "" : ",") + parsed.varlist[i];
        present += ")";
        tsIf = parsed.ifExpr.has_value() && !parsed.ifExpr->empty()
                   ? "(" + *parsed.ifExpr + ") & " + present
                   : present;
      }
    }
    const auto callTabstat = [&](const std::optional<std::string>& by, const std::optional<std::string>& cond,
                                 bool groups, bool noTotal) {
      return weightFlag.empty()
                 ? ojson(handlers::runTabstat(parsed.varlist, by, statCsv, session, cond, groups, noTotal,
                                              parsed.inRange))
                 : ojson(handlers::runTabstatWeighted(parsed.varlist, by, statCsv, session, cond, groups,
                                                      noTotal, weightFlag, *parsed.weightVar,
                                                      parsed.inRange));
    };
    ojson tsBlock = callTabstat(byVal, tsIf, /*groups=*/true, tsNoTotal);
    if (tsBlock.value("type", std::string()) == "error") { blocks = failed(tsBlock); return true; }
    // missing: the by variable's missing values are groups of their own. A numeric missing
    // sorts after every number (`.` before `.a` ... `.z`), a string's "" before every text;
    // the Total row then covers every selected row, the missing ones included (bank:
    // `tabstat price mpg weight, by(rep78) missing` shows the `.` group and Total over 74).
    if (tsMissing && byVal.has_value() && !byVal->empty() && tsBlock.contains("groups")) {
      Datastore* msds = session.datastore();
      std::set<int> kinds;
      bool strBy = false, strMissing = false;
      if (msds != nullptr && msds->has(*byVal)) {
        strBy = msds->dtype(*byVal) == "factor";
        std::unique_ptr<Datastore> filtered;
        Datastore* src = msds;
        if ((tsIf.has_value() && !tsIf->empty()) || (parsed.inRange.has_value() && !parsed.inRange->empty())) {
          handlers::InRangeFilterResult fr;
          try {
            fr = handlers::applyIfAndInFilter(session, msds->path(), tsIf, parsed.inRange);
          } catch (const CobraError& ce) {
            blocks = failed(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
            return true;
          }
          if (fr.ok) {
            filtered = std::make_unique<Datastore>(fr.dataPath);
            src = filtered.get();
          }
        }
        const cecon::Column& bc = src->col(*byVal);
        const int64_t nrows = src->nRows();
        if (strBy) {
          const int32_t* codes = bc.codes();
          for (int64_t i = 0; i < nrows; ++i)
            if (codes[i] < 0 || bc.levels[static_cast<size_t>(codes[i])].empty()) strMissing = true;
        } else {
          const double* d = bc.f64();
          for (int64_t i = 0; i < nrows; ++i)
            if (std::isnan(d[i])) {
              const int kind = cecon::missing_kind(d[i]);
              kinds.insert(kind < 0 ? 0 : kind);
            }
        }
      }
      auto andIf = [&](const std::string& cond) {
        return tsIf.has_value() && !tsIf->empty() ? "(" + *tsIf + ") & " + cond : cond;
      };
      ojson extra = ojson::array();
      if (strMissing) {
        ojson all = callTabstat(std::nullopt, andIf(*byVal + "==\"\""), false, false);
        if (all.value("type", std::string()) == "error") { blocks = failed(all); return true; }
        extra.push_back(ojson{{"stats", all["total"]}, {"value", ""}});
      }
      for (int kind : kinds) {
        const std::string lit = kind == 0 ? std::string(".") : std::string(".") + static_cast<char>('a' + kind - 1);
        ojson all = callTabstat(std::nullopt, andIf(*byVal + "==" + lit), false, false);
        if (all.value("type", std::string()) == "error") { blocks = failed(all); return true; }
        extra.push_back(ojson{{"stats", all["total"]}, {"value", lit}});
      }
      if (!extra.empty()) {
        ojson merged = ojson::array();
        if (strMissing) merged.push_back(extra[0]);
        for (const auto& g : tsBlock["groups"]) merged.push_back(g);
        for (size_t i = strMissing ? 1 : 0; i < extra.size(); ++i) merged.push_back(extra[i]);
        tsBlock["groups"] = merged;
        if (!tsNoTotal) {
          ojson all = callTabstat(std::nullopt, tsIf, false, false);
          if (all.value("type", std::string()) == "error") { blocks = failed(all); return true; }
          tsBlock["total"] = all["total"];
        }
      }
    }
    // The text table for a `log using` transcript (as measured on StataBE).
    {
      std::optional<ojson> tsRefusal;
      std::string tsText = renderTabstatTable(session, parsed, tsBlock, byVal, columnsVal,
                                              tsNoTotal, statKeys, statRows, tsRefusal);
      if (tsRefusal.has_value()) { blocks = failed(*tsRefusal); return true; }
      session.pendingTabstatLogText = std::move(tsText);
    }
    // The groups were read for the text table; the emitted block carries them
    // only where it always has (save/nototal: runTabstatCore's freeze note).
    if (!(tsSave || tsNoTotal)) tsBlock.erase("groups");
    // r() is tabstat's after every run: empty without `save` (sx_tabstat_unsaved,
    // rowsof(r(StatTotal)) r(109) in Stata), and with `save` r(Stat#)/r(name#)
    // per by() group in the group order and r(StatTotal) unless nototal
    // (pins_oracle_g.log:189-220; sx_tabstat_plain, _s_by, _by_str).
    eval::replaceRClass(session.eval);
    std::vector<std::string> posted;
    if (tsSave) {
      std::vector<std::string> colVars;
      if (tsBlock.contains("vars") && tsBlock["vars"].is_array())
        for (const auto& vn : tsBlock["vars"])
          if (vn.is_string()) colVars.push_back(vn.get<std::string>());
      auto buildStatMatrix = [&](const ojson& statsObj) {
        return tabstatStatMatrix(statsObj, statKeys, statRows, colVars);
      };
      int gi = 0;
      if (tsBlock.contains("groups") && tsBlock["groups"].is_array()) {
        for (const auto& g : tsBlock["groups"]) {
          if (!g.is_object() || !g.contains("stats")) continue;
          ++gi;
          session.eval.rMatrices.emplace_back("Stat" + std::to_string(gi),
                                              buildStatMatrix(g["stats"]));
          // The group's NAME: its value label when the session holds one for
          // this by-variable, a string group's text, and otherwise the value
          // itself with no trailing ".0" (Stata prints `1982`, not `1982.0`).
          std::string nm;
          if (g.contains("value")) {
            if (g["value"].is_string()) {
              nm = g["value"].get<std::string>();
            } else if (g["value"].is_number()) {
              const double gv = g["value"].get<double>();
              bool labelled = false;
              if (byVal.has_value()) {
                auto vlIt = session.valueLabels.find(*byVal);
                if (vlIt != session.valueLabels.end() &&
                    std::fabs(gv - std::nearbyint(gv)) < 1e-9) {
                  auto lit = vlIt->second.find(static_cast<int>(std::nearbyint(gv)));
                  if (lit != vlIt->second.end()) {
                    nm = lit->second;
                    labelled = true;
                  }
                }
              }
              if (!labelled) {
                std::ostringstream os;
                if (std::fabs(gv - std::nearbyint(gv)) < 1e-9) {
                  os << static_cast<long long>(std::nearbyint(gv));
                } else {
                  os << gv;
                }
                nm = os.str();
              }
            }
          }
          session.eval.rStrings.set("name" + std::to_string(gi), nm);
          posted.push_back("Stat" + std::to_string(gi));
          posted.push_back("name" + std::to_string(gi));
        }
      }
      if (!tsNoTotal && tsBlock.contains("total") && tsBlock["total"].is_object()) {
        session.eval.rMatrices.emplace_back("StatTotal", buildStatMatrix(tsBlock["total"]));
        posted.push_back("StatTotal");
      }
    }
    eval::declareRPosted(session.eval, "tabstat", std::move(posted));
    blocks.push_back(tsBlock);
    return true;
  }


  // BUG-105 abbreviation stopgap: `cor` (see the `describe`/`d` note above).
  // Deliberately NOT `c`: real Stata's minimum abbreviation for `correlate`
  // is `cor` (`c` alone is ambiguous), and the BUGLOG finding names the
  // 3-letter spelling specifically.
  if (cmd == "corr" || cmd == "cor" || cmd == "pwcorr" || cmd == "correlate") {
    bool pairwise = false, sig = false, obs = false, covar = false;
    handlers::CorrExtra extra;
    bool bonferroni = false, sidak = false;
    std::optional<std::string> starText, printText;
    std::vector<std::string> correlateOnly, pwcorrOnly;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "pairwise" && v.isBool && v.boolValue) pairwise = true;
      if (k == "sig" && v.isBool && v.boolValue) sig = true;
      if (k == "obs" && v.isBool && v.boolValue) obs = true;
      // BUG-1014: correlate's covariance (abbreviation cov, correlate.sthlp).
      if (k == "covariance" && (!v.isBool || v.boolValue)) covar = true;
      // correlate's means / noformat / wrap; pwcorr's bonferroni, sidak, listwise
      // (casewise), star(#), print(#) (correlate.sthlp, pwcorr.ado's syntax line).
      if (k == "means" || k == "noformat" || k == "wrap") correlateOnly.push_back(k);
      if (k == "means") extra.means = true;
      if (k == "bonferroni" || k == "sidak" || k == "listwise" || k == "casewise" ||
          k == "star" || k == "print")
        pwcorrOnly.push_back(k);
      if (k == "bonferroni") bonferroni = true;
      if (k == "sidak") sidak = true;
      if (k == "listwise" || k == "casewise") extra.listwise = true;
      if (k == "star") starText = v.isBool ? std::string() : v.strValue;
      if (k == "print") printText = v.isBool ? std::string() : v.strValue;
    }
    // BS-097: pairwise/sig/obs are the only corr/pwcorr/correlate options
    // this arm reads (real Stata also has means/covariance/kendall/star()/
    // bonferroni/sidak/print()/listwise/casewise, not implemented here).
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kCorrKnownOpts = kVocabCorr.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kCorrKnownOpts)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    if (cmd == "pwcorr") pairwise = true;
    {
      const auto refuse198 = [&](const std::string& msg) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", msg, std::nullopt, std::nullopt,
                                                std::nullopt, std::string("r(198)"))));
        return true;
      };
      if (cmd == "pwcorr" && !correlateOnly.empty())
        return refuse198("option " + correlateOnly.front() + " not allowed");
      if (cmd != "pwcorr" && !pwcorrOnly.empty())
        return refuse198("option " + pwcorrOnly.front() + " not allowed");
      // pwcorr.ado: bonferroni and sidak together are r(198); star(#) and print(#)
      // are levels, # >= 1 read as a percentage, and a level still >= 1 "out of range".
      if (bonferroni && sidak) return refuse198("options bonferroni and sidak may not be combined");
      extra.adjust = bonferroni ? 1 : sidak ? 2 : 0;
      for (const auto& [text, target, name] :
           {std::tuple<std::optional<std::string>*, double*, const char*>{&starText, &extra.star, "star"},
            std::tuple<std::optional<std::string>*, double*, const char*>{&printText, &extra.print, "print"}}) {
        if (!text->has_value()) continue;
        const std::string t = strutil::trim(**text);
        char* end = nullptr;
        double lv = std::strtod(t.c_str(), &end);
        if (t.empty() || end == nullptr || *end != '\0' || std::isnan(lv))
          return refuse198("option " + std::string(name) + "() incorrectly specified");
        if (lv >= 1) {
          lv /= 100.0;
          if (lv >= 1) return refuse198(std::string(name) + "() out of range");
        }
        *target = lv;
      }
    }

    // BS-028 (BETA Phase C batch C1): `obs` was previously mismapped
    // straight into `sig` — `correlate, obs` silently became `correlate,
    // sig`, which then hard-errors cobra's own "corr --sig requires
    // --pairwise" instead of real Stata's actual "option obs not allowed"
    // (oracle-pinned: rc=198); `pwcorr, obs` silently became `pwcorr, sig`
    // (showing p-values the user never asked for instead of the per-pair N
    // Stata actually displays). Real Stata: `obs` is a pwcorr-only option —
    // correlate (listwise) already prints its "(obs=N)" banner
    // unconditionally, so an explicit `obs` there is not valid syntax at
    // all, regardless of `pairwise`.
    if (obs && cmd != "pwcorr") {
      blocks.push_back(ojson(eval::engineError("option obs not allowed", "unsupported_option")));
      return true;
    }
    // `pwcorr, obs`: cobra's own `corr --pairwise` output already carries a
    // full nobs[i][j] matrix REGARDLESS of --sig (verified against a live
    // cobra run) and cobraToEstimationM4's passthrough already forwards
    // "nobs" — so requesting --pairwise (pwcorr always does, above) is
    // already enough for the block to carry the correct per-pair N data.
    // NOTE (frontend, out of scope — piet/frontend/** is forbidden for this
    // batch): console.js's formatCorrelate() has no code path that reads
    // `block.nobs` to render a per-cell N row for pwcorr (only the
    // `showSig` p-value row exists, gated on `block.sig`) — until a
    // companion frontend change adds one, `pwcorr ..., obs` renders the
    // plain correlation matrix with no extra row. That is still a real
    // improvement over before: `sig` is no longer force-set true by `obs`,
    // so the display no longer shows spurious, un-requested p-values
    // either.
    // covariance is correlate's, not pwcorr's (pwcorr.ado's syntax line has no
    // such option: r(198)).
    if (covar && cmd == "pwcorr") {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "option covariance not allowed",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              std::string("r(198)"))));
      return true;
    }
    std::string corrWeightFlag;
    if (parsed.weightType.has_value() && parsed.weightVar.has_value()) {
      const std::string wt = lower(*parsed.weightType);
      corrWeightFlag = (wt == "aw" || wt == "aweight") ? "--aweight" : "--fweight";
    }
    ojson corrBlock = ojson(handlers::runCorrStata(
        parsed.varlist, pairwise, sig, covar, corrWeightFlag,
        parsed.weightVar.has_value() ? *parsed.weightVar : std::string(), session, parsed.ifExpr,
        parsed.inRange, extra));
    // BS-028 composing 1-liner (BETA campaign C2): record `obs` on the
    // block itself so a future frontend change can decide whether to
    // render the per-pair N matrix (`nobs`, already flowing per the NOTE
    // above) without re-deriving "was `obs` requested" from opts a second
    // time. Only reachable here when cmd=="pwcorr" (the `correlate, obs`
    // case already returned above).
    if (obs && corrBlock.is_object()) {
      corrBlock["obs"] = true;
    }
    // The names Stata posts are declared by runCorrStata (correlate's help;
    // RULING.md D.3.5), per the form actually run.
    blocks.push_back(corrBlock);
    return true;
  }


  if (cmd == "ttesti") {
    blocks.push_back(ojson(handlers::runTtesti(parsed, session)));
    return true;
  }

  if (cmd == "ttest") {
    // BUG-1015: runTtest posts r() itself, every name of the form run and no
    // other, from the engine's own `r` object (FU-269 FU-270; w2o_rnames N07 N09
    // N10, w2as_summary_stats_family sx_ttest_*). Before, this arm rebuilt a
    // one-sample set for every form (r(sd) included) from the display block.
    ojson ttestBlk = ojson(handlers::runTtest(parsed, session));
    blocks.push_back(std::move(ttestBlk));
    return true;
  }


  // P3 weights wave: `mean`/`proportion` — new registry rows (PweightOnly),
  // routed to the new handlers::runMean/runProportion (forward-declared at
  // the top of this file — see that comment for why they're not in
  // piet/handlers.h). Both eclass in real Stata; e(b)/_b[...] wiring happens
  // inside the handler itself (session.setLastModel), same mechanism
  // regress above uses — no dispatcher-side r()/e() plumbing needed here.
  if (cmd == "mean") {
    // BUG-373: r(title) is the table caption Stata publishes for these two.
    ojson meanBlk = ojson(handlers::runMean(parsed, session));
    if (meanBlk.value("type", std::string()) != "error")
      session.eval.rStrings.set("title", "Mean estimation");
    blocks.push_back(std::move(meanBlk));
    return true;
  }

  if (cmd == "proportion") {
    ojson propBlk = ojson(handlers::runProportion(parsed, session));
    if (propBlk.value("type", std::string()) != "error")
      session.eval.rStrings.set("title", "Proportion estimation");
    blocks.push_back(std::move(propBlk));
    return true;
  }
  return false;
}

}
