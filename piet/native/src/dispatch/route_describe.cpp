// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "summary_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
#include "piet/handlers/descriptive.h"
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
    eval::replaceRClass(session.eval);
    eval::declareRPosted(session.eval, "codebook", {});
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
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabDescribe.known, kVocabDescribe.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    if (!session.hasData()) {
      // Mission 5: empty description (0 vars), matching real Stata — see
      // the kNoDataCommands comment above for why this is handled here
      // rather than in handlers::runDescribe() itself.
      ojson emptyDescribe;
      emptyDescribe["type"] = "describe";
      emptyDescribe["variables"] = ojson::array();
      // BUG-1447: an empty dataset is described too (StataBE 19.5 after `clear`:
      // r(N) 0, r(k) 0, r(width) 0, r(changed) 0).
      session.eval.rValues.set("N", 0.0);
      session.eval.rValues.set("k", 0.0);
      session.eval.rValues.set("width", 0.0);
      session.eval.rValues.set("changed", session.datasetChanged ? 1.0 : 0.0);
      eval::declareRPosted(session.eval, "describe", {"N", "k", "width", "changed"});
      blocks.push_back(emptyDescribe);
      return true;
    }
    std::optional<std::vector<std::string>> vl;
    if (!parsed.varlist.empty()) vl = parsed.varlist;
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
      session.eval.rValues.set("N", static_cast<double>(nObs));
      // BUG-1447: r(k) and r(width) are the dataset's (describeDatasetShape).
      const DescribeShape shape = describeDatasetShape(session);
      session.eval.rValues.set("k", static_cast<double>(shape.k));
      if (shape.width.has_value()) session.eval.rValues.set("width", *shape.width);
      session.eval.rValues.set("changed", session.datasetChanged ? 1.0 : 0.0);
      if (dVarlist) {
        std::string joined;
        for (const auto& nm : dnames) joined += (joined.empty() ? "" : " ") + nm;
        session.eval.rStrings.set("varlist", joined);
        std::string sortlist;
        if (Datastore* dds = session.datastore()) sortlist = dds->sortedBy();
        session.eval.rStrings.set("sortlist", sortlist);
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
        eval::declareRPosted(session.eval, "describe", {"N", "k", "width", "changed"});
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
    // The row count is read off the RESIDENT CARD, never via session.datastore().
    // describe's whole D1 card-plane design is that a factor-only describe
    // constructs ZERO Datastores, and a construct here would have silently
    // undone that optimization for every describe — the C4-L2B MECHANISM gate
    // caught exactly that when this first went in. When the card cannot answer
    // (no resident card / not current), r(N) is left alone rather than forcing
    // the construct: an honest absence beats a hidden cost. r(k) and r(width)
    // come off that card too, or else off the Datastore runDescribe above
    // already opened (describeDatasetShape), so they cost no construct.
    {
      if (std::optional<int64_t> rows = session.residentRowsIfCurrent()) {
        session.eval.rValues.set("N", static_cast<double>(*rows));
      }
      // BUG-1447: r(k) and r(width) are the dataset's, r(changed) is c(changed)
      // (StataBE 19.5: r(changed) 1 after edits, 0 right after `save` or `use`).
      const DescribeShape shape = describeDatasetShape(session);
      session.eval.rValues.set("k", static_cast<double>(shape.k));
      if (shape.width.has_value()) session.eval.rValues.set("width", *shape.width);
      session.eval.rValues.set("changed", session.datasetChanged ? 1.0 : 0.0);
      // LBL-DATALABEL: describe posts r(datalabel) when the dataset has a label
      // (w2ao_label_family.log lb_data; lb_var_label reads "" without one). The
      // posted set is declared, so an absent r(datalabel) expands to "" instead
      // of the unknown-set refusal; an r(width) of unknown string widths refuses
      // by name.
      std::vector<std::string> posted{"N", "k", "width", "changed"};
      if (!session.dataLabel.empty()) {
        session.eval.rStrings.set("datalabel", session.dataLabel);
        posted.push_back("datalabel");
      } else {
        session.eval.rStrings.erase("datalabel");
      }
      eval::declareRPosted(session.eval, "describe", std::move(posted));
    }
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
    blocks.push_back(ojson(handlers::runList(vl, parsed.ifExpr, parsed.inRange, session)));
    return true;
  }


  if (cmd == "levelsof") {
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError("levelsof: variable name required")));
      return true;
    }
    std::optional<std::string> localName;
    bool clean = false;
    // BS-029 (BETA Phase C batch C1): `missing`/`separate()` are now
    // implemented (previously honest-rejected as unknown options).
    bool missing = false;
    std::optional<std::string> separateSep;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "local" && !v.isBool) localName = v.strValue;
      if (k == "clean" && v.isBool && v.boolValue) clean = true;
      if (k == "missing" && v.isBool && v.boolValue) missing = true;
      if (k == "separate" && !v.isBool) separateSep = v.strValue;
    }
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kLevelsofKnownOpts = kVocabLevelsof.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kLevelsofKnownOpts)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    ojson result = ojson(
        handlers::runLevelsof(parsed.varlist[0], localName, clean, parsed.ifExpr, session));
    if (result.value("type", "") == "levelsof" && result.contains("levels") &&
        result["levels"].is_array()) {
      // handlers::runLevelsof (descriptive/levels_command.cpp) has no
      // missing/separate support at all: its numeric/factor
      // scan explicitly drops NaN/negative codes (missing) and always
      // space-joins. Both are applied here as a post-process over its
      // `levels` array instead of duplicating that scan. `missing` appends
      // the dataset's own missing marker (".") after the ordinary
      // (non-missing) level list — Stata sorts missing last, so simple
      // append-at-end is correct; `separate(SEP)` re-joins whatever level
      // list results with SEP instead of the default space.
      std::vector<std::string> levelList;
      for (const auto& l : result["levels"]) levelList.push_back(l.get<std::string>());
      if (missing) {
        // NOTE: checks the dataset's UNFILTERED missingness — combining
        // `missing` with an `if` that itself changes whether the variable
        // still has missing rows is a known, unexercised edge case (the
        // oracle's `missing` probe carries no `if`).
        Datastore* ds = session.datastore();
        if (ds != nullptr && ds->has(parsed.varlist[0]) && ds->nMissing(parsed.varlist[0]) > 0) {
          levelList.push_back(".");
        }
      }
      std::string sep = separateSep.has_value() ? *separateSep : " ";
      std::string joined;
      for (size_t i = 0; i < levelList.size(); ++i) {
        if (i) joined += sep;
        joined += levelList[i];
      }
      result["levels"] = ojson(levelList);
      result["text"] = joined;
      // r(levels): Stata stores levelsof's level list as a STRING r-class
      // result, read back via the `` `r(levels)' `` macro-delimited form
      // (eval/macros.cpp's rStrings lookup) — this codebase never actually
      // wired that up (runLevelsof only ever sets the OPTIONAL local()
      // name, never r(levels) itself); wire it here since this is the only
      // site that knows the FINAL (missing/separate-adjusted) joined text.
      session.eval.rStrings.set("levels", joined);
      // BUG-373: Stata also publishes the counts. r(r) is the number of DISTINCT
      // levels and r(N) the number of observations they were drawn from — the two
      // a macro-driven do-file actually loops on.
      session.eval.rValues.set("r", static_cast<double>(levelList.size()));
      if (std::optional<int64_t> lvRows = session.residentRowsIfCurrent())
        session.eval.rValues.set("N", static_cast<double>(*lvRows));
      else if (Datastore* lvDs = session.datastore())
        session.eval.rValues.set("N", static_cast<double>(lvDs->nRows()));
      // Re-set local(name) too, in case runLevelsof already set it above
      // with the PRE-adjustment (missing/separate-unaware) text.
      if (localName.has_value() && !localName->empty()) {
        session.eval.locals.set(*localName, joined);
      }
      // levelsof's help; RULING.md D.3.11.
      eval::declareRPosted(session.eval, "levelsof", {"N", "r", "levels"});
    }
    blocks.push_back(result);
    return true;
  }


  if (cmd == "xtsum") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    blocks.push_back(ojson(handlers::runXtsum(parsed.varlist, session)));
    return true;
  }


  if (cmd == "xttab") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    blocks.push_back(ojson(handlers::runXttab(parsed.varlist, session)));
    return true;
  }


  if (cmd == "inspect") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    ojson blk = ojson(handlers::runInspect(parsed.varlist, session));
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
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabDs.known, kVocabDs.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    bool dsNot = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "not" && v.isBool && v.boolValue) dsNot = true;
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
    std::string joined;
    for (size_t i = 0; i < names.size(); ++i) {
      if (i) joined += " ";
      joined += names[i];
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
    dsOut["text"] = joined;
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
    if (lname.empty() || spec.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `unab` requires `unab lname : varlist`", "syntax_error")));
      return true;
    }
    auto vr = piet::loops::expandVarlist(spec, session);
    if (vr.isError) {
      blocks.push_back(vr.error);
      return true;
    }
    // Stata's unab errors when a name does not exist. expandVarlist only proves
    // that for a WILDCARD (a bare unknown name passes straight through), so the
    // literal names are checked here too rather than defining a local that names
    // a column the dataset does not have.
    //
    // The card is the cheap source but it is not always live, and "no card" must
    // NOT mean "skip the check" — that is how an unchecked name slips through
    // (caught by this lock's own (c) case, which failed until the datastore
    // fallback below was added).
    {
      std::vector<std::string> known;
      if (const std::vector<VarInfo>* card = session.residentReadCard()) {
        for (const auto& vi : *card) known.push_back(vi.name);
      } else if (Datastore* udsp = session.datastore()) {
        known = udsp->names();
      } else {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return true;
      }
      for (const auto& t : vr.tokens) {
        if (std::find(known.begin(), known.end(), t) == known.end()) {
          blocks.push_back(
              ojson(eval::engineError("variable " + t + " not found", "unknown_variable")));
          return true;
        }
      }
    }
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
