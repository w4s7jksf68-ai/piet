// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/data_values.h"
#include "piet/dispatcher.h"  // dispatch(): cumul marks its if/in sample with generate

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <regex>
#include <ctime>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data/frame_internal.h"  // canonical numeric storage rounding for recast
#include "data/missing.h"         // cumul's by() groups: each missing code is its own
#include "core/rng.h"         // Philox4x32-10, the engine's own counter-based RNG
#include "core/rng_mt64.h"    // Stata's session stream: bsample's fast path
#include "estimation/bsample_draws.h"    // the fast path's draw, shared with the bootstrap prefix
#include "core/rng_normal.h"  // uniform01 / normal_icdf (AS241) -- drawnorm
#include "io/win_compat.h"    // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"  // piet::fmtStataDefaultG — Stata's %9.0g default
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"  // pythonFloatRepr -- round-trippable CSV numerics
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/storage_type.h"  // FU-944: input's c(type) and float store
#include "piet/strutil.h"
#include "estimation/estimation_internal.h"  // esampleHeadMask / esampleRewriteHeadMask
#include "engine/scratch_columns.h"          // cumul's if/in restriction and weight columns
#include "dataset_shape_support.h"
#include "dataset_shape_options.h"

namespace piet::handlers {

using namespace dataset_shape_detail;

namespace {

// ===========================================================================
// THE ONE PERCENTILE RULE
// ===========================================================================
//
// TRANSCRIBED, not re-derived, from dispatcher.cpp's winsor2 arm (the `pct`
// lambda beside `// Stata's percentile rule, the same one Frame::xtile and
// summarize,detail use`), which is itself the engine's `Frame::xtile` rule. Two
// percentile rules in one product is a silently wrong number waiting for a
// fixture that does not exist: `pctile`/`_pctile` here and `xtile`/`winsor2`
// there must answer the same question with the same arithmetic, so this is a
// verbatim copy and any change to either is a change to both.
//
// `sorted` must be ascending and free of missing.
double stataPercentile(const std::vector<double>& sorted, double p) {
  const int64_t n = static_cast<int64_t>(sorted.size());
  if (n == 0) return std::numeric_limits<double>::quiet_NaN();
  if (n == 1) return sorted[0];
  const double Pi = static_cast<double>(n) * p / 100.0;
  const double fl = std::floor(Pi);
  if (fl == Pi && Pi >= 1 && static_cast<int64_t>(Pi) < n)
    return (sorted[static_cast<size_t>(Pi) - 1] + sorted[static_cast<size_t>(Pi)]) / 2.0;
  int64_t idx = static_cast<int64_t>(fl) + 1;
  if (idx > n) idx = n;
  return sorted[static_cast<size_t>(idx) - 1];
}

// The weighted form of the same rule, [D] pctile "Methods and formulas"
// (/Applications/StataNow/docs/d.pdf, pctile p.776): with x(j) ascending and
// w(j) its weight, N = Σw, P = N p/100 and W(i) = Σ_{j<=i} w(j), take the first
// i with W(i) > P; the percentile is (x(i-1) + x(i))/2 when W(i-1) = P, else
// x(i). With every w(j) = 1 this is stataPercentile exactly. The weights are
// the raw ones, never rescaled: Stata compares W(i-1) = P on the weights as
// given, and a rescaled aweight moves that equality (w2aj_qualifiers
// qf_pt_aw_sp r(r2) 6.875 is the tie average; rescaled sums give 7.25).
//
// `sorted` is ascending in x, free of missing, every weight non-zero.
double stataWeightedPercentile(const std::vector<std::pair<double, double>>& sorted, double p) {
  if (sorted.empty()) return std::numeric_limits<double>::quiet_NaN();
  double total = 0.0;
  for (const auto& e : sorted) total += e.second;
  const double P = total * p / 100.0;
  double W = 0.0;
  for (size_t i = 0; i < sorted.size(); ++i) {
    const double before = W;
    W += sorted[i].second;
    if (W > P) {
      if (i > 0 && before == P) return (sorted[i - 1].first + sorted[i].first) / 2.0;
      return sorted[i].first;
    }
  }
  return sorted.back().first;
}

}  // namespace

// ===========================================================================
// pctile / _pctile
// ===========================================================================

namespace {

// The percentile list a `pctile`/`_pctile` command asks for. Either
// `percentiles()`/`p()` verbatim, or the nq-1 interior quantile points.
std::optional<std::vector<double>> percentileList(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts, const std::string& verb,
    int defaultNq, ojson* err) {
  const std::optional<std::string> pOpt = optStrAny(opts, {"percentiles", "p"});
  const std::optional<std::string> nqOpt = optStrAny(opts, {"nquantiles", "nq"});
  if (pOpt.has_value() && nqOpt.has_value()) {
    *err = rsSyntax(verb + ": percentiles() and nquantiles() cannot both be specified");
    return std::nullopt;
  }
  std::vector<double> out;
  if (pOpt.has_value()) {
    // A numlist may be comma- or space-separated; both spellings appear in the
    // corpus and Stata accepts either.
    std::string spec = *pOpt;
    for (char& c : spec)
      if (c == ',') c = ' ';
    for (const auto& t : splitWs(spec)) {
      double v = 0.0;
      if (!parseStrictDouble(t, &v)) {
        *err = rsSyntax(verb + ": `" + t + "' in percentiles() is not a number");
        return std::nullopt;
      }
      if (!(v > 0.0 && v < 100.0)) {
        *err = rsSyntax(verb + ": percentiles() takes values strictly between 0 and 100, got " + t);
        return std::nullopt;
      }
      out.push_back(v);
    }
    if (out.empty()) {
      *err = rsSyntax(verb + ": percentiles() is empty");
      return std::nullopt;
    }
    return out;
  }
  long long nq = defaultNq;
  if (nqOpt.has_value()) {
    if (!parseStrictInt(*nqOpt, &nq) || nq < 2) {
      *err = rsSyntax(verb + ": nquantiles() requires a whole number of at least 2, got `" +
                      *nqOpt + "'");
      return std::nullopt;
    }
  }
  for (long long i = 1; i < nq; ++i)
    out.push_back(100.0 * static_cast<double>(i) / static_cast<double>(nq));
  return out;
}

// The sorted, missing-free values of a numeric column, or an error block.
std::optional<std::vector<double>> sortedValues(SessionState& session, const std::string& verb,
                                                const std::string& var, ojson* err) {
  ReadFrame rf(session, {var});
  Datastore* ds = rf.get();
  if (ds == nullptr) {
    *err = rsError(verb + ": could not read the data" + (rf.why().empty() ? "" : " — " + rf.why()),
                   "data_io");
    return std::nullopt;
  }
  if (!ds->has(var)) {
    *err = rsError("variable " + var + " not found");
    return std::nullopt;
  }
  if (ds->dtype(var) == "factor") {
    *err = rsError(verb + ": `" + var + "' is a string variable; percentiles need a numeric one.");
    return std::nullopt;
  }
  std::vector<double> v;
  const double* d = ds->col(var).f64();
  const int64_t n = ds->nRows();
  v.reserve(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i)
    if (!std::isnan(d[i])) v.push_back(d[i]);
  std::sort(v.begin(), v.end());
  return v;
}

// pctile, _pctile and xtile parse "aweight fweight pweight"
// (pctile's and xtile's weight syntax)
// and mark the sample with the weight: a missing or zero weight leaves the
// sample, a negative one is r(402), a non-integer fweight r(401). `rowWeight`
// (when non-null) receives every row's weight, NaN where the row is out of the
// sample, so xtile can leave those rows missing.
struct WeightedValues {
  std::vector<std::pair<double, double>> sorted;  // (x, w) ascending in x
};

std::optional<WeightedValues> sortedWeightedValues(SessionState& session, const std::string& verb,
                                                   const std::string& var,
                                                   const std::string& weightVar,
                                                   const std::string& weightType, ojson* err,
                                                   std::vector<double>* rowX = nullptr,
                                                   std::vector<double>* rowWeight = nullptr) {
  ReadFrame rf(session, {var, weightVar});
  Datastore* ds = rf.get();
  if (ds == nullptr) {
    *err = rsError(verb + ": could not read the data" + (rf.why().empty() ? "" : " — " + rf.why()),
                   "data_io");
    return std::nullopt;
  }
  for (const std::string* name : {&var, &weightVar}) {
    if (!ds->has(*name)) {
      *err = rsError("variable " + *name + " not found");
      return std::nullopt;
    }
    if (ds->dtype(*name) == "factor") {
      *err = rsErrorRc(verb + ": `" + *name + "' is a string variable", 109);
      return std::nullopt;
    }
  }
  const std::string wt = lowerOf(weightType);
  const bool isFreq = wt == "fw" || wt == "fweight";
  const double* x = ds->col(var).f64();
  const double* w = ds->col(weightVar).f64();
  const int64_t n = ds->nRows();
  WeightedValues out;
  out.sorted.reserve(static_cast<size_t>(n));
  if (rowX) rowX->assign(static_cast<size_t>(n), std::numeric_limits<double>::quiet_NaN());
  if (rowWeight) rowWeight->assign(static_cast<size_t>(n), std::numeric_limits<double>::quiet_NaN());
  for (int64_t i = 0; i < n; ++i) {
    const double wi = w[i];
    if (std::isnan(wi)) continue;
    if (wi < 0.0) {
      *err = rsErrorRc("negative weights encountered", 402);
      return std::nullopt;
    }
    if (isFreq && wi != std::floor(wi)) {
      *err = rsErrorRc("may not use noninteger frequency weights", 401);
      return std::nullopt;
    }
    if (wi == 0.0 || std::isnan(x[i])) continue;
    out.sorted.emplace_back(x[i], wi);
    if (rowX) (*rowX)[static_cast<size_t>(i)] = x[i];
    if (rowWeight) (*rowWeight)[static_cast<size_t>(i)] = wi;
  }
  std::stable_sort(out.sorted.begin(), out.sorted.end(),
                   [](const std::pair<double, double>& a, const std::pair<double, double>& b) {
                     return a.first < b.first;
                   });
  return out;
}

bool hasWeight(const eval::ParsedCommand& parsed) {
  return parsed.weightType.has_value() && !parsed.weightType->empty() &&
         parsed.weightVar.has_value() && !parsed.weightVar->empty();
}

}  // namespace

ojson runPctile(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabPctile.known,
                                            kVocabPctile.unimplemented))
    return ojson(*err);
  // `if`/`in` is refused for the same reason `xtile` refuses it
  // (data_quantiles.cpp): with a qualifier Stata derives the cutpoints
  // from the RESTRICTED sample, and forwarding a --keepif would compute them
  // over every row and then blank the rest -- the same bins with a different
  // name, which is a wrong number, not a missing feature.
  if (auto q = refuseQualifiers(parsed, "pctile")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  const std::string joined = joinWith(parsed.varlist, " ");
  const size_t eq = joined.find('=');
  if (eq == std::string::npos)
    return rsSyntax("pctile: syntax is `pctile newvar = varname, nquantiles(#) | percentiles(list) "
                    "[genp(newvar2)]`");
  const std::string newvar = trimOf(joined.substr(0, eq));
  const std::string src = trimOf(joined.substr(eq + 1));
  if (newvar.empty() || src.empty())
    return rsSyntax("pctile: syntax is `pctile newvar = varname, nquantiles(#) | percentiles(list) "
                    "[genp(newvar2)]`");
  ojson guard = newVarGuard(session, "pctile", newvar);
  if (!guard.is_null()) return guard;
  // BUG-1120: `pctile _skip = x' is "_skip not allowed" r(101) (StataBE 19.5).
  if (newvar == "_skip" || newvar == "_all") return rsErrorRc(newvar + " not allowed", 101);
  const std::optional<std::string> genp = optStr(parsed.opts, "genp");
  if (genp.has_value()) {
    ojson g2 = newVarGuard(session, "pctile", *genp);
    if (!g2.is_null()) return g2;
    if (*genp == newvar)
      return rsSyntax("pctile: genp() must name a different variable from the one being generated");
  }

  ojson err;
  auto pcts = percentileList(parsed.opts, "pctile", 2, &err);
  if (!pcts.has_value()) return err;
  std::optional<std::vector<double>> sorted;
  std::optional<WeightedValues> weighted;
  if (hasWeight(parsed)) {
    weighted = sortedWeightedValues(session, "pctile", src, *parsed.weightVar, *parsed.weightType,
                                    &err);
    if (!weighted.has_value()) return err;
  } else {
    sorted = sortedValues(session, "pctile", src, &err);
    if (!sorted.has_value()) return err;
  }

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("pctile: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();

  // Row i holds percentile i; every row past the list is MISSING -- `.`, never
  // 0, which would read as a real percentile of zero.
  std::vector<std::string> cells(static_cast<size_t>(n));
  std::vector<std::string> pcells(static_cast<size_t>(n));
  for (size_t i = 0; i < pcts->size() && static_cast<int64_t>(i) < n; ++i) {
    cells[i] = numCell(weighted ? stataWeightedPercentile(weighted->sorted, (*pcts)[i])
                                : stataPercentile(*sorted, (*pcts)[i]));
    pcells[i] = numCell((*pcts)[i]);
  }

  std::string cur = *session.currentHbin();
  std::vector<std::string> newVars{newvar};
  std::string out;
  if (auto e2 = appendColumn(session, cur, newvar, cells, "f64", &out)) return *e2;
  cur = out;
  if (genp.has_value()) {
    if (auto e3 = appendColumn(session, cur, *genp, pcells, "f64", &out)) return *e3;
    cur = out;
    newVars.push_back(*genp);
  }
  session.load(cur);
  return mutationResult(session, "pctile", newVars);
}

ojson runPctileR(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabPctileR.known,
                                            kVocabPctileR.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "_pctile")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.size() != 1)
    return rsSyntax("_pctile: syntax is `_pctile varname, nquantiles(#) | percentiles(list)`");

  ojson err;
  auto pcts = percentileList(parsed.opts, "_pctile", 2, &err);
  if (!pcts.has_value()) return err;
  std::optional<std::vector<double>> sorted;
  std::optional<WeightedValues> weighted;
  if (hasWeight(parsed)) {
    weighted = sortedWeightedValues(session, "_pctile", parsed.varlist[0], *parsed.weightVar,
                                    *parsed.weightType, &err);
    if (!weighted.has_value()) return err;
  } else {
    sorted = sortedValues(session, "_pctile", parsed.varlist[0], &err);
    if (!sorted.has_value()) return err;
  }

  // r(r1)..r(rN) IS this command -- there is no column and no table. The arm
  // deliberately does not call captureRFromBlock (there is no block to capture
  // FROM), so the fill happens here or not at all (defect class 5). Stale
  // r(r#) from a longer previous call are cleared so a shorter list cannot be
  // read as if it were still long.
  for (int i = 1; i <= 1000; ++i) {
    const std::string k = "r" + std::to_string(i);
    if (!session.eval.rValues.contains(k)) break;
    session.eval.rValues.erase(k);
  }
  for (size_t i = 0; i < pcts->size(); ++i) {
    session.eval.rValues.set("r" + std::to_string(i + 1),
                             weighted ? stataWeightedPercentile(weighted->sorted, (*pcts)[i])
                                      : stataPercentile(*sorted, (*pcts)[i]));
  }
  // Deliberately NO r(N): Stata's `_pctile` stores r(r1)..r(rN) and nothing
  // else, and publishing an extra scalar would let a do-file read a number this
  // command does not promise.
  // Stata's `_pctile` prints nothing.
  return silent();
}

// xtile with [aw=]/[fw=]/[pw=]: xtile takes the cutpoints from
// `pctile ... [weight], n(#)` (it accepts aweight fweight pweight),
// so they are stataWeightedPercentile at 100k/nq. A row outside the weighted
// sample (missing or zero weight: `mark touse [wt]`) stays missing. The
// category is 1 + the number of cutpoints strictly below the value, which is
// xtile's numbering with duplicate cutpoints kept (the engine's
// xtile_assign rule, src/data/frame.cpp).
ojson runXtileWeighted(const std::string& newvar, const std::string& srcvar, int nq,
                       const std::string& weightVar, const std::string& weightType,
                       SessionState& session) {
  ojson err;
  std::vector<double> rowX, rowW;
  auto weighted =
      sortedWeightedValues(session, "xtile", srcvar, weightVar, weightType, &err, &rowX, &rowW);
  if (!weighted.has_value()) return err;
  if (weighted->sorted.empty()) return rsErrorRc("no observations", 2000);
  std::vector<double> cut;
  for (int k = 1; k < nq; ++k)
    cut.push_back(stataWeightedPercentile(weighted->sorted, 100.0 * k / nq));
  std::vector<std::string> cells(rowX.size());
  for (size_t i = 0; i < rowX.size(); ++i) {
    if (std::isnan(rowX[i]) || std::isnan(rowW[i])) continue;
    int g = 1;
    for (double c : cut)
      if (rowX[i] > c) ++g;
    cells[i] = numCell(static_cast<double>(g));
  }
  std::string out;
  if (auto e = appendColumn(session, *session.currentHbin(), newvar, cells, "f64", &out)) return *e;
  session.load(out);
  return mutationResult(session, "gen", {newvar});
}

}  // namespace piet::handlers
