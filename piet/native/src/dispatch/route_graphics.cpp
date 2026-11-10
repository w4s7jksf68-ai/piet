// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "graphics_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
#include "piet/handlers/graph.h"
namespace piet::dispatch_detail {
bool route_graphics(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    const std::string& graphLineTyped,
    QualifierColumnGuard& conditionMask) {


  // ==================== AER-SCOPE1 bundle J (GRAPH-X) arms ==================
  if (cmd == "grc1leg2") {
    ojson result = ojson(handlers::runGrc1leg(parsed, session));
    registerGraphOutput(result, session, parsed.opts, graphLineTyped);  // BS-011
    if (!(result.is_array() && result.empty())) blocks.push_back(result);
    return true;
  }

  // ==================== AER-SCOPE2 bundle K1 arms =========================
  // The corpus's remaining small names. `grc1leg` is an alias, `beep` and
  // `window` are accept-notes, and the three renamers below them have real
  // handlers in datamgmt/variable_bulk_rename.cpp and datamgmt/variable_move.cpp --
  // every rule in which was measured against StataBE with the packages installed,
  // before it was written. `parallel` is
  // the one still open (K1c).
  if (cmd == "grc1leg") {
    // Vince Wiggins' original (stata.trk: stata.com/users/vwiggins). We answer
    // it with the grc1leg2 handler — Mead Over's enhanced rewrite — which is an
    // ACCEPTED-NAMED substitution, not an equivalence: the two packages have
    // different option defaults, so the registry carries both authors and the
    // probe keeps an arm on a default that differs.
    ojson result = ojson(handlers::runGrc1leg(parsed, session));
    registerGraphOutput(result, session, parsed.opts, graphLineTyped);  // BS-011
    if (!(result.is_array() && result.empty())) blocks.push_back(result);
    return true;
  }


  if (cmd == "grstyle") {
    // Accept-note, not a silent no-op: this build applies no scheme styling, and
    // saying so once is the difference between "your styling was ignored" and a
    // user believing it took effect. A refusal would stop do-files that open
    // with `grstyle init` before any real work.
    ojson note;
    note["type"] = "display";
    note["text"] = "(grstyle: scheme styling is not applied in this build — accepted and ignored)";
    blocks.push_back(note);
    return true;
  }

  if (cmd == "cdfplot") {
    ojson result = ojson(handlers::runCdfplot(parsed, session));
    registerGraphOutput(result, session, parsed.opts, graphLineTyped);  // BS-011
    if (!(result.is_array() && result.empty())) blocks.push_back(result);
    return true;
  }

  if (cmd == "event_plot") {
    ojson result = ojson(handlers::runEventPlot(parsed, session));
    registerGraphOutput(result, session, parsed.opts, graphLineTyped);  // BS-011
    if (!(result.is_array() && result.empty())) blocks.push_back(result);
    return true;
  }


  if (cmd == "coefplot") {
    // coefplot-family AN-1..AN-4: coefplot.ado 1.8.8's own pipeline
    // (graph/coefplot.cpp) decides what is plotted, generate() and r(); the
    // option set is coefplot's (its unknown options are twoway's and pass), so
    // the W4 vocabulary net no longer runs here. `event_plot` keeps calling
    // runCoefplot directly. name() onto a graph in memory without replace is
    // r(110) before anything is computed (w2an cf_rf_name_dup, log:3076-3084).
    const std::string cpName = graphNameOption(parsed.opts);
    if (auto clash = serset::nameCollision(session, cpName)) {
      blocks.push_back(*clash);
      return true;
    }
    ojson drawRows = ojson::array();
    ojson pre = handlers::runCoefplotJann(session, parsed, &drawRows);
    for (auto& b : pre) {
      const bool isErr = b.is_object() && b.value("type", std::string()) == "error";
      blocks.push_back(b);
      if (isErr) return true;
    }
    if (drawRows.empty()) return true;  // (nothing to plot)
    // The drawn block: the established coefplot block of the stored models a
    // bare model list names (freeze-pinned shape); for any other statement the
    // same shape built from the rows coefplot.ado plots.
    ojson result;
    {
      std::string head = rawArgsAfterCmd(parsed.raw);
      const long long hc = eval::findUnquotedComma(head);
      if (hc >= 0) head = head.substr(0, static_cast<size_t>(hc));
      const bool bare = !trimWs(head).empty() && head.find('(') == std::string::npos &&
                        head.find('|') == std::string::npos && head.find('\\') == std::string::npos;
      if (bare) {
        ojson old = ojson(handlers::runCoefplot(parsed.varlist, parsed.opts, session));
        if (old.is_object() && old.value("type", std::string()) == "graph") result = old;
      }
      if (result.is_null()) {
        ojson models = ojson::array();
        std::map<std::string, size_t> at;
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (const auto& r : drawRows) {
          const std::string pl = r.value("plot_label", std::string());
          if (!at.count(pl)) {
            at[pl] = models.size();
            models.push_back(ojson{{"name", pl}, {"coefs", ojson::array()}});
          }
          models[at[pl]]["coefs"].push_back(
              ojson{{"name", r.value("name", std::string())}, {"b", r["b"]}, {"ci_lo", r["ci_lo"]}, {"ci_hi", r["ci_hi"]}});
          for (const char* k : {"b", "ci_lo", "ci_hi"}) {
            if (r[k].is_number()) {
              lo = std::min(lo, r[k].get<double>());
              hi = std::max(hi, r[k].get<double>());
            }
          }
        }
        if (!std::isfinite(lo)) {
          lo = -1;
          hi = 1;
        }
        const double pad = hi > lo ? (hi - lo) * 0.05 : 0.5;
        result["type"] = "graph";
        result["kind"] = "coefplot";
        result["title"] = "Coefficient plot";
        result["x_label"] = "Coefficient estimate";
        result["y_label"] = "Coefficient";
        result["models"] = models;
        result["level"] = 95;
        result["n_models"] = static_cast<int64_t>(models.size());
        result["x_min"] = lo - pad;
        result["x_max"] = hi + pad;
        result["name"] = nullptr;
      }
    }
    registerGraphOutput(result, session, parsed.opts, graphLineTyped);  // BS-011
    ojson savedNote;  // BUG-1529: `file cf1.gph saved` (StataBE 19.5, g2.do)
    for (const auto& [k, v] : parsed.opts) {
      if (k == "saving" && !v.isBool) {
        ojson se = handlers::saveGraphOption(result, v.strValue, session, nullptr, &savedNote);
        if (!se.is_null()) {
          blocks.push_back(se);
          return true;
        }
      }
    }
    blocks.push_back(result);
    if (!savedNote.is_null()) blocks.push_back(savedNote);
    return true;
  }


  if (cmd == "marginsplot") {
    // BUG-1422: `marginsplot [, options]` takes no varlist, `if`, `in` or weights;
    // a stray term used to be dropped here and the plot drawn (or refused at rc 1
    // for want of margins), where Stata stops first, in this order: r(101)
    // "varlist not allowed", "if not allowed", "in range not allowed", "weights
    // not allowed" (StataBE 19.5, after `margins`). The tokenizer leaves `in` and
    // weights to this arm (registry.h qualifiersRefusedByArm).
    const char* notAllowed = !parsed.varlist.empty()       ? "varlist not allowed"
                             : parsed.ifExpr.has_value()     ? "if not allowed"
                             : parsed.inRange.has_value()    ? "in range not allowed"
                             : parsed.weightType.has_value() ? "weights not allowed"
                                                             : nullptr;
    if (notAllowed != nullptr) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", notAllowed, std::nullopt, std::nullopt,
                                              std::nullopt, "r(101)")));
      return true;
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMarginsplot.known,
                                              kVocabMarginsplot.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    ojson result = ojson(handlers::runMarginsplot(parsed.opts, session));
    graphAcceptedNotApplied(result, "marginsplot", parsed.opts, kMarginsplotApplied);
    registerGraphOutput(result, session, parsed.opts, graphLineTyped);  // BS-011
    blocks.push_back(result);
    return true;
  }


  // serset-twoway SS: the `serset` command family (serset.sthlp; piet/serset.h).
  // It needs no data: bare `serset` with nothing in memory is rc 0 with r(id) -1
  // (w2al_twoway_family.log:177).
  if (cmd == "serset") {
    bool clearOpt = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "clear") {
        clearOpt = true;
        continue;
      }
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "option " + k + " not allowed",
                                              std::nullopt, std::nullopt, std::nullopt, "r(198)")));
      return true;
    }
    ojson r = serset::runSerset(session, parsed.varlist, clearOpt);
    for (auto& b : r) blocks.push_back(b);
    return true;
  }


  // ------------------------------------------------------------------
  // M4a item 4 + M4b: graph JSON surface. Scope: scatter/histogram/
  // kdensity/line/xtline/tsline + the bare (non-parenthesised) `twoway
  // PLOTTYPE var...` single-plot form (M4a); `graph export`/`graph save`/
  // `graph combine` (M4b — see graph/files.cpp's runGraphExport/Save/Combine).
  // The parenthesised multi-layer `twoway (rarea ...) (scatter ...)` form
  // (twoway_opts.py/twoway_parse.py) remains M4a-remaining/out of scope —
  // rejected explicitly below, not silently mis-parsed.
  if (cmd == "scatter" || cmd == "histogram" || cmd == "hist" || cmd == "kdensity" ||
      cmd == "line" || cmd == "xtline" || cmd == "tsline" || cmd == "twoway" || cmd == "graph") {
    std::string gc = cmd;
    std::vector<std::string> vl = parsed.varlist;
    std::optional<std::string> ifExpr = parsed.ifExpr;

    if (cmd == "graph") {
      std::string sub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
      // graph.ado's subcommand abbreviations: `do` == bsubstr(full, 1,
      // max(min, length(do))).
      auto subIs = [&](const char* full, size_t minLen) {
        const std::string f(full);
        return sub.size() >= minLen && sub.size() <= f.size() && f.compare(0, sub.size(), sub) == 0;
      };
      // graph-family AI-4/AI-6/AI-7/AI-9: graph describe/copy/rename/display/
      // draw/close/set/query (graph/named_graphs.cpp).
      {
        const std::string subArgs = [&]() {
          std::string a = rawArgsAfterCmd(rawArgsAfterCmd(parsed.raw));
          const long long c = eval::findUnquotedComma(a);
          if (c >= 0) a = a.substr(0, static_cast<size_t>(c));
          return trimWs(a);
        }();
        if (sub == "draw" || subIs("display", 2)) {
          ojson r = handlers::runGraphDisplay(session, subArgs, parsed.opts);
          for (auto& b : r) blocks.push_back(b);
          return true;
        }
        if (subIs("describe", 1)) {
          ojson r = handlers::runGraphDescribe(session, subArgs);
          for (auto& b : r) blocks.push_back(b);
          return true;
        }
        if (sub == "copy" || sub == "rename") {
          bool replace = false;
          for (const auto& [k, v] : parsed.opts) {
            if (k == "replace") {
              replace = true;
              continue;
            }
            blocks.push_back(ojson(eval::errorBlock("syntax_error", "option " + k + " not allowed",
                                                    std::nullopt, std::nullopt, std::nullopt, "r(198)")));
            return true;
          }
          std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
          ojson r = handlers::runGraphCopyRename(session, sub == "rename", names, replace);
          for (auto& b : r) blocks.push_back(b);
          return true;
        }
        // graph close: windows only; a batch run has none (w2ai gf_close_065/062
        // rc 0). graph set: printer/window/export settings
        // (gf_set_window, gf_set_ps rc 0); nothing here reads them.
        // graph query lists the settings.
        if (sub == "close" || sub == "set" || subIs("query", 1)) return true;
      }
      if (sub == "export") {
        // The filename can contain spaces (the Korean Desktop is "바탕 화면", and
        // any "My Folder\..." path), so extract it from the RAW command like
        // `use` does — parsed.varlist[1] is whitespace-tokenized and truncates a
        // spaced path at the first space (-> ext "" -> "unsupported format ''").
        // BUG-251/WS-Q: was `lower(raw).find("export")` + `substr(pos + 6)`.
        // npos-GUARDED, so this pair never crashed — but it is the same
        // re-derivation, with a literal copy of a SUB-command spelling, and its
        // npos path degrades silently to the whitespace-tokenized varlist that
        // the site exists to avoid. Two token-skips is what "after `graph
        // export`" means, and it needs no spelling at all.
        std::string exAfter = rawArgsAfterCmd(rawArgsAfterCmd(parsed.raw));
        size_t exComma = exAfter.find(',');  // drop trailing `, replace`/`width()` options
        if (exComma != std::string::npos) exAfter = trimWs(exAfter.substr(0, exComma));
        if (exAfter.size() >= 2 && ((exAfter.front() == '"' && exAfter.back() == '"') ||
                                    (exAfter.front() == '\'' && exAfter.back() == '\'')))
          exAfter = exAfter.substr(1, exAfter.size() - 2);
        std::string filepath = !exAfter.empty()
                                   ? exAfter
                                   : (parsed.varlist.size() > 1 ? parsed.varlist[1] : std::string());
        if (filepath.empty()) {
          blocks.push_back(ojson(eval::engineError("syntax_error: `graph export` requires a filename")));
          return true;
        }
        blocks.push_back(ojson(handlers::runGraphExport(filepath, parsed.opts, session)));
        return true;
      }
      if (sub == "save") {
        // graph save: `graph save [name] filename [, replace asis]` --
        // one word is the file, two are the graph name and the file; each may be
        // quoted (w2ai gf_save_007 `graph save "Graph" "file"`, gf_save_gp3
        // `graph save gp3 "file"`, log:1478-1491). The raw text is read so a
        // quoted path with blanks stays whole.
        std::optional<std::string> name;
        std::optional<std::string> filepath;
        std::string svAfter = rawArgsAfterCmd(rawArgsAfterCmd(parsed.raw));
        const long long svComma = eval::findUnquotedComma(svAfter);
        if (svComma >= 0) svAfter = svAfter.substr(0, static_cast<size_t>(svComma));
        std::vector<std::string> words;
        {
          size_t i = 0;
          while (i < svAfter.size()) {
            while (i < svAfter.size() && std::isspace(static_cast<unsigned char>(svAfter[i]))) ++i;
            if (i >= svAfter.size()) break;
            if (svAfter[i] == '"' || svAfter[i] == '\'') {
              const char q = svAfter[i];
              const size_t close = svAfter.find(q, i + 1);
              words.push_back(svAfter.substr(i + 1, close == std::string::npos ? std::string::npos
                                                                                : close - i - 1));
              i = close == std::string::npos ? svAfter.size() : close + 1;
              continue;
            }
            size_t j = i;
            while (j < svAfter.size() && !std::isspace(static_cast<unsigned char>(svAfter[j]))) ++j;
            words.push_back(svAfter.substr(i, j - i));
            i = j;
          }
        }
        if (words.empty()) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", "filename name required", std::nullopt,
                                                  std::nullopt, std::nullopt, "r(198)")));
          return true;
        }
        if (words.size() > 2) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax, " + words[2] + " not allowed",
                                                  std::nullopt, std::nullopt, std::nullopt, "r(198)")));
          return true;
        }
        if (words.size() == 2) {
          name = words[0];
          filepath = words[1];
        } else {
          filepath = words[0];
        }
        blocks.push_back(ojson(handlers::runGraphSave(name, filepath, parsed.opts, session)));
        return true;
      }
      if (sub == "combine") {
        std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
        // graph-family AI-4: name() onto a graph in memory without replace is r(110).
        const std::string cbNameOpt = graphNameOption(parsed.opts);
        if (auto clash = serset::nameCollision(session, cbNameOpt)) {
          blocks.push_back(*clash);
          return true;
        }
        ojson combined = ojson(handlers::runGraphCombine(names, parsed.opts, session));
        ojson cbSavedNote;  // BUG-1529: `file v10.gph saved` (StataBE 19.5, g1.do V10)
        if (combined.is_object() && combined.value("type", std::string()) == "graph") {
          // graph-family AI-4: the combined graph registers like any other
          // (family `combine`, w2ai gf_cmb_desc_dc log:1812-1813), and its
          // saving() writes it (AI-11, gf_cmb_019_f log:1774-1787).
          const ojson cbMeta = graphMetaFor(graphLineTyped, "combine", parsed.opts);
          serset::registerGraph(session, combined, cbNameOpt, {}, &cbMeta);
          std::optional<std::string> savingOpt;
          for (const auto& [k, v] : parsed.opts) {
            if (k == "saving" && !v.isBool) savingOpt = v.strValue;
          }
          if (savingOpt.has_value()) {
            ojson se = handlers::saveGraphOption(combined, *savingOpt, session, nullptr, &cbSavedNote);
            if (!se.is_null()) {
              blocks.push_back(se);
              return true;
            }
          }
        }
        // W4: runGraphCombine reads exactly these eight (graph/files.cpp).
        // The corpus also hands it `scale()` (25 uses) and `xcommon` (1), which
        // it does not read; those now resolve (kGraphCosmetic) and are named in
        // the note instead of vanishing. No reject net is added here: this
        // sub-command arm never had one, and widening it is W4's neighbour, not
        // W4 -- what the wave owes is that an accepted option is never silent.
        static const std::unordered_set<std::string> kCombineApplied = {
            "rows", "cols", "title", "subtitle", "note", "name", "imargin", "graphregion", "saving"};
        graphAcceptedNotApplied(combined, "graph combine", parsed.opts, kCombineApplied);
        blocks.push_back(combined);
        if (!cbSavedNote.is_null()) blocks.push_back(cbSavedNote);
        return true;
      }
      // serset-twoway GST / HIST-4: `graph dir [pattern] [, memory gph detail]`
      // (graph dir): r(list) = the in-memory names (byte-sorted) and the *.gph
      // files, `"`mlist' `glist'"'` (w2al tf_ss_two_gd `tf_g1 tf_g2 `, log:5510).
      // serset-twoway GST: `graph use filename [, name() nodraw ...]`.
      if (sub == "use") {
        std::string useAfter = rawArgsAfterCmd(rawArgsAfterCmd(parsed.raw));
        const long long uc = eval::findUnquotedComma(useAfter);
        if (uc >= 0) useAfter = useAfter.substr(0, static_cast<size_t>(uc));
        ojson r = handlers::runGraphUse(trimWs(useAfter), parsed.opts, session);
        for (auto& b : r) blocks.push_back(b);
        return true;
      }
      if (sub == "dir") {
        bool memOpt = false, gphOpt = false;
        for (const auto& [k, v] : parsed.opts) {
          if (k == "memory" || k == "mem" || k == "m") {
            memOpt = true;
          } else if (k == "gph" || k == "g") {
            gphOpt = true;
          } else if (k == "detail" || k == "d") {
            continue;
          } else {
            blocks.push_back(ojson(eval::errorBlock("syntax_error", "option " + k + " not allowed",
                                                    std::nullopt, std::nullopt, std::nullopt,
                                                    "r(198)")));
            return true;
          }
        }
        const std::string pattern = parsed.varlist.size() > 1 ? parsed.varlist[1] : std::string();
        std::string cwdDir;
        if (session.cwd.has_value() && !session.cwd->empty()) {
          cwdDir = *session.cwd;
        } else {
          std::error_code cwdEc;
          cwdDir = std::filesystem::current_path(cwdEc).u8string();
        }
        ojson r = serset::runGraphDir(session, pattern, memOpt, gphOpt, cwdDir);
        for (auto& b : r) blocks.push_back(b);
        return true;
      }
      if (sub == "drop") {
        // BS-056: `graph drop {namelist | _all}` -- only `parsed.varlist[1]`
        // (the FIRST name) was ever read, so `graph drop g1 g2` silently
        // dropped g1 and said nothing about g2; a name that didn't match
        // ANY registered graph (including a literal "all" typed without
        // the leading underscore Stata's own grammar requires) hit the same
        // `erase()` no-op-if-absent call and still reported "(graph
        // dropped)" — a silent false success either way. Now: every name
        // token is processed; `_all` (case-insensitive, anywhere in the
        // list) clears the whole named-graph store; otherwise every name is
        // validated to exist BEFORE any of them are dropped (all-or-nothing,
        // no partial drop on error) and an unknown name is an honest reject
        // naming it, never a silent no-op.
        std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
        bool dropAll = false;
        for (const auto& nm : names) {
          if (lower(nm) == "_all") {
            dropAll = true;
            break;
          }
        }
        if (dropAll) {
          session.namedGraphs.clear();
          session.graphMeta.clear();  // graph-family AI-4
          session.currentGraph.clear();
          // serset-twoway SS: a graph's sersets go with it (w2al tf_ss_afterdrop_sc
          // `0|-1|`, log:5525).
          serset::dropGraphSersets(session, {}, true);
        } else if (!names.empty()) {
          for (const auto& nm : names) {
            if (session.namedGraphs.count(nm) == 0) {
              // graph drop: `graph X not found` / `no graphs dropped`, r(111)
              // (w2ai gf_drop_absent, log:2252-2259).
              blocks.push_back(ojson(eval::errorBlock("engine_error",
                                                      "graph " + nm + " not found\nno graphs dropped",
                                                      std::nullopt, std::nullopt, std::nullopt,
                                                      "r(111)")));
              return true;
            }
          }
          for (const auto& nm : names) {
            session.namedGraphs.erase(nm);
            session.graphMeta.erase(nm);
            if (session.currentGraph == nm) session.currentGraph.clear();
          }
          serset::dropGraphSersets(session, names, false);
        }
        ojson dropOut;
        dropOut["type"] = "display";
        dropOut["class"] = "note";
        dropOut["text"] = "(graph dropped)";
        blocks.push_back(dropOut);
        return true;
      }
      // `graph twoway ...` / other `graph <sub>` forms: strip the leading
      // "graph" token's sub-command and fall through to the plot dispatch
      // below (mirrors commands.py's final `sub_varlist = parsed.varlist[1:]`
      // fallthrough).
      // serset-twoway WT: the `graph` row forwards weight clauses now (for `graph
      // twoway`); the sub-commands that do not take one here (bar/hbar/box/dot/
      // pie are not implemented; export/save/combine/drop/dir/use take none)
      // keep the refusal the tokenizer gate gave them, verbatim.
      // graph-family AI-3: graph bar/hbar/box/hbox take [aw fw pw] (graph_bar.sthlp
      // weights; w2ai gf_bar_aw/fw/pw, gf_box_aw); their handler answers iw.
      if (parsed.weightType.has_value() && !parsed.weightType->empty() &&
          !(sub == "twoway" || sub == "tw" || sub == "two" || sub == "twow" || sub == "twowa" ||
            sub == "bar" || sub == "hbar" || sub == "box" || sub == "hbox")) {
        blocks.push_back(ojson(eval::unsupportedOption(
            "[" + *parsed.weightType + "=" + parsed.weightVar.value_or("") + "]",
            "`graph` does not support weighted estimation in this engine (no "
            "[aw=]/[fw=]/[pw=]/[iw=] support for this command). Remove the weight specification "
            "— unweighted estimation will differ from weighted results.")));
        return true;
      }
      if (sub == "twoway" || sub == "tw" || sub == "two" || sub == "twow" || sub == "twowa") {
        // serset-twoway PARA: `gr tw (...)` is `graph twoway` (w2al tf_q_051).
        gc = "twoway";
        vl = std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end());
      } else if (!sub.empty()) {
        gc = sub;
        vl = std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end());
      } else {
        gc = "scatter";
      }
    }

    if (!session.hasData()) {
      blocks.push_back(ojson(eval::noDataLoaded()));
      return true;
    }

    // serset-twoway IN: the twoway family (scatter/line/tsline/twoway/graph)
    // honours `in` now -- the layer path reads each plot's own and the global
    // one, the single-plot path filters by it below. The other graph commands
    // keep the refusal.
    const bool inServed = cmd == "scatter" || cmd == "line" || cmd == "tsline" || cmd == "twoway" ||
                          cmd == "graph";
    if (!inServed && parsed.inRange.has_value() && !parsed.inRange->empty()) {
      blocks.push_back(ojson(eval::engineError(
          "graph: `in` range is not yet supported for graph commands; use `if`.",
          "syntax_error")));
      return true;
    }

    // `twoway (plottype ...) (plottype ...) ...` (parenthesised multi-layer
    // form): route through runTwowayLayers when raw_args contains a
    // top-level paren group — mirrors dispatcher.py's "Stage-4: multi-layer
    // twoway" branch (raw_args extraction regex `^\s*twoway\b\s*(.*)$` /
    // `^\s*graph\s+twoway\b\s*(.*)$`, checked BEFORE the bare single-plot
    // form below so a parenthesised layer group never falls through to the
    // bare-form plot-type stripping).
    // serset-twoway PARA: `scatter y x ... || scatter ... || lfit ...` (the
    // scatter/line/tsline COMMAND with a top-level `||`) is the same twoway graph
    // (w2al tf_tw_082).
    std::string cmdLevelPipeArgs;
    if (cmd == "scatter" || cmd == "line" || cmd == "tsline") {
      const std::string after = rawArgsAfterCmd(parsed.raw);
      if (expandDoublePipeLayers(after) != after) {
        gc = "twoway";
        cmdLevelPipeArgs = cmd + " " + after;
      }
    }
    if (gc == "twoway") {
      static const std::regex reTwowayRaw(R"(^\s*twoway\b\s*(.*)$)", std::regex::icase);
      static const std::regex reGraphTwowayRaw(
          R"(^\s*graph\s+(?:tw|two|twow|twowa|twoway)\b\s*(.*)$)", std::regex::icase);
      std::string rawArgs;
      std::smatch m;
      // AER-SCOPE1 bundle A: `tw (sc y x)` and `gr twoway ...` resolve to the
      // canonical `cmd` at dispatch, but THESE regexes read the raw line, which
      // still carries what the user typed. rawWithCanonicalCmd rewrites just the
      // leading word (BUG-251's fix, reused here) so the abbreviation reaches
      // the same layer parser as the full spelling.
      const std::string graphRaw = rawWithCanonicalCmd(parsed.raw, cmdTyped, cmd);
      if (!cmdLevelPipeArgs.empty()) {
        rawArgs = cmdLevelPipeArgs;
      } else if (cmd == "graph" && std::regex_match(graphRaw, m, reGraphTwowayRaw)) {
        rawArgs = m[1].str();
      } else if (std::regex_match(graphRaw, m, reTwowayRaw)) {
        rawArgs = m[1].str();
      }
      // BS-053: `||`-separated layers, no parens at all -- synthesize the
      // parenthesised equivalent BEFORE the hasToplevelParens gate below so
      // it takes the exact same multi-layer path a hand-parenthesised
      // command would (see expandDoublePipeLayers' doc comment above). A
      // no-op (returns the input unchanged) when there's no top-level `||`,
      // or when the command already used real parens.
      if (!rawArgs.empty() && !twoway::hasToplevelParens(rawArgs)) {
        rawArgs = expandDoublePipeLayers(rawArgs);
      }
      // serset-twoway PARB/FN/SCI: a bare `twoway function ...` / `twoway
      // scatteri ...` is read by the layer parser too: its text is an expression
      // or an immediate point list, never a varlist (w2al tf_fn_noy, tf_sci_lab).
      bool bareImmediate = false;
      {
        const std::string ra = trimWs(rawArgs);
        size_t he = 0;
        while (he < ra.size() && !std::isspace(static_cast<unsigned char>(ra[he])) && ra[he] != ',') ++he;
        const std::string head = lower(ra.substr(0, he));
        bareImmediate = head == "function" || head == "scatteri";
      }
      if (!rawArgs.empty() && (twoway::hasToplevelParens(rawArgs) || bareImmediate)) {
        twoway::ParseResult pr = twoway::parseTwoway(rawArgs);
        // W4: the layer parser reads the comma side ITSELF (twoway_parse.cpp's
        // parseLayerInterior / parseTwoway step 1), so these keys never passed
        // through the dispatcher's canonicalizeForCommand and arrived at
        // layerStyle spelled the way the user typed them. That is BUG-233's
        // shape exactly: the net (now) accepts `lc(gs11)` as a prefix of
        // `lcolor`, and layerStyle -- which compares raw keys and reads `lcolor`,
        // `lw`, `lp`, `mc`, `ms`, `msi`, `mz` but NOT `lc` -- then dropped it
        // unread. Corpus-measured: `lc` (4 uses), `msymb` (14), `xti`/`yti`,
        // `xlab`/`ylab`, `xsc`/`ysc`, `leg`, `horiz`. Every one of them is a
        // unique prefix of a member of the registered vocabulary, so the prefix
        // rule reaches all of them and the registration needs no declared-alias
        // table (BUG-430's mechanism is for the ones a prefix CANNOT reach, and
        // this surface has none in the corpus).
        for (auto& lspec : pr.layers) {
          eval::canonicalizeForCommand(lspec.opts, "twoway_layer");
        }
        eval::canonicalizeForCommand(pr.globalOpts, "twoway_layer");
        hoistTwowayLevelOptions(pr);  // serset-twoway PARA
        if (pr.layers.empty()) {
          blocks.push_back(ojson(eval::engineError(
              "twoway: no plot layers found. Check syntax.", "syntax_error")));
          return true;
        }
        // serset-twoway PARA: the layer parser's global block is the whole
        // truth. The tokenizer's opts (parsed.opts) were merged in here, but
        // the tokenizer cuts at the FIRST comma, which in `plot1, opts || plot2`
        // or `(plot1, opts) (plot2)` lies inside a plot, so its keys were the
        // plot heads, `||` and variable names (`option || not allowed`).
        std::vector<std::pair<std::string, eval::OptValue>> merged = pr.globalOpts;
        // Use ONLY the layer parser's global if — the tokenizer's if_expr is
        // meaningless here (its greedy `\bif` match swallows paren groups).
        Datastore* dsPtrTw = session.datastore();
        // W4: the unknown-option net for BOTH planes (the global block and each
        // layer's interior) lives inside runTwowayLayers, AFTER its
        // supported-plot-type pre-scan, so `(rcap ...)` still answers "plot type
        // is not supported" rather than complaining about `lstyle`. See
        // graph/twoway.cpp. Only the note is assembled here, because only the
        // dispatcher knows which of the two routes produced the block.
        //
        // The note reads the global block AND every layer interior: a cosmetic
        // option written inside a plot's own parentheses is just as silently
        // dropped as one written after the comma, so both are reported.
        std::vector<std::pair<std::string, eval::OptValue>> twNoteOpts = merged;
        for (const auto& lspec : pr.layers) {
          for (const auto& kv : lspec.opts) twNoteOpts.push_back(kv);
        }
        // serset-twoway SS: name() onto a graph in memory without replace is
        // r(110) before anything is drawn (w2al tf_st_dup2, log:5024-5029).
        const std::string twNameOpt = graphNameOption(merged);
        if (auto clash = serset::nameCollision(session, twNameOpt)) {
          blocks.push_back(*clash);
          return true;
        }
        // serset-twoway QIF: a plot's own `if` (or the global one) holding a
        // string function or a compare against "" is read by the row evaluator
        // through a hidden mask column, as every other command's is (dispatchImpl's
        // QualifierColumnGuard, which removes the columns when this returns); the
        // engine's f64 evaluator cannot (w2al tf_q_039 log:4760-4767,
        // tf_q_emptystr, tf_q_globalstr).
        {
          auto routeCondition = [&](std::optional<std::string>& cond) -> std::optional<ojson> {
            if (!cond.has_value() || cond->empty() || !session.hasData() ||
                !strmutate::needsConditionMask(session, *cond)) {
              return std::nullopt;
            }
            strmutate::ConditionMaskInstall mi = strmutate::installConditionMask(session, *cond);
            if (!mi.ok) return mi.error;
            conditionMask.columns.push_back(mi.column);
            cond = mi.condition;
            return std::nullopt;
          };
          if (auto e = routeCondition(pr.ifExpr)) {
            blocks.push_back(*e);
            return true;
          }
          for (auto& l : pr.layers) {
            if (auto e = routeCondition(l.ifExpr)) {
              blocks.push_back(*e);
              return true;
            }
          }
          dsPtrTw = session.datastore();
        }
        std::vector<serset::PlotSpec> twPlots;
        ojson twResult = ojson(handlers::runTwowayLayers(dsPtrTw, session, pr.layers, merged, pr.ifExpr,
                                                         &twPlots, pr.inRange));
        {
          std::unordered_set<std::string> twApplied = {"title", "subtitle", "xtitle", "ytitle",
                                                       "note", "name"};
          twApplied.insert(kGraphFitApplied.begin(), kGraphFitApplied.end());
          twApplied.insert(kGraphTwowayGlobals.begin(), kGraphTwowayGlobals.end());
          twApplied.insert(kGraphLayerStyle.begin(), kGraphLayerStyle.end());
          twApplied.insert("sort");  // line/connected/area/spike/dropline/bar order by x only under it (BUG-404)
          graphAcceptedNotApplied(twResult, "twoway", twNoteOpts, twApplied);
        }
        ojson twSavedNote;  // BUG-1529: `file n2sv1.gph saved` (StataBE 19.5, a15.do)
        if (twResult.is_object() && twResult.value("type", std::string()) == "graph") {
          // serset-twoway SS: the one registration (lastGraph, namedGraphs, the
          // graph's sersets; piet/serset.h).
          const ojson twMeta = graphMetaFor(graphLineTyped, "twoway", merged);  // graph-family AI-4
          serset::registerGraph(session, twResult, twNameOpt,
                                serset::buildPlotSersets(std::move(twPlots)), &twMeta);
          // serset-twoway GST: saving() writes the graph and its sersets (w2al
          // tf_tw_069a_f, tf_st_sav_f; r(602) without replace onto a file that
          // exists: tf_st_noreplace, log:5131-5138).
          std::optional<std::string> savingOpt;
          for (const auto& [k, v] : merged) {
            if (k == "saving" && !v.isBool) savingOpt = v.strValue;
          }
          if (savingOpt.has_value()) {
            const serset::GraphName gn = serset::parseNameOption(twNameOpt);
            const std::vector<serset::Serset> owned =
                serset::ownedBy(session, gn.name.empty() ? std::string("Graph") : gn.name);
            ojson se = handlers::saveGraphOption(twResult, *savingOpt, session, &owned, &twSavedNote);
            if (!se.is_null()) {
              blocks.push_back(se);
              return true;
            }
          }
        }
        handlers::decimateGraphBlockForView(twResult);  // L2-FIX-B (BUG-066): live-view only
        blocks.push_back(twResult);
        if (!twSavedNote.is_null()) blocks.push_back(twSavedNote);
        return true;
      }
    }

    // serset-twoway HKL: `twoway kdensity|histogram var` is the twoway PLOT
    // TYPE (n 300 over [min, max]; no start() unless given), not the
    // kdensity/histogram COMMAND (n 50 over a grid widened by the bandwidth;
    // start(r(min)); the bin note): w2al tf_kd_n log:3524-3540, tf_hist_082.
    // It takes the layer renderer like the range plots below.
    bool twowayKdHist = false;
    // `twoway PLOTTYPE var...` (bare, non-parenthesised form): strip the
    // leading plot-type keyword.
    if (gc == "twoway") {
      // BS-054: `twoway kdensity x` / `twoway histogram x` (the umbrella-
      // prefixed spellings) previously weren't in this set at all, so
      // "kdensity"/"histogram" fell straight through as an ordinary
      // varlist token — `gc` degraded to "scatter" and `vl` still started
      // with the literal word "kdensity"/"histogram", which runScatter then
      // tried to look up AS A VARIABLE ("variable kdensity not found" —
      // exactly the reported symptom). The un-prefixed bare forms
      // (`kdensity x` / `histogram x`, no leading "twoway") already worked
      // fine via their own top-level `cmd` dispatch below; adding these two
      // names here just makes the "twoway "-prefixed spelling strip down to
      // the SAME already-working gc=="kdensity"/gc=="histogram" branch
      // further down this function (no new handler needed).
      static const std::set<std::string> kTwowayPlotTypes = {
          "line",     "scatter", "connected", "area",      "bar",      "spike",
          "dot",      "dropline", "rarea",    "rcap",      "rcapsym",
          "kdensity", "histogram", "hist",
          // serset-twoway FIT/PARB: the fit plot types are ordinary bare types
          // (w2al tf_lfit_range `twoway lfit y x, range(-2 14)`).
          "lfit",     "qfit",    "lfitci",    "qfitci",
          "rspike",   "rline",   "rconnected", "rscatter",  // serset-twoway RNG
      };
      if (!vl.empty() && kTwowayPlotTypes.count(lower(vl[0]))) {
        gc = lower(vl[0]);
        vl = std::vector<std::string>(vl.begin() + 1, vl.end());
        twowayKdHist = gc == "kdensity" || gc == "histogram" || gc == "hist";
      } else {
        gc = "scatter";
      }
    }

    // serset-twoway SS: name() onto a graph in memory without replace is r(110)
    // before anything is drawn (w2al tf_st_dup2, log:5024-5029).
    const std::string bareNameOpt = graphNameOption(parsed.opts);
    if (auto clash = serset::nameCollision(session, bareNameOpt)) {
      blocks.push_back(*clash);
      return true;
    }

    // serset-twoway QIF: a string condition takes the row-mask route (see the
    // layer path above).
    if (ifExpr.has_value() && !ifExpr->empty() &&
        strmutate::needsConditionMask(session, *ifExpr)) {
      strmutate::ConditionMaskInstall mi = strmutate::installConditionMask(session, *ifExpr);
      if (!mi.ok) {
        blocks.push_back(mi.error);
        return true;
      }
      conditionMask.columns.push_back(mi.column);
      ifExpr = mi.condition;
    }

    Datastore* dsPtr = session.datastore();
    std::string tmpGraphHbin;
    std::unique_ptr<Datastore> tmpDs;
    const bool bareIn = parsed.inRange.has_value() && !parsed.inRange->empty();
    if (bareIn) {
      // serset-twoway IN: `in` (with or without `if`) on the single-plot path.
      try {
        handlers::InRangeFilterResult fr =
            handlers::applyIfAndInFilter(session, *session.currentHbin(), ifExpr, parsed.inRange);
        if (!fr.ok) {
          blocks.push_back(handlers::inRangeRejectError(fr, *parsed.inRange, gc));
          return true;
        }
        if (fr.dataPath != *session.currentHbin()) tmpGraphHbin = fr.dataPath;
        tmpDs = std::make_unique<Datastore>(fr.dataPath);
        dsPtr = tmpDs.get();
      } catch (const CobraError& ce) {
        blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
        return true;
      }
    } else if (ifExpr.has_value() && !ifExpr->empty()) {
      tmpGraphHbin = dispatcherNextPath(session.workdir(), "graphif");
      // BUG-369 follow-up: `twoway ... if L.y>0` — same injection as above.
      std::vector<std::string> args = translate::injectTsContextForExpr(
          {"eval", "--data", dsPtr->path(), "--keepif", *ifExpr, "--out", tmpGraphHbin},
          *ifExpr, session);
      try {
        runCobraJson(args);
      } catch (const CobraError& ce) {
        // BUG-038(a): route through the shared mapping point instead of
        // formatting "[code] message" inline (piet::cobraErrorMessage,
        // cobra_runner.h) — same fix as every other CobraError catch site.
        blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
        return true;
      }
      tmpDs = std::make_unique<Datastore>(tmpGraphHbin);
      dsPtr = tmpDs.get();
    }

    ojson result;
    std::optional<ojson> histOptErr;  // BUG-1306: histogram's unknown option, emitted after its note
    // BUG-034: `graph bar/hbar/pie/box/dot ...` used to fall straight
    // through to the `else` below and get misrouted through runScatter,
    // producing the misleading "scatter requires two variables" error
    // instead of an honest gap (root cause: unrecognized graph
    // sub-commands set gc=sub above with nothing downstream to catch
    // them). Mirrors the parenthesised multi-layer `twoway (plottype ...)`
    // form's reject-before-output guard (runTwowayLayers' supported-plot-type
    // pre-scan, graph/twoway.cpp — BUG-129) — applied here to the single-plot
    // dispatch path so these name the gap explicitly (SPEC I-3) rather than
    // silently misrouting to scatter's grammar.
    static const std::set<std::string> kUnsupportedBareGraphTypes = {
        "bar", "hbar", "pie", "box", "hbox", "dot",
    };
    // BUG-129/BUG-130 (graph-honesty): the bare (non-parenthesised)
    // `twoway rarea ...` / `twoway rcap ...` / `twoway area ...` etc. spellings
    // previously fell through to runScatter below — `rarea` silently
    // misrendered as a 2-var scatter (its band never built; BUG-130) and every
    // unsupported range-plot type drew an empty/wrong plot with rc=0 (BUG-129
    // silent-drop). Route these twoway-specific plot types through the SAME
    // layer machinery the parenthesised form uses (a single synthesised
    // layer): rarea reaches its real band builder, and every unsupported type
    // gets runTwowayLayers' honest reject-before-output. scatter/line/
    // connected/kdensity/histogram keep their dedicated single-plot handlers
    // here (established JSON shapes + selftests unchanged).
    static const std::set<std::string> kBareTwowayLayerRoute = {
        "rarea", "rcap", "rcapsym", "area", "spike", "dropline",
        "lfit",  "qfit", "lfitci",  "qfitci",  // serset-twoway FIT
        "rspike", "rline", "rconnected", "rscatter",  // serset-twoway RNG
    };
    // BUG-336 (graph arm): this arm had NO unknown-option net at all — nine
    // command tokens accepting any option name and dropping it. The known sets
    // below are MEASURED from the handler bodies in graph/commands.cpp and
    // graph/distribution_commands.cpp (optStr/optBool/
    // optTitleStr reads), not guessed:
    //   every plot: title/xtitle/ytitle/note      (runScatter:271, runGraphLine:849,
    //                                              runXtline:950, runTsline:1054)
    //   histogram : width/bin/freq/frequency/fraction/percent/normal (:387-521)
    //   kdensity  : kernel/bwidth/n/normal                            (:683-794)
    //   xtline    : overlay                                           (:980)
    // `name` is read by the arm itself (namedGraphs capture below), and `legend`
    // is ACCEPTED though nobody reads it: p5.do:167 uses it, so it is corpus-live,
    // and a graph is a visual surface where an unhonoured hint is not a wrong
    // answer — the same accept-visual-hint call already made for `list, noobs`.
    // Options Stata has and PIET does not honour are listed as unimplemented so
    // they say so instead of vanishing.
    // ORDER IS LOAD-BEARING: the unsupported-plot-type and bare-twoway-layer
    // routes below own their own honest rejects, and those are about the COMMAND,
    // not its options. Running this net first made `graph bar y, over(x)` answer
    // "option `over` not allowed" instead of "graph bar is not implemented" —
    // caught by four existing selftest legs (mission 2 / BUG-105(1)), which is
    // exactly what they are for.
    // AER scope-2 W2: the four literal sets below moved to file scope, one
    // copy each, and are now REGISTERED per command token (kVocabGraphBase and
    // friends). This arm is unchanged: it still assembles gknown from gc — the
    // resolved PLOT TYPE, which for `twoway`/`graph` is derived from the
    // varlist and is not the dispatched token — and the registration exists so
    // the census can see the vocabulary, not to decide anything here.
    if (kUnsupportedBareGraphTypes.count(gc) == 0 && kBareTwowayLayerRoute.count(gc) == 0 &&
        !twowayKdHist) {
      std::unordered_set<std::string> gknown = kGraphSharedKnown;
      const std::unordered_set<std::string>* gunimpl = &kGraphBaseUnimpl;
      if (gc == "histogram" || gc == "hist") {
        gknown.insert(kGraphHistExtra.begin(), kGraphHistExtra.end());
        gunimpl = &kGraphHistUnimpl;
      } else if (gc == "kdensity") {
        gknown.insert(kGraphKdensExtra.begin(), kGraphKdensExtra.end());
        gunimpl = &kGraphKdensUnimpl;
      } else if (gc == "xtline") {
        gknown.insert(kGraphXtlineExtra.begin(), kGraphXtlineExtra.end());
      }
      if (auto err = eval::rejectUnknownOptions(parsed.opts, gknown, *gunimpl)) {
        // histogram refuses an option it does not know only after it has
        // printed the (bin=..) note (StataBE 19.5, BUG-1306: note, then
        // r(198)). The histogram branch below emits it after runHistogram.
        if (gc != "histogram" && gc != "hist") {
          blocks.push_back(ojson(*err));
          return true;
        }
        histOptErr = ojson(*err);
      }
    }
    // W4 -- THE WIRING GAP, and the reason this is a conditional re-route and
    // not an unconditional one.
    //
    // MEASURED on main today: `scatter y x, xline(0)` was r(198) while
    // `twoway (scatter y x) (line y x), xline(0)` drew the reference line. The
    // renderer for xline/yline/xscale/yscale/xlabel/ylabel/legend/subtitle
    // exists (graph/twoway.cpp's `axes`/`legend` block, and graph.js draws both ONLY
    // for kind=="twoway"); the single-plot arm simply could not reach it. So the
    // fix is routing, not a new feature.
    //
    // Unconditional routing is what the plan REFUSED, and the pins say why: the
    // freeze fixtures scatter/graph/line/tsline/histogram/kdensity all pin BARE
    // commands byte for byte, with `"kind":"scatter"` and the flat
    // points/x_var/y_var shape, and two self-test legs (selftest.cpp:3981,
    // :21887) assert kind=="scatter" for a bare `scatter`. A bare or title-only
    // command therefore stays exactly where it is; only a statement that carries
    // one of the eight layer-honoured globals moves.
    //
    // `vl.size() == 2` is load-bearing and was measured too: `scatter y1 y2 x` is
    // Stata's shared-x multi-series form and runScatter builds a `series` array
    // for it (BS-050, graph/commands.cpp's runScatter), while `twoway::LayerSpec` carries a
    // single yvar/xvar pair. Re-routing a 3+-token varlist would silently drop
    // every series after the first.
    const bool layerReroutable = (gc == "scatter" || gc == "line" || gc == "connected");
    bool wantsTwowayGlobals = false;
    for (const auto& kv : parsed.opts) {
      if (kGraphTwowayGlobals.count(kv.first) != 0) {
        wantsTwowayGlobals = true;
        break;
      }
    }
    const bool rerouteToLayers = layerReroutable && vl.size() == 2 && wantsTwowayGlobals;
    // The keys the branch taken below actually READS -- the input to the
    // accepted-and-not-applied note. Every bare handler reads these five
    // (runScatter:271 / runGraphLine:849 / runXtline:950 / runTsline:1054 for the
    // four titles, this arm's own name() capture below for `name`).
    std::unordered_set<std::string> gApplied = {"title", "xtitle", "ytitle", "note", "name"};
    // serset-twoway SS: the plots this statement's sersets are built from.
    std::vector<serset::PlotSpec> barePlots;
    // serset-twoway WT: the statement's weight type as its full name.
    std::string bareWeightType;
    if (parsed.weightType.has_value() && !parsed.weightType->empty()) {
      const std::string wt = lower(*parsed.weightType);
      if (wt == "fw" || wt == "fweight") bareWeightType = "fweight";
      else if (wt == "aw" || wt == "aweight") bareWeightType = "aweight";
      else if (wt == "pw" || wt == "pweight") bareWeightType = "pweight";
      else if (wt == "iw" || wt == "iweight") bareWeightType = "iweight";
      else bareWeightType = wt;
    }
    // scatter/line/connected/tsline take aw/fw/pw (w2al tf_r_iw `iweights not
    // allowed`, r(101), log:5208-5217).
    if ((gc == "scatter" || gc == "line" || gc == "connected" || gc == "tsline") &&
        !bareWeightType.empty() && bareWeightType != "aweight" && bareWeightType != "fweight" &&
        bareWeightType != "pweight") {
      if (!tmpGraphHbin.empty()) cecon::native_unlink(tmpGraphHbin);
      blocks.push_back(ojson(eval::errorBlock("engine_error", bareWeightType + "s not allowed",
                                              std::nullopt, std::nullopt, std::nullopt, "r(101)")));
      return true;
    }
    // serset-twoway REF: a string variable in a scatter/line plot's varlist is
    // r(109) (w2al tf_r_sc_strx, log:5330-5339); it used to plot factor codes.
    if (gc == "scatter" || gc == "line" || gc == "connected" || gc == "tsline") {
      for (const auto& v : vl) {
        if (dsPtr != nullptr && dsPtr->has(v) && dsPtr->dtype(v) == "factor") {
          if (!tmpGraphHbin.empty()) cecon::native_unlink(tmpGraphHbin);
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error", "string variables not allowed in varlist;\n" + v + " is a string variable",
              std::nullopt, std::nullopt, std::nullopt, "r(109)")));
          return true;
        }
      }
    }
    if (rerouteToLayers || kBareTwowayLayerRoute.count(gc) != 0 || twowayKdHist) {
      twoway::LayerSpec spec;
      spec.plottype = gc == "hist" ? std::string("histogram") : gc;
      spec.varlist = vl;
      spec.opts = parsed.opts;
      spec.weightType = bareWeightType;  // serset-twoway WT
      spec.weightExp = parsed.weightVar.value_or(std::string());
      // `dsPtr` is already the (optionally `if`-filtered) datastore from the
      // graphif temp above, so pass no global-if here (avoids a double filter).
      result = ojson(handlers::runTwowayLayers(dsPtr, session, {spec}, parsed.opts,
                                               std::nullopt, &barePlots));
      gApplied.insert(kGraphTwowayGlobals.begin(), kGraphTwowayGlobals.end());
      gApplied.insert(kGraphLayerStyle.begin(), kGraphLayerStyle.end());
      gApplied.insert(kGraphFitApplied.begin(), kGraphFitApplied.end());
      gApplied.insert("sort");  // BUG-404: the layers order by x only under it
      if (twowayKdHist) {
        gApplied.insert(kGraphHistExtra.begin(), kGraphHistExtra.end());
        gApplied.insert(kGraphKdensExtra.begin(), kGraphKdensExtra.end());
        gApplied.insert({"range", "area"});
      }
    } else if (gc == "histogram" || gc == "hist") {
      // gfx-histogram-kdensity-family: the `(bin=#, start=#, width=#)` note is
      // its own display block, printed before the graph; the graph block
      // stays an object.
      std::vector<ojson> pre;
      std::vector<serset::Serset> histSets;  // serset-twoway HIST-1
      result = ojson(handlers::runHistogram(session, *dsPtr, vl, parsed, &pre, &histSets));
      for (auto& b : pre) blocks.push_back(std::move(b));
      if (histOptErr.has_value() && !(result.is_object() && result.value("type", std::string()) == "error")) {
        if (!tmpGraphHbin.empty()) cecon::native_unlink(tmpGraphHbin);
        blocks.push_back(*histOptErr);
        return true;
      }
      for (auto& hs : histSets) {
        serset::PlotSpec ps;
        ps.computed = std::move(hs);
        barePlots.push_back(std::move(ps));
      }
      gApplied.insert(kGraphHistExtra.begin(), kGraphHistExtra.end());
      for (const char* carried : {"addplot", "addlabopts", "blcolor"}) gApplied.erase(carried);
      gApplied.insert("saving");
    } else if (gc == "line") {
      result = ojson(handlers::runGraphLine(*dsPtr, vl, parsed.opts));
      gApplied.insert("sort");  // BUG-404: data order unless sort
    } else if (gc == "kdensity") {
      // runKdensity posts r(), writes generate() (session.load replaces the
      // head: dsPtr is not read after this call) and, under nograph, draws
      // nothing -- then only its `pre` blocks are emitted.
      std::vector<ojson> pre;
      bool drawn = false;
      std::vector<serset::Serset> kdSets;  // serset-twoway HIST-1
      result = ojson(handlers::runKdensity(session, *dsPtr, vl, parsed, &pre, &drawn, &kdSets));
      for (auto& b : pre) blocks.push_back(std::move(b));
      for (auto& ks : kdSets) {
        serset::PlotSpec ps;
        ps.computed = std::move(ks);
        barePlots.push_back(std::move(ps));
      }
      if (!drawn && !(result.is_object() && result.value("type", std::string()) == "error")) {
        if (!tmpGraphHbin.empty()) cecon::native_unlink(tmpGraphHbin);
        return true;
      }
      gApplied.insert(kGraphKdensExtra.begin(), kGraphKdensExtra.end());
      gApplied.insert("saving");
    } else if (gc == "xtline") {
      result = ojson(handlers::runXtline(*dsPtr, vl, session.panelvar, session.timevar, parsed.opts));
      gApplied.insert(kGraphXtlineExtra.begin(), kGraphXtlineExtra.end());
    } else if (gc == "tsline") {
      result = ojson(handlers::runTsline(*dsPtr, vl, session.timevar, parsed.opts));
    } else if (cmd == "graph" && (gc == "bar" || gc == "hbar" || gc == "box" || gc == "hbox")) {
      // graph-family AI-1/AI-2/AI-3: the bar/box statistics and serset
      // (graph_bar.cpp). Its options are drawing options except the ones the
      // handler reads; they are accepted (the block notes them).
      handlers::BarGraphResult br = handlers::runGraphBarBox(session, *dsPtr, gc, vl, parsed);
      result = br.block;
      for (auto& s : br.sets) {
        serset::PlotSpec ps;
        ps.computed = std::move(s);
        barePlots.push_back(std::move(ps));
      }
      // (`overlay`/`percent` are the canonical spellings parsed.opts gives `over`
      // and `percentages`; the handler re-reads the typed options.)
      for (const char* k : {"over", "overlay", "percent", "asyvars", "ascategory", "cw", "missing",
                            "percentages", "stack",
                            "horizontal", "vertical", "noout", "nooutsides", "nofill", "saving",
                            "nodraw", "scheme", "xsize", "ysize"})
        gApplied.insert(k);
    } else if (kUnsupportedBareGraphTypes.count(gc) != 0) {
      result = ojson(eval::unsupportedCommand("graph " + gc, std::nullopt, std::nullopt, "piet_gap",
                                              eval::UnsupportedCommandKind::kNotImplemented));
    } else {
      result = ojson(handlers::runScatter(*dsPtr, vl, parsed.opts));
    }
    graphAcceptedNotApplied(result, gc, parsed.opts, gApplied);
    const bool bareIsGraph = result.is_object() && result.value("type", std::string()) == "graph";
    // serset-twoway SS: the bare data plots' sersets (piet/serset.h rule (a)/(d)):
    // scatter/line/connected `y1..yk x [mlabel]`, tsline `y1..yk <timevar>`, over
    // the plotted sample (dsPtr is the `if`-filtered frame) in data order.
    if (bareIsGraph && barePlots.empty() && dsPtr != nullptr &&
        (gc == "scatter" || gc == "line" || gc == "connected" || gc == "tsline")) {
      std::vector<std::string> svars = vl;
      if (gc == "tsline" && session.timevar.has_value()) svars.push_back(*session.timevar);
      const size_t roleCount = svars.size();
      // serset-twoway WT: the weight variable follows the plot's variables
      // (w2al tf_wt_line_s1 `y x w`, log:1223).
      if (!bareWeightType.empty() && parsed.weightVar.has_value()) {
        const std::string wv = trimWs(*parsed.weightVar);
        if (dsPtr->has(wv) && std::find(svars.begin(), svars.end(), wv) == svars.end()) svars.push_back(wv);
      }
      if (gc != "tsline") {
        std::optional<std::string> ml;
        for (const auto& [k, v] : parsed.opts) {
          if (k == "mlabel" && !v.isBool) ml = trimWs(v.strValue);
        }
        if (ml.has_value() && !ml->empty() && dsPtr->has(*ml) &&
            std::find(svars.begin(), svars.end(), *ml) == svars.end()) {
          svars.push_back(*ml);
        }
      }
      bool sortOpt = false;
      for (const auto& [k, v] : parsed.opts) {
        if (k == "sort" && (!v.isBool || v.boolValue)) sortOpt = true;
      }
      std::vector<int64_t> rows(static_cast<size_t>(dsPtr->nRows()));
      for (size_t i = 0; i < rows.size(); ++i) rows[i] = static_cast<int64_t>(i);
      serset::PlotSpec ps;
      for (const auto& v : svars) ps.cols.push_back(serset::variableColumn(session, *dsPtr, v, rows));
      ps.sorted = sortOpt && gc != "tsline";
      ps.sortCol = roleCount > 0 ? static_cast<int>(roleCount) - 1 : -1;
      barePlots.push_back(std::move(ps));
    }
    if (!tmpGraphHbin.empty()) cecon::native_unlink(tmpGraphHbin);

    // M4b: cache the last graph so `graph export`/`graph save` can persist
    // it (mirrors dispatcher.py's `if result.get("type")=="graph": session.
    // last_graph = result`).
    // BUG-1529: saving()'s `file ... saved` note follows the graph (StataBE 19.5,
    // a15.do: histogram prints its bin note, then the two file lines).
    ojson bareSavedNote;
    if (bareIsGraph) {
      // BS-010 (U3) + serset-twoway SS: every bare graph registers (an unnamed
      // one as "Graph") together with its sersets — the one registration
      // (piet/serset.h registerGraph).
      const ojson bareMeta = graphMetaFor(
          graphLineTyped, (gc == "bar" || gc == "hbar" || gc == "box" || gc == "hbox") ? "bar" : "twoway",
          parsed.opts);  // graph-family AI-4 (bar/hbar/box are family `bar`: w2ai gf_box_out_d)
      serset::registerGraph(session, result, bareNameOpt,
                            serset::buildPlotSersets(std::move(barePlots)), &bareMeta);
      // gfx-histogram-kdensity-family: histogram's and kdensity's saving()
      // writes the graph file (w2aq_histogram_family hk_h_069by_f,
      // hk_h_031disc_f). The last saving() wins, as Stata's syntax keeps it.
      // serset-twoway GST: every graph of this arm (scatter/line/tsline/twoway
      // forms: w2al tf_line_sav_f, tf_sc_003sav_f), with the graph's sersets.
      {
        std::optional<std::string> savingOpt;
        for (const auto& [k, v] : parsed.opts) {
          if (k == "saving" && !v.isBool) savingOpt = v.strValue;
        }
        if (savingOpt.has_value()) {
          const serset::GraphName gn = serset::parseNameOption(bareNameOpt);
          const std::vector<serset::Serset> owned =
              serset::ownedBy(session, gn.name.empty() ? std::string("Graph") : gn.name);
          ojson se = handlers::saveGraphOption(result, *savingOpt, session, &owned, &bareSavedNote);
          if (!se.is_null()) {
            blocks.push_back(se);
            return true;
          }
        }
      }
    }

    // L2-FIX-B (BUG-066): thin the LIVE-VIEW copy only. session.lastGraph /
    // namedGraphs were deep-copied full above, so `graph export`/`save` keep
    // every point (golden-pinned); this shrinks the console/bridge block.
    handlers::decimateGraphBlockForView(result);
    blocks.push_back(result);
    if (!bareSavedNote.is_null()) blocks.push_back(bareSavedNote);
    return true;
  }
  return false;
}

}
