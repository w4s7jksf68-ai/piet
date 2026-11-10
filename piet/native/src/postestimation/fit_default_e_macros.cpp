// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Default e(cmdline), e(title), e(predict), e(estat_cmd), e(marginsok) of a fit.

#include "piet/eval/tokenizer.h"
#include "vendor/nlohmann/json.hpp"

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
#include "estimation/estimation_internal.h"
#include "estimators/postest_names.h"  // factor_vars_of / parse_level_ref (the engine's one resolver)
#include "dispatch/registry.h"  // resolveCanonical: the command word as Stata names it
#include "rtable_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace rtable_detail {

// e(cmdline) of a fit whose handler posted none: every estimator posts the ado's
// `"<cmd> `0'"' -- the command word as Stata names it, then the rest as typed
// (StataBE 19.5, 26 estimators in scratch-laneA/lead/cmdline/cl.do: `reg income
// beertax` posts "regress income beertax", `xtlogit hi beertax, fe` "xtlogit hi
// beertax, fe"). A handler's own posting (regress's ordered e() lists, leebounds,
// bootstrap) is kept.
void postDefaultCmdline(SessionState& session) {
  if (session.eval.eStrings.find("cmdline") != nullptr) return;
  // the leaf's own line after macro expansion (empty: none recorded, post none)
  const std::string& rawCmd = session.leafCmdExpanded;
  const std::string word = commandWord(rawCmd);
  if (word.empty()) return;
  size_t k = rawCmd.find_first_not_of(" \t");
  k = rawCmd.find_first_of(" \t,", k);
  std::string rest;
  if (k != std::string::npos) {
    k = rawCmd.find_first_not_of(" \t", k);
    if (k != std::string::npos) rest = rawCmd.substr(k);  // trailing blanks stay (BUG-1048)
  }
  const std::string canon = registry::resolveCanonical(word);
  session.eval.setEString("cmdline", rest.empty() ? canon : canon + " " + rest);
  session.noteEOnFit("cmdline");  // the next install drops it
}

// e(title), e(predict), e(estat_cmd) and e(marginsok) of a fit whose handler posted
// none. Each ado posts the same words for every variant PIET fits (StataBE 19.5,
// BUG-1492/BUG-1493, scratch-laneA2/agents/epost/s1.do: default, robust and cluster VCEs,
// weights, noconstant, or, baseoutcome(), quantile(), ul()/ll(), families and links); ""
// = Stata posts none (xtreg and qreg have no e(title), areg no e(estat_cmd)). Keyed by
// e(cmd) and, where one word names several of Stata's estimators, by the fit: xtreg's fe
// (the engine's feols) and re ("xtreg_re"), clogit and xtpoisson only as the fe fits
// (`xtlogit, fe` posts e(cmd) clogit), ivregress by method, ivreghdfe as 2SLS. A prefixed
// fit (e(prefix) svy) posts its own and is left alone.
void postDefaultEMacros(SessionState& session) {
  const auto& lmo = session.lastModel();
  if (!lmo.has_value() || session.ePostedLive || !session.eval.eStrings.get("prefix").empty())
    return;
  const std::string cmd = session.eval.eStrings.get("cmd");
  const std::string& engine = lmo->cobraCmd;
  const std::string est = lmo->result.has_value() && lmo->result->is_object()
                              ? lmo->result->value("estimator", std::string())
                              : std::string();
  struct Macros {
    const char *title, *predict, *estatCmd, *marginsok;
  };
  std::optional<Macros> m;
  if (cmd == "regress") {
    m = Macros{"Linear regression", "regres_p", "regress_estat", "XB default"};
  } else if (cmd == "areg") {
    m = Macros{"Linear regression, absorbing indicators", "areg_p", "", ""};
  } else if (cmd == "reghdfe") {
    m = Macros{"HDFE Linear regression", "reghdfe_p", "reghdfe_estat", ""};
  } else if (cmd == "logit") {
    m = Macros{"Logistic regression", "logit_p", "logit_estat", "default Pr"};
  } else if (cmd == "logistic") {
    m = Macros{"Logistic regression", "logistic_p", "logit_estat", "default Pr"};
  } else if (cmd == "probit") {
    m = Macros{"Probit regression", "probit_p", "probit_estat", "default Pr"};
  } else if (cmd == "clogit" && est == "xtlogit_fe") {
    m = Macros{"Conditional fixed-effects logistic regression", "xtlogit_fe_p", "", "PU0 XB"};
  } else if (cmd == "xtpoisson" && est == "xtpoisson_fe") {
    m = Macros{"Conditional fixed-effects Poisson regression", "xtpoisson_fe_p", "", ""};
  } else if (cmd == "xtreg" && engine == "feols") {
    m = Macros{"", "xtrefe_p", "xtreg_estat", ""};
  } else if (cmd == "xtreg" && est == "xtreg_re") {
    m = Macros{"", "xtrere_p", "xtreg_estat", ""};
  } else if (cmd == "poisson") {
    m = Macros{"Poisson regression", "poisso_p", "poisson_estat", ""};
  } else if (cmd == "ppmlhdfe") {
    m = Macros{"HDFE PPML regression", "ppmlhdfe_p", "reghdfe5_estat", "default"};
  } else if (cmd == "ivregress") {
    const char* title = nullptr;
    if (engine == "liml")
      title = "Instrumental-variables LIML regression";
    else if (est == "ivgmm" || est == "ivgmm_multi")
      title = "Instrumental-variables GMM regression";
    else if (est == "iv2sls" || est == "iv2sls_multi")
      title = "Instrumental-variables 2SLS regression";
    if (title != nullptr) m = Macros{title, "ivregress_p", "ivregress_estat", "XB default"};
  } else if (cmd == "ivreghdfe" && engine == "iv" && (est == "iv2sls" || est == "iv2sls_multi")) {
    m = Macros{"IV (2SLS) estimation", "reghdfe_p", "", "XB default"};
  } else if (cmd == "tobit") {
    m = Macros{"Tobit regression", "tobit_p", "",
               "default XB Pr(passthru) E(passthru) YStar(passthru)"};
  } else if (cmd == "nbreg") {
    m = Macros{"Negative binomial regression", "nbreg_p", "", ""};
  } else if (cmd == "qreg") {
    m = Macros{"", "qreg_p", "", ""};
  } else if (cmd == "glm") {
    m = Macros{"Generalized linear models", "glim_p", "", "default"};
  } else if (cmd == "ologit") {
    m = Macros{"Ordered logistic regression", "ologit_p", "ologit_estat", ""};
  } else if (cmd == "mlogit") {
    m = Macros{"Multinomial logistic regression", "mlogit_p", "", ""};
  }
  if (!m.has_value()) return;
  const auto fill = [&session](const char* name, const char* v) {
    if (*v == '\0' || session.eval.eStrings.find(name) != nullptr) return;
    session.eval.setEString(name, v);
    session.noteEOnFit(name);  // the next install drops it
  };
  fill("title", m->title);
  fill("predict", m->predict);
  fill("estat_cmd", m->estatCmd);
  fill("marginsok", m->marginsok);
}

}  // namespace rtable_detail

}  // namespace piet::handlers
