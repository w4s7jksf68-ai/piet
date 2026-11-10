// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// oaxaca: twofold pooled Blinder-Oaxaca decomposition.

#include "piet/handlers/estimation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/distributions.h"  // betai / betaitail / z_critical (shared with cobra)
#include "core/exact_sum.h"  // ExactSum: summarize's accumulation (centile's mean)
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "piet/twoway_opts.h"
#include "estimation_internal.h"
#include "estimators/postest_names.h"  // factor_vars_of / parse_level_ref (the engine's one resolver)
#include "dispatch/registry.h"  // resolveCanonical: the command word as Stata names it
#include "statistics_command_support.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace statcmd_detail;

// ===========================================================================
// AER scope-2 bundle B2 -- `oaxaca` (Ben Jann, SSC 4.1.1)
//
// `oaxaca depvar indepvars, by(groupvar) pooled [relax noisily level(#)]`
//
// ONLY the twofold `pooled` decomposition is computed, because that is the form
// the twelve-paper corpus writes (034: `... sch2-sch531, by(parentedex) relax
// noisily pooled`) and the only form whose inference was VERIFIED before this
// handler existed: internal note oaxaca-se-20260908/REPORT.md reproduces all
// 120 SEs and every e(V)/e(V0) entry of 12 live-Stata fixtures to 3.8e-14, and
// 48 deliberately wrong constructions fail. The threefold DEFAULT (no `pooled`)
// is a NAMED refusal, not a silent fall-through to the twofold numbers: Stata
// answers a different table there, and an rc 0 with the wrong table is the
// cardinal defect class.
//
// What the verification pinned, and this handler must not lose:
//   * `pooled` IMPLIES robust `suest` (oaxaca.hlp: "suest is implied by
//     pooled"), so e(vce)=robust and e(suest)=suest even though nobody typed
//     vce(robust). The engine owns that construction (src/estimators/oaxaca.cpp).
//   * `relax` leaves a dropped coefficient as 0 with SE 0 (NOT missing), and
//     Stata drops the FIRST of two duplicates, keeping the later one
//     (rank_oracle.txt CASE 13) -- the opposite of regress's convention.
//   * Group 1 is the LOWER group value; e(N_1)/e(N_2) are per-group counts.
//
// e(b) is one row whose column names are Stata's `eq:name` spellings
// (`overall:difference`, `explained:x1`, `unexplained:_cons`), so `_b[...]` and
// `_se[...]` resolve by exact match. NAMED DIVERGENCE: PIET's matrix store has
// no separate row/column EQUATION metadata, so `: colnames e(b)` answers the
// joined spelling where Stata answers the bare name plus `: coleq`.
namespace {
const eval::OptVocab& kVocabOaxaca = eval::registerOptVocab(
    {"oaxaca"},
    {{"by", "pooled", "relax", "noisily", "level", "linear", "nolegend"},
     {"swap", "logit", "probit", "nodetail", "adjust", "threefold", "weight", "omega",
      "reference", "split", "svy", "vce", "cluster", "fixed", "suest", "nosuest", "nose",
      "model1", "model2", "x1", "x2", "xb", "eform", "categorical", "detail", "normalize"},
     // oaxaca.ado's syntax line declares one-letter stems the 2-char prefix
     // rule cannot reach: p:ooled l:evel a:djust w:eight o:mega.
     {{"p", "pooled"}, {"l", "level"}, {"a", "adjust"}, {"w", "weight"}, {"o", "omega"}}});

// An engine refusal carrying an exact Stata rc in `detail` ("r(499)") outranks
// the coarse code mapping; `no_obs` is Stata's r(2000); a bad_input with no
// rc is the r(198) class. Never the default engine_error, which reads as rc 1
// and silently breaks `_rc` assertions (the RC defect class this campaign has
// shipped before).
ojson oaxacaEngineError(const CobraError& ce) {
  const std::string msg = "oaxaca: " + cobraErrorMessage(ce);
  static const std::regex reRc(R"(r\((\d+)\))");
  std::smatch m;
  if (std::regex_search(ce.detail, m, reRc)) return aerErrorReason(msg, std::stoi(m.str(1)));
  if (ce.code == "no_obs") return aerErrorReason(msg, 2000);
  if (ce.code == "bad_input" || ce.code == "collinear") return aerErrorReason(msg, 198);
  return aerError(msg, ce.code);
}
}  // namespace

ojson runOaxaca(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!engineHasSubcommand("oaxaca")) return engineTooOld("oaxaca", "oaxaca");
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabOaxaca.known,
                                            kVocabOaxaca.unimplemented)) {
    return ojson(*err);
  }
  std::string by, level;
  bool pooled = false, relax = false, noisily = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "by") by = strutil::trim(v.strValue);
    else if (k == "pooled") pooled = true;
    else if (k == "relax") relax = true;
    else if (k == "noisily") noisily = true;
    else if (k == "level") level = strutil::trim(v.strValue);
    // `linear` is the default model and `nolegend` suppresses a legend this
    // build never prints: both are exact no-ops, accepted without a note.
  }
  if (by.empty()) return aerErrorReason("oaxaca: option by() required", 198);
  if (!pooled) {
    // The threefold default (and every other twofold weighting) is a different
    // table with its own, unverified inference. Say so by name.
    return ojson(eval::refusedCommand("oaxaca", "oaxaca_threefold_default"));
  }
  if (parsed.varlist.empty()) return aerSyntaxError("oaxaca: syntax is `oaxaca depvar indepvars, by(groupvar) pooled`");

  // The corpus form is `... sch2-sch531`: a RANGE inside an estimation varlist.
  // Estimation rows do not opt into the dispatcher's plain-varlist expansion
  // (they legitimately carry `i.x`/`L.x`), so the expansion is asked for HERE,
  // through the one expander PIET has (BUG-371/BUG-450), on the whole list.
  std::vector<std::string> vars = parsed.varlist;
  bool patterned = false;
  for (const auto& t : vars) {
    if (t.find('*') != std::string::npos || t.find('?') != std::string::npos ||
        t.find('-') != std::string::npos || strutil::lower(t) == "_all") {
      patterned = true;
      break;
    }
  }
  if (patterned) {
    std::string spec;
    for (std::size_t i = 0; i < vars.size(); ++i) spec += (i ? " " : "") + vars[i];
    loops::VarlistExpandResult ex = loops::expandVarlist(spec, session);
    if (ex.isError) return ex.error;
    vars = ex.tokens;
  }
  for (const auto& t : vars) {
    // Factor-variable and time-series operators have no engine path here.
    if (t.find('.') != std::string::npos)
      return ojson(eval::refusedCommand("oaxaca", "factor_variable_operators"));
  }
  if (vars.size() < 2)
    return aerErrorReason("oaxaca: at least one regressor is required in this build", 198);
  const std::string y = vars[0];
  std::vector<std::string> xs(vars.begin() + 1, vars.end());

  std::vector<std::string> args = {"oaxaca", "--data", *session.currentHbin(), "--y", y,
                                   "--x", est_detail::joinCommaM4(xs), "--by", by, "--pooled"};
  if (relax) args.push_back("--relax");
  if (!level.empty()) { args.push_back("--level"); args.push_back(level); }
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    args.push_back("--keepif");
    args.push_back(*parsed.ifExpr);
  }

  nlohmann::json raw;
  try {
    raw = runCobraJson(args);
  } catch (const CobraError& ce) {
    return oaxacaEngineError(ce);
  }

  // e(): measured on StataBE (tests/validate/b2_oracle.json ox_* and the
  // oaxaca-se-20260908 captures): e(cmd) e(model) e(N) e(N_1) e(N_2) e(vce)
  // e(suest) e(depvar), plus e(b)/e(V).
  const std::vector<std::string> names = raw["e_b"]["names"].get<std::vector<std::string>>();
  const std::vector<double> beta = jsonDoubles(raw["e_b"]["values"]);
  session.eval.setEScalar("N", static_cast<double>(raw.value("n_obs", 0)));
  session.eval.setEScalar("N_1", static_cast<double>(raw.value("N_1", 0)));
  session.eval.setEScalar("N_2", static_cast<double>(raw.value("N_2", 0)));
  session.eval.setEScalar("level", raw.value("level", 95.0));
  session.eval.setEString("cmd", "oaxaca");
  session.eval.setEString("depvar", y);
  session.eval.setEString("model", "linear");
  session.eval.setEString("vce", "robust");
  session.eval.setEString("suest", "suest");
  session.eval.setEString("by", by);
  session.eval.setEString("title", "Blinder-Oaxaca decomposition");
  session.eval.setEString("properties", "b V");
  auto postM = [&session](const std::string& nm, eval::EvalMatrix m) {
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == nm) { kv.second = std::move(m); return; }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
  };

  // The LastModel is what `_b[]`/`_se[]`/`ereturn list`/outreg2 read; the fit
  // filter is carried so a post-estimation refit reconstructs the same sample.
  LastModel lm;
  lm.cobraCmd = "oaxaca";
  lm.y = y;
  lm.depvar = y;
  lm.x = xs;
  lm.vcov = "robust";
  lm.dataPath = *session.currentHbin();
  est_detail::captureFitFilterM4(lm, args);
  lm.result = ojson(raw);
  session.setLastModel(lm);
  // eb-order T-P3 part 0: a fresh setLastModel clears the previous fit's block
  // and posted e(b)/e(V), so this fit posts its own AFTER it -- the e(b)/e(V)
  // pair, and the estimation block `_b[]`, eststo and esttab read (the same
  // block this handler returns; its coeftable names are the raw eq:name ones,
  // so `_b[overall:difference]` is unchanged, aer_estim2_oracle.log:242).
  {
    eval::EvalMatrix b;
    b.data.push_back(beta);
    b.rownames = {"y1"};
    b.colnames = names;
    postM("b", std::move(b));
    eval::EvalMatrix V;
    for (const auto& row : raw["e_V"]["matrix"]) V.data.push_back(jsonDoubles(row));
    V.rownames = names;
    V.colnames = names;
    postM("V", std::move(V));
  }
  const ojson estBlock = est_detail::cobraToEstimationM4(ojson(raw), "oaxaca", &session);
  session.lastEstBlock = estBlock;

  ojson blocks = ojson::array();
  if (noisily) {
    // `noisily` shows the two group regressions and the pooled one in Stata.
    // Those fits are not surfaced by this build; the decomposition is the same.
    blocks.push_back(displayBlock(
        "(oaxaca noisily: the underlying group and pooled regressions are not displayed "
        "in PIET -- decomposition unaffected)"));
  }
  if (raw.contains("warnings") && raw["warnings"].is_array())
    for (const auto& w : raw["warnings"]) blocks.push_back(displayBlock(w.get<std::string>()));
  blocks.push_back(estBlock);
  return blocks;
}

}  // namespace piet::handlers
