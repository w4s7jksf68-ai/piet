// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// astile: xtile's quantile groups computed within each by() group of the if/in sample.
// Shared helpers: column_write_support.h. The family note for this command:
//
//   astile       implements astile per its help file (Shah): xtile's cut points per by()
//                group. The cut points are the engine's own `egen pctile` (the same
//                stata_pctile rule the engine's xtile uses, at the same p = 100*k/nq), and
//                the group number is 1 + #(x > cut), xtile's assignment rule. One engine
//                call for any number of groups.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/handlers/data_values.h"
#include "piet/loops.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"
#include "piet/storage_type.h"
#include "piet/strutil.h"
#include "column_write_support.h"

// integ's spline must round every product and sum separately, as StataBE's expression
// evaluator does; a fused multiply-add changes the last bit (integ_basic is pinned to it).
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch. The aliases are the minimum
// abbreviations the commands' own syntax lines declare (Generate -> g, Trapezoid -> t,
// Initial -> i, Label -> l, Nquantiles -> n), which the 2-character prefix floor cannot reach.
namespace {

// astile: every option of the syntax line is read; qc() (the criterion the breakpoints are
// taken over) included.
const piet::eval::OptVocab& kVocabAstile = piet::eval::registerOptVocab(
    {"astile"}, {{"nquantiles", "by", "qc"}, {}, {{"n", "nquantiles"}}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace column_write_detail;

namespace {

// The text after `=` as typed: up to the comma, a `[` or the `if`/`in` keyword, with every
// blank before it kept, as Stata's `=/exp` hands it on (review gb B6 `3 quantiles of xf `;
// probe p7: `x , nq(3)` -> "x ", `x  if g==1` -> "x  "). `fallback` when the line has no `=`.
std::string typedExpression(const std::string& line, const std::string& fallback) {
  const size_t e = line.find('=');
  if (e == std::string::npos) return fallback;
  size_t b = e + 1;
  while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
  size_t stop = line.size();
  int depth = 0;
  bool inStr = false;
  for (size_t i = b; i < line.size(); ++i) {
    const char ch = line[i];
    if (ch == '"') inStr = !inStr;
    if (inStr) continue;
    if (ch == '(') ++depth;
    if (ch == ')') --depth;
    if (depth > 0) continue;
    if (ch == ',' || ch == '[') {
      stop = i;
      break;
    }
    if (i > b && std::isspace(static_cast<unsigned char>(line[i - 1])) &&
        (line.compare(i, 2, "if") == 0 || line.compare(i, 2, "in") == 0) &&
        (i + 2 == line.size() || std::isspace(static_cast<unsigned char>(line[i + 2])))) {
      stop = i;
      break;
    }
  }
  std::string out = line.substr(b, stop - b);
  if (stop == line.size())  // the end of the line is not a separator
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
  return out.empty() ? fallback : out;
}

}  // namespace

// `astile newvar = varname [if] [in] [, nquantiles(#) by(varlist) qc(exp)]`; `bysort g:`
// arrives as by(). Implements astile per its help file (Shah): xtile's quantile groups,
// computed within each by() group of the if/in sample (community log astile_vs_xtile10: 0
// disagreements with xtile; astile_nq2_by / _nq4_by / _byprefix / _if). Unlike xtile,
// nquantiles() may exceed the group's size (probe p1 asmall/anq20) and must be a whole number
// in 2..4097 (anq1, review gk K10: r(3598)). The variable is c(type), double with by(), and is
// labelled "# quantiles of <exp as typed>". An existing name and an expression in place of a
// variable are r(3598), as the package's Mata routine answers them (astile_exists,
// astile_expr); a string variable is r(109) (probe p1 astr).
ojson runAstile(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabAstile.known,
                                            kVocabAstile.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  const std::string arg = strutil::trim(parsed.argText);
  const size_t eq = arg.find('=');
  if (eq == std::string::npos)
    return syntaxError("astile: syntax is `astile newvar = varname, nquantiles(#)'");
  const std::string newvar = strutil::trim(arg.substr(0, eq));
  const std::string expText = strutil::trim(arg.substr(eq + 1));
  if (newvar.empty() || expText.empty()) return syntaxError("invalid syntax");
  const std::string expTyped = typedExpression(parsed.raw, expText);
  if (splitWs(newvar).size() != 1 || !strutil::isValidStataIdentifier(newvar))
    return syntaxError(newvar + " invalid name");

  std::string nqText = "2";
  if (auto q = optStr(parsed.opts, "nquantiles")) {
    if (!q->empty()) nqText = *q;
  }
  if (columnExists(session, newvar))
    return rcError("variable " + newvar + " already defined", 3598);
  // The package hands the text after `=` to Mata as a variable name.
  std::string srcvar;
  {
    const std::vector<std::string> words = splitWs(expText);
    const bool plainName = words.size() == 1 && strutil::isValidStataIdentifier(words.front());
    std::vector<std::string> one;
    if (!plainName || resolveVars(session, {words.front()}, &one).has_value() || one.size() != 1)
      return rcError("variable " + words.front() + " not found", 3598);
    srcvar = one.front();
  }
  double nqD = 0.0;
  if (!parseStrictDouble(nqText, &nqD) || nqD != std::floor(nqD))  // review gk K10
    return rcError("option nq() incorrectly specified", 3598);
  if (nqD < 2 || nqD > 4097) return rcError("nquantiles must be between 2 and 4097", 3598);
  const int nq = static_cast<int>(nqD);
  std::vector<std::string> byVars;
  if (auto b = optStr(parsed.opts, "by")) {
    if (auto e = resolveVars(session, splitWs(*b), &byVars)) return *e;
  }
  {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return ojson(eval::noDataLoaded());
    if (ds->col(srcvar).dtype == cecon::DType::Factor) return rcError("type mismatch", 109);
  }

  // The values the cut points come from: the variable itself, or a copy that is missing
  // outside if/in.
  ScratchCols scratch(session);
  std::string xcol = srcvar;
  if (hasQualifier(parsed)) {
    xcol = "__piet_t1g_ax";
    dropColumns(session, {xcol});
    scratch.add(xcol);
    if (auto e = firstError(dispatch("quietly generate double " + xcol + " = " + srcvar +
                                         qualifierText(parsed),
                                     session)))
      return *e;
  }
  // qc(): the breakpoints come from the sample's rows that meet the criterion, and every row
  // of the sample is then placed against them (astile's help, option 3; probe p5 aqc*).
  std::string cutSource = xcol;
  if (auto qc = optStr(parsed.opts, "qc")) {
    if (qc->empty()) return syntaxError("qc() requires an expression");
    cutSource = "__piet_t1g_aq";
    dropColumns(session, {cutSource});
    scratch.add(cutSource);
    if (auto e = firstError(dispatch("quietly generate double " + cutSource + " = " + xcol +
                                         " if (" + *qc + ")",
                                     session)))
      return *e;
  }
  // One engine call: the nq-1 cut points per group (egen pctile at p = 100*k/nq, the p the
  // engine's xtile uses), then 1 + #(x > cut), then the cut columns go.
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin()};
  std::string assign = newvar + "=cond(missing(" + xcol + "),.,1";
  std::vector<std::string> cuts;
  for (int k = 1; k < nq; ++k) {
    const std::string c = "__piet_t1g_c" + std::to_string(k);
    cuts.push_back(c);
    const double p = 100.0 * k / nq;
    std::string spec = c + "=pctile(" + cutSource + "), p(" + eval::pythonFloatRepr(p) + ")";
    if (!byVars.empty()) spec += " by(" + joinSpace(byVars) + ")";
    args.push_back("--egen");
    args.push_back(spec);
    assign += "+(" + xcol + ">" + c + ")";
  }
  assign += ")";
  args.push_back("--gen");
  args.push_back(assign);
  for (const auto& c : cuts) {
    args.push_back("--drop");
    args.push_back(c);
  }
  const std::string out = mintScratchPath(session.workdir(), "t1gen_astile");
  args.push_back("--out");
  args.push_back(out);
  {
    SessionState::ResidentEligible resident(session);
    if (auto e = runVerb(args)) {
      cecon::native_unlink(out);
      return *e;
    }
    session.load(out);
  }
  scratch.clear();
  // The type: double with by() (or the by-prefix), else c(type) like any new variable --
  // float by default, double under `set type double` (review gc C1/C2).
  const std::string type =
      byVars.empty() ? storage::resolveCreateType(session, "") : std::string("double");
  storage::recordCreateType(session, newvar, byVars.empty() ? "" : "double", type);
  session.setVarLabel(newvar, nqText + " quantiles of " + expTyped);
  return mutationResult(session, "gen", {newvar});
}

}  // namespace piet::handlers
