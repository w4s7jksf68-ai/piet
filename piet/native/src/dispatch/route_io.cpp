// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {
bool route_io(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {
  if (cmd == "use") {
    // `nolabel` retains labels/attachments but omits definition tables. Parse
    // all options before extracting the filename (lb_rt_nolabel uses both).
    const std::unordered_set<std::string>& kUseKnownOpts = kVocabUse.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kUseKnownOpts)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    std::string raw = parsed.raw;
    // BUG-251/WS-Q: was `lowerRaw.find("use")` + `substr(pos + 3)` — a LITERAL
    // spelling, unguarded. `use` really is abbreviable in Stata (measured
    // 2026-08-07 on StataBE: `u` and `us` both answer rc 0), so the moment the
    // registry row gained the abbreviations this threw on the line `u`.
    std::string afterUse = rawArgsAfterCmd(raw);
    bool useClears = false, useNoLabel = false;
    for (const auto& [key, value] : parsed.opts) {
      if (value.isBool && !value.boolValue) continue;
      if (key == "clear" || key == "replace") useClears = true;
      else if (key == "nolabel") useNoLabel = true;
    }
    // A comma inside a quoted filename is part of the filename.
    char quote = 0;
    for (size_t i = 0; i < afterUse.size(); ++i) {
      if (quote) { if (afterUse[i] == quote) quote = 0; }
      else if (afterUse[i] == '"') quote = afterUse[i];
      else if (afterUse[i] == ',') { afterUse.resize(i); break; }
    }
    // [D] use: without `clear`, data changed since they were last saved are
    // not discarded -- r(4) "no; dataset in memory has changed since last
    // saved", use's own wording (w2ar_data_io_family.log:4368
    // dx_u_noclear_changed `4|k=7|N=4|`; after `clear` the same line loads,
    // dx_u_077_noclear). The import verbs' r(4) says "data in memory would be lost".
    if (!useClears && session.hasData() && session.datasetChanged) {
      blocks.push_back(ojson(eval::errorBlock("engine_error",
                                              "no; dataset in memory has changed since last saved",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              std::string("r(4)"))));
      return true;
    }
    size_t b = afterUse.find_last_not_of(" \t");
    afterUse = (b == std::string::npos) ? "" : afterUse.substr(0, b + 1);
    std::string filepath = !afterUse.empty() ? afterUse
                           : (!parsed.varlist.empty() ? parsed.varlist[0] : "");
    if (filepath.empty()) {
      blocks.push_back(ojson(eval::engineError("syntax_error: `use` requires a filename", "syntax_error")));
      return true;
    }
    ojson useResult = handlers::runUse(filepath, session);
    if (useNoLabel && useResult.value("type", std::string()) != "error") {
      session.labelDefs.clear();
      session.valueLabels.clear();
      session.labelSeq.clear();
      // runUse rendered its card before nolabel withdrew the definitions.
      if (useResult.contains("varlist_after"))
        useResult["varlist_after"] = handlers::mutationResult(session, "use")["varlist_after"];
    }
    // BUG A / hbin-sniff honesty note: runUse() attaches a "note" field to
    // its own block when it had to load a mismatched-extension file by
    // sniffing hbin content — lift it into its OWN prepended, UNPREFIXED
    // echo block (same pattern xtset/tsset/cd/pwd/clear already use for a
    // second informational echo — see fuzz_sequences.py's own doc-comment:
    // this is NOT a new command boundary, only a ".  "-prefixed echo is).
    if (useResult.contains("note")) {
      ojson noteBlock;
      noteBlock["type"] = "echo";
      noteBlock["text"] = useResult["note"].get<std::string>();
      blocks.push_back(noteBlock);
      useResult.erase("note");
    }
    blocks.push_back(useResult);
    return true;
  }


  // ------------------------------------------------------------------
  // Mission 3 (hardening-queue item, not in BUGLOG): `sysuse DATASET[,
  // clear]` / `sysuse dir`. Real Stata's `sysuse` loads one of a whole
  // library of named sample datasets by bare name (no path); PIET ships
  // exactly one today — piet/packaging/sample/fatalities.csv, the SAME
  // fixture main.cpp's runUiSmoke() already loads (bundle-first via
  // piet::bundleResourcesDir()+"/sample/fatalities.csv", falling back to
  // the dev-tree piet/packaging/sample/fatalities.csv — see main.cpp
  // ~957-968 and its resolveFrontendIndexPath() sibling ~252-269). This
  // arm mirrors that exact bundle-first/dev-tree-fallback order via the
  // fileExistsLocal()/devTreeSampleFatalitiesCsv() helpers above rather
  // than inventing a second convention. Trailing-token arity (1 token —
  // DATASET or `dir`) is enforced by mission 1's shared kFixedArityCommands
  // table above, not re-checked here.
  if (cmd == "sysuse") {
    if (!parsed.varlist.empty() && lower(parsed.varlist[0]) == "dir") {
      ojson dirOut;
      dirOut["type"] = "display";
      dirOut["text"] = "fatalities";
      blocks.push_back(dirOut);
      return true;
    }
    std::string dsArg = parsed.varlist.empty() ? std::string() : parsed.varlist[0];
    if (dsArg.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `sysuse` requires a dataset name (or `sysuse dir` to list what's available)",
          "syntax_error")));
      return true;
    }
    std::string dsLower = lower(dsArg);
    static const std::string kDtaSuffix = ".dta";
    if (dsLower.size() > kDtaSuffix.size() &&
        dsLower.compare(dsLower.size() - kDtaSuffix.size(), kDtaSuffix.size(), kDtaSuffix) == 0) {
      dsLower = dsLower.substr(0, dsLower.size() - kDtaSuffix.size());
    }
    if (dsLower != "fatalities") {
      blocks.push_back(ojson(eval::engineError(
          "sysuse: unknown dataset `" + dsArg + "` — the only dataset bundled with this version of "
              "PIET is `fatalities` (run `sysuse dir` to list what's available).",
          "engine_error")));
      return true;
    }
    std::string sampleCsv;
    if (std::optional<std::string> resources = bundleResourcesDir()) {
      std::string bundled = *resources + "/sample/fatalities.csv";
      if (fileExistsLocal(bundled)) sampleCsv = bundled;
    }
    if (sampleCsv.empty()) sampleCsv = devTreeSampleFatalitiesCsv();
    blocks.push_back(handlers::runUse(sampleCsv, session));
    return true;
  }


  if (cmd == "save") {
    std::string raw = parsed.raw;
    // BUG-105/WS-K: search for the TYPED spelling, not the literal "save".
    // `sav "f.dta"` made find("save") return npos and substr(npos + 4) threw
    // std::out_of_range ("basic_string") — a hard crash of the whole run, not a
    // reject. Found by the spot-check, not by reading: two arms re-derive their
    // arguments from the RAW line by locating the command word in it, so
    // normalising the routed token is necessary but NOT sufficient for them.
    // BUG-251/WS-Q: and searching for `cmdTyped` is still a search — correct
    // only by an invariant nothing here checks, and still unguarded against
    // npos. The tail never depended on the spelling at all.
    std::string after = rawArgsAfterCmd(raw);
    bool inQ = false;
    long commaIdx = -1;
    for (size_t i = 0; i < after.size(); ++i) {
      if (after[i] == '"') {
        inQ = !inQ;
      } else if (after[i] == ',' && !inQ) {
        commaIdx = static_cast<long>(i);
        break;
      }
    }
    bool replace = false;
    bool emptyOk = false;
    if (commaIdx >= 0) {
      std::string optsTail = trimWs(lower(after.substr(static_cast<size_t>(commaIdx) + 1)));
      after = trimWs(after.substr(0, static_cast<size_t>(commaIdx)));
      // Every `save` suboption besides `replace`/`emptyok` (nolabel/orphans/
      // version(#)) is unimplemented — reject the first one by name instead
      // of silently discarding the whole tail (BUG-141 class: a dropped
      // option looks identical to an honoured one from the caller's side).
      // B5-D9: `emptyok` is accepted and otherwise ignored -- see kVocabSave's
      // registration comment for why it is a validated no-op on this verb.
      std::istringstream optsStream(optsTail);
      std::string opt;
      while (optsStream >> opt) {
        if (opt == "replace") {
          replace = true;
          continue;
        }
        if (opt == "emptyok") {
          emptyOk = true;
          continue;
        }
        blocks.push_back(ojson(eval::unsupportedOption(
            opt, "`save` supports only `replace`/`emptyok` in this build; nolabel/orphans/"
                 "version() are not implemented.")));
        return true;
      }
    }
    std::string filepath = !after.empty() ? after
                           : (!parsed.varlist.empty() ? parsed.varlist[0] : "");
    if (filepath.empty()) {
      blocks.push_back(ojson(eval::engineError("syntax_error: `save` requires a filename")));
      return true;
    }
    // BUG-1458: a dataset with observations but no variables (`clear` / `set
    // obs 3`) is StataBE's r(111) "no variables defined" unless emptyok.
    if (!emptyOk) {
      // The card answers "no variables" without opening the frame (C4-L2A: the
      // save's entry must construct no Datastore and snapshot no dirty head).
      const std::vector<VarInfo>* card = session.residentReadCard();
      Datastore* ds = card != nullptr ? nullptr : session.datastore();
      if (card != nullptr ? card->empty() : (ds != nullptr && ds->names().empty())) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "no variables defined",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                std::string("r(111)"))));
        return true;
      }
    }
    blocks.push_back(ojson(handlers::runSaveData(filepath, replace, session)));
    return true;
  }


  // BUG-343: `file open|write|read|close`. Registered under the first word with
  // sub-verb routing inside the handler — the same shape `export`/`import` use
  // (registry.cpp), because the command table has to recognize the LINE before
  // anything can look at what follows it. An empty result means "did its work,
  // says nothing", exactly like postfile below.
  if (cmd == "file") {
    ojson r = handlers::runFileCmd(parsed.raw, session);
    if (!(r.is_array() && r.empty()) && !r.is_null()) blocks.push_back(r);
    return true;
  }


  if (cmd == "postfile") {
    ojson r = handlers::runPostfile(parsed.raw, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return true;
  }


  if (cmd == "post") {
    ojson r = handlers::runPost(parsed.raw, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return true;
  }


  if (cmd == "postclose") {
    std::string handle = parsed.varlist.empty() ? std::string() : parsed.varlist[0];
    ojson r = handlers::runPostclose(handle, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return true;
  }


  if (cmd == "export" || cmd == "import") {
    // Two-word I/O verbs: `export delimited|excel ... using FILE` and
    // `import delimited|excel [using] FILE`. The sub-verb decides the format;
    // anything else is an honest reject naming what this build supports.
    static const std::regex reSub(R"(^\s*(?:export|import)\s+(\w+)\b\s*(.*)$)",
                                  std::regex::icase);
    std::smatch m;
    if (!std::regex_search(parsed.raw, m, reSub)) {
      blocks.push_back(ojson(eval::engineError(
          cmdTyped + ": expected `" + cmdTyped + " delimited ...` or `" + cmdTyped +
          " excel ...`")));
      return true;
    }
    std::string sub = lower(m.str(1));
    std::string rest = trimWs(m.str(2));
    // import / export dispatch the sub-verb by an exact table of
    // spellings: delim, delimi, delimit, delimite, delimited and exc, exce,
    // excel (w2ar dx_exd_varlist `export delim id s g using ...` rc 0,
    // log:468; dx_imd_038_delim rc 0, log:2708).
    {
      static const std::map<std::string, std::string> kSubSpellings = {
          {"delim", "delimited"}, {"delimi", "delimited"},   {"delimit", "delimited"},
          {"delimite", "delimited"}, {"delimited", "delimited"}, {"exc", "excel"},
          {"exce", "excel"},       {"excel", "excel"}};
      auto it = kSubSpellings.find(sub);
      if (it != kSubSpellings.end()) sub = it->second;
    }
    // Split `[varlist] using "FILE"` (export) / `[using] "FILE"` (import). The
    // option comma-tail is already stripped into parsed.opts. Words are split
    // quote-aware, and an `if`/`in` qualifier (the tokenizer already parsed it
    // into parsed.ifExpr / parsed.inRange) ends the file name.
    std::string spec = rest;
    {
      size_t comma = std::string::npos;
      bool inQ = false;
      for (size_t i = 0; i < spec.size(); ++i) {
        if (spec[i] == '"') inQ = !inQ;
        else if (spec[i] == ',' && !inQ) { comma = i; break; }
      }
      if (comma != std::string::npos) spec = spec.substr(0, comma);
      spec = trimWs(spec);
    }
    std::vector<std::string> words;
    {
      std::string cur;
      size_t i = 0;
      while (i < spec.size()) {
        const char c = spec[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
          if (!cur.empty()) { words.push_back(cur); cur.clear(); }
          ++i;
          continue;
        }
        if (c == '`' && i + 1 < spec.size() && spec[i + 1] == '"') {
          const size_t j = spec.find("\"'", i + 2);
          const size_t end = j == std::string::npos ? spec.size() : j + 2;
          cur += spec.substr(i, end - i);
          i = end;
          continue;
        }
        if (c == '"') {
          const size_t j = spec.find('"', i + 1);
          const size_t end = j == std::string::npos ? spec.size() : j + 1;
          cur += spec.substr(i, end - i);
          i = end;
          continue;
        }
        cur += c;
        ++i;
      }
      if (!cur.empty()) words.push_back(cur);
    }
    {
      // Drop the qualifier tail: `... using f if x < 3 in 1/3`.
      for (size_t i = 0; i < words.size(); ++i) {
        const std::string lw = lower(words[i]);
        if (lw == "if" || lw == "in") { words.resize(i); break; }
      }
    }
    std::string pathArg;
    std::vector<std::string> varlist;
    bool sawUsing = false;
    for (const auto& w : words) {
      if (lower(w) == "using") { sawUsing = true; continue; }
      if (sawUsing) pathArg = w;
      else varlist.push_back(w);
    }
    if (!sawUsing) {
      // No `using`: export delimited / export excel re-parse the line
      // as `using <0>`, so the whole spec names the file (dx_exd_delim_novar
      // `export delimited `e1', delim(",") novar replace` rc 0, log:543;
      // dx_exx_097 `export excel "wb5", ...` log:1353). import takes the
      // same shape (`import delimited "f.csv"`).
      if (varlist.size() == 1) pathArg = varlist.front();
      else if (cmd == "import" && !varlist.empty()) pathArg = varlist.back();
      varlist.clear();
    }
    if (sub == "sas" || sub == "spss") {
      // import sas / import spss (User ruling (b), scope-ge2/data-io-family/
      // DECISION-import-fred-sas-spss.md): no reader. A missing file is
      // Stata's own r(601) with the data untouched (w2ar dx_imsas_missing,
      // dx_imspss_missing: 601, k unchanged, log:4702/4714); an existing one
      // is refused by name.
      if (cmd == "import" && !pathArg.empty()) {
        std::string p = resolveUserPath(session, pathArg);
        std::string shown = typedUserPath(pathArg);  // named as typed (BUG-410)
        const size_t slash = p.find_last_of('/');
        const size_t dot = p.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
          p += (sub == "sas" ? ".sas7bdat" : ".sav");
          shown += (sub == "sas" ? ".sas7bdat" : ".sav");
        }
        if (!cecon::native_exists(p)) {
          blocks.push_back(ojson(eval::errorBlock("engine_error", "file " + shown + " not found",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  "r(601)")));
          return true;
        }
      }
    }
    if (sub != "delimited" && sub != "excel") {
      blocks.push_back(ojson(eval::engineError(
          cmdTyped + " " + sub + ": not supported — this build implements `" + cmdTyped +
          " delimited` and `" + cmdTyped + " excel`.")));
      return true;
    }
    if (pathArg.empty()) {
      blocks.push_back(ojson(eval::engineError(
          cmdTyped + " " + sub + ": missing the file. Usage: " + cmdTyped + " " + sub +
          (cmd == "export" ? " [varlist] using \"file\"" : " using \"file\""))));
      return true;
    }
    // BUG-336 (batch 2): known sets measured from the four handler bodies
    // (hasFlagOpt/parseDelimiterOpt reads). `clear` is KNOWN on import because
    // PIET's import always replaces the in-memory dataset -- the flag's
    // semantic is satisfied, not dropped.
    {
      // AER scope-2 W2: the four branch sets moved to file scope (one copy
      // each) so kVocabImport/kVocabExport can be their per-command union.
      // The branch net still decides; nothing about the sets changed.
      std::unordered_set<std::string> known, unimpl;
      if (cmd == "export" && sub == "delimited") {
        known = kExportDelimKnown;
        unimpl = kExportDelimUnimpl;
      } else if (cmd == "export") {
        known = kExportExcelKnown;
        unimpl = kExportExcelUnimpl;
      } else if (sub == "delimited") {
        known = kImportDelimKnown;
        unimpl = kImportDelimUnimpl;
      } else {
        known = kImportExcelKnown;
        unimpl = kImportExcelUnimpl;
      }
      if (auto err = eval::rejectUnknownOptions(parsed.opts, known, unimpl)) {
        blocks.push_back(ojson(*err));
        return true;
      }
    }
    if (cmd == "export") {
      blocks.push_back(ojson(sub == "delimited"
                                 ? handlers::runExportDelimited(varlist, pathArg, parsed.opts,
                                                                parsed.ifExpr, parsed.inRange, session)
                                 : handlers::runExportExcel(varlist, pathArg, parsed.opts,
                                                            parsed.ifExpr, parsed.inRange, session)));
    } else {
      // [D] import delimited / import excel: `clear` is what lets the import
      // replace data changed since they were last saved; without it, r(4)
      // "no; data in memory would be lost", the data untouched
      // (w2ar_data_io_family.log dx_imd_noclear, dx_imx_noclear `4|k=7|N=4|`).
      // `import excel ..., describe` loads nothing and is not refused. A file
      // that is not there is answered first: `import delimited using
      // nofile.csv` on changed data is r(601) in StataBE 19.5, not r(4).
      bool importClears = false, describeOnly = false;
      for (const auto& [k, v] : parsed.opts) {
        if (k == "clear" && (!v.isBool || v.boolValue)) importClears = true;
        if (k == "describe" && (!v.isBool || v.boolValue)) describeOnly = true;
      }
      if (!importClears && !describeOnly && session.hasData() && session.datasetChanged &&
          cecon::native_exists(handlers::importSourcePath(session, pathArg, sub != "delimited"))) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "no; data in memory would be lost",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                std::string("r(4)"))));
        return true;
      }
      blocks.push_back(ojson(sub == "delimited"
                                 ? handlers::runImportDelimited(varlist, pathArg, parsed.opts, session)
                                 : handlers::runImportExcel(pathArg, parsed.opts, session)));
    }
    return true;
  }


  // ==================== AER-SCOPE1 bundle G (IO-TEXT) arms ==================
  if (cmd == "insheet") {
    pushEnvResult(blocks, handlers::runInsheet(parsed, session));
    return true;
  }

  if (cmd == "outsheet") {
    pushEnvResult(blocks, handlers::runOutsheet(parsed, session));
    return true;
  }

  if (cmd == "infile") {
    pushEnvResult(blocks, handlers::runInfile(parsed, session));
    return true;
  }

  if (cmd == "infix") {
    pushEnvResult(blocks, handlers::runInfix(parsed, session));
    return true;
  }

  if (cmd == "outfile") {
    pushEnvResult(blocks, handlers::runOutfile(parsed, session));
    return true;
  }

  if (cmd == "saveold") {
    pushEnvResult(blocks, handlers::runSaveold(parsed, session));
    return true;
  }

  if (cmd == "putexcel") {
    // RAW line, not `parsed`: putexcel's grammar is `A1 = <value>`, and the
    // tokenizer splits on `=` and on the commas inside `matrix(M), names`, so
    // a parsed varlist cannot reconstruct the assignment. Same reason the
    // `matrix` arm re-parses its own raw line.
    pushEnvResult(blocks, handlers::runPutexcel(rawWithCanonicalCmd(parsed.raw, cmdTyped, cmd), session));
    return true;
  }

  if (cmd == "filefilter") {
    pushEnvResult(blocks, handlers::runFilefilter(parsed, session));
    return true;
  }

  if (cmd == "copy") {
    pushEnvResult(blocks, handlers::runCopy(parsed, session));
    return true;
  }
  return false;
}

}
