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
      "skip", "varwidth"},
     {"type", "all", "any", "format", "columns", "vallabel", "vallabelname", "vallabeltext",
      "vallabeltextdef", "vallabeltextuse", "vallabelcountdef", "vallabelcountuse", "char",
      "charname", "chartext", "placeholder", "detail"}});

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
  if (hasFlag(parsed, "varlabel") || optStr(parsed, "varlabeltext").has_value())
    session.syncEvalVarLabels();
  if (hasFlag(parsed, "varlabel")) {
    std::vector<std::string> keep;
    for (const auto& v : vars)
      if (!varLabelOf(session, v).empty()) keep.push_back(v);
    vars = keep;
  }
  if (auto text = optStr(parsed, "varlabeltext")) {
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
  out.push_back(displayBlock(displayInColumns(shown, static_cast<int>(*indent),
                                              static_cast<int>(*skip))));

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
