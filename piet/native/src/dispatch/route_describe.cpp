// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/eval/result_order.h"  // orderRResults (describe/inspect list r() as Stata does)
#include "piet/eval/tokenizer.h"     // scanOneToken (ds has()/not() patterns)
#include "summary_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
#include "piet/handlers/descriptive.h"
#include "piet/handlers/variables.h"
#include "piet/cclass.h"  // kSetMarkerPrefix: `set varabbrev off` for unab
#include "piet/strutil.h"

#include <algorithm>
#include <regex>
#include <sstream>
namespace piet::dispatch_detail {
bool route_describe(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {


  if (cmd == "codebook") {
    // BUG-336 (batch 2): known set measured from the handler body -- codebook reads no options (delegates to runSummarize per-var).
    // BUG-371: `compact` moves from the honest-reject set into the implemented one.
    // Nothing new is computed for it — this arm already gathers obs/unique/mean/
    // min/max/label for every variable; compact only decides which get printed.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabCodebook.known, kVocabCodebook.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // BS-025 (BETA Phase C batch C1): codebook used to be a bare alias of
    // `describe` (same shared arm as below) — so `codebook income if
    // year==1982` silently ignored the `if` entirely (returned the SAME
    // unfiltered stats as plain `codebook income`), and the payload was
    // only describe's type/format/label/n_missing/n_unique, never
    // codebook's own range/mean/sd/percentiles. This arm gives codebook
    // its own implementation: honour `if` via the same keepif-into-
    // temp-hbin idiom already used by the tabulate/table `if` arms above
    // (dispatcherNextPath + `cobra eval --keepif` + a fresh Datastore over
    // the filtered copy), compute per-variable type/format/label/
    // n_missing/n_unique directly off that (filtered) Datastore — mirroring
    // handlers::runDescribe's own numeric-unique-count scan — and pull
    // mean/sd/min/max/p10/p25/p50/p75/p90 from handlers::runSummarize(...,
    // detail=true, ...): cobra's `eval --summarize --detail` raw output
    // already carries every Stata percentile (verified live: p1/p5/p10/
    // p25/p50/p75/p90/p95/p99), and runSummarize forwards that raw per-var
    // object VERBATIM into its "stats" — summarize's own renderer just
    // never displays them. No handler changes needed: both
    // runSummarize and runDescribe's per-variable logic are reused/mirrored
    // as-is.
    bool cbCompact = false, cbProblems = false, cbDetail = false;
    for (const auto& [k, v] : parsed.opts) {
      const bool on = !v.isBool || v.boolValue;
      if (k == "compact" && on) cbCompact = true;
      if (k == "problems" && on) cbProblems = true;
      if (k == "detail" && on) cbDetail = true;
    }
    // compact excludes every other option (codebook.ado: opts_exclusive).
    if (cbCompact && (cbProblems || cbDetail)) {
      blocks.push_back(ojson(eval::errorBlock(
          "syntax_error", std::string("options compact and ") + (cbProblems ? "problems" : "detail") +
                              " may not be combined",
          std::nullopt, std::nullopt, std::nullopt, "r(198)")));
      return true;
    }
    // `problems` alone prints only the problem report; with `detail` it follows
    // the full per-variable report, which this build does not print.
    if (cbProblems && cbDetail) {
      blocks.push_back(ojson(eval::errorBlock(
          "unsupported_option",
          "Option `detail` with `problems` is a valid Stata option combination that this version "
          "of PIET does not yet implement for `codebook`.",
          std::nullopt, std::nullopt, std::nullopt, "r(198)")));
      return true;
    }
    if (!session.hasData()) {
      blocks.push_back(ojson(eval::noDataLoaded()));
      return true;
    }
    Datastore* ds = session.datastore();
    std::unique_ptr<Datastore> codebookIfDs;
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
      std::string codebookIfPath = dispatcherNextPath(session.workdir(), "codebookif");
      // BUG-369 follow-up: `codebook x if L.y>0` — same injection every other
      // user-if surface carries; no-op unless the predicate names a TS operator.
      std::vector<std::string> ifArgs = translate::injectTsContextForExpr(
          {"eval", "--data", ds->path(), "--keepif", *parsed.ifExpr, "--out", codebookIfPath},
          *parsed.ifExpr, session);
      try {
        runCobraJson(ifArgs);
      } catch (const CobraError& ce) {
        blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
        return true;
      }
      codebookIfDs = std::make_unique<Datastore>(codebookIfPath);
      ds = codebookIfDs.get();
    }
    std::vector<std::string> names = parsed.varlist.empty() ? ds->names() : parsed.varlist;
    for (const auto& name : names) {
      if (!ds->has(name)) {
        blocks.push_back(ojson(eval::engineError("variable " + name + " not found")));
        return true;
      }
    }
    // The per-variable reports need observations (marksample + count, then error 2000).
    if (!cbCompact && ds->nRows() == 0) {
      blocks.push_back(ojson(eval::errorBlock("engine_error", "no observations", std::nullopt,
                                              std::nullopt, std::nullopt, "r(2000)")));
      return true;
    }
    if (cbProblems) {
      blocks.push_back(handlers::runCodebookProblems(*ds, names, session, true));
      return true;
    }
    // Batch every numeric var into ONE cobra summarize,detail call (not one
    // per variable) — its raw per-var object already carries mean/sd/min/
    // max/p10/p25/p50/p75/p90, Stata-exact.
    std::vector<std::string> f64Names;
    for (const auto& name : names) {
      if (ds->dtype(name) == "f64") f64Names.push_back(name);
    }
    ojson f64Stats = ojson::object();
    if (!f64Names.empty()) {
      ojson sumBlock = ojson(handlers::runSummarize(f64Names, /*detail=*/true, session,
                                                     parsed.ifExpr, std::nullopt));
      if (sumBlock.value("type", "") == "summarize" && sumBlock.contains("stats")) {
        f64Stats = sumBlock["stats"];
      }
    }

    ojson variables = ojson::array();
    for (const auto& name : names) {
      VarInfo vi = ds->varinfo(name);
      auto metaIt = session.varMeta.find(name);
      std::string stataType;
      std::string fmt;
      if (metaIt != session.varMeta.end() && metaIt->second.stataType.has_value()) {
        stataType = *metaIt->second.stataType;
      } else {
        stataType = (vi.dtype == "factor") ? "str#" : "float";
      }
      // format-display-default-lifecycle: the one resolver describe and `: format`
      // share, not a codebook-local fallback table.
      fmt = session.displayFormat(vi);
      std::string label;
      if (metaIt != session.varMeta.end() && metaIt->second.label.has_value()) {
        label = *metaIt->second.label;
      }

      ojson entry;
      entry["name"] = name;
      entry["dtype"] = stataType;
      entry["format"] = fmt;
      entry["label"] = label;
      entry["n_missing"] = ds->nMissing(name);
      entry["n_obs"] = ds->nRows();

      if (vi.dtype == "factor") {
        entry["n_unique"] = static_cast<int64_t>(vi.levels.size());
      } else {
        const cecon::Column& col = ds->col(name);
        const double* d = col.f64();
        int64_t count = col.nbytes / static_cast<int64_t>(sizeof(double));
        std::vector<double> valid;
        valid.reserve(count);
        for (int64_t i = 0; i < count; ++i) {
          if (!std::isnan(d[i])) valid.push_back(d[i]);
        }
        std::sort(valid.begin(), valid.end());
        valid.erase(std::unique(valid.begin(), valid.end()), valid.end());
        entry["n_unique"] = static_cast<int64_t>(std::min<size_t>(valid.size(), 65536));

        if (f64Stats.contains(name)) {
          const ojson& st = f64Stats[name];
          for (const char* k : {"mean", "sd", "min", "max", "p10", "p25", "p50", "p75", "p90"}) {
            if (st.contains(k)) entry[k] = st[k];
          }
        }
      }
      variables.push_back(entry);
    }
    ojson out;
    // No dedicated frontend renderer exists yet for codebook's full payload
    // (range/unique/mean/sd/percentiles combined) — describe's own switch
    // case already renders type/format/label/n_missing/n_unique from this
    // exact "describe" block shape (console.js's formatDescribe), so reuse
    // it rather than emitting a block type the frontend cannot render at
    // all; the extra numeric fields ride along on each variable entry for
    // when the frontend adds codebook-specific rendering (out of scope for
    // this native-only batch — piet/frontend/** is forbidden here).
    out["type"] = "describe";
    out["variables"] = variables;
    // BUG-371: the compact flag rides on the same block. The frontend switches on
    // it (console.js formatDescribe) to print Stata's one-line-per-variable table
    // instead of the storage/format/label layout; a consumer that ignores the flag
    // still sees every field it saw before, so this is purely additive.
    for (const auto& [k, v] : parsed.opts)
      if (k == "compact" && (!v.isBool || v.boolValue)) out["compact"] = true;
    // BUG-1490: codebook replaces r() with nothing (StataBE 19.5, rlife/side2.do:
    // `: r(scalars)`, `: r(macros)` and `: r(matrices)` are empty after `codebook
    // x1`, `codebook x1 x2` and `codebook`), so the summarize run above leaves none
    // of its r() behind and every r() name reads as missing.
    if (cbCompact) {
      eval::replaceRClass(session.eval);
      eval::declareRPosted(session.eval, "codebook", {});
    } else {
      // The problem lists codebook leaves in r() (empty ones are not stored).
      (void)handlers::runCodebookProblems(*ds, names, session, false);
    }
    blocks.push_back(out);
    return true;
  }


  // BUG-105 abbreviation stopgap: `d` is real Stata's minimum-length
  // abbreviation of `describe` (the general prefix resolver is a separate
  // arc — BACKLOG BUG105-112-HARDENING-REMAINDER). Every spelling added
  // here is ALSO added to this command's registry.cpp `abbrev` list, which
  // is what makes the registry-backed qualifier gates (tokenizer's `in`/
  // weight allow-lists) and testRegistryRoutes see the same alias set.
  if (cmd == "describe" || cmd == "desc" || cmd == "d") {
    // BUG-336 (batch 2): known set measured from the handler body -- runDescribe
    // itself takes no options parameter. B5-D5: `fullnames` is now accepted
    // (kVocabDescribe.known) as a validated no-op -- see that registration's
    // comment for the receipt; short/simple/numbers/detail remain refused by
    // name because they change what prints.
    // `s` is the shortest abbreviation of `short` that describe's option
    // grammar leaves unambiguous against `simple` (vs-stata bank 7a9493d5bfae6927:
    // `describe, s` is the short listing, r(N) r(k) r(changed) posted).
    for (auto& kv : parsed.opts)
      if (kv.first == "s") kv.first = "short";
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabDescribe.known, kVocabDescribe.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // `describe [varlist] using FILE`: the FILE is described and the data in
    // memory are not. This arm answered the data in memory instead (or an empty
    // description with nothing loaded), treating `using` and the file name as
    // variables it then skipped -- a silent wrong answer.
    {
      static const std::regex reUsing(R"(\busing\s+(.+?)\s*(?:,|$))", std::regex::icase);
      std::smatch um;
      const auto usingWord =
          std::find_if(parsed.varlist.begin(), parsed.varlist.end(),
                       [](const std::string& t) { return lower(t) == "using"; });
      if (usingWord != parsed.varlist.end() && std::regex_search(parsed.raw, um, reUsing)) {
        // describe.ado: `replace` goes to describe_mk, whose syntax takes no
        // `using` (r(101) "using not allowed"); `simple` with any other option
        // is r(198) "simple may not be combined with other options"; the
        // file_options are short, simple and varlist, and any other option
        // (fullnames, numbers, clear) is r(198) with `using` ([D] describe).
        bool uShort = false, uSimple = false, uVarlist = false, uReplace = false;
        std::string uOther;
        for (const auto& [k, v] : parsed.opts) {
          (void)v;
          if (k == "short") uShort = true;
          else if (k == "simple") uSimple = true;
          else if (k == "varlist") uVarlist = true;
          else if (k == "replace") uReplace = true;
          else if (uOther.empty()) uOther = k;
        }
        const auto refuse = [&](const std::string& msg, const char* rc) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", msg, std::nullopt,
                                                  std::nullopt, std::nullopt, rc)));
          return true;
        };
        if (uReplace) return refuse("using not allowed", "r(101)");
        if (uSimple && (uShort || uVarlist || !uOther.empty()))
          return refuse("simple may not be combined with other options", "r(198)");
        if (!uOther.empty()) return refuse("option " + uOther + " not allowed", "r(198)");
        // The names as TYPED: the tokenizer resolves an abbreviation against the
        // data in memory (`displ` -> `displacement`), and with `using` Stata does
        // not abbreviate, so they are read off the raw line before `using`.
        std::vector<std::string> names;
        {
          std::istringstream words(parsed.raw.substr(0, static_cast<size_t>(um.position(0))));
          std::string w;
          bool first = true;
          while (words >> w) {
            if (first) {
              first = false;  // the command word
              continue;
            }
            names.push_back(w);
          }
        }
        blocks.push_back(ojson(
            handlers::runDescribeUsing(names, um.str(1), uShort, uSimple, uVarlist, session)));
        return true;
      }
    }
    // describe.ado: `simple` may not be combined with another option (r(198));
    // with `replace` the call is describe_mk's, which takes only clear/replace.
    {
      bool mSimple = false, mReplace = false;
      std::string mOther, mNotMk;
      for (const auto& [k, v] : parsed.opts) {
        (void)v;
        if (k == "simple") mSimple = true;
        else if (k == "replace") mReplace = true;
        else if (mOther.empty()) mOther = k;
        if (k != "replace" && k != "clear" && k != "simple" && mNotMk.empty()) mNotMk = k;
      }
      // describe_mk's syntax is `[varlist], [CLEAR REPLACE]`: with `replace`
      // any other option is r(198) "option NAME not allowed".
      if (mReplace && !mNotMk.empty()) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "option " + mNotMk + " not allowed",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(198)")));
        return true;
      }
      if (mSimple && (mReplace || !mOther.empty())) {
        blocks.push_back(ojson(eval::errorBlock(
            "syntax_error",
            mReplace ? std::string("option simple not allowed")
                     : std::string("simple may not be combined with other options"),
            std::nullopt, std::nullopt, std::nullopt, "r(198)")));
        return true;
      }
    }
    if (!session.hasData()) {
      // Mission 5: empty description (0 vars), matching real Stata — see
      // the kNoDataCommands comment above for why this is handled here
      // rather than in handlers::runDescribe() itself.
      ojson emptyDescribe;
      emptyDescribe["type"] = "describe";
      emptyDescribe["variables"] = ojson::array();
      // BUG-1447: an empty dataset is described too (StataBE 19.5 after `clear`:
      // r(N) 0, r(k) 0, r(width) 0, r(changed) 0, and the edition limits).
      handlers::postDescribeResults(session);
      blocks.push_back(emptyDescribe);
      return true;
    }
    std::optional<std::vector<std::string>> vl;
    if (!parsed.varlist.empty()) {
      // The varlist as Stata reads it: wildcards and ranges expanded, an
      // abbreviation resolved, a name that is no variable r(111) `variable
      // NAME not found`. runDescribe skipped a name it did not find, so
      // `describe nosuch` was an empty description at rc 0 and `describe pri`
      // described nothing (both off the resident card: no Datastore is built).
      // `~` is the varlist's abbreviation wildcard (`displ~`: the one variable
      // whose name it matches; several are r(111) "ambiguous abbreviation"),
      // which the shared expander does not take: resolved here first.
      std::vector<std::string> known;
      if (const std::vector<VarInfo>* card = session.residentReadCard()) {
        for (const auto& v : *card) known.push_back(v.name);
      } else if (Datastore* kds = session.datastore()) {
        known = kds->names();
      }
      std::string spec;
      for (const auto& t0 : parsed.varlist) {
        std::string t = t0;
        if (t.find('~') != std::string::npos) {
          std::string pat = t;
          std::replace(pat.begin(), pat.end(), '~', '*');
          std::vector<std::string> hit;
          for (const auto& n : known)
            if (piet::strutil::globMatch(pat, n)) hit.push_back(n);
          if (hit.size() != 1) {
            blocks.push_back(ojson(eval::errorBlock(
                "engine_error",
                hit.empty() ? "variable " + t + " not found" : t + " ambiguous abbreviation",
                std::nullopt, std::nullopt, std::nullopt, "r(111)")));
            return true;
          }
          t = hit.front();
        }
        spec += (spec.empty() ? "" : " ") + t;
      }
      piet::loops::VarlistExpandResult ex = piet::loops::expandVarlist(spec, session);
      if (ex.isError) {
        blocks.push_back(ex.error);
        return true;
      }
      if (auto err = piet::loops::checkKnownVarlist(ex.tokens, session)) {
        blocks.push_back(*err);
        return true;
      }
      vl = ex.tokens;
    }
    ojson desc = ojson(handlers::runDescribe(vl, session));
    bool dShort = false, dSimple = false, dVarlist = false, dReplace = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "short") dShort = true;
      if (k == "simple") dSimple = true;
      if (k == "varlist") dVarlist = true;
      if (k == "replace") dReplace = true;
    }
    if ((dShort || dSimple || dVarlist || dReplace) && desc.is_object() &&
        desc.value("type", std::string()) != "error" && desc.contains("variables")) {
      std::vector<std::string> dnames;
      for (const auto& v : desc.at("variables")) dnames.push_back(v.value("name", std::string()));
      int64_t nObs = 0;
      if (std::optional<int64_t> rows = session.residentRowsIfCurrent()) nObs = *rows;
      else if (Datastore* dds = session.datastore()) nObs = dds->nRows();
      if (dReplace) {
        // The description replaces the data in memory: with unsaved changes
        // Stata refuses unless `clear` is given too (r(4); vs-stata bank
        // cc76c913068222b6, `generate z = 1` then `describe, replace`).
        bool dClear = false;
        for (const auto& [k, v] : parsed.opts) {
          (void)v;
          if (k == "clear") dClear = true;
        }
        if (!dClear && session.datasetChanged) {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error",
              "no; data in memory would be lost\n    Specify describe ..., replace clear to "
              "perform the command anyway.",
              std::nullopt, std::nullopt, std::nullopt, "r(4)")));
          return true;
        }
        // The description becomes the dataset, one observation per variable.
        delimited::Table t;
        t.names = {"position", "name", "type", "isnumeric", "format", "vallab", "varlab"};
        t.labels.assign(t.names.size(), std::string());
        size_t pos = 0;
        for (const auto& v : desc.at("variables")) {
          ++pos;
          const std::string nm = v.value("name", std::string());
          const std::string ty = v.value("dtype", std::string());
          auto lb = session.varLblname.find(nm);
          t.rows.push_back({std::to_string(pos), nm, ty,
                            ty.rfind("str", 0) == 0 ? "0" : "1", v.value("format", std::string()),
                            lb == session.varLblname.end() ? std::string() : lb->second,
                            v.value("label", std::string())});
        }
        t.types.resize(t.names.size());
        for (size_t c = 0; c < t.names.size(); ++c) {
          std::vector<const std::string*> cells;
          for (const auto& r : t.rows) cells.push_back(&r[c]);
          t.types[c] = delimited::typeColumn(cells, c != 0 && c != 3, false, false);
        }
        if (auto err = delimited::stageAndAdopt(session, t, "describe", std::string())) {
          blocks.push_back(*err);
          return true;
        }
        blocks.push_back(ojson(handlers::mutationResult(session, "describe")));
        return true;
      }
      // BUG-1447: r(N), r(k) and r(width) are the dataset's, whatever was
      // described; `short` and `simple` post the full set too (r(datalabel)
      // included, StataBE 19.5 `describe, short` after `restore`).
      handlers::postDescribeResults(session);
      if (dVarlist) {
        std::string joined;
        for (const auto& nm : dnames) joined += (joined.empty() ? "" : " ") + nm;
        session.eval.rStrings.set("varlist", joined);
        std::string sortlist;
        if (Datastore* dds = session.datastore()) sortlist = dds->sortedBy();
        session.eval.rStrings.set("sortlist", sortlist);
        eval::orderRResults(session.eval, {"sortlist", "varlist", "datalabel"});
        // `varlist` adds r(varlist) and r(sortlist) to describe's set.
        std::vector<std::string> names = *session.eval.rDeclaredNames;
        names.push_back("varlist");
        names.push_back("sortlist");
        eval::declareRPosted(session.eval, "describe", std::move(names));
      }
      if (dShort || dSimple) {
        ojson out;
        out["type"] = "display";
        if (dSimple) {
          std::string joined;
          for (const auto& nm : dnames) joined += (joined.empty() ? "" : "  ") + nm;
          out["text"] = joined;
        } else {
          out["text"] = "Contains data\n Observations: " + std::to_string(nObs) +
                        "\n    Variables: " + std::to_string(dnames.size());
        }
        blocks.push_back(out);
        return true;
      }
    }
    // BUG-159 (P4 wave): `describe` is r-class in Stata — it posts r(N), r(k)
    // and r(width). PIET posted nothing, so a following `r(k)` errored with
    // "run an r-class command first" after the user had just run one. r(N) is
    // the DATASET's row count (not the described-variable count), and so is
    // r(k) its variable count (BUG-1447), not the number described.
    //
    // r(N) is the dataset's row count, off the resident card when it is
    // current (a card-plane describe constructs no Datastore, C4-L2B) and off
    // the Datastore runDescribe already opened otherwise; r(k), r(width),
    // r(changed) (c(changed): 1 after edits, 0 right after `save` or `use`),
    // the edition limits and r(datalabel) when the dataset has a label
    // (handlers::postDescribeResults).
    handlers::postDescribeResults(session);
    blocks.push_back(desc);
    return true;
  }


  // BUG-105 abbreviation stopgap: `su` (see the `describe`/`d` note above).
  if (cmd == "summarize" || cmd == "sum" || cmd == "su") {
    bool detail = false;
    for (const auto& [k, v] : parsed.opts) {
      if ((k == "detail" || k == "d") && v.isBool && v.boolValue) detail = true;
    }
    // P4 / BUG-060: unknown-option rejection net. `detail`/`d` and `meanonly`
    // are the summarize options this version of PIET accepts. `meanonly`
    // (BUG-090) tells Stata to skip the variance/percentile pass entirely —
    // r(N)/r(mean)/r(min)/r(max)/r(sum)/r(sum_w) are published, but r(sd)
    // (and the derived r(Var)/r(median)/percentiles/skewness/kurtosis) are
    // NOT: BUG-476, measured StataBE 19.5
    // (tests/eval/golden/aer_b4/pins_oracle_r.log, batch R,
    // `B4VAL|mean|+1.81541bacf914cX+00c|sd_is_missing|+1.0000000000000X+3ff`
    // — mean published, r(sd) is Stata's `.`). PIET's runSummarize still
    // computes the full stats unconditionally (a superset of what meanonly
    // leaves in memory, and cheaper to keep than to special-case) — the
    // DISPLAY and the rendered block are unaffected by meanonly either way
    // (Stata also suppresses meanonly's display; that half is NOT changed
    // here, see the r()-only prune below) — only r() is pruned to match
    // what Stata actually leaves set. Anything else isn't a real summarize
    // option at all -> r(198)-parity reject naming the token.
    bool meanonly = false;
    // BUG-333: `separator(#)` is a display option — Stata draws a horizontal rule
    // every # variables (0 = no rules, default 5). Measured on StataBE 19.5:
    // separator(0)/(2)/(-1) are all rc 0, separator(x) is r(198), so only a
    // non-numeric argument is refused. This loop is kept hand-rolled (rather than
    // switched to rejectUnknownOptions) because it predates the registry and is
    // the summarize arm's only gate; widening it in place is the smaller change.
    long summarizeSeparator = -1;  // -1 = not given; the renderer defaults to 5
    for (const auto& [k, v] : parsed.opts) {
      if (k == "meanonly" && (!v.isBool || v.boolValue)) {
        meanonly = true;
        continue;
      }
      if (k == "detail" || k == "d") continue;
      if (k == "separator") {
        if (v.isBool) {
          blocks.push_back(ojson(eval::engineError(
              "option `separator` requires a number, e.g. separator(5)", "unsupported_option")));
          return true;
        }
        try {
          size_t used = 0;
          summarizeSeparator = std::stol(v.strValue, &used);
          if (used != v.strValue.size()) throw std::invalid_argument("trailing");
        } catch (const std::exception&) {
          blocks.push_back(ojson(eval::engineError(
              "option `separator()` incorrectly specified — invalid number", "unsupported_option")));
          return true;
        }
        if (summarizeSeparator < 0) summarizeSeparator = 0;  // Stata takes -1 as "no rules"
        continue;
      }
      blocks.push_back(
          ojson(eval::engineError("option `" + k + "` not allowed", "unsupported_option")));
      return true;
    }
    std::optional<std::vector<std::string>> vl;
    if (!parsed.varlist.empty()) vl = parsed.varlist;
    // `summarize ... if COND [in RANGE]` is now wired: the row filter
    // reaches the engine via cobra keepif inside runSummarize (Doctrine ①:
    // qualifier honoured, never dropped). BS-090 (BETA Phase G): `in` is now
    // legal here too (registry.cpp's allowInRange), composed with `if` via
    // AND inside runSummarize/applyIfAndInFilter.
    // Step-1 D3 / P3 weights wave: `parsed.weightVar` only survives the
    // tokenizer's guard for this branch when weightType is fw/fweight/aw/
    // aweight (registry.cpp's summarize row is AweightFweight) — any other
    // weight type on summarize was already rejected upstream. Which cobra
    // flag gets used (--fweight vs --aweight) depends on WHICH of those the
    // user actually typed, so branch on parsed.weightType here rather than
    // baking one flag into runSummarize itself.
    bool summarizeIsAweight = parsed.weightType.has_value() &&
        (lower(*parsed.weightType) == "aw" || lower(*parsed.weightType) == "aweight");
    // QF-SUM-IW: summarize takes [iw=] (w2aj_qualifiers.log:826 qf_sw_iw_*), but
    // not with detail: r(101) "iweights not allowed" (log:911-912 qf_sw_iw_detail).
    const bool summarizeIsIweight = parsed.weightType.has_value() &&
        (lower(*parsed.weightType) == "iw" || lower(*parsed.weightType) == "iweight");
    if (summarizeIsIweight && detail) {
      blocks.push_back(ojson(eval::errorBlock("engine_error", "iweights not allowed", std::nullopt,
                                              std::nullopt, std::nullopt, "r(101)")));
      return true;
    }
    ojson summBlock = summarizeIsAweight || summarizeIsIweight
        ? ojson(handlers::runSummarizeAweight(vl, detail, session, parsed.ifExpr, parsed.weightVar,
                                              parsed.inRange,
                                              summarizeIsIweight ? "--iweight" : "--aweight"))
        : ojson(handlers::runSummarize(vl, detail, session, parsed.ifExpr, parsed.weightVar,
                                       parsed.inRange));
    // dispatcher.py's post-handler hook: `capture_r_from_block(result_block,
    // session)` — every native-handler result feeds this (summarize is the
    // r-class case the M3a corpus exercises; lincom/nlcom land when ported).
    // Missing entirely through M2 — this is the fix (not new M3a scope).
    // BUG-333: carry the separator to the renderer. Emitted only when the user
    // asked, so every existing summarize fixture stays byte-identical.
    if (summarizeSeparator >= 0 && summBlock.value("type", "") != "error")
      summBlock["separator"] = summarizeSeparator;
    captureSummarizeR(summBlock, meanonly, detail, session);
    blocks.push_back(summBlock);
    return true;
  }


  if (cmd == "count") {
    // BS-097: `count [if COND]` has no comma-options at all (neither branch
    // below reads parsed.opts) — known-set is the empty set.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // A string condition is counted row-wise inside runCount (BUG-032, BUG-412).
    ojson countBlock = ojson(handlers::runCount(parsed.ifExpr, session, parsed.inRange));
    if (countBlock.value("type", std::string()) != "error")
      eval::declareRPosted(session.eval, "count", {"N"});  // count's help; RULING.md D.3.3
    blocks.push_back(std::move(countBlock));
    return true;
  }


  // BUG-105 abbreviation stopgap: `l` (see the `describe`/`d` note above).
  // `l` joins the `list` REGISTRY ROW's abbrev list, so `l in 1/5` clears
  // the tokenizer's registry-driven `in` gate exactly as `list in 1/5` does
  // (that gate resolves the command token through registry::findByName, so
  // a dispatcher-only alias would route but then be rejected for `in`).
  if (cmd == "list" || cmd == "browse" || cmd == "br" || cmd == "l") {
    // BUG-336 (batch 2): known set measured from the handler body -- runList renders a GUI grid: noobs/clean are presentation flags whose effect the grid already
    // satisfies (output byte-identical, measured) and the p8 wave + maxperf corpus use them.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabList.known, kVocabList.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // B5-D6: `separator`/`sep(#)` is parsed (same strict-numeric idiom as
    // summarize's separator above) rather than blindly discarded, so a
    // non-numeric argument still gets an honest reject. The value itself is
    // then dropped: runList's grid draws no every-N-rows rule (same
    // validated-no-op class as noobs/clean, see kVocabList's comment).
    // W1a-1: `abbreviate(#)` takes the same road -- the number is checked, the
    // grid has no header width to apply it to (kVocabList's comment).
    for (const auto& [k, v] : parsed.opts) {
      if (k != "separator" && k != "abbreviate") continue;
      if (v.isBool) {
        blocks.push_back(ojson(eval::engineError(
            "option `" + k + "` requires a number, e.g. " + k + (k == "separator" ? "(5)" : "(8)"),
            "unsupported_option")));
        return true;
      }
      try {
        size_t used = 0;
        (void)std::stol(v.strValue, &used);
        if (used != v.strValue.size()) throw std::invalid_argument("trailing");
      } catch (const std::exception&) {
        blocks.push_back(ojson(eval::engineError(
            "option `" + k + "()` incorrectly specified — invalid number", "unsupported_option")));
        return true;
      }
    }
    // BUG-508: a varlist naming a variable this frame does not have used to be
    // DROPPED in silence — `list a nosuchcol` rendered one column at rc 0 and
    // said nothing, which is the failure mode this campaign ranks worst: the
    // run is GREEN and the table is wrong. Stata is r(111) with the name
    // (oracle tests/eval/golden/bug508_varlist_oracle.log: L1/L2 for a bare
    // name, L5 for a wildcard that matches nothing, and rc 0 for `alpha*` and
    // `alpha-beta`, which is why this cannot be a plain has() loop).
    //
    // Shape copied from the `unab` arm below, including its lock: the resident
    // card is the cheap source but it is not always live, and "no card" must
    // NOT mean "skip the check". Wildcards and `a-b` ranges go through
    // piet::loops::expandVarlist so `x*` means here exactly what it means to
    // `foreach ... of varlist` — the expander's own header forbids a second
    // opinion. The expander does not validate a bare unknown name (it passes
    // straight through; unab's lock records the same), so that half is checked
    // against the frame here.
    if (!parsed.varlist.empty()) {
      std::vector<std::string> knownVars;
      if (const std::vector<VarInfo>* card = session.residentReadCard()) {
        for (const auto& vi : *card) knownVars.push_back(vi.name);
      } else if (Datastore* ldsp = session.datastore()) {
        knownVars = ldsp->names();
      } else {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return true;
      }
      std::string listSpec;
      for (const auto& t : parsed.varlist) {
        if (!listSpec.empty()) listSpec += " ";
        listSpec += t;
      }
      auto lvr = piet::loops::expandVarlist(listSpec, session);
      if (lvr.isError) {
        // The measured trigger is a wildcard matching nothing, and Stata names
        // the TOKEN the user typed (`variable zz* not found`), not the pattern
        // machinery. Find the one that matched nothing rather than blaming the
        // first wildcard in the list.
        for (const auto& t : parsed.varlist) {
          if (t.empty() || t.back() != '*') continue;
          const std::string pre = t.substr(0, t.size() - 1);
          bool any = false;
          for (const auto& k : knownVars) {
            if (k.rfind(pre, 0) == 0) { any = true; break; }
          }
          if (!any) {
            blocks.push_back(ojson(eval::engineError("variable " + t + " not found")));
            return true;
          }
        }
        blocks.push_back(lvr.error);
        return true;
      }
      for (const auto& t : lvr.tokens) {
        if (std::find(knownVars.begin(), knownVars.end(), t) == knownVars.end()) {
          blocks.push_back(ojson(eval::engineError("variable " + t + " not found")));
          return true;
        }
      }
    }
    std::optional<std::vector<std::string>> vl;
    if (!parsed.varlist.empty()) vl = parsed.varlist;
    bool listNoLabel = false;
    for (const auto& kv : parsed.opts)
      if (kv.first == "nolabel") listNoLabel = true;
    blocks.push_back(
        ojson(handlers::runList(vl, parsed.ifExpr, parsed.inRange, session, listNoLabel)));
    return true;
  }


  if (cmd == "levelsof") {
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError("levelsof: variable name required")));
      return true;
    }
    // glevelsof folds onto this arm (gtools_aliases.cpp); the typed head decides
    // whose rules publish: Stata's levelsof.ado or gtools' glevelsof.ado.
    bool gtools = false;
    {
      std::istringstream w(session.leafCmdExpanded);
      std::string head;
      w >> head;
      head = head.substr(0, head.find(','));
      std::transform(head.begin(), head.end(), head.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      gtools = head == "glevelsof";
    }
    // A str option keeps its text without one layer of quotes: separate(" | ")
    // is ` | ` (levelsof, foreign, separate(" | ") clean -> `0 | 1`).
    auto unquoteOpt = [](const std::string& v) {
      if (v.size() >= 4 && v.compare(0, 2, "`\"") == 0 && v.compare(v.size() - 2, 2, "\"'") == 0)
        return v.substr(2, v.size() - 4);
      if (v.size() >= 2 && v.front() == '"' && v.back() == '"') return v.substr(1, v.size() - 2);
      return v;
    };
    handlers::LevelsofRequest req;
    req.vars = parsed.varlist;
    req.ifExpr = parsed.ifExpr;
    req.inRange = parsed.inRange;
    req.gtools = gtools;
    std::unordered_set<std::string> known = kVocabLevelsof.known;
    if (gtools) {
      known.insert("colseparate");
      known.insert("silent");
    }
    for (const auto& [k, v] : parsed.opts) {
      if (k == "local" && !v.isBool) req.localName = v.strValue;
      if (k == "clean" && v.isBool && v.boolValue) req.clean = true;
      if (k == "missing" && v.isBool && v.boolValue) req.missing = true;
      if (k == "separate" && !v.isBool) req.separate = unquoteOpt(v.strValue);
      if (gtools && k == "colseparate" && !v.isBool)
        req.colseparate = unquoteOpt(v.strValue);
      if (gtools && k == "silent" && v.isBool && v.boolValue) req.silent = true;
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, known)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    ojson lv = handlers::runLevelsofCommand(req, session);
    if (!(lv.is_object() && lv.value("silent", false))) blocks.push_back(std::move(lv));
    return true;
  }


  if (cmd == "xtsum") {
    // xtsum.ado: `syntax [varlist(ts)] [if] [in] [, I(varname)]`. i() names the panel variable
    // (the xtset one otherwise); the varlist may hold wildcards and abbreviations and is every
    // variable when absent.
    std::string idOverride;
    auto optsNoI = parsed.opts;
    for (const auto& kv : parsed.opts)
      if (kv.first == "i" && !kv.second.isBool) idOverride = strutil::trim(kv.second.strValue);
    optsNoI.erase(std::remove_if(optsNoI.begin(), optsNoI.end(),
                                 [](const auto& kv) { return kv.first == "i"; }),
                  optsNoI.end());
    if (auto err = eval::rejectUnknownOptions(optsNoI, {})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    std::vector<std::string> xtVars;
    if (!parsed.varlist.empty() && session.hasData()) {
      std::string spec;
      for (const auto& t : parsed.varlist) spec += (spec.empty() ? "" : " ") + t;
      piet::loops::VarlistExpandResult ex = piet::loops::expandVarlist(spec, session);
      if (ex.isError) {
        blocks.push_back(ex.error);
        return true;
      }
      if (auto err = piet::loops::checkKnownVarlist(ex.tokens, session)) {
        blocks.push_back(*err);
        return true;
      }
      xtVars = ex.tokens;
    }
    ojson xblk = ojson(handlers::runXtsum(xtVars, session, parsed.ifExpr, parsed.inRange, idOverride));
    // r() is the last variable's: N n Tbar mean sd min max sd_b min_b max_b sd_w min_w max_w
    // (xtsum.ado `return scalar` after each variable; a missing one is stored as missing).
    if (xblk.value("type", std::string()) != "error" && xblk.contains("xtsum") && xblk.contains("xtsum_last")) {
      const ojson& e = xblk["xtsum"][xblk["xtsum_last"].get<std::string>()];
      const auto num = [&](const char* k) {
        return e.contains(k) && e[k].is_number() ? e[k].get<double>()
                                                 : std::numeric_limits<double>::quiet_NaN();
      };
      eval::replaceRClass(session.eval);
      session.eval.rValues.set("N", num("N"));
      session.eval.rValues.set("n", num("n_units"));
      session.eval.rValues.set("Tbar", num("T_bar"));
      session.eval.rValues.set("mean", num("mean"));
      session.eval.rValues.set("sd", num("sd_overall"));
      session.eval.rValues.set("min", num("min_overall"));
      session.eval.rValues.set("max", num("max_overall"));
      session.eval.rValues.set("sd_b", num("sd_between"));
      session.eval.rValues.set("min_b", num("min_between"));
      session.eval.rValues.set("max_b", num("max_between"));
      session.eval.rValues.set("sd_w", num("sd_within"));
      session.eval.rValues.set("min_w", num("min_within"));
      session.eval.rValues.set("max_w", num("max_within"));
      eval::orderRResults(session.eval, {"max_w", "min_w", "sd_w", "max_b", "min_b", "sd_b", "max", "min",
                                         "sd", "mean", "Tbar", "n", "N"});
      eval::declareRPosted(session.eval, "xtsum",
                           {"max_w", "min_w", "sd_w", "max_b", "min_b", "sd_b", "max", "min", "sd",
                            "mean", "Tbar", "n", "N"});
      xblk.erase("xtsum_last");
    }
    blocks.push_back(std::move(xblk));
    return true;
  }


  if (cmd == "xttab") {
    // With the data xtset, Stata reads the varlist and the options before it looks
    // at the variable: two variables are r(103) "too many variables specified", an
    // unknown option r(198), and then a string variable r(109) "<v> must be
    // numeric" (bank: `xttab jail service`, `xttab jail, zznotanoption`, `xttab
    // jail`, `xttab jail, i(sid)`, `xttab jail in 1/100`; without xtset it is r(459)
    // first, the handler's). i() names the panel variable.
    std::string idOverride;
    auto optsNoI = parsed.opts;
    for (const auto& kv : parsed.opts)
      if (kv.first == "i" && !kv.second.isBool) idOverride = strutil::trim(kv.second.strValue);
    optsNoI.erase(std::remove_if(optsNoI.begin(), optsNoI.end(),
                                 [](const auto& kv) { return kv.first == "i"; }),
                  optsNoI.end());
    if (!idOverride.empty() || (session.panelvar.has_value() && !session.panelvar->empty())) {
      if (parsed.varlist.size() > 1) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "too many variables specified",
                                                std::nullopt, std::nullopt, std::nullopt, "r(103)")));
        return true;
      }
      if (auto err = eval::rejectUnknownOptions(optsNoI, {})) {
        blocks.push_back(ojson(*err));
        return true;
      }
      if (parsed.varlist.size() == 1 && session.datastore() != nullptr &&
          session.datastore()->has(parsed.varlist[0]) &&
          session.datastore()->dtype(parsed.varlist[0]) == "factor") {
        blocks.push_back(ojson(eval::errorBlock("engine_error", parsed.varlist[0] + " must be numeric",
                                                std::nullopt, std::nullopt, std::nullopt, "r(109)")));
        return true;
      }
    }
    if (auto err = eval::rejectUnknownOptions(optsNoI, {})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    ojson tblk = ojson(handlers::runXttab(parsed.varlist, session, parsed.ifExpr, parsed.inRange, idOverride));
    // r(): the Overall/Between/Within table as the matrix r(results) (a value column and the
    // five statistics, the last row the Total with a missing value) and the panel count,
    // stored as r(n_panels) and, hidden from `return list`, r(n) (xttab.ado).
    if (tblk.value("type", std::string()) != "error" && tblk.contains("xttab_results")) {
      eval::EvalMatrix m;
      for (const auto& row : tblk["xttab_results"]) {
        std::vector<double> r;
        for (const auto& v : row) r.push_back(v.is_number() ? v.get<double>() : std::numeric_limits<double>::quiet_NaN());
        m.data.push_back(std::move(r));
        m.rownames.push_back("r" + std::to_string(m.data.size()));
      }
      m.colnames = {"value", "Overall:Freq", "Overall:Percent", "Between:Freq", "Between:Percent",
                    "Within:Percent"};
      eval::replaceRClass(session.eval);
      session.eval.rValues.set("n", tblk["n_units"].get<double>());
      session.eval.markRHidden("n");
      session.eval.rValues.set("n_panels", tblk["n_units"].get<double>());
      session.eval.rMatrices.emplace_back("results", std::move(m));
      eval::declareRPosted(session.eval, "xttab", {"n_panels", "results"});
      tblk.erase("xttab_results");
    }
    blocks.push_back(std::move(tblk));
    return true;
  }


  if (cmd == "inspect") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    ojson blk = ojson(handlers::runInspect(parsed.varlist, parsed.ifExpr, parsed.inRange, session));
    // BUG-373: inspect is r-class in Stata and mine published nothing — a command
    // I wrote the day before, computing every one of these and dropping them.
    // Exactly the class the declared-returns sweep exists to find.
    if (blk.value("type", std::string()) != "error" && blk.contains("inspect")) {
      static const std::vector<std::string> kInspectR = {"N",        "N_neg",    "N_0",
                                                         "N_pos",    "N_negint", "N_posint",
                                                         "N_unique", "N_undoc"};
      for (const auto& k : kInspectR) {
        if (!blk["inspect"].contains(k)) continue;
        const auto& v = blk["inspect"][k];
        // a missing count (N_unique past 99, N_undoc without a value label) is Stata's `.`
        session.eval.rValues.set(k, v.is_number() ? v.get<double>() : std::nan(""));
      }
      // Stata lists them `N_neg N_negint N_0 N_pos N_posint N N_unique N_undoc`
      // (vs-stata truth bank).
      eval::orderRResults(session.eval, {"N_neg", "N_negint", "N_0", "N_pos", "N_posint", "N",
                                         "N_unique", "N_undoc"});
      eval::declareRPosted(session.eval, "inspect", kInspectR);  // BUG-1453: Stata's whole set
    }
    blocks.push_back(std::move(blk));
    return true;
  }


  if (cmd == "ds") {
    // Stata's ds has a large filter vocabulary (has()/alpha/varwidth/detail);
    // most remain unimplemented and refused BY NAME rather than ignored — a
    // dropped filter would silently list the WRONG variables, which is worse
    // than an error. B5-D7: `not` is now accepted (kVocabDs.known) and
    // implemented as the complement of the varlist against every variable in
    // the dataset — runKeep's own complement idiom (datamgmt/observation_selection.cpp). Measured
    // B4REC|ds|not|0| (tests/eval/golden/aer_b4/pins_oracle_r.log:163-164).
    // Bound to the REGISTERED vocabulary below (kVocabDs), not an inline
    // literal set — this arm used to carry its own duplicate copy of
    // kVocabDs.known/unimplemented, so the census and the arm could drift
    // apart; fixed here.
    // has(type ...) / not(type ...) are implemented (ds_util.ado's Type: a type
    // list of string, numeric, alias, str#, byte .. double, strL, or str widths as
    // a numlist from 1 to 2045); every other has()/not() filter (varlabel,
    // vallabel, format, char, linkname) keeps the refusal by name.
    std::optional<std::string> dsTypeFilter;  // the type list
    bool dsTypeNot = false;
    bool dsHasOther = false;
    // has()/not() of variable labels, value labels or formats (ds_util.ado's
    // VarlVall and Format, StataBE 19.5 vs-stata bank 7c34c0e3cf1b603a
    // `has(format %8.0g)`, 5267f0c4c3045bce `has(vallabel)`, e2667f88646bcc8e
    // `has(varlabel *Weight*)`, db7de5b7d20145f0 with `insensitive`): a label
    // filter with no pattern keeps the variables that have one; a pattern is
    // matched as match() does (* and ?), a format also with `%` put before it.
    // char() and linkname() keep the refusal by name.
    std::vector<std::pair<std::string, eval::OptValue>> dsOpts;
    bool dsHasOpt = false, dsNotOpt = false, dsNot2 = false;
    struct DsMetaFilter {
      std::string kind;
      std::vector<std::string> patterns;  // each word of the list is a pattern (a quoted one is whole)
      bool negate;
    };
    // `which` of has()/not(): blank-separated words, a double-quoted run being one word.
    auto dsPatternWords = [](const std::string& text) {
      std::vector<std::string> out;
      size_t i = 0;
      while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i >= text.size()) break;
        std::string w;
        if (text[i] == '"') {
          const size_t close = text.find('"', i + 1);
          const size_t end = close == std::string::npos ? text.size() : close;
          w = text.substr(i + 1, end - i - 1);
          i = close == std::string::npos ? text.size() : close + 1;
        } else {
          while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) w += text[i++];
        }
        out.push_back(w);
      }
      return out;
    };
    // (The language lane implemented the same three filters separately; this one is kept. Its
    // bank cells: 7c34c0e3cf1b603a `has(format %8.0g)`, 5267f0c4c3045bce `has(vallabel)`,
    // e2667f88646bcc8e `has(varlabel *Weight*)`, db7de5b7d20145f0 with `insensitive`.)
    std::vector<DsMetaFilter> dsMetaFilters;
    for (const auto& kv : parsed.opts) {
      const bool valued = !kv.second.isBool;
      if ((kv.first == "has" || kv.first == "not") && valued) {
        (kv.first == "has" ? dsHasOpt : dsNotOpt) = true;
        std::istringstream words(kv.second.strValue);
        std::string what;
        words >> what;
        std::string after;  // the text after the filter's name, as typed
        std::getline(words, after, '\0');
        std::string rest, w;
        for (std::istringstream ws(after); ws >> w;) rest += (rest.empty() ? "" : " ") + w;
        const std::string lw = lower(what);
        if (!lw.empty() && std::string("type").compare(0, lw.size(), lw) == 0) {
          dsTypeFilter = rest;
          dsTypeNot = kv.first == "not";
          continue;
        }
        // varlabel / vallabel / format / char: a filter on the variable's own metadata
        // (ds_util.ado's Varlabel, Vallabel, Format and Char; the pattern is strmatch's).
        {
          auto prefixOf = [&](const char* full, size_t minLen) {
            return lw.size() >= minLen && std::string(full).compare(0, lw.size(), lw) == 0;
          };
          std::string kind;
          if (prefixOf("varlabel", 4)) kind = "varlabel";
          else if (prefixOf("vallabel", 4)) kind = "vallabel";
          else if (prefixOf("format", 1)) kind = "format";
          else if (prefixOf("char", 1)) kind = "char";  // `char`, not `characteristics`
          if (!kind.empty()) {
            dsMetaFilters.push_back({kind, dsPatternWords(rest), kv.first == "not"});
            continue;
          }
        }
        {
          // Only a word that names no filter at all is the syntax error; linkname (needs 5
          // letters) is a real filter this build does not run, refused by name below.
          const bool linkname = lw.size() >= 5 && std::string("linkname").compare(0, lw.size(), lw) == 0;
          if (!linkname) {
            blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                                    "invalid " + kv.first + "() option", std::nullopt,
                                                    std::nullopt, std::nullopt, "r(198)")));
            return true;
          }
        }
        dsHasOther = true;
        dsOpts.push_back({"has", kv.second});  // the by-name refusal below
        continue;
      }
      if (kv.first == "not" && kv.second.isBool && kv.second.boolValue) dsNot2 = true;
      dsOpts.push_back(kv);
    }
    if (!dsHasOther && dsHasOpt + dsNotOpt + dsNot2 > 1) {
      // ds_util.ado's wording for each pair.
      const std::string msg = dsHasOpt && dsNotOpt && dsNot2 ? "may not combine options has(), not(), and not"
                              : !dsHasOpt                     ? "may not combine options not() and not"
                              : !dsNotOpt                     ? "may not combine options has() and not"
                                                              : "may not combine options has() and not()";
      blocks.push_back(ojson(eval::errorBlock("syntax_error", msg, std::nullopt, std::nullopt,
                                              std::nullopt, "r(198)")));
      return true;
    }
    if (auto err = eval::rejectUnknownOptions(dsOpts, kVocabDs.known, kVocabDs.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    bool dsNot = false, dsAlpha = false, dsDetail = false, dsInsensitive = false;
    std::optional<int> dsVarwidth, dsSkip, dsIndent;
    for (const auto& [k, v] : dsOpts) {
      if (k == "not" && v.isBool && v.boolValue) dsNot = true;
      if (k == "alpha" && (!v.isBool || v.boolValue)) dsAlpha = true;
      if (k == "detail" && (!v.isBool || v.boolValue)) dsDetail = true;
      if (k == "insensitive" && (!v.isBool || v.boolValue)) dsInsensitive = true;
      if ((k == "varwidth" || k == "skip" || k == "indent") && !v.isBool) {
        const std::string t = trimWs(v.strValue);
        char* endp = nullptr;
        const double d = std::strtod(t.c_str(), &endp);
        // ds_util's ranges: varwidth() 5..32, skip() 1..10, indent() 0..244.
        const double lo = k == "varwidth" ? 5 : k == "skip" ? 1 : 0;
        const double hi = k == "varwidth" ? 32 : k == "skip" ? 10 : 244;
        if (t.empty() || *endp != '\0' || d != std::floor(d) || d < lo || d > hi) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                                  k + "() invalid", std::nullopt, std::nullopt,
                                                  std::nullopt, "r(198)")));
          return true;
        }
        (k == "varwidth" ? dsVarwidth : k == "skip" ? dsSkip : dsIndent) = static_cast<int>(d);
      }
    }
    // The type list: each word a known type or, all together, a numlist of str
    // widths (ds_util.ado CheckType); empty is "invalid has() option".
    std::vector<std::string> dsTypes;
    if (dsTypeFilter.has_value()) {
      const std::string optName = dsTypeNot ? "not" : "has";
      static const std::set<std::string> kTypeWords = {"alias", "unknown", "byte",   "int",
                                                       "long",  "float",   "double", "string",
                                                       "str#",  "strL",    "numeric"};
      std::istringstream words(*dsTypeFilter);
      std::vector<std::string> named;
      std::string rest;
      for (std::string w; words >> w;) {
        if (std::find(dsTypes.begin(), dsTypes.end(), w) != dsTypes.end() ||
            std::find(named.begin(), named.end(), w) != named.end())
          continue;  // `list uniq`
        if (kTypeWords.count(w)) named.push_back(w);
        else rest += (rest.empty() ? "" : " ") + w;
      }
      if (named.empty() && rest.empty()) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid " + optName + "() option",
                                                std::nullopt, std::nullopt, std::nullopt, "r(198)")));
        return true;
      }
      dsTypes = named;
      if (!rest.empty()) {
        std::vector<std::string> widths;
        bool ok = piet::loops::expandNumlist(rest, &widths);
        for (const auto& wd : widths) {
          char* end = nullptr;
          const double v = std::strtod(wd.c_str(), &end);
          ok = ok && end != nullptr && *end == '\0' && v == std::floor(v) && v >= 1 && v <= 2045;
        }
        if (!ok) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid variable type(s)",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  "r(198)")));
          return true;
        }
        for (const auto& wd : widths) dsTypes.push_back(wd);
      }
    }
    auto dsAllNames = [&](ojson* errOut) -> std::optional<std::vector<std::string>> {
      if (const std::vector<VarInfo>* card = session.residentReadCard()) {
        std::vector<std::string> out;
        for (const auto& vi : *card) out.push_back(vi.name);
        return out;
      }
      Datastore* dsp = session.datastore();
      if (dsp == nullptr) {
        *errOut = ojson(eval::noDataLoaded());
        return std::nullopt;
      }
      return dsp->names();
    };
    std::vector<std::string> names;
    if (parsed.varlist.empty()) {
      ojson dsErr;
      auto all = dsAllNames(&dsErr);
      if (!all.has_value()) {
        blocks.push_back(dsErr);
        return true;
      }
      // `ds, not` with no varlist has nothing to complement against -- same as
      // plain `ds` (every variable).
      names = *all;
    } else {
      auto vr = piet::loops::expandVarlist(joinVarlist(parsed.varlist), session);
      if (vr.isError) {
        blocks.push_back(vr.error);
        return true;
      }
      if (!dsNot) {
        names = vr.tokens;
        ojson dsErr;
        if (auto all = dsAllNames(&dsErr)) {
          for (const auto& n : names)
            if (std::find(all->begin(), all->end(), n) == all->end()) {
              blocks.push_back(ojson(eval::errorBlock("engine_error",
                                                      "variable " + n + " not found", std::nullopt,
                                                      std::nullopt, std::nullopt, "r(111)")));
              return true;
            }
        }
      } else {
        ojson dsErr;
        auto all = dsAllNames(&dsErr);
        if (!all.has_value()) {
          blocks.push_back(dsErr);
          return true;
        }
        // Complement, preserving dataset column order (matches runKeep).
        for (const auto& n : *all) {
          if (std::find(vr.tokens.begin(), vr.tokens.end(), n) == vr.tokens.end()) {
            names.push_back(n);
          }
        }
      }
    }
    if (dsTypeFilter.has_value()) {
      // ds_util.ado's Type: a variable joins once per matching type word, in
      // varlist order; has() keeps those, not() removes them.
      Datastore* dsp = session.datastore();
      if (dsp == nullptr) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return true;
      }
      std::vector<std::string> matched;
      for (const auto& x : names) {
        const bool isString = dsp->has(x) && dsp->dtype(x) == "factor";
        std::optional<std::string> declared;
        auto meta = session.varMeta.find(x);
        if (meta != session.varMeta.end() && meta->second.stataType.has_value() &&
            !meta->second.stataType->empty())
          declared = *meta->second.stataType;
        if (!declared.has_value() && !isString) declared = "float";  // describe's fallback
        for (const auto& w : dsTypes) {
          bool hit = false;
          if (w == "string") hit = isString;
          else if (w == "numeric") hit = !isString;
          else if (w == "alias" || w == "unknown") hit = false;
          else {
            if (!declared.has_value()) {
              blocks.push_back(ojson(eval::engineError(
                  "ds: the storage type of string variable " + x +
                      " is not recorded in this version of PIET, so has(type " + w +
                      ") cannot be decided",
                  "unsupported_option")));
              return true;
            }
            if (w == "str#") hit = isString && *declared != "strL";
            else hit = *declared == "str" + w || *declared == w;
          }
          if (hit) matched.push_back(x);
        }
      }
      if (dsTypeNot) {
        std::vector<std::string> kept;
        for (const auto& x : names)
          if (std::find(matched.begin(), matched.end(), x) == matched.end()) kept.push_back(x);
        names = kept;
      } else {
        names = matched;
      }
      if (names.empty()) {
        // ds_util.ado exits before `return local varlist`: r() is left empty.
        eval::replaceRClass(session.eval);
        eval::declareRPosted(session.eval, "ds", {});
        return true;
      }
    }
    // The metadata of the variables the filters and `detail` read (describe's own entries).
    std::map<std::string, ojson> dsMeta;
    if ((!dsMetaFilters.empty() || dsDetail) && !names.empty()) {
      ojson d = handlers::runDescribe(names, session);
      if (d.is_object() && d.contains("variables"))
        for (const auto& e : d["variables"]) dsMeta[e.value("name", std::string())] = e;
    }
    if (!dsMetaFilters.empty()) {
      auto fold = [&](std::string t) { return dsInsensitive ? lower(t) : t; };
      for (const auto& f : dsMetaFilters) {
        std::vector<std::string> pats;
        for (const auto& w : f.patterns) pats.push_back(fold(w));
        auto anyMatch = [&](const std::string& text, bool alsoPercent) {
          for (const auto& w : pats)
            if (strutil::globMatch(w, text) || (alsoPercent && strutil::globMatch("%" + w, text)))
              return true;
          return false;
        };
        if (f.kind == "format" && pats.empty()) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                                  std::string("invalid ") + (f.negate ? "not" : "has") + "() option",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  "r(198)")));
          return true;
        }
        std::vector<std::string> kept;
        for (const auto& x : names) {
          const ojson& e = dsMeta[x];
          bool hit = false;
          if (f.kind == "varlabel") {
            const std::string lab = e.value("label", std::string());
            hit = pats.empty() ? !lab.empty() : anyMatch(fold(lab), false);
          } else if (f.kind == "vallabel") {
            const std::string vl = e.value("val_label", std::string());
            hit = pats.empty() ? !vl.empty() : (!vl.empty() && anyMatch(fold(vl), false));
          } else if (f.kind == "format") {
            // a pattern matches the format as typed or with the percent sign put in front of it
            hit = anyMatch(fold(e.value("format", std::string())), true);
          } else {
            for (const auto& c : session.characteristics)
              if (c.varname == x && (pats.empty() || anyMatch(fold(c.charname), false)))
                hit = true;
          }
          if (hit != f.negate) kept.push_back(x);
        }
        names = std::move(kept);
      }
      if (names.empty()) {
        eval::replaceRClass(session.eval);
        eval::declareRPosted(session.eval, "ds", {});
        return true;
      }
    }
    if (dsAlpha) std::sort(names.begin(), names.end());
    std::string joined;
    for (size_t i = 0; i < names.size(); ++i) {
      if (i) joined += " ";
      joined += names[i];
    }
    // The text ds prints: describe's table under `detail`; otherwise one line, or, under
    // alpha / varwidth / skip / indent, the names in columns, filled down each column
    // (vs-stata bank: `ds, indent(6)`, `ds, skip(5)`, `ds, varwidth(20)`, `ds, alpha`).
    std::string dsText = joined;
    if (dsDetail) {
      auto padR = [](std::string t, size_t w) {
        if (t.size() < w) t.append(w - t.size(), ' ');
        return t;
      };
      std::string out = "Variable      Storage   Display    Value\n"
                        "    name         type    format    label      Variable label\n" +
                        std::string(80, '-');
      for (const auto& x : names) {
        const ojson& e = dsMeta[x];
        out += "\n" + padR(x, 16) + padR(e.value("dtype", std::string()), 8) +
               padR(e.value("format", std::string()), 11) +
               padR(e.value("val_label", std::string()), 11) + e.value("label", std::string());
        while (!out.empty() && out.back() == ' ') out.pop_back();
      }
      dsText = out;
    } else if (dsAlpha || dsVarwidth.has_value() || dsSkip.has_value() || dsIndent.has_value()) {
      size_t longest = 0;
      for (const auto& x : names) longest = std::max(longest, x.size());
      const int indent = dsIndent.value_or(0);
      const int vw = dsVarwidth.has_value() ? *dsVarwidth
                                            : static_cast<int>(longest) + dsSkip.value_or(2);
      const int width = 80;
      const int n = static_cast<int>(names.size());
      int maxCols = std::max(1, (width - indent) / std::max(1, vw));
      const int rows = (n + maxCols - 1) / maxCols;
      const int cols = rows > 0 ? (n + rows - 1) / rows : 0;
      std::string out;
      for (int r = 0; r < rows; ++r) {
        std::string line(static_cast<size_t>(indent), ' ');
        for (int c = 0; c < cols; ++c) {
          const int k = c * rows + r;
          if (k >= n) break;
          std::string cell = names[static_cast<size_t>(k)];
          if (static_cast<int>(cell.size()) < vw) cell.append(static_cast<size_t>(vw) - cell.size(), ' ');
          line += cell;
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += (r ? "\n" : "") + line;
      }
      dsText = out;
    }
    // r(varlist) is a STRING r-class result. captureRFromBlock has no string
    // branch (it only ever rebuilds the numeric store), so this is set directly —
    // the same one-liner `levelsof` uses for r(levels).
    session.eval.rStrings.set("varlist", joined);
    // BUG-1082: no `_rc` write here. Stata's _rc is sticky -- only an error or
    // `capture` sets it -- so `capture noisily renpfix tt u` (r(110)) then
    // `quietly ds` still reads 110 (w2ax_t1_official.log renpfix_collide).
    // BUG-1490: r(varlist) is ds's whole r() (StataBE 19.5, rlife/side2.do: `ds`
    // and `ds x*` leave r(macros) `varlist`, no scalar, no matrix).
    eval::declareRPosted(session.eval, "ds", {"varlist"});
    ojson dsOut;
    dsOut["type"] = "display";
    dsOut["text"] = dsText;
    blocks.push_back(std::move(dsOut));
    return true;
  }


  if (cmd == "unab") {
    // `unab lname : varlist` — a colon form, split on the first UNQUOTED colon the
    // same way the `frame NAME: CMD` arm does.
    std::string rest = trimWs(parsed.raw);
    {
      size_t sp = rest.find_first_of(" \t");
      rest = (sp == std::string::npos) ? std::string() : trimWs(rest.substr(sp + 1));
    }
    static const std::regex reUnabColon(R"(:)");
    std::smatch cm;
    if (!eval::searchUnquoted(rest, reUnabColon, cm)) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `unab` requires `unab lname : varlist`", "syntax_error")));
      return true;
    }
    std::string lname = trimWs(rest.substr(0, static_cast<size_t>(cm.position())));
    std::string spec = trimWs(rest.substr(static_cast<size_t>(cm.position()) + 1));
    if (lname.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `unab` requires `unab lname : varlist`", "syntax_error")));
      return true;
    }
    auto refuse = [&](const std::string& msg, int rc) {
      blocks.push_back(ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                              std::nullopt, "r(" + std::to_string(rc) + ")")));
      return true;
    };
    // `unab lname : [varlist] [, min(#) max(#) name(string)]` (StataBE 19.5,
    // vs-stata bank varlist_unab / varlist_cmds groups): the options follow the
    // first comma; any other word there is r(198) "option NAME not allowed"
    // (`price,mpg`, 3fb7e8f304f04d43).
    long minVars = 1, maxVars = -1;
    std::string listName;
    if (const size_t comma = spec.find(','); comma != std::string::npos) {
      const std::string optText = spec.substr(comma + 1);
      spec = trimWs(spec.substr(0, comma));
      static const std::regex reOpt(R"(^\s*([A-Za-z_]+)\s*(?:\(([^)]*)\))?)");
      std::string o = optText;
      while (!trimWs(o).empty()) {
        std::smatch om;
        if (!std::regex_search(o, om, reOpt))
          return refuse("invalid syntax", 198);
        const std::string k = lower(om.str(1));
        const std::string v = trimWs(om.str(2));
        if ((k == "min" || k == "max") && om[2].matched) {
          char* end = nullptr;
          const long n = std::strtol(v.c_str(), &end, 10);
          if (v.empty() || end == nullptr || *end != '\0') return refuse("invalid syntax", 198);
          (k == "min" ? minVars : maxVars) = n;
        } else if (k == "name" && om[2].matched) {
          listName = v;
        } else {
          return refuse("option " + om.str(1) + " not allowed", 198);
        }
        o = o.substr(static_cast<size_t>(om.position(0) + om.length(0)));
      }
    }
    std::vector<std::string> known;
    if (const std::vector<VarInfo>* card = session.residentReadCard()) {
      for (const auto& vi : *card) known.push_back(vi.name);
    } else if (Datastore* udsp = session.datastore()) {
      known = udsp->names();
    } else if (!spec.empty()) {
      blocks.push_back(ojson(eval::noDataLoaded()));
      return true;
    }
    const bool abbrevOn = !varAbbreviationOff(session);
    // The variable a name stands for: itself, else (abbreviation on) the one
    // variable it begins; -1 none, -2 several.
    auto resolve = [&](const std::string& tok) -> long {
      long hit = -1;
      for (size_t k = 0; k < known.size(); ++k) {
        if (known[k] == tok) return static_cast<long>(k);
        if (abbrevOn && known[k].size() > tok.size() && known[k].compare(0, tok.size(), tok) == 0)
          hit = hit == -1 ? static_cast<long>(k) : -2;
      }
      return hit;
    };
    auto refuseName = [&](const std::string& tok, long at) {
      return refuse(at == -2 ? tok + " ambiguous abbreviation" : "variable " + tok + " not found", 111);
    };
    // Parentheses only group (`(price mpg)` is price mpg, 024522b260a37e2a); a
    // `-` may carry blanks on either side.
    std::string flat;
    for (size_t i = 0; i < spec.size(); ++i) {
      const char c = spec[i] == '(' || spec[i] == ')' ? ' ' : spec[i];
      if (c == '-') {
        while (!flat.empty() && std::isspace(static_cast<unsigned char>(flat.back()))) flat.pop_back();
        flat.push_back('-');
        while (i + 1 < spec.size() && std::isspace(static_cast<unsigned char>(spec[i + 1]))) ++i;
        continue;
      }
      flat.push_back(c);
    }
    std::vector<std::string> names;
    std::istringstream ts(flat);
    for (std::string tok; ts >> tok;) {
      if (lower(tok) == "_all") {
        names.insert(names.end(), known.begin(), known.end());
        continue;
      }
      // `L.price`, `i.foreign`: r(101) (StataBE 19.5, vs-stata bank
      // 482f681193482389, b6008c3cf5fd5632).
      if (tok.find_first_of(".#") != std::string::npos)
        return refuse("factor-variable and time-series operators not allowed", 101);
      if (const size_t dash = tok.find('-'); dash != std::string::npos) {
        // `price-` and `-price` are r(198) "nothing found where name expected";
        // a range end that is no name (`pr*-rep78`) r(198) "- invalid name"
        // (b30044e1c0cca968, 62d2511ec9a4badc).
        if (dash == 0 || dash + 1 == tok.size())
          return refuse("nothing found where name expected", 198);
        const std::string a = tok.substr(0, dash), b = tok.substr(dash + 1);
        if (!strutil::isValidStataIdentifier(a) || !strutil::isValidStataIdentifier(b))
          return refuse("- invalid name", 198);
        const long ia = resolve(a);
        if (ia < 0) return refuseName(a, ia);
        const long ib = resolve(b);
        if (ib < 0) return refuseName(b, ib);
        if (ia > ib) return refuse("variables out of order", 111);
        for (long k = ia; k <= ib; ++k) names.push_back(known[static_cast<size_t>(k)]);
        continue;
      }
      if (tok.find_first_of("*?~") != std::string::npos) {
        // `~` matches like `*` but must name exactly one variable (`g~o` is
        // gear_ratio), whatever `set varabbrev` says (bb74262d7439e51b).
        std::string pat = tok;
        std::replace(pat.begin(), pat.end(), '~', '*');
        std::vector<std::string> hits;
        for (const auto& v : known)
          if (strutil::globMatch(pat, v)) hits.push_back(v);
        if (hits.empty()) return refuse("variable " + tok + " not found", 111);
        if (hits.size() > 1 && tok.find('~') != std::string::npos)
          return refuse(tok + " ambiguous abbreviation", 111);
        names.insert(names.end(), hits.begin(), hits.end());
        continue;
      }
      if (!strutil::isValidStataIdentifier(tok)) return refuse(tok + " invalid name", 198);
      const long at = resolve(tok);
      if (at < 0) return refuseName(tok, at);
      names.push_back(known[static_cast<size_t>(at)]);
    }
    // min() / max(): too few r(102), too many r(103) (unab x : , empty; `max(2)`
    // over three names; 3848f3cdf174e5fe, b3f8e962d0dcba95, 4408c8c3f53be608).
    const std::string who = listName.empty() ? trimWs(spec) + ":" : listName + ":";
    if (static_cast<long>(names.size()) < minVars)
      return refuse(who + "\ntoo few variables specified", 102);
    if (maxVars >= 0 && static_cast<long>(names.size()) > maxVars)
      return refuse(who + "\ntoo many variables specified", 103);
    struct {
      std::vector<std::string> tokens;
    } vr{names};
    std::string joined;
    for (size_t i = 0; i < vr.tokens.size(); ++i) {
      if (i) joined += " ";
      joined += vr.tokens[i];
    }
    session.eval.locals.set(lname, joined);
    // BUG-1082: and no `_rc` write -- a successful unab leaves _rc as it was.
    return true;  // Stata's unab prints nothing; it defines the local.
  }
  return false;
}

}
