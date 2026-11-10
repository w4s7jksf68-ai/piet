// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/loops.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "piet/cclass.h"
#include "piet/counters.h"  // BUG-350 coverage: the loop-capture drop site counts too
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/tokenizer.h"
#include "piet/parallel.h"
#include "piet/stata_format.h"  // BUG-1395: numlist element text is %15.0g
#include "piet/strutil.h"
#include "core/expr_blanks.h"  // FU-875: Stata's blank squeeze and r(132) pairing rule
#include "core/ident.h"  // Unicode names: the shared identifier byte tests
#include "data/missing.h"  // BUG-921: the extended-missing kind of a cell read by the if condition
#include "dispatch/dispatch_profile.h"  // EXPRLAYER: expression-zone attribution (opt-in, stderr)

#include "loops_internal.h"

namespace piet::loops {

using namespace loops_detail;

namespace {

constexpr int kNumlistMaxElements = 10000;

// ---------------------------------------------------------------------------
// Numlist expansion (loops.py's _parse_numlist)

// BUG-1395: whether the element past the end still belongs follows StataBE 19.5's
// own measurements, not one formula: no rule tried reproduces all 2,392 measured
// ranges (lane expr-sem x1b nl*.do, ho.do and r3/g1.do; the reviews' vnl*.do,
// v95b-d.do and nlx.do). `v` is start + k*step and `run` the element's value
// (numlistValue below). An element that is a whole number gets no hair:
// 0(1)2.9999999999999996, 0.5(0.5)2.99999999999999 and 1(1.5)3.99999999999999 stop
// short of it. An end the value lands within one unit in the last place of is
// kept (21.10(0.07)21.31). Otherwise `v` may pass the end by a hair relative to the
// distance covered, k*|step|, compared in doubles, so the hair rounds to the end's
// grid: 0(0.1)0.3 ends at .3, 0(2.5)7.4999999999999 keeps 7.5, 0(0.1)0.299999999999996
// drops .3, and 156.33(0.05)158.38 and -200.10(0.03)-198.30 keep an end `v` passes by
// one unit in the last place. That keeps every end Stata keeps, here and on a fresh
// hold-out of 360 ranges (r3/g2.do). 226 of the 2,392 and 3 of the 360 keep an end
// Stata drops, as base PIET did: 60.9(0.02)61, 10(.005)10.02, 1000.1(0.1)1000.4
// and the step 0.02 from 32.5 to 63.3 and from 128.4 up.
bool numlistPastEnd(double v, double run, double k, double step, double end) {
  const double over = step > 0 ? v - end : end - v;
  if (over <= 0) return false;
  if (v == std::floor(v)) return true;
  const double runOver = step > 0 ? run - end : end - run;
  const double endUlp = std::nextafter(std::fabs(end), INFINITY) - std::fabs(end);
  if (runOver <= endUlp) return false;
  const double hair = 1.335e-14 * std::fabs(k * step);
  return step > 0 ? v > end + hair : v < end - hair;
}

// BUG-1395: an element's value is the running sum, start, start + step, ..., as
// StataBE 19.5 holds it: 100(-0.1)0 prints 9.900000000001 and .1000000000014 and
// -1(0.10000000000001).5 9.97812943e-14, the sum's digits, where start + k*step
// prints 9.9, .1 and 9.99200722e-14. A sum that lands within 1.01e-14 of the
// distance covered, |sum - start|, of a whole number becomes that number, and the
// sum goes on from there: -1(.2)1 and .3(-.1)-.3 print 0, 27.88(-0.17)-192.78
// steps on from 17 and prints .85 .68 .51, 29.8(-0.1)-26.7 prints 1 .9 ... 0 ... -1,
// while 11.36(-0.01)-0.58 never snaps and prints -.0099999999998. The element the
// end is reached at prints as the end: 100(-0.1)0 ends at 0, -10.65(0.01)0.11 at
// .11. Element texts then differ from Stata's in 12 of 106,814 measured and 5 of
// 146,546 on the hold-out, all in four ranges whose snaps sit within 0.5% of the
// cut: Stata snapped 27.88(-0.17)-192.78 at 17 but not -55.76(0.17)87.55 at -34 at
// the same ratio, so the cut Stata applies is not exactly this one.
double numlistValue(double run, double start) {
  const double whole = std::round(run);
  if (whole != run && std::fabs(run - whole) < 1.01e-14 * std::fabs(run - start)) return whole;
  return run;
}

}  // namespace

namespace loops_detail {

NumlistResult parseNumlist(const std::string& spec) {
  NumlistResult r;
  std::vector<double> result;

  // A number as Stata writes one: `.5`, `-.25`, `1e3` and `1.5e-3` too
  // (StataBE: -.5(.25)0 is -.5 -.25 0; 1e15(1e15)3e15 is three elements).
  static const std::string kNum = R"([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)";
  static const std::regex reRangeStep("^(" + kNum + ")\\((" + kNum + ")\\)(" + kNum + ")$");
  static const std::regex reSlash("^(" + kNum + ")/(" + kNum + ")$");

  std::istringstream iss(spec);
  std::string tok;
  while (iss >> tok) {
    std::smatch m;
    double start = 0, step = 0, end = 0;
    bool range = false;
    if (std::regex_match(tok, m, reRangeStep)) {
      start = std::stod(m[1]), step = std::stod(m[2]), end = std::stod(m[3]);
      if (step == 0.0) {
        r.isError = true;
        r.error = errBlock("numlist: step cannot be zero");
        return r;
      }
      // The step runs toward the end whatever its sign: StataBE 19.5 expands
      // 3(1)1 to 3 2 1 and 1(-1)3 to 1 2 3.
      if ((end - start) * step < 0) step = -step;
      range = true;
    } else if (std::regex_match(tok, m, reSlash)) {
      start = std::stod(m[1]), end = std::stod(m[2]);
      step = start <= end ? 1.0 : -1.0;
      range = true;
    }
    if (range) {
      double run = start;
      for (int k = 0;; ++k) {
        // start + k*step, unfused, is what the end test reads (numlistPastEnd).
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
        const double v = start + k * step;
        if (k > 0) run = numlistValue(run + step, start);
        if (numlistPastEnd(v, run, k, step, end)) break;
        const double hair = 1.335e-14 * std::fabs(k * step);
        result.push_back(k > 0 && v >= end - hair && v <= end + hair ? end : run);
        if (k + 1 > kNumlistMaxElements) {
          r.isError = true;
          r.error = errBlock("numlist: more than " + std::to_string(kNumlistMaxElements) + " elements");
          return r;
        }
      }
      continue;
    }
    try {
      size_t pos = 0;
      double v = std::stod(tok, &pos);
      if (pos != tok.size()) throw std::invalid_argument("trailing");
      result.push_back(v);
    } catch (...) {
      r.isError = true;
      r.error = errBlock("numlist: invalid token '" + tok + "'");
      return r;
    }
  }
  if (result.size() > static_cast<size_t>(kNumlistMaxElements)) {
    r.isError = true;
    r.error = errBlock("numlist: more than " + std::to_string(kNumlistMaxElements) + " elements");
    return r;
  }
  // BUG-1395: each element's text is its %15.0g rendering with the padding
  // dropped -- StataBE 19.5 (x1b/a6-a8.do, 38 values): .0625 not 0.0625,
  // .1234567890123, 1.00000000e-20, 1.23456789e+13 but 1234567890123, -.3333333333333.
  for (double v : result) r.tokens.push_back(piet::fmt::general(v, 15, 0));
  return r;
}

}  // namespace loops_detail

bool expandNumlist(const std::string& spec, std::vector<std::string>* elements) {
  NumlistResult nr = parseNumlist(spec);
  if (nr.isError) return false;
  *elements = std::move(nr.tokens);
  return true;
}

}  // namespace piet::loops
