// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Censored estimation.
#include "piet/handlers/estimation.h"
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

ojson runTobit(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: tobit requires a dependent variable and at least one regressor",
        "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::optional<std::string> llVal, ulVal;
  bool sawLl = false, sawUl = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "ll") {
      sawLl = true;
      if (!v.isBool) llVal = v.strValue;
    }
    if (k == "ul") {
      sawUl = true;
      if (!v.isBool) ulVal = v.strValue;
    }
  }
  if (!sawLl && !sawUl) {
    return engineErrorBlockM4("syntax_error: tobit requires at least one of ll()/ul()",
                              "syntax_error");
  }

  std::vector<std::string> argv = {"tobit", "--y", y, "--x", joinCommaM4(xs)};

  // Stata's BARE `ll`/`ul` (no numeric value) means "censor at the depvar's
  // own observed min/max" — cobra's tobit has no bare-flag equivalent
  // (live-verified: `--ll` with no value fails inside cobra's own arg
  // parser, "stod: no conversion", exit 1), so PIET computes the value
  // itself (computeYMinMaxM4 above) and always forwards an explicit number.
  if (sawLl) {
    std::string numStr;
    if (llVal.has_value() && !trimM4(*llVal).empty()) {
      numStr = trimM4(*llVal);
    } else {
      double mn = 0, mx = 0;
      ojson errBlock;
      if (!computeYMinMaxM4(session, y, parsed.ifExpr, mn, mx, errBlock)) return errBlock;
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.17g", mn);
      numStr = buf;
    }
    argv.push_back("--ll");
    argv.push_back(numStr);
  }
  if (sawUl) {
    std::string numStr;
    if (ulVal.has_value() && !trimM4(*ulVal).empty()) {
      numStr = trimM4(*ulVal);
    } else {
      double mn = 0, mx = 0;
      ojson errBlock;
      if (!computeYMinMaxM4(session, y, parsed.ifExpr, mn, mx, errBlock)) return errBlock;
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.17g", mx);
      numStr = buf;
    }
    argv.push_back("--ul");
    argv.push_back(numStr);
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // level() — NEVER forwarded. cobra tobit's own usage banner does NOT list
  // --level at all (unlike qreg below), and live-verified: `cobra tobit
  // HISTORICAL, kept because the reasoning still teaches: `cobra tobit --level 90` used to
  // hard-error "unrecognized option '--level' for 'tobit'" (bad_input, exit 3) with a CLEAN
  // empty stdout — not the BUG-039 masking shape — which is why PIET refused honestly here
  // rather than forwarding into a silent drop. BUG-202 gave the engine the surface, so the
  // refusal's premise expired and the arm below forwards instead (BUG-274).
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      // BUG-274: this used to reject, on the premise that "the engine's tobit estimator
      // has no --level surface at all". That premise was TRUE when written and is FALSE now —
      // BUG-202 gave tobit `--level`, with the multiplier chosen per estimator rather than
      // blanket-swapped (tobit takes t(df_r) to match its own p-value dispatch). A refusal whose
      // stated reason has expired is worse than no refusal: it tells the user the capability does
      // not exist when it does. Forward it, using this file's own established idiom.
      argv.push_back("--level");
      argv.push_back(v.strValue);
      continue;
    }
  }

  // W:P5: `tobit …, vce(robust)`. The engine's tobit now carries a
  // score-based observation-level sandwich (`--vce robust`); cluster/twoway
  // do not exist there, so vcovFromOpts's other shapes stay rejected rather
  // than silently downgraded to iid.
  for (const auto& [k, v] : parsed.opts) {
    if (k != "vce" && k != "robust") continue;
    const std::string want = v.isBool ? std::string("robust") : lowerM4(v.strValue);
    if (want != "robust") {
      return engineErrorBlockM4(
          "vce(" + want + ") is not supported for `tobit` in this engine; vce(robust) is",
          "unsupported_option");
    }
    argv.push_back("--vce");
    argv.push_back("robust");
  }
  // BUG-1544: tobit takes aweights, fweights, iweights and pweights (tobit.ado's
  // gsem; the engine fits them all). A pweight implies vce(robust), the
  // buildEstimationArgvM4 rule (BUG-536; StataBE 19.5 `tobit yc x z [pw=wf],
  // ll(1.2)` e(vce) robust, _se[x] .19377991916672471).
  if (parsed.weightType.has_value() && parsed.weightVar.has_value() &&
      !parsed.weightVar->empty()) {
    const std::string wt = lowerM4(*parsed.weightType);
    const std::string wtype = (wt == "aw" || wt == "aweight")   ? "aw"
                              : (wt == "fw" || wt == "fweight") ? "fw"
                              : (wt == "iw" || wt == "iweight") ? "iw"
                              : (wt == "pw" || wt == "pweight") ? "pw"
                                                                : "";
    if (!wtype.empty()) {
      if (wtype == "pw" && std::find(argv.begin(), argv.end(), "--vce") == argv.end()) {
        argv.push_back("--vce");
        argv.push_back("robust");
      }
      argv.push_back("--weight");
      argv.push_back(*parsed.weightVar);
      argv.push_back("--wtype");
      argv.push_back(wtype);
    }
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabTobit.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
  }

  argv.push_back("--emit-v");  // BUG-1196: the fit's e(V), posted below
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, "tobit", &session);
  block["depvar"] = y;  // cobra tobit's raw JSON carries no "depvar" field at all (live-verified)
  static const char* kTobitFields[] = {
      "n_left_censored", "n_right_censored", "n_uncensored", "ll_limit",
      "ul_limit",        "sigma",            "se_sigma",     "variance",
      "se_variance",
  };
  for (const char* k : kTobitFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }

  LastModel lm;
  lm.cobraCmd = "tobit";  // already anticipated in runMargins's kLinearCmds (below) — dydx after
                          // tobit correctly rejects, predictive margins/(x) still coarse-OLS.
  lm.y = y;
  lm.x = xs;
  lm.depvar = y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  // BUG-1196: Stata's e(b) is the depvar's equation, then the residual variance
  // (StataBE 19.5 `yc:x1 yc:x2 yc:_cons /var(e.yc)`); e(V) is the fit's, its var
  // row/column in Stata's variance metric (cmds_mle.cpp cmd_tobit --emit-v).
  if (raw.contains("e_V") && raw.contains("coefs") && raw.contains("variance") &&
      raw["variance"].is_number()) {
    std::vector<AncillaryEbColM4> cols;
    if (depvarEqColsM4(y, raw["coefs"], "b", cols)) {
      const std::string var = "/var(e." + y + ")";
      cols.push_back({var, var, raw["variance"].get<double>()});
      (void)postAncillaryEbM4(session, raw, cols);
    }
  }
  return block;
}

// ===========================================================================
// heckman (BS-070, BETA D2 batch): `heckman depvar indepvars [if], select
// (selvar = selvars) [twostep]` -> `cobra heckman --select "S=Z1,Z2"`.
// Oracle: d2-oracle-stata.log D2c (twostep) / D2d (Stata's own default MLE,
// for comparison only — NOT wired, see below).
//
// Stata's DEFAULT (no `twostep`) fits full MLE; cobra's heckman usage
// banner says it implements ONLY "two-step Heckit (probit selection +
// inverse-Mills correction)" — live-verified further and more subtly: a
// BARE `cobra heckman` call (no --twostep flag at all) ALREADY computes the
// two-step result (matches oracle D2c exactly: beertax b=-606.9695,
// se=338.0717, lambda=-2436.094/se=12262.59, rho=-0.87975, sigma=2769.076,
// all byte-for-byte), while EXPLICITLY passing the CLI's own documented
// `--twostep` flag is actively HARMFUL: it hard-errors "unrecognized option
// '--twostep' for 'heckman'" on stderr (exit 3) while STILL printing the
// exact same full, valid result JSON to stdout first — the identical
// cobra_runner.cpp stdout/exit-code masking shape the D1 report already
// flagged for ivregress/reg3's --level/--no-intercept. So this NEVER sends
// --twostep: Stata's `twostep` maps to the engine's only-ever-computed
// default, and Stata's bare-MLE form has no engine equivalent — honest
// reject rather than silently answering an MLE request with two-step
// numbers (gate ①).

}  // namespace piet::handlers
