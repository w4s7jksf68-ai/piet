// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/data_values.h"
#include "piet/handlers/session_commands.h"
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

// ===========================================================================
// drawnorm
// ===========================================================================

namespace {

// Dense lower Cholesky in place. false = not positive definite, which is a
// REFUSAL rather than a repaired matrix: drawing from a silently patched
// covariance answers a question the user did not ask.
bool cholesky(std::vector<std::vector<double>>& a) {
  const size_t k = a.size();
  std::vector<std::vector<double>> L(k, std::vector<double>(k, 0.0));
  for (size_t i = 0; i < k; ++i) {
    for (size_t j = 0; j <= i; ++j) {
      double s = a[i][j];
      for (size_t m = 0; m < j; ++m) s -= L[i][m] * L[j][m];
      if (i == j) {
        if (!(s > 0.0)) return false;
        L[i][j] = std::sqrt(s);
      } else {
        L[i][j] = s / L[j][j];
      }
    }
  }
  a = std::move(L);
  return true;
}

// A numeric option that may be a NUMLIST or the name of a session matrix.
// Returns nullopt with `*err` set when it is neither.
std::optional<std::vector<double>> numsOrMatrixRow(SessionState& session, const std::string& verb,
                                                   const std::string& optName,
                                                   const std::string& spec, ojson* err) {
  const std::vector<std::string> toks = splitWs(spec);
  std::vector<double> out;
  bool allNums = !toks.empty();
  for (const auto& t : toks) {
    double v = 0.0;
    if (!parseStrictDouble(t, &v)) {
      allNums = false;
      break;
    }
    out.push_back(v);
  }
  if (allNums) return out;
  if (toks.size() == 1) {
    if (const eval::EvalMatrix* m = session.eval.findMatrix(toks[0])) {
      out.clear();
      for (const auto& row : m->data)
        for (double v : row) out.push_back(v);
      return out;
    }
  }
  *err = rsSyntax(verb + ": " + optName + "(" + spec +
                  ") is neither a list of numbers nor the name of a matrix in memory");
  return std::nullopt;
}

}  // namespace

ojson runDrawnorm(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabDrawnorm.known,
                                            kVocabDrawnorm.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "drawnorm")) return *q;
  if (parsed.varlist.empty())
    return rsSyntax("drawnorm: syntax is `drawnorm newvarlist, n(#) [means() sds() corr()|cov() "
                    "seed() clear]`");
  const size_t k = parsed.varlist.size();
  for (const auto& nm : parsed.varlist)
    if (!strutil::isValidStataIdentifier(nm))
      return rsSyntax("drawnorm: invalid variable name `" + nm + "' — " +
                      strutil::stataIdentifierRuleText());

  if (hasFlag(parsed.opts, "clear")) {
    // `clear` here is Stata's ordinary `clear`: drop the data in memory before
    // drawing. Routed through runClear so this is ONE implementation of what
    // clearing means, not a second one that forgets a plane.
    ojson cleared = runClear("", session);
    if (isErrorBlock(cleared)) return cleared;
  }

  std::vector<double> means(k, 0.0);
  std::vector<double> sds(k, 1.0);
  if (auto m = optStr(parsed.opts, "means")) {
    ojson err;
    auto v = numsOrMatrixRow(session, "drawnorm", "means", *m, &err);
    if (!v.has_value()) return err;
    if (v->size() != k)
      return rsSyntax("drawnorm: means() has " + std::to_string(v->size()) + " element(s) for " +
                      std::to_string(k) + " variable(s)");
    means = *v;
  }
  if (auto s = optStr(parsed.opts, "sds")) {
    ojson err;
    auto v = numsOrMatrixRow(session, "drawnorm", "sds", *s, &err);
    if (!v.has_value()) return err;
    if (v->size() != k)
      return rsSyntax("drawnorm: sds() has " + std::to_string(v->size()) + " element(s) for " +
                      std::to_string(k) + " variable(s)");
    for (double x : *v)
      if (!(x >= 0.0)) return rsSyntax("drawnorm: sds() must be non-negative");
    sds = *v;
  }

  const std::optional<std::string> corrOpt = optStr(parsed.opts, "corr");
  const std::optional<std::string> covOpt = optStr(parsed.opts, "cov");
  if (corrOpt.has_value() && covOpt.has_value())
    return rsSyntax("drawnorm: corr() and cov() cannot both be specified");
  if (covOpt.has_value() && (optStr(parsed.opts, "sds").has_value()))
    return rsSyntax("drawnorm: cov() already carries the scale; sds() cannot be combined with it");

  // Sigma: cov() outright, or diag(sds) * corr * diag(sds). Default corr = I.
  std::vector<std::vector<double>> sigma(k, std::vector<double>(k, 0.0));
  auto squareFrom = [&](const std::string& optName, const std::string& spec,
                        std::vector<std::vector<double>>* outM, ojson* err) -> bool {
    if (const eval::EvalMatrix* m = session.eval.findMatrix(spec)) {
      if (m->data.size() != k)
        return (*err = rsSyntax("drawnorm: matrix " + spec + " is " +
                                std::to_string(m->data.size()) + " rows for " + std::to_string(k) +
                                " variable(s)"),
                false);
      for (size_t i = 0; i < k; ++i) {
        if (m->data[i].size() != k)
          return (*err = rsSyntax("drawnorm: matrix " + spec + " is not square"), false);
        (*outM)[i] = m->data[i];
      }
      return true;
    }
    // A flat numlist: either k*k values (full matrix, row-major) or the lower
    // triangle Stata's own `corr(1 \ .5 1)` spelling writes.
    std::vector<std::string> toks;
    for (const auto& t : splitWs(spec))
      if (t != "\\") toks.push_back(t);
    std::vector<double> vals;
    for (const auto& t : toks) {
      double v = 0.0;
      if (!parseStrictDouble(t, &v)) {
        *err = rsSyntax("drawnorm: " + optName + "(" + spec +
                        ") is neither a matrix in memory nor a list of numbers");
        return false;
      }
      vals.push_back(v);
    }
    if (vals.size() == k * k) {
      for (size_t i = 0; i < k; ++i)
        for (size_t j = 0; j < k; ++j) (*outM)[i][j] = vals[i * k + j];
      return true;
    }
    if (vals.size() == k * (k + 1) / 2) {
      size_t p = 0;
      for (size_t i = 0; i < k; ++i)
        for (size_t j = 0; j <= i; ++j) {
          (*outM)[i][j] = vals[p];
          (*outM)[j][i] = vals[p];
          ++p;
        }
      return true;
    }
    *err = rsSyntax("drawnorm: " + optName + "() has " + std::to_string(vals.size()) +
                    " element(s); " + std::to_string(k * k) + " (full) or " +
                    std::to_string(k * (k + 1) / 2) + " (lower triangle) expected");
    return false;
  };

  if (covOpt.has_value()) {
    ojson err;
    if (!squareFrom("cov", *covOpt, &sigma, &err)) return err;
  } else {
    std::vector<std::vector<double>> corr(k, std::vector<double>(k, 0.0));
    for (size_t i = 0; i < k; ++i) corr[i][i] = 1.0;
    if (corrOpt.has_value()) {
      ojson err;
      if (!squareFrom("corr", *corrOpt, &corr, &err)) return err;
    }
    for (size_t i = 0; i < k; ++i)
      for (size_t j = 0; j < k; ++j) sigma[i][j] = sds[i] * corr[i][j] * sds[j];
  }

  std::vector<std::vector<double>> L = sigma;
  if (!cholesky(L))
    return rsError("drawnorm: correlation matrix is not positive definite");

  const uint64_t seed = drawSeed(session, optStr(parsed.opts, "seed"));

  int64_t n = 0;
  Datastore* ds = nullptr;
  if (session.hasData()) {
    try {
      ds = session.datastore();
    } catch (const std::exception& e) {
      return rsError(std::string("drawnorm: ") + e.what(), "data_io");
    }
  }
  if (ds != nullptr) {
    n = ds->nRows();
    if (auto v = optStr(parsed.opts, "n")) {
      long long want = 0;
      if (!parseStrictInt(*v, &want))
        return rsSyntax("drawnorm: n() requires a whole number, got `" + *v + "'");
      if (want != n)
        return rsError("drawnorm: n(" + *v + ") disagrees with the " + std::to_string(n) +
                       " observation(s) already in memory; drop the data first (`clear`) or omit "
                       "n().");
    }
    for (const auto& nm : parsed.varlist) {
      ojson guard = newVarGuard(session, "drawnorm", nm);
      if (!guard.is_null()) return guard;
      // BUG-1120: `drawnorm _all' is "_all not allowed" r(101) (StataBE 19.5).
      if (nm == "_skip" || nm == "_all") return rsErrorRc(nm + " not allowed", 101);
    }
  } else {
    auto v = optStr(parsed.opts, "n");
    if (!v.has_value())
      return rsSyntax("drawnorm: with no data in memory, n(#) is required");
    long long want = 0;
    if (!parseStrictInt(*v, &want) || want <= 0)
      return rsSyntax("drawnorm: n() requires a positive whole number, got `" + *v + "'");
    n = want;
  }

  // x = mu + L z, with z_i drawn from the (seed, variable, row) counter. Two
  // draws of the same command give the same numbers, and the value of variable
  // i at row j never depends on how many draws came before it.
  std::vector<std::vector<double>> draws(static_cast<size_t>(n), std::vector<double>(k, 0.0));
  for (int64_t r = 0; r < n; ++r) {
    std::vector<double> z(k);
    for (size_t i = 0; i < k; ++i)
      z[i] = cecon::normal_icdf(
          cecon::uniform01(seed, static_cast<uint64_t>(i), static_cast<uint64_t>(r)));
    for (size_t i = 0; i < k; ++i) {
      double acc = means[i];
      for (size_t j = 0; j <= i; ++j) acc += L[i][j] * z[j];
      draws[static_cast<size_t>(r)][i] = acc;
    }
  }

  if (ds == nullptr) {
    CsvTable t;
    for (const auto& nm : parsed.varlist) {
      t.names.push_back(nm);
      t.isFactor.push_back(0);
    }
    t.rows.reserve(static_cast<size_t>(n));
    for (int64_t r = 0; r < n; ++r) {
      std::vector<std::string> row;
      row.reserve(k);
      for (size_t i = 0; i < k; ++i) row.push_back(numCell(draws[static_cast<size_t>(r)][i]));
      t.rows.push_back(std::move(row));
    }
    std::string outPath;
    if (auto err = writeAndConvert(session, t, "drawnorm", &outPath)) return *err;
    session.load(outPath);
    if (auto bad = assertRowCount(session, "drawnorm", n)) return *bad;
    return mutationResult(session, "drawnorm", parsed.varlist);
  }

  std::string cur = *session.currentHbin();
  for (size_t i = 0; i < k; ++i) {
    std::vector<std::string> cells;
    cells.reserve(static_cast<size_t>(n));
    for (int64_t r = 0; r < n; ++r) cells.push_back(numCell(draws[static_cast<size_t>(r)][i]));
    std::string out;
    if (auto err = appendColumn(session, cur, parsed.varlist[i], cells, "f64", &out)) return *err;
    cur = out;
  }
  session.load(cur);
  return mutationResult(session, "drawnorm", parsed.varlist);
}

}  // namespace piet::handlers
