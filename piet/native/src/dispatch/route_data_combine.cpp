// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "data_support.h"
#include "prefix_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
#include "piet/handlers/data_combine.h"
#include "piet/handlers/observations.h"
namespace piet::dispatch_detail {
bool route_data_combine(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {


  if (cmd == "reshape") {
    // reshape.ado has no qualifiers: `if`, `in` and the weight's `[` are words of
    // its stub list (registry.cpp kQualifiersRefusedByArm). A weight is r(198)
    // "[ invalid name"; under `wide`, whose stubs are variables, the first of
    // `if` / `in` is r(111) "variable if not found" once the stubs before it are
    // found. A `long` line keeps PIET's refusal: its stubs are prefixes, and
    // Stata's answer there is unmeasured.
    if (parsed.weightType.has_value()) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "[ invalid name", std::nullopt,
                                              std::nullopt, std::nullopt, "r(198)")));
      return true;
    }
    if (parsed.ifExpr.has_value() || parsed.inRange.has_value()) {
      const std::string dir = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
      if (dir == "wide" && session.hasData()) {
        for (size_t k = 1; k < parsed.varlist.size(); ++k) {
          if (!targetVarExists(session, parsed.varlist[k]).value_or(false)) {
            blocks.push_back(ojson(eval::errorBlock(
                "engine_error", "variable " + parsed.varlist[k] + " not found", std::nullopt,
                std::nullopt, std::nullopt, "r(111)")));
            return true;
          }
        }
        const size_t ifAt = parsed.ifExpr.has_value() ? parsed.raw.find(" if ") : std::string::npos;
        const size_t inAt = parsed.inRange.has_value() ? parsed.raw.find(" in ") : std::string::npos;
        const std::string word = ifAt < inAt ? "if" : "in";
        blocks.push_back(ojson(eval::errorBlock("engine_error", "variable " + word + " not found",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(111)")));
        return true;
      }
      blocks.push_back(ojson(eval::engineError(
          "`reshape` takes no `if` or `in` qualifier; subset the data first (keep if ...).",
          "unsupported_option")));
      return true;
    }
    if (preflight::gate("reshape", session, "", 0, blocks) == preflight::Verdict::Cancel) return true;
    ojson reshaped = ojson(handlers::runReshape(parsed.varlist, parsed.opts, session));
    // e(sample) T2 (SPEC §4.3): reshape rebuilds the observations, so the
    // estimation-sample mask cannot follow them -- Stata's marker is gone after
    // it (w2_esample.log:1174 E9 `reshape wide` then `count if e(sample)` 0).
    // The engine carried the hidden column like any other (a wide reshape took
    // one value per i: 3 rows counted), so it is dropped from the new frame:
    // the model's e(sample) then reads 0 (column absent), never rows it was
    // not fitted on. No block: the column was never in the user's varlist.
    if (reshaped.is_object() && reshaped.value("type", std::string()) == "data_mutation" &&
        session.esampleColumnInstalls > 0) {
      try {
        Datastore* rds = session.datastore();
        if (rds != nullptr && rds->has(kEsampleColumn)) {
          const std::string out = mintScratchPath(session.workdir(), "reshape_esample_drop");
          runCobraJson({"eval", "--data", *session.currentHbin(), "--drop", kEsampleColumn,
                        "--out", out});
          session.load(out);
        }
      } catch (const std::exception& e) {
        blocks.push_back(reshaped);
        blocks.push_back(ojson(eval::engineError(
            std::string("reshape: the estimation-sample marker could not be removed from the "
                        "reshaped data (") + e.what() + ")")));
        return true;
      }
    }
    blocks.push_back(reshaped);
    return true;
  }


  if (cmd == "append") {
    // BS-097: this arm never reads parsed.opts at all today (only parsed.raw
    // via regex + parsed.varlist) — real Stata `append` options (nolabel,
    // generate()) are still rejected rather than silently dropped.
    // W:P5: `force` is accepted. In Stata it permits appending when a
    // variable's type differs between master and using (string vs numeric),
    // filling the using side's cells with missing of the master's type.
    // BUG-550: this was a no-op over the engine's promote-both-to-f64 default,
    // which is NOT what force does (string codes became numbers, and without
    // force the clash was rc 0 instead of r(106)). runAppend now passes
    // `--type-clash error|force` (w2_data_io.log app15/app16/app17).
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kAppendKnownOpts = kVocabAppend.known;
    // append.ado runs `capture quietly describe using FILE` (r-class) on every
    // using file before `_append` parses the rest of the options, so a call
    // that gets past `using` replaces r() even when it then fails (aer_w2
    // w2b_dataio append_ge(src): `ge(src)` r(198), then r(sum) reads `.`).
    {
      static const std::regex reAppendUsing(R"(\busing\s+\S)", std::regex::icase);
      if (std::regex_search(parsed.raw, reAppendUsing)) {
        eval::replaceRClass(session.eval);
        eval::declareRPosted(session.eval, "append", {});
      }
    }
    // append.ado's syntax line declares `GENerate(name)`: gen ... generate all
    // name it (w2aw_data_combine_family.log dc_a_gen `gen(src)` rc 0). Only that
    // declared minimum is honoured; the rest of the net stays exact.
    std::vector<std::pair<std::string, eval::OptValue>> appendOpts = parsed.opts;
    for (auto& kv : appendOpts) {
      if (kv.first.size() >= 3 && kv.first.size() < 8 &&
          std::string("generate").compare(0, kv.first.size(), kv.first) == 0)
        kv.first = "generate";
    }
    if (auto err = eval::rejectUnknownOptions(appendOpts, kAppendKnownOpts)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // B5-D10: `generate(NAME)` -- see kVocabAppend's registration comment.
    std::optional<std::string> appendGenVar, appendKeep;
    bool appendForce = false, appendNolabel = false, appendNonotes = false;
    for (const auto& [k, v] : appendOpts) {
      if (k == "generate" && !v.isBool) appendGenVar = v.strValue;
      if (k == "keep") appendKeep = v.isBool ? std::string() : v.strValue;  // BUG-549
      if (k == "force") appendForce = true;  // BUG-550
      if (k == "nolabel") appendNolabel = true;  // BUG-551
      if (k == "nonotes") appendNonotes = true;
    }
    static const std::regex reUsing(R"(\busing\s+(.+?)(?:\s*,|$))", std::regex::icase);
    std::smatch m;
    std::string filepath;
    if (std::regex_search(parsed.raw, m, reUsing)) {
      filepath = trimWs(m.str(1));
    } else if (parsed.varlist.size() >= 2 && lower(parsed.varlist[0]) == "using") {
      filepath = parsed.varlist[1];
    } else {
      blocks.push_back(ojson(eval::engineError(
          "append: `using` filename required — syntax: append using <file>")));
      return true;
    }
    // Several files (append: `using filenames`), quote-aware.
    std::vector<std::string> appendFiles;
    {
      size_t i = 0;
      while (i < filepath.size()) {
        while (i < filepath.size() && std::isspace(static_cast<unsigned char>(filepath[i]))) ++i;
        if (i >= filepath.size()) break;
        if (filepath[i] == '"') {
          const size_t close = filepath.find('"', i + 1);
          const size_t end = close == std::string::npos ? filepath.size() : close;
          appendFiles.push_back(filepath.substr(i + 1, end - i - 1));
          i = end + 1;
        } else if (filepath.compare(i, 2, "`\"") == 0) {
          const size_t close = filepath.find("\"'", i + 2);
          const size_t end = close == std::string::npos ? filepath.size() : close;
          appendFiles.push_back(filepath.substr(i + 2, end - i - 2));
          i = end + 2;
        } else {
          size_t j = i;
          while (j < filepath.size() && !std::isspace(static_cast<unsigned char>(filepath[j]))) ++j;
          appendFiles.push_back(filepath.substr(i, j - i));
          i = j;
        }
      }
    }
    for (const auto& f : appendFiles)
      if (preflight::gate("append", session, f, 0, blocks) == preflight::Verdict::Cancel) return true;
    std::vector<std::string> typeNotes;
    ojson appended = handlers::runAppendFiles(appendFiles, session, appendGenVar, appendKeep,
                                              appendForce, appendNolabel, appendNonotes,
                                              &typeNotes);
    // BUG-1092: append's widening lines, command output printed as it appends.
    for (const std::string& text : typeNotes) {
      ojson b;
      b["type"] = "display";
      b["text"] = text;
      blocks.push_back(std::move(b));
    }
    const bool appendFailed =
        appended.is_object() && appended.value("type", std::string()) == "error";
    blocks.push_back(std::move(appended));
    if (!appendFailed && !appendFiles.empty()) {
      // append.ado `describe using`s each file in turn, so r() afterwards is the last file's
      // (r(N), r(k), r(width), the limits and r(datalabel); vs-stata bank).
      const std::string& lastFile = appendFiles.back();
      const std::string savedLeaf = session.leafCmdExpanded;
      std::string q;
      for (char c : lastFile) q += c == '"' ? '\'' : c;
      (void)dispatch("quietly describe using \"" + q + "\"", session);
      session.leafCmdExpanded = savedLeaf;
    }
    return true;
  }


  if (cmd == "merge") {
    // fmerge (ftools' join.ado) folds onto this arm. What it refuses outright, by return code 9
    // (vs-stata bank): a first word that is not a merge type, the 1:m and m:m types, and the
    // update / replace options; keep() takes only result categories (r(117)); `sorted` does
    // nothing but warn.
    bool isFmerge = false;
    std::optional<ojson> fmergeWarning;
    {
      const std::string& L = session.leafCmdExpanded;
      const size_t b0 = L.find_first_not_of(" \t");
      if (b0 != std::string::npos) {
        const size_t e0 = L.find_first_of(" \t,", b0);
        if (lower(L.substr(b0, e0 == std::string::npos ? std::string::npos : e0 - b0)) == "fmerge")
          isFmerge = true;
        if (isFmerge && e0 != std::string::npos) {
          const std::string args = trimWs(L.substr(e0));
          const size_t te = args.find_first_of(" \t");
          const std::string tok = lower(args.substr(0, te));
          auto refuse9 = [&](const std::string& msg) {
            blocks.push_back(ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                                    std::nullopt, std::string("r(9)"))));
            return true;
          };
          if (!tok.empty()) {
            if (tok == "1:m" || tok == "1:n")
              return refuse9("1:m merges not supported, use join with the into() option");
            if (tok == "m:m" || tok == "n:n" || tok == "m:n" || tok == "n:m")
              return refuse9("have you read the disclaimer about m:m merges? (this merge type is "
                             "dangerous and not supported)");
            if (tok != "1:1" && tok != "m:1" && tok != "n:1")
              return refuse9("invalid merge type: " + args.substr(0, te));
          }
        }
      }
    }
    static const std::regex reMergeHead(R"(^\s*merge\s+([^\s,]+)\s*(.*)$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(parsed.raw, m, reMergeHead)) {
      blocks.push_back(ojson(eval::engineError(
          "merge: syntax error. Usage: merge 1:1|m:1|1:m|m:m KEYLIST using FILE [, opts]")));
      return true;
    }
    const std::string mtypeRaw = m.str(1);
    std::string rest = m.str(2);
    if (mtypeRaw.find(':') == std::string::npos) {
      // The old merge syntax: `merge [varlist] using file [, update replace
      // keep(varlist) _merge(name) sort unique uniqmaster uniqusing nolabel
      // nosummary]`. keep() names USING variables (merge's keepusing()); both
      // datasets must already be sorted by the varlist unless `sort` (r(5));
      // duplicates pair as m:m unless a uniq option asks otherwise.
      const long long comma = eval::findUnquotedComma(parsed.raw);
      std::string head = comma >= 0 ? parsed.raw.substr(0, static_cast<size_t>(comma)) : parsed.raw;
      static const std::regex reOldHead(R"(^\s*merge\s+(.*?)\busing\b\s*(.*?)\s*$)",
                                        std::regex::icase);
      std::smatch om;
      if (!std::regex_match(head, om, reOldHead)) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "using required", std::nullopt,
                                                std::nullopt, std::nullopt, std::string("r(100)"))));
        return true;
      }
      std::vector<std::string> oldKeys;
      {
        std::istringstream iss(om.str(1));
        for (std::string t; iss >> t;) oldKeys.push_back(t);
      }
      std::string oldFile = trimWs(om.str(2));
      if (oldFile.size() >= 2 && ((oldFile.front() == '"' && oldFile.back() == '"') ||
                                  (oldFile.front() == '\'' && oldFile.back() == '\''))) {
        oldFile = oldFile.substr(1, oldFile.size() - 2);
      }
      if (oldKeys.empty() || oldFile.empty()) {
        blocks.push_back(ojson(eval::engineError(
            "merge: the old syntax without match variables is not supported by this build")));
        return true;
      }
      std::vector<std::pair<std::string, eval::OptValue>> oldOpts =
          comma >= 0 ? eval::parseOpts(parsed.raw.substr(static_cast<size_t>(comma) + 1))
                     : std::vector<std::pair<std::string, eval::OptValue>>{};
      std::vector<std::pair<std::string, eval::OptValue>> newOpts;
      std::string oldType = "m:m";
      bool oldSort = false;
      static const std::unordered_set<std::string> kOldKnown = {
          "update", "replace", "keep",       "_merge",    "sort",    "unique",
          "uniqmaster", "uniqusing", "nolabel", "nosummary", "nokeep", "nonotes"};
      eval::canonicalizeOptionKeys(oldOpts, kOldKnown);
      for (const auto& [k, v] : oldOpts) {
        if (k == "keep" && !v.isBool) newOpts.emplace_back("keepusing", v);
        else if (k == "_merge" && !v.isBool) newOpts.emplace_back("generate", v);
        else if (k == "update" || k == "replace") newOpts.emplace_back(k, v);
        else if (k == "sort") oldSort = true;
        else if (k == "unique") oldType = "1:1";
        else if (k == "uniqmaster") oldType = oldType == "m:1" ? "1:1" : "1:m";
        else if (k == "uniqusing") oldType = oldType == "1:m" ? "1:1" : "m:1";
        else if (k == "nolabel" || k == "nosummary" || k == "nokeep" || k == "nonotes") continue;
        else {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", "option " + k + " not allowed",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  std::string("r(198)"))));
          return true;
        }
      }
      if (oldSort && oldType == "m:m") oldType = "1:1";  // merge_10: sort implies unique
      if (preflight::gate("merge", session, oldFile, 0, blocks) == preflight::Verdict::Cancel)
        return true;
      blocks.push_back(ojson(handlers::runMerge(oldType, oldKeys, oldFile, newOpts, session,
                                                /*sequential=*/false, /*oldSyntax=*/true,
                                                oldSort)));
      return true;
    }
    // merge type: n spells m; anything else is r(198).
    std::string mergeType;
    {
      static const std::map<std::string, std::string> kMtype = {
          {"1:1", "1:1"}, {"1:m", "1:m"}, {"m:1", "m:1"}, {"m:m", "m:m"}, {"1:n", "1:m"},
          {"n:1", "m:1"}, {"n:n", "m:m"}, {"n:m", "m:m"}, {"m:n", "m:m"}};
      auto it = kMtype.find(mtypeRaw);
      if (it == kMtype.end()) {
        blocks.push_back(ojson(eval::errorBlock(
            "syntax_error",
            "merge " + mtypeRaw + ":  invalid merge type\n    merge types are 1:1, 1:m, m:1, or m:m",
            std::nullopt, std::nullopt, std::nullopt, std::string("r(198)"))));
        return true;
      }
      mergeType = it->second;
    }
    // `merge 1:1 _n` is the sequential merge (merge).
    bool mergeSequential = false;
    {
      static const std::regex reSeq(R"(^_n(?=[\s,]|$)\s*)");
      std::smatch sm;
      if (std::regex_search(rest, sm, reSeq)) {
        if (mergeType != "1:1") {
          blocks.push_back(ojson(eval::errorBlock(
              "syntax_error",
              mtypeRaw + " _n invalid\n    the syntax for a sequential merge is merge 1:1 _n ...",
              std::nullopt, std::nullopt, std::nullopt, std::string("r(198)"))));
          return true;
        }
        mergeSequential = true;
        rest = rest.substr(static_cast<size_t>(sm.length(0)));
      }
    }
    // strip trailing options (`, opt...`) — split at the first comma
    // followed by a word char.
    static const std::regex reOptSplit(R"(,\s*(?=\w))");
    std::string restNoOpts;
    {
      std::smatch om;
      if (std::regex_search(rest, om, reOptSplit)) {
        restNoOpts = trimWs(rest.substr(0, static_cast<size_t>(om.position(0))));
      } else {
        restNoOpts = trimWs(rest);
      }
    }
    static const std::regex reUsingSplit(R"(\busing\b)", std::regex::icase);
    std::smatch usingM;
    if (!std::regex_search(restNoOpts, usingM, reUsingSplit)) {
      blocks.push_back(ojson(eval::engineError(
          "merge: missing `using FILE` clause. Usage: merge 1:1 id using \"file.hbin\"")));
      return true;
    }
    std::string keyPart = trimWs(restNoOpts.substr(0, static_cast<size_t>(usingM.position(0))));
    std::string filePart =
        trimWs(restNoOpts.substr(static_cast<size_t>(usingM.position(0) + usingM.length(0))));
    if (filePart.size() >= 2 && ((filePart.front() == '"' && filePart.back() == '"') ||
                                 (filePart.front() == '\'' && filePart.back() == '\''))) {
      filePart = filePart.substr(1, filePart.size() - 2);
    }
    std::vector<std::string> keylist;
    {
      std::istringstream iss(keyPart);
      std::string tok;
      while (iss >> tok) keylist.push_back(tok);
    }
    if (mergeSequential && !keylist.empty()) {
      blocks.push_back(ojson(eval::errorBlock(
          "syntax_error", "varlist cannot be specified after 1:1 _n", std::nullopt, std::nullopt,
          std::nullopt, std::string("r(198)"))));
      return true;
    }
    if (keylist.empty() && !mergeSequential) {
      blocks.push_back(ojson(eval::errorBlock(
          "syntax_error", "varlist must be specified after merge " + mtypeRaw, std::nullopt,
          std::nullopt, std::nullopt, std::string("r(198)"))));
      return true;
    }
    if (filePart.empty()) {
      blocks.push_back(ojson(eval::engineError("merge: empty filename after `using`")));
      return true;
    }
    if (preflight::gate("merge", session, filePart, 0, blocks) == preflight::Verdict::Cancel) return true;
    // e(sample) T2 / FU-496: a `1:m` merge copies a master row once per
    // matching using row. StataBE keeps the marker on ONE copy, the first in
    // merge's output order, and reads 0 on the others, so the count stays e(N)
    // (w2_esample_copies.log:596 M1 `0|9|0|9|22|matched=9|not_first_of_run=0|`;
    // a review measured 18 for e(N) 9 when the cell rode on every copy).
    // Using-only rows carry no cell and read 0 (M2 `using_only=0`, :664).
    // handlers::esampleKeepFirstCopy applies the rule; a key it cannot read
    // withdraws the marker by name.
    std::optional<size_t> esMasterRows;
    if (mergeType == "1:m") {
      if (auto cells = handlers::esampleHeadMask(session)) esMasterRows = cells->size();
    }
    // Stata's option syntax binds `keep (1 3)` like `keep(1 3)` (w2aw_data_combine_family.log
    // dc_mk_spaced, rc 0): re-read merge's option tail with a blank before `(` closed up.
    std::vector<std::pair<std::string, eval::OptValue>> mergeOpts = parsed.opts;
    {
      const long long comma = eval::findUnquotedComma(parsed.raw);
      if (comma >= 0) {
        static const std::regex reSpacedParen(R"(([A-Za-z_][A-Za-z0-9_]*)\s+\()");
        mergeOpts = eval::parseOpts(std::regex_replace(
            parsed.raw.substr(static_cast<size_t>(comma) + 1), reSpacedParen, "$1("));
      }
    }
    // fmerge (ftools) folds onto this arm (gtools_aliases.cpp) but is join.ado,
    // whose result table is `tab <_merge>, nolabel nofreq` over the merged data
    // unless noreport: r(N) = observations left, r(r) = how many of the codes
    // 1/2/3 remain (vs-stata bank: fmerge 1:1 make using u -> r(N) 74 r(r) 2;
    // keep(match) -> 20 1). Under nogenerate join still tabulates, on a tempvar.
    bool fmergeReport = false;
    std::string fmergeMarker;
    bool fmergeScratchMarker = false;
    {
      std::istringstream w(session.leafCmdExpanded);
      std::string head;
      w >> head;
      if (lower(head.substr(0, head.find(','))) == "fmerge") {
        bool noreport = false, nogen = false;
        std::string genName = "_merge";
        for (const auto& [k, v] : mergeOpts) {
          if (k == "noreport") noreport = true;
          if (k == "nogenerate") nogen = true;
          if (k == "generate" && !v.isBool) genName = trimWs(v.strValue);
        }
        fmergeReport = !noreport;
        if (fmergeReport) {
          if (nogen) {
            fmergeMarker = "__fmerge_marker";
            fmergeScratchMarker = true;
            std::vector<std::pair<std::string, eval::OptValue>> kept;
            for (const auto& kv : mergeOpts)
              if (kv.first != "nogenerate") kept.push_back(kv);
            kept.emplace_back("generate", eval::OptValue::ofString(fmergeMarker));
            mergeOpts = std::move(kept);
          } else {
            fmergeMarker = genName;
          }
        }
      }
    }
    if (isFmerge) {
      std::vector<std::pair<std::string, eval::OptValue>> kept;
      for (const auto& kv : mergeOpts) {
        const std::string& k = kv.first;
        if (k == "update" || k == "replace")
          return blocks.push_back(ojson(eval::errorBlock(
                     "engine_error", "-" + k + "- option not allowed", std::nullopt, std::nullopt,
                     std::nullopt, std::string("r(9)")))),
                 true;
        if (k == "keep" && !kv.second.isBool) {
          std::istringstream cats(kv.second.strValue);
          for (std::string c; cats >> c;) {
            const std::string lc = lower(c);
            static const char* const kWords[] = {"master", "using", "match", "matched",
                                                 "match_update", "match_updates",
                                                 "match_conflict", "match_conflicts"};
            bool ok = lc.size() == 1 && lc[0] >= '1' && lc[0] <= '5';
            for (const char* w : kWords)
              if (lc.size() >= 2 && std::string(w).compare(0, lc.size(), lc) == 0) ok = true;
            if (!ok)
              return blocks.push_back(ojson(eval::errorBlock(
                         "engine_error", "invalid category: <" + c + ">", std::nullopt,
                         std::nullopt, std::nullopt, std::string("r(117)")))),
                     true;
          }
        }
        if (k == "sorted") {
          ojson w;
          w["type"] = "display";
          w["text"] = "warning: -sorted- option does nothing";
          fmergeWarning = std::move(w);
          continue;
        }
        kept.push_back(kv);
      }
      mergeOpts = std::move(kept);
    }
    if (fmergeWarning.has_value()) blocks.push_back(*fmergeWarning);
    std::vector<std::string> fmergeBrought;
    ojson mergeBlock = ojson(handlers::runMerge(mergeType, keylist, filePart, mergeOpts, session,
                                                mergeSequential, false, false, isFmerge,
                                                isFmerge ? &fmergeBrought : nullptr));
    const bool mergeFailed =
        mergeBlock.is_object() && mergeBlock.value("type", std::string()) == "error";
    if (fmergeReport && !mergeFailed) {
      Datastore* fds = session.datastore();
      if (fds != nullptr && fds->has(fmergeMarker)) {
        const int64_t nRows = fds->nRows();
        const double* mk = fds->col(fmergeMarker).f64();
        std::set<double> codes;
        for (int64_t i = 0; mk != nullptr && i < nRows; ++i)
          if (!std::isnan(mk[i])) codes.insert(mk[i]);
        if (nRows > 0) {
          eval::replaceRClass(session.eval);
          session.eval.rValues.set("N", static_cast<double>(nRows));
          session.eval.rValues.set("r", static_cast<double>(codes.size()));
          eval::declareRPosted(session.eval, "fmerge", {"N", "r"});
        }
        if (fmergeScratchMarker) {
          ojson dropped = handlers::runDrop({fmergeMarker}, session);
          if (dropped.is_object() && dropped.value("type", std::string()) == "error")
            mergeBlock = dropped;
          else
            mergeBlock = handlers::mutationResult(session, "merge");
        }
      }
    }
    if (isFmerge && !fmergeReport && !mergeFailed) {
      // join.ado leaves the names of the variables it brought in r(varlist) under noreport.
      std::string joined;
      for (const auto& n : fmergeBrought) joined += (joined.empty() ? "" : " ") + n;
      eval::replaceRClass(session.eval);
      session.eval.rStrings.set("varlist", joined);
      eval::declareRPosted(session.eval, "fmerge", {"varlist"});
    }
    blocks.push_back(mergeBlock);
    if (esMasterRows.has_value() && !mergeFailed) {
      size_t masterOrigin = 0;
      if (auto after = handlers::esampleHeadMask(session)) {
        for (double v : *after) masterOrigin += std::isnan(v) ? 0 : 1;
      }
      if (masterOrigin > *esMasterRows &&
          !handlers::esampleKeepFirstCopy(session, keylist, nullptr) &&
          !handlers::esampleMarkHeadLost(session, EsampleLostBy::kMerge1m)) {
        blocks.push_back(ojson(eval::engineError(
            "merge: the datasets were merged, but the estimation-sample marker could not be "
            "withdrawn from the copied observations; e(sample) is not reliable until the next "
            "estimation.")));
      }
    }
    return true;
  }


  // ------------------------------------------------------------------
  // frames (P6 wave) — `frame` is BOTH a command family (`frame put/drop`)
  // and a prefix (`frame NAME: CMD`), so the sub-token decides which. See
  // datamgmt/frames.cpp for the design; the current frame is always `default`
  // in this build and `frame create`/`change`/`pwf` are honest rejects, not
  // silent no-ops.
  if (cmd == "frame") {
    static const std::regex reFrame(R"(^\s*frame\s+(.+)$)", std::regex::icase);
    std::smatch fm;
    if (!std::regex_match(parsed.raw, fm, reFrame)) {
      blocks.push_back(ojson(eval::engineError(
          "frame: syntax error. Usage: frame put VARLIST [if], into(NAME) | frame drop NAME | "
          "frame NAME: COMMAND")));
      return true;
    }
    std::string rest = trimWs(fm.str(1));
    // `frame NAME: CMD` — the colon (outside quotes) marks the prefix form.
    // Checked BEFORE the sub-command split so a frame legitimately named
    // `drop` still reaches its own prefix arm rather than being read as the
    // drop sub-command.
    {
      static const std::regex reColon(R"(:)");
      std::smatch cm;
      if (eval::searchUnquoted(rest, reColon, cm)) {
        std::string frameName = trimWs(rest.substr(0, static_cast<size_t>(cm.position(0))));
        std::string innerCmd =
            trimWs(rest.substr(static_cast<size_t>(cm.position(0) + cm.length(0))));
        // A one-token name is the prefix form; anything else (e.g.
        // `frame put x y, into(f)` has no colon at all) never reaches here.
        if (!frameName.empty() && frameName.find_first_of(" \t") == std::string::npos) {
          if (innerCmd.empty()) {
            blocks.push_back(ojson(eval::engineError(
                "frame " + frameName + ": no command found after the colon")));
            return true;
          }
          ojson inner = handlers::runFramePrefix(frameName, innerCmd, session);
          if (inner.is_array()) {
            for (const auto& b : inner) blocks.push_back(b);
          } else {
            blocks.push_back(inner);
          }
          return true;
        }
      }
    }

    std::string sub;
    std::string subRest;
    {
      size_t sp = rest.find_first_of(" \t");
      if (sp == std::string::npos) {
        sub = lower(rest);
      } else {
        sub = lower(rest.substr(0, sp));
        subRest = trimWs(rest.substr(sp));
      }
    }

    // StataBE 19.5 (vs-stata bank 2026-11-10): `frames dir, zznotanoption` is
    // r(101) "options not allowed"; `frame drop default` (the current frame) is
    // r(119) "may not drop current frame"; `frame put price` and `frame
    // putlabel foreign` without into() are r(198) "option into() required".
    if (sub == "dir" && !parsed.opts.empty()) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "options not allowed", std::nullopt,
                                              std::nullopt, std::nullopt, "r(101)")));
      return true;
    }
    if (sub == "drop" && trimWs(subRest) == "default") {
      blocks.push_back(ojson(eval::errorBlock("engine_error", "may not drop current frame",
                                              std::nullopt, std::nullopt, std::nullopt, "r(119)")));
      return true;
    }
    if (sub == "put" || sub == "putlabel") {
      bool hasInto = false;
      for (const auto& kv : parsed.opts) hasInto = hasInto || lower(kv.first) == "into";
      if (!hasInto) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "option into() required",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(198)")));
        return true;
      }
    }
    if (sub == "create" || sub == "change" || sub == "cwf" || sub == "pwf" || sub == "rename" ||
        sub == "copy" || sub == "dir") {
      blocks.push_back(ojson(eval::unsupportedCommand(
          "frame " + sub, std::nullopt, std::nullopt, "piet_gap",
          eval::UnsupportedCommandKind::kNotImplemented)));
      return true;
    }

    if (sub == "drop") {
      if (subRest.empty()) {
        blocks.push_back(ojson(eval::engineError("frame drop: frame name required")));
        return true;
      }
      ojson r = handlers::runFrameDrop(subRest, session);
      if (!(r.is_array() && r.empty())) blocks.push_back(r);
      return true;
    }

    if (sub == "put") {
      // `frame put VARLIST [if COND], into(NAME)`
      auto [head, optsPart] = splitTopLevelComma(subRest);
      std::string varsPart = trimWs(head);
      std::optional<std::string> ifExpr;
      {
        static const std::regex reIf(R"(\s+if\s+(.+)$)", std::regex::icase);
        std::smatch im;
        if (eval::searchUnquoted(varsPart, reIf, im)) {
          ifExpr = trimWs(im.str(1));
          varsPart = trimWs(varsPart.substr(0, static_cast<size_t>(im.position(0))));
        }
      }
      std::vector<std::string> varlist;
      {
        std::istringstream iss(varsPart);
        std::string tok;
        while (iss >> tok) varlist.push_back(tok);
      }
      std::string into;
      for (const auto& [k, v] : parsed.opts) {
        if (lower(k) == "into") into = v.strValue;
      }
      // BUG-233: the one copy of this vocabulary now lives in the file-scope
      // registration block (see top of file); this binding keeps every existing
      // use — including the reject net below — reading the same set.
      const std::unordered_set<std::string>& kFramePutKnown = kVocabFramePut.known;
      if (auto err = eval::rejectUnknownOptions(parsed.opts, kFramePutKnown)) {
        blocks.push_back(ojson(*err));
        return true;
      }
      (void)optsPart;
      ojson r = handlers::runFramePut(varlist, ifExpr, into, session);
      if (!(r.is_array() && r.empty())) blocks.push_back(r);
      return true;
    }

    // A word that is no subcommand of frame at all is r(198) "subcommand X is
    // unrecognized" (vs-stata bank `frame zzz`, `frame , zzabbrprobe`, `frames
    // zzz`); Stata's own subcommands this build lacks keep the refusal below.
    {
      static const std::set<std::string> kFrameSubs = {
          "create", "change", "pwf", "put", "putlabel", "copy", "rename", "drop", "dir",
          "reset", "post", "describe", "use", "save", "append", "list", "prefix"};
      if (kFrameSubs.count(sub) == 0) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                                "subcommand " + sub + " is unrecognized",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(198)")));
        return true;
      }
    }
    blocks.push_back(ojson(eval::engineError(
        "frame " + sub +
        ": unrecognized subcommand. This build supports `frame put`, `frame drop` and the "
        "`frame NAME: COMMAND` prefix.")));
    return true;
  }


  // `frlink m:1|1:1 KEY [KEY2 ...], frame(F) generate(G)`
  if (cmd == "frlink") {
    static const std::regex reFrlink(R"(^\s*frlink\s+(1:1|m:1)\s+(.+)$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(parsed.raw, m, reFrlink)) {
      // A first word that is no frlink subcommand is r(197) "WORD invalid frlink
      // subcommand" (vs-stata bank f7492847b783d44c `frlink , zzabbrprobe`).
      static const std::regex reComma(R"(^\s*frlink\s*,)", std::regex::icase);
      if (std::regex_search(parsed.raw, reComma)) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", ", invalid frlink subcommand",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(197)")));
        return true;
      }
      blocks.push_back(ojson(eval::engineError(
          "frlink: syntax error. Usage: frlink m:1|1:1 KEY [KEY2 ...], frame(FRAMENAME) "
          "generate(NEWVAR)")));
      return true;
    }
    std::string linkType = lower(trimWs(m.str(1)));
    auto [keyHead, keyOpts] = splitTopLevelComma(trimWs(m.str(2)));
    (void)keyOpts;
    std::vector<std::string> keylist;
    {
      std::istringstream iss(trimWs(keyHead));
      std::string tok;
      while (iss >> tok) keylist.push_back(tok);
    }
    std::string frameName, genName;
    for (const auto& [k, v] : parsed.opts) {
      const std::string kl = lower(k);
      if (kl == "frame") frameName = v.strValue;
      if (kl == "generate" || kl == "gen") genName = v.strValue;
    }
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kFrlinkKnown = kVocabFrlink.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kFrlinkKnown)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // [D] frlink: frame() is required -- r(198) "option frame() required"
    // (vs-stata bank 6a8a0dd21eb41e8e `frlink 1:1 make`) -- and its first word
    // names the frame (`frame(f2 make)` adds the key names there); without
    // generate() the link variable takes the frame's name ("the default is
    // generate(framename)"; `frlink 1:1 make, frame(f2)` then `frget ...,
    // from(f2)` run in StataBE 19.5).
    {
      std::istringstream fw(frameName);
      std::string first;
      fw >> first;
      if (first.empty()) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "option frame() required",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(198)")));
        return true;
      }
      if (genName.empty() && frameName == first) genName = first;
    }
    blocks.push_back(ojson(handlers::runFrlink(linkType, keylist, frameName, genName, session)));
    return true;
  }


  // `frget NEW=VAR [NEW2=VAR2 ...], from(LINKVAR)`
  if (cmd == "frget") {
    static const std::regex reFrget(R"(^\s*frget\s+(.+)$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(parsed.raw, m, reFrget)) {
      blocks.push_back(ojson(eval::engineError(
          "frget: syntax error. Usage: frget NEWVAR=VAR [...], from(LINKVAR)")));
      return true;
    }
    auto [head, optsPart] = splitTopLevelComma(trimWs(m.str(1)));
    (void)optsPart;
    std::vector<std::pair<std::string, std::string>> renames;
    {
      std::istringstream iss(trimWs(head));
      std::string tok;
      while (iss >> tok) {
        size_t eq = tok.find('=');
        if (eq == std::string::npos) {
          // Bare `frget VAR` copies VAR under its own name.
          renames.emplace_back(tok, tok);
        } else {
          renames.emplace_back(trimWs(tok.substr(0, eq)), trimWs(tok.substr(eq + 1)));
        }
      }
    }
    std::string fromLink;
    for (const auto& [k, v] : parsed.opts) {
      if (lower(k) == "from") fromLink = v.strValue;
    }
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kFrgetKnown = kVocabFrget.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kFrgetKnown)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // from() is required and is spelled in full: `frget price2` and `frget
    // mpg3, f(f3)` / `fr()` / `fro()` are r(198) "option from() required"
    // (vs-stata bank a03966b9b9b01fba, 505b505622cf70da ..).
    if (trimWs(fromLink).empty()) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "option from() required",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              "r(198)")));
      return true;
    }
    blocks.push_back(ojson(handlers::runFrget(renames, fromLink, session)));
    return true;
  }


  // BS-004/D5 (BETA B-data): `joinby key1 [key2 ...] using FILE` — bare form
  // (audit-confirmed syntax: `joinby [varlist] using filename [,
  // update replace unmatched(none|both|master|using) _merge(varname)
  // nolabel]`; analysis.do:451's own call uses zero options, the
  // unmatched(none) default). Mirrors the `merge` arm immediately above
  // (same raw-text `using`-split shape), minus merge's type-prefix capture
  // group (joinby has none).
  //
  // item3 (this lane, BUG-477 brief): `unmatched(none)` graduated to a real
  // acceptance below — it is provably the SAME operation as the bare form
  // (Stata's own default), not a new engine capability. `update`/`replace`/
  // `_merge()`/`nolabel`/`unmatched(master|both)` are still an honest reject
  // (D5: -> post-beta WP-18 for the first four; unmatched(master|both) needs
  // engine work this lane's brief scoped out of, see the session report) —
  // enforced by the known-options net below PLUS a value-level check for
  // `unmatched()` specifically, not a per-option allow-list.
  if (cmd == "joinby") {
    static const std::regex reJoinby(R"(^\s*joinby\s+(.+)$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(parsed.raw, m, reJoinby)) {
      blocks.push_back(ojson(eval::engineError(
          "joinby: syntax error. Usage: joinby key1 [key2 ...] using \"file\"")));
      return true;
    }
    std::string rest = m.str(1);
    // strip trailing options (`, opt...`) — split at the first comma
    // followed by a word char (same grammar the merge arm above uses).
    static const std::regex reOptSplitJoinby(R"(,\s*(?=\w))");
    std::string restNoOpts;
    {
      std::smatch om;
      if (std::regex_search(rest, om, reOptSplitJoinby)) {
        restNoOpts = trimWs(rest.substr(0, static_cast<size_t>(om.position(0))));
      } else {
        restNoOpts = trimWs(rest);
      }
    }
    static const std::regex reUsingSplitJoinby(R"(\busing\b)", std::regex::icase);
    std::smatch usingM;
    if (!std::regex_search(restNoOpts, usingM, reUsingSplitJoinby)) {
      // StataBE 19.5: r(100) "using required" (vs-stata bank `joinby make`).
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "using required", std::nullopt,
                                              std::nullopt, std::nullopt, "r(100)")));
      return true;
    }
    std::string keyPart = trimWs(restNoOpts.substr(0, static_cast<size_t>(usingM.position(0))));
    std::string filePart =
        trimWs(restNoOpts.substr(static_cast<size_t>(usingM.position(0) + usingM.length(0))));
    if (filePart.size() >= 2 && ((filePart.front() == '"' && filePart.back() == '"') ||
                                 (filePart.front() == '\'' && filePart.back() == '\''))) {
      filePart = filePart.substr(1, filePart.size() - 2);
    }
    std::vector<std::string> keylist;
    {
      std::istringstream iss(keyPart);
      std::string tok;
      while (iss >> tok) keylist.push_back(tok);
    }
    // An empty key list joins on the variables master and using have in common
    // (joinby.ado's Common; runJoinby works them out).
    if (filePart.empty()) {
      blocks.push_back(ojson(eval::engineError("joinby: empty filename after `using`")));
      return true;
    }
    // joinby.ado's unmatched(none|master|using|both) and _merge(name): the
    // engine keeps the unmatched rows and writes the 1/2/3 marker (cobra joinby
    // --unmatched/--merge-var; w2aw_data_combine_family.log dc_j_*).
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabJoinby.known,
                                              kVocabJoinby.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // joinby.ado UnMatch: unmatched() lowercased and prefix-matched
    // to both / master / using / none; anything else r(198) (dc_j_xyz).
    std::string joinUnmatched = "none";
    std::optional<std::string> joinMergeVar;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "_merge" && !v.isBool) joinMergeVar = strutil::trim(v.strValue);
      if (k != "unmatched") continue;
      const std::string val = v.isBool ? std::string() : strutil::lower(strutil::trim(v.strValue));
      if (val.empty()) continue;
      bool found = false;
      for (const char* word : {"both", "master", "using", "none"}) {
        const std::string w(word);
        if (val.size() <= w.size() && w.compare(0, val.size(), val) == 0) {
          joinUnmatched = w;
          found = true;
          break;
        }
      }
      if (!found) {
        blocks.push_back(ojson(eval::errorBlock(
            "syntax_error", strutil::trim(v.strValue) + " is invalid value for unmatch()",
            std::nullopt, std::nullopt, std::nullopt, std::string("r(198)"))));
        return true;
      }
    }
    if (preflight::gate("joinby", session, filePart, 0, blocks) == preflight::Verdict::Cancel) return true;
    bool joinNolabel = false;
    for (const auto& [k, v] : parsed.opts)
      if (k == "nolabel" && (v.isBool ? v.boolValue : true)) joinNolabel = true;
    blocks.push_back(ojson(handlers::runJoinby(keylist, filePart, session, joinUnmatched,
                                               joinMergeVar, joinNolabel)));
    return true;
  }


  if (cmd == "duplicates") {
    std::string sub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
    std::optional<std::vector<std::string>> subVarlist;
    if (parsed.varlist.size() > 1) {
      subVarlist = std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end());
    }
    std::optional<std::string> genvar;
    for (const auto& [k, v] : parsed.opts) {
      if ((k == "gen" || k == "generate") && !v.isBool) {
        genvar = v.strValue;
        break;
      }
    }
    // BS-097: `gen`/`generate` and `force` are the options this arm reads
    // (force: the drop rules below).
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    // duplicates' subcommand abbreviations (report, list and tag to one
    // letter; drop only in full) and its `force' rules: `drop varlist' needs
    // force (r(198)), `drop' over all variables refuses it (r(101)), tag's
    // syntax has no force (r(198)). w2ah_data_verbs.log ah_dup_drop_noforce rc
    // 198 with _N 8, ah_dup_drop_all_force rc 101 with _N 8; main dropped rows
    // at rc 0 in both.
    {
      const size_t l = sub.size();
      if (l > 0 && std::string("report").compare(0, l, sub) == 0) sub = "report";
      else if (l > 0 && std::string("examples").compare(0, l, sub) == 0) sub = "examples";
      else if (l > 0 && std::string("list").compare(0, l, sub) == 0) sub = "list";
      else if (l > 0 && std::string("tag").compare(0, l, sub) == 0) sub = "tag";
    }
    const std::unordered_set<std::string>& kDuplicatesKnownOpts = kVocabDuplicates.known;
    const bool listOptions = sub == "examples" || sub == "list";  // BUG-1096, see the vocabulary
    auto dupRefuse = [&](const std::string& msg, int rc) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", msg, std::nullopt, std::nullopt,
                                              std::nullopt,
                                              "r(" + std::to_string(rc) + ")")));
      return blocks;
    };
    // BUG-1096: examples and list pass every option to `list`, which knows neither
    // generate() nor force and refuses the first option it does not know, as
    // typed: `duplicates list x, g(z)` is "option g() not allowed", `, force g(z)`
    // "option force not allowed", `, nol g(z)` the g() (StataBE 19.5, r(198)).
    // report takes and ignores both. The options before the refused one still go
    // through the net, so an earlier unimplemented one is named first.
    // examples and list hand their options to `list` only when there is something to list: with no
    // duplicates duplicates.ado prints "(0 observations are duplicates)" and exits, whatever the
    // options (vs-stata bank: `duplicates list foreign rep78 price, zznotanoption` is rc 0 on data
    // with none). So for those two the options are read after the run, below.
    // report reads no option at all (vs-stata bank: `duplicates report foreign, zznotanoption` is rc 0).
    if (!listOptions && sub != "report") {
      if (auto err = eval::rejectUnknownOptions(parsed.opts, kDuplicatesKnownOpts,
                                                std::unordered_set<std::string>{})) {
        blocks.push_back(ojson(*err));
        return true;
      }
    }
    bool dupForce = false;
    for (const auto& [k, v] : parsed.opts)
      if (k == "force" && (!v.isBool || v.boolValue)) dupForce = true;
    if (sub == "drop" && subVarlist.has_value() && !dupForce)
      { blocks = dupRefuse("force option required with duplicates drop varlist", 198); return true; }
    if (sub == "drop" && !subVarlist.has_value() && dupForce)
      { blocks = dupRefuse("options not allowed", 101); return true; }
    if (sub == "tag" && dupForce) { blocks = dupRefuse("option force not allowed", 198); return true; }
    ojson dupRes = ojson(handlers::runDuplicates(sub, subVarlist, genvar, session, parsed.ifExpr,
                                                 parsed.inRange));
    if (listOptions && !parsed.opts.empty() && dupRes.is_object() &&
        dupRes.value("type", std::string()) != "error") {
      const std::string dupText = dupRes.value("text", std::string());
      const bool listed = sub == "list" ? dupRes.value("type", std::string()) == "browse"
                                        : dupText.find("(0 observations are duplicates)") ==
                                              std::string::npos;
      if (listed) {
        // The options go to `list` (duplicates.ado adds `subvarname noobs` itself). The spelling
        // in parsed.opts is already the bank-resolved one: a canonical list option, or the typed
        // key when Stata refuses that spelling. generate() and force are not list's. Stata's
        // refusals are answered as Stata answers them (r(198), r(111) for mean() / sum() / N()
        // naming a variable outside the varlist); a valid list option is not drawn by this
        // build's duplicates block, so apart from nolabel (the block carries its value labels)
        // and fast (no visible effect) it keeps the honest "not implemented" refusal.
        enum Form { kBare, kArg, kOptArg };
        static const std::map<std::string, Form> kListOpts = {
            {"abbreviate", kArg},  {"absolute", kBare},    {"clean", kBare},     {"compress", kBare},
            {"constant", kOptArg}, {"display", kBare},     {"divider", kBare},   {"ds", kOptArg},
            {"fast", kBare},       {"footer", kBare},      {"fvall", kBare},     {"header", kBare},
            {"labvar", kArg},      {"linesize", kArg},     {"mean", kOptArg},    {"n", kOptArg},
            {"nocompress", kBare}, {"nodotz", kBare},      {"noheader", kBare},  {"nolabel", kBare},
            {"notrim", kBare},     {"relative", kBare},    {"separator", kArg},  {"sepby", kArg},
            {"sepbyexp", kArg},    {"string", kArg},       {"sum", kOptArg},     {"table", kBare}};
        std::vector<std::string> typedNames;
        for (const auto& kv : parsed.opts) typedNames.push_back(kv.first);
        try {
          const auto asTyped = eval::tokenize(parsed.raw).opts;
          if (asTyped.size() == parsed.opts.size()) {
            typedNames.clear();
            for (const auto& kv : asTyped) typedNames.push_back(kv.first);
          }
        } catch (const eval::TokenizerError&) {
        }
        const std::vector<std::string> keyVars = subVarlist.value_or(std::vector<std::string>{});
        bool noLabel = false;
        std::optional<std::string> notDrawn;
        for (size_t oi = 0; oi < parsed.opts.size(); ++oi) {
          const std::string& k = parsed.opts[oi].first;
          const eval::OptValue& v = parsed.opts[oi].second;
          const std::string& typed = typedNames[oi];
          auto bad = [&](const std::string& msg, int rc) {
            blocks.push_back(ojson(eval::errorBlock("syntax_error", msg, std::nullopt, std::nullopt,
                                                    std::nullopt, "r(" + std::to_string(rc) + ")")));
            return true;
          };
          const std::string shown = typed + (v.isBool ? "" : "()");
          if (k == "gen" || k == "generate" || k == "force")
            return bad("option " + shown + " not allowed", 198);
          const auto it = kListOpts.find(k);
          // N() is capital in list's syntax; a lower-case n is not an option of it.
          if (it == kListOpts.end() || (k == "n" && typed != "N"))
            return bad("option " + shown + " not allowed", 198);
          const bool hasArg = !v.isBool;
          if ((it->second == kArg && !hasArg) || (it->second == kBare && hasArg))
            return bad("option " + typed + " incorrectly specified", 198);
          if (k == "separator" && hasArg) {
            size_t usedLen = 0;
            bool numeric = true;
            try {
              (void)std::stol(v.strValue, &usedLen);
            } catch (const std::exception&) {
              numeric = false;
            }
            if (!numeric || usedLen != v.strValue.size())
              return bad("option " + typed + " incorrectly specified", 198);
          }
          if ((k == "mean" || k == "sum" || k == "n") && hasArg) {
            std::istringstream names(v.strValue);
            for (std::string nm; names >> nm;)
              if (std::find(keyVars.begin(), keyVars.end(), nm) == keyVars.end())
                return bad(typed + "() variable " + nm + " not in varlist", 111);
          }
          if (k == "nolabel") noLabel = true;
          else if (k != "fast" && !notDrawn) notDrawn = k;
        }
        if (notDrawn) {
          blocks.push_back(ojson(eval::unsupportedOption(
              *notDrawn,
              "`duplicates " + sub + "` hands its options to `list`, but this build does not draw "
              "the effect of `" + *notDrawn + "` in the duplicates table.")));
          return true;
        }
        if (noLabel && dupRes.contains("columns") && dupRes["columns"].is_array())
          for (auto& col : dupRes["columns"])
            if (col.is_object()) col.erase("value_labels");
      }
    }
    blocks.push_back(std::move(dupRes));
    return true;
  }
  return false;
}

}
