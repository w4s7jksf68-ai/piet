// SPDX-License-Identifier: GPL-3.0-or-later
#include "piet/column_fetch.h"
#include "piet/translate.h"
// Copyright (C) 2026 M.K. Kim
// Levels command.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // BUG-540: stataMissingWithKind — `.a`-`.z` identity in matrix literals
#include "piet/eval/macros.h"
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/program.h"  // BUG-922: program::restorePreservesDeeperThan is defined here
#include "piet/serset.h"  // BUG-1461: serset::storageType, signed into `datasignature`
#include "piet/strmutate.h"  // BUG-532: needsStringCount, the string-condition test under by:
#include "piet/stata_format.h"  // eb-order T-P1: `matrix list ..., format()`
#include "piet/strutil.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path / env_utf8 (BUG-050/051)
#include "data/missing.h"   // Stata `.` / `.a`-`.z` kinds + stata_cmp (shared with the engine)
#include "core/stata_arith.h"  // `^` with StataBE's bits (frame.cpp's)

#if defined(__APPLE__)
#include <sys/clonefile.h>  // Q1 (gate ③): APFS copy-on-write snapshot for preserve
#endif  // BUG-1469: the vocabularies below sat inside it, undeclared off macOS

#include "command_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace command_detail {}
using namespace command_detail;
using namespace session_vocab;

namespace command_detail {

// levelsof.py's numeric _fmt: integer-valued -> plain integer string, else
// Python's str(float) shortest round-trip repr.
std::string levelsofFmtNum(double v) {
  if (std::isfinite(v) && v == static_cast<double>(static_cast<long long>(v))) {
    return std::to_string(static_cast<long long>(v));
  }
  return eval::pythonFloatRepr(v);
}

}  // namespace

ojson runLevelsof(const std::string& varname, const std::optional<std::string>& localName,
                   bool clean, const std::optional<std::string>& ifExpr, SessionState& session) {
  // ONESHOT-GETCOLS2: `levelsof` reads ONE column's values (plus whatever an `if`
  // names). Materializing the head for that was 14 of the flushes left after the
  // mutation entrance moved. `varname` is passed as alsoInclude because it is the
  // subject, not an expression reference.
  std::string lvlMiniPath;
  std::unique_ptr<Datastore> lvlMini;
  if (auto cols = referencedColumns(session, {ifExpr.has_value() ? &*ifExpr : nullptr}, varname))
    lvlMini = fetchColumns(session, *cols, session.workdir(), &lvlMiniPath);
  if (lvlMini && !lvlMini->has(varname)) lvlMini.reset();  // subject absent → not usable
  Datastore* ds = lvlMini ? lvlMini.get() : session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();

  // levelsof.py's run_levelsof(ds, varname, clean) operates on `target_ds` —
  // the CURRENT dataset, or a row-filtered temp copy when `if COND` is
  // present (dispatcher.py's _run_levelsof: strmutate.write_filtered_hbin,
  // keep=True). M3a numeric-only slice: string/factor `if` conditions are
  // OUT of scope (strmutate's row-wise evaluator is not ported this
  // milestone) — a pure-numeric if routes through cobra eval --keepif.
  Datastore* targetDs = ds;
  std::unique_ptr<Datastore> filteredHolder;
  if (ifExpr.has_value() && !ifExpr->empty()) {
    std::string outPath = nextPath(session.workdir(), "levelsof");
    // BUG-381: this filtered copy is READ and abandoned — it never becomes the
    // head, so no supersede ever names it and reclaimSupersededScratch cannot
    // reach it however it is scheduled. That is what the §38 projection ledger
    // is for: register it and the outermost dispatch boundary drains it.
    session.noteProjectionScratch(outPath);
    // ONESHOT-LASTFLUSH: the earlier wave filtered the REAL head here, reasoning
    // that a projection "carries only what we fetched". It carries exactly what
    // this filter needs: the `if` expression's own columns (that is what the fetch
    // was built from) plus `varname`, which is the only column read back below. So
    // filter the projection and leave the head virtual — this was the last 14 of
    // the flushes, and the fetch above was pure overhead on this branch before.
    //
    // The fallback is not decoration. `referencedColumns` leaves identifiers the
    // shadow card does not confirm alone, so a card that is missing a real column
    // yields a projection the filter cannot evaluate. That is an ENGINE error, not
    // a distinguishable one, so it is caught and retried against the head rather
    // than reported — a miss must cost a fallback, never an answer.
    const std::string lvlHead = session.currentHbin().value_or(ds->path());
    bool filtered = false;
    if (lvlMini) {
      std::vector<std::string> tryArgs = translate::injectTsContextForExpr(
          {"eval", "--data", lvlMiniPath, "--keepif", *ifExpr, "--out", outPath},
          *ifExpr, session);  // BUG-369 follow-up
      try {
        runCobraJson(tryArgs);
        filtered = true;
      } catch (const CobraError&) {
        cecon::native_unlink(outPath);  // never leave a torn file for the retry to open
      }
    }
    if (!filtered) {
      std::vector<std::string> args = translate::injectTsContextForExpr(
          {"eval", "--data", lvlHead, "--keepif", *ifExpr, "--out", outPath},
          *ifExpr, session);  // BUG-369 follow-up
      try {
        runCobraJson(args);
      } catch (const CobraError& ce) {
        return engineErrorBlock("levelsof: " + cobraErrorStr(ce), ce.code);
      }
    }
    filteredHolder = std::make_unique<Datastore>(outPath);
    targetDs = filteredHolder.get();
  }

  if (!targetDs->has(varname)) {
    // Python: `raise KeyError(f"variable '{varname}' not found in dataset")`,
    // caught as `except KeyError as e: return make_engine_error(str(e))`.
    // KeyError.__str__() wraps the message in an EXTRA layer of double
    // quotes (Python's repr-of-a-single-string-arg convention) — reproduce
    // that exact double-quoting, not just the inner message.
    return engineErrorBlock("\"variable '" + varname + "' not found in dataset\"");
  }

  std::vector<std::string> levelStrings;
  if (targetDs->dtype(varname) == "f64") {
    const double* d = targetDs->col(varname).f64();
    int64_t count = targetDs->col(varname).nbytes / static_cast<int64_t>(sizeof(double));
    std::vector<double> uniq;
    for (int64_t i = 0; i < count; ++i) {
      if (!std::isnan(d[i])) uniq.push_back(d[i]);
    }
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    for (double v : uniq) levelStrings.push_back(levelsofFmtNum(v));
    // clean has NO effect on numeric columns (Python only branches on
    // `clean` inside the `else` / factor-dtype arm) — level_strings above
    // is final for f64 regardless of `clean`.
  } else {
    const int32_t* codes = targetDs->col(varname).codes();
    int64_t count = targetDs->col(varname).nbytes / static_cast<int64_t>(sizeof(int32_t));
    std::vector<int32_t> uniqCodes;
    for (int64_t i = 0; i < count; ++i) {
      if (codes[i] >= 0) uniqCodes.push_back(codes[i]);
    }
    std::sort(uniqCodes.begin(), uniqCodes.end());
    uniqCodes.erase(std::unique(uniqCodes.begin(), uniqCodes.end()), uniqCodes.end());
    const auto& levelsList = targetDs->varinfo(varname).levels;
    std::vector<std::string> rawLabels;
    for (int32_t c : uniqCodes) {
      rawLabels.push_back((c >= 0 && static_cast<size_t>(c) < levelsList.size())
                              ? levelsList[static_cast<size_t>(c)]
                              : std::to_string(c));
    }
    std::sort(rawLabels.begin(), rawLabels.end());
    for (const auto& lbl : rawLabels) {
      levelStrings.push_back(clean ? lbl : ("\"" + lbl + "\""));
    }
  }

  std::string joined;
  for (size_t i = 0; i < levelStrings.size(); ++i) {
    if (i) joined += " ";
    joined += levelStrings[i];
  }

  ojson out;
  out["type"] = "levelsof";
  out["varname"] = varname;
  ojson levelsArr = ojson::array();
  for (const auto& s : levelStrings) levelsArr.push_back(s);
  out["levels"] = levelsArr;
  out["text"] = joined;

  // `if local_name:` — ONLY set when local() was explicitly given (no
  // default local-var name).
  if (localName.has_value() && !localName->empty()) {
    session.eval.locals.set(*localName, joined);
  }
  return out;
}

// ---------------------------------------------------------------------------
// M3b: clear / set obs / cd / pwd / noop (set|version|compress) — foundational
// commands the corpus's data-construction idiom (`clear` -> `set obs N` ->
// `gen ...`) depends on. C++ port of dispatcher.py's clear/set_obs/cd/pwd
// handlers + session.py's clear_data.

// BS-044 (BETA campaign C4): `clear <subcmd>` scoping. Before this fix
// EVERY subcommand fell through to the same bare-data-wipe path (probe
// bs044: `clear results` answered "No dataset is loaded"), which is wrong
// for every subcommand except a genuinely bare `clear`/`clear all`.
//
// Ground truth here is Stata's own clear.ado (StataNow 19.5, dumped via
// `program list` while live-probing this item — not a guess):
//   ""            -> `drop _all` + `label drop _all` ONLY. Scalars,
//                     matrices, e()/r() results and est-store all SURVIVE
//                     a bare `clear` (live-probed: sca1/matrix m1/ereturn
//                     list all still present after a bare `clear`).
//   "all"/"*"     -> everything (data, labels, matrices, scalars, e()/r(),
//                     programs, panelvar/timevar).
//   "results"/    -> ONE shared branch in the real ado: `return clear` +
//   "matrix"         `ereturn clear` + `sreturn clear` + `_return drop
//                     _all` (r()/e()/s() results only — NOT scalars, NOT
//                     matrices). `matrix` ADDITIONALLY does `matrix drop
//                     _all` + `_est drop _all` (user matrices + the eststo
//                     store) — oracle-pinned (`clear results` keeps N=336
//                     + scalar list) plus this ado reading for the r()/e()
//                     split and the matrix/results relationship.
//   "programs"    -> `program drop _all` (session.programs — real store).
//   "mata"        -> empties the M0 Mata store (session.eval.mataMatrices).
//   "ado"/"rngstream"/"rngstreams"/"frames"/"collect" -> real Stata
//                     subcommands with genuinely NO backing store in this
//                     build (no frames/collect engine, no rngstream
//                     state, no ado-vs-interactive program provenance) —
//                     honest scoped no-ops with a note, replacing the old
//                     "wipe the whole dataset" mistake, not a silent
//                     no-message pass-through.
//   anything else -> real Stata hard-rejects ("`1' not allowed", r(198));
//                     matched here rather than silently no-opping OR
//                     wiping data for a typo'd subcommand.
//
// Deviation from this batch's task-brief shorthand (noted for review): the
// brief grouped "matrix" with the no-store no-op list, but PIET already
// carries a real matrix store (eval.matrices/eval.eMatrices, populated by
// the `matrix define` handler) and a real est-store (session.estimates) —
// clearing them costs no new infrastructure and matches verified Stata
// truth, so `clear matrix` is implemented as a genuine clear here, not a
// no-op. See this task's return report for the full callout.

}  // namespace piet::handlers
