// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Row conditions through the row evaluator: count if, keep if / drop if, and the
// hidden qualifier, weight and row-expression columns other commands' `if` reads.
// Part of the row-evaluator family; the evaluator is datamgmt/row_evaluation.cpp.
#include "piet/strmutate.h"
#include "piet/column_fetch.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/counters.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"  // protectLiterals / substituteBareName (installRowExpression)
#include "piet/eval/pyeval.h"
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"  // MECHOPT W3: the resident arm folds through it
#include "piet/strutil.h"
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "piet/storage_type.h"  // FU-849: replace's lattice (promotedType)
#include "data/missing.h"  // the data plane's extended-missing NaN payloads
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)
#include "row_evaluation_internal.h"

namespace piet::strmutate {

using namespace row_eval_detail;

// BS-090 (BETA Phase G): AND a co-occurring `in a/b` into an already-built
// per-row mask (Doctrine ① — count/keep-if/drop-if's row-wise string detour
// must never silently drop an `in` that reached here alongside a
// string-referencing `if`, just because the `if` half needed this path
// instead of cobra's --keepif). Returns false (mask left UNTOUCHED) on an
// `in` that names no rows -- caller must treat that as its own distinct
// failure, not silently proceed with an unfiltered mask. `outRefusal` is then
// InRangeBounds::refusal, Stata's sentence ("observation numbers out of range"
// for `in 2/9` on 4 rows, "'0' invalid observation number" for `in 0`), or ""
// for a malformed range; the caller renders it with inRangeRejectError, as the
// engine route does. This used to answer every failure with its own "This `in`
// row-range form is not supported" text, so `count if s == "a" in 2/9` and
// `assert !missing(s) in 2/9` worded Stata's r(198) differently from `count
// in 2/9`.
static bool applyInRangeToMask(std::vector<bool>& mask, int64_t nRowsTotal,
                               const std::optional<std::string>& inRange,
                               std::string& outRefusal) {
  if (!inRange.has_value() || inRange->empty()) return true;
  piet::handlers::InRangeBounds rr = piet::handlers::parseInRangeBounds(*inRange, nRowsTotal);
  if (!rr.ok) {
    outRefusal = rr.refusal;
    return false;
  }
  for (int64_t i = 0; i < static_cast<int64_t>(mask.size()); ++i) {
    long long oneBased = static_cast<long long>(i) + 1;
    if (oneBased < rr.a || oneBased > rr.b) mask[static_cast<size_t>(i)] = false;
  }
  return true;
}

// The row mask of a condition, by the row evaluator `count if` has always used:
// countRowsIf and installConditionMask both read it here, so the route below is
// not a second evaluator. `nRows` is the frame's row count.
struct RowMask {
  bool ok = true;
  std::vector<bool> mask;
  int64_t nRows = 0;
  std::string errMsg;
  std::string errReason;  // "r(NNN)" when the evaluator named Stata's code
  std::string errText;    // with errReason: the evaluator's own words, or ""
};
static RowMask rowMaskIf(SessionState& session, const std::string& ifExpr,
                         const std::optional<std::string>& inRange = std::nullopt) {
  RowMask r;
  // ONESHOT-GETCOLS2: same shape as the mutation entrance — this counter needs the
  // predicate's columns, not the frame.
  // BUG-370: `count if grp=="a" & L.x>0` — the operator half is resolved by the
  // engine into a scratch frame, the string half by the evaluator below. Nothing
  // is loaded as the head, so this read-only command stays read-only.
  TsIfPrep tsPrep = materializeTsIf(session, ifExpr, inRange);
  if (tsPrep.failed) {
    r.ok = false;
    r.errMsg = tsPrep.errMsg;
    return r;
  }
  std::string cntMiniPath;
  std::unique_ptr<Datastore> cntMini;
  std::unique_ptr<Datastore> tsFrame;
  if (!tsPrep.evalPath.empty()) {
    tsFrame = std::make_unique<Datastore>(tsPrep.evalPath);
  } else if (namesHiddenColumn(tsPrep.rewrittenIf)) {
    // A hidden column is on no card, so a projection would lack it (datastore.h:
    // such a condition reads the frame that carries the column).
  } else if (auto cols = referencedColumns(session, {&tsPrep.rewrittenIf}, "")) {
    cntMini = fetchColumns(session, *cols, session.workdir(), &cntMiniPath);
  }
  Datastore* ds = tsFrame ? tsFrame.get() : (cntMini ? cntMini.get() : session.datastore());
  std::string condPy = rewriteExpr(tsPrep.rewrittenIf);
  EvalRowsResult er = evalRows(session, *ds, std::nullopt, std::optional<std::string>(condPy));
  if (!er.ok) {
    r.ok = false;
    r.errMsg = er.errMsg;
    r.errReason = er.errReason;
    r.errText = er.errText;
    return r;
  }
  r.nRows = ds->nRows();
  r.mask = std::move(er.mask);
  return r;
}

CountResult countRowsIf(SessionState& session, const std::string& ifExpr,
                        const std::optional<std::string>& inRange) {
  CountResult r;
  if (!session.hasData()) {
    r.ok = false;
    r.errMsg = "count: no dataset in memory";
    return r;
  }
  RowMask rm = rowMaskIf(session, ifExpr, inRange);
  if (!rm.ok) {
    r.ok = false;
    r.errMsg = rm.errText.empty() ? rm.errMsg : rm.errText;
    r.errReason = rm.errReason;
    return r;
  }
  if (!applyInRangeToMask(rm.mask, rm.nRows, inRange, r.errMsg)) {
    r.ok = false;
    r.inRangeError = true;
    return r;
  }
  long long cnt = 0;
  for (bool m : rm.mask) {
    if (m) ++cnt;
  }
  r.n = cnt;
  return r;
}

// `__piet_qual<kind><k>__` (piet/datastore.h kQualifierColumnPrefix), k unique in
// the process, so a command nested inside another's dispatch (eststo:, bootstrap:)
// installs its own and never overwrites the one its driver reads.
static std::string nextQualifierColumn(const char* kind) {
  static unsigned long long installs = 0;
  return std::string(kQualifierColumnPrefix) + kind + std::to_string(++installs) + "__";
}

// A condition's row mask as the f64 0/1 column `column` of a copy of `basePath`
// written to `outPath` (`cobra setcol`); the error block when it is not.
static std::optional<ojson> writeMaskColumn(SessionState& session, const std::string& basePath,
                                            const std::vector<bool>& mask,
                                            const std::string& column,
                                            const std::string& outPath) {
  const std::string valuesPath = nextPath(session.workdir(), "qualif") + ".csv";
  {
    std::ofstream vf(cecon::to_native_path(valuesPath), std::ios::binary);
    if (!vf) return errBlock("if: the condition's row mask could not be written");
    for (bool m : mask) vf << (m ? "1\n" : "0\n");
    vf.flush();
    if (!vf) {
      cecon::native_unlink(valuesPath);
      return errBlock("if: the condition's row mask could not be written");
    }
  }
  const CobraRunResult cr = runCobra({"setcol", "--data", basePath, "--var", column, "--values",
                                      valuesPath, "--dtype", "f64", "--new", "--out", outPath});
  cecon::native_unlink(valuesPath);
  // BUG-124's torn-write guard: a stateless --out that is not on disk is a failed
  // write.
  if (!cr.ok || !cecon::native_exists(outPath)) {
    return errBlock(std::string("if: the condition's row mask could not be installed: ") +
                    errorFromRaw(cr).what());
  }
  return std::nullopt;
}

// A failure that named Stata's code is worded as Stata words it, with no "if: "
// prefix: a string variable read as a truth value is "type mismatch" r(109)
// (StataBE 19.5, 2026-11-03: `assert x > 0 if g`, `assert g == g if g` and
// `assert x if x > 0 & g` with g a string variable), the words `count if g`
// already gave.
static ojson rowMaskErrBlock(const std::string& errMsg, const std::string& errReason,
                             const std::string& errText) {
  if (errReason.empty()) return errBlock("if: " + errMsg);
  const std::string msg = !errText.empty()           ? errText
                          : errReason == "r(109)"    ? std::string("type mismatch")
                                                     : "if: " + errMsg;
  return ojson(
      eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt, errReason));
}

ConditionMaskInstall installConditionMask(SessionState& session, const std::string& cond,
                                          const std::vector<std::string>& groupKeys) {
  ConditionMaskInstall out;
  auto fail = [&out](ojson block) {
    out.ok = false;
    out.error = std::move(block);
    return out;
  };
  if (!session.hasData() || !session.currentHbin().has_value()) {
    return fail(ojson(eval::noDataLoaded()));
  }
  // BUG-1436: `by g: generate z = _n if strlen(s) > 2 & _n > 1` reads `_n` within
  // the group (StataBE 19.5: z = . . 3 . 2 . 4 on groups of 3 and 4, measured
  // 2026-10-29), so the evaluator reads it from a scratch copy that numbers each
  // group; the mask lines up with the head's rows, which that copy keeps.
  std::optional<std::string> groupCond;
  std::string byPath;
  if (!groupKeys.empty()) {
    session.ensureMaterializedHead();
    byPath = nextPath(session.workdir(), "qualif_byn") + ".hbin";
    try {
      groupCond =
          handlers::groupObsNumberFrame(session, *session.currentHbin(), cond, groupKeys, byPath);
    } catch (const CobraError& ce) {
      return fail(errBlock(cobraErrorMessage(ce), ce.code));
    }
  }
  RowMask rm;
  if (groupCond.has_value()) {
    EvalRowsResult er;
    {
      Datastore ds(byPath);
      er = evalRows(session, ds, std::nullopt, std::optional<std::string>(rewriteExpr(*groupCond)));
    }
    cecon::native_unlink(byPath);
    if (!er.ok) return fail(rowMaskErrBlock(er.errMsg, er.errReason, er.errText));
    rm.mask = std::move(er.mask);
  } else {
    rm = rowMaskIf(session, cond);
    if (!rm.ok) return fail(rowMaskErrBlock(rm.errMsg, rm.errReason, rm.errText));
  }
  const std::string column = nextQualifierColumn("if");
  const std::string basePath = *session.currentHbin();
  const std::string maskHbin = nextPath(session.workdir(), "qualif") + ".hbin";
  // Stateless (a real file, not the resident frame): the §38 column projection
  // (column_fetch.cpp referencedColumns) names only the columns the varlist card
  // lists, and a hidden column is on no card, so a projection cut from a dirty
  // resident head would lack the mask (`summarize x if strpos(...)`: r(111)
  // `unknown variable __piet_qualif1__`). A clean head takes no projection.
  if (auto err = writeMaskColumn(session, basePath, rm.mask, column, maskHbin)) return fail(*err);
  const bool changed = session.datasetChanged;
  session.pinPreInstallHead();  // a driver may hold the pre-install head's path
  session.load(maskHbin);
  session.carryCardAcrossHiddenInstall(basePath, maskHbin);
  session.datasetChanged = changed;
  out.column = column;
  out.condition = "(" + column + "!=0)";
  return out;
}

ConditionMaskInstall writeConditionMaskColumn(SessionState& session, const std::string& dataPath,
                                              const std::string& cond, const std::string& column,
                                              const std::string& outPath) {
  ConditionMaskInstall out;
  Datastore ds(dataPath);
  EvalRowsResult er =
      evalRows(session, ds, std::nullopt, std::optional<std::string>(rewriteExpr(cond)));
  if (!er.ok) {
    out.ok = false;
    out.error = rowMaskErrBlock(er.errMsg, er.errReason, er.errText);
    return out;
  }
  if (auto err = writeMaskColumn(session, dataPath, er.mask, column, outPath)) {
    out.ok = false;
    out.error = *err;
    return out;
  }
  out.column = column;
  out.condition = "(" + column + "!=0)";
  return out;
}

ConditionMaskInstall installWeightExpression(SessionState& session, const std::string& expr) {
  ConditionMaskInstall out;
  if (!session.hasData() || !session.currentHbin().has_value()) {
    out.ok = false;
    out.error = ojson(eval::noDataLoaded());
    return out;
  }
  const std::string column = nextQualifierColumn("w");
  const std::string basePath = *session.currentHbin();
  const std::string outHbin = nextPath(session.workdir(), "qualw") + ".hbin";
  // Stateless for the reason installConditionMask gives.
  const CobraRunResult cr = runCobra(
      {"eval", "--data", basePath, "--gen", column + "=(" + expr + ")", "--out", outHbin});
  if (!cr.ok || !cecon::native_exists(outHbin)) {
    const CobraError ce = errorFromRaw(cr);
    out.ok = false;
    out.error = errBlock(std::string("weight expression `") + expr + "`: " + ce.what());
    return out;
  }
  const bool changed = session.datasetChanged;
  session.pinPreInstallHead();
  session.load(outHbin);
  session.carryCardAcrossHiddenInstall(basePath, outHbin);
  session.datasetChanged = changed;
  out.column = column;
  out.condition = column;
  return out;
}

ConditionMaskInstall installRowExpression(SessionState& session, const std::string& expr,
                                          const std::string& byVars, bool countValues) {
  ConditionMaskInstall out;
  auto fail = [&out](ojson block) {
    out.ok = false;
    out.error = std::move(block);
    return out;
  };
  if (!session.hasData() || !session.currentHbin().has_value()) {
    return fail(ojson(eval::noDataLoaded()));
  }
  // This evaluator's sum() is no running sum (exprfns.cpp fnSum answers its
  // argument), which the ados' `generate` of the expression is.
  if (eval::exprHasRunningSum(expr)) {
    out.declined = true;
    return fail(errBlock("sum() inside an egen argument is evaluated by the engine"));
  }
  const std::string basePath = *session.currentHbin();
  std::string e = expr;
  std::vector<std::string> byList;
  {
    std::string tok;
    for (char ch : byVars + " ") {
      if (ch == ' ' || ch == ',' || ch == '\t') {
        if (!tok.empty()) byList.push_back(tok);
        tok.clear();
      } else {
        tok += ch;
      }
    }
  }
  // Under a by-list (the caller passes one only for the functions whose ados
  // generate the expression `by` the groups: total, mean, max, ...), _n and _N
  // count within the group, the rows an if/in leaves out included (StataBE:
  // `egen t = total(strlen(s) * _n), by(g)` on g = 1 1 2 2 2 is 7 and 9, `egen
  // t = total(_N) if x != 4, by(g)` 4 and 6). This evaluator's _n and _N are
  // the frame's, so the engine writes the group's into two columns of a
  // scratch copy (`--bygen`, the by-prefix's own machinery) and the expression
  // reads those instead. Without a by-list, _n and _N are the frame's even
  // under if/in (`egen t = total(_n) if x > 2` is 3+4+5).
  std::unique_ptr<Datastore> byFrame;
  if (!byList.empty() && namesObsPosition(e)) {
    std::string by;
    for (const auto& v : byList) by += (by.empty() ? "" : ",") + v;
    // Plain names, not qualifier columns: those are hidden from the Datastore
    // the evaluator reads, and these never reach the working frame.
    static unsigned long long installs = 0;
    const std::string k = std::to_string(++installs);
    const std::string posCol = "__piet_bypos" + k;
    const std::string sizeCol = "__piet_bysize" + k;
    const std::string scratch = nextPath(session.workdir(), "qualx") + ".hbin";
    // Not ResidentEligible: the Datastore below reads --out back as a file.
    const CobraRunResult cr =
        runCobra({"eval", "--data", basePath, "--bygen", posCol + "=_n, by(" + by + ")", "--bygen",
                  sizeCol + "=_N, by(" + by + ")", "--out", scratch});
    if (!cr.ok || !cecon::native_exists(scratch)) {
      out.declined = true;
      return fail(errBlock(std::string("by-group positions: ") + errorFromRaw(cr).what()));
    }
    byFrame = std::make_unique<Datastore>(scratch);
    std::vector<std::string> spans;
    e = eval::protectLiterals(e, &spans);
    e = eval::substituteBareName(e, "_n", posCol);
    e = eval::substituteBareName(e, "_N", sizeCol);
    e = eval::restoreLiterals(e, spans);
  }
  // The expression's columns, not the frame (contract_A §38), as rowMaskIf.
  std::string miniPath;
  std::unique_ptr<Datastore> mini;
  if (!byFrame) {
    if (auto cols = referencedColumns(session, {&e}, ""))
      mini = fetchColumns(session, *cols, session.workdir(), &miniPath);
  }
  Datastore* ds = byFrame ? byFrame.get() : (mini ? mini.get() : session.datastore());
  if (ds == nullptr) return fail(ojson(eval::noDataLoaded()));
  const EvalRowsResult er =
      evalRows(session, *ds, std::optional<std::string>(rewriteExpr(e)), std::nullopt);
  if (!er.ok) {
    // A code the evaluator named is the answer (`max(upper(s) & 1)`: r(109));
    // anything else it cannot read (a subscript) is the engine's.
    out.declined = er.errReason.empty();
    return fail(rowErrBlock(er.errText.empty() ? er.errMsg : er.errText, er));
  }
  // A string result is what Stata's `generate double` inside the egen ado
  // refuses: `egen m = max(upper(s))` is r(109). count() is the exception: it
  // counts the values that are not missing, a string's included (StataBE:
  // `egen c = count(substr(s,1,1))` is 4 with one "" in five rows), so its
  // column holds 1 for a non-empty string and missing for "".
  bool isString = false;
  for (size_t i = 0; i < er.values.size() && !isString; ++i)
    isString = er.mask[i] && er.valueIsSet[i] && er.values[i].isStr();
  if (isString && !countValues) {
    return fail(ojson(eval::errorBlock("engine_error", "type mismatch", std::nullopt,
                                       std::nullopt, std::nullopt, "r(109)")));
  }
  const std::string column = nextQualifierColumn("x");
  const std::string valuesPath = nextPath(session.workdir(), "qualx") + ".csv";
  {
    std::ofstream vf(cecon::to_native_path(valuesPath), std::ios::binary);
    if (!vf) return fail(errBlock("the expression's values could not be written"));
    for (size_t i = 0; i < er.values.size(); ++i) {
      double v = std::nan("");
      if (er.valueIsSet[i])
        v = !isString ? coerceNum(er.values[i]) : coerceStr(er.values[i]).empty() ? v : 1.0;
      vf << csvNum(v) << "\n";
    }
    vf.flush();
    if (!vf) {
      cecon::native_unlink(valuesPath);
      return fail(errBlock("the expression's values could not be written"));
    }
  }
  const std::string outHbin = nextPath(session.workdir(), "qualx") + ".hbin";
  // Stateless for the reason installConditionMask gives.
  const CobraRunResult cr = runCobra({"setcol", "--data", basePath, "--var", column, "--values",
                                      valuesPath, "--dtype", "f64", "--new", "--out", outHbin});
  cecon::native_unlink(valuesPath);
  if (!cr.ok || !cecon::native_exists(outHbin)) {
    return fail(errBlock(std::string("the expression's values could not be installed: ") +
                         errorFromRaw(cr).what()));
  }
  const bool changed = session.datasetChanged;
  session.pinPreInstallHead();
  session.load(outHbin);
  session.carryCardAcrossHiddenInstall(basePath, outHbin);
  session.datasetChanged = changed;
  out.column = column;
  out.condition = column;
  return out;
}

void removeConditionMask(SessionState& session, const std::string& column) {
  if (column.empty() || !session.hasData() || !session.currentHbin().has_value()) return;
  const std::string basePath = *session.currentHbin();
  const std::string outHbin = nextPath(session.workdir(), "qualdrop") + ".hbin";
  CobraRunResult cr;
  {
    std::optional<SessionState::ResidentEligible> resident;
    resident.emplace(session);
    cr = runCobra({"eval", "--data", basePath, "--drop", column, "--out", outHbin});
  }
  // A failed drop is a frame that no longer carries the column (collapse,
  // reshape, use): nothing to take back.
  if (!cr.ok || (!session.residentFrameLive() && !cecon::native_exists(outHbin))) return;
  const bool changed = session.datasetChanged;
  session.load(outHbin);
  session.carryCardAcrossHiddenInstall(basePath, outHbin);
  session.datasetChanged = changed;
}

ojson filterRowsIf(SessionState& session, const std::string& ifExpr, bool keep,
                   const std::optional<std::string>& inRange) {
  std::string action = keep ? "keep if" : "drop if";
  if (!session.hasData()) return errBlock(action + ": no dataset in memory");
  // INSPECT-2608 ENTRANCE (contract_A §38, the same projection runStringGen and
  // runStringReplace already take): the row-wise evaluator needs the one or two
  // columns the CONDITION names, and `session.datastore()` was flushing the whole
  // resident frame to disk to give them to it. Measured on the acceptance
  // corpus's controls=yes arm, this form was the single largest item — 49.4 s
  // and 2.7 GB of touched bytes over 11 calls.
  //
  // The projection is only usable when the exit below can also avoid needing
  // every column (the fast mask path); the CSV fallback rebuilds the frame from
  // `ds` and would silently drop unprojected columns. So `dsFull` stays the
  // authority for that path and the projection is scoped to the mask.
  // BUG-370 — see materializeTsIf. Only the MASK is computed off the scratch
  // frame; both exits below re-derive their own authority from the session (the
  // fast path re-reads currentHbin(), the CSV path re-opens session.datastore()),
  // so a temp column cannot reach the filtered result.
  TsIfPrep tsPrep = materializeTsIf(session, ifExpr, inRange);
  if (tsPrep.failed) return errBlock(action + ": " + tsPrep.errMsg, tsPrep.errCode);
  std::string miniPath;
  std::unique_ptr<Datastore> mini;
  std::unique_ptr<Datastore> tsFrame;
  if (!tsPrep.evalPath.empty()) {
    tsFrame = std::make_unique<Datastore>(tsPrep.evalPath);
  } else {
    const std::string* pExpr = &tsPrep.rewrittenIf;
    if (auto cols = referencedColumns(session, {pExpr}, ""))
      mini = fetchColumns(session, *cols, session.workdir(), &miniPath);
  }
  Datastore* dsEval = tsFrame ? tsFrame.get() : (mini ? mini.get() : session.datastore());
  std::string condPy = rewriteExpr(tsPrep.rewrittenIf);
  EvalRowsResult er = evalRows(session, *dsEval, std::nullopt, std::optional<std::string>(condPy));
  if (!er.ok && mini) {
    // A projection miss must cost a fallback, never an answer: re-evaluate
    // against the full frame before believing the failure.
    mini.reset();
    dsEval = session.datastore();
    er = evalRows(session, *dsEval, std::nullopt, std::optional<std::string>(condPy));
  }
  if (!er.ok) return rowErrBlock(action + ": " + er.errMsg, er);

  int64_t n = dsEval->nRows();
  // BS-090: AND a co-occurring `in a/b` into the mask BEFORE the keep/drop
  // split below — for keep-if this restricts to the in-range cond-true
  // rows directly; for drop-if the SAME AND'd mask, then negated by the
  // `== keep` comparison just below, correctly drops only the rows that
  // satisfy BOTH `if` and `in` (Stata's documented if+in intersection
  // semantics), leaving out-of-range rows untouched either way.
  if (inRange.has_value() && !inRange->empty()) {
    piet::handlers::InRangeFilterResult fr;
    if (!applyInRangeToMask(er.mask, n, inRange, fr.refusal)) {
      fr.ok = false;
      return piet::handlers::inRangeRejectError(fr, *inRange, keep ? "keep" : "drop");
    }
  }
  // Rows to KEEP: (mask == keep) — keep-if keeps cond-true rows; drop-if keeps
  // cond-false rows.
  std::vector<char> sel(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) sel[static_cast<size_t>(i)] = (er.mask[static_cast<size_t>(i)] == keep) ? 1 : 0;

  // INSPECT-2608 EXIT — the fast path. The slow path below rewrites EVERY column
  // of the frame as text and re-converts it; what the filter actually needs is a
  // row mask. Push the mask this function already computed as ONE resident f64
  // column, then let the engine do the row slice and delete the column in a
  // single call (`eval --keepif M --drop M`, the shape inrange_filter already
  // uses). Mechanisms are all pre-existing: no new engine op, no contract change.
  //
  // Fallback discipline: ANY failure here falls through to the whole-frame CSV
  // path with nothing lost — this is a pure optimization and a miss must cost a
  // fallback, not an answer. `PIET_NO_RESIDENT_KEEPIF` forces the old path (the
  // A/B arm every probe in this file has).
  if (std::getenv("PIET_NO_RESIDENT_KEEPIF") == nullptr && !residentReadDisabled() &&
      session.currentHbin().has_value()) {
    // A name no Stata identifier can collide with (leading underscore + the
    // reserved prefix this file already uses for scratch), re-checked against
    // the live varlist card anyway.
    const std::string maskVar = "__piet_filtermask";
    bool collides = false;
    if (const auto* card = session.shadowVarlistIfCurrent())
      for (const auto& info : *card)
        if (info.name == maskVar) { collides = true; break; }
    if (!collides) {
      std::string valuesPath = nextPath(session.workdir(), "filtermask") + ".csv";
      bool wrote = false;
      {
        std::ofstream vf(cecon::to_native_path(valuesPath), std::ios::binary);
        if (vf) {
          for (int64_t i = 0; i < n; ++i) vf << (sel[static_cast<size_t>(i)] ? "1\n" : "0\n");
          vf.flush();
          wrote = static_cast<bool>(vf);
        }
      }
      if (wrote) {
        const std::string basePath = *session.currentHbin();
        std::string maskHbin = nextPath(session.workdir(), "filtermask") + ".hbin";
        std::optional<SessionState::ResidentEligible> resident;
        resident.emplace(session);
        CobraRunResult cr = runCobra({"setcol", "--data", basePath, "--var", maskVar, "--values",
                                      valuesPath, "--dtype", "f64", "--new", "--out", maskHbin});
        cecon::native_unlink(valuesPath);
        if (cr.ok) {
          session.load(maskHbin);
          std::string outHbin2 = nextPath(session.workdir(), "filterif") + ".hbin";
          CobraRunResult cr2 = runCobra({"eval", "--data", *session.currentHbin(), "--keepif",
                                         maskVar + " != 0", "--drop", maskVar, "--out", outHbin2});
          if (cr2.ok) {
            resident.reset();
            session.load(outHbin2);
            return piet::handlers::mutationResult(session, "keepif", /*newVars=*/{},
                                                  /*droppedVars=*/{});
          }
          // The slice failed with the mask column already committed. Take the
          // column back out before falling through, so the slow path below does
          // not carry a scratch column into the user's frame.
          std::string cleanHbin = nextPath(session.workdir(), "filterif") + ".hbin";
          CobraRunResult cr3 = runCobra(
              {"eval", "--data", *session.currentHbin(), "--drop", maskVar, "--out", cleanHbin});
          if (cr3.ok) session.load(cleanHbin);
          resident.reset();
          session.invalidateDatastore();
        }
      } else {
        cecon::native_unlink(valuesPath);
      }
    }
  }

  // Write a filtered CSV (all columns, selected rows) -> cobra convert.
  Datastore* ds = session.datastore();
  std::vector<std::string> names = ds->names();
  std::vector<char> isFactor(names.size());
  std::vector<const double*> f64ptrs(names.size(), nullptr);
  std::vector<const int32_t*> codeptrs(names.size(), nullptr);
  std::vector<const std::vector<std::string>*> levelptrs(names.size(), nullptr);
  std::set<std::string> factorOut;
  for (size_t c = 0; c < names.size(); ++c) {
    if (ds->dtype(names[c]) != "f64") {
      isFactor[c] = 1;
      codeptrs[c] = ds->col(names[c]).codes();
      levelptrs[c] = &ds->col(names[c]).levels;
      factorOut.insert(names[c]);
    } else {
      isFactor[c] = 0;
      f64ptrs[c] = ds->col(names[c]).f64();
    }
  }

  std::string csvPath = nextPath(session.workdir(), "filterif") + ".csv";
  {
    std::ofstream csv(cecon::to_native_path(csvPath), std::ios::binary);
    if (!csv) return errBlock(action + ": could not open scratch CSV for write");
    for (size_t c = 0; c < names.size(); ++c) {
      if (c) csv << ",";
      csv << names[c];
    }
    csv << "\n";
    for (int64_t i = 0; i < n; ++i) {
      if (!sel[static_cast<size_t>(i)]) continue;
      for (size_t c = 0; c < names.size(); ++c) {
        if (c) csv << ",";
        if (isFactor[c]) {
          int32_t code = codeptrs[c][i];
          std::string lv = (code >= 0 && static_cast<size_t>(code) < levelptrs[c]->size())
                               ? (*levelptrs[c])[static_cast<size_t>(code)]
                               : "";
          csv << csvEscape(lv);
        } else {
          csv << csvNum(f64ptrs[c][i]);
        }
      }
      csv << "\n";
    }
    // BUG-124 sibling: flush + check (see writeNew's identical guard) — a
    // silently short CSV here would drop kept rows without any error.
    csv.flush();
    if (!csv) {
      cecon::native_unlink(csvPath);
      return errBlock(action + ": failed writing scratch CSV (disk full or I/O error)");
    }
  }

  std::string outHbin = nextPath(session.workdir(), "filterif") + ".hbin";
  std::vector<std::string> args = {"convert", "--data", csvPath, "--out", outHbin, "--delim", ","};
  if (!factorOut.empty()) {
    std::string joined;
    bool first = true;
    for (const auto& nm : names) {
      if (factorOut.count(nm)) {
        if (!first) joined += ",";
        joined += nm;
        first = false;
      }
    }
    args.push_back("--factor");
    args.push_back(joined);
  }
  CobraRunResult cr = runCobra(args);
  cecon::native_unlink(csvPath);
  if (!cr.ok) {
    // BUG-124: same dead-catch hole as writeNew — surface the engine error and
    // remove the torn partial instead of load()ing it (see writeNew's comment).
    cecon::native_unlink(outHbin);
    return errBlock(action + ": error writing hbin: " + errorFromRaw(cr).what());
  }
  if (!cecon::native_exists(outHbin))
    return errBlock(action + ": failed writing output dataset (disk full or I/O error)");

  session.load(outHbin);
  session.invalidateDatastore();
  Datastore* ds2 = session.datastore();
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "keepif";
  ojson varlistAfter = ojson::array();
  auto vlist = session.varlistWithMeta(ds2);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: keep the cache current for the next mutation
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = ds2 ? ds2->nRows() : 0;
  return out;
}

}  // namespace piet::strmutate
