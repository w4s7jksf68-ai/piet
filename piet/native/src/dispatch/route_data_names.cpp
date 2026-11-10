// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "data_support.h"
#include "prefix_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {
bool route_data_names(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {


  if (cmd == "rename" || cmd == "ren") {
    // BUG-336 (batch 2): known set measured from the handler body -- runRename takes no options; Stata rename has none on the 1:1 form either.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRename.known, kVocabRename.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // ------------------------------------------------------------------
    // B4-W3b: the GROUP form `rename _all, lower` / `rename *, upper`.
    //
    // Routed BEFORE the wildcard reject below, because `*` is exactly the
    // spelling the corpus uses and the reject is about the PATTERN form
    // (`rename v* w*`), which this is not: there is no target pattern here at
    // all, only a case rule applied to every name.
    bool renLower = false, renUpper = false, renProper = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "lower" && (!v.isBool || v.boolValue)) renLower = true;
      if (k == "upper" && (!v.isBool || v.boolValue)) renUpper = true;
      if (k == "proper" && (!v.isBool || v.boolValue)) renProper = true;
    }
    if (renLower || renUpper || renProper) {
      if (static_cast<int>(renLower) + static_cast<int>(renUpper) + static_cast<int>(renProper) > 1) {
        blocks.push_back(ojson(eval::errorBlock(
            "bad_input", "rename: lower, upper and proper are alternatives — specify one.")));
        return true;
      }
      if (!session.hasData()) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return true;
      }
      std::vector<std::string> names;
      if (const auto* card = session.shadowVarlistIfCurrent()) {
        for (const auto& info : *card) names.push_back(info.name);
      } else if (Datastore* rds = session.datastore()) {
        names = rds->names();
      }
      // rename.ado's case form: `rename oldnames, lower|upper|proper`, the
      // old names a varlist -- names, abbreviations, wildcards, optionally in
      // parentheses (aer_w2 w2_data_io.log:832-848 ren1-ren7: `rename ab,
      // upper` renames abc; `rename *Y, proper` renames XY).
      std::vector<std::string> selected;
      {
        std::string spec = rawArgsAfterCmd(parsed.raw);
        bool inQ = false;
        for (size_t i = 0; i < spec.size(); ++i) {
          if (spec[i] == '"') inQ = !inQ;
          if (spec[i] == ',' && !inQ) { spec = spec.substr(0, i); break; }
        }
        for (char& c : spec)
          if (c == '(' || c == ')') c = ' ';
        std::istringstream iss(spec);
        std::string tok;
        auto glob = [](const std::string& pat, const std::string& str) {
          // * any run, ? one character, ~ one character too (Stata varlist).
          std::function<bool(size_t, size_t)> m = [&](size_t i, size_t j) -> bool {
            if (i == pat.size()) return j == str.size();
            if (pat[i] == '*') {
              for (size_t k = j; k <= str.size(); ++k)
                if (m(i + 1, k)) return true;
              return false;
            }
            if (j < str.size() && (pat[i] == '?' || pat[i] == '~' || pat[i] == str[j]))
              return m(i + 1, j + 1);
            return false;
          };
          return m(0, 0);
        };
        while (iss >> tok) {
          std::vector<std::string> hit;
          if (tok == "_all") {
            hit = names;
          } else if (tok.find_first_of("*?~") != std::string::npos) {
            for (const auto& nm : names)
              if (glob(tok, nm)) hit.push_back(nm);
          } else if (std::find(names.begin(), names.end(), tok) != names.end()) {
            hit.push_back(tok);
          } else {
            for (const auto& nm : names)
              if (nm.size() > tok.size() && nm.compare(0, tok.size(), tok) == 0) hit.push_back(nm);
            if (hit.size() > 1) {
              blocks.push_back(ojson(eval::errorBlock("engine_error", tok + " ambiguous abbreviation",
                                                      std::nullopt, std::nullopt, std::nullopt,
                                                      "r(111)")));
              return true;
            }
          }
          if (hit.empty()) {
            blocks.push_back(ojson(eval::errorBlock("engine_error", "variable " + tok + " not found",
                                                    std::nullopt, std::nullopt, std::nullopt,
                                                    "r(111)")));
            return true;
          }
          for (const auto& h : hit)
            if (std::find(selected.begin(), selected.end(), h) == selected.end()) selected.push_back(h);
        }
      }
      auto recase = [&](std::string t) {
        for (size_t i = 0; i < t.size(); ++i) {
          char& c = t[i];
          const bool up = renUpper || (renProper && i == 0);
          c = up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                 : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return t;
      };
      // The collision check runs over the WHOLE mapping before a single rename
      // is issued. Two old names onto one new name is r(198) "new name
      // specified repeatedly" (ren7); a new name that is another variable
      // outside the set is r(110) "variable already exists" (ren3).
      std::map<std::string, std::string> target;  // newname -> the name claiming it
      std::vector<std::pair<std::string, std::string>> moves;
      for (const auto& nm : selected) {
        const std::string nn = recase(nm);
        auto it = target.find(nn);
        if (it != target.end()) {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error", "1 new name specified repeatedly in newname", std::nullopt,
              std::nullopt, std::nullopt, "r(198)")));
          return true;
        }
        target.emplace(nn, nm);
        if (nn != nm) moves.emplace_back(nm, nn);
      }
      for (const auto& [from, to] : moves) {
        if (std::find(names.begin(), names.end(), to) != names.end() &&
            std::find(selected.begin(), selected.end(), to) == selected.end()) {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error", "1 variable already exists: " + to, std::nullopt, std::nullopt,
              std::nullopt, "r(110)")));
          return true;
        }
      }
      if (moves.empty()) {
        ojson note;
        note["type"] = "display";
        note["class"] = "note";
        note["text"] = "(all newnames==oldnames)";
        blocks.push_back(note);
        return true;
      }
      // A rename whose TARGET is another variable's current name would be
      // rejected by runRename's own exists-guard even though the mapping as a
      // whole is a valid permutation (`A`,`b` -> `a`,`B` under upper/lower).
      // Two passes through a reserved prefix keeps every intermediate name
      // unique and never collides with a real one (`__b4ren_` is not a legal
      // Stata name a user could hold).
      std::vector<std::string> renamed;
      bool viaTemp = false;
      for (const auto& [from, to] : moves) {
        if (std::find(names.begin(), names.end(), to) != names.end()) viaTemp = true;
      }
      auto doRename = [&](const std::string& from, const std::string& to) -> bool {
        ojson r = handlers::runRename(from, to, session);
        if (r.is_object() && r.value("type", std::string()) == "error") {
          blocks.push_back(r);
          return false;
        }
        return true;
      };
      if (viaTemp) {
        for (size_t i = 0; i < moves.size(); ++i) {
          if (!doRename(moves[i].first, "__b4ren_" + std::to_string(i))) return true;
        }
        for (size_t i = 0; i < moves.size(); ++i) {
          if (!doRename("__b4ren_" + std::to_string(i), moves[i].second)) return true;
        }
      } else {
        for (const auto& [from, to] : moves) {
          if (!doRename(from, to)) return true;
        }
      }
      for (const auto& [from, to] : moves) renamed.push_back(to);
      ojson note;
      note["type"] = "display";
      note["class"] = "note";
      note["text"] = "(" + std::to_string(renamed.size()) + " variable" +
                     (renamed.size() == 1 ? "" : "s") + " renamed)";
      blocks.push_back(note);
      return true;
    }
    bool hasWildcard = std::any_of(parsed.varlist.begin(), parsed.varlist.end(),
                                   [](const std::string& t) {
                                     return t.find('*') != std::string::npos ||
                                            t.find('?') != std::string::npos;
                                   });
    std::optional<long long> renumberStart;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "renumber" || k == "addnumber") {
        long long st = 1;
        if (!v.isBool) {
          try {
            st = std::stoll(trimWs(v.strValue));
          } catch (...) {
            blocks.push_back(ojson(eval::errorBlock("engine_error", k + "() invalid", std::nullopt,
                                                    std::nullopt, std::nullopt, "r(198)")));
            return true;
          }
        }
        renumberStart = st;
      }
    }
    const bool hasHash = parsed.varlist.size() == 2 &&
                         parsed.varlist[1].find('#') != std::string::npos;
    if (hasWildcard || hasHash) {
      // rename.ado's group form, one old pattern onto one new pattern: `*`/`?`
      // in the old names, `*` (the matched text) or `#` in the new; with
      // renumber[(#)]/addnumber[(#)] the `#` numbers the variables in dataset
      // order from # (default 1) (aer_w2 w2b_dataio rename_renumber `rename v*
      // w#, renumber(10)` -> w10 w11 w12; rename_addnumber `rename w* z#,
      // addnumber` -> z1 z2 z3; w2b_dataio.log:735-752).
      if (parsed.varlist.size() != 2) {
        blocks.push_back(ojson(eval::engineError(
            "rename: this build serves one old pattern onto one new pattern "
            "(`rename v* w*`, `rename v* w#, renumber`)")));
        return true;
      }
      if (!session.hasData()) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return true;
      }
      std::vector<std::string> names;
      if (const auto* card = session.shadowVarlistIfCurrent()) {
        for (const auto& info : *card) names.push_back(info.name);
      } else if (Datastore* rds = session.datastore()) {
        names = rds->names();
      }
      const std::string oldPat = parsed.varlist[0], newPat = parsed.varlist[1];
      // Match `oldPat` against `name`; `cap` receives the text the first
      // wildcard covered (`*` any run, `?` one character, `#` a digit run).
      std::function<bool(size_t, size_t, std::string*)> match;
      const std::string* cur = nullptr;
      match = [&](size_t i, size_t j, std::string* cap) -> bool {
        const std::string& str = *cur;
        if (i == oldPat.size()) return j == str.size();
        const char pc = oldPat[i];
        if (pc == '*' || pc == '#') {
          for (size_t k = j; k <= str.size(); ++k) {
            if (pc == '#' && k > j && !std::isdigit(static_cast<unsigned char>(str[k - 1]))) break;
            std::string sub;
            if (match(i + 1, k, &sub)) {
              if (cap != nullptr && cap->empty()) *cap = str.substr(j, k - j);
              return true;
            }
          }
          return false;
        }
        if (j < str.size() && (pc == '?' || pc == str[j])) return match(i + 1, j + 1, cap);
        return false;
      };
      std::vector<std::pair<std::string, std::string>> moves;
      long long counter = renumberStart.value_or(1);
      for (const auto& nm : names) {
        cur = &nm;
        std::string cap;
        if (!match(0, 0, &cap)) continue;
        std::string nn;
        for (char c : newPat) {
          if (c == '*') nn += cap;
          else if (c == '#') nn += renumberStart.has_value() ? std::to_string(counter) : cap;
          else nn += c;
        }
        ++counter;
        moves.emplace_back(nm, nn);
      }
      if (moves.empty()) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "variable " + oldPat + " not found",
                                                std::nullopt, std::nullopt, std::nullopt, "r(111)")));
        return true;
      }
      std::set<std::string> targets;
      for (const auto& [from, to] : moves) {
        if (!targets.insert(to).second) {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error", "1 new name specified repeatedly in newname", std::nullopt,
              std::nullopt, std::nullopt, "r(198)")));
          return true;
        }
        const bool movedAway = std::any_of(moves.begin(), moves.end(),
                                           [&](const auto& m) { return m.first == to; });
        if (std::find(names.begin(), names.end(), to) != names.end() && !movedAway) {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error", "1 variable already exists: " + to, std::nullopt, std::nullopt,
              std::nullopt, "r(110)")));
          return true;
        }
      }
      for (size_t i = 0; i < moves.size(); ++i) {
        if (moves[i].first == moves[i].second) continue;
        ojson r = handlers::runRename(moves[i].first, "__pren_" + std::to_string(i), session);
        if (r.is_object() && r.value("type", std::string()) == "error") {
          blocks.push_back(r);
          return true;
        }
      }
      for (size_t i = 0; i < moves.size(); ++i) {
        if (moves[i].first == moves[i].second) continue;
        ojson r = handlers::runRename("__pren_" + std::to_string(i), moves[i].second, session);
        if (r.is_object() && r.value("type", std::string()) == "error") {
          blocks.push_back(r);
          return true;
        }
      }
      return true;
    }
    if (parsed.varlist.size() != 2) {
      blocks.push_back(
          ojson(eval::engineError("rename requires exactly two names: `rename old new`")));
      return true;
    }
    // `replace` on the 1:1 form is accepted (aer_w2 w2b_dataio
    // rename_replace_opt `rename x n2012, replace` rc 0, w2b_dataio.log:720).
    // The OLD name may be an abbreviation ([U] 11.2), resolved by the rule
    // keep/drop and the by keys use (StataBE rn.do: `rename number_people_clicked
    // ppl` renames number_people_clicked_w3; 007's appendix tables).
    std::string oldName = parsed.varlist[0];
    if (oldName.find_first_of("*?-") == std::string::npos) {
      std::vector<std::string> one = {oldName};
      if (auto err = handlers::resolveByKeyNames(session, one)) {
        blocks.push_back(*err);
        return true;
      }
      oldName = one.front();
    }
    blocks.push_back(ojson(handlers::runRename(oldName, parsed.varlist[1], session)));
    return true;
  }


  if (cmd == "clonevar") {
    // `clonevar new = old` or `clonevar new old` (both forms accepted —
    // strip a literal '=' then split on whitespace, mirrors commands.py's
    // `" ".join(vl).replace("=", " ").split()`).
    // BUG-127 class: an optional storage-type prefix (`clonevar double x = y`)
    // used to make `double` the NEWVAR and silently discard `x` — strip it
    // through the same stripStorageType gen/egen already use.
    StorageTypeInfo cvInfo = stripStorageType(joinVarlist(parsed.varlist));
    // BUG-1120 review: a storage type where NEWVAR stands names nothing, as in
    // generate: `clonevar double = x` and `clonevar str5 = s` are "too few
    // variables specified" r(102) (StataBE 19.5).
    if (!cvInfo.typeToken.empty() && trimWs(cvInfo.expr).rfind('=', 0) == 0) {
      if (auto r = strutil::generateNameRefusal(
              strippedTypeWord(joinVarlist(parsed.varlist), cvInfo))) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", r->message, std::nullopt,
                                                std::nullopt, std::nullopt,
                                                "r(" + std::to_string(r->rc) + ")")));
        return true;
      }
    }
    if (cvInfo.stringTyped) {
      blocks.push_back(ojson(eval::engineError(
          "clonevar: string storage types are not supported — PIET stores f64/factor")));
      return true;
    }
    std::string joined = cvInfo.expr;
    for (auto& c : joined) {
      if (c == '=') c = ' ';
    }
    std::istringstream iss(joined);
    std::vector<std::string> toks;
    std::string tok;
    while (iss >> tok) toks.push_back(tok);
    if (toks.empty()) {
      if (auto err = conditionAsNameError(parsed.ifExpr)) {
        blocks.push_back(*err);
        return true;
      }
    }
    if (toks.size() < 2) {
      blocks.push_back(
          ojson(eval::engineError("clonevar: syntax is `clonevar NEWVAR = OLDVAR`")));
      return true;
    }
    // BS-031(c): parsed.ifExpr was already correctly extracted by the
    // tokenizer (`clonevar inc2 = income if year==1982`) but never forwarded
    // to the handler at all — see runClonevar's own comment for the fix.
    // BUG-1096: `in` rides along too (it was refused by name).
    blocks.push_back(ojson(handlers::runClonevar(toks.front(), toks.back(), session, parsed.ifExpr,
                                                 parsed.inRange)));
    appendGenMissingNote(blocks, toks.front());  // clonevar.ado's own `gen` prints it
    return true;
  }


  // `format varlist %fmt` and `format %fmt varlist` ([D] format: "are the same
  // commands"; 069 writes `format %td c_dob`, 075 `format %12.0fc N`). The
  // tokenizer has no special handling for a `%`-prefixed token and the plain-
  // varlist expansion passes it through, so the %fmt is the LAST token or the
  // FIRST one. No %fmt at all is `format [varlist]`, which lists the formats.
  if (cmd == "format") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    std::vector<std::string> fmtVarlist;
    std::string fmt;
    const std::vector<std::string>& toks = parsed.varlist;
    size_t nPct = 0;
    for (const auto& t : toks)
      if (!t.empty() && t[0] == '%') ++nPct;
    if (nPct == 0) {
      fmtVarlist = toks;
    } else if (nPct == 1 && toks.back()[0] == '%' && toks.size() > 1) {
      fmt = toks.back();
      fmtVarlist.assign(toks.begin(), toks.end() - 1);
    } else if (nPct == 1 && toks.front()[0] == '%' && toks.size() > 1) {
      fmt = toks.front();
      fmtVarlist.assign(toks.begin() + 1, toks.end());
    } else {
      blocks.push_back(ojson(eval::errorBlock(
          "syntax_error", "format: syntax is `format varlist %fmt` or `format %fmt varlist`",
          std::nullopt, std::nullopt, std::nullopt, "r(198)")));
      return true;
    }
    // runFormat returns an empty array as the silent-success sentinel (Stata's
    // `format` prints nothing); pushing it as a block would make downstream
    // block processing call .value("type") on an array, which throws. Guard
    // it exactly like the other silent handlers (matrix/label/log arms).
    ojson fr = handlers::runFormat(fmtVarlist, fmt, session);
    if (!(fr.is_array() && fr.empty())) blocks.push_back(fr);
    return true;
  }


  if (cmd == "label" && !parsed.varlist.empty()) {
    // BUG-524: canonicalise the subcommand ONCE, here, against the floor
    // table (resolveLabelSub above); every branch below compares the full
    // spelling. Below a floor is Stata's "invalid syntax" r(198)
    // (label_d_too_short, label_va_too_short) and the data are untouched; a
    // token that is not a prefix of any documented subcommand keeps the
    // unknown-subcommand refusal (BUG-105 arm 3 pins that wording; Stata's own
    // rc for it is unmeasured, so it stays what it was).
    //
    // This resolution now runs BEFORE the generic option-vocabulary reject
    // below (BUG-527): `label language NAME, new` needs the resolved
    // subcommand to intercept `, new` on its own terms (oracle rc 0), and
    // that decision has to be made before the generic net answers rc 198
    // for it.
    const std::string subTyped = lower(parsed.varlist[0]);
    std::string sub;
    switch (resolveLabelSub(subTyped, &sub)) {
      case LabelSubMatch::kResolved:
        break;
      case LabelSubMatch::kTooShort:
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax",
                                                "label " + subTyped, std::nullopt, std::nullopt,
                                                "r(198)")));
        return true;
      case LabelSubMatch::kUnknown:
        blocks.push_back(ojson(eval::unsupportedCommand(
            "label " + subTyped, std::nullopt, std::nullopt, "piet_gap",
            eval::UnsupportedCommandKind::kUnknownSubcommand)));
        return true;
    }
    // BUG-527: `label language` takes its own options (new, copy, delete,
    // rename); kVocabLabel still lists new/rename/delete as unimplemented for
    // every OTHER subcommand.
    // BUG-336 (batch 2): known set measured from the handler body. `label define`
    // reads its own options by word from the raw tail (add/modify/replace/nofix,
    // each abbreviable: `, a` is add, lb_add_a), so it is not netted here; `label
    // values` takes nofix (lb_val_nofix); other label subcommands take none.
    if (sub != "define" && sub != "language") {
      if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabLabel.known,
                                                kVocabLabel.unimplemented)) {
        blocks.push_back(ojson(*err));
        return true;
      }
    }
    if (sub == "variable") {
      if (parsed.varlist.size() < 2) {
        blocks.push_back(ojson(eval::engineError("label variable: missing varname")));
        return true;
      }
      std::string varname = parsed.varlist[1];
      // Text is everything after the varname token in the raw line, quotes stripped.
      // BUG-105/WS-K: typed spelling, not the literal — see the `save` arm's
      // comment. `labe variable x "t"` threw std::out_of_range here.
      // BUG-251/WS-Q: this one searched for the command word only to slice
      // FROM it — i.e. to strip leading whitespace that eval::tokenize() has
      // already stripped. `raw.substr(npos)` throws unconditionally, with no
      // wrap-around to hide it. trimWs() is the whole of what it wanted.
      std::string rawArgs = trimWs(parsed.raw);
      // strip "label variable VARNAME" prefix (3 tokens) — split on whitespace,
      // keep the 4th+ as text (mirrors commands.py's maxsplit=3).
      size_t p = 0;
      for (int k = 0; k < 3 && p != std::string::npos; ++k) {
        p = rawArgs.find_first_of(" \t", p);
        if (p != std::string::npos) p = rawArgs.find_first_not_of(" \t", p);
      }
      std::string text = (p == std::string::npos) ? "" : trimWs(rawArgs.substr(p));
      // EM9: an outer compound pair `"..."' is stripped too (`label var x `"the "x"
      // var"'`, w2af_macro_text_file.log:1560; w2ao lb_var_lab_cq log:124).
      if (eval::startsCompound(text, 0) && eval::compoundSpanEnd(text, 0) == text.size()) {
        text = text.substr(2, text.size() - 4);
      } else if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') ||
                                      (text.front() == '\'' && text.back() == '\''))) {
        text = text.substr(1, text.size() - 2);
      }
      // The varname abbreviates (lb_var_abbrev: `label variable miss_age` labels
      // miss_age_PAP); a name that is not a variable is r(111) (lb_var_absent);
      // the label keeps its first 80 characters (lb_var_long, log:190).
      if (session.hasData()) {
        std::vector<std::string> one{varname};
        if (auto err = handlers::resolveByKeyNames(session, one)) {
          blocks.push_back(*err);
          return true;
        }
        Datastore* lds = session.datastore();
        if (one.size() != 1 || lds == nullptr || !lds->has(one.front())) {
          blocks.push_back(ojson(eval::errorBlock("engine_error", "variable " + varname +
                                                                      " not found",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  "r(111)")));
          return true;
        }
        varname = one.front();
      }
      text = variableLabelText(text);
      ojson lv = handlers::runLabelVariable(varname, text, session);
      if (!(lv.is_array() && lv.empty())) blocks.push_back(lv);
      return true;
    }
    if (sub == "values") {
      if (parsed.varlist.size() < 2) {
        blocks.push_back(ojson(eval::engineError("label values: requires varname [lblname]")));
        return true;
      }
      // LBL-SEMANTICS: `label values VARLIST [LBLNAME|.]` -- the LAST token is the
      // set (`.` or none detaches) and every token before it a variable
      // (w2ao_label_family lb_val_multi `label values timecat fl db to`: all three
      // carry `to`); an absent variable is r(111) (:782 lb_val_absent) and a
      // string one r(181) "may not label strings" (:768 lb_val_str), each before
      // anything is attached.
      std::vector<std::string> vars(parsed.varlist.begin() + 1, parsed.varlist.end());
      std::optional<std::string> lblname;
      if (vars.size() >= 2) {
        lblname = vars.back();
        vars.pop_back();
        if (*lblname == ".") lblname.reset();
      }
      bool nofix = false;
      for (const auto& [k, v] : parsed.opts)
        if (k == "nofix") nofix = true;
      if (session.hasData()) {
        loops::VarlistExpandResult ex = loops::expandVarlist(joinSpace(vars), session);
        if (ex.isError) {
          blocks.push_back(ex.error);
          return true;
        }
        vars = ex.tokens;
        if (auto err = handlers::resolveByKeyNames(session, vars)) {
          blocks.push_back(*err);
          return true;
        }
        Datastore* lds = session.datastore();
        for (const auto& v : vars) {
          if (lds == nullptr || !lds->has(v)) {
            blocks.push_back(ojson(eval::errorBlock("engine_error", "variable " + v + " not found",
                                                    std::nullopt, std::nullopt, std::nullopt,
                                                    "r(111)")));
            return true;
          }
        }
        if (lblname.has_value()) {
          for (const auto& v : vars) {
            if (lds->dtype(v) == "factor") {
              blocks.push_back(ojson(eval::errorBlock("engine_error", "may not label strings",
                                                      std::nullopt, std::nullopt, std::nullopt,
                                                      "r(181)")));
              return true;
            }
          }
        }
      }
      for (const auto& v : vars) {
        ojson lv = handlers::runLabelValues(v, lblname, session, nofix);
        if (!(lv.is_array() && lv.empty())) {
          blocks.push_back(lv);
          return true;
        }
      }
      return true;
    }
    if (sub == "define") {
      // BUG-105/WS-K: runLabelDefine anchors on `^label\s+define\s+`.
      // BUG-524: ... so the typed subcommand (`def`, `de`) is canonicalised in
      // the raw line too, or `label def yn2 0 "no"` would fail its own anchor.
      ojson lv = handlers::runLabelDefine(
          rawWithCanonicalLabelSub(rawWithCanonicalCmd(parsed.raw, cmdTyped, cmd), subTyped, sub),
          session);
      if (!(lv.is_array() && lv.empty())) blocks.push_back(lv);
      return true;
    }
    if (sub == "list") {
      std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
      ojson ll = ojson(handlers::runLabelList(names, session));
      if (ll.is_array()) {
        for (auto& b : ll) blocks.push_back(b);
      } else {
        blocks.push_back(std::move(ll));
      }
      return true;
    }
    if (sub == "data") {
      // Stata's documented behaviour: label data sets a dataset label of up to
      // 80 characters, and with no label given it removes the existing one.
      // session.dataLabel is the field the .dta writer already
      // carries (handlers_data_verbs_internal.h); this is its first in-session
      // mutator, so `label data` no longer stops a do-file (14 corpus uses in
      // 5 papers). Not an oracle arm -- the definition is the help file's.
      // Text is everything after the subcommand token, quotes stripped (the
      // `label variable` branch's own slicing, one token earlier).
      std::string rawArgs = trimWs(parsed.raw);
      size_t p = 0;
      for (int k = 0; k < 2 && p != std::string::npos; ++k) {
        p = rawArgs.find_first_of(" \t", p);
        if (p != std::string::npos) p = rawArgs.find_first_not_of(" \t", p);
      }
      std::string text = (p == std::string::npos) ? "" : trimWs(rawArgs.substr(p));
      if (eval::startsCompound(text, 0) && eval::compoundSpanEnd(text, 0) == text.size()) {
        text = text.substr(2, text.size() - 4);  // EM9, as `label variable`
      } else if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
      }
      session.dataLabel = text;
      return true;  // silent success, as `label variable` is
    }
    if (sub == "language") {
      // LBL-LANGUAGE: the whole subcommand (bare r(), new [copy], switch, delete,
      // rename) is handlers::runLabelLanguage; its options are its own.
      std::vector<std::string> args(parsed.varlist.begin() + 1, parsed.varlist.end());
      ojson lv = handlers::runLabelLanguage(args, parsed.opts, session);
      if (!(lv.is_array() && lv.empty())) blocks.push_back(lv);
      return true;
    }
    if (sub == "dir") {
      blocks.push_back(ojson(handlers::runLabelDir(session)));
      return true;
    }
    if (sub == "drop") {
      std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
      if (names.empty()) {
        blocks.push_back(ojson(eval::engineError("label drop: specify at least one label name")));
        return true;
      }
      ojson lv = handlers::runLabelDrop(names, session);
      if (!(lv.is_array() && lv.empty())) blocks.push_back(lv);
      return true;
    }
    // BUG-524: what is left resolved to a documented subcommand PIET has no
    // store for (copy, rename, save). A recognised feature, refused by NAME --
    // never accepted-and-ignored, which for `label save` would look like a
    // file was written.
    blocks.push_back(ojson(eval::unsupportedCommand(
        "label " + sub, std::nullopt, std::nullopt, "piet_gap",
        eval::UnsupportedCommandKind::kNotImplemented)));
    return true;
  }


  // P1-W2-0: `labelbook [LBLNAME...]` — a top-level command (not a `label`
  // subcommand), so parsed.varlist holds the optional label-set names
  // directly (no subcommand-token stripping, unlike the `label list` arm
  // above).
  if (cmd == "labelbook") {
    // BUG-336 (batch 2): known set measured from the handler body -- runLabelbook takes no options.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabLabelbook.known, kVocabLabelbook.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // BUG-373: Stata publishes the label-set names it walked as r(names).
    ojson lbBlk = ojson(handlers::runLabelbook(parsed.varlist, session));
    if (lbBlk.value("type", std::string()) != "error") {
      std::string names;
      for (const auto& kv : session.labelDefs) {
        if (!names.empty()) names += " ";
        names += kv.first;
      }
      session.eval.rStrings.set("names", names);
    }
    blocks.push_back(std::move(lbBlk));
    return true;
  }
  return false;
}

}
