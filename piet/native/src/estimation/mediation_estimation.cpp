// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Mediation estimation.
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

#include "estimator_command_support.h"
#include "extended_estimation_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace extended_fit_detail {}
using namespace extended_fit_detail;
using namespace extended_estimation_vocab;

namespace extended_fit_detail {

// Depth-aware top-level paren-group splitter — unlike parseIvParenM4/
// reg3's own naive find(')')-based splitter above (safe there because
// neither ivregress's (endog=instruments) block nor a reg3 equation ever
// nests a SECOND paren inside), mediate's `(treat, continuous(L0 L1))`
// block genuinely nests one paren inside another; a naive first-')' scan
// would truncate at continuous('s OWN close paren instead of the outer
// block's.
struct ParenGroupsResultM4 {
  std::vector<std::string> groups;
  bool errored = false;
  std::string errorMsg;
};

ParenGroupsResultM4 splitParenGroupsM4(const std::string& joined) {
  ParenGroupsResultM4 r;
  size_t i = 0, n = joined.size();
  while (i < n) {
    while (i < n && (joined[i] == ' ' || joined[i] == '\t')) ++i;
    if (i >= n) break;
    if (joined[i] != '(') {
      r.errored = true;
      r.errorMsg = "expected a parenthesised block";
      return r;
    }
    size_t start = i + 1;
    int depth = 1;
    ++i;
    while (i < n && depth > 0) {
      if (joined[i] == '(') ++depth;
      else if (joined[i] == ')') --depth;
      ++i;
    }
    if (depth != 0) {
      r.errored = true;
      r.errorMsg = "unbalanced parentheses";
      return r;
    }
    r.groups.push_back(joined.substr(start, (i - 1) - start));
  }
  return r;
}

}  // namespace

ojson runMediate(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  std::string joined = joinSpaceM4(parsed.varlist);
  ParenGroupsResultM4 pg = splitParenGroupsM4(joined);
  if (pg.errored || pg.groups.size() != 3) {
    return engineErrorBlockM4(
        "syntax_error: mediate requires exactly three parenthesised blocks: "
        "(outcome [controls]) (mediator [controls]) (treatment, continuous(#level0 #level1))",
        "syntax_error");
  }

  // The outcome and mediator blocks are `(depvar [indepvars] [, model noconstant])`
  // where model is one of linear (default), probit, logit, expmean, poisson.
  // Split each at its first comma; the option words are read below in Stata's
  // order (varlist first, then the options, outcome before mediator).
  auto splitModelBlock = [](const std::string& g, std::string* vars, std::string* opts) {
    const size_t c = g.find(',');
    *vars = c == std::string::npos ? g : g.substr(0, c);
    *opts = c == std::string::npos ? std::string() : g.substr(c + 1);
  };
  std::string yVarPart, yOptPart, mVarPart, mOptPart;
  splitModelBlock(pg.groups[0], &yVarPart, &yOptPart);
  splitModelBlock(pg.groups[1], &mVarPart, &mOptPart);
  std::vector<std::string> yToks = splitTokensM4(yVarPart);
  std::vector<std::string> mToks = splitTokensM4(mVarPart);
  if (yToks.empty() || mToks.empty()) {
    return engineErrorBlockM4(
        "syntax_error: mediate's outcome/mediator blocks each require a variable name",
        "syntax_error");
  }
  std::string yVar = yToks[0];
  std::string mVar = mToks[0];
  std::vector<std::string> controls;
  for (size_t k = 1; k < yToks.size(); ++k) controls.push_back(yToks[k]);
  for (size_t k = 1; k < mToks.size(); ++k) {
    bool already = false;
    for (const auto& c : controls) {
      if (c == mToks[k]) already = true;
    }
    if (!already) controls.push_back(mToks[k]);
  }

  // Split the treatment block on its FIRST top-level comma into `tvar` and
  // its own option tail (the same comma-then-options grammar parseOpts
  // applies to a whole command line, applied here to this ONE
  // already-isolated group).
  const std::string& tGroup = pg.groups[2];
  size_t tComma = std::string::npos;
  {
    int depth = 0;
    for (size_t k = 0; k < tGroup.size(); ++k) {
      if (tGroup[k] == '(') {
        ++depth;
      } else if (tGroup[k] == ')') {
        --depth;
      } else if (tGroup[k] == ',' && depth == 0) {
        tComma = k;
        break;
      }
    }
  }
  std::string tVarPart = tComma == std::string::npos ? tGroup : tGroup.substr(0, tComma);
  std::string tOptPart = tComma == std::string::npos ? std::string() : tGroup.substr(tComma + 1);
  std::vector<std::string> tToks = splitTokensM4(tVarPart);
  if (tToks.size() != 1) {
    return engineErrorBlockM4(
        "syntax_error: mediate's treatment block must name exactly one variable", "syntax_error");
  }
  std::string tVar = tToks[0];

  // mediate parses each block's varlist first: a name that is no variable (nor
  // the abbreviation of one) is r(111) "variable zzz not found" ahead of any
  // model-form check (vs-stata bank d0314497 `mediate (zzz age) (hours age)
  // (union)`). Operator / factor terms are left to the fit.
  {
    Datastore* ds = session.datastore();
    std::vector<std::string> names = {yVar, mVar, tVar};
    names.insert(names.end(), controls.begin(), controls.end());
    for (std::string n : names) {
      const size_t cut = n.find(',');
      if (cut != std::string::npos) n = n.substr(0, cut);
      n = trimM4(n);
      if (n.empty() || n.find_first_of(".#*?~-") != std::string::npos || ds->has(n)) continue;
      bool prefix = false;
      for (const auto& have : ds->names())
        if (have.compare(0, n.size(), n) == 0) prefix = true;
      if (!prefix) return stataRefusalM4b("variable " + n + " not found", 111);
    }
  }

  // Each model block's options: exactly the five model words and noCONStant;
  // anything else, or two model words, is r(198) "invalid <which> model
  // specification".
  struct ModelSpecM4 {
    std::string model = "linear";
    bool noconstant = false;
  };
  auto parseModelOpts = [](const std::string& opts, const char* which, ModelSpecM4* out,
                           ojson* err) -> bool {
    std::vector<std::string> models;
    for (const std::string& raw : splitTokensM4(opts)) {
      std::string w = raw;
      std::transform(w.begin(), w.end(), w.begin(), [](unsigned char ch) { return std::tolower(ch); });
      if (w == "linear" || w == "probit" || w == "logit" || w == "poisson") {
        models.push_back(w);
      } else if (w.size() >= 3 && std::string("expmean").compare(0, w.size(), w) == 0) {
        models.push_back("expmean");
      } else if (w.size() >= 6 && std::string("noconstant").compare(0, w.size(), w) == 0) {
        out->noconstant = true;
      } else {
        *err = stataRefusalM4b(std::string("invalid ") + which +
                                   " model specification; option " + raw + " not allowed",
                               198);
        return false;
      }
    }
    if (models.size() > 1) {
      *err = stataRefusalM4b(std::string("invalid ") + which +
                                 " model specification; the " + which +
                                 " model must be one of linear, probit, logit, expmean, or poisson",
                             198);
      return false;
    }
    if (!models.empty()) out->model = models.front();
    return true;
  };
  ModelSpecM4 ySpec, mSpec;
  {
    ojson err;
    if (!parseModelOpts(yOptPart, "outcome", &ySpec, &err)) return err;
    if (!parseModelOpts(mOptPart, "mediator", &mSpec, &err)) return err;
  }
  if (ySpec.model == "logit" && (mSpec.model == "linear" || mSpec.model == "expmean")) {
    return stataRefusalM4b("logit outcome model may not be combined with " + mSpec.model +
                               " mediator model; consider using probit instead of logit",
                           197);
  }
  if (yVar == mVar && mVar == tVar)
    return stataRefusalM4b("outcome, mediator, and treatment variables must be different variables", 198);
  if (yVar == mVar)
    return stataRefusalM4b("outcome and mediator variables must be different variables", 198);
  if (yVar == tVar)
    return stataRefusalM4b("outcome and treatment variables must be different variables", 198);
  if (mVar == tVar)
    return stataRefusalM4b("mediator and treatment variables must be different variables", 198);
  if (ySpec.model != "linear" || mSpec.model != "linear" || ySpec.noconstant || mSpec.noconstant) {
    return engineErrorBlockM4(
        "mediate: only linear outcome and mediator models with a constant are implemented in this "
        "version of PIET (" + ySpec.model + " outcome, " + mSpec.model + " mediator requested).",
        "unsupported_option");
  }

  std::vector<std::pair<std::string, eval::OptValue>> tOpts = eval::parseOpts(tOptPart);
  // BUG-263: this is a second-source opts vector (same class as BUG-262's
  // runLog re-parse) — the dispatcher-level canonicalization point never sees
  // it, and until this net existed a typo'd or unknown sub-block key was
  // silently dropped and the fall-through reject below misdiagnosed the
  // user's SPECIFICATION ("only a continuous treatment ... is supported")
  // when the actual cause was an unread option key. Canonicalize FIRST so the
  // `k == "continuous"` reads below see canonical spellings (the BUG-262
  // one-vector law), then reject anything the sub-block grammar doesn't have.
  eval::canonicalizeOptionKeys(tOpts, kVocabMediateTreat.known);
  if (auto err = eval::rejectUnknownOptions(tOpts, kVocabMediateTreat.known,
                                            kVocabMediateTreat.unimplemented)) {
    return *err;
  }
  std::optional<std::string> continuousVal;
  bool sawContinuousFlag = false;
  for (const auto& [k, v] : tOpts) {
    if (k == "continuous") {
      if (v.isBool) sawContinuousFlag = true;
      else continuousVal = v.strValue;
    }
  }
  // Only a continuous treatment WITH two explicit levels is in beta scope
  // (see the function-header comment) — binary/default treatment (no
  // continuous() at all) and a bare `continuous` flag (no levels) both
  // honest-reject naming continuous(#level0 #level1) as the only supported
  // form, rather than silently answering with a different treatment model.
  if (!sawContinuousFlag && !continuousVal.has_value()) {
    // A binary or multivalued treatment is not fitted here, but Stata's own
    // refusals of the treatment variable come first and are reproduced: over
    // the if/in rows it must hold nonnegative integers (r(459)); over the
    // estimation sample (if/in and every model variable nonmissing) no rows is
    // r(2000), one level is r(459) "does not vary", and more than 10 levels
    // without `force` is r(459). Checked only when every model term is a plain
    // variable name (an operator or factor term goes straight to the refusal).
    std::vector<std::string> modelVars = {yVar, mVar, tVar};
    modelVars.insert(modelVars.end(), controls.begin(), controls.end());
    Datastore* ds = session.datastore();
    bool plain = !parsed.inRange.has_value();
    for (const auto& v : modelVars)
      if (!ds->has(v) || ds->dtype(v) != "f64") plain = false;
    bool force = false;
    for (const auto& [k, v] : parsed.opts)
      if (k == "force") force = true;
    if (plain) {
      const size_t N = static_cast<size_t>(ds->nRows());
      std::vector<char> inIf(N, 1);
      if (parsed.ifExpr.has_value()) {
        // The if is evaluated into a 0/1 column on a scratch copy of the data
        // (a missing condition is true, Stata's rule), like a marked-sample
        // temporary: the data in memory is not touched, so c(changed) and the
        // sort order stay as they were when mediate then refuses.
        SessionState::ResidentSuspend statelessChain(session);
        session.ensureMaterializedHead();
        RowIndexCache maskFrame;
        maskFrame.path = *session.currentHbin();
        maskFrame.nRows = static_cast<int64_t>(N);
        if (auto refusal = addIfMaskColumn(session, maskFrame, *parsed.ifExpr, {}))
          return *refusal;
        {
          Datastore maskData(maskFrame.path);
          const double* truth = maskData.col(maskFrame.maskVar).f64();
          for (size_t i = 0; i < N; ++i) inIf[i] = truth[i] != 0.0;
        }
        cecon::native_unlink(maskFrame.path);
        ds = session.datastore();
      }
      auto column = [&ds, N](const std::string& name) {
        const double* p = ds->col(name).f64();
        return std::vector<double>(p, p + N);
      };
      const std::vector<double> tv = column(tVar);
      for (size_t i = 0; i < N; ++i) {
        if (!inIf[i] || std::isnan(tv[i])) continue;
        if (tv[i] < 0 || tv[i] != std::floor(tv[i])) {
          return stataRefusalM4b("treatment variable " + tVar +
                                     " may only contain nonnegative integer values; use option "
                                     "continuous() if you want " + tVar +
                                     " to be treated as a continuous variable",
                                 459);
        }
      }
      std::vector<char> touse = inIf;
      for (const auto& v : modelVars) {
        const std::vector<double> c = column(v);
        for (size_t i = 0; i < N; ++i)
          if (std::isnan(c[i])) touse[i] = 0;
      }
      std::set<double> levels;
      for (size_t i = 0; i < N; ++i)
        if (touse[i]) levels.insert(tv[i]);
      if (levels.empty()) return stataRefusalM4b("no observations", 2000);
      if (levels.size() == 1)
        return stataRefusalM4b("treatment variable " + tVar + " does not vary", 459);
      if (levels.size() > 10 && !force) {
        return stataRefusalM4b(
            "multivalued treatment " + tVar +
                " has more than 10 levels; use option continuous() if you wish to specify " +
                tVar +
                " as a continuous variable, or option force to proceed with it as a discrete "
                "variable",
            459);
      }
    }
    return engineErrorBlockM4(
        "mediate: only a continuous treatment with two explicit levels is supported in this "
        "version of PIET — use `mediate (...) (...) (treatvar, continuous(#level0 #level1))`.",
        "unsupported_option");
  }
  if (!continuousVal.has_value() || trimM4(*continuousVal).empty()) {
    return engineErrorBlockM4(
        "mediate: continuous() requires exactly two numeric levels, e.g. continuous(0.5 1) — a "
        "bare `continuous` flag with no levels is not supported.",
        "unsupported_option");
  }
  std::vector<std::string> levelToks = splitTokensM4(*continuousVal);
  if (levelToks.size() != 2) {
    return engineErrorBlockM4(
        "mediate: only a two-level continuous() contrast is supported in this version of PIET "
        "(got " +
            std::to_string(levelToks.size()) + " level(s)) — use continuous(#level0 #level1).",
        "unsupported_option");
  }
  double l0 = 0, l1 = 0;
  try {
    l0 = std::stod(levelToks[0]);
    l1 = std::stod(levelToks[1]);
  } catch (...) {
    return engineErrorBlockM4("syntax_error: continuous() levels must be numeric", "syntax_error");
  }
  if (l0 == l1) {
    return engineErrorBlockM4("syntax_error: continuous() levels must differ", "syntax_error");
  }

  std::vector<std::string> argv = {"mediate", "--y", yVar, "--mediator", mVar, "--treat", tVar};
  if (!controls.empty()) {
    argv.push_back("--controls");
    argv.push_back(joinCommaM4(controls));
  }
  for (const auto& [k, v] : parsed.opts) {
    if (k == "reps" && !v.isBool) {
      argv.push_back("--reps");
      argv.push_back(v.strValue);
    } else if (k == "seed" && !v.isBool) {
      argv.push_back("--seed");
      argv.push_back(v.strValue);
    } else if (k == "level" && !v.isBool) {
      argv.push_back("--level");
      argv.push_back(v.strValue);
    }
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabMediate.known;
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

  ojson block;
  block["type"] = "estimation";
  block["estimator"] = "mediate";
  block["depvar"] = yVar;
  block["n_obs"] = raw.value("n_obs", 0);
  block["treatment_levels"] = ojson::array({l0, l1});
  // ENGINE-PARITY DISCLOSURE, NOT STATA-PARITY (unlike every estimator
  // above, where point estimates match the oracle): cobra's `mediate`
  // computes a plain linear (Baron-Kenny, NO treatment-mediator
  // interaction), PER-UNIT causal-mediation decomposition — verified by
  // hand against this exact oracle by refitting the a-path (mediator~treat)
  // and b-path (outcome~treat+mediator) via `cobra feols` directly: cobra
  // mediate's own a/b/direct/indirect fields are EXACTLY those no-
  // interaction OLS coefficients (a=-0.1279895, b=1384.099235, both match
  // feols byte-for-byte). Stata's `continuous(#L0 #L1)` mediate instead
  // fits a RICHER model with an explicit treatment#mediator interaction
  // term (the oracle's own footer: "Outcome equation includes
  // treatment–mediator interaction") and reports the VanderWeele-formula
  // NDE/NIE AT THE SPECIFIC (L0,L1) contrast. Manually reconstructing that
  // formula from two `feols` calls (the a-path plus an interaction-
  // augmented b-path) DOES reproduce the oracle's NDE=-855.2799/
  // NIE=-107.2333 exactly — confirming the missing interaction term is the
  // entire source of the gap — but cobra's `mediate` subcommand itself has
  // NO --level0/--level1/interaction surface at all (checked its usage
  // banner AND a `strings` scan of the binary for hidden flags: none).
  // Hand-rolling the VanderWeele formula natively (plus its own bootstrap
  // inference, since cobra's own SEs/CIs would no longer apply once the
  // model changes) is out of this batch's realistic scope and arguably
  // cobra's job, not PIET's wiring layer. So: dispatch IS wired (the
  // command is recognized, a real argv is built, the engine is actually
  // invoked, real numbers come back) but the point estimates below are the
  // ENGINE's own per-unit, no-interaction decomposition, NOT a
  // Stata-matching (L0 vs L1)-contrast result — `direct`/`indirect`/
  // `total` are NOT literal NDE/NIE/TE. Flagged to the campaign as a
  // possible engine-side feature request (interaction term + explicit
  // contrast levels), the same disclosure shape as D1's ivregress
  // SE-convention gap, just a materially larger (whole-model, not
  // SE-formula) gap here.
  static const char* kMediateFields[] = {
      "a",           "b",           "direct",       "indirect",  "total",
      "se_a",        "se_b",        "se_aroian",    "se_goodman", "se_sobel",
      "z",           "p",           "ci_lo",        "ci_hi",     "proportion_mediated",
      "bootstrap",
  };
  for (const char* k : kMediateFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }

  LastModel lm;
  lm.cobraCmd = "mediate";  // added to runMargins's kLinearCmds below — margins after mediate has
                            // no coherent meaning; dydx/AME should reject, not silently compute.
  lm.y = yVar;
  lm.x = {mVar, tVar};
  lm.depvar = yVar;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  return block;
}

// ===========================================================================
// poisson / ppmlhdfe / fepois / nbreg / xtpoisson / xtlogit / drdid (BETA D3
// batch, panel/count wirings: BS-073/074/075/076). Oracle (poisson-family,
// nbreg, xtpoisson, xtlogit): internal capture
// d3-oracle-stata.log (StataBE 19.5 on piet/packaging/sample/
// fatalities.csv, `gen cnt=round(spirits*3)` + `encode state, gen(snum)` +
// `xtset snum year`; sections D3a-D3g). Oracle (drdid): tests/validate/
// drdid2x2.{do,dta,log}.


}  // namespace piet::handlers
