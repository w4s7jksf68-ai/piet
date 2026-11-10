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
  const std::vector<std::string> text = columnAsStrings(ds, *valuesVar);

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
// renvarlab
// ===========================================================================
//
// A rename LOOP over runRename, with every target name validated before the
// first rename happens -- a collision on the third variable must not leave the
// first two renamed. The package's label-driven renaming (derive each new name
// from the variable's LABEL) is refused by the vocabulary rather than
// approximated: that is a different derivation of the names, not an option on
// this one.
ojson runRenvarlab(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRenvarlab.known,
                                            kVocabRenvarlab.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  const std::string prefix = optStr(parsed.opts, "prefix").value_or(std::string());
  const std::string suffix = optStr(parsed.opts, "suffix").value_or(std::string());
  const bool upper = hasFlag(parsed.opts, "upper");
  const bool lower = hasFlag(parsed.opts, "lower");
  if (upper && lower)
    return utilSyntax("renvarlab: `upper` and `lower` cannot both be given.");
  if (prefix.empty() && suffix.empty() && !upper && !lower)
    return utilSyntax(
        "renvarlab: nothing to do -- give at least one of prefix(), suffix(), upper, lower.");

  Datastore* dsp = nullptr;
  try {
    dsp = session.datastore();
  } catch (const std::exception& e) {
    return utilError(std::string("renvarlab: ") + e.what(), "data_io");
  }
  if (dsp == nullptr) return ojson(eval::noDataLoaded());

  std::vector<std::string> vars;
  if (parsed.varlist.empty()) {
    vars = dsp->names();
  } else {
    // expandVarlist is the ONE place the wildcard/hyphen varlist grammar lives.
    loops::VarlistExpandResult ex = loops::expandVarlist(joinWith(parsed.varlist, " "), session);
    if (ex.isError) return ex.error;
    vars = ex.tokens;
  }
  if (vars.empty()) return utilError("renvarlab: no variables to rename");

  const std::set<std::string> renaming(vars.begin(), vars.end());
  std::set<std::string> taken;
  for (const auto& nm : dsp->names()) {
    if (renaming.count(nm) == 0) taken.insert(nm);
  }

  std::vector<std::pair<std::string, std::string>> plan;
  std::set<std::string> targets;
  for (const auto& nm : vars) {
    std::string out = prefix + nm + suffix;
    if (upper) out = upperAscii(out);
    if (lower) out = lowerOf(out);
    if (out == nm) continue;  // nothing to do for this one
    if (!strutil::isValidStataIdentifier(out))
      return utilSyntax("renvarlab: `" + nm + "' would become `" + out +
                        "', which is not a legal variable name -- " +
                        strutil::stataIdentifierRuleText());
    if (taken.count(out) != 0)
      return utilError("renvarlab: `" + nm + "' would become `" + out +
                           "', which already exists and is not itself being renamed.",
                       "bad_input");
    if (!targets.insert(out).second)
      return utilError("renvarlab: two variables would both become `" + out + "'.", "bad_input");
    plan.emplace_back(nm, out);
  }
  if (plan.empty()) return silent();

  std::vector<std::string> fromNames, toNames;
  for (const auto& [from, to] : plan) {
    ojson r = runRename(from, to, session);
    if (const ojson* e = firstError(r)) return *e;
    fromNames.push_back(from);
    toNames.push_back(to);
  }
  // BUG-452 (2026-09-07, AER scope 2 K1b): this used to return silent() and
  // SWALLOW every runRename's mutationResult, so a command that rewrites the
  // variable plane published nothing at all -- the consumer of the block stream
  // never learned the names changed. It went unseen because renvarlab's freeze
  // row is `no_fixture`: the capture gates can only watch what a command emits
  // ([[gates-watch-production-not-absence]]). Found when the identical swallow
  // in the new `tolower` made ITS freeze golden pin the preamble's varlist.
  // ONE block for the whole plan, with the complete new/dropped pair, rather
  // than one per rename.
  return mutationResult(session, "rename", toNames, fromNames);
}

// ===========================================================================
// splitvallabels
// ===========================================================================
//
// Chops a value label's texts at whitespace boundaries into pieces of at most
// length() characters and publishes them as r(relabel) -- the string a graph
// command's `relabel()` option takes verbatim. Read-only: it never touches the
// label plane it reads.
ojson runSplitvallabels(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSplitvallabels.known,
                                            kVocabSplitvallabels.unimplemented))
    return ojson(*err);
  if (parsed.varlist.size() != 1)
    return utilSyntax("splitvallabels: syntax is `splitvallabels varname [, length(#)]`");
  const std::string var = parsed.varlist[0];

  long long length = 12;  // splitvallabels.ado's own default
  if (auto lv = optStr(parsed.opts, "length")) {
    if (!parseStrictInt(*lv, &length) || length < 1)
      return utilSyntax("splitvallabels: length() requires a whole number of at least 1, got `" +
                        *lv + "'");
  }

  // The variable's value label, from the session's label plane. valueLabels is
  // the resolved per-variable map; when a variable was attached to a label set
  // that has since been (re)defined, varLblname + labelDefs is the same answer
  // from its two halves.
  std::map<long long, std::string> codes;
  auto vlIt = session.valueLabels.find(var);
  if (vlIt != session.valueLabels.end()) {
    for (const auto& [code, text] : vlIt->second) codes.emplace(static_cast<long long>(code), text);
  } else {
    auto lnIt = session.varLblname.find(var);
    if (lnIt != session.varLblname.end()) {
      auto defIt = session.labelDefs.find(lnIt->second);
      if (defIt != session.labelDefs.end()) {
        for (const auto& [codeStr, text] : defIt->second) {
          long long c = 0;
          if (parseStrictInt(codeStr, &c)) codes.emplace(c, text);
        }
      }
    }
  }
  if (codes.empty())
    return utilError("splitvallabels: `" + var +
                     "' has no value label attached, so there is nothing to split.");

  auto chop = [&](const std::string& text) {
    // Greedy packing at WHITESPACE boundaries: a word longer than length() is
    // its own piece rather than being cut mid-word (the option is a target, not
    // a hard truncation -- `nobreak`, which would suppress the chopping
    // entirely, is the refused option).
    std::vector<std::string> pieces;
    std::string cur;
    for (const auto& w : splitWs(text)) {
      if (cur.empty()) {
        cur = w;
      } else if (static_cast<long long>(cur.size() + 1 + w.size()) <= length) {
        cur += " " + w;
      } else {
        pieces.push_back(cur);
        cur = w;
      }
    }
    if (!cur.empty()) pieces.push_back(cur);
    if (pieces.empty()) pieces.emplace_back();
    return pieces;
  };

  std::string relabel;
  for (const auto& [code, text] : codes) {
    if (text.find('"') != std::string::npos)
      return utilError("splitvallabels: the label text for code " + std::to_string(code) +
                       " contains a double quote, which cannot travel inside r(relabel).");
    if (!relabel.empty()) relabel += " ";
    relabel += std::to_string(code);
    for (const auto& p : chop(text)) relabel += " \"" + p + "\"";
  }
  session.eval.rStrings.set("relabel", relabel);
  return silent();
}


}  // namespace piet::handlers
