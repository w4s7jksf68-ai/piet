// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/data_values.h"
#include "piet/eval/exprfns.h"  // B3(c): exprHasRunningSum

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>

#include "estimation/estimation_internal.h"  // e(sample) T2: the esample helpers' declarations
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/counters.h"  // PP-L1 use-plane split counters (runUse)
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/storage_type.h"  // BUG-471 T2: --store-type
#include "piet/strmutate.h"      // BUG-1326: assert's string-condition count
#include "core/exact_sum.h"  // cecon::ExactSum: summarize's accumulation (BUG-1013)
#include "estimators/posthest.h"  // fitted design atoms for named absorbed effects
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)
#include "io/columnar.h"  // cecon::hbin_level_unescape: .dta label text (BUG-1372)
#include "piet/strutil.h"
#include "piet/translate.h"
#include "engine/engine_call_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

using namespace engine_call_detail;

// ---------------------------------------------------------------------------
// gen / replace

namespace {

// Splits "NEWVAR = RHS" into {newvar, rhs} (mutate.py's manual split on the
// FIRST '='). Returns {expr,""} unsplit if there is no '='.
std::pair<std::string, std::string> splitAssign(const std::string& expr) {
  auto pos = expr.find('=');
  if (pos == std::string::npos) return {expr, ""};
  std::string lhs = expr.substr(0, pos);
  std::string rhs = expr.substr(pos + 1);
  auto trim = [](std::string s) {
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
  };
  return {trim(lhs), trim(rhs)};
}

}  // namespace

ojson runGen(const std::string& expr, const std::optional<std::string>& ifExpr,
             SessionState& session, const std::string& storeType) {
  if (!session.currentHbin().has_value()) {
    return eval::noDataLoaded();
  }
  std::string outPath = nextPath(session.workdir(), "gen");
  std::string newvar, genExpr;
  std::string genMask;   // B3(c): see exprHasRunningSum -- sum() cannot be cond-folded
  if (ifExpr.has_value() && !ifExpr->empty()) {
    auto [nv, rhs] = splitAssign(expr);
    newvar = nv;
    if (rhs.empty()) rhs = ".";
    // Nor can a draw: the engine evaluates every argument of cond(), as Stata
    // does (BUG-1050), so a folded qualifier would draw on the rows it excludes.
    if (piet::eval::exprHasRunningSum(rhs) || translate::exprHasDrawToken(rhs)) {
      genExpr = newvar + "=" + rhs;
      genMask = *ifExpr;
    } else {
      genExpr = newvar + "=cond((" + *ifExpr + "),(" + rhs + "),.)";
    }
  } else {
    genExpr = expr;
    auto [nv, rhs] = splitAssign(expr);
    newvar = rhs.empty() && expr.find('=') == std::string::npos ? "" : nv;
  }
  // BUG C (W3 fuzzer stabilization, shared with string_generate_replace.cpp::runStringGen
  // and variable_copy_rename.cpp::runRename — piet::strutil::isValidStataIdentifier
  // is the ONE validator all three call): this numeric path used to ship
  // `newvar` straight to `cobra eval --gen` with no check at all, so
  // `gen 9zzz = 1` silently created a column named "9zzz" (cobra's hbin
  // header is a bare JSON string field — it has no opinion on Stata's
  // identifier rule).
  if (!newvar.empty() && !strutil::isValidStataIdentifier(newvar)) {
    return engineErrorBlock(
        "generate: invalid variable name `" + newvar + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");
  }
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin(), "--gen", genExpr,
                                    "--out", outPath};
  if (!genMask.empty()) { args.push_back("--gen-mask"); args.push_back(genMask); }
  // BUG-471 T2: the resolved storage type (the dispatcher's resolver) — the
  // engine stores float(v) / trunc(v), not the full double under a label.
  storage::appendStoreTypeArg(args, storeType);
  // BUG-369: `gen ly = L.y` / `replace y = y - L.y`. The engine resolves L./F./D./S.
  // inside an expression now, but only against tsset/xtset metadata, which does not
  // live in the .hbin — it has to ride the call, exactly as the estimator path has
  // always done (translate::injectTsContext at :1737). Gated on the expression
  // actually carrying an operator so a since-dropped timevar can't break plain gens.
  // The --gen-mask condition is parsed against the same frame, so the
  // context scan reads it too (a mask with `L.z` needs the tsset metadata).
  const std::string ctxExpr = genMask.empty() ? genExpr : genExpr + " " + genMask;
  args = translate::injectTsContextForExpr(args, ctxExpr, session);
  args = translate::injectRngContextForExpr(args, ctxExpr, session);
  // Stage 1b: opt this pure-add mutation into the resident frame plane — its
  // --out becomes the head and is never read back as a file (session.load() +
  // the mutationResult shadow card render it), so the engine advances the
  // resident frame in RAM with no per-verb scratch write.
  SessionState::ResidentEligible resident(session);
  nlohmann::json response;
  try {
    response = runCobraJson(args);
    translate::absorbRngDraws(response, session);
  } catch (const CobraError& ce) {
    // dispatcher.py's generic native_handler wrapper: `except CobraError as
    // ce: make_engine_error(str(ce), code=ce.code)` — str(ce) is the
    // bracket-prefixed "[code] message" (cobra_runner.py's CobraError.__str__).
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  std::vector<std::string> newVars;
  if (!newvar.empty()) newVars.push_back(newvar);
  ojson block = mutationResult(session, "gen", newVars);
  translate::carryStoreOverflow(response, block);  // BUG-1123
  return block;
}

ojson runReplace(const std::string& expr, const std::optional<std::string>& ifExpr,
                 SessionState& session, const std::string& storeType, bool noPromote) {
  if (!session.currentHbin().has_value()) {
    return eval::noDataLoaded();
  }
  auto [varname, rhs] = splitAssign(expr);
  if (rhs.empty() && expr.find('=') == std::string::npos) {
    return engineErrorBlock("replace requires `varname = expr`");
  }
  // BUG C: same shared validator as runGen (see its comment above) — an
  // invalid target name here can only mean a malformed command (a genuinely
  // EXISTING variable was always a valid identifier when it was created),
  // so this is a defensive, low-frequency check kept for symmetry with
  // gen/rename rather than a commonly-hit path.
  if (!strutil::isValidStataIdentifier(varname)) {
    return engineErrorBlock(
        "replace: invalid variable name `" + varname + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");
  }
  // BUG-841: a running sum cannot be cond-folded here either (see runGen):
  // `replace y = sum(x) if z > 0` accumulated the unselected rows into the
  // selected rows' totals at rc 0 (w2m_bymask C09 :361 is the same shape
  // through `_n`). runBysortGen with an empty by-list is this verb in the
  // dataset's context, and it sends the `if` as --gen-mask with --gen-replace
  // and decides the store type from the masked values.
  // A draw cannot be folded either (see runGen): the excluded rows would draw.
  if (ifExpr.has_value() && !ifExpr->empty() &&
      (piet::eval::exprHasRunningSum(rhs) || translate::exprHasDrawToken(rhs))) {
    return runBysortGen(session, expr, {}, ifExpr, false, true, std::nullopt, std::nullopt,
                        storeType, noPromote);
  }
  std::string outPath = nextPath(session.workdir(), "replace");
  std::string genExpr;
  if (ifExpr.has_value() && !ifExpr->empty()) {
    genExpr = varname + "=cond((" + *ifExpr + "),(" + rhs + ")," + varname + ")";
  } else {
    genExpr = varname + "=" + rhs;
  }
  // BUG-471 T2: the type the store takes. An integer-typed variable receiving
  // a value it cannot hold widens by Stata's lattice before the ONE store
  // (replace_byte_promote :810 ... replace_int_overflow :893); the decision reads
  // the touched values through a read-only engine eval of the same expression
  // into a scratch column of a projection — no dispatch(), so r()/e() are what
  // they were. Engine calls per replace: float/double/unrecorded target 1; an
  // integer target under nopromote, or with a literal the type holds, or with an
  // unqualified literal, 1; any other integer target 2 (probe + store), 3 when
  // the head is resident (+ the column fetch).
  std::string finalStoreType = storeType;
  {
    const bool qualified = ifExpr.has_value() && !ifExpr->empty();
    const std::string probeRhs =
        qualified ? "cond((" + *ifExpr + "),(" + rhs + "),.)" : rhs;
    storage::ReplaceTypeDecision dec = storage::decideReplaceStoreType(
        session, storeType, noPromote, varname, rhs, qualified, "--gen",
        std::string(storage::kReplaceProbeVar) + "=" + probeRhs,
        {rhs, ifExpr.value_or(std::string())}, /*drawContext=*/true);
    if (dec.error.has_value()) return *dec.error;
    finalStoreType = dec.storeType;
  }
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin(), "--gen", genExpr,
                                    "--out", outPath};
  storage::appendStoreTypeArg(args, finalStoreType);
  // BUG-369: `gen ly = L.y` / `replace y = y - L.y`. The engine resolves L./F./D./S.
  // inside an expression now, but only against tsset/xtset metadata, which does not
  // live in the .hbin — it has to ride the call, exactly as the estimator path has
  // always done (translate::injectTsContext at :1737). Gated on the expression
  // actually carrying an operator so a since-dropped timevar can't break plain gens.
  args = translate::injectTsContextForExpr(args, genExpr, session);
  // `replace x = runiform()` draws from the session's Stata stream like generate
  // (StataBE: after `set seed 5` it takes the stream's first draw, impl_C
  // stata_probe P8); it used to carry no stream at all, so every such replace
  // drew the same fixed Philox column whatever `set seed` said.
  args = translate::injectRngContextForExpr(args, genExpr, session);
  // Stage 1b: opt this in-place rewrite into the resident frame plane (see
  // runGen) — its --out becomes the head, never read back as a file.
  SessionState::ResidentEligible resident(session);
  try {
    translate::absorbRngDraws(runCobraJson(args), session);
  } catch (const CobraError& ce) {
    // Nothing was stored, so the label stays the type the column still holds.
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  storage::recordReplaceType(session, varname, storeType, finalStoreType);
  // F9: replace rewrites ONLY `varname` in place — name it so the nMiss seed
  // carries every other column instead of a full rescan. rewrittenVars is
  // the shared mutationResult's 5th (LAST) parameter, not its 3rd — pass
  // newVars={} and droppedVars={} explicitly so `varname` lands in the
  // rewrittenVars slot, not droppedVars (the consolidation hazard this
  // call site is the one real instance of: this file's old local
  // mutationResult had rewrittenVars in position 3; the shared signature's
  // position 3 is droppedVars).
  return mutationResult(session, "gen", {}, {}, {varname});
}

}  // namespace piet::handlers
