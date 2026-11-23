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
#include "piet/loops.h"  // expandNumlist: percentiles() takes a numlist
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

// The `altdef` rule, [D] pctile "Methods and formulas" (d.pdf p.776): with
// x(1..n) ascending, i = floor((n+1)p/100) and h = (n+1)p/100 - i, the p-th
// percentile is (1-h) x(i) + h x(i+1), where x(0) is x(1) and x(n+1) is x(n).
// Unweighted only: Stata refuses altdef with weights (r(198)).
//
// `sorted` must be ascending and free of missing.
double stataAltdefPercentile(const std::vector<double>& sorted, double p) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
  const int64_t n = static_cast<int64_t>(sorted.size());
  if (n == 0) return std::numeric_limits<double>::quiet_NaN();
  const double P = static_cast<double>(n + 1) * p / 100.0;
  const double fl = std::floor(P);
  const double h = P - fl;
  const auto at = [&](int64_t j) {
    if (j < 1) j = 1;
    if (j > n) j = n;
    return sorted[static_cast<size_t>(j - 1)];
  };
  const int64_t i = static_cast<int64_t>(fl);
  return (1.0 - h) * at(i) + h * at(i + 1);
}

namespace {

// pctile/xtile's `[type] newvar` (`syntax newvarname`): an optional storage type
// before the name, spelled as generate reads one (case matters: `Byte` is a
// name). The type word goes to *type ("" when absent); the name is left in
// *name for the caller's own checks.
void splitTypedNewvar(const std::string& typed, std::string* type, std::string* name) {
  std::vector<std::string> words = splitWs(typed);
  type->clear();
  if (words.size() == 2 && strutil::isStorageTypeWord(words[0])) {
    *type = words[0];
    *name = words[1];
    return;
  }
  *name = trimOf(typed);
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
    // percentiles() takes a numlist: comma- or space-separated numbers and the
    // `a(s)b` / `a/b` ranges (StataBE 19.5: p(10(10)90) is nine percentiles).
    // The ranges are expanded by the shared numlist expander forvalues uses.
    std::string spec = *pOpt;
    for (char& c : spec)
      if (c == ',') c = ' ';
    std::vector<std::string> elements;
    if (!loops::expandNumlist(spec, &elements)) {
      *err = rsErrorRc(verb + ": invalid numlist in percentiles(): `" + *pOpt + "'", 121);
      return std::nullopt;
    }
    for (const auto& t : elements) {
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

// pctile's and _pctile's sample, as their `mark touse [wt] if in` + `markout touse x` draw it:
// the expression `src` evaluated by generate (so a misspelt name or a string answers as
// generate does: r(111), r(109) "type mismatch"), on the rows if/in keep, the weight
// validated on those rows only (a missing or zero weight leaves the sample, a negative one is
// r(402), a fractional fweight r(401)), and a missing value of `src` leaving the sample.
// Exactly one of *sorted / *weighted is filled.
std::optional<ojson> pctileSampleValues(SessionState& session, const eval::ParsedCommand& parsed,
                                        const std::string& src,
                                        std::optional<std::vector<double>>* sorted,
                                        std::optional<WeightedValues>* weighted) {
  std::string qualifier;
  if (parsed.ifExpr.has_value() && !trimOf(*parsed.ifExpr).empty())
    qualifier += " if " + *parsed.ifExpr;
  if (parsed.inRange.has_value() && !trimOf(*parsed.inRange).empty())
    qualifier += " in " + *parsed.inRange;
  ScratchColumns scratch(session);
  std::string xCol;
  if (auto failed = scratch.add(src, qualifier, &xCol)) return *failed;
  std::string mCol;
  if (!qualifier.empty())
    if (auto failed = scratch.add("1", qualifier, &mCol)) return *failed;
  const bool isWeighted = hasWeight(parsed);
  std::string wCol;
  if (isWeighted)
    if (auto failed = scratch.add(*parsed.weightVar, "", &wCol)) return *failed;
  const std::vector<double> x = scratch.values(xCol);
  const std::vector<double> mark =
      mCol.empty() ? std::vector<double>(x.size(), 1.0) : scratch.values(mCol);
  if (!isWeighted) {
    std::vector<double> v;
    v.reserve(x.size());
    for (size_t i = 0; i < x.size(); ++i)
      if (!std::isnan(mark[i]) && !std::isnan(x[i])) v.push_back(x[i]);
    std::sort(v.begin(), v.end());
    *sorted = std::move(v);
    return std::nullopt;
  }
  const std::vector<double> w = scratch.values(wCol);
  const std::string wt = lowerOf(*parsed.weightType);
  const bool isFreq = wt == "fw" || wt == "fweight";
  WeightedValues out;
  for (size_t i = 0; i < x.size(); ++i) {
    if (std::isnan(mark[i]) || std::isnan(w[i])) continue;
    if (w[i] < 0.0) return rsErrorRc("negative weights encountered", 402);
    if (isFreq && w[i] != std::floor(w[i]))
      return rsErrorRc("may not use noninteger frequency weights", 401);
    if (w[i] == 0.0 || std::isnan(x[i])) continue;
    out.sorted.emplace_back(x[i], w[i]);
  }
  std::stable_sort(out.sorted.begin(), out.sorted.end(),
                   [](const std::pair<double, double>& a, const std::pair<double, double>& b) {
                     return a.first < b.first;
                   });
  *weighted = std::move(out);
  return std::nullopt;
}

}  // namespace

ojson runPctile(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabPctile.known,
                                            kVocabPctile.unimplemented))
    return ojson(*err);
  // `if`/`in` restrict the sample the percentiles are taken over (pctile.ado's
  // `mark`/`markout`); the new variable still runs over every row, holding
  // percentile i in row i.
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  const std::string joined = joinWith(parsed.varlist, " ");
  const size_t eq = joined.find('=');
  if (eq == std::string::npos)
    return rsSyntax("pctile: syntax is `pctile [type] newvar = varname, nquantiles(#) "
                    "[genp(newvar2) altdef]`");
  // `pctile [type] newvar = exp` (pctile.ado: `syntax newvarname(gen) =/exp`):
  // the new variable is created as the declared type, else c(type), and each
  // percentile is stored into it (`replace q = r(r#)`), so a float result
  // holds the float-rounded percentile.
  std::string typeToken, newvar;
  splitTypedNewvar(joined.substr(0, eq), &typeToken, &newvar);
  const std::string src = trimOf(joined.substr(eq + 1));
  if (newvar.empty() || src.empty())
    return rsSyntax("pctile: syntax is `pctile [type] newvar = varname, nquantiles(#) "
                    "[genp(newvar2) altdef]`");
  ojson guard = newVarGuard(session, "pctile", newvar);
  if (!guard.is_null()) return guard;
  // BUG-1120: `pctile _skip = x' is "_skip not allowed" r(101) (StataBE 19.5).
  if (newvar == "_skip" || newvar == "_all") return rsErrorRc(newvar + " not allowed", 101);
  const bool altdef = hasFlag(parsed.opts, "altdef");
  // pctile.ado's first check after syntax.
  if (altdef && hasWeight(parsed))
    return rsErrorRc("altdef option cannot be used with weights", 198);
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
  // `pctile newvar = exp`: exp may be any expression (pctile.ado's `syntax ... =/exp`).
  if (auto failed = pctileSampleValues(session, parsed, src, &sorted, &weighted)) return *failed;
  // `count if touse` is 0: r(2000) (bank a625f5f37472fbb9 `if price>999999`).
  if ((sorted.has_value() && sorted->empty()) || (weighted.has_value() && weighted->sorted.empty()))
    return rsErrorRc("no observations", 2000);
  // A str# declaration creates a string variable the percentiles cannot be
  // stored into (`replace q = r(r1)`: type mismatch).
  if (!typeToken.empty() && !storage::isNumericType(typeToken))
    return rsErrorRc("type mismatch", 109);
  const std::string storeType = storage::resolveCreateType(session, typeToken);
  const auto storeKind = [](const std::string& t) {
    if (t == "byte") return cecon::StoreType::Byte;
    if (t == "int") return cecon::StoreType::Int;
    if (t == "long") return cecon::StoreType::Long;
    if (t == "float") return cecon::StoreType::Float;
    return cecon::StoreType::Double;
  };

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("pctile: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();
  // pctile.ado: nquantiles() above _N + 1 is r(198) "nquantiles() must be less
  // than or equal to number of observations plus one" (vs-stata bank
  // 63438804235153fa `pctile pc = price, nq(100)` on 74 observations).
  if (!optStrAny(parsed.opts, {"percentiles", "p"}).has_value() &&
      static_cast<int64_t>(pcts->size()) + 1 > n + 1)
    return rsErrorRc("nquantiles() must be less than or equal to number of observations plus one",
                     198);

  // Row i holds percentile i; every row past the list is MISSING -- `.`, never
  // 0, which would read as a real percentile of zero.
  // genp() is `gen float genp = 100*_n/nq` (pctile.ado): float-rounded.
  std::vector<std::string> cells(static_cast<size_t>(n));
  std::vector<std::string> pcells(static_cast<size_t>(n));
  std::vector<double> values;
  for (size_t i = 0; i < pcts->size(); ++i)
    values.push_back(weighted ? stataWeightedPercentile(weighted->sorted, (*pcts)[i])
                     : altdef ? stataAltdefPercentile(*sorted, (*pcts)[i])
                              : stataPercentile(*sorted, (*pcts)[i]));
  // pctile.ado fills the variable one `replace q = r(r#) in #` at a time, and
  // `replace` PROMOTES an integer type that cannot hold the value (byte/int ->
  // float on a fraction, long -> double; byte -> int -> long on overflow:
  // storage::promotedType, the lattice `replace` itself uses). So a declared
  // `byte`/`int`/`long` ends as the narrowest type that took every value, each
  // value stored at the type in force when it arrived.
  std::string finalType = storeType;
  for (size_t i = 0; i < pcts->size() && static_cast<int64_t>(i) < n; ++i) {
    const double v = values[i];
    if (!std::isnan(v) && storage::isIntegerType(finalType))
      finalType = storage::promotedType(finalType, v != std::floor(v), v, v);
    cells[i] = numCell(cecon::frame_detail::round_to_store(v, storeKind(finalType)));
    pcells[i] = numCell(cecon::frame_detail::round_to_store((*pcts)[i], cecon::StoreType::Float));
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
  // pctile is not r-class: the r() it leaves is its last internal `_pctile`'s,
  // r(r1)..r(r#) unrounded. Up to nq(1001) that is one call; past it pctile.ado
  // batches the percentiles (its `j` counter): the first call takes 1,000, each
  // later one 999 (j restarts at 1 and is incremented before the next value),
  // and the last batch is what r() holds.
  eval::replaceRClass(session.eval);
  {
    const size_t total = values.size();
    const size_t first = total <= 1000 ? 0 : 1000 + 999 * ((total - 1000 - 1) / 999);
    std::vector<std::string> posted;
    for (size_t i = first; i < total; ++i) {
      posted.push_back("r" + std::to_string(i - first + 1));
      session.eval.rValues.set(posted.back(), values[i]);
    }
    // The whole set, so a read of an r(r#) past it is `.` as in Stata.
    eval::declareRPosted(session.eval, "pctile", posted);
  }
  storage::recordCreateType(session, newvar, typeToken, finalType);
  session.setVarLabel(newvar, "percentiles of " + src);
  if (genp.has_value()) storage::recordCreateType(session, *genp, "float", "float");
  return mutationResult(session, "pctile", newVars);
}

ojson runPctileR(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabPctileR.known,
                                            kVocabPctileR.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  // `_pctile varname`: a second variable is r(103) (bank 59f3e88f7f8556e4).
  if (parsed.varlist.size() > 1) return rsErrorRc("too many variables specified", 103);
  if (parsed.varlist.size() != 1)
    return rsSyntax("_pctile: syntax is `_pctile varname, nquantiles(#) | percentiles(list)`");

  const bool altdef = hasFlag(parsed.opts, "altdef");
  if (altdef && hasWeight(parsed))
    return rsErrorRc("altdef option cannot be used with weights", 198);
  ojson err;
  auto pcts = percentileList(parsed.opts, "_pctile", 2, &err);
  if (!pcts.has_value()) return err;
  // A string variable has no observation _pctile can use: StataBE 19.5 prints
  // "no observations", leaves r() empty and exits 0 (`_pctile make, nq(4)` on
  // auto, rc 0, `return list` empty) -- not the r(2000) pctile/xtile give.
  if (!hasWeight(parsed)) {
    ReadFrame rf(session, {parsed.varlist[0]});
    Datastore* ds = rf.get();
    if (ds != nullptr && ds->has(parsed.varlist[0]) && ds->dtype(parsed.varlist[0]) == "factor") {
      eval::replaceRClass(session.eval);
      return displayBlock("no observations");
    }
  }
  std::optional<std::vector<double>> sorted;
  std::optional<WeightedValues> weighted;
  {
    // The variable must exist under its own name (a varname, not an expression).
    const std::string& name = parsed.varlist[0];
    for (char c : name)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
            static_cast<unsigned char>(c) >= 0x80))
        return rsErrorRc("variable " + name + " not found", 111);
    if (auto failed = pctileSampleValues(session, parsed, name, &sorted, &weighted))
      return *failed;
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
  // Posted rN first and r1 last, so `: r(scalars)` and `return list` (newest
  // first) list r1 r2 ... rN as Stata does (vs-stata truth bank).
  std::vector<std::string> posted;
  for (size_t i = pcts->size(); i-- > 0;) {
    posted.push_back("r" + std::to_string(i + 1));
    session.eval.rValues.set(posted.back(),
                             weighted ? stataWeightedPercentile(weighted->sorted, (*pcts)[i])
                             : altdef ? stataAltdefPercentile(*sorted, (*pcts)[i])
                                      : stataPercentile(*sorted, (*pcts)[i]));
  }
  eval::declareRPosted(session.eval, "_pctile", posted);
  // Deliberately NO r(N): Stata's `_pctile` stores r(r1)..r(rN) and nothing
  // else, and publishing an extra scalar would let a do-file read a number this
  // command does not promise.
  // Stata's `_pctile` prints nothing.
  return silent();
}

long long xtileDistinctCutpoints(const std::vector<double>& values,
                                 const std::vector<double>* weights, long long nq, bool altdef) {
  std::vector<double> sorted;
  std::vector<std::pair<double, double>> pairs;
  for (size_t row = 0; row < values.size(); ++row) {
    if (std::isnan(values[row])) continue;
    if (weights != nullptr) {
      const double w = (*weights)[row];
      if (std::isnan(w) || !(w > 0.0)) continue;
      pairs.emplace_back(values[row], w);
    } else {
      sorted.push_back(values[row]);
    }
  }
  std::sort(sorted.begin(), sorted.end());
  std::stable_sort(pairs.begin(), pairs.end(),
                   [](const std::pair<double, double>& a, const std::pair<double, double>& b) {
                     return a.first < b.first;
                   });
  std::set<double> cuts;
  for (long long k = 1; k < nq; ++k) {
    const double p = 100.0 * static_cast<double>(k) / static_cast<double>(nq);
    const double c = weights != nullptr ? stataWeightedPercentile(pairs, p)
                     : altdef           ? stataAltdefPercentile(sorted, p)
                                        : stataPercentile(sorted, p);
    if (!std::isnan(c)) cuts.insert(c);
  }
  return static_cast<long long>(cuts.size());
}

std::optional<ojson> storeXtileAltdef(SessionState& session, const std::string& target,
                                      const std::vector<double>& values, long long nq) {
  std::vector<double> sorted;
  for (double v : values)
    if (!std::isnan(v)) sorted.push_back(v);
  std::sort(sorted.begin(), sorted.end());
  std::vector<double> cut;
  for (long long k = 1; k < nq; ++k)
    cut.push_back(stataAltdefPercentile(sorted, 100.0 * static_cast<double>(k) /
                                                    static_cast<double>(nq)));
  std::vector<std::string> cells(values.size());
  for (size_t row = 0; row < values.size(); ++row) {
    if (std::isnan(values[row])) continue;
    long long category = 1;
    for (double c : cut)
      if (values[row] > c) ++category;
    cells[row] = std::to_string(category);
  }
  (void)session.datastore();  // the head file appendColumn extends
  std::string written;
  if (auto failed = appendColumn(session, *session.currentHbin(), target, cells, "f64", &written))
    return *failed;
  session.load(written);
  return std::nullopt;
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
