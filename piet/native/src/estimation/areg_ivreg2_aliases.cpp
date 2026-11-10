// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// areg, ivreg2 and xtivreg2: option normalisation, then the existing estimators.
//
//   E1  aliases onto estimators PIET already has. `areg` IS regress with an
//       absorb(); `ivreg2`/`xtivreg2` are ivregress under community spellings.
//       These normalise options and delegate -- no new mathematics.

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
// E1 — aliases

// areg IS regress with an absorb(), so it routes to the same handler with a
// different stataCmd. Everything that makes areg areg (the e(df_a) absorbed
// degrees of freedom, the "Linear regression, absorbing indicators" header)
// belongs to runRegress's areg arm, not to a second copy of the estimator.
// areg's clustered dof (it charges an FE nested in the cluster, where reghdfe
// and xtreg,fe exempt it) is runRegress's areg arm too (BUG-568); that is what
// retired the BUG-684 stop-gap which used to refuse weighted areg + cluster here.
ojson runAreg(const eval::ParsedCommand& parsed, SessionState& session) {
  // BUG-569: areg's option abbreviations. w2b_estim.log measures `a(id)`,
  // `ab(id)`, `abs(id)` (areg_a(id) .. areg_abs(id)), `cl(id)` (areg_a_cl) and
  // `r` (areg_a_r) all at rc 0, but areg has no row in the generated minimum
  // table the dispatcher resolves abbreviations from, and runRegress only reads
  // the full names -- so each of them was refused "option `a` not allowed".
  // Canonicalized here to the names runRegress reads, at the measured floors
  // (absorb 1, cluster 2, robust 1); anything shorter stays as typed and is
  // refused by runRegress's unknown-option net.
  eval::ParsedCommand canon = parsed;
  auto isAbbrev = [](const std::string& key, const std::string& full, size_t floor) {
    return key.size() >= floor && key.size() <= full.size() &&
           full.compare(0, key.size(), key) == 0;
  };
  bool sawAbsorb = false;
  for (auto& [k, v] : canon.opts) {
    (void)v;
    if (isAbbrev(k, "absorb", 1)) {
      k = "absorb";
      sawAbsorb = true;
    } else if (isAbbrev(k, "cluster", 2)) {
      k = "cluster";
    } else if (isAbbrev(k, "robust", 1)) {
      k = "robust";
    }
  }
  // Stata: areg without absorb() is r(198), not a silent plain regression.
  if (!sawAbsorb) return aerSyntaxError("option absorb() required");

  return runRegress(canon, session, "areg");
}

// ivreg2 / xtivreg2 -> runIvRegress. The spellings differ from ivregress in
// their option vocabulary, not in their estimator, so this normalises and
// delegates.
ojson runIvreg2(const eval::ParsedCommand& parsed, SessionState& session,
                const std::string& stataCmd) {
  eval::ParsedCommand p = parsed;

  bool robust = false, gmm2s = false;
  std::string cluster;
  bool fe = false, fd = false, small = false;
  const bool xtTyped = stataCmd == "xtivreg2" && strutil::lower(parsed.cmd) == "xtivreg2";
  std::vector<std::pair<std::string, eval::OptValue>> kept;
  for (const auto& kv : p.opts) {
    const std::string& k = kv.first;
    // BUG-569 (review round): the value shape, as runRegress checks it. A
    // valued `r(x)` used to count as robust, and a bare `cl` or an empty
    // `cl()` set no cluster, so the fit ran iid at rc 0.
    if (k == "r" || k == "robust") {
      if (!kv.second.isBool) return aerSyntaxError("option `robust` takes no argument");
      robust = true;
    } else if (k == "cl" || k == "cluster") {
      if (kv.second.isBool || strutil::trim(kv.second.strValue).empty()) {
        return aerSyntaxError("option `cluster` requires a variable: cluster(varname)");
      }
      cluster = kv.second.strValue;
    } else if (k == "gmm2s") {
      gmm2s = true;
    } else if (k == "fe") {
      fe = true;
    } else if (k == "fd" && xtTyped) {
      fd = true;
    } else if (k == "small") {
      small = true;
    } else {
      kept.push_back(kv);
    }
  }

  // `gmm2s` alone is not GMM in the sense ivregress means: without a robust or
  // clustered weight matrix the two-step estimator collapses to 2SLS, so the
  // honest translation is 2sls plus a note rather than a silent method switch.
  const bool weighted = robust || !cluster.empty();
  const std::string method = (gmm2s && weighted) ? "gmm" : "2sls";
  // xtivreg2: exactly one of fe and fd, else
  // "error - must specify either fe or fd option" and error 198
  // (w2d_uncovered_xtivreg2.log:2866 xtivreg2_nofe_ctl).
  if (xtTyped && fe == fd) {
    return ojson(eval::errorBlock("syntax_error", "error - must specify either fe or fd option",
                                  std::nullopt, std::nullopt, std::nullopt, "r(198)"));
  }
  if (xtTyped && fd) {
    return aerError(
        "Option `fd` is a valid xtivreg2 option that this version of PIET does not implement "
        "(the first-differences estimator); `fe` is implemented.",
        "unsupported_option");
  }

  // B4 P0-C: `eval::OptValue{}` here was a WRONG NUMBER, not a style nit. A
  // default-constructed OptValue has isBool == false (tokenizer.h:33), and the
  // only reader of this key — vcovFromOpts (estimation_support.cpp) — matches
  // `k == "robust" && v.isBool && v.boolValue`. So the flag this loop had just
  // parsed was re-emitted in a shape its consumer could not see, `--vcov hc1`
  // was never appended, and `ivreg2 ..., robust` returned the IID standard
  // error while the census certified the option as supported. Measured on
  // auto.csv: se[mpg] 65.3314331 for bare AND robust AND small; StataBE gives
  // 65.33143 / 70.08472 / 66.23260 (oracle_p.log:250, :297, :341).
  if (robust) kept.emplace_back("robust", eval::OptValue::ofBool(true));
  if (!cluster.empty()) {
    eval::OptValue cv;
    cv.strValue = cluster;
    kept.emplace_back("cluster", cv);
  }
  if (fe) {
    if (!session.panelvar.has_value()) {
      return aerError(stataCmd + ": option fe requires a panel setting -- run xtset first",
                      "syntax_error");
    }
    eval::OptValue av;
    av.strValue = *session.panelvar;
    kept.emplace_back("absorb", av);
  }
  p.opts = kept;

  // ivreg2's default inference is large-sample (z); `small` asks for the
  // t/F small-sample correction. ivregress's absorb path forces --small
  // unconditionally, which is right for `ivreghdfe` and wrong here, so the
  // flag is threaded (see the forceSmall argument below) rather than inherited.
  // B4 P0-C, the OTHER half — and it is not the same defect as `robust` above.
  // This line used to read `(void)small;` at the bottom of the function: the
  // flag was parsed, kept out of `kept` so it could not be rejected, and then
  // explicitly DISCARDED. The comment above it claimed "the flag is threaded
  // rather than inherited", which described a wiring that did not exist —
  // `--small` reached cobra only through `allowAbsorb`, i.e. only for
  // `ivreg2 ..., fe`. So `ivreg2 ..., small` certified an option it dropped.
  // Threading it is what the comment already said should happen.
  // `smallDefault=fe` reproduces exactly what `allowAbsorb` used to imply here: ivreg2 reports
  // the small-sample correction when it is absorbing, and not otherwise. It is spelled out now
  // because the two meanings were one bit until 2026-09-09 (BUG-474's split).
  // xtivreg2 calls ivreg2 on the within-transformed data with dofminus(N_g) and no
  // `small` of its own (xtivreg2): large-sample z statistics unless
  // `small` is typed (w2d_uncovered_xtivreg2.log:2647 `z`), so its default is not
  // the absorbing ivreg2 spelling's.
  ojson out = runIvRegress(method, p, /*allowAbsorb=*/fe, stataCmd, session,
                           /*smallDefault=*/fe && !xtTyped, /*forceSmall=*/small);
  if (gmm2s && !weighted && out.is_object() && out.value("type", std::string()) != "error") {
    out["notes"].push_back(
        "gmm2s without robust or cluster() is 2SLS: the two-step weight matrix "
        "equals the 2SLS one under homoskedasticity.");
  }
  // e(model) (ivreg2): gmm2s under gmm2s, with or without robust/cluster()
  // (StataBE 19.5, BUG-1223: `ivreg2 y (x = z z2), gmm2s` posts gmm2s, with _b equal to
  // 2SLS's up to rounding, and boottest then prints "GMM no longer supported"); ols when
  // there is no endogenous regressor (BUG-438); iv otherwise.
  const bool ivreg2Typed = stataCmd == "ivreg2" && strutil::lower(parsed.cmd) == "ivreg2";
  if (ivreg2Typed && out.is_object() && out.value("type", std::string()) != "error" &&
      session.lastModel().has_value()) {
    const std::string model =
        gmm2s ? "gmm2s" : (session.lastModel()->ivEndog.empty() ? "ols" : "iv");
    session.eval.setEString("model", model);
    session.noteEOnFit("model");
  }
  return out;
}

}  // namespace piet::handlers
