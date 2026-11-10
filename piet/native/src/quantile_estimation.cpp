// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Quantile estimation.
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

ojson runQreg(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: qreg requires a dependent variable and at least one regressor",
        "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::vector<std::string> argv = {"qreg", "--y", y, "--x", joinCommaM4(xs)};

  for (const auto& [k, v] : parsed.opts) {
    if (k == "quantile" && !v.isBool) {
      double q = 0;
      try {
        q = std::stod(trimM4(v.strValue));
      } catch (...) {
        return engineErrorBlockM4("syntax_error: quantile() must be numeric", "syntax_error");
      }
      if (q > 1.0) q /= 100.0;  // Stata allows either a (0,1) fraction or a (1,100) percentage
      if (!(q > 0.0 && q < 1.0)) {
        return engineErrorBlockM4(
            "syntax_error: quantile() must be strictly between 0 and 1 (or 0 and 100 as a "
            "percentage)",
            "syntax_error");
      }
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.17g", q);
      argv.push_back("--tau");
      argv.push_back(buf);
    }
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      argv.push_back("--level");
      argv.push_back(v.strValue);
    }
  }

  // w2b-estim-vce-options: vce(robust) and [pw=] -> `cobra qreg --vce robust
  // [--weight W --wtype pw]` (qreg_fit_robust; w2b_estim.log:818
  // qreg_vce_robust, :791 qreg_pw_vce_robust `robust`). vce(iid) is the
  // default and forwards nothing. Any other vce() -- Stata's own denmethod/
  // bwidth suboptions, `vce(robust, kernel)` etc. -- is refused: only the
  // default fitted/hsheather pair is implemented and oracle-pinned.
  bool qregRobust = false;
  bool qregIid = false;  // vce(iid) spelled out: r(404) beside a pweight, below
  for (const auto& [k, v] : parsed.opts) {
    if (k != "vce") continue;
    const std::string lv = v.isBool ? std::string() : lowerM4(trimM4(v.strValue));
    if (lv == "robust") {
      qregRobust = true;
    } else if (lv == "iid") {
      qregIid = true;
    } else {
      return engineErrorBlockM4(
          "Option `vce(" + (v.isBool ? std::string() : v.strValue) +
              ")` is not implemented for `qreg` in this version of PIET; vce(iid) and "
              "vce(robust) are.",
          "unsupported_option");
    }
  }
  // FU-106 (w2p_estim_ev.log section A): Stata's qreg treats a pweight as
  // implying vce(robust). `qreg y x1 x2 [pw=w], quantile(q)` WITHOUT vce() runs
  // at rc 0 as vce(robust) (A_qreg_pw_q50_novce :299 == A_qreg_pw_q50_robust
  // :364, e(vce) robust, e(vcetype) Robust), so the pweight sets the robust
  // path here, before the --vce argv is built; the forwarded argv is then the
  // explicit vce(robust) one. An explicit vce(iid) is r(404) (:405, :411).
  const bool qregWeighted = parsed.weightType.has_value() && parsed.weightVar.has_value() &&
                            !parsed.weightVar->empty();
  if (qregWeighted) {
    const std::string wt = lowerM4(*parsed.weightType);
    if (wt != "pw" && wt != "pweight") {
      // [fw=]/[iw=] with the default vce(iid) are fitted natively
      // (runQregNative); only their other VCE forms reach this line.
      return engineErrorBlockM4("`qreg` [" + wt + "=] is implemented only with the default "
                                "vce(iid) in this version of PIET.",
                                "unsupported_option");
    }
    if (qregIid) {
      return ojson(eval::errorBlock(
          "engine_error",
          "option vce(iid) is not possible with pweighted data; pweights imply robust",
          std::nullopt, std::nullopt, std::nullopt, "r(404)"));
    }
    qregRobust = true;
  }
  if (qregRobust) {
    argv.push_back("--vce");
    argv.push_back("robust");
  }
  if (qregWeighted) {
    argv.push_back("--weight");
    argv.push_back(*parsed.weightVar);
    argv.push_back("--wtype");
    argv.push_back("pw");
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabQreg.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, "qreg", &session);
  block["depvar"] = y;  // cobra qreg's raw JSON carries no "depvar" field at all (live-verified)
  static const char* kQregFields[] = {"tau", "sparsity", "bwidth", "sum_adev"};
  for (const char* k : kQregFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // FU-184: e(q) is the fitted quantile (w2p_estim_ev.log :104/:140/:170/:203/
  // :238/:267: .25/.75/.5). Both e()-scalar resolvers (eval/macros.cpp
  // resolveEScalar, display_hardening.cpp) read lm.result[key] with key == the
  // Stata name when the name is absent from eScalarKey(), so `q` needs no table
  // edit: publish it in the engine reply that becomes lm.result -- the model
  // result only, NOT the block, whose bytes internal capture qreg.json pins. The `tau` key stays (selftest.cpp:27497-27526 pins it).
  if (raw.contains("tau") && raw["tau"].is_number()) raw["q"] = raw["tau"];

  LastModel lm;
  lm.cobraCmd = "qreg";  // already anticipated in runMargins's kLinearCmds (below).
  lm.y = y;
  lm.x = xs;
  lm.depvar = y;
  // setLastModel publishes lm.vcov "hc1" as e(vce) `robust` / e(vcetype)
  // `Robust` (w2b_estim.log:791); the default stays "iid" (qreg_ctl :844 `iid`).
  if (qregRobust) lm.vcov = "hc1";
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  return block;
}

// ===========================================================================
// mediate (BS-072, BETA D2 batch): Stata 18 syntax `mediate (Y [controls])
// (M [controls]) (T, continuous(#L0 #L1))` -> `cobra mediate --y Y
// --mediator M --treat T [--controls ...] [--reps N] [--seed N] [--level
// N]`. Oracle: probes/d2b-mediate.log (the ONLY working Stata invocation
// against this dataset — a bare continuous treatment var with no
// continuous() at all is Stata's OWN r(459); probes/d2-oracle-stata.log's
// D2g shows a bare `continuous` flag with no levels is Stata's OWN r(198))
// — so only the continuous(#L0 #L1) two-level form is in beta scope; every
// other treatment shape honest-rejects naming this as the supported form.

}  // namespace piet::handlers
