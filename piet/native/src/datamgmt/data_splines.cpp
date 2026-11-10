// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Data splines.
#include "piet/handlers/data_values.h"
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
  for (const auto& [k, v] : opts) {
    if (k == "displayknots" && v.isBool && v.boolValue) {
      return engineErrorBlockM4(
          "Option `displayknots` is a valid Stata option that this version of PIET does not yet "
          "implement for `mkspline` (no engine CLI equivalent — knot positions are never "
          "surfaced back to PIET).",
          "unsupported_option");
    }
  }

  std::optional<std::string> nknotsVal, knotsVal;
  for (const auto& [k, v] : opts) {
    if (k == "nknots" && !v.isBool) nknotsVal = v.strValue;
    if (k == "knots" && !v.isBool) knotsVal = v.strValue;
  }
  if (nknotsVal.has_value() && knotsVal.has_value()) {
    return engineErrorBlockM4("mkspline: nknots() and knots() may not be combined.",
                              "syntax_error");
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabMkspline.known;
  if (auto err = eval::rejectUnknownOptions(opts, kKnown)) {
    return *err;
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
        return out;
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
  return out;
}


}  // namespace piet::handlers
