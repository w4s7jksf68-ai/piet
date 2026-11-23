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
#include "postestimation/rtable_internal.h"  // stataColumns: a factor fit's e(b) columns
#include "estimators/posthest.h"  // cecon::margins_parse_at (header-inline)

#include "extended_estimation_internal.h"
#include "estimation_options.h"
#include "estimation_families_internal.h"  // inRangeConditionM4

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
  // Neither ll() nor ul(): Stata fits the uncensored normal model by maximum
  // likelihood (gsem's gaussian regress; vs-stata bank fa235e5d `tobit mpg weight`:
  // Limits -inf / +inf, var(e.mpg) 11.50634 = rss / N).

  // ll(#) above ul(#): Stata's r(198) "observations with ll() > ul() not allowed"
  // (vs-stata bank 3218474c `tobit mpg weight, ll(30) ul(17)`); the engine's MLE
  // failed on it at rc 1.
  if (llVal.has_value() && ulVal.has_value()) {
    char* e1 = nullptr;
    char* e2 = nullptr;
    const std::string a = trimM4(*llVal), b = trimM4(*ulVal);
    const double lo = std::strtod(a.c_str(), &e1), hi = std::strtod(b.c_str(), &e2);
    if (!a.empty() && !b.empty() && e1 != nullptr && *e1 == '\0' && e2 != nullptr && *e2 == '\0' &&
        lo > hi)
      return stataRefusalM4b("observations with ll() > ul() not allowed", 198);
  }

  // ll(varname) / ul(varname): Stata's per-observation limits (vs-stata bank 631a18de
  // `tobit mpg weight, ll(lo)`). The engine takes one number per limit, and the
  // value reached it as text it could not read (rc 1 "stod: no conversion"):
  // refused by name.
  for (const auto* lim : {&llVal, &ulVal}) {
    if (!lim->has_value()) continue;
    const std::string t = trimM4(**lim);
    char* e = nullptr;
    (void)std::strtod(t.c_str(), &e);
    if (!t.empty() && (e == nullptr || *e != '\0'))
      return engineErrorBlockM4("Option `" + std::string(lim == &llVal ? "ll" : "ul") + "(" + t +
                                    ")` with a variable or expression is valid Stata that this "
                                    "version of PIET does not implement for `tobit`: only a number "
                                    "is supported.",
                                "unsupported_option");
  }

  std::vector<std::string> argv = {"tobit", "--y", y, "--x", joinCommaM4(xs)};
  // `in a/b`: folded into the fit's `if` (bank ee3b4109 `tobit mpg weight if price
  // > 4000 in 1/60, ll(17)`), which the bare ll/ul sample reads too.
  std::optional<std::string> fitIf;
  if (auto err = inRangeConditionM4(session, parsed, fitIf, "tobit")) return *err;

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
      if (!computeYMinMaxM4(session, y, fitIf, mn, mx, errBlock)) return errBlock;
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
      if (!computeYMinMaxM4(session, y, fitIf, mn, mx, errBlock)) return errBlock;
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.17g", mx);
      numStr = buf;
    }
    argv.push_back("--ul");
    argv.push_back(numStr);
  }
  if (fitIf.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*fitIf);
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
    // vce(cluster V): the engine's --cluster (scores summed within V, G/(G-1))
    {
      std::istringstream is(v.isBool ? std::string() : v.strValue);
      std::string w0, cv, extra;
      is >> w0 >> cv >> extra;
      const std::string w0l = lowerM4(w0);
      static const std::regex reVar(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
      if (k == "vce" && w0l.size() >= 2 && std::string("cluster").compare(0, w0l.size(), w0l) == 0 &&
          !cv.empty() && extra.empty() && std::regex_match(cv, reVar)) {
        argv.push_back("--cluster");
        argv.push_back(cv);
        continue;
      }
    }
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

  // iterate(#): ml's iteration cap (vs-stata bank 04017236 `tobit y mpg weight,
  // ll(0) iterate(2)`: e(converged) 0 at the second iterate).
  for (const auto& [k, v] : parsed.opts) {
    if (k != "iterate") continue;
    const std::string val = trimM4(v.isBool ? std::string() : v.strValue);
    char* end = nullptr;
    const double d = std::strtod(val.c_str(), &end);
    if (val.empty() || end == nullptr || *end != '\0' || !(d >= 0 && d <= 16000) ||
        d != std::floor(d))
      return engineErrorBlockM4("option iterate() must be an integer from 0 to 16000 in this build "
                                "(got `" + (v.isBool ? std::string() : v.strValue) + "`)",
                                "unsupported_option");
    argv.push_back("--iterate");
    argv.push_back(std::to_string(static_cast<int>(d)));
    break;
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabTobit.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kVocabTobit.unimplemented)) {
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
    // The engine's "not implemented" forms (vce(cluster) under an fweight or
    // aweight, or with fewer clusters than parameters; Stata fits both, vs-stata
    // bank 8dcb229c) are this build's named r(198) refusal, not an engine error.
    if (ce.code == "unsupported") {
      std::string what = cobraErrorStrM4(ce);
      static const std::regex reTag(R"(^\s*\[[a-z_]+\]\s*(tobit:\s*)?)");
      what = std::regex_replace(what, reTag, "");
      return engineErrorBlockM4("`tobit`: " + what + " in this version of PIET (valid Stata).",
                                "unsupported_option");
    }
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
    bool ok = depvarEqColsM4(y, raw["coefs"], "b", cols);
    // A factor-variable fit: Stata's columns are those r(table) lays out, a base
    // level a structural 0 (vs-stata bank dda72913 `tobit mpg weight i.rep78,
    // ll(17)`: `mpg:weight mpg:1b.rep78 .. mpg:5.rep78 mpg:_cons /var(e.mpg)`). An
    // engine-omitted column is not named here: nothing is posted then.
    if (!ok && block.contains("coeftable") && block["coeftable"].is_array()) {
      std::vector<rtable_detail::RtCol> rc;
      bool placed = false;
      try {
        placed = rtable_detail::stataColumns(session, *session.lastModel(), block["coeftable"], rc);
      } catch (const std::exception&) {
        placed = false;
      }
      if (placed) {
        ok = true;
        cols.clear();
        for (const auto& c : rc) {
          if (c.row == nullptr) {
            cols.push_back({y + ":" + c.name, std::string(), 0.0});
            continue;
          }
          const std::string en = c.row->value("name", std::string());
          const ojson& bv = c.row->contains("b") ? (*c.row)["b"] : ojson();
          const ojson& sv = c.row->contains("se") ? (*c.row)["se"] : ojson();
          if (en.empty() || !bv.is_number() ||
              (bv.get<double>() == 0.0 && sv.is_number() && sv.get<double>() == 0.0)) {
            ok = false;
            break;
          }
          const bool cons = en == "(intercept)" || en == "_cons";
          cols.push_back({y + ":" + (cons ? std::string("_cons") : c.name), en, bv.get<double>()});
        }
      }
    }
    if (ok) {
      const std::string var = "/var(e." + y + ")";
      cols.push_back({var, var, raw["variance"].get<double>()});
      (void)postAncillaryEbM4(session, raw, cols);
    }
  }
  // The residual variance's row, as Stata's table prints it after the
  // coefficients (`var(e.mpg)`: no test, the interval on the log scale) and as
  // r(table) carries it (`/var(e.mpg)`, t and p `.b`).
  if (raw.contains("variance") && raw["variance"].is_number() && raw.contains("se_variance") &&
      raw["se_variance"].is_number() && raw.contains("variance_ci") &&
      raw["variance_ci"].is_array() && raw["variance_ci"].size() == 2 &&
      session.lastEstBlock.has_value() && (*session.lastEstBlock).contains("coeftable") &&
      (*session.lastEstBlock)["coeftable"].is_array()) {
    ojson row;
    row["name"] = "/var(e." + y + ")";
    row["b"] = raw["variance"];
    row["se"] = raw["se_variance"];
    row["t"] = nullptr;
    row["p"] = nullptr;
    row["ci_lo"] = raw["variance_ci"][0];
    row["ci_hi"] = raw["variance_ci"][1];
    row["ci_log"] = true;
    (*session.lastEstBlock)["coeftable"].push_back(row);
    block = *session.lastEstBlock;
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
