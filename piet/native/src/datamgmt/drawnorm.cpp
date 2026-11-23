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
#include <functional>
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
#include "matrix/lapack_dsyevr.h"  // _checkpd's spectral decomposition
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
  // The new-variable list expands `x1-x3` to x1 x2 x3 (a shared stem), as
  // syntax's newvarlist does (vs-stata bank `drawnorm x1-x3, n(3) clear`).
  std::vector<std::string> newNames;
  {
    static const std::regex reRange(R"(^([A-Za-z_]+)([0-9]+)-([A-Za-z_]+)([0-9]+)$)");
    for (const auto& t : parsed.varlist) {
      std::smatch rm;
      if (std::regex_match(t, rm, reRange) && rm.str(1) == rm.str(3) &&
          std::atoll(rm.str(2).c_str()) <= std::atoll(rm.str(4).c_str())) {
        for (long long a = std::atoll(rm.str(2).c_str()); a <= std::atoll(rm.str(4).c_str()); ++a)
          newNames.push_back(rm.str(1) + std::to_string(a));
      } else {
        newNames.push_back(t);
      }
    }
  }
  const std::vector<std::string>& names = newNames;
  const size_t k = names.size();
  for (const auto& nm : names)
    if (!strutil::isValidStataIdentifier(nm))
      return rsSyntax("drawnorm: invalid variable name `" + nm + "' — " +
                      strutil::stataIdentifierRuleText());

  // What drawnorm.ado leaves in r() before it does anything to the data: `quietly count` (r(N)),
  // or, when n() asks for a different number of observations than the data in memory hold,
  // `quietly describe, short` of those data (vs-stata bank). cov() and corr() go on to
  // _checkpd, whose own results replace these, so they are left alone.
  struct RSnapshot {
    std::vector<std::pair<std::string, double>> values;
    std::vector<std::pair<std::string, std::string>> strings;
    std::vector<std::string> names;
  };
  std::optional<RSnapshot> rResidue;
  if (session.hasData() && !optStr(parsed.opts, "cov").has_value() &&
      !optStr(parsed.opts, "corr").has_value()) {
    Datastore* d0 = nullptr;
    try {
      d0 = session.datastore();
    } catch (const std::exception&) {
    }
    long long want = 0;
    const auto nOpt = optStr(parsed.opts, "n");
    const bool differs = d0 != nullptr && nOpt.has_value() && parseStrictInt(*nOpt, &want) &&
                         want != d0->nRows();
    const std::string savedLeaf = session.leafCmdExpanded;
    (void)dispatch(differs ? "quietly describe, short" : "quietly count", session);
    session.leafCmdExpanded = savedLeaf;
    RSnapshot snap;
    for (const auto& kv : session.eval.rValues.items()) {
      snap.values.push_back(kv);
      snap.names.push_back(kv.first);
    }
    for (const auto& kv : session.eval.rStrings.items())
      if (!eval::isReservedStoreKey(kv.first)) {
        snap.strings.push_back(kv);
        snap.names.push_back(kv.first);
      }
    rResidue = std::move(snap);
  }
  std::function<void()> publishCheckPd;  // set once the spectral decomposition has run
  auto restoreResidue = [&]() {
    if (publishCheckPd) {
      publishCheckPd();
      return;
    }
    if (!rResidue.has_value()) return;
    eval::replaceRClass(session.eval);
    for (const auto& [k, v] : rResidue->values) session.eval.rValues.set(k, v);
    for (const auto& [k, v] : rResidue->strings) session.eval.rStrings.set(k, v);
    for (const char* h : {"N_max", "k_max", "widthmax"})
      if (session.eval.rValues.contains(h)) session.eval.markRHidden(h);
    eval::declareRPosted(session.eval, "drawnorm", rResidue->names);
  };
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
  // cstorage(): how a matrix given to cov() / corr() is laid out. full (the default) is the
  // whole k x k matrix; lower / upper is a row or column vector of the triangle, row by row.
  std::string cstorage = "full";
  if (auto cs = optStr(parsed.opts, "cstorage")) {
    const std::string w = strutil::lower(strutil::trim(*cs));
    if (!w.empty() && std::string("full").compare(0, w.size(), w) == 0) cstorage = "full";
    else if (!w.empty() && std::string("lower").compare(0, w.size(), w) == 0) cstorage = "lower";
    else if (!w.empty() && std::string("upper").compare(0, w.size(), w) == 0) cstorage = "upper";
    else return rsSyntax("drawnorm: cstorage(" + *cs + ") invalid");
  }
  // cstorage_given: the option was typed (an empty or absent option reads as none).
  const bool cstorageGiven = optStr(parsed.opts, "cstorage").has_value() &&
                             !strutil::trim(*optStr(parsed.opts, "cstorage")).empty();
  auto squareFrom = [&](const std::string& optName, const std::string& spec,
                        std::vector<std::vector<double>>* outM, ojson* err) -> bool {
    // The matrix as given: a matrix in memory, or a literal read the way `matrix input` reads
    // it (rows split at a backslash, entries by blanks or commas), so a literal is a 1 x n row
    // vector unless it has backslashes.
    std::vector<std::vector<double>> given;
    if (const eval::EvalMatrix* m = session.eval.findMatrix(spec)) {
      given = m->data;
    } else {
      std::string rowText;
      std::vector<std::string> rows;
      for (char ch : spec) {
        if (ch == '\\') {
          rows.push_back(rowText);
          rowText.clear();
        } else {
          rowText += ch;
        }
      }
      rows.push_back(rowText);
      for (const auto& r : rows) {
        std::string spaced = r;
        for (char& ch : spaced)
          if (ch == ',') ch = ' ';
        std::vector<double> row;
        for (const auto& t : splitWs(spaced)) {
          double v = 0.0;
          if (!t.empty() && t[0] == '.' && (t.size() == 1 || std::isalpha(static_cast<unsigned char>(t[1])))) {
            v = std::nan("");
          } else if (!parseStrictDouble(t, &v)) {
            *err = rsSyntax("drawnorm: " + optName + "(" + spec + ") invalid");
            return false;
          }
          row.push_back(v);
        }
        if (row.empty() || (!given.empty() && row.size() != given[0].size())) {
          *err = rsSyntax("drawnorm: " + optName + "(" + spec + ") invalid");
          return false;
        }
        given.push_back(std::move(row));
      }
    }
    const size_t nr = given.size();
    const size_t nc = given.empty() ? 0 : given[0].size();
    for (const auto& row : given)
      for (double v : row)
        if (std::isnan(v)) {
          *err = rsErrorRc(optName + "(" + spec + ") has missing values", 504);
          return false;
        }
    if (k == 1) {
      if (nr != 1 || nc != 1) {
        *err = rsErrorRc("matrix " + spec + " should be a row or column vector", 503);
        return false;
      }
      (*outM)[0][0] = given[0][0];
      return true;
    }
    if (nr == k && nc == k) {
      if (cstorageGiven && cstorage != "full") {
        *err = rsSyntax("drawnorm: invalid cstorage; matrix is found to be square");
        return false;
      }
      for (size_t i = 0; i < k; ++i) (*outM)[i] = given[i];
      return true;
    }
    if (nr == 1 || nc == 1) {
      if (!cstorageGiven) {
        *err = rsSyntax("drawnorm: option cstorage() required with vectorized corr/cov matrix");
        return false;
      }
      if (cstorage == "full") {
        *err = rsSyntax("drawnorm: option cstorage() invalid; vectorized corr/cov matrix found");
        return false;
      }
      std::vector<double> flat;
      for (const auto& row : given)
        for (double v : row) flat.push_back(v);
      if (flat.size() != k * (k + 1) / 2) {
        *err = rsErrorRc("size of vector " + spec + " invalid; " + std::to_string(k * (k + 1) / 2) +
                             "-vector expected",
                         503);
        return false;
      }
      size_t q = 0;
      for (size_t i = 0; i < k; ++i)
        for (size_t j = (cstorage == "lower" ? 0 : i); j < (cstorage == "lower" ? i + 1 : k); ++j) {
          (*outM)[i][j] = flat[q];
          (*outM)[j][i] = flat[q];
          ++q;
        }
      return true;
    }
    *err = rsErrorRc("matrix " + spec + " is " + std::to_string(nr) + "x" + std::to_string(nc) +
                         ", not " + std::to_string(k) + "x" + std::to_string(k),
                     503);
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

  // cov() / corr(): _checkpd's spectral decomposition of the matrix as given, whose results
  // (npos, nneg, nnzero, npzero, L, Ev, C) stay in r() when the command ends.
  struct CheckPd {
    double nneg = 0, nnzero = 0, npzero = 0, npos = 0;
    std::vector<double> ev;                    // descending, as symeigen returns them
    std::vector<std::vector<double>> vec;      // vec[i][j]: component i of eigenvector j
    std::vector<std::vector<double>> c;        // C, or the PSD repair of it
  };
  std::optional<CheckPd> checkPd;
  const bool forcePsd = hasFlag(parsed.opts, "forcepsd");
  std::vector<std::vector<double>> Lfac;  // the factor the draws use: x = mean + Lfac z
  if (covOpt.has_value() || corrOpt.has_value()) {
    CheckPd cp;
    std::vector<std::vector<double>> given(k, std::vector<double>(k, 0.0));
    {
      ojson e2;
      if (covOpt.has_value()) {
        if (!squareFrom("cov", *covOpt, &given, &e2)) return e2;
      } else if (!squareFrom("corr", *corrOpt, &given, &e2)) {
        return e2;
      }
    }
    std::vector<double> a(k * k), w, z;
    for (size_t i = 0; i < k; ++i)
      for (size_t j = 0; j < k; ++j) a[i + j * k] = given[i][j];
    if (piet::lapack::dsyevrVAL(static_cast<int>(k), a, 0.0, w, z) != 0)
      return rsError("drawnorm: the spectral decomposition of the matrix failed");
    cp.ev.assign(w.rbegin(), w.rend());
    cp.vec.assign(k, std::vector<double>(k, 0.0));
    for (size_t j = 0; j < k; ++j)
      for (size_t i = 0; i < k; ++i) cp.vec[i][j] = z[i + (k - 1 - j) * k];
    const double tol = std::fabs(1e-8 * cp.ev[0]);
    for (double e : cp.ev) {
      if (e < -tol) cp.nneg += 1;
      else if (e < 0) cp.nnzero += 1;
      else if (e <= tol) cp.npzero += 1;
    }
    cp.npos = static_cast<double>(k) - cp.nneg - cp.nnzero - cp.npzero;
    if (!forcePsd && cp.nneg > 0)
      return rsErrorRc((covOpt.has_value() ? *covOpt : *corrOpt) + " not positive (semi)definite", 506);
    cp.c = given;
    if (forcePsd && cp.nneg + cp.nnzero > 0) {
      for (double& e : cp.ev) e = std::max(0.0, e);
      for (size_t i = 0; i < k; ++i)
        for (size_t j = 0; j < k; ++j) {
          double acc = 0.0;
          for (size_t m = 0; m < k; ++m) acc += cp.vec[i][m] * cp.ev[m] * cp.vec[j][m];
          cp.c[i][j] = acc;
        }
    }
    if (cp.npos == static_cast<double>(k)) {
      Lfac = sigma;  // positive definite: the Cholesky factor of the covariance, as before
      if (!cholesky(Lfac)) return rsError("drawnorm: correlation matrix is not positive definite");
    } else {
      // semidefinite (forcepsd repaired or singular): x = mean + V sqrt(Ev) z
      Lfac.assign(k, std::vector<double>(k, 0.0));
      for (size_t i = 0; i < k; ++i)
        for (size_t j = 0; j < k; ++j) Lfac[i][j] = cp.vec[i][j] * std::sqrt(std::max(0.0, cp.ev[j]));
      if (corrOpt.has_value())  // sds scale each variable
        for (size_t i = 0; i < k; ++i)
          for (size_t j = 0; j < k; ++j) Lfac[i][j] *= sds[i];
    }
    checkPd = std::move(cp);
  } else {
    Lfac = sigma;
    if (!cholesky(Lfac))
      return rsError("drawnorm: correlation matrix is not positive definite");
  }
  const std::vector<std::vector<double>>& L = Lfac;

  if (checkPd.has_value()) {
    publishCheckPd = [&session, &checkPd, k]() {
      eval::replaceRClass(session.eval);
      const CheckPd& cp = *checkPd;
      session.eval.rValues.set("nneg", cp.nneg);
      session.eval.rValues.set("nnzero", cp.nnzero);
      session.eval.rValues.set("npzero", cp.npzero);
      session.eval.rValues.set("npos", cp.npos);
      auto put = [&](const char* name, const std::vector<std::vector<double>>& data,
                     const char* colStem) {
        eval::EvalMatrix m;
        m.data = data;
        for (size_t i = 0; i < data.size(); ++i) m.rownames.push_back("r" + std::to_string(i + 1));
        for (size_t j = 0; j < (data.empty() ? 0 : data[0].size()); ++j)
          m.colnames.push_back(std::string(colStem) + std::to_string(j + 1));
        m.gen = eval::nextStoreGeneration();
        session.eval.rMatrices.emplace_back(name, std::move(m));
      };
      auto displayedVec = cp.vec;
      if (k == 2) {
        // The recorded two-variable symeigen columns are oriented from the
        // first nonzero component. Sign choices leave the decomposition
        // invariant. Keep the original factor used for seeded draws.
        for (size_t j = 0; j < k; ++j) {
          const double first = displayedVec[0][j] != 0.0 ? displayedVec[0][j] : displayedVec[1][j];
          if (first < 0.0)
            for (auto& row : displayedVec) row[j] = -row[j];
        }
      }
      put("L", displayedVec, "e");
      put("Ev", {cp.ev}, "e");
      put("C", cp.c, "c");
      eval::declareRPosted(session.eval, "drawnorm",
                           {"nneg", "nnzero", "npzero", "npos", "L", "Ev", "C"});
      (void)k;
    };
  }
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
    for (const auto& nm : names) {
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
      for (size_t j = 0; j < k; ++j) acc += L[i][j] * z[j];
      draws[static_cast<size_t>(r)][i] = acc;
    }
  }

  if (ds == nullptr) {
    CsvTable t;
    for (const auto& nm : names) {
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
    ojson made0 = mutationResult(session, "drawnorm", names);
    restoreResidue();
    return made0;
  }

  std::string cur = *session.currentHbin();
  for (size_t i = 0; i < k; ++i) {
    std::vector<std::string> cells;
    cells.reserve(static_cast<size_t>(n));
    for (int64_t r = 0; r < n; ++r) cells.push_back(numCell(draws[static_cast<size_t>(r)][i]));
    std::string out;
    if (auto err = appendColumn(session, cur, names[i], cells, "f64", &out)) return *err;
    cur = out;
  }
  session.load(cur);
  ojson made = mutationResult(session, "drawnorm", names);
  restoreResidue();
  return made;
}

}  // namespace piet::handlers
