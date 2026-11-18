// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// findname -- list the variables whose labels match a pattern (variable-metadata family;
// shared helpers in datamgmt/variable_metadata_support.h).
//
//   findname  variable labels through the `: variable label` projection
//             (SessionState::syncEvalVarLabels); the glob is strutil::globMatch.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "data/missing.h"  // cecon::stata_cmp / missing_kind -- Stata's `!=`, extended missings
#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"  // handleDisplay (display's default number text), resolveVarName
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // applyStataFmt, fnAbbrev
#include "piet/eval/macros.h"   // captureRFromBlock
#include "piet/eval/pyvalue.h"  // pythonFloatRepr / PyValue
#include "piet/eval/session.h"  // replaceRClass / declareRPosted
#include "piet/eval/tokenizer.h"  // scanOneToken / quotedSpanMask
#include "piet/handlers/variables.h"
#include "piet/handlers/descriptive.h"
#include "piet/dispatcher.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // expandVarlist -- the ONE varlist wildcard/range expander
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "vendor/nlohmann/json.hpp"

#include "datamgmt/variable_metadata_support.h"

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch (defect class 3).
// `known` is what the handler READS; `unimplemented` is a real option of the same
// command this build does not do, refused by name.
namespace {

// findname.ado 1.1.2's syntax line. The selection options this build reads are the
// variable-label pair plus the presentation/result ones; every other selector would
// pick a DIFFERENT set of variables, so each is refused rather than ignored.
const piet::eval::OptVocab& kVocabFindname = piet::eval::registerOptVocab(
    {"findname"},
    {{"varlabeltext", "varlabel", "insensitive", "not", "alpha", "local", "cslocal", "indent",
      "skip", "varwidth", "type", "all", "any", "format", "columns", "vallabel", "vallabelname",
      "vallabeltext", "vallabeltextdef", "vallabeltextuse", "vallabelcountdef",
      "vallabelcountuse", "char", "charname", "chartext", "placeholder", "detail"},
     {}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace variable_metadata_detail;

namespace {

// The variable label `: variable label v` answers -- the one projection describe shares.
std::string varLabelOf(SessionState& session, const std::string& v) {
  const std::string* l = session.eval.varLabels.find(v);
  return l == nullptr ? std::string() : *l;
}

// ===========================================================================
// findname
// ===========================================================================
//
// Implements findname's variable-label selection per its help file (Nicholas J. Cox,
// findname 1.1.2); identified against StataBE probes (w2ax_t1_community.log findname_*,
// mx2.log FN|*):
//   * each varlabeltext() word is a pattern matched against the WHOLE label, `*` any run
//     and `?` one character, case-sensitive unless `insensitive` (findname_case: `P*`
//     finds only "Price 1"); the words are Stata's tokens, so `varlabeltext(price two)`
//     is two patterns and `varlabeltext("")` matches the empty label (FN|unq, FN|emptypat);
//   * a leading varlist restricts the search (findname_varlist), bare findname lists every
//     variable (findname_bare), `not` takes the complement over every variable, `alpha`
//     sorts;
//   * nothing found is rc 0, nothing printed and r() cleared (findname_none); otherwise the
//     names print in columns -- each abbreviated to varwidth() (default 12), column width
//     = longest + skip() (default 2), as many columns as the line holds, filled down first
//     (findname_glob2 `p1  q`, FN|forty, FN|insen `alpha_long~e  b             ...`) --
//     and r(varlist) is the full names. local()/cslocal() copy it to a local;
//   * if/in pick observations, which only the value-based selectors (not implemented here)
//     read -- an empty sample is r(2000) "no observations" all the same; indent() below 0
//     is 0 (review p1 fn_if0, fn_in, fn_indneg).
//

std::string displayInColumns(const std::vector<std::string>& items, int indent, int pad) {
  if (items.empty()) return std::string();
  size_t wid = 0;
  for (const auto& x : items) wid = std::max(wid, x.size());
  wid += static_cast<size_t>(pad);
  const long long cols = (kLineSize + 1 - indent) / static_cast<long long>(wid);
  std::vector<std::string> lines;
  if (cols < 2) {
    for (const auto& x : items) lines.push_back(std::string(static_cast<size_t>(indent), ' ') + x);
    return joinWords(lines, "\n");
  }
  const long long n = static_cast<long long>(items.size());
  const long long nLines = (n + cols - 1) / cols;
  for (long long i = 1; i <= nLines; ++i) {
    const long long top = std::min(cols * nLines + i, n);
    std::string line;
    size_t col = static_cast<size_t>(indent);
    for (long long j = i; j <= top; j += nLines) {
      line = padTo(line, col);
      line += items[static_cast<size_t>(j - 1)];
      col += wid;
    }
    lines.push_back(line);
  }
  return joinWords(lines, "\n");
}

}  // namespace

ojson runFindname(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabFindname.known,
                                            kVocabFindname.unimplemented))
    return ojson(*err);
  // findname is r-class: whatever it finds, r() is replaced (findname_none leaves an
  // empty r(varlist) behind a previous non-empty one). Nothing found posts nothing.
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "findname", {});

  ojson perr;
  auto varwidth = intOption(parsed, "varwidth", 12, &perr);
  if (!varwidth) return perr;
  auto skip = intOption(parsed, "skip", 2, &perr);
  if (!skip) return perr;
  auto indent = intOption(parsed, "indent", 0, &perr);
  if (!indent) return perr;
  if (*varwidth < 5 || *varwidth > 32) return rcError("varwidth() should be in the range 5..32", 198);
  if (*skip < 1 || *skip > 10) return rcError("skip() should be in the range 1..10", 198);
  if (*indent < 0) *indent = 0;  // DisplayInCols' own rule (review p1 fn_indneg)
  if (*indent > kLineSize) *indent = kLineSize;

  // if/in select observations, which only the value-based selectors read; with none of
  // those an empty sample is still r(2000) (review p1 fn_if0, fn_in).
  const bool qualified = (parsed.ifExpr.has_value() && !strutil::trim(*parsed.ifExpr).empty()) ||
                         (parsed.inRange.has_value() && !strutil::trim(*parsed.inRange).empty());
  if (qualified) {
    ColumnScope scope(session, {}, parsed.ifExpr, parsed.inRange, "findname");
    if (scope.failed()) return scope.error();
    if (scope.get()->nRows() == 0) return rcError("no observations", 2000);
  }

  const std::vector<std::string> all = allVarNames(session);
  std::vector<std::string> vars;
  if (!parsed.varlist.empty()) {
    if (auto e = unabVarlist(strutil::trim(parsed.argText), session, &vars)) return *e;
  } else {
    vars = all;  // `syntax [varlist]`: no data means no variables, and findname exits
  }

  const bool insensitive = hasFlag(parsed, "insensitive");
  if (hasFlag(parsed, "varlabel") || optStr(parsed, "varlabel").has_value() ||
      optStr(parsed, "varlabeltext").has_value())
    session.syncEvalVarLabels();
  if (hasFlag(parsed, "varlabel")) {
    std::vector<std::string> keep;
    for (const auto& v : vars)
      if (!varLabelOf(session, v).empty()) keep.push_back(v);
    vars = keep;
  }
  // varlabel(patterns) selects by label text, as varlabeltext() does.
  std::optional<std::string> labelPatternText = optStr(parsed, "varlabeltext");
  if (!labelPatternText.has_value())
    for (const auto& [k, v] : parsed.opts)
      if (k == "varlabel" && !v.isBool) labelPatternText = v.strValue;  // the bare flag has no patterns
  if (auto text = labelPatternText) {
    std::vector<std::string> patterns = stataWords(*text);
    // `varlabeltext("")`: one empty pattern, which matches an empty label (FN|emptypat).
    if (patterns.empty() && !strutil::trim(*text).empty()) patterns.emplace_back();
    if (insensitive)
      for (auto& p : patterns) p = strutil::lower(p);
    std::vector<std::string> keep;
    for (const auto& v : vars) {
      std::string lbl = varLabelOf(session, v);
      if (insensitive) lbl = strutil::lower(lbl);
      for (const auto& p : patterns) {
        if (strutil::globMatch(p, lbl)) {
          keep.push_back(v);
          break;
        }
      }
    }
    vars = keep;
  }
  // ---- the selectors that read the variables' own metadata and values ----
  // Each keeps the variables that meet it; all of them together are an intersection (findname's
  // "and"). vs-stata bank: type(), format(), vallabel*, char*, columns(), all(), any(), detail.
  std::map<std::string, ojson> meta;
  auto metaOf = [&](const std::string& v) -> const ojson& {
    static const ojson kEmpty = ojson::object();
    auto it = meta.find(v);
    return it == meta.end() ? kEmpty : it->second;
  };
  const bool needMeta = optStr(parsed, "type").has_value() || optStr(parsed, "format").has_value() ||
                        hasFlag(parsed, "vallabel") || optStr(parsed, "vallabel").has_value() ||
                        optStr(parsed, "vallabelname").has_value() ||
                        optStr(parsed, "vallabeltext").has_value() ||
                        optStr(parsed, "vallabeltextdef").has_value() ||
                        optStr(parsed, "vallabeltextuse").has_value() ||
                        optStr(parsed, "vallabelcountdef").has_value() ||
                        optStr(parsed, "vallabelcountuse").has_value() || hasFlag(parsed, "detail");
  if (needMeta && !vars.empty()) {
    ojson d = runDescribe(vars, session);
    if (d.is_object() && d.contains("variables"))
      for (const auto& e : d["variables"]) meta[e.value("name", std::string())] = e;
  }
  // A selector's patternlist, absent for the bare form (`vallabel`, `char`).
  auto valuedOpt = [&](const char* key) -> std::optional<std::string> {
    for (const auto& [k, v] : parsed.opts)
      if (k == key && !v.isBool) return v.strValue;
    return std::nullopt;
  };
  auto foldCase = [&](std::string t) { return insensitive ? strutil::lower(t) : t; };
  auto anyPatternMatches = [&](const std::vector<std::string>& patterns, const std::string& text) {
    for (const auto& p : patterns)
      if (strutil::globMatch(foldCase(p), foldCase(text))) return true;
    return false;
  };
  auto narrow = [&](const std::function<bool(const std::string&)>& keepIt) {
    std::vector<std::string> keep;
    for (const auto& v : vars)
      if (keepIt(v)) keep.push_back(v);
    vars = std::move(keep);
  };
  auto numlistOf = [&](const std::string& text, std::vector<double>* out, ojson* err) -> bool {
    std::vector<std::string> toks;
    if (!piet::loops::expandNumlist(text, &toks)) {
      *err = rcError("invalid numlist", 121);
      return false;
    }
    for (const auto& t : toks) out->push_back(std::strtod(t.c_str(), nullptr));
    return true;
  };

  if (auto text = optStr(parsed, "type")) {
    // A storage type, str# or a width numlist, string, numeric.
    std::set<std::string> kinds;
    std::set<int> widths;
    {
      static const std::set<std::string> kTypeWords = {"byte", "int",  "long", "float", "double",
                                                       "string", "numeric", "str#", "strL"};
      std::string numRest;
      for (const auto& w : stataWords(*text)) {
        if (kTypeWords.count(w)) kinds.insert(w);
        else numRest += (numRest.empty() ? "" : " ") + w;
      }
      if (!numRest.empty()) {
        ojson nerr;
        std::vector<double> ws;
        if (!numlistOf(numRest, &ws, &nerr)) return rcError("invalid variable type(s)", 198);
        for (double w : ws) {
          if (w != std::floor(w) || w < 1 || w > 2045) return rcError("invalid type() option", 198);
          widths.insert(static_cast<int>(w));
        }
      }
      if (kinds.empty() && widths.empty()) return rcError("invalid type() option", 198);
    }
    Datastore* tds = session.datastore();
    narrow([&](const std::string& v) {
      const bool isString = tds != nullptr && tds->has(v) && tds->dtype(v) == "factor";
      std::string st = metaOf(v).value("dtype", std::string());
      if (isString && (st.empty() || st == "str#")) {
        size_t longest = 1;
        const cecon::Column& c = tds->col(v);
        for (const auto& lv : c.levels) longest = std::max(longest, lv.size());
        st = longest > 2045 ? "strL" : "str" + std::to_string(longest);
      }
      if (kinds.count("string") && isString) return true;
      if (kinds.count("numeric") && !isString) return true;
      if (kinds.count(st)) return true;
      if (kinds.count("str#") && isString && st != "strL") return true;
      if (isString && st.rfind("str", 0) == 0 && st != "strL")
        for (int w : widths)
          if (st == "str" + std::to_string(w)) return true;
      return false;
    });
  }
  if (auto text = optStr(parsed, "format")) {
    const std::vector<std::string> patterns = stataWords(*text);
    narrow([&](const std::string& v) {
      return anyPatternMatches(patterns, metaOf(v).value("format", std::string()));
    });
  }
  if (hasFlag(parsed, "vallabel"))
    narrow([&](const std::string& v) { return !metaOf(v).value("val_label", std::string()).empty(); });
  for (const char* key : {"vallabel", "vallabelname"})
    if (auto text = valuedOpt(key)) {
      const std::vector<std::string> patterns = stataWords(*text);
      narrow([&](const std::string& v) {
        const std::string lab = metaOf(v).value("val_label", std::string());
        return !lab.empty() && anyPatternMatches(patterns, lab);
      });
    }
  // The labelled values a variable uses: its distinct non-missing values that its value
  // label names.
  auto usedLabelled = [&](const std::string& v, std::vector<std::string>* texts) -> int {
    const std::string lab = metaOf(v).value("val_label", std::string());
    auto def = session.labelDefs.find(lab);
    Datastore* uds = session.datastore();
    if (lab.empty() || def == session.labelDefs.end() || uds == nullptr || !uds->has(v) ||
        uds->dtype(v) != "f64")
      return 0;
    std::set<double> seen;
    const double* x = uds->col(v).f64();
    for (int64_t i = 0; i < uds->nRows(); ++i)
      if (!std::isnan(x[i]) && x[i] == std::floor(x[i])) seen.insert(x[i]);
    int n = 0;
    for (double val : seen) {
      auto t = def->second.find(std::to_string(static_cast<long long>(val)));
      if (t == def->second.end()) continue;
      ++n;
      if (texts != nullptr) texts->push_back(t->second);
    }
    return n;
  };
  if (auto text = optStr(parsed, "vallabeltext")) {
    const std::vector<std::string> patterns = stataWords(*text);
    narrow([&](const std::string& v) {
      auto def = session.labelDefs.find(metaOf(v).value("val_label", std::string()));
      if (def == session.labelDefs.end()) return false;
      for (const auto& kv : def->second)
        if (anyPatternMatches(patterns, kv.second)) return true;
      return false;
    });
  }
  if (auto text = optStr(parsed, "vallabeltextdef")) {
    const std::vector<std::string> patterns = stataWords(*text);
    narrow([&](const std::string& v) {
      auto def = session.labelDefs.find(metaOf(v).value("val_label", std::string()));
      if (def == session.labelDefs.end()) return false;
      for (const auto& kv : def->second)
        if (anyPatternMatches(patterns, kv.second)) return true;
      return false;
    });
  }
  if (auto text = optStr(parsed, "vallabeltextuse")) {
    const std::vector<std::string> patterns = stataWords(*text);
    narrow([&](const std::string& v) {
      std::vector<std::string> used;
      usedLabelled(v, &used);
      for (const auto& t : used)
        if (anyPatternMatches(patterns, t)) return true;
      return false;
    });
  }
  if (auto text = optStr(parsed, "vallabelcountdef")) {
    std::vector<double> want;
    ojson nerr;
    if (!numlistOf(*text, &want, &nerr)) return nerr;
    narrow([&](const std::string& v) {
      auto def = session.labelDefs.find(metaOf(v).value("val_label", std::string()));
      if (def == session.labelDefs.end()) return false;
      return std::find(want.begin(), want.end(), static_cast<double>(def->second.size())) != want.end();
    });
  }
  if (auto text = optStr(parsed, "vallabelcountuse")) {
    std::vector<double> want;
    ojson nerr;
    if (!numlistOf(*text, &want, &nerr)) return nerr;
    narrow([&](const std::string& v) {
      if (metaOf(v).value("val_label", std::string()).empty()) return false;
      return std::find(want.begin(), want.end(), static_cast<double>(usedLabelled(v, nullptr))) !=
             want.end();
    });
  }
  // Characteristics: char / charname select by the variable's characteristic NAMES (any one
  // when no pattern is given), chartext by their text.
  if (hasFlag(parsed, "char"))
    narrow([&](const std::string& v) {
      for (const auto& c : session.characteristics)
        if (c.varname == v) return true;
      return false;
    });
  for (const char* key : {"char", "charname"})
    if (auto text = valuedOpt(key)) {
      const std::vector<std::string> patterns = stataWords(*text);
      narrow([&](const std::string& v) {
        for (const auto& c : session.characteristics)
          if (c.varname == v && anyPatternMatches(patterns, c.charname)) return true;
        return false;
      });
    }
  if (auto text = optStr(parsed, "chartext")) {
    const std::vector<std::string> patterns = stataWords(*text);
    narrow([&](const std::string& v) {
      for (const auto& c : session.characteristics)
        if (c.varname == v && anyPatternMatches(patterns, c.value)) return true;
      return false;
    });
  }
  if (auto text = optStr(parsed, "columns")) {
    std::vector<double> want;
    ojson nerr;
    if (!numlistOf(*text, &want, &nerr)) return nerr;
    narrow([&](const std::string& v) {
      const auto at = std::find(all.begin(), all.end(), v);
      if (at == all.end()) return false;
      const double pos = static_cast<double>(at - all.begin()) + 1.0;
      return std::find(want.begin(), want.end(), pos) != want.end();
    });
  }
  // all() / any(): a condition on each variable's values over the sample, the variable standing
  // where the placeholder (`@`, or placeholder()) is. A variable the condition cannot be
  // evaluated for (a string against a number) does not meet it.
  {
    const auto allCond = optStr(parsed, "all");
    const auto anyCond = optStr(parsed, "any");
    if (allCond.has_value() || anyCond.has_value()) {
      std::string ph = strutil::trim(optStr(parsed, "placeholder").value_or(std::string()));
      if (ph.empty()) ph = "@";
      std::string userIf, userIn;
      if (parsed.ifExpr.has_value() && !strutil::trim(*parsed.ifExpr).empty())
        userIf = " & (" + *parsed.ifExpr + ")";
      if (parsed.inRange.has_value() && !strutil::trim(*parsed.inRange).empty())
        userIn = " in " + *parsed.inRange;
      const std::string savedLeaf = session.leafCmdExpanded;
      auto countOf = [&](const std::string& cond) -> std::optional<double> {
        ojson b = dispatch("quietly count if (" + cond + ")" + userIf + userIn, session);
        session.leafCmdExpanded = savedLeaf;
        if (firstError(b) != nullptr) return std::nullopt;
        const double* n = session.eval.rValues.find("N");
        return n != nullptr ? std::optional<double>(*n) : std::nullopt;
      };
      auto subst = [&](const std::string& cond, const std::string& v) {
        std::string out;
        for (size_t i = 0; i < cond.size();) {
          if (cond.compare(i, ph.size(), ph) == 0) {
            out += v;
            i += ph.size();
          } else {
            out += cond[i++];
          }
        }
        return out;
      };
      const std::optional<double> nSample = countOf("1");
      narrow([&](const std::string& v) {
        if (allCond.has_value()) {
          const auto n = countOf(subst(*allCond, v));
          if (!n.has_value() || !nSample.has_value() || *n != *nSample) return false;
        }
        if (anyCond.has_value()) {
          const auto n = countOf(subst(*anyCond, v));
          if (!n.has_value() || *n <= 0) return false;
        }
        return true;
      });
    }
  }
  if (hasFlag(parsed, "not")) {
    std::vector<std::string> comp;
    for (const auto& v : all)
      if (std::find(vars.begin(), vars.end(), v) == vars.end()) comp.push_back(v);
    vars = comp;
  }
  if (vars.empty()) return silent();
  if (hasFlag(parsed, "alpha")) std::sort(vars.begin(), vars.end());

  std::vector<std::string> shown;
  shown.reserve(vars.size());
  for (const auto& v : vars) shown.push_back(stataAbbrev(v, static_cast<int>(*varwidth)));
  ojson out = ojson::array();
  if (hasFlag(parsed, "detail")) {
    // detail: describe's table of the variables found.
    if (meta.empty() || meta.count(vars.front()) == 0) {
      ojson d = runDescribe(vars, session);
      if (d.is_object() && d.contains("variables"))
        for (const auto& e : d["variables"]) meta[e.value("name", std::string())] = e;
    }
    auto padR = [](std::string t, size_t w) {
      if (t.size() < w) t.append(w - t.size(), ' ');
      return t;
    };
    std::string tbl = "Variable      Storage   Display    Value\n"
                      "    name         type    format    label      Variable label\n" +
                      std::string(80, '-');
    for (const auto& v : vars) {
      const ojson& e = metaOf(v);
      std::string row = padR(v, 16) + padR(e.value("dtype", std::string()), 8) +
                        padR(e.value("format", std::string()), 11) +
                        padR(e.value("val_label", std::string()), 11) +
                        e.value("label", std::string());
      while (!row.empty() && row.back() == ' ') row.pop_back();
      tbl += "\n" + row;
    }
    out.push_back(displayBlock(tbl));
  } else {
    out.push_back(displayBlock(displayInColumns(shown, static_cast<int>(*indent),
                                                static_cast<int>(*skip))));
  }

  const std::string joined = joinWords(vars);
  session.eval.rStrings.set("varlist", joined);
  eval::declareRPosted(session.eval, "findname", {"varlist"});
  const std::string loc = strutil::trim(optStr(parsed, "local").value_or(std::string()));
  const std::string csloc = strutil::trim(optStr(parsed, "cslocal").value_or(std::string()));
  if (!loc.empty()) session.eval.locals.set(loc, joined);
  if (!csloc.empty()) {
    if (csloc == loc) out.push_back(displayBlock("\nnote: cslocal() result overwrites local() result"));
    session.eval.locals.set(csloc, joinWords(vars, ","));
  }
  return out;
}

}  // namespace piet::handlers
