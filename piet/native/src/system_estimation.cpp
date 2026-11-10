// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// System estimation.
#include "piet/handlers.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/settings.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "piet/stata_format.h"
#include "estimation_internal.h"
#include "estimators/posthest.h"  // cecon::margins_parse_at (header-inline)

#include "extended_estimation_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace extended_fit_detail {}
using namespace extended_fit_detail;
using namespace extended_estimation_vocab;

ojson runReg3(const eval::ParsedCommand& parsed, SessionState& session,
              const std::string& stataCmd) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // sureg is `reg3 <eqns> ..., sure` with e(cmd)
  // set to sureg; its isure/dfk/small/corr/constraints are not
  // forwarded by this build, so they are refused by name below, never ignored.
  const bool isSureg = stataCmd == "sureg";
  if (isSureg) {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSureg.known,
                                              kVocabSureg.unimplemented)) {
      return *err;
    }
  }

  // splitWs() already broke each parenthesised equation into separate
  // varlist tokens; rejoin with single spaces and re-split on the paren
  // structure (same rationale as parseIvParenM4 above — Stata's own
  // whitespace inside the parens carries no meaning).
  std::string joined = joinSpaceM4(parsed.varlist);
  std::vector<std::string> groups;
  bool parenOk = true;
  {
    size_t i = 0;
    while (i < joined.size()) {
      while (i < joined.size() && (joined[i] == ' ' || joined[i] == '\t')) ++i;
      if (i >= joined.size()) break;
      if (joined[i] != '(') {
        parenOk = false;
        break;
      }
      size_t close = joined.find(')', i);
      if (close == std::string::npos) {
        parenOk = false;
        break;
      }
      groups.push_back(joined.substr(i + 1, close - i - 1));
      i = close + 1;
    }
  }
  if (!parenOk) {
    return engineErrorBlockM4(
        "syntax_error: reg3 requires each equation in parentheses: (depvar regressors...)",
        "syntax_error");
  }
  if (groups.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: reg3 requires at least two equations: (y1 x1 x2) (y2 x3 y1)",
        "syntax_error");
  }

  std::vector<std::string> eqArgs;
  for (const auto& g : groups) {
    std::vector<std::string> toks = splitTokensM4(g);
    if (toks.size() < 2) {
      return engineErrorBlockM4(
          "syntax_error: malformed reg3 equation `(" + g + ")`: needs a dependent variable and "
          "at least one regressor",
          "syntax_error");
    }
    std::string depvar = toks[0];
    std::vector<std::string> regs(toks.begin() + 1, toks.end());
    eqArgs.push_back(depvar + "=" + joinCommaM4(regs));
  }

  bool sawSure = isSureg, sawExplicit3sls = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "sure" && v.isBool && v.boolValue) sawSure = true;
    if (k == "3sls" && v.isBool && v.boolValue) sawExplicit3sls = true;
  }
  if (sawSure && sawExplicit3sls) {
    return engineErrorBlockM4("syntax_error: `sure` and `3sls` may not be combined", "syntax_error");
  }

  // vce()/robust/cluster/noconstant/level — cobra reg3 has NO --vcov/
  // --no-intercept/--level surface at all (live-verified: all three hard-
  // error "unrecognized option" at cobra's own CLI layer, exit nonzero, but
  // (--no-intercept/--level only, NOT --vcov) still print a full valid
  // stdout JSON first — the SAME cobra_runner.cpp BUG-039 masking shape the
  // ivregress/liml branch above documents; --vcov itself errors cleanly
  // with empty stdout, but is rejected here anyway for a uniform, honest
  // message rather than relying on that distinction). Reject before ever
  // building the argv.
  for (const auto& [k, v] : parsed.opts) {
    (void)v;
    if (k == "vce" || k == "vcov" || k == "robust" || k == "cluster") {
      return engineErrorBlockM4(
          "Option `" + k + "` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (the engine's reg3 estimator has no vcov surface yet — classical "
          "SEs only).",
          "unsupported_option");
    }
  }
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      return engineErrorBlockM4(
          "Option `" + k + "` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "`.",
          "unsupported_option");
    }
    if (k == "level" && !v.isBool) {
      return engineErrorBlockM4(
          "Option `level` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "`.",
          "unsupported_option");
    }
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabReg3.known;
  if (!isSureg) {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
      return *err;
    }
  }

  std::vector<std::string> argv = {"reg3"};
  for (const auto& e : eqArgs) {
    argv.push_back("--eq");
    argv.push_back(e);
  }
  // Stata's OWN default is 3SLS (sure only when explicitly requested) —
  // oracle-verified (D1g, no options, matches --method 3sls exactly) —
  // while cobra's reg3 default is the OPPOSITE (SUR unless --method 3sls is
  // given — oracle D1h/`sure` matches cobra's no-flag default exactly). So
  // --method must ALWAYS be sent explicitly; relying on cobra's own default
  // would silently swap which method every plain `reg3 (...) (...)` (no
  // options) call actually ran.
  argv.push_back("--method");
  argv.push_back(sawSure ? "sure" : "3sls");
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    // --emit-v: the stacked system e(V) (y:x1 ... x3:_cons) that e(b)/e(V)
    // and estout's matrix mode read; not part of argv (a refit flag).
    std::vector<std::string> fitArgv = argv;
    fitArgv.push_back("--emit-v");
    raw = runCobraJsonM4(fitArgv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, "reg3", &session);

  LastModel lm;
  lm.cobraCmd = "reg3";
  if (isSureg) lm.stataCmd = "sureg";
  std::vector<std::string> firstToks = splitTokensM4(groups[0]);
  lm.y = firstToks.empty() ? std::string() : firstToks.front();
  // No single lm.x is fully coherent for a multi-equation system; best-
  // effort populate from the FIRST equation only (test/lincom/margins/
  // predict after reg3 inherit the SAME coarse-approximation caveat noted
  // on the IV path above — a pre-existing, system-wide LastModel
  // simplification, not new here).
  if (firstToks.size() > 1) lm.x.assign(firstToks.begin() + 1, firstToks.end());
  lm.depvar = lm.y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  session.attachFitCovariance(raw);  // e(V): the engine's stacked system covariance
  return block;
}

// ===========================================================================
// boottest (BS-066): wild cluster bootstrap post-estimation, requires a
// preceding vce(cluster ...) fit in session.lastModel().


}  // namespace piet::handlers
