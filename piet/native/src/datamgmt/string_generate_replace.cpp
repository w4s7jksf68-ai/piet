// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// String generate / replace through the row evaluator (runStringGen,
// runStringReplace): expression typing, storage rounding and the setcol writer.
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

namespace {

// Which kinds of value (string, numeric) the expression produced. It replaces
// _result_is_string (string-valued if ANY cond-true value is a str), which said
// nothing when the mask kept no row.
// BUG-1122 (FU-1338): Stata types an expression once, not row by row, and a
// generate or replace whose expression is not of its variable's type is r(109)
// type mismatch with nothing written (StataBE 19.5, measured 2026-11-03:
// `generate double x = s`, `generate str3 z = x`, `replace x = "a"` on a numeric
// x and `replace s = 1` on a string s; also under `if 0`, where no row is
// evaluated, and an untyped `generate z = s if 0` is a str1 of empty strings).
// The rows the mask kept say which types the expression produced; when it kept
// none, the expression is evaluated once more without the mask. If that
// evaluation fails with r(109), the expression itself is the type mismatch,
// whatever the mask: `generate z = x + "a" if 0`, `generate z = s + 1 if x > 5`
// and `replace s = s + 1 if 0` are r(109) in Stata, with nothing written, as
// their unmasked twins are. Neither set (no observations, or that evaluation
// failed some other way) refuses nothing on a guess. Both set is an expression
// whose rows disagree, which Stata never types: `generate t = cond(x>1, s, .)`
// is r(109) as well. The rows a mask excluded are not read, so `replace s =
// cond(x>1, s, 1) if x > 1` still passes where Stata refuses it.
struct ExprKinds {
  bool str = false;
  bool num = false;
  bool mismatch = false;  // the unmasked evaluation raised r(109)
};

ExprKinds exprKinds(SessionState& session, Datastore& ds, const EvalRowsResult& er,
                    const std::optional<std::string>& rhsPy,
                    const std::optional<std::string>& condPy) {
  ExprKinds k;
  for (size_t i = 0; i < er.values.size(); ++i) {
    if (!er.mask[i] || !er.valueIsSet[i]) continue;
    if (er.values[i].isStr()) k.str = true;
    else k.num = true;
  }
  if (k.str || k.num || !condPy.has_value() || er.values.empty()) return k;
  const EvalRowsResult all = evalRows(session, ds, rhsPy, std::nullopt);
  if (all.ok) {
    for (size_t i = 0; i < all.values.size(); ++i) {
      if (!all.valueIsSet[i]) continue;
      if (all.values[i].isStr()) k.str = true;
      else k.num = true;
    }
  } else {
    k.mismatch = all.errReason == "r(109)";
  }
  return k;
}

ojson typeMismatchBlock() {
  return eval::errorBlock("engine_error", "type mismatch", std::nullopt, std::nullopt,
                          std::nullopt, std::string("r(109)"));
}

// The value a numeric column of Stata storage type `t` holds for `v`: the rules of
// the engine's cecon::frame_detail::round_to_store (src/data/frame_internal.h, not
// on PIET's include path), which `cobra eval --store-type` applies on the numeric
// path (BUG-471 T1). This path writes through `cobra setcol`, which takes no type,
// so `generate clk = clock(s,"MDYhm")` claimed float and stored the double:
// 1894703400000 where Stata stores 1894703431680, and mm() of it was 59 where
// Stata says 58 (w2y_fn_datetime fd_clock_mdyhm_f, fd_mm_float). Float is
// (double)(float)v, past the float range missing; byte/int/long truncate toward
// zero, out of range missing; "" and double are the identity. Missing stays.
double storeAs(double v, const std::string& t) {
  if (t.empty() || t == "double" || std::isnan(v) || std::isinf(v)) return v;
  const double na = std::numeric_limits<double>::quiet_NaN();
  if (t == "float") {
    if (std::fabs(v) >= 0x1p+127) return na;
    const double f = static_cast<double>(static_cast<float>(v));
    return std::fabs(f) > 0x1.fffffep+126 ? na : f;
  }
  double lo = -2147483647.0, hi = 2147483620.0;  // long
  if (t == "byte") { lo = -127.0; hi = 100.0; }
  else if (t == "int") { lo = -32767.0; hi = 32740.0; }
  else if (t != "long") return v;
  const double r = std::trunc(v);
  if (r < lo || r > hi) return na;
  return r + 0.0;
}

// _write_new: write a fresh hbin with `varname` set from the evaluated rows.
// C++ path uses a CSV round-trip through `cobra convert` (piet/native has no
// hbin-write API; this mirrors postfile.cpp::runPostclose's established
// pattern). replace_existing: rows where cond is false keep the column's
// existing value (replace); else become missing (gen).

ojson writeNew(SessionState& session, Datastore& ds, const std::string& varname,
               const EvalRowsResult& er, bool isString, bool replaceExisting,
               const std::string& action, const std::vector<std::string>& newVars,
               const std::string& dataPath, const std::string& storeType = std::string(),
               const std::string& replaceResolvedType = std::string(), bool noPromote = false,
               int64_t* truncated = nullptr) {
  int64_t n = ds.nRows();

  // Existing column view (for replace's cond-false keep-original path).
  bool varExists = ds.has(varname);
  bool existingIsFactor = varExists && ds.dtype(varname) != "f64";
  const double* existingF64 = (varExists && !existingIsFactor) ? ds.col(varname).f64() : nullptr;
  const int32_t* existingCodes = existingIsFactor ? ds.col(varname).codes() : nullptr;
  const std::vector<std::string>* existingLevels =
      existingIsFactor ? &ds.col(varname).levels : nullptr;

  // Compute the new column's per-row output.
  std::vector<std::string> outStrings;  // isString path
  std::vector<double> outNums;          // numeric path
  int64_t storeOverflow = 0;            // values storeAs turned into missing (BUG-1123)
  counters::strmutPhases().calls += 1;
  {
  PhaseTimer _ptOut(&counters::strmutPhases().outbuildNs);
  if (isString) {
    outStrings.resize(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
      size_t si = static_cast<size_t>(i);
      if (er.mask[si] && er.valueIsSet[si]) {
        outStrings[si] = coerceStr(er.values[si]);
      } else if (replaceExisting && existingIsFactor) {
        int32_t code = existingCodes[i];
        outStrings[si] = (code >= 0 && static_cast<size_t>(code) < existingLevels->size())
                             ? (*existingLevels)[static_cast<size_t>(code)]
                             : "";
      } else if (replaceExisting && existingF64 != nullptr) {
        double fv = existingF64[i];
        outStrings[si] = std::isnan(fv) ? "" : coerceStr(PyValue::floating(fv));
      } else {
        outStrings[si] = "";  // gen, cond false -> missing string
      }
    }
  } else {
    outNums.resize(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
      size_t si = static_cast<size_t>(i);
      if (er.mask[si] && er.valueIsSet[si]) {
        const double v = coerceNum(er.values[si]);
        outNums[si] = storeAs(v, storeType);
        if (std::isnan(outNums[si]) && !std::isnan(v)) ++storeOverflow;
      } else if (replaceExisting && existingF64 != nullptr) {
        outNums[si] = existingF64[i];
      } else {
        outNums[si] = std::nan("");
      }
    }
  }
  }  // end outbuild phase

  // A declared `strN` (varMeta.stataType, recorded by the dispatcher before a
  // typed generate runs) bounds the value in BYTES: `generate str3 r = upper(s)`
  // keeps "HEL" (w2x_fn_string_math fx_upper_str3, fx_substr_str3). A replace
  // whose longest value outgrows the declared width promotes the type instead,
  // as Stata does: str2 -> str17 (fx_subinstr_rep); past 2045 bytes it is strL.
  //
  // FU-936 (format-display-default-lifecycle): an UNTYPED string generate (or
  // `generate str`) is as wide as its longest value, at least str1, and
  // strL past 2045 bytes: `generate su2 = "y" + "x"` is str2, and its default
  // format follows the width (%14s for a str14; w2af_format_lifecycle.log
  // af_gen_str_untyped2, af_gen_str_untyped14, af_gen_str_untyped_var). With
  // no width recorded, `: type` answered r(101) and every width displayed %9s.
  //
  // BUG-1143 (FU-1050, FU-1103): under replace's `nopromote` the str# stays and
  // each value written longer than it is truncated, as for a typed generate;
  // `truncated` counts them for Stata's "(N values truncated because of storage
  // type)" note (StataBE 19.5, measured 2026-11-03: `replace v = upper("hello")
  // in 1, nopromote` on a str2 keeps str2 "HE" and prints the note for 1 value;
  // a strL target holds any value and truncates none).
  std::optional<std::string> promotedType;
  if (isString) {
    auto mit = session.varMeta.find(varname);
    const std::string declared =
        (mit != session.varMeta.end() && mit->second.stataType) ? *mit->second.stataType : "";
    static const std::regex reStrN(R"(^str([0-9]+)$)");
    std::smatch sm;
    size_t longest = 0;
    for (const auto& v : outStrings) longest = std::max(longest, v.size());
    if (std::regex_match(declared, sm, reStrN)) {
      const size_t width = static_cast<size_t>(std::stoul(sm[1].str()));
      if (!replaceExisting || noPromote) {
        for (size_t i = 0; i < outStrings.size(); ++i) {
          if (outStrings[i].size() <= width) continue;
          outStrings[i].resize(width);
          if (truncated != nullptr && er.mask[i] && er.valueIsSet[i]) ++*truncated;
        }
      } else if (longest > width) {
        promotedType = longest > 2045 ? std::string("strL") : "str" + std::to_string(longest);
        // A held format equal to the old width's default follows the new width
        // (prepareStorageChange; af_replace_str12_str14 `str14|%14s`).
        VarInfo before;
        before.name = varname;
        before.dtype = "factor";
        session.prepareStorageChange(before);
      }
    } else if (!replaceExisting && declared != "strL") {
      promotedType = longest > 2045 ? std::string("strL")
                                    : "str" + std::to_string(std::max<size_t>(1, longest));
    }
  }

  // Hand the engine's single-column `cobra setcol` verb just the one new/changed
  // column instead of re-serializing the WHOLE frame to CSV + `cobra convert`
  // (which re-factorizes every untouched factor column). The per-row
  // outStrings/outNums above already carry every gen/replace semantic; every
  // OTHER column/dictionary byte is preserved. isString => --dtype factor: a
  // genuine "" and a missing row both become an empty payload line -> setcol
  // missing (code -1), the SAME mapping the old `cobra convert` empty-factor-
  // cell path produced, so no ""-level is introduced. target present (replace)
  // => in-place patch; target new (gen) => --new appends.
  bool targetPresent = ds.has(varname);
  std::string valuesPath = nextPath(session.workdir(), "strmut") + ".csv";
  {
    PhaseTimer _ptCsv(&counters::strmutPhases().csvwriteNs);
    std::ofstream csv(cecon::to_native_path(valuesPath), std::ios::binary);
    if (!csv) return errBlock("gen/replace: could not open scratch CSV for write");
    for (int64_t i = 0; i < n; ++i) {
      size_t si = static_cast<size_t>(i);
      if (isString) csv << csvEscape(outStrings[si]);
      else csv << csvNum(outNums[si]);
      csv << "\n";
    }
    // BUG-124 sibling (BUG-109's writer-side precedent applied to this PIET
    // writer): an ofstream swallows ENOSPC/IO errors into badbit silently — a
    // short payload would setcol into an honest row-count mismatch (or, worse, a
    // silently truncated column). Flush and check before handing it to the engine.
    csv.flush();
    if (!csv) {
      cecon::native_unlink(valuesPath);
      return errBlock("gen/replace: failed writing scratch CSV (disk full or I/O error)");
    }
  }
  // FLOOR (1) site class IPC, `semantic_zero` class (counters.h): bill the
  // scratch CSV this process wrote for the single-column `cobra setcol` below
  // (destring/tostring precedent, destring.cpp) — the `--out` hbin
  // is billed once at the COLUMNAR site via Datastore, not here.
  {
    std::error_code cec;
    auto csvSz = std::filesystem::file_size(cecon::to_native_path(valuesPath), cec);
    counters::addIpcBytes(cec ? 0 : static_cast<int64_t>(csvSz), /*setcolShaped=*/true);
  }

  // MECHOPT W3 (STRMUT-RESIDENT): the C4-L4 setcol residency switch, exactly
  // the destring/tostring/encode/clonevar resolution — one env for the
  // C2-specific escape, the C4-family escape disables it alongside. On the
  // resident arm the engine's cmd_setcol runs over the frame-run channel
  // (acquire_data serves the cached resident view, commit_frame stages instead
  // of writing), so the per-call FULL-FRAME `--out` hbin write — 2,560 of them
  // per acceptance-corpus run, 283 s of engine wait — never happens, and
  // neither does the PIET-side re-open chain (rl_load) that used to read it
  // back. The entry side is deliberately unchanged: evalRows reads real row
  // values, so the head materialization that built `ds` stays.
  const bool residentSetcol =
      std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr && !residentReadDisabled();

  std::string outHbin = nextPath(session.workdir(), "strmut") + ".hbin";
  std::vector<std::string> args = {"setcol",   "--data",   dataPath,
                                   "--var",    varname,    "--values",
                                   valuesPath, "--dtype",  isString ? "factor" : "f64",
                                   "--out",    outHbin};
  if (!targetPresent) args.push_back("--new");
  std::optional<SessionState::ResidentEligible> setcolResident;
  if (residentSetcol) setcolResident.emplace(session);
  CobraRunResult cr = runCobra(args);
  cecon::native_unlink(valuesPath);
  if (!cr.ok) {
    // BUG-124 (the crash): a failed setcol (ENOSPC, engine error) can leave a
    // partially-written outHbin (the engine's writer creates the file before it
    // can detect the failure). The old code discarded runCobra's result (the
    // catch was dead), so a torn file passed the exists() check below, got
    // load()ed + mmap'd, and the next varlist scan walked off the mapping
    // (SIGSEGV KERN_INVALID_ADDRESS at region end — the fair-run .ips).
    // Surface the engine's real error and remove the torn partial.
    cecon::native_unlink(outHbin);
    return errBlock(std::string("gen/replace: error writing hbin: ") + errorFromRaw(cr).what());
  }
  // BUG-124 torn-write guard: STATELESS ARM ONLY. The resident arm never writes
  // an --out file (the router strips it; the engine stages a pending frame), so
  // a native_exists probe there would turn every resident string-gen into a
  // false "disk full" — the destring precedent's exact clause.
  if (!residentSetcol && !cecon::native_exists(outHbin)) {
    return errBlock("gen/replace: failed writing output dataset (disk full or I/O error)");
  }
  if (promotedType) session.varMeta[varname].stataType = *promotedType;
  // FU-849: a numeric replace records the lattice's widening (and the creation
  // format) exactly where the numeric path does, before the varlist is built.
  if (!isString && !replaceResolvedType.empty())
    storage::recordReplaceType(session, varname, replaceResolvedType, storeType);

  if (residentSetcol) {
    // MECHOPT W3, resident arm: record the virtual head and fold the delta
    // through mutationResult — the same seam every other resident mutation
    // rides (tryResidentMutationCard consumes the engine's vars_delta; no
    // datastore re-materialization, no varlist rebuild by hand). Calling the
    // old hand-rolled tail here would `session.datastore()` the virtual head
    // back into a real file and lose everything the plane bought — the seam
    // review's named trap.
    session.load(outHbin);
    ojson residentOut =
        targetPresent
            ? piet::handlers::mutationResult(session, action, /*newVars=*/{}, /*droppedVars=*/{},
                                             /*rewrittenVars=*/{varname})
            : piet::handlers::mutationResult(session, action, {varname});
    // C4-L4 sibling of destring's memo: the values THIS process just computed
    // and committed, so a chained single-column consumer (destring/encode of
    // the value we just wrote) never re-materializes the virtual head to learn
    // what PIET already knew. After mutationResult — the fold's own vars_delta
    // pass erases the name first.
    if (session.residentHeadDirty()) {
      SessionState::ResidentComputedCol put;
      put.nRows = n;
      if (isString) {
        put.dtype = "factor";
        put.strs = std::move(outStrings);
      } else {
        put.dtype = "f64";
        put.f64 = std::move(outNums);
      }
      session.residentComputedColPut(varname, std::move(put));
    }
    if (!replaceExisting && storeOverflow > 0) residentOut["store_overflow"] = storeOverflow;
    return residentOut;
  }

  // Everything from here to the return is the POST-engine phase: re-open the
  // patched hbin, rebuild the varlist card, seed nMiss. Declared AFTER the
  // error returns above so only the success path is billed, and it lives to end
  // of function, so `reload` covers the whole tail.
  PhaseTimer _ptReload(&counters::strmutPhases().reloadNs);
  Datastore* ds2 = nullptr;
  {
    // rlLoad: re-open the hbin the engine just patched. Frame-load
    // materialization tax — the sub-phase C3 descriptor carry-over targets.
    PhaseTimer _ptLoad(&counters::strmutPhases().rlLoadNs);
    // ROWLOOP-RLLOAD-SUBSPLIT: arm the {materialize, open, reclaim, close}
    // sub-timers for exactly this scope. session.load()/datastore() are also
    // called by `use`, the dispatcher and other verbs, so an unarmed global
    // sub-total would not be a partition of rl_load (counters.h RlArm).
    counters::RlArm _rlArm;
    // ALLOC-C3A-ARC1-CARRY: hand the parse we are about to throw away to the
    // open that is about to rebuild it. `setcol` rewrote exactly ONE column and
    // left every other column's dictionary byte-identical (contract_A §28), so
    // re-lexing 2.4-2.8 MB of level strings — 99% of them belonging to columns
    // nothing touched — is work this frame has already done.
    //
    // Raised AFTER the native_exists guard above (brief §5-3): if the engine
    // never wrote the file, the guard has already returned and no carry is in
    // flight to be consumed against a file that does not exist. Scoped to this
    // block, so the descriptor cannot outlive the one load it describes.
    cecon::CarryDelta _carryDelta;
    _carryDelta.kind = targetPresent ? cecon::CarryKind::Replace : cecon::CarryKind::Append;
    _carryDelta.name = varname;
    _carryDelta.dtype = isString ? cecon::DType::Factor : cecon::DType::F64;
    SessionState::CarryArm _carryArm(session, outHbin, _carryDelta);
    session.load(outHbin);
    session.invalidateDatastore();
    ds2 = session.datastore();
  }
  // C1-CARD-REBUILD-REDUCE: seed the unchanged columns' missing-counts BEFORE
  // the varlist rebuild below, so each such column's buildVarInfo skips its
  // O(nrows) NaN/negative-code scan (and the whole-column mmap fault-in that
  // dominates the wide-frame "card tax"). `setcol` above patched ONLY `varname`
  // and left every other column — data AND dictionary — byte-identical, which
  // is exactly the pure-add / in-place-rewrite shape the NUMERIC gen/replace
  // path already carries through mutationResult -> seedNMissForPureAdd
  // (C2-F13a): string gen/replace simply never reached that seed because it
  // rebuilds by hand instead of through mutationResult. gen (newVars={varname})
  // hits branch (1) — carry all, scan the new column fresh; replace (newVars={},
  // rewrittenVars={varname}) hits branch (2) — carry all but the rewritten
  // target. Same PIET_NO_NMISS_CACHE A/B correctness gate as every other seed
  // site; freeze --check byte-compares varlist_after (n_missing included) as the
  // parapet. Runs before updateNMissCache below, so it reads the PRE-mutation
  // counts (this op's refresh happens at line ~820).
  std::vector<std::string> seedRewritten;
  if (replaceExisting) seedRewritten.push_back(varname);
  {
    PhaseTimer _ptNmiss(&counters::strmutPhases().rlNmissNs);
    session.seedNMissForPureAdd(ds2, action, newVars, /*droppedVars=*/{}, seedRewritten);
  }
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = action;
  ojson varlistAfter = ojson::array();
  PhaseTimer _ptVarlist(&counters::strmutPhases().rlVarlistNs);
  auto vlist = session.varlistWithMeta(ds2);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: keep the cache current for the next mutation
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = ds2 ? ds2->nRows() : 0;
  if (!newVars.empty()) out["new_vars"] = newVars;
  // BUG-1123: generate's note counts the expression's missing values, not the ones
  // the storage type could not hold (the numeric path's engine `store_overflow`);
  // the dispatcher's appendGenMissingNote takes these off and drops the field.
  if (!replaceExisting && storeOverflow > 0) out["store_overflow"] = storeOverflow;
  return out;
}

std::optional<std::pair<std::string, std::string>> splitAssign(const std::string& expr) {
  auto pos = expr.find('=');
  if (pos == std::string::npos) return std::nullopt;
  return std::make_pair(trim(expr.substr(0, pos)), trim(expr.substr(pos + 1)));
}

}  // namespace

ojson runStringGen(SessionState& session, const std::string& expr,
                   const std::optional<std::string>& ifExpr, bool strTyped,
                   const std::string& storeType, bool numTyped,
                   const std::optional<std::string>& inRange) {
  if (!session.hasData()) return errBlock("generate: no dataset in memory");
  auto parts = splitAssign(expr);
  if (!parts.has_value()) return errBlock("generate: syntax is `gen newvar = expr`");
  std::string varname = parts->first, rhs = parts->second;
  // BUG C: shared validator (piet::strutil::isValidStataIdentifier) — see its
  // header comment. Was a TU-local `identRe()` regex here (no length cap,
  // and the numeric `gen` path never called it at all); now the ONE point.
  if (!strutil::isValidStataIdentifier(varname)) {
    return errBlock(
        "generate: invalid variable name `" + varname + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");
  }
  // ONESHOT-GETCOLS B2. The entrance used to be `session.datastore()`, which flushes
  // the WHOLE resident frame to disk so the row-wise evaluator below can read the two
  // or three columns `rhs`/`if` name — 154+42 of 350 head materializations on the
  // reference slice, ~161 MB each. Fetch just those columns instead (contract_A §38).
  //
  // Every failure falls back to the old path with nothing lost: this is a pure
  // optimization, and a miss must cost a fallback rather than an answer. `gen`'s
  // varname is deliberately NOT in the fetch list — it must not exist yet, and the
  // projection's `has()` returning false is exactly the check below.
  // BUG-370: resolve any L./F./D./S. in the `if` into real columns first (see
  // materializeTsIf). `tsPrep.evalPath` is empty for every predicate without one,
  // which is the path everything below already took.
  TsIfPrep tsPrep = materializeTsIf(session, ifExpr.value_or(""), inRange);
  if (tsPrep.failed) return errBlock("generate: " + tsPrep.errMsg, tsPrep.errCode);
  const std::optional<std::string> effIf =
      ifExpr.has_value() ? std::optional<std::string>(tsPrep.rewrittenIf) : std::nullopt;
  std::string miniPath;
  std::unique_ptr<Datastore> mini;
  std::unique_ptr<Datastore> tsFrame;
  if (!tsPrep.evalPath.empty()) {
    // The operator columns live only in this scratch frame, so evaluation reads it
    // whole rather than a projection. `dataPath` below stays the real head — the
    // write must land on the user's frame, never on the scratch.
    tsFrame = std::make_unique<Datastore>(tsPrep.evalPath);
  } else if (auto cols = referencedColumns(session, {&rhs, effIf.has_value() ? &*effIf : nullptr}, "")) {
    mini = fetchColumns(session, *cols, session.workdir(), &miniPath);
  }
  Datastore* ds = tsFrame ? tsFrame.get() : (mini ? mini.get() : session.datastore());
  const std::string dataPath = (tsFrame || mini)
                                   ? session.currentHbin().value_or(ds->path())
                                   : ds->path();
  // On the fetch path the projection cannot answer "does varname exist?" — it was
  // deliberately not requested. The shadow card can, and it is the same card the
  // fetch list was proved against, so the two agree by construction.
  bool alreadyDefined;
  if (tsFrame) {
    // The scratch frame carries every user column plus the temps, so it can answer
    // this directly — and it must, since the temps are not on the card.
    alreadyDefined = ds->has(varname);
  } else if (mini) {
    alreadyDefined = false;
    if (const auto* card = session.shadowVarlistIfCurrent())
      for (const auto& info : *card)
        if (info.name == varname) { alreadyDefined = true; break; }
  } else {
    alreadyDefined = ds->has(varname);
  }
  if (alreadyDefined) return errBlock("variable " + varname + " already defined");

  std::optional<std::string> rhsPy = rewriteExpr(rhs);
  std::optional<std::string> condPy =
      effIf.has_value() ? std::optional<std::string>(rewriteExpr(*effIf)) : std::nullopt;

  EvalRowsResult er = evalRows(session, *ds, rhsPy, condPy);
  if (!er.ok) return rowErrBlock("generate: " + er.errMsg, er);

  const ExprKinds kinds = exprKinds(session, *ds, er, rhsPy, condPy);
  if (kinds.mismatch || (kinds.str && kinds.num) || (strTyped && kinds.num) ||
      (numTyped && kinds.str))
    return typeMismatchBlock();
  bool isString = strTyped || kinds.str;
  return writeNew(session, *ds, varname, er, isString, /*replaceExisting=*/false, "gen", {varname},
                  dataPath, storeType);
}

ojson runStringReplace(SessionState& session, const std::string& expr,
                       const std::optional<std::string>& ifExpr, bool strTyped,
                       bool noPromote, std::vector<std::string>* notes,
                       const std::optional<std::string>& inRange) {
  if (!session.hasData()) return errBlock("replace: no dataset in memory");
  auto parts = splitAssign(expr);
  if (!parts.has_value()) return errBlock("replace requires `varname = expr`");
  std::string varname = parts->first, rhs = parts->second;
  // ONESHOT-GETCOLS B2 — see runStringGen for why. `replace` DOES include varname in
  // the fetch: writeNew reads its existing values for the cond-false keep-original
  // path, and its dtype decides the string/numeric branch below.
  // BUG-370 — see runStringGen above for the shape and why the scratch frame is
  // not adopted as the head.
  TsIfPrep tsPrep = materializeTsIf(session, ifExpr.value_or(""), inRange);
  if (tsPrep.failed) return errBlock("replace: " + tsPrep.errMsg, tsPrep.errCode);
  const std::optional<std::string> effIf =
      ifExpr.has_value() ? std::optional<std::string>(tsPrep.rewrittenIf) : std::nullopt;
  std::string miniPath;
  std::unique_ptr<Datastore> mini;
  std::unique_ptr<Datastore> tsFrame;
  if (!tsPrep.evalPath.empty()) {
    tsFrame = std::make_unique<Datastore>(tsPrep.evalPath);
  } else if (auto cols = referencedColumns(
                 session, {&rhs, effIf.has_value() ? &*effIf : nullptr}, varname)) {
    mini = fetchColumns(session, *cols, session.workdir(), &miniPath);
  }
  if (mini && !mini->has(varname)) mini.reset();  // varname absent => not the target frame
  Datastore* ds = tsFrame ? tsFrame.get() : (mini ? mini.get() : session.datastore());
  const std::string dataPath = (tsFrame || mini)
                                   ? session.currentHbin().value_or(ds->path())
                                   : ds->path();
  if (!ds->has(varname)) return errBlock("variable " + varname + " not found");

  std::optional<std::string> rhsPy = rewriteExpr(rhs);
  std::optional<std::string> condPy =
      effIf.has_value() ? std::optional<std::string>(rewriteExpr(*effIf)) : std::nullopt;

  EvalRowsResult er = evalRows(session, *ds, rhsPy, condPy);
  if (!er.ok) return rowErrBlock("replace: " + er.errMsg, er);

  bool targetIsFactor = ds->dtype(varname) != "f64";
  const ExprKinds kinds = exprKinds(session, *ds, er, rhsPy, condPy);
  if (kinds.mismatch || (targetIsFactor ? kinds.num : kinds.str)) return typeMismatchBlock();
  bool isString = strTyped || targetIsFactor;
  // The type a numeric store takes: the variable's own (storage::
  // resolveReplaceType -- a float stays float, w2e_float_default replace_float_23),
  // widened by Stata's lattice (storage::promotedType) when an integer type
  // receives a value it cannot hold, as the numeric path does
  // (replace_byte_promote :810: a fraction into a byte makes it float). FU-849:
  // this path used to take only the float case, so `replace b = real("0.1")` on
  // a byte kept the byte label and stored the double 0.1. The touched values are
  // the rows this replace writes (the mask), non-missing ones only, as the
  // numeric path's probe summarizes them. Under `nopromote` the type stays and
  // storeAs truncates toward zero, out of range missing, as
  // storage::decideReplaceStoreType does on the numeric path
  // (replace_byte_nopromote :827: 0.1 into a byte stays byte, value 0).
  std::string resolvedType, storeType;
  if (!isString) {
    resolvedType = storage::resolveReplaceType(session, varname);
    storeType = resolvedType;
    if (storage::isIntegerType(resolvedType) && !noPromote) {
      bool any = false, anyFraction = false;
      double lo = 0.0, hi = 0.0;
      for (size_t i = 0; i < er.values.size(); ++i) {
        if (!er.mask[i] || !er.valueIsSet[i]) continue;
        const double v = coerceNum(er.values[i]);
        if (std::isnan(v)) continue;
        if (!any) lo = hi = v;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        anyFraction = anyFraction || v != std::trunc(v);
        any = true;
      }
      if (any) storeType = storage::promotedType(resolvedType, anyFraction, lo, hi);
    }
  }
  // BUG-1143 (FU-1050, FU-1103): the integer store under `nopromote` changes a
  // written value two ways, and Stata counts each: a fraction it truncates, and
  // a value outside the type's range it makes missing (StataBE 19.5, measured
  // 2026-11-03: `replace b = real("2.7"), nopromote` on a byte prints "(3 values
  // truncated because of storage type)", `replace i = real("1e6"), nopromote`
  // on an int "(3 values changed to missing because of storage type)", and one
  // replace doing both prints both lines in that order; 100.5 into a byte is
  // 100, truncated; a float target and a missing value print neither).
  int64_t truncated = 0, toMissing = 0;
  if (!isString && noPromote && storage::isIntegerType(storeType)) {
    for (size_t i = 0; i < er.values.size(); ++i) {
      if (!er.mask[i] || !er.valueIsSet[i]) continue;
      const double v = coerceNum(er.values[i]);
      if (std::isnan(v)) continue;
      const double stored = storeAs(v, storeType);
      if (std::isnan(stored)) ++toMissing;
      else if (stored != v) ++truncated;
    }
  }
  ojson out = writeNew(session, *ds, varname, er, isString, /*replaceExisting=*/true, "gen", {},
                       dataPath, storeType, resolvedType, noPromote, &truncated);
  if (notes != nullptr && out.value("type", std::string()) != "error") {
    auto note = [notes](int64_t k, const std::string& what) {
      if (k > 0)
        notes->push_back("(" + std::to_string(k) + (k == 1 ? " value " : " values ") + what +
                         " because of storage type)");
    };
    note(truncated, "truncated");
    note(toMissing, "changed to missing");
  }
  return out;
}

}  // namespace piet::strmutate
