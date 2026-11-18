// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/utility.h"
#include "piet/handlers/variables.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>  // std::memcpy -- the ±0-safe key bit-normalization
#include <ctime>
#include <exception>
#include <functional>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/loops.h"  // expandVarlist -- the ONE varlist wildcard/range expander
#include "piet/mutation_result.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

#include "utility_support.h"
#include "utility_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace utility_detail;

// ===========================================================================
// labmask
// ===========================================================================
//
// `labmask numvar, values(textvar)` gives numvar a value label whose text comes
// from textvar. It is a LABEL command, not a data command: the composed
// `label define` goes to runLabelDefine and the attach to runLabelValues, so
// the label plane has exactly one writer.
ojson runLabmask(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabLabmask.known,
                                            kVocabLabmask.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.size() != 1)
    return utilSyntax("labmask: syntax is `labmask numvar, values(varname) [lblname(name)]`");
  // `labmask(yaxis), value(v)`: Stata's varname parser takes one pair of
  // parentheses around the name (w2ao_label_family.log:2570 lb_labmask_paren).
  std::string numvar = parsed.varlist[0];
  if (numvar.size() >= 2 && numvar.front() == '(' && numvar.back() == ')')
    numvar = strutil::trim(numvar.substr(1, numvar.size() - 2));
  const std::optional<std::string> valuesVar = optStr(parsed.opts, "values");
  if (!valuesVar.has_value() || valuesVar->empty())
    return utilSyntax("labmask: values(varname) is required -- it names the variable whose text "
                      "becomes the label.");
  const std::string lblname = optStr(parsed.opts, "lblname").value_or(numvar);
  if (!strutil::isValidStataIdentifier(lblname))
    return utilSyntax("labmask: `" + lblname + "' is not a usable label name -- " +
                      strutil::stataIdentifierRuleText());

  // labmask.ado: `marksample touse` (if/in, numvar non-missing), r(2000) when
  // it selects nothing (lb_labmask_empty `labmask state if state > 9`).
  ReadScope scope(session, parsed, {numvar, *valuesVar}, "labmask", true);
  if (scope.failed())
    return scope.error().is_null() ? utilError("labmask: could not read the data", "data_io")
                                   : scope.error();
  Datastore& ds = *scope.get();
  if (!ds.has(numvar)) return utilError("variable " + numvar + " not found");
  if (!ds.has(*valuesVar)) return utilError("variable " + *valuesVar + " not found");
  if (ds.dtype(numvar) == "factor")
    return utilError("labmask: `" + numvar +
                     "' is a string variable; a value label can only be attached to a numeric "
                     "variable.");

  const int64_t n = ds.nRows();
  const ColView num = viewOf(ds, numvar);
  std::vector<std::string> text = columnAsStrings(ds, *valuesVar);
  if (hasFlag(parsed.opts, "decode")) {
    // decode: the text of each observation is the value label of the values variable at its
    // value (a number with no label there is its own text, as decode gives it).
    if (ds.dtype(*valuesVar) == "factor")
      return utilErrorRc("type mismatch", 109);
    const auto li = session.varLblname.find(*valuesVar);
    if (li == session.varLblname.end() || li->second.empty())
      return utilErrorRc("variable " + *valuesVar + " has no value label attached", 182);
    const auto def = session.labelDefs.find(li->second);
    const ColView vv = viewOf(ds, *valuesVar);
    for (int64_t i = 0; i < n; ++i) {
      if (cellMissing(vv, i)) {
        text[static_cast<size_t>(i)].clear();
        continue;
      }
      const double v = vv.d[i];
      std::string key = v == std::floor(v) ? std::to_string(static_cast<long long>(v)) : numText(v);
      if (def != session.labelDefs.end()) {
        const auto t = def->second.find(key);
        if (t != def->second.end()) key = t->second;
      }
      text[static_cast<size_t>(i)] = key;
    }
  }

  // Two things must hold before a single label is defined: every code is an
  // integer, and every observation with the same code carries the same text.
  // Both are checked over the WHOLE column first -- a half-written label set
  // would be worse than no label at all.
  std::map<long long, std::string> mapping;
  int64_t used = 0;
  for (int64_t i = 0; i < n; ++i)
    if (!cellMissing(num, i)) ++used;
  if (used == 0)
    return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt, std::nullopt,
                                  std::nullopt, "r(2000)"));
  // labmask.ado's two refusals, r(198) with its own texts (lb_labmask_nonint,
  // lb_labmask_notconst): checked over every used row before anything is defined.
  for (int64_t i = 0; i < n; ++i) {
    if (cellMissing(num, i)) continue;
    if (num.d[i] != std::floor(num.d[i]))
      return ojson(eval::errorBlock("engine_error", "may not label non-integers", std::nullopt,
                                    std::nullopt, std::nullopt, "r(198)"));
  }
  {
    std::map<long long, std::string> seen;
    for (int64_t i = 0; i < n; ++i) {
      if (cellMissing(num, i)) continue;
      const long long code = static_cast<long long>(num.d[i]);
      const std::string& t = text[static_cast<size_t>(i)];
      auto it = seen.find(code);
      if (it == seen.end()) seen.emplace(code, t);
      else if (it->second != t)
        return ojson(eval::errorBlock("engine_error",
                                      *valuesVar + " not constant within groups of " + numvar,
                                      std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    }
  }
  for (int64_t i = 0; i < n; ++i) {
    if (cellMissing(num, i)) continue;
    const double v = num.d[i];
    if (v != std::floor(v))
      return utilError("labmask: `" + numvar + "' has the non-integer value " + numText(v) +
                       " in observation " + intText(i + 1) +
                       "; a value label can only label integer codes.");
    if (std::fabs(v) > 2147483647.0)
      return utilError("labmask: `" + numvar + "' has the value " + numText(v) +
                       " in observation " + intText(i + 1) +
                       ", which is outside the range a value label can code.");
    const long long code = static_cast<long long>(v);
    const std::string& t = text[static_cast<size_t>(i)];
    if (t.empty()) continue;  // no text to attach for this observation
    if (t.find('"') != std::string::npos)
      return utilError("labmask: the text for code " + std::to_string(code) + " (`" + t +
                       "') contains a double quote, which `label define` cannot carry in this "
                       "build. Remove the quote from `" + *valuesVar + "' first.");
    auto it = mapping.find(code);
    if (it == mapping.end()) {
      mapping.emplace(code, t);
    } else if (it->second != t) {
      return utilError("labmask: code " + std::to_string(code) + " of `" + numvar +
                       "' carries two different texts in `" + *valuesVar + "' (`" + it->second +
                       "' and `" + t + "'); labmask needs one text per code.");
    }
  }
  if (mapping.empty())
    return utilError("labmask: `" + *valuesVar + "' supplies no text for any value of `" + numvar +
                     "'; there is nothing to label.");

  std::string line = "label define " + lblname;
  for (const auto& [code, t] : mapping) line += " " + std::to_string(code) + " \"" + t + "\"";
  line += ", modify";
  ojson defined = runLabelDefine(line, session);
  if (const ojson* e = firstError(defined)) return *e;
  ojson attached = runLabelValues(numvar, lblname, session);
  if (const ojson* e = firstError(attached)) return *e;
  return silent();
}

// ===========================================================================
// splitvallabels
// ===========================================================================
//
//     splitvallabels varname [if] [in] [, length(#) nobreak recode vals(numlist) local(macname)]
//
// Breaks the value-label text of every value the variable takes (in the rows selected) into
// lines of at most length() characters (default 15) -- at blanks, and a word longer than that
// is itself cut into length()-sized pieces unless nobreak -- and publishes them as
// r(relabel), the string a graph command's relabel() option takes: for each value
//     `` code `" `"line"' `"line"' "' ``
// with an empty second line (`""`) for a text that fits on one. vals() keeps only the values
// listed; recode numbers the values 1, 2, ... in order instead of their own codes; local()
// also leaves the string (without its leading blank) in a local macro. A variable without a
// value label leaves nothing. Read-only: the label plane is not touched.
ojson runSplitvallabels(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSplitvallabels.known,
                                            kVocabSplitvallabels.unimplemented))
    return ojson(*err);
  if (parsed.varlist.size() != 1)
    return ojson(eval::errorBlock("syntax_error", "varlist required", std::nullopt, std::nullopt,
                                  std::nullopt, "r(100)"));
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  const std::string var = parsed.varlist[0];
  Datastore* ds0 = session.datastore();
  if (ds0 == nullptr || !ds0->has(var))
    return utilErrorRc("variable " + var + " not found", 111);

  long long length = 15;
  if (auto lv = optStr(parsed.opts, "length")) {
    if (!parseStrictInt(trimOf(*lv), &length) || length < 1)
      return utilErrorRc("length() invalid", 198);
  }
  const bool nobreak = hasFlag(parsed.opts, "nobreak");
  const bool recode = hasFlag(parsed.opts, "recode");
  std::set<double> keepVals;
  bool haveVals = false;
  if (auto vv = optStr(parsed.opts, "vals")) {
    haveVals = true;
    for (const auto& w : splitWs(*vv)) {
      double d = 0;
      if (!parseStrictDouble(w, &d)) return utilErrorRc("vals() invalid", 198);
      keepVals.insert(d);
    }
  }

  // The value-label set attached to the variable.
  std::map<long long, std::string> codes;
  auto lnIt = session.varLblname.find(var);
  if (lnIt != session.varLblname.end() && !lnIt->second.empty()) {
    auto defIt = session.labelDefs.find(lnIt->second);
    if (defIt != session.labelDefs.end()) {
      for (const auto& [codeStr, text] : defIt->second) {
        long long c = 0;
        if (parseStrictInt(codeStr, &c)) codes.emplace(c, text);
      }
    }
  } else if (auto vlIt = session.valueLabels.find(var); vlIt != session.valueLabels.end()) {
    for (const auto& [code, text] : vlIt->second) codes.emplace(static_cast<long long>(code), text);
  }
  if (lnIt == session.varLblname.end() && codes.empty()) return silent();

  // The values the variable takes in the rows selected.
  ReadScope scope(session, parsed, {var}, "splitvallabels", true);
  if (scope.failed()) return scope.error().is_null() ? ojson(eval::noDataLoaded()) : scope.error();
  Datastore* ds = scope.get();
  std::set<double> values;
  if (ds->dtype(var) == "f64") {
    const cecon::Column& col = ds->col(var);
    const double* d = col.f64();
    for (int64_t i = 0; i < ds->nRows(); ++i)
      if (!std::isnan(d[i])) values.insert(d[i]);
  } else {
    return utilErrorRc("type mismatch", 109);
  }

  auto utf8len = [](const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
      if ((c & 0xC0) != 0x80) ++n;
    return n;
  };
  auto cutChars = [&](const std::string& w, size_t n) {  // first n characters
    size_t i = 0, k = 0;
    while (i < w.size() && k < n) {
      ++i;
      while (i < w.size() && (static_cast<unsigned char>(w[i]) & 0xC0) == 0x80) ++i;
      ++k;
    }
    return i;
  };
  auto chop = [&](const std::string& text) {
    std::vector<std::string> toks;
    for (const auto& w : splitWs(text)) {
      if (nobreak || static_cast<long long>(utf8len(w)) <= length) {
        toks.push_back(w);
        continue;
      }
      std::string rest = w;
      while (static_cast<long long>(utf8len(rest)) > length) {
        const size_t at = cutChars(rest, static_cast<size_t>(length));
        toks.push_back(rest.substr(0, at));
        rest = rest.substr(at);
      }
      if (!rest.empty()) toks.push_back(rest);
    }
    std::vector<std::string> pieces;
    std::string cur;
    for (const auto& w : toks) {
      if (cur.empty()) cur = w;
      else if (static_cast<long long>(utf8len(cur) + 1 + utf8len(w)) <= length) cur += " " + w;
      else {
        pieces.push_back(cur);
        cur = w;
      }
    }
    if (!cur.empty()) pieces.push_back(cur);
    return pieces;
  };

  std::string relabel;
  long long k = 0;
  for (double v : values) {
    if (haveVals && keepVals.count(v) == 0) continue;
    ++k;
    const long long code = static_cast<long long>(v);
    std::string text;
    auto it = v == std::floor(v) ? codes.find(code) : codes.end();
    if (it != codes.end()) text = it->second;
    else text = numText(v);
    std::vector<std::string> pieces = chop(text);
    relabel += " " + std::to_string(recode ? k : code) + " `\" ";
    for (const auto& p : pieces) relabel += "`\"" + p + "\"' ";
    if (pieces.size() <= 1) relabel += "\"\" ";
    relabel += "\"'";
  }
  eval::replaceRClass(session.eval);
  session.eval.rStrings.set("relabel", relabel);
  eval::declareRPosted(session.eval, "splitvallabels", {"relabel"});
  if (auto lm = optStr(parsed.opts, "local")) {
    const std::string name = trimOf(*lm);
    if (!name.empty()) session.eval.locals.set(name, relabel.empty() ? relabel : relabel.substr(1));
  }
  return silent();
}


}  // namespace piet::handlers
