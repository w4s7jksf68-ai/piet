// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Data splines.
#include "piet/handlers/data_values.h"
#include "dataset_shape_support.h"
#include "piet/mutation_result.h"
#include "piet/dispatcher.h"  // dispatch(): displayknots draws r(knots) with matlist
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/display_hardening.h"  // exprUnboundNameRefusal (predict, BUG-1248)
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/settings.h"
#include "piet/storage_type.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "estimation/estimation_internal.h"
#include "estimators/postest_names.h"  // parse_level_ref (the engine's one resolver)
#include "estimators/posthest.h"  // margins_grid / margins_parse_at (header-inline)

#include "postestimation/postestimation_internal.h"
#include "estimation/estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace postest_detail {}
using namespace postest_detail;
using namespace postestimation_vocab;


namespace {

// mkspline's second and third forms are registered next to the cubic form's options.
const piet::eval::OptVocab& kVocabMksplineSegments =
    piet::eval::registerOptVocab({"mkspline"}, {{"marginal", "pctile"}, {}});

// A number as a Stata macro holds it: 16 significant digits. mkspline.ado carries its knots
// through macros, so every knot it uses or posts is the value read back from that text.
double viaMacro(double v) {
  char b[64];
  std::snprintf(b, sizeof b, "%.16g", v);
  return std::strtod(b, nullptr);
}

// What `display` prints for a knot typed as `text`: the number, nine digits at most.
std::string knotText(double v) {
  char b[64];
  std::snprintf(b, sizeof b, "%.9g", v);
  std::string t = b;
  if (t.rfind("0.", 0) == 0) t.erase(0, 1);
  else if (t.rfind("-0.", 0) == 0) t.erase(1, 1);
  return t;
}

// centile's own rule: the P-th percentile is the (n+1)P/100-th smallest value, interpolating
// between neighbours, and the extremes outside [1, n].
double centileRule(const std::vector<double>& sorted, double P) {
  const double n = static_cast<double>(sorted.size());
  if (sorted.empty()) return std::numeric_limits<double>::quiet_NaN();
  const double pos = (n + 1.0) * P / 100.0;
  if (pos <= 1.0) return sorted.front();
  if (pos >= n) return sorted.back();
  const double j = std::floor(pos);
  const size_t lo = static_cast<size_t>(j) - 1;
  return sorted[lo] + (pos - j) * (sorted[lo + 1] - sorted[lo]);
}

// The type `compress` leaves a float column in: a byte or int when every value is a whole
// number that fits, else still a float.
std::string compressedFloatType(const std::vector<double>& vals) {
  double lo = 0.0, hi = 0.0;
  for (double x : vals) {
    if (std::isnan(x)) continue;
    if (x != std::floor(x) || !std::isfinite(x)) return "float";
    lo = std::min(lo, x);
    hi = std::max(hi, x);
  }
  if (lo >= -127.0 && hi <= 100.0) return "byte";
  if (lo >= -32767.0 && hi <= 32740.0) return "int";
  return "float";  // never a long: it is no narrower than a float
}

void postKnots(SessionState& session, const std::vector<double>& knots, const std::string& rowName) {
  eval::replaceRClass(session.eval);
  session.eval.rValues.set("N_knots", static_cast<double>(knots.size()));
  eval::EvalMatrix m;
  m.data = {knots};
  m.rownames = {rowName};
  for (size_t i = 0; i < knots.size(); ++i) m.colnames.push_back("knot" + std::to_string(i + 1));
  session.eval.rMatrices.emplace_back("knots", std::move(m));
  eval::declareRPosted(session.eval, "mkspline", {"N_knots", "knots"});
}

// displayknots: the ado's `matlist` of the knot matrix it returns (r(knots)), drawn as every
// matlist is (vs-stata bank 6f0ca3dea08825dd: `knot1` over `mpg | 20`).
ojson withKnotTable(SessionState& session, ojson mut, bool displayKnots) {
  if (!displayKnots || (mut.is_object() && mut.value("type", std::string()) == "error")) return mut;
  ojson out = ojson::array();
  out.push_back(std::move(mut));
  for (auto& b : dispatch("matlist r(knots)", session))
    if (!(b.is_object() && b.value("type", std::string()) == "echo")) out.push_back(b);
  return out;
}

}  // namespace

// `mkspline v1 k1 v2 k2 ... vK = x` and `mkspline stub # = x [, pctile]`: linear splines,
// segment by segment, as mkspline.ado builds them (`marginal` makes the slopes' changes the
// variables). Each variable is a float over the if/in sample with x present, compressed.
ojson runMksplineSegments(SessionState& session, const std::string& lhs, const std::string& srcVar,
                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                          const std::vector<char>& inSample) {
  using dataset_shape_detail::appendColumn;
  using dataset_shape_detail::numCell;
  using dataset_shape_detail::nextPath;
  if (auto err = eval::rejectUnknownOptions(opts, kVocabMkspline.known)) return *err;
  bool displayKnots = false;
  for (const auto& [k, v] : opts)
    if (k == "displayknots" && v.isBool && v.boolValue) displayKnots = true;
  bool marginal = false, pctile = false;
  for (const auto& [k, v] : opts) {
    if (k == "marginal") marginal = true;
    if (k == "pctile") pctile = true;
  }
  auto rcErr = [](const std::string& msg, int rc) {
    return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                  "r(" + std::to_string(rc) + ")"));
  };
  std::vector<std::string> toks;
  {
    std::istringstream is(lhs);
    for (std::string t; is >> t;) toks.push_back(t);
  }
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  if (!ds->has(srcVar)) return rcErr("variable " + srcVar + " not found", 111);
  if (ds->dtype(srcVar) != "f64")
    return rcErr("string variables not allowed in varlist;\n" + srcVar + " is a string variable", 109);
  const int64_t n = ds->nRows();
  const double* x = ds->col(srcVar).f64();
  std::vector<double> used;  // x over the sample
  for (int64_t i = 0; i < n; ++i)
    if (inSample[static_cast<size_t>(i)] && !std::isnan(x[i])) used.push_back(x[i]);

  std::vector<std::string> names;
  std::vector<double> knots;       // the numbers the segments use
  std::vector<double> postedKnots; // what r(knots) holds
  std::string postedRow = srcVar;
  if (toks.size() == 2) {
    // stub # = x
    char* end = nullptr;
    const double nn = std::strtod(toks[1].c_str(), &end);
    if (end == toks[1].c_str() || *end != '\0') return rcErr("'" + toks[1] + "' found where integer expected", 7);
    if (nn != std::floor(nn)) return rcErr("'" + toks[1] + "' found where integer expected", 7);
    if (nn < 2) return rcErr("invalid syntax", 198);
    if (used.empty()) return rcErr("no observations", 2000);
    const int nseg = static_cast<int>(nn);
    if (!pctile) {
      const double mn = *std::min_element(used.begin(), used.end());
      const double mx = *std::max_element(used.begin(), used.end());
      const double incr = viaMacro((mx - mn) / nn);
      double base = viaMacro(mn);
      for (int i = 1; i < nseg; ++i) {
        base = viaMacro(base + incr);
        knots.push_back(base);
        postedKnots.push_back(base);
      }
    } else {
      std::vector<double> sorted = used;
      std::sort(sorted.begin(), sorted.end());
      const double incr = viaMacro(100.0 / nn);
      double base = 0.0;
      for (int i = 1; i < nseg; ++i) {
        base = viaMacro(base + incr);
        const double c = centileRule(sorted, base);
        postedKnots.push_back(c);
        knots.push_back(viaMacro(c));
      }
    }
    postedRow = "r1";
    for (int i = 1; i <= nseg; ++i) names.push_back(toks[0] + std::to_string(i));
  } else {
    // v1 k1 v2 k2 ... vK
    if (toks.size() < 3 || toks.size() % 2 == 0) return rcErr("invalid syntax", 198);
    for (size_t i = 0; i < toks.size(); i += 2) {
      names.push_back(toks[i]);
      if (i + 1 < toks.size()) {
        char* end = nullptr;
        const double kv = std::strtod(toks[i + 1].c_str(), &end);
        if (end == toks[i + 1].c_str() || *end != '\0')
          return rcErr("'" + toks[i + 1] + "' found where number expected", 7);
        // (the ado's order test compares with a bound it never updates: unsorted knots run)
        knots.push_back(kv);
      }
    }
    postedKnots = knots;
  }
  for (const auto& nm : names) {
    ojson guard = dataset_shape_detail::newVarGuard(session, "mkspline", nm);
    if (!guard.is_null()) return guard;
  }
  if (used.empty()) return rcErr("no observations", 2000);

  const size_t nseg = names.size();
  std::string cur = *session.currentHbin();
  struct Made { std::string type, label; };
  std::vector<Made> made(nseg);
  for (size_t seg = 0; seg < nseg; ++seg) {
    std::vector<double> vals(static_cast<size_t>(n), std::numeric_limits<double>::quiet_NaN());
    const double prev = seg == 0 ? 0.0 : knots[seg - 1];
    const double upper = seg + 1 < nseg ? knots[seg] : std::numeric_limits<double>::infinity();
    for (int64_t i = 0; i < n; ++i) {
      if (!inSample[static_cast<size_t>(i)] || std::isnan(x[i])) continue;
      double v;
      if (marginal) v = seg == 0 ? x[i] : std::max(0.0, x[i] - prev);
      else if (seg == 0) v = std::min(x[i], upper);
      else v = std::max(std::min(x[i], upper), prev) - prev;
      vals[static_cast<size_t>(i)] = static_cast<double>(static_cast<float>(v));
    }
    std::vector<std::string> cells;
    cells.reserve(vals.size());
    for (double v : vals) cells.push_back(numCell(v));
    std::string out;
    if (auto err = appendColumn(session, cur, names[seg], cells, "f64", &out)) return *err;
    cur = out;
    made[seg].type = compressedFloatType(vals);
    const std::string lo = seg == 0 ? "." : knotText(knots[seg - 1]);
    const std::string hi = (marginal || seg + 1 == nseg) ? "." : knotText(knots[seg]);
    made[seg].label = srcVar + ": (" + lo + "," + hi + ")";
  }
  session.load(cur);
  for (size_t seg = 0; seg < nseg; ++seg) {
    session.implicitFloatVars.erase(names[seg]);
    session.varMeta.erase(names[seg]);
    VarMeta& vm = session.varMeta[names[seg]];
    vm.stataType = made[seg].type;
    vm.format = "%9.0g";
    vm.label = made[seg].label;
  }
  ojson mut = mutationResult(session, "gen", names);
  postKnots(session, postedKnots, postedRow);
  return withKnotTable(session, std::move(mut), displayKnots);
}

ojson runMkspline(const std::string& stubIn, const std::string& srcVarIn,
                   const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                   const std::optional<std::string>& ifExpr, SessionState& session) {
  // C4-L4B (FIX-C4L4B-RESIDUALS ①): entry validation reads a NAME and the
  // column NAME SET and nothing else, so the resident card answers both (the
  // D1 rule) and the datastore opens LAZILY. Escape (PIET_NO_RESIDENT_READ)
  // keeps the eager entry — and the whole stateless tail below — VERBATIM, so
  // the floor RED arm reproduces the pre-C4 path with one env.
  if (piet::residentReadDisabled()) {
    if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
      return eval::noDataLoaded();
    }
  } else if (!session.hasData()) {
    return eval::noDataLoaded();
  }
  std::string stub = trimM4(stubIn);
  std::string srcVar = trimM4(srcVarIn);
  if (stub.empty() || stub.find_first_of(" \t") != std::string::npos) {
    return engineErrorBlockM4(
        "syntax_error: `mkspline` requires a single STUB name on the left of `=` (the multi-"
        "newvar form `mkspline v1 v2 v3 = x` is not supported in this version of PIET).",
        "syntax_error");
  }
  if (srcVar.empty() || srcVar.find_first_of(" \t") != std::string::npos) {
    return engineErrorBlockM4("syntax_error: `mkspline` requires a single source variable",
                              "syntax_error");
  }
  Datastore* ds = nullptr;
  auto ensureDs = [&]() -> Datastore* {
    if (ds == nullptr) ds = session.datastore();
    return ds;
  };
  const std::vector<VarInfo>* card = session.residentReadCard();
  auto cardHas = [&](const std::string& name) -> bool {
    for (const auto& vi : *card) {
      if (vi.name == name) return true;
    }
    return false;
  };
  if (card != nullptr ? !cardHas(srcVar) : !ensureDs()->has(srcVar)) {
    return engineErrorBlockM4("Variable '" + srcVar + "' not found.");
  }
  if (ifExpr.has_value()) {
    return engineErrorBlockM4(
        "mkspline: `if` is not supported in this version of PIET (live-verified: the engine's "
        "`eval --keepif` DROPS non-matching rows from the WHOLE dataset rather than leaving the "
        "new spline variables missing for them the way real Stata's if-qualified generate-family "
        "commands do — forwarding it would silently shrink the dataset).",
        "unsupported_option");
  }

  bool cubic = false;
  for (const auto& [k, v] : opts) {
    if (k == "cubic" && v.isBool && v.boolValue) cubic = true;
  }
  if (!cubic) {
    return engineErrorBlockM4(
        "mkspline: only the `cubic` (restricted cubic spline) form is supported in this version "
        "of PIET — the engine's eval --mkspline op has no linear/piecewise-linear basis (it "
        "always computes a cubic basis regardless of the `cubic` keyword), so PIET requires "
        "`cubic` to be typed explicitly rather than silently answer the linear form with a cubic "
        "result.",
        "unsupported_option");
  }
  bool displayKnots = false;
  for (const auto& [k, v] : opts)
    if (k == "displayknots" && v.isBool && v.boolValue) displayKnots = true;

  std::optional<std::string> nknotsVal, knotsVal;
  for (const auto& [k, v] : opts) {
    if (k == "nknots" && !v.isBool) nknotsVal = v.strValue;
    if (k == "knots" && !v.isBool) knotsVal = v.strValue;
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabMkspline.known;
  if (auto err = eval::rejectUnknownOptions(opts, kKnown)) {
    return *err;
  }

  // The default knot positions exist for 3 to 7 knots only: `nknots(2)` and
  // `nknots(8)` are r(498) "Restricted cubic splines with # knots at default
  // values not allowed" (vs-stata bank c9deb962b89341de, 7af8f79cd4812e01).
  if (nknotsVal.has_value() && !knotsVal.has_value()) {
    const std::string nk = trimM4(*nknotsVal);
    if (!nk.empty() && nk.find_first_not_of("0123456789") == std::string::npos && nk.size() < 6) {
      const int n = std::stoi(nk);
      if (n < 3 || n > 7)
        return ojson(eval::errorBlock("engine_error",
                                      "Restricted cubic splines with " + nk +
                                          " knots at default values not allowed",
                                      std::nullopt, std::nullopt, std::nullopt, "r(498)"));
    }
  }
  // The knots the engine will place, worked out the way mkspline.ado does (for r(knots)), and
  // the refusals that come with them: a count that disagrees with nknots(), fewer than two
  // knots, knots out of order, knots too close for the sample.
  std::vector<double> cubicKnots;
  {
    std::vector<double> typed;
    if (knotsVal.has_value()) {
      std::string kv0 = *knotsVal;
      std::replace(kv0.begin(), kv0.end(), ',', ' ');
      std::istringstream is(kv0);
      for (std::string w; is >> w;) {
        char* end = nullptr;
        const double kv = std::strtod(w.c_str(), &end);
        if (end == w.c_str() || *end != '\0')
          return ojson(eval::errorBlock("engine_error", "invalid numlist", std::nullopt, std::nullopt,
                                        std::nullopt, "r(121)"));
        typed.push_back(kv);
      }
    }
    auto err498 = [](const std::string& m) {
      return ojson(eval::errorBlock("engine_error", m, std::nullopt, std::nullopt, std::nullopt, "r(498)"));
    };
    int nk = 5;
    if (nknotsVal.has_value()) {
      char* end = nullptr;
      const std::string t = trimM4(*nknotsVal);
      const double nv = std::strtod(t.c_str(), &end);
      if (!t.empty() && *end == '\0') nk = static_cast<int>(nv);
    }
    if (knotsVal.has_value()) {
      if (nknotsVal.has_value() && static_cast<int>(typed.size()) != nk)
        return err498("Count in nknots must be the same as the number of knots specified.");
      nk = static_cast<int>(typed.size());
      if (nk < 2) return err498("Restricted cubic splines must have at least 2 knots.");
      for (size_t j = 1; j < typed.size(); ++j)
        if (typed[j] <= typed[j - 1]) return err498("Knots must be in increasing order.");
      cubicKnots = typed;
    } else if (nk >= 3 && nk <= 7) {
      Datastore* kd = ensureDs();
      if (kd != nullptr && kd->has(srcVar) && kd->dtype(srcVar) == "f64") {
        const double* cx = kd->col(srcVar).f64();
        std::vector<double> xs;
        for (int64_t i = 0; i < kd->nRows(); ++i)
          if (!std::isnan(cx[i])) xs.push_back(cx[i]);
        std::sort(xs.begin(), xs.end());
        static const std::vector<std::vector<double>> kP = {
            {10, 50, 90}, {5, 35, 65, 95}, {5, 27.5, 50, 72.5, 95}, {5, 23, 41, 59, 77, 95},
            {2.5, 18.33, 34.17, 50, 65.83, 81.67, 97.5}};
        const double N = static_cast<double>(xs.size());
        for (double P : kP[static_cast<size_t>(nk - 3)]) cubicKnots.push_back(viaMacro(centileRule(xs, P)));
        if (!xs.empty()) {
          const double lo = centileRule(xs, 5.0 / (N + 1.0) * 100.0);
          const double hi = centileRule(xs, (N - 4.0) / (N + 1.0) * 100.0);
          if (cubicKnots.front() < lo) cubicKnots.front() = viaMacro(lo);
          if (cubicKnots.back() > hi) cubicKnots.back() = viaMacro(hi);
        }
      }
    }
    if (cubicKnots.size() >= 3 &&
        (cubicKnots[0] >= cubicKnots[1] ||
         cubicKnots.back() <= cubicKnots[cubicKnots.size() - 2]))
      return ojson(eval::errorBlock("engine_error", "Sample size too small for this many knots.",
                                    std::nullopt, std::nullopt, std::nullopt, "r(198)"));
  }
  std::string mkArg = stub + "=" + srcVar + ", cubic";
  if (nknotsVal.has_value() && !trimM4(*nknotsVal).empty()) {
    mkArg += ", nknots(" + trimM4(*nknotsVal) + ")";
  } else if (knotsVal.has_value() && !trimM4(*knotsVal).empty()) {
    mkArg += ", knots(" + trimM4(*knotsVal) + ")";
  }

  std::set<std::string> beforeSet;
  if (card != nullptr) {
    for (const auto& vi : *card) beforeSet.insert(vi.name);
  } else {
    std::vector<std::string> before = ensureDs()->names();
    beforeSet.insert(before.begin(), before.end());
  }

  std::vector<std::string> argv = {"eval", "--data", *session.currentHbin(), "--mkspline", mkArg};
  std::string outPath = nextPathEst(session.workdir(), "mkspline");
  argv.push_back("--out");
  argv.push_back(outPath);

  {
    // C4-L4B: `eval --mkspline` is a single TERMINAL mutation — its `--out`
    // becomes the head and is never read back as a file — so it takes the
    // resident frame plane, the same opt-in `recode`'s no-`if` arm has held
    // since MAXPERF Stage 2. Escape: pre-C4 stateless routing, verbatim.
    std::optional<SessionState::ResidentEligible> mksplineResident;
    if (!piet::residentReadDisabled()) mksplineResident.emplace(session);
    try {
      runCobraJsonM4(argv);
    } catch (const CobraError& ce) {
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
  }

  session.load(outPath);
  // C4-L4B resident tail: fold the frame delta into the shadow card instead of
  // invalidate+reopen. mkspline is the one verb in this class that cannot NAME
  // its new columns up front — the engine reports the generated basis names on
  // stderr only (apps/cmds/cmds_data.cpp:144), never in the JSON PIET reads —
  // so `new_vars` is diffed off the freshly FOLDED card and appended last,
  // which is exactly where the stateless block below has always put it
  // (ordered_json preserves insertion order; the two blocks are key-for-key
  // identical). The delta is consumed here, so a fold that DECLINES falls
  // through to the stateless rebuild below with nothing left pending — the
  // same landing spot mutationResult uses for its own nullopt.
  if (!piet::residentReadDisabled()) {
    if (auto folded = session.tryResidentMutationCard("mkspline", {}, {}, {})) {
      if (const std::vector<VarInfo>* after = session.residentReadCard()) {
        ojson out = std::move(*folded);
        ojson newVars = ojson::array();
        for (const auto& vi : *after) {
          if (!beforeSet.count(vi.name)) newVars.push_back(vi.name);
        }
        out["new_vars"] = newVars;
        if (!cubicKnots.empty()) postKnots(session, cubicKnots, srcVar);
        return withKnotTable(session, std::move(out), displayKnots && !cubicKnots.empty());
      }
    }
  }
  session.invalidateDatastore();
  Datastore* dsAfter = session.datastore();

  std::vector<std::string> newVarsVec;
  if (dsAfter != nullptr) {
    for (const auto& n : dsAfter->names()) {
      if (!beforeSet.count(n)) newVarsVec.push_back(n);
    }
  }
  ojson newVars = ojson::array();
  for (const auto& n : newVarsVec) newVars.push_back(n);

  // C1-CARD-REBUILD-REDUCE A11 sibling: `mkspline` (engine `eval --mkspline`)
  // appends only the new basis columns (newVarsVec = names present after but not
  // before) and byte-preserves every other column, so seed the unchanged
  // columns' missing-counts (kPureAdd branch 1, action "mkspline") BEFORE the
  // varlist rebuild below skips each column's O(nrows) nMissing scan. Byte-
  // identical vs PIET_NO_NMISS_CACHE=1.
  session.seedNMissForPureAdd(dsAfter, "mkspline", newVarsVec, /*droppedVars=*/{},
                             /*rewrittenVars=*/{});

  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "mkspline";
  ojson varlistAfter = ojson::array();
  auto vlist = session.varlistWithMeta(dsAfter);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: keep the cache current for the next mutation
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = dsAfter ? dsAfter->nRows() : 0;
  out["new_vars"] = newVars;
  if (!cubicKnots.empty()) postKnots(session, cubicKnots, srcVar);
  return withKnotTable(session, std::move(out), displayKnots && !cubicKnots.empty());
}


}  // namespace piet::handlers
