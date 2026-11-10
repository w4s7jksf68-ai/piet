// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// integ: numerical integration of y over x (cubic spline or trapezoid), r(integral) and
// generate(); the spline kernel lives here with its only caller. Shared helpers:
// column_write_support.h. The family note for this command:
//
//   integ        implements the cubic-spline rule described in [R] dydx "Methods and
//                formulas" (clamped spline, endpoint slopes from a quadratic through the
//                three end points; trapezoid on request). This is the one computation here,
//                because no other command in PIET fits a spline; identified against StataBE
//                (w2ax_t1_official.log integ_*; probe p2 i3/i2/i1/ities).
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

// integ: every option of the syntax line is read (double/float pick the stored type).
const piet::eval::OptVocab& kVocabInteg = piet::eval::registerOptVocab(
    {"integ"}, {{"generate", "replace", "by", "trapezoid", "initial", "double", "float"},
                {},
                {{"g", "generate"}, {"t", "trapezoid"}, {"i", "initial"}}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace column_write_detail;

namespace {

// ===========================================================================
// integ -- the cubic spline of [R] dydx, Methods and formulas
// ===========================================================================

// Second derivatives y'' of the spline through n >= 3 points with distinct, ascending x. The
// end slopes are those of the quadratic through the three points at each end; the interior
// rows are the tridiagonal sweep, then the back substitution. Each operation is written in
// the order StataBE evaluates it, unfused; that reproduces integ_basic (34.83333333333333)
// and probe p2 i3 (+1.1555555555555X+003) to the last bit. On irregularly spaced x a 1-ulp
// difference can remain (review gd D1: +1.0500c0dd332deX+004 in StataBE, ...dd here).
std::vector<double> splineSecondDerivs(const std::vector<double>& x, const std::vector<double>& y) {
  const size_t n = x.size();
  std::vector<double> s(n, 0.0), z(n, 0.0), y2(n, 0.0);
  s[0] = -0.5;
  z[0] = (3.0 / (x[1] - x[0])) * ((y[1] - y[2]) / (x[1] - x[2]) - (y[0] - y[2]) / (x[0] - x[2]));
  for (size_t i = 1; i + 1 < n; ++i) {
    const double w = (x[i] - x[i - 1]) / (x[i + 1] - x[i - 1]);
    const double p = w * s[i - 1] + 2.0;
    s[i] = (w - 1.0) / p;
    z[i] = (6.0 * ((y[i + 1] - y[i]) / (x[i + 1] - x[i]) - (y[i] - y[i - 1]) / (x[i] - x[i - 1])) /
                (x[i + 1] - x[i - 1]) -
            w * z[i - 1]) /
           p;
  }
  const size_t m = n - 1;
  y2[m] = ((6.0 / (x[m] - x[m - 1])) *
               ((y[m] - y[m - 2]) / (x[m] - x[m - 2]) - (y[m - 1] - y[m - 2]) / (x[m - 1] - x[m - 2])) -
           z[m - 1]) /
          (s[m - 1] + 2.0);
  for (size_t k = m; k-- > 0;) y2[k] = s[k] * y2[k + 1] + z[k];
  return y2;
}

// The running integral at each point: I0 + sum over the intervals so far of the spline's exact
// integral, h(y_k + y_k+1)/2 - h^3(y''_k + y''_k+1)/24 (the trapezoid alone with `trapezoid`, or
// when there are fewer than three points).
std::vector<double> runningIntegral(const std::vector<double>& x, const std::vector<double>& y,
                                    bool trapezoid, double initial) {
  const size_t n = x.size();
  const bool spline = !trapezoid && n > 2;
  std::vector<double> y2;
  if (spline) y2 = splineSecondDerivs(x, y);
  std::vector<double> out(n, 0.0);
  double run = 0.0;
  for (size_t i = 0; i < n; ++i) {
    if (i > 0) {
      const double h = x[i] - x[i - 1];
      const double c = spline ? (h * h * (y2[i] + y2[i - 1])) / 12.0 : 0.0;
      run += (h * ((y[i] + y[i - 1]) - c)) / 2.0;
    }
    out[i] = initial + run;
  }
  return out;
}

}  // namespace

// `integ yvar xvar [if] [in] [, generate(newvar) replace by(varlist) trapezoid initial(#)
// double float]`. Implements integ per [R] dydx: the points are the observations with both
// variables non-missing (and every by() variable non-missing), taken in ascending x; tied x
// values are replaced by their mean y. Without by() it prints the count and the integral and
// sets r(N_points) and r(integral); with by() it prints nothing and r() is cleared
// (probe p2 iby). generate() is float unless `double` (or set type double), labelled
// "Integral of <y>" (probe p2 itiesg).
ojson runInteg(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabInteg.known,
                                            kVocabInteg.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  std::vector<std::string> vars;
  if (auto e = resolveVars(session, parsed.varlist, &vars)) return *e;
  if (vars.size() < 2) return rcError("too few variables specified", 102);  // probe p2 ione
  if (vars.size() > 2) return rcError("too many variables specified", 103);
  const std::string yvar = vars[0], xvar = vars[1];

  const std::optional<std::string> gen = optStr(parsed.opts, "generate");
  const bool replaceOpt = hasFlag(parsed.opts, "replace");
  const bool trapezoid = hasFlag(parsed.opts, "trapezoid");
  const bool dbl = hasFlag(parsed.opts, "double");
  const bool flt = hasFlag(parsed.opts, "float");
  std::vector<std::string> byToks;
  if (auto b = optStr(parsed.opts, "by")) byToks = splitWs(*b);
  if (!gen.has_value() || gen->empty()) {
    if (!byToks.empty())
      return syntaxError("you must specify option generate() with option by()");  // ibynogen
    if (replaceOpt) return syntaxError("you must specify option generate() with option replace");
    if (dbl) return syntaxError("you must specify option generate() with option double");
  }
  if (dbl && flt) return syntaxError("only one of double or float is allowed");
  double initial = 0.0;
  if (auto iv = optStr(parsed.opts, "initial")) {
    if (!parseStrictDouble(*iv, &initial))
      return syntaxError("option initial() incorrectly specified");
  }
  std::string genVar;
  if (gen.has_value() && !gen->empty()) {
    const std::vector<std::string> names = splitWs(*gen);
    for (const auto& nm : names) {
      if (!strutil::isValidStataIdentifier(nm)) return syntaxError(nm + " invalid name");
      if (columnExists(session, nm) && !replaceOpt)
        return rcError("option generate():\nvariable " + nm + " already defined", 110);  // iexist
    }
    if (names.size() > 1) return rcError("option generate():  too many variables specified", 103);
    genVar = names.front();
  }
  std::vector<std::string> byVars;
  if (auto e = resolveVars(session, byToks, &byVars)) return *e;

  std::vector<char> mask;
  if (auto e = sampleMask(session, parsed, &mask)) return *e;
  Datastore* ds = session.datastore();
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  if (ds->col(yvar).dtype == cecon::DType::Factor || ds->col(xvar).dtype == cecon::DType::Factor)
    return rcError("type mismatch", 109);
  const std::vector<double> y = columnDoubles(*ds, yvar);
  const std::vector<double> x = columnDoubles(*ds, xvar);
  std::vector<KeyCol> keys;
  for (const auto& b : byVars) keys.push_back(readKeyCol(*ds, b));
  const size_t n = y.size();

  // The sample: if/in, both variables non-missing, every by() variable non-missing.
  std::vector<size_t> rows;
  for (size_t i = 0; i < n; ++i) {
    if (!mask[i] || std::isnan(x[i]) || std::isnan(y[i])) continue;
    bool byMiss = false;
    for (const auto& k : keys) byMiss = byMiss || keyMissing(k, i);
    if (!byMiss) rows.push_back(i);
  }
  // Ascending x within each by() group; a tie keeps the current row order.
  std::stable_sort(rows.begin(), rows.end(), [&](size_t a, size_t b) {
    const int c = keysCompare(keys, a, b);
    if (c != 0) return c < 0;
    return x[a] < x[b];
  });

  std::vector<double> integral(n, std::numeric_limits<double>::quiet_NaN());
  int64_t nPoints = 0;
  double lastIntegral = std::numeric_limits<double>::quiet_NaN();
  bool anyTie = false;
  size_t g0 = 0;
  while (g0 < rows.size()) {
    size_t g1 = g0 + 1;
    while (g1 < rows.size() && keysCompare(keys, rows[g1], rows[g0]) == 0) ++g1;
    // One point per distinct x: the mean y over a tie (the running sum over the tie divided
    // by its size, in sort order), placed on the tie's last row.
    std::vector<double> px, py;
    std::vector<size_t> at;
    size_t t0 = g0;
    while (t0 < g1) {
      size_t t1 = t0 + 1;
      while (t1 < g1 && x[rows[t1]] == x[rows[t0]]) ++t1;
      double sum = 0.0;
      for (size_t q = t0; q < t1; ++q) sum += y[rows[q]];
      if (t1 - t0 > 1) anyTie = true;
      px.push_back(x[rows[t0]]);
      py.push_back(t1 - t0 > 1 ? sum / static_cast<double>(t1 - t0) : y[rows[t0]]);
      at.push_back(rows[t1 - 1]);
      t0 = t1;
    }
    std::vector<double> I = runningIntegral(px, py, trapezoid, initial);
    // `float` makes the running integral a float variable: every value, r(integral) among
    // them, is stored as a float (review gd D5/D10).
    if (flt)
      for (double& v : I) v = toFloat(v);
    for (size_t k = 0; k < I.size(); ++k) integral[at[k]] = I[k];
    nPoints += static_cast<int64_t>(px.size());
    if (!I.empty()) lastIntegral = I.back();
    g0 = g1;
  }
  if (nPoints == 0) return rcError("no observations", 2000);  // probe p2 inone
  // StataBE keeps a tie's value on ONE of its rows, the one its own (unstable) sort puts last
  // -- probe p2 irepl stored it on the FIRST row of the tie {1,4} and the LAST of {3,7}. Which
  // row that is cannot be reproduced, so a generate() over tied x is refused rather than
  // written to a row Stata might not have chosen. The integral itself does not depend on it.
  if (!genVar.empty() && anyTie)
    return rcError(
        "integ: " + xvar + " has tied values in the sample. Stata stores generate() on one row "
        "of each tie, chosen by its own sort, which this version of PIET cannot reproduce; the "
        "integral itself is served without generate(). Collapse the ties first (e.g. `collapse "
        "(mean) " + yvar + ", by(" + xvar + ")`).",
        199);

  ojson out = ojson::array();
  if (byVars.empty()) {
    eval::replaceRClass(session.eval);
    session.eval.rValues.set("N_points", static_cast<double>(nPoints));
    session.eval.rValues.set("integral", lastIntegral);
    eval::declareRPosted(session.eval, "integ", {"N_points", "integral"});
    out.push_back(displayBlock("\nnumber of points = " + std::to_string(nPoints) +
                               "\n\nintegral         = " +
                               strutil::trim(fmt::general(lastIntegral, 10, 0)) + "\n"));
  } else {
    // integ is r-class and posts nothing with by(): r() is emptied, and a read of
    // r(integral) is `.` (probe p2 iby).
    eval::replaceRClass(session.eval);
    eval::declareRPosted(session.eval, "integ", {});
  }
  if (!genVar.empty()) {
    const bool asDouble = dbl || (!flt && storage::resolveCreateType(session, "") == "double");
    const std::string type = asDouble ? "double" : "float";
    if (replaceOpt && columnExists(session, genVar)) dropColumns(session, {genVar});
    std::vector<std::string> cells;
    cells.reserve(n);
    for (double v : integral) cells.push_back(numCell(v));
    if (auto e = setColumn(session, genVar, cells, false, true, type)) return *e;
    storage::recordCreateType(session, genVar, type, type);
    session.setVarLabel(genVar, "Integral of " + yvar);
    out.push_back(mutationResult(session, "gen", {genVar}));
  }
  return out;
}

}  // namespace piet::handlers
