// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {
bool route_extensions(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {


  // ==================== AER-SCOPE1 bundle C (GTOOLS) arms =================
  // Reached only for the forms the translation step could NOT fold onto a
  // native command: `gstats` is a multiplex over two different natives, and
  // `gquantiles` in its `_pctile`/`pctile` form has no native twin at all
  // (the `xtile` form was already rewritten to `xtile` upstream).
  if (cmd == "gstats") {
    ojson r = handlers::runGstats(parsed, session);
    // `gstats sum` renders summarize's table, so it must also PUBLISH what
    // summarize publishes. r() is filled at the dispatcher call site, not
    // inside the handler (the summarize arm above does exactly this), so a
    // forwarding arm that only pushed the block would draw the same table and
    // leave r(N)/r(mean) empty -- and `gstats sum x` followed by
    // `display r(mean)` is precisely how the corpus uses it.
    if (r.is_object() && r.value("type", std::string()) != "error") {
      eval::captureRFromBlock(r, session.eval);
    }
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return true;
  }

  if (cmd == "gquantiles") {
    ojson r = handlers::runGQuantilesPctile(parsed, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return true;
  }

  if (cmd == "strdist") {
    // AER scope 2 B2: the last name blocking paper 100.
    pushEnvResult(blocks, handlers::runStrdist(parsed, session));
    return true;
  }

  if (cmd == "tolower" || cmd == "renvars" || cmd == "move") {
    // pushEnvResult flattens an array and drops nulls, which is the shape all
    // three return: an array of display blocks (possibly empty, which is how
    // the silent path says "nothing to print").
    pushEnvResult(blocks, cmd == "tolower"  ? handlers::runTolower(parsed, session)
                  : cmd == "renvars" ? handlers::runRenvars(parsed, session)
                                     : handlers::runMove(parsed, session));
    return true;
  }

  if (cmd == "geodist") {
    pushEnvResult(blocks, handlers::runGeodist(parsed, session));
    return true;
  }

  if (cmd == "geonear") {
    pushEnvResult(blocks, handlers::runGeonear(parsed, session));
    return true;
  }


  // ==================== AER-SCOPE1 bundle I (UTIL) arms =====================
  if (cmd == "unique" || cmd == "gunique") {
    // One arm, two spellings: gtools' `gunique` differs from `unique` only in
    // name, and giving it its own handler would be a second thing to keep in
    // step. The handler reads `parsed.cmd` for the two options that ARE
    // gunique-only and refuses them by name.
    pushEnvResult(blocks, handlers::runUnique(parsed, session));
    return true;
  }

  if (cmd == "distinct") {
    pushEnvResult(blocks, handlers::runDistinct(parsed, session));
    return true;
  }

  if (cmd == "missings") {
    pushEnvResult(blocks, handlers::runMissings(parsed, session));
    return true;
  }

  if (cmd == "mdesc") {
    pushEnvResult(blocks, handlers::runMdesc(parsed, session));
    return true;
  }

  if (cmd == "labmask") {
    pushEnvResult(blocks, handlers::runLabmask(parsed, session));
    return true;
  }

  if (cmd == "renvarlab") {
    pushEnvResult(blocks, handlers::runRenvarlab(parsed, session));
    return true;
  }

  if (cmd == "splitvallabels") {
    pushEnvResult(blocks, handlers::runSplitvallabels(parsed, session));
    return true;
  }

  if (cmd == "marksample") {
    pushEnvResult(blocks, handlers::runMarksample(parsed, session));
    return true;
  }

  if (cmd == "filelist") {
    pushEnvResult(blocks, handlers::runFilelist(parsed, session));
    return true;
  }

  if (cmd == "carryforward") {
    pushEnvResult(blocks, handlers::runCarryforward(parsed, session));
    return true;
  }

  if (cmd == "lookfor") {
    pushEnvResult(blocks, handlers::runLookfor(parsed, session));
    return true;
  }

  if (cmd == "ihstrans") {
    pushEnvResult(blocks, handlers::runIhstrans(parsed, session));
    return true;
  }

  if (cmd == "sctorezone") {
    pushEnvResult(blocks, handlers::runSctorezone(parsed, session));
    return true;
  }

  if (cmd == "rangestat") {
    pushEnvResult(blocks, handlers::runRangestat(parsed, session));
    return true;
  }

  if (cmd == "tsspell") {
    pushEnvResult(blocks, handlers::runTsspell(parsed, session));
    return true;
  }

  // ==================== AER tier-1 arms (2026-10-28) =====================
  // One arm per command, each through pushEnvResult (handlers may answer an
  // array of blocks). `macro` has no arm: tryMacroVerbIntercept consumes it
  // before the tokenizer, whose unsupported_macro net would otherwise fire.
  {
    using TierOneHandler = ojson (*)(const eval::ParsedCommand&, SessionState&);
    static const std::map<std::string, TierOneHandler> kTierOne = {
        {"unzipfile", &handlers::runUnzipfile},   {"zipfile", &handlers::runZipfile},
        {"zipuse", &handlers::runZipuse},         {"zipsave", &handlers::runZipsave},
        {"require", &handlers::runRequire},       {"_dots", &handlers::runDots},
        {"postutil", &handlers::runPostutil},     {"markout", &handlers::runMarkout},
        {"lrtest", &handlers::runLrtest},         {"findname", &handlers::runFindname},
        {"xtdescribe", &handlers::runXtdescribe}, {"cf", &handlers::runCf},
        {"fvunab", &handlers::runFvunab},         {"numlabel", &handlers::runNumlabel},
        {"labvars", &handlers::runLabvars},       {"renpfix", &handlers::runRenpfix},
        {"mvdecode", &handlers::runMvdecode},     {"integ", &handlers::runInteg},
        {"sencode", &handlers::runSencode},       {"fillmissing", &handlers::runFillmissing},
        {"astile", &handlers::runAstile},         {"matsort", &handlers::runMatsort},
        {"matselrc", &handlers::runMatselrc},
    };
    auto it1 = kTierOne.find(cmd);
    if (it1 != kTierOne.end()) {
      // The registry's allowIf is descriptive only (see the hausman note
      // below), so a row that takes no `if` refuses it here, before the
      // handler: Stata answers "if not allowed", r(101) (StataBE, review of
      // the tier-1 META lane: cf, fvunab, numlabel). renpfix's ado parses the
      // qualifier as a third word, r(198); labvars reads it as label text and
      // so judges it itself.
      const registry::CommandSpec* t1spec = registry::findByName(cmd);
      if (parsed.ifExpr.has_value() && t1spec != nullptr && !t1spec->allowIf &&
          cmd != "labvars") {
        const bool asSyntax = (cmd == "renpfix");
        // cf is r-class: Stata's refusal still replaces r() with nothing
        // (META review p3, `cf x using f if x > 1` then r(Nsum) is `.`).
        if (cmd == "cf") {
          eval::replaceRClass(session.eval);
          eval::declareRPosted(session.eval, "cf", {});
        }
        blocks.push_back(ojson(eval::errorBlock(
            "syntax_error", asSyntax ? "invalid syntax" : "if not allowed", std::nullopt,
            std::nullopt, std::nullopt, asSyntax ? "r(198)" : "r(101)")));
        return true;
      }
      pushEnvResult(blocks, it1->second(parsed, session));
      return true;
    }
  }

  if (cmd == "odbc") {
    blocks.push_back(ojson(eval::refusedCommand(cmdTyped, "no_odbc_driver")));
    return true;
  }

  if (cmd == "spshape2dta") {
    blocks.push_back(ojson(eval::refusedCommand(cmdTyped, "no_shapefile_reader")));
    return true;
  }


  // ==================== AER-SCOPE1 bundle F (RESHAPE) arms ==================
  // Shape and sample verbs. Three of them round-trip the whole frame through a
  // CSV and therefore go through the pre-flight gate first -- the same shape
  // the `expand` arm above uses: reject net, then pre-flight, then handler.
  // `pushEnvResult` flattens and drops nulls (defect class 1).
  if (cmd == "split") {
    pushEnvResult(blocks, handlers::runSplit(parsed, session));
    return true;
  }

  if (cmd == "xpose") {
    if (preflight::gate("xpose", session, "", 1, blocks) == preflight::Verdict::Cancel) return true;
    pushEnvResult(blocks, handlers::runXpose(parsed, session));
    return true;
  }

  if (cmd == "recast") {
    // format-display-default-lifecycle: recast moves the storage type, never the
    // display format (`recast double y` of a float stays %9.0g,
    // internal capture exprfns_contract_oracle.log:735). Each named
    // variable's current format is pinned before runRecast moves the type (a
    // string's default instead follows the new width: prepareStorageChange),
    // and a refused recast takes the pins back.
    std::map<std::string, std::optional<VarMeta>> priorMeta;
    if (parsed.varlist.size() >= 2 && session.hasData()) {
      std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
      Datastore* ds = nullptr;
      try {
        ds = session.datastore();
      } catch (const std::exception&) {
        ds = nullptr;  // runRecast reports the load failure itself
      }
      if (ds != nullptr) {
        for (const VarInfo& v : ds->formatCard(names)) {
          auto it = session.varMeta.find(v.name);
          priorMeta[v.name] =
              it == session.varMeta.end() ? std::nullopt : std::optional<VarMeta>(it->second);
          session.prepareStorageChange(v);
        }
      }
    }
    ojson r = handlers::runRecast(parsed, session);
    if (r.is_object() && r.value("type", std::string()) == "error") {
      for (const auto& [name, meta] : priorMeta) {
        if (meta.has_value()) session.varMeta[name] = *meta;
        else session.varMeta.erase(name);
      }
    }
    pushEnvResult(blocks, r);
    return true;
  }

  if (cmd == "insobs") {
    pushEnvResult(blocks, handlers::runInsobs(parsed, session));
    return true;
  }

  if (cmd == "stack") {
    {
      // The pre-flight factor is the number of variables being stacked, which
      // is how many copies of the row count the result carries.
      const int64_t k = static_cast<int64_t>(parsed.varlist.size());
      if (preflight::gate("stack", session, "", k > 0 ? k : 1, blocks) == preflight::Verdict::Cancel)
        return true;
    }
    pushEnvResult(blocks, handlers::runStack(parsed, session));
    return true;
  }

  if (cmd == "separate") {
    pushEnvResult(blocks, handlers::runSeparate(parsed, session));
    return true;
  }

  if (cmd == "mvencode") {
    pushEnvResult(blocks, handlers::runMvencode(parsed, session));
    return true;
  }

  if (cmd == "sample") {
    pushEnvResult(blocks, handlers::runSample(parsed, session));
    return true;
  }

  if (cmd == "splitsample") {
    pushEnvResult(blocks, handlers::runSplitsample(parsed, session));
    return true;
  }

  if (cmd == "bsample") {
    if (preflight::gate("bsample", session, "", 1, blocks) == preflight::Verdict::Cancel)
      return true;
    ojson bs = ojson(handlers::runBsample(parsed, session));
    // e(sample) T2: Stata keeps the marker on one copy of each drawn observation,
    // which one following its own sort (StataBE capture w2_esample_copies X2
    // `10|61|m0=20`): named, never guessed.
    bool bsFailed = false;
    for (const auto& b : bs.is_array() ? bs : ojson::array({bs}))
      bsFailed = bsFailed || (b.is_object() && b.value("type", std::string()) == "error");
    pushEnvResult(blocks, bs);
    // The fast method (bsampleIsStataFastPath) draws Stata's own rows and keeps the
    // marker on them itself; the SRSWR/cluster paths draw PIET's rows, so the
    // marker Stata would have kept is not knowable there and is withdrawn.
    if (!bsFailed && !handlers::bsampleIsStataFastPath(parsed) &&
        !handlers::esampleMarkHeadLost(session, EsampleLostBy::kBsample)) {
      blocks.push_back(ojson(eval::engineError(
          "bsample: the sample was drawn, but the estimation-sample marker could not be "
          "withdrawn from it; e(sample) is not reliable until the next estimation.")));
    }
    return true;
  }

  if (cmd == "drawnorm") {
    pushEnvResult(blocks, handlers::runDrawnorm(parsed, session));
    return true;
  }

  if (cmd == "input") {
    // A FOLDED `input` block was intercepted on the raw line far above; only a
    // bare `input` with no block reaches here, and it gets an honest error
    // rather than silently doing nothing.
    pushEnvResult(blocks, handlers::runInput(parsed.raw, session));
    return true;
  }

  if (cmd == "notes") {
    pushEnvResult(blocks, handlers::runNotes(parsed, session));
    return true;
  }

  if (cmd == "compare") {
    pushEnvResult(blocks, handlers::runCompare(parsed, session));
    return true;
  }

  if (cmd == "cumul") {
    pushEnvResult(blocks, handlers::runCumul(parsed, session));
    return true;
  }

  if (cmd == "pctile") {
    pushEnvResult(blocks, handlers::runPctile(parsed, session));
    return true;
  }

  if (cmd == "_pctile") {
    // r-class: `_pctile x, p(10 90)` publishes r(r1)/r(r2) and touches no
    // column. The handler fills r() itself here (it is the whole point of the
    // command), so unlike `gstats sum` this arm does not call
    // captureRFromBlock -- there is no table block to capture FROM.
    pushEnvResult(blocks, handlers::runPctileR(parsed, session));
    return true;
  }
  return false;
}

}
