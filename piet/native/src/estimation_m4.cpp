// M4a estimation + diagnostic surface — C++ port of
// piet/backend/piet/router/translate.py's build_estimation_argv/
// build_csdid_argv/build_corr_argv/build_ttest_argv routing plus
// dispatcher.py's _cobra_to_estimation / _csdid_to_estimation / _run_predict
// / the test_cmd/lincom_cmd/nlcom_cmd/margins_cmd post-estimation handlers
// and the hettest/ovtest/vif "estat" diagnostic rewrite. See
// piet/native/include/piet/handlers.h for the per-handler oracle-source
// mapping (ROADMAP M4a item 2).
//
// Scope: xtreg (fe/re), logit, probit, csdid (+ estat simple/event/
// calendar/group/pretrend), tabstat, correlate/pwcorr/ttest, predict,
// test, lincom, nlcom, margins, estat hettest/ovtest/vif, ivregress/
// ivreghdfe/reg3/boottest (BETA D1), tobit/heckman/qreg/mediate (BETA D2,
// BS-069/070/071/072).
#include "piet/handlers.h"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/settings.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"

// ---------------------------------------------------------------------------
// BUG-233 / FIX:OPTNET-ABBREV-SWEEP — this file's option vocabularies.
//
// Every set below used to be a function-local `static const` sitting next to its
// reject net, which meant nothing upstream of the handler could see it — and the
// handler's own `k == "fullname"` comparisons therefore ran against RAW keys
// while eval::rejectUnknownOptions had already ACCEPTED an abbreviation of them.
// That is BUG-231's mechanism, and it was live in nets across six files.
//
// Registering here (static init, before any dispatch) lets dispatcher.cpp's single
// resolution point canonicalize the keys BEFORE routing. The sets did not get
// copied: each net now binds a reference back to its registered entry, so there is
// still exactly one place each spelling is written.
namespace {
const piet::eval::OptVocab& kVocabTtest = piet::eval::registerOptVocab(
    {"ttest"}, {{"level", "by", "unequal", "welch", "paired", "reverse", "unpaired"},
     {}});
const piet::eval::OptVocab& kVocabCsdid = piet::eval::registerOptVocab(
    {"csdid"}, {{"ivar", "time", "gvar", "method", "comparison", "pscoretrim", "window", "cluster", "long2"},
     {}});
const piet::eval::OptVocab& kVocabReg3 = piet::eval::registerOptVocab(
    {"reg3"}, {{"sure", "3sls"},
     {}});
const piet::eval::OptVocab& kVocabBoottest = piet::eval::registerOptVocab(
    {"boottest"}, {{"reps", "seed", "weights", "null",
                                                          "cluster"},
     {}});
const piet::eval::OptVocab& kVocabTobit = piet::eval::registerOptVocab(
    {"tobit"}, {{"ll", "ul", "level", "vce", "robust"},
     {}});
const piet::eval::OptVocab& kVocabHeckman = piet::eval::registerOptVocab(
    {"heckman"}, {{"select", "twostep", "level"},
     {}});
const piet::eval::OptVocab& kVocabQreg = piet::eval::registerOptVocab(
    {"qreg"}, {{"quantile", "level", "vce"},
     {}});
const piet::eval::OptVocab& kVocabMediate = piet::eval::registerOptVocab(
    {"mediate"}, {{"reps", "seed", "level"},
     {}});
// BUG-263: mediate's treatment sub-block `(tvar, ...)` is a SECOND
// eval::parseOpts vector built inside runMediate itself (BUG-262's class), so
// the dispatcher's resolution point can never see its keys — the handler
// canonicalizes and rejects locally against this entry. Registered under its
// own token, NOT under "mediate": registerOptVocab UNIONs per token and
// kVocabMediate above binds the "mediate" slot by reference, so adding
// `continuous` there would make the TOP-LEVEL net accept-and-silently-ignore
// a trailing `, continuous(...)` — BUG-231's exact shape. The sub-block
// grammar is closed at exactly one option ([CAUSAL] mediate, Syntax:
// `(tvar [, continuous(numlist)])`), so the unimplemented set is empty by
// measurement, not omission; the "real Stata shape PIET doesn't implement"
// honesty lives in the handler's own numlist-arity message (a >2-value
// continuous() numlist is real Stata, and gets the two-level-only reject).
const piet::eval::OptVocab& kVocabMediateTreat = piet::eval::registerOptVocab(
    {"mediate:tvar"}, {{"continuous"},
     {}});
const piet::eval::OptVocab& kVocabDrdid = piet::eval::registerOptVocab(
    {"drdid"}, {{"ivar", "time",     "treatment", "dripw",
                                                          "reg",  "ipw",      "std_ipw",   "trad"},
     {}});
// Item 4 (User ruling 2026-09-09): `post` moves to `known` -- see
// estimation_m4c.cpp's copy (the one actually READ by runMargins) for the
// full rationale. Three byte-identical copies must move together.
const piet::eval::OptVocab& kVocabMargins = piet::eval::registerOptVocab(
    {"margins"}, {{"at", "dydx", "post"},
     {}});
const piet::eval::OptVocab& kVocabEstatDurbinalt = piet::eval::registerOptVocab(
    {"estat"}, {{"small", "lags"},
     {}});
const piet::eval::OptVocab& kVocabEstatEvent = piet::eval::registerOptVocab(
    {"estat"}, {{"window", "post"},
     {}});
const piet::eval::OptVocab& kVocabNewey = piet::eval::registerOptVocab(
    {"newey"}, {{"lag"},
     {}});
const piet::eval::OptVocab& kVocabPrais = piet::eval::registerOptVocab(
    {"prais"}, {{"corc", "twostep", "rhotype"},
     {}});
const piet::eval::OptVocab& kVocabWntestq = piet::eval::registerOptVocab(
    {"wntestq"}, {{"lags"},
     {}});
const piet::eval::OptVocab& kVocabSdtest = piet::eval::registerOptVocab(
    {"sdtest"}, {{"by", "level"},
     {}});
const piet::eval::OptVocab& kVocabHausman = piet::eval::registerOptVocab(
    {"hausman"}, {{},
     {"constant", "sigmamore", "sigmaless", "force", "alleqs",
      "skipeqs",  "equations", "df",        "tconsistent", "tefficient"}});
const piet::eval::OptVocab& kVocabChow = piet::eval::registerOptVocab(
    {"chow"}, {{"by"},
     {}});
const piet::eval::OptVocab& kVocabMkspline = piet::eval::registerOptVocab(
    {"mkspline"}, {{"cubic", "nknots", "knots",
                                                          "displayknots"},
     {}});
const piet::eval::OptVocab& kVocabIvRegress = piet::eval::registerOptVocab(
    {"ivregress", "ivreg", "ivreghdfe", "reghdfe"}, {{"vce", "vcov", "robust", "cluster",
      "gmm",
      "absorb", "keepsingletons", "noconstant", "nocons", "level"},
     {}});
const piet::eval::OptVocab& kVocabPoisson = piet::eval::registerOptVocab(
    {"poisson", "ppmlhdfe", "fepois"}, {{"vce", "vcov", "robust", "cluster", "absorb",
      "offset", "exposure", "noconstant", "nocons", "level"},
     {}});
const piet::eval::OptVocab& kVocabNbreg = piet::eval::registerOptVocab(
    {"nbreg"}, {{"vce", "vcov", "robust", "cluster", "noconstant",
      "nocons", "offset", "exposure", "dispersion", "level"},
     {}});
// The two conditionally-built nets: the base literal lives here (one copy), and
// the registered vocabulary is base + the command-conditional key, so the
// dispatcher can canonicalize `offs`/`dri` while the handler's own net keeps
// deciding whether the canonical spelling is allowed for the command at hand.
const std::unordered_set<std::string> kXtPoissonLogitBase = {
    "fe", "re", "pa", "vce", "vcov", "robust", "cluster", "noconstant", "nocons", "level"};
const piet::eval::OptVocab& kVocabXtPoissonLogit = piet::eval::registerOptVocab(
    {"xtpoisson", "xtlogit"},
    {[] { auto s = kXtPoissonLogitBase; s.insert("offset"); return s; }(), {}});
const std::unordered_set<std::string> kDfullerPperronBase = {
    "lags", "noconstant", "nocons", "trend"};
const piet::eval::OptVocab& kVocabDfullerPperron = piet::eval::registerOptVocab(
    {"dfuller", "pperron"},
    {[] { auto s = kDfullerPperronBase; s.insert("drift"); return s; }(), {}});
// w2b-estim-vce-options: probit's `robust` abbreviates to ONE character --
// tests/eval/golden/aer_w2/w2b_estim.log:706 `probit d x1 x2, r nocons` is rc 0
// with the Robust std. err. column. logit gets `r` from the measured-minimums
// table (opt_minimums_bug264.inc `{"robust", 1}`); probit has no robust row
// there, and the 1-char spelling is below canonicalizeForCommand's prefix
// floor, so it is DECLARED here as an alias (BUG-430's mechanism). `known`
// holds `robust` alone: it is the only key this registration speaks for, and
// every longer prefix of it is accepted by the same 1-char minimum. The
// handler's own net (buildEstimationArgvM4 kEstimationKnownOpts) is unchanged.
const piet::eval::OptVocab& kVocabProbitRobust = piet::eval::registerOptVocab(
    {"probit"}, {{"robust"}, {}, {{"r", "robust"}}});
}  // namespace

#include "estimation_m4_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

// R2: the shared helper block that lived here as an anonymous namespace is now
// `namespace est_detail` (definitions HERE, declarations in
// estimation_m4_internal.h) so the two continuation TUs can link against it.
namespace est_detail {

// A5-04: forward to the shared strutil copy (piet/strutil.h) — kept as thin
// wrappers (not removed) so this file's many lowerM4(...)/trimM4(...) call
// sites didn't need touching. Pure dedup, no behavior change.
std::string lowerM4(std::string s) { return strutil::lower(std::move(s)); }

std::string trimM4(const std::string& s) { return strutil::trim(s); }

ojson engineErrorBlockM4(const std::string& message, const std::string& code) {
  return eval::engineError(message, code);
}

// BUG-569 (review round): the VCE options' value SHAPE, twin of handlers.cpp
// vceOptionShapeError (the reasons are written there). Each VCE loop in this
// file reads `cluster` only when valued and `robust` only as a flag, so a bare
// `cluster`, an empty `cluster()` or a valued `robust(x)` was skipped and the
// model ran iid at rc 0. Measured on the branch binary 2026-09-29:
// `xtreg y x1, fe cluster`, `logit yb x1, cluster`, `probit yb x1, robust(id)`
// and `poisson cnt x1, cluster` each returned rc 0 with e(vce) iid.
static std::optional<std::string> vceOptionShapeErrorM4(const eval::ParsedCommand& parsed) {
  for (const auto& [k, v] : parsed.opts) {
    if (k == "cluster" && (v.isBool || trimM4(v.strValue).empty())) {
      return std::string("option `cluster` requires a variable: cluster(varname)");
    }
    if (k == "robust" && !v.isBool) {
      return std::string("option `robust` takes no argument");
    }
  }
  return std::nullopt;
}

// BUG-038(a): forwards to the shared mapping point (piet::cobraErrorMessage,
// cobra_runner.h) — kept as a thin wrapper (not removed) so this file's
// many call sites didn't need touching. Pure dedup, no behavior change here.
std::string cobraErrorStrM4(const CobraError& ce) { return cobraErrorMessage(ce); }

std::string joinCommaM4(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ",";
    s += v[i];
  }
  return s;
}

// kZ975: moved to estimation_m4_internal.h as inline constexpr (R2 — internal-linkage const cannot cross the TU cut)

double normalPvalue(double z) { return std::erfc(std::abs(z) / std::sqrt(2.0)); }

void fillCoeftableStatsM4(ojson& block);

// BUG-102: a factor-variable coefficient row's engine name is `<level>.<var>`
// (BUG-101, model.cpp:526), or `<level>.<var>#<level>.<var>` for interactions.
// Real Stata substitutes the attached value label for the bare level when
// DISPLAYING such a row (verified live against StataBE 19.5: `regress y
// i.state_num` with `label values state_num statelbl` prints "Arizona"/
// "Idaho" rows, not "2.state_num"/"3.state_num") — but that live table also
// relies on a shared "state_num" group header PIET's flat one-row-per-
// coefficient table has no equivalent for (formatOneTable, console.js), so
// the ".varname" suffix is kept here rather than dropped bare: the level
// portion alone is substituted, segment-by-segment across any "#" interaction
// join, matching the scoping note's "rewrite factor-level row names" framing.
std::string substituteFactorLevelLabelM4(const std::string& name, const SessionState& session) {
  return strutil::substituteFactorLevelLabel(name, session.valueLabels);
}

// Applies substituteFactorLevelLabelM4 to every row["name"] in a coeftable
// array in place. A no-op (session.valueLabels empty, or a coeftable that
// isn't an array) costs one map .empty() check — freeze/pangram-safe for the
// overwhelming majority of runs that never attach a value label.
void applyFactorValueLabelsM4(ojson& coeftable, const SessionState& session) {
  // BUG-397 (2026-09-02, the g12 oracle-first leg's FIX:BUG102-COEFTABLE-NAME-PARITY): the row's
  // `name` is Stata's e(b) colname and is INVARIANT under value labels (`1983.yearpl` stays
  // `1983.yearpl`; `_b[1983.yearpl]` must keep resolving). The label is DISPLAY: it rides on two
  // additive fields — `stub` = the BUG-102 display shape with the level segment substituted
  // (`Odd83.yearpl`), which the frontend prints in place of the name; `label` = the bare label
  // text of the (last) substituted level (`Odd83`), Stata's own stub. Rows without a label get
  // neither field, so every unlabeled fixture is byte-identical.
  if (session.valueLabels.empty() || !coeftable.is_array()) return;
  for (auto& row : coeftable) {
    if (!row.is_object() || !row.contains("name") || !row["name"].is_string()) continue;
    const std::string name = row["name"].get<std::string>();
    std::string sub = substituteFactorLevelLabelM4(name, session);
    if (sub == name) continue;
    row["stub"] = sub;
    // bare label = the text before the first '.' of the LAST '#'-segment that changed
    std::string lastSeg = sub.substr(sub.find_last_of('#') == std::string::npos ? 0 : sub.find_last_of('#') + 1);
    row["label"] = lastSeg.substr(0, lastSeg.find('.'));
  }
}

// -- _cobra_to_estimation (dispatcher.py) — full port including the BUG-006
// margins AME b=null remap that handlers.cpp's cobraToEstimation explicitly
// does NOT include (that copy is the M2/regress-only vertical slice). This
// is the M4a-complete version used by every command in this file.
// `session` is optional (default nullptr): BUG-102 value-label substitution
// only runs when a caller has one to offer (regression-family estimators);
// callers with no natural SessionState in scope (corr/tabstat) are unchanged.
ojson cobraToEstimationM4(const ojson& raw, const std::string& cobraCmd,
                           const SessionState* session) {
  ojson block;
  block["type"] = "estimation";
  std::string estimator = raw.value("estimator", cobraCmd);
  block["estimator"] = estimator;
  block["depvar"] = raw.value("depvar", "");
  // BUG-165 (W:P5): a two-sample ttest has no single `n_obs` on the engine's
  // wire — it reports the two group counts — so the plain default published
  // 0 observations for a test that used thousands. Sum the groups when they
  // are what the engine gave us; every other estimator is unchanged.
  if (!raw.contains("n_obs") && raw.contains("n_1") && raw.contains("n_2") &&
      raw["n_1"].is_number() && raw["n_2"].is_number()) {
    block["n_obs"] = raw["n_1"].get<long long>() + raw["n_2"].get<long long>();
  } else {
    block["n_obs"] = raw.value("n_obs", 0);
  }

  // BUG-112(8), consumption half. cobra emits non-fatal model-spec notes as a
  // top-level `warnings` array (contract A; `add_warnings`, absent when empty)
  // — Stata's `note:` line, e.g. "99b.rep78 identifies no observations in the
  // sample" when an `ib#.` base level is absent. Until now NOTHING on this side
  // read the key: the success path discards cobra's stderr and no consumer
  // looked for a new JSON field, so the engine half shipped INERT. Carrying it
  // onto the block is the whole fix — a warning produced and never shown is a
  // warning that does not exist, and doctrine I says PIET mirrors what cobra
  // says, nothing less. Optional and absent-when-empty on this side too, so a
  // clean estimation stays byte-identical (freeze: 0 fixtures carry the key).
  if (raw.contains("warnings") && raw["warnings"].is_array() && !raw["warnings"].empty()) {
    block["warnings"] = raw["warnings"];
  }

  ojson coeftable;
  bool hasCoeftable = raw.contains("coeftable");
  if (hasCoeftable) coeftable = raw["coeftable"];

  // BUG-006: margins AME — coeftable has b=null; remap from coefs[].dydx.
  bool allBNull = hasCoeftable && coeftable.is_array() && !coeftable.empty();
  if (allBNull) {
    for (const auto& row : coeftable) {
      if (!row.contains("b") || !row["b"].is_null()) {
        allBNull = false;
        break;
      }
    }
  } else {
    allBNull = false;
  }
  if (hasCoeftable && estimator == "margins" && allBNull) {
    ojson coefsRaw = raw.value("coefs", ojson::array());
    std::map<std::string, ojson> coefsByName;
    for (const auto& c : coefsRaw) {
      if (c.contains("name")) coefsByName[c["name"].get<std::string>()] = c;
    }
    ojson remapped = ojson::array();
    for (const auto& row : coeftable) {
      std::string name = row.value("name", std::string());
      auto it = coefsByName.find(name);
      ojson c = (it != coefsByName.end()) ? it->second : ojson::object();
      ojson dydx = c.contains("dydx") ? c["dydx"] : ojson(nullptr);
      ojson se = (it != coefsByName.end() && c.contains("se")) ? c["se"] : row.value("se", ojson(nullptr));
      ojson z = (it != coefsByName.end() && c.contains("t")) ? c["t"] : row.value("t", ojson(nullptr));
      if (dydx.is_null()) dydx = row.value("b", ojson(nullptr));

      ojson r;
      r["name"] = name;
      std::optional<double> bF, seF, zF, pF, ciLo, ciHi;
      try {
        if (!dydx.is_null()) bF = dydx.get<double>();
        if (!se.is_null()) seF = se.get<double>();
        if (!z.is_null()) zF = z.get<double>();
        if (bF.has_value() && seF.has_value()) {
          if (!zF.has_value() && *seF != 0.0) zF = *bF / *seF;
          if (zF.has_value()) pF = normalPvalue(*zF);
          ciLo = *bF - kZ975 * *seF;
          ciHi = *bF + kZ975 * *seF;
        }
      } catch (...) {
        bF.reset();
      }
      r["b"] = bF.has_value() ? ojson(*bF) : dydx;
      r["se"] = seF.has_value() ? ojson(*seF) : se;
      r["t"] = zF.has_value() ? ojson(*zF) : z;
      r["p"] = pF.has_value() ? ojson(*pF) : ojson(nullptr);
      r["ci_lo"] = ciLo.has_value() ? ojson(*ciLo) : ojson(nullptr);
      r["ci_hi"] = ciHi.has_value() ? ojson(*ciHi) : ojson(nullptr);
      remapped.push_back(r);
    }
    coeftable = remapped;
  }

  if (hasCoeftable) block["coeftable"] = coeftable;
  if (hasCoeftable && session) applyFactorValueLabelsM4(block["coeftable"], *session);
  // BS-065: cobra's `reg3 --tidy` payload carries its per-equation results
  // under "equations" (an array of {depvar, coefs, coeftable} — NOT the
  // {..., r2_within, dof_resid} shape --y-batch commands like feols/iv use
  // for "outcomes") rather than under the key "outcomes" itself. Rather than
  // inventing a second, parallel multi-model block shape for the frontend
  // to learn, this reuses the EXACT SAME block["outcomes"] output shape
  // outcomes already produces (falling back to null for the two per-eq
  // stats reg3 doesn't emit, r2_within/dof_resid — an honest engine-data
  // gap, not a frontend bug: verified live, cobra's reg3 --tidy simply
  // never computes them per-equation) so any existing "iterate
  // block.outcomes[]" frontend renderer picks up reg3 for free.
  if (raw.contains("outcomes") || raw.contains("equations")) {
    const ojson& srcArr = raw.contains("outcomes") ? raw["outcomes"] : raw["equations"];
    ojson outcomes = ojson::array();
    for (const auto& o : srcArr) {
      ojson oo;
      // P7: mlogit's equations are keyed by OUTCOME, not by a dependent variable
      // (there is only one depvar, split across K−1 equations). Falling through
      // to the empty default rendered three unlabeled blocks with no way to tell
      // which outcome each belonged to — the numbers were right and the table was
      // unreadable. The base outcome carries no coefficients by construction, so
      // it is labelled as such rather than shown as an empty equation.
      if (o.contains("outcome") && !o.contains("depvar")) {
        oo["depvar"] = o["outcome"].is_string() ? o["outcome"].get<std::string>()
                                                : o["outcome"].dump();
        oo["outcome"] = o["outcome"];
        if (o.value("base", false)) oo["base"] = true;
      } else {
        oo["depvar"] = o.value("depvar", "");
      }
      oo["coeftable"] = o.value("coeftable", ojson::array());
      if (session) applyFactorValueLabelsM4(oo["coeftable"], *session);
      oo["r2"] = o.contains("r2_within") ? o["r2_within"] : o.value("r2", ojson(nullptr));
      oo["r2_within"] = o.contains("r2_within") ? o["r2_within"] : ojson(nullptr);
      oo["dof_resid"] = o.contains("dof_resid") ? o["dof_resid"] : ojson(nullptr);
      outcomes.push_back(oo);
    }
    block["outcomes"] = outcomes;
  }

  static const char* kPassthrough[] = {
      "r2", "r2_within", "sigma2", "deviance", "converged", "iters", "n_clusters",
      "cluster_dof_k", "dof_resid", "first_stage_F", "kp_wald_F", "hansen_j", "hansen_j_p",
      "F", "J", "chi2", "df1", "df2", "p_F", "p_chi2", "restrictions",
      "margin", "at", "margins", "df", "mean_1", "mean_2", "by", "coefs",
      // BUG-165 (W:P5): ttest's per-group counts and dispersions. The engine
      // has always emitted n_1/n_2/sd_1/sd_2 — Stata prints all four in the
      // group rows (`Obs`, `Std. dev.`) — but they were never forwarded, so
      // the block reached the frontend with only the pooled diff. Worse, the
      // engine emits no `n_obs` key for ttest at all, so cobraToEstimationM4's
      // `raw.value("n_obs", 0)` default published **n_obs: 0** on the wire: a
      // test over 3,822 observations reporting zero. Carrying these lets the
      // n_obs fallback below compute the real total.
      "n_1", "n_2",
      "r", "variables", "p", "nobs", "method", "pairwise", "sig",
      // BS-026 (BETA Phase C batch C1): cobra's `tabstat --tidy` payload
      // carries the actual per-var per-stat table in "stats" (the
      // requested stat-name list, e.g. ["mean","sd","n"]) and "total" (a
      // {varname: {statname: value}} object) plus "vars" (the ordered var
      // list) — none of the three were forwarded, so every tabstat block
      // reached the frontend as an empty shell ({estimator:"tabstat",
      // n_obs, by:null}, "by" already being the only tabstat-relevant key
      // in the original list above). Verified against a live cobra
      // `tabstat --tidy` run: these are cobra's OWN field names (not a
      // frontend field-name mismatch to translate) — the frontend has no
      // tabstat table renderer at all yet regardless (out of scope,
      // piet/frontend/** forbidden this batch), so this fixes the DATA
      // layer; the block now genuinely carries every stat that was asked
      // for.
      "stats", "total", "vars",
  };
  for (const char* k : kPassthrough) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  static const char* kOls[] = {"adj_r2", "rmse", "mss", "rss", "df_m", "df_r"};
  for (const char* k : kOls) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // BUG-156 (wiring half, P2-PREP): poisson/fepois fit statistics. `chi2` is
  // already carried by kPassthrough above; `chi2_df` and `r2_p` (Stata's
  // e(r2_p), the pseudo-R2 spelling poisson uses — distinct from the
  // `r2_pseudo` key logit/probit emit) were dropped on the floor, so even
  // once the engine emits them they could not reach a consumer.
  //
  // The engine does NOT emit these keys yet — that is the parallel half of
  // BUG-156 and is engine COMPUTE work. Until it does, `display e(r2_p)`
  // still answers missing; this is the wiring so it stops being TWO fixes.
  static const char* kGlm[] = {"ll", "ll_0", "r2_pseudo", "df_chi2", "chi2_df", "r2_p"};
  for (const char* k : kGlm) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // BUG-156 (wiring half, P2-PREP): feols single-FE variance decomposition —
  // Stata's e(sigma_u)/e(sigma_e)/e(rho)/e(corr). Same story as kGlm above:
  // allowlisted now, emitted by the engine in the parallel workstream.
  static const char* kXtreg[] = {"r2_between", "r2_overall", "sigma_u", "sigma_e", "rho",
                                 "corr_u_xb"};
  for (const char* k : kXtreg) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // BS-066: cobra's `wildboot --tidy` payload has NO "coeftable" at all (a
  // single wild-cluster-bootstrap test result, not a coefficient table) —
  // its own field names (verified live) carry the actual answer.
  static const char* kWildboot[] = {
      "beta", "p_equaltail", "p_symmetric", "null_beta", "seed", "reps", "weights", "t0",
      "webb_fallback",
  };
  for (const char* k : kWildboot) {
    if (raw.contains(k)) block[k] = raw[k];
  }

  if (raw.contains("absorbed") && !raw["absorbed"].empty()) block["absorbed"] = raw["absorbed"];
  if (raw.contains("vcov_type") && raw["vcov_type"].is_string() &&
      !raw["vcov_type"].get<std::string>().empty()) {
    block["vcov_type"] = raw["vcov_type"];
  }
  return block;
}

// -- shared vcov-option parsing (translate.py's _vcov_from_opts) -----------
// Returns the --vcov[/--cluster] argv tail; throws a std::string error
// message (caller wraps into unsupported_option) on a malformed vce().
// struct VcovResult: defined in estimation_m4_internal.h (R2 split)

// BS-067 (BETA D1): `vce(cluster A B)` / legacy `cluster(A B)` naming TWO
// space-separated variables -> `--vcov twoway --cluster A,B` (cobra's
// feols/iv/fepois --vcov surface accepts "twoway" as a 4th mode alongside
// iid/hc1/cluster — live-verified against the D1i reghdfe 2-way oracle:
// `reghdfe income beertax, absorb(state) vce(cluster state year)` ->
// beertax se=1867.524 matches byte-for-byte through this exact
// --vcov twoway --cluster state,year shape). One var -> the pre-existing
// single-cluster path (unchanged). Three+ vars -> named refusal: multiway
// clustering with three or more variables is implemented for reghdfe only
// (handlers.cpp splitClusterVars), so every M4 command refuses it.
struct ClusterSplitM4 {
  std::vector<std::string> argv;  // {"--vcov", TYPE, "--cluster", VARS}
  bool errored = false;
  std::string errorMsg;
};

ClusterSplitM4 splitClusterVarsM4(const std::string& raw) {
  ClusterSplitM4 r;
  std::vector<std::string> toks;
  std::istringstream iss(raw);
  std::string t;
  while (iss >> t) toks.push_back(t);
  if (toks.empty()) {
    r.errored = true;
    r.errorMsg = "cluster variable required";
    return r;
  }
  if (toks.size() == 1) {
    r.argv = {"--vcov", "cluster", "--cluster", toks[0]};
  } else if (toks.size() == 2) {
    r.argv = {"--vcov", "twoway", "--cluster", toks[0] + "," + toks[1]};
  } else {
    r.errored = true;
    r.errorMsg =
        "three or more cluster variables are not supported for this command in this "
        "version of PIET; multiway clustering with three or more cluster variables is "
        "implemented for reghdfe only";
  }
  return r;
}

VcovResult vcovFromOpts(const eval::ParsedCommand& parsed, bool* sawRobustFlag) {
  VcovResult out;
  if (auto shapeErr = vceOptionShapeErrorM4(parsed)) {
    out.errored = true;
    out.errorMsg = *shapeErr;
    return out;
  }
  std::optional<std::string> vceVal;
  std::optional<std::string> clusterOpt;
  bool robustFlag = false;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !vceVal.has_value()) {
      vceVal = v.isBool ? std::string() : v.strValue;
    } else if (k == "robust" && v.isBool && v.boolValue) {
      robustFlag = true;
    } else if (k == "cluster" && !v.isBool) {
      clusterOpt = v.strValue;
    }
  }
  if (sawRobustFlag) *sawRobustFlag = robustFlag;
  if (vceVal.has_value() && !vceVal->empty()) {
    // W1b: `vce()`'s argument abbreviates -- ONE canonicaliser for all three
    // sites (piet/handlers.h `canonicalVceType`), measured floors.
    std::string vceRest;
    const std::string lv = canonicalVceType(*vceVal, &vceRest);
    if (lv == "robust" || lv == "hc1") {
      out.argv = {"--vcov", "hc1"};
    } else if (lv == "cluster") {
      std::string cvar = clusterOpt.value_or("");
      if (!vceRest.empty()) cvar = vceRest;
      if (cvar.empty()) {
        out.errored = true;
        out.errorMsg = "Option `vce(cluster)` is not supported in this version of PIET. "
                        "Cluster variable required: vce(cluster varname)";
        return out;
      }
      ClusterSplitM4 cs = splitClusterVarsM4(cvar);
      if (cs.errored) {
        out.errored = true;
        out.errorMsg = cs.errorMsg;
        return out;
      }
      out.argv = cs.argv;
    } else if (lv == "iid") {
      out.argv = {"--vcov", "iid"};
    } else if (!unimplementedVceType(*vceVal).empty()) {
      out.errored = true;
      out.errorMsg = "Option `vce(" + unimplementedVceType(*vceVal) +
                     ")` is a valid Stata option that this version of PIET does not implement";
      return out;
    } else {
      out.errored = true;
      out.errorMsg = "Unknown vce type: " + *vceVal;
      return out;
    }
  } else if (clusterOpt.has_value() && !clusterOpt->empty()) {
    // BUG-055 / P2: Stata's legacy `cluster(varname)` — with neither vce()
    // nor robust given — is a synonym for `vce(cluster varname)`: it
    // implies robust-cluster VCE, not iid. Before this fix, `clusterOpt`
    // was parsed (above) but only ever consulted from inside the
    // `vce(cluster ...)` branch, so a bare `cluster(v)` fell through both
    // `if`s and silently ran iid (v wholly ignored) for every caller of
    // this shared helper (xtreg fe/re, logit, probit). Oracle
    // P2_legacycluster (mirrors handlers.cpp::runRegress's identical fix).
    // BUG-537 (W2 estimopts): tested BEFORE the bare-`robust` branch, which
    // used to win and drop the cluster (HC1, N_clust 0) whenever both were
    // typed -- `xtreg ..., robust fe cluster(firmID)` x12 in one corpus
    // file. Stata's cluster wins: w2_pweight.log R5_pw_robust_AND_cluster_
    // equals_R4 is byte-identical to R4. Twin ordering in runRegress.
    ClusterSplitM4 cs = splitClusterVarsM4(*clusterOpt);
    if (cs.errored) {
      out.errored = true;
      out.errorMsg = cs.errorMsg;
      return out;
    }
    out.argv = cs.argv;
  } else if (robustFlag) {
    out.argv = {"--vcov", "hc1"};
  }
  return out;
}

// build_estimation_argv (translate.py) — shared skeleton for xtreg/logit/
// probit's argv construction (fe/absorb/vcov/noconstant/level/TS-injection).
// `extraFe` mirrors the xtreg,fe router's `extra={"fe": panelvar}` override.
// struct EstimationArgvResult: defined in estimation_m4_internal.h (R2 split)

// W:P5 fit-sample fidelity (BUG-163). The fit argv is ground truth for what
// restriction/weights the engine actually estimated under — scan it once at
// LastModel-caching time so every post-estimation refit can replay the SAME
// sample instead of silently answering from the full dataset. Called at every
// `lm.dataPath = ...` site in this file (handlers.cpp's runRegress sets
// lm.fitDataPath directly instead — it materializes its if/in filter into a
// scratch file rather than passing --keepif).
void captureFitFilterM4(LastModel& lm, const std::vector<std::string>& argv) {
  for (size_t i = 0; i + 1 < argv.size(); ++i) {
    if (argv[i] == "--keepif") lm.fitKeepif = argv[i + 1];
    if (argv[i] == "--weight") lm.fitWeight = argv[i + 1];
    if (argv[i] == "--wtype") lm.fitWtype = argv[i + 1];
  }
}

EstimationArgvResult buildEstimationArgvM4(const std::string& cobraCmd,
                                            const eval::ParsedCommand& parsed, SessionState& session,
                                            const std::optional<std::string>& extraFe) {
  EstimationArgvResult res;
  if (parsed.varlist.empty()) {
    res.errored = true;
    res.errorBlock = engineErrorBlockM4("syntax_error: no variables specified", "syntax_error");
    return res;
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::vector<std::string> argv = {cobraCmd, "--y", y};
  if (!xs.empty()) {
    argv.push_back("--x");
    argv.push_back(joinCommaM4(xs));
  } else {
    argv.push_back("--x");
    argv.push_back("(intercept)");
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // --fe: extraFe (xtreg,fe router) OR absorb() option.
  std::string fe = extraFe.value_or("");
  if (fe.empty()) {
    for (const auto& [k, v] : parsed.opts) {
      if (k == "absorb" && !v.isBool) fe = v.strValue;
    }
  }
  if (!fe.empty()) {
    std::string joined;
    std::string cur;
    for (char c : fe) {
      if (c == ' ' || c == ',') {
        if (!cur.empty()) {
          if (!joined.empty()) joined += ",";
          joined += cur;
          cur.clear();
        }
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) {
      if (!joined.empty()) joined += ",";
      joined += cur;
    }
    if (!joined.empty()) {
      argv.push_back("--fe");
      argv.push_back(joined);
    }
  }

  VcovResult vr = vcovFromOpts(parsed);
  if (vr.errored) {
    res.errored = true;
    res.errorBlock = engineErrorBlockM4(vr.errorMsg, "unsupported_option");
    return res;
  }
  // P3 / BUG-056: cobra's logit/probit estimator does not implement
  // cluster-robust VCE. Reject HONESTLY here — whether the user asked via
  // `vce(cluster ...)` or the legacy `cluster()` synonym (BUG-055, above,
  // now maps to the exact same "--vcov cluster" shape in vr.argv for every
  // caller of this shared builder) — instead of forwarding a flag cobra
  // would otherwise have to silently drop or bounce as an unknown-flag
  // error (engine E6 net). vr.argv is always exactly {"--vcov", TYPE, ...}
  // (see every branch of vcovFromOpts above), so this check is precise.
  // BS-067: also catches "twoway" (a 2-var cluster now that
  // splitClusterVarsM4 exists) — logit/probit never gained cluster-robust
  // VCE of any shape, so a `vce(cluster A B)` must reject the SAME way a
  // single-var `vce(cluster A)` always has, not slip past this guard just
  // because vr.argv[1] reads "twoway" instead of "cluster".
  // W:P5: single-variable vce(cluster) is now IMPLEMENTED for logit/probit
  // (weighted/unweighted score sandwich in the engine, src/estimators/logit),
  // so the blanket reject above narrows to the shape that is still missing:
  // two-way clustering, which no MLE path in this engine computes. Keeping
  // the reject for "twoway" is the honest half — a silent fallback to
  // one-way would answer a different question.
  if ((cobraCmd == "logit" || cobraCmd == "probit") && vr.argv.size() >= 2 &&
      vr.argv[0] == "--vcov" && vr.argv[1] == "twoway") {
    res.errored = true;
    res.errorBlock = engineErrorBlockM4(
        "vce(cluster A B) — two-way clustering is not supported for logit/probit in this engine; "
        "one-way vce(cluster A) is",
        "unsupported_option");
    return res;
  }
  // BUG-536 (W2 estimopts): a probability weight IMPLIES a robust VCE in
  // Stata -- w2_pweight.log R2_pw is byte-identical to R3 (`vce(robust)`),
  // and X4_pw_fe to X5 (the xtreg,fe rewrite below turns this hc1 into the
  // panel cluster). Twin of runRegress's rule: only when no vce()/robust/
  // cluster() chose one. logit's engine refused a bare pweight outright
  // ("requires vce(robust)", rc 1) where Stata runs it robust at rc 0.
  if (vr.argv.empty() && parsed.weightType.has_value() && parsed.weightVar.has_value() &&
      !parsed.weightVar->empty()) {
    const std::string wt = lowerM4(*parsed.weightType);
    if (wt == "pw" || wt == "pweight") vr.argv = {"--vcov", "hc1"};
  }
  for (const auto& a : vr.argv) argv.push_back(a);

  // W:P5: forward the weight, normalized to cobra's 2-letter --wtype
  // spelling — the same shape handlers.cpp::runRegress has used since P3.
  // The tokenizer's registry gate (registry.cpp WeightPolicy) already
  // decided WHICH types are legal for this command, so any weight reaching
  // here is one the command declares support for; an engine that cannot
  // honour it surfaces its own error through the ordinary CobraError catch.
  if (parsed.weightType.has_value() && parsed.weightVar.has_value() &&
      !parsed.weightVar->empty()) {
    const std::string wt = lowerM4(*parsed.weightType);
    std::string wtype;
    if (wt == "aw" || wt == "aweight") wtype = "aw";
    else if (wt == "fw" || wt == "fweight") wtype = "fw";
    else if (wt == "iw" || wt == "iweight") wtype = "iw";
    else if (wt == "pw" || wt == "pweight") wtype = "pw";
    if (!wtype.empty()) {
      argv.push_back("--weight");
      argv.push_back(*parsed.weightVar);
      argv.push_back("--wtype");
      argv.push_back(wtype);
    }
  }

  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      argv.push_back("--no-intercept");
    }
  }
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      try {
        int lv = std::stoi(v.strValue);
        if (lv < 1 || lv > 99) {
          res.errored = true;
          res.errorBlock = engineErrorBlockM4("level(#) must be an integer between 1 and 99.",
                                              "unsupported_option");
          return res;
        }
        argv.push_back("--level");
        argv.push_back(std::to_string(lv));
      } catch (...) {
        res.errored = true;
        res.errorBlock = engineErrorBlockM4("level(#) requires an integer argument, e.g. level(90).",
                                            "unsupported_option");
        return res;
      }
    }
  }
  // BS-048 (BETA A4) — NOT extended here: verified that cobra's own
  // xtreg/logit/probit CLIs never read --level at all (grepped every
  // apps/cmds/*.cpp `opt(a, "--level", ...)` site — only qreg/sdtest/ttest/
  // mediate do, and only ttest is wired into PIET's dispatch surface today;
  // runTtest carries the session.level fallback instead). Sending --level
  // to a command that never consumes it trips cobra's E6 unconsumed-flag
  // net (apps/cmds/common.cpp::check_unconsumed_flags) and turns an
  // otherwise-successful estimation into a hard error — so a session.level
  // fallback HERE would make an ordinary `set level 90` + subsequent
  // xtreg/logit/probit call newly error where it worked before (a
  // regression this fix must not introduce). The pre-existing explicit
  // `level(#)` handling above has the SAME latent gap (untested by any
  // freeze fixture — no `level(...)` usage anywhere in debug_all.do) but is
  // OUT of this phase's 4-mechanism scope to fix; flagged for the operator.

  // P4 / BUG-060: unknown-option rejection net for xtreg(fe/re)/logit/
  // probit. Known-option set is the UNION of what THIS shared builder
  // consumes (absorb/vce/vcov/robust/cluster/noconstant/nocons/level,
  // above) and what runXtreg's caller-side checks already consume BEFORE
  // invoking this builder (fe/re model-select; be/gls/mle/ec1/pa reject) —
  // one shared, deliberately-permissive net rather than a per-caller list,
  // so an option one caller legitimately handles is never misflagged as
  // leftover for another caller of this same builder (conservative by
  // design: false-accept is safe here, false-reject is not — see the
  // BUG-060 report for the full over-rejection sweep list). No curated
  // "valid-Stata-but-gap" bucket here (unlike regress's `beta`): no
  // evidenced case yet, so every leftover reads as the r(198)-parity
  // "not allowed" message.
  static const std::set<std::string> kEstimationKnownOpts = {
      "absorb", "vce", "vcov", "robust", "cluster", "noconstant", "nocons",
      "level",  "fe",  "re",   "be",     "gls",     "mle",        "ec1",  "pa",
  };
  // BUG-333: `or` (odds-ratio display) and `iterate(#)` are logit-ONLY. They are
  // gated on cobraCmd rather than added to the shared set above, because this
  // builder also serves probit/xtreg/etc., where Stata offers neither — widening
  // the shared set would manufacture an over-acceptance on four other commands.
  // `or` is consumed in the dispatcher (display metric, never reaches the engine);
  // `iterate` is forwarded below.
  const bool logitOnlyOk = (cobraCmd == "logit");
  // BUG-331: `offset()` is a logit/probit option in Stata (StataBE 19.5 oracle,
  // 2026-08-17) and the engine implements it as of cobra 0.42.0. Gated on the two
  // commands that have it rather than widened into the shared set, for the same
  // reason `or`/`iterate` are: this builder also serves xtreg and friends.
  const bool offsetOk = (cobraCmd == "logit" || cobraCmd == "probit");
  for (const auto& [k, v] : parsed.opts) {
    (void)v;
    if (kEstimationKnownOpts.count(k)) continue;
    if (logitOnlyOk && (k == "or" || k == "iterate")) continue;
    if (offsetOk && k == "offset") continue;
    res.errored = true;
    res.errorBlock = engineErrorBlockM4("option `" + k + "` not allowed", "unsupported_option");
    return res;
  }

  // GLM-style: cobra's logit/probit CLI spells its VCE selector `--vce`, not
  // the `--vcov` every linear estimator uses, and names the robust shape
  // "robust" rather than "hc1". W:P5 widens this remap from robust-only to
  // cluster as well — the engine now implements clustered MLE VCE, and
  // leaving `--vcov cluster` unmapped made a supported combination fail as an
  // unrecognized flag. `--cluster VAR` rides along unchanged (same spelling
  // on both CLIs).
  if (cobraCmd == "logit" || cobraCmd == "probit") {
    for (size_t i = 0; i < argv.size(); ++i) {
      if (argv[i] != "--vcov" || i + 1 >= argv.size()) continue;
      if (argv[i + 1] == "hc1") {
        argv[i] = "--vce";
        argv[i + 1] = "robust";
      } else if (argv[i + 1] == "cluster") {
        argv[i] = "--vce";
      }
    }
  }

  // TS-context injection (MI-083 §16) — same helper regress uses.
  std::vector<std::string> tsTokens = {y};
  for (const auto& x : xs) tsTokens.push_back(x);
  if (translate::hasTsToken(tsTokens)) {
    argv = translate::injectTsContext(argv, session);
  }

  // BUG-333: forward logit's --iterate. This MUST push onto the local `argv`,
  // not onto res.argv — the assignment on the next line overwrites res.argv
  // wholesale, which silently swallowed the first version of this forward
  // (measured: `logit ..., iterate(2)` returned the un-truncated default fit).
  if (logitOnlyOk) {
    for (const auto& [k, v] : parsed.opts)
      if (k == "iterate" && !v.isBool) {
        argv.push_back("--iterate");
        argv.push_back(v.strValue);
        break;
      }
  }

  // BUG-331: forward `offset(VAR)`. Same local-`argv` rule as --iterate above —
  // res.argv is overwritten wholesale below, so pushing onto res.argv here would
  // silently swallow the flag (the defect BUG-333's comment records).
  if (offsetOk) {
    for (const auto& [k, v] : parsed.opts)
      if (k == "offset" && !v.isBool) {
        argv.push_back("--offset");
        argv.push_back(v.strValue);
        break;
      }
  }

  res.argv = argv;
  return res;
}

// P6: the ONE choke point every cobra invocation built in this file passes
// through, so the persisted `provenance_hash` setting (settings.h/.cpp)
// applies uniformly across every M4a estimator and post-estimation command
// without a per-call-site decision. Interactive default is OFF
// (Settings::provenanceHash = false) -> append `--no-provenance` (VERIFIED
// E6-net-safe: cobra consumes it, never rejects it, and simply omits its
// `provenance{}` block from the result JSON). The user opting IN
// (provenanceHash = true) appends nothing, so cobra's own CLI default
// (provenance ON) is unchanged — this is a PIET-native default flip only,
// never the engine's.
nlohmann::json runCobraJsonM4(std::vector<std::string> args) {
  if (!piet::settings::load().provenanceHash) {
    args.push_back("--no-provenance");
  }
  return runCobraJson(args);
}

std::string nextPathM4(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

// BUG-042: shared if-prefilter for tabstat/correlate/ttest — none of the
// three has a --keepif of its own on cobra's CLI (checked against each
// subcommand's own usage banner), so this mirrors handlers.cpp's
// runSummarize if-prefilter (the SAME row-filter infra: `cobra eval
// --keepif COND --out tmp.hbin`, then point the real call at the filtered
// copy instead of the live hbin). No ifExpr/inRange -> returns hbinPath
// unchanged, zero behavior change on the unqualified path (freeze-compatible).
// On a cobra error, returns nullopt and leaves the CommandErrorResult block
// in `errBlockOut` for the caller to return directly.
// `inRange` (BS-090, BETA Phase G, default nullopt): an optional `in a/b`
// row-range, composed with `ifExpr` via AND through
// piet::handlers::applyIfAndInFilter — only ttest/correlate/pwcorr pass a
// real value here (registry.cpp's allowInRange); tabstat/tobit's existing
// call sites are untouched (still pass no 5th argument), so their behavior
// is byte-identical to before this phase.
std::optional<std::string> applyIfFilterM4(SessionState& session, const std::string& hbinPath,
                                           const std::optional<std::string>& ifExpr,
                                           ojson& errBlockOut,
                                           const std::optional<std::string>& inRange) {
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveIn = inRange.has_value() && !inRange->empty();
  if (!haveIf && !haveIn) return hbinPath;
  InRangeFilterResult fr;
  try {
    fr = applyIfAndInFilter(session, hbinPath, ifExpr, inRange);
  } catch (const CobraError& ce) {
    errBlockOut = engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    return std::nullopt;
  }
  if (!fr.ok) {
    errBlockOut = inRangeRejectError(fr, *inRange, "this command");
    return std::nullopt;
  }
  return fr.dataPath;
}

// BS-069 (BETA D2): Stata's tobit accepts BARE `ll`/`ul` (no numeric value)
// meaning "censor at the depvar's own observed min/max" — cobra's tobit
// CLI has no equivalent (live-verified: `--ll` with no following value fails
// inside cobra's own arg parser with "stod: no conversion", exit 1). This
// computes the min/max of `yName` over the SAME sample cobra will fit on
// (the `if`-filtered copy, reusing applyIfFilterM4 above) so PIET can pass
// an explicit numeric value either way. Returns false + fills errBlockOut on
// any failure (missing data, non-numeric column, filter error, no rows).
bool computeYMinMaxM4(SessionState& session, const std::string& yName,
                       const std::optional<std::string>& ifExpr, double& outMin, double& outMax,
                       ojson& errBlockOut) {
  std::string basePath = *session.currentHbin();
  ojson filterErr;
  auto filteredPath = applyIfFilterM4(session, basePath, ifExpr, filterErr);
  if (!filteredPath.has_value()) {
    errBlockOut = filterErr;
    return false;
  }
  bool madeTemp = (*filteredPath != basePath);
  std::optional<Datastore> tempDs;
  Datastore* ds = nullptr;
  if (madeTemp) {
    tempDs.emplace(*filteredPath);
    ds = &*tempDs;
  } else {
    ds = session.datastore();
  }
  bool ok = false;
  if (ds != nullptr && ds->has(yName) && ds->dtype(yName) == "f64") {
    const cecon::Column& c = ds->col(yName);
    const double* d = c.f64();
    int64_t count = c.nbytes / static_cast<int64_t>(sizeof(double));
    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    bool any = false;
    for (int64_t i = 0; i < count; ++i) {
      double v = d[i];
      if (std::isnan(v)) continue;
      any = true;
      if (v < mn) mn = v;
      if (v > mx) mx = v;
    }
    if (any) {
      outMin = mn;
      outMax = mx;
      ok = true;
    }
  }
  if (madeTemp) cecon::native_unlink(*filteredPath);
  if (!ok) {
    errBlockOut = engineErrorBlockM4(
        "tobit: could not compute the observed min/max of `" + yName + "` for a bare ll/ul",
        "engine_error");
    return false;
  }
  return true;
}

}  // namespace est_detail
using namespace est_detail;

// ===========================================================================
// tabstat / correlate / pwcorr / ttest — all route through cobraToEstimationM4
// (dispatcher.py's "4b. Cobra estimator" argv-Dispatch path; no native
// handler in the oracle, just an argv builder + the standard estimation
// wrapper).

// W:P5: tabstat's body, split at the row-filter boundary. runTabstat (below)
// does the filtering and calls this; the `by G: tabstat` driver in
// dispatcher.cpp has ALREADY materialized its group slice — with the
// sub-command's own `if` folded in — and calls runTabstatOnData, which lands
// here without repeating the filter. This split is what lets the by-prefix
// driver REUSE tabstat rather than grow a second copy of it (the P4
// `by: regress` precedent).
namespace {
ojson runTabstatCore(const std::vector<std::string>& varlist,
                      const std::optional<std::string>& byVal,
                      const std::optional<std::string>& statsVal, const std::string& dataPath,
                      bool wantGroups, bool noTotal) {
  const std::string* dataPathOpt = &dataPath;
  std::vector<std::string> args = {"tabstat", "--vars", joinCommaM4(varlist)};
  if (byVal.has_value() && !byVal->empty()) {
    args.push_back("--by");
    args.push_back(*byVal);
  }
  if (statsVal.has_value() && !statsVal->empty()) {
    // space/comma -> comma-joined.
    std::string joined;
    std::string cur;
    for (char c : *statsVal) {
      if (c == ' ' || c == ',') {
        if (!cur.empty()) {
          if (!joined.empty()) joined += ",";
          joined += cur;
          cur.clear();
        }
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) {
      if (!joined.empty()) joined += ",";
      joined += cur;
    }
    args.push_back("--stats");
    args.push_back(joined);
  } else {
    args.push_back("--stats");
    args.push_back("mean,sd,min,max,n");
  }
  args.push_back("--data");
  args.push_back(*dataPathOpt);
  args.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(args);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson block = cobraToEstimationM4(raw, "tabstat");
  // ------------------------------------------------------------------
  // B4-W3b. The engine has ALWAYS emitted a `groups` array under `--by`
  // (verified on this build: `cobra tabstat --by year` returns seven group
  // objects), and cobraToEstimationM4 has never carried it — so `tabstat y x1,
  // by(region)` rendered the TOTAL row only. That gap is NOT closed here, and
  // deliberately: four freeze fixtures (tabstat/corr/pwcorr/levelsof.json)
  // byte-pin the group-less block for exactly that command, and re-pinning
  // them is a main-checkout edit outside this branch's manifest.
  //
  // What IS closed is the half `save`/`nototal` reach: both spellings answered
  // r(198) until this delta, so no captured fixture can contain either, and
  // carrying `groups` only when one of them is present cannot move a pin.
  if (wantGroups && raw.contains("groups") && raw["groups"].is_array()) {
    block["groups"] = raw["groups"];
  }
  if (noTotal) block.erase("total");
  return block;
}
}  // namespace

ojson runTabstat(const std::vector<std::string>& varlist, const std::optional<std::string>& byVal,
                  const std::optional<std::string>& statsVal, SessionState& session,
                  const std::optional<std::string>& ifExpr, bool wantGroups, bool noTotal) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (varlist.empty()) return engineErrorBlockM4("tabstat requires at least one variable");

  ojson ifErr;
  auto dataPathOpt = applyIfFilterM4(session, *session.currentHbin(), ifExpr, ifErr);
  if (!dataPathOpt.has_value()) return ifErr;
  return runTabstatCore(varlist, byVal, statsVal, *dataPathOpt, wantGroups, noTotal);
}

ojson runTabstatOnData(const std::vector<std::string>& varlist,
                        const std::optional<std::string>& statsVal, SessionState& session,
                        const std::string& dataPath) {
  (void)session;  // slice already materialized by the by-prefix driver
  if (varlist.empty()) return engineErrorBlockM4("tabstat requires at least one variable");
  return runTabstatCore(varlist, std::nullopt, statsVal, dataPath, /*wantGroups=*/false,
                        /*noTotal=*/false);
}

ojson runCorr(const std::vector<std::string>& varlist, bool pairwise, bool sig,
              SessionState& session, const std::optional<std::string>& ifExpr,
              const std::optional<std::string>& inRange) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (varlist.empty()) {
    return engineErrorBlockM4("syntax_error: corr requires at least two variables", "syntax_error");
  }
  ojson ifErr;
  auto dataPathOpt = applyIfFilterM4(session, *session.currentHbin(), ifExpr, ifErr, inRange);
  if (!dataPathOpt.has_value()) return ifErr;

  std::vector<std::string> args = {"corr", "--vars", joinCommaM4(varlist)};
  if (pairwise) args.push_back("--pairwise");
  if (sig) args.push_back("--sig");
  args.push_back("--data");
  args.push_back(*dataPathOpt);
  args.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(args);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson corrBlock = cobraToEstimationM4(raw, "corr");
  // BUG-353 (W1 lens L3): `correlate` is an r-class command and PIET published
  // NOTHING — `di r(rho)` after a correlate answered "r(rho) is not set", which
  // is the honest-but-wrong answer to a question Stata answers. Measured on
  // StataBE 19.5 (`sysuse auto`): `correlate mpg weight` sets r(N)=74 and
  // r(rho)=-.80717486, and with THREE variables r(rho) stays the FIRST PAIR's
  // correlation (-.80717486 again), i.e. it is always the [1,2] element rather
  // than something only defined for two variables.
  //
  // The engine already reports both — `r` (the matrix) and `n_obs` — so this is
  // pure carriage, at the one seam that has them.
  if (raw.contains("n_obs") && raw["n_obs"].is_number()) {
    session.eval.rValues.set("N", raw["n_obs"].get<double>());
  }
  if (raw.contains("r") && raw["r"].is_array() && raw["r"].size() >= 2 &&
      raw["r"][0].is_array() && raw["r"][0].size() >= 2 && raw["r"][0][1].is_number()) {
    session.eval.rValues.set("rho", raw["r"][0][1].get<double>());
  }
  return corrBlock;
}

ojson runTtest(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  const std::vector<std::string>& varlist = parsed.varlist;
  const std::string& raw = parsed.raw;
  if (varlist.empty()) {
    return engineErrorBlockM4("syntax_error: ttest requires a variable", "syntax_error");
  }

  std::vector<std::string> levelArgs;
  bool levelExplicit = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      levelExplicit = true;
      try {
        int lv = std::stoi(v.strValue);
        levelArgs = {"--level", std::to_string(lv)};
      } catch (...) {
      }
    }
  }
  // BS-048 (BETA A4): no explicit level() — fall back to the session's `set
  // level`/`set clevel` default. ttest is the ONE PIET-wired command whose
  // cobra CLI genuinely reads --level (cmd_ttest, apps/cmds/cmds_summary.cpp
  // — applies it to alpha/t_critical throughout); session.level==95 default
  // keeps this a no-op until `set level` actually runs. (buildEstimationArgvM4
  // / handlers.cpp::runRegress deliberately do NOT carry this same fallback —
  // see their own comments: cobra's feols/xtreg/logit/probit CLIs never
  // consume --level at all, so forwarding it there would trip cobra's E6
  // unconsumed-flag net instead of doing anything useful.)
  if (!levelExplicit && session.level != 95) {
    levelArgs = {"--level", std::to_string(session.level)};
  }

  std::optional<std::string> byVal;
  bool unequal = false;
  bool paired = false;
  bool reverse = false;
  bool unpaired = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "by" && !v.isBool) byVal = v.strValue;
    if ((k == "unequal" || k == "welch") && v.isBool && v.boolValue) unequal = true;
    if (k == "paired" && v.isBool && v.boolValue) paired = true;
    if (k == "reverse" && v.isBool && v.boolValue) reverse = true;
    if (k == "unpaired" && v.isBool && v.boolValue) unpaired = true;
  }

  // BS-059/BS-089: level/by/unequal/welch/paired/reverse/unpaired are the
  // only ttest options this handler reads — a leftover key reads as an
  // honest reject.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kTtestKnownOpts = kVocabTtest.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kTtestKnownOpts)) {
    return *err;
  }

  std::vector<std::string> args;
  if (byVal.has_value() && !byVal->empty()) {
    args = {"ttest", "--var", varlist[0], "--by", *byVal};
    if (unequal) args.push_back("--unequal");
    if (reverse) args.push_back("--reverse");
    for (const auto& a : levelArgs) args.push_back(a);
  } else {
    std::smatch m;
    std::regex eqRe(R"(==\s*(\S+))");
    if (std::regex_search(raw, m, eqRe)) {
      std::string rhs = trimM4(m[1].str());
      try {
        size_t pos;
        std::stod(rhs, &pos);  // Python's float(rhs) — value unused, only validity matters
        if (pos == rhs.size()) {
          args = {"ttest", "--var", varlist[0], "--against", rhs};
          if (paired) args.push_back("--paired");
          for (const auto& a : levelArgs) args.push_back(a);
        } else {
          throw std::invalid_argument("not fully numeric");
        }
      } catch (...) {
        // rhs is a variable name -> paired/two-sample.
        args = {"ttest", "--vars", varlist[0] + "," + rhs};
        if (paired) args.push_back("--paired");
        if (unpaired) args.push_back("--unpaired");
        if (unpaired && unequal) args.push_back("--unequal");
        if (reverse) args.push_back("--reverse");
        for (const auto& a : levelArgs) args.push_back(a);
      }
    } else if (varlist.size() >= 2) {
      args = {"ttest", "--vars", varlist[0] + "," + varlist[1]};
      if (paired) args.push_back("--paired");
      if (unpaired) args.push_back("--unpaired");
      if (unpaired && unequal) args.push_back("--unequal");
      if (reverse) args.push_back("--reverse");
      for (const auto& a : levelArgs) args.push_back(a);
    } else {
      args = {"ttest", "--var", varlist[0], "--against", "0"};
      for (const auto& a : levelArgs) args.push_back(a);
    }
  }

  // BUG-042: `ttest ... if COND` — ttest's own cobra CLI has no --keepif;
  // pre-filter via the shared row-filter infra (same helper tabstat/corr use
  // above). `parsed.ifExpr`/`parsed.inRange` are already available on this
  // handler's own argument (no signature change needed, unlike tabstat/corr).
  // BS-090 (BETA Phase G): `in` now composes with `if` via AND here too.
  ojson ifErr;
  auto dataPathOpt =
      applyIfFilterM4(session, *session.currentHbin(), parsed.ifExpr, ifErr, parsed.inRange);
  if (!dataPathOpt.has_value()) return ifErr;

  args.push_back("--data");
  args.push_back(*dataPathOpt);
  args.push_back("--tidy");

  ojson raw2;
  try {
    raw2 = runCobraJsonM4(args);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson ttBlock = cobraToEstimationM4(raw2, "ttest");
  // BUG-165 (W:P5): the per-group dispersions Stata prints in the group rows
  // ("Std. dev."). Forwarded HERE rather than from the shared passthrough
  // list, because `sdtest` emits the same two key names and already forwards
  // them from its own field list — adding them to the shared list moved them
  // earlier in sdtest's key order, a byte change to an output this fix has no
  // business touching.
  for (const char* k : {"sd_1", "sd_2"}) {
    if (raw2.contains(k)) ttBlock[k] = raw2[k];
  }
  // BUG-354 (W1 lens L3): `ttest` is r-class and published nothing, so
  // `di r(t)` after one answered "r(t) is not set". Stata's one-sample return
  // list, measured on StataBE 19.5 (`ttest mpg == 20` on auto): mu_1 21.297297,
  // N_1 74, t 1.9289201, p .05762867, se .67255109, df_t 73. NOTE r(sd) is
  // MISSING there — Stata leaves it unset for the one-sample form — so this
  // publishes sd only when the engine actually reports one, rather than
  // inventing a value Stata does not have.
  {
    auto put = [&](const char* rname, const char* jkey) {
      if (raw2.contains(jkey) && raw2[jkey].is_number())
        session.eval.rValues.set(rname, raw2[jkey].get<double>());
    };
    put("N_1", "n_obs");
    put("mu_1", "mean");
    put("t", "t");
    put("p", "p");
    put("se", "se");
    put("df_t", "df");
    put("sd", "sd");
  }
  return ttBlock;
}

namespace {

// dispatcher.py's "Cache last_model for test/margins/predict/lincom/
// coefplot/eststo" block — extracts --y/--x/--fe/--vcov/--cluster from the
// argv actually sent to cobra (post TS-injection) so post-estimation
// commands reconstruct the SAME call. Mirrors handlers.cpp::runRegress's
// identical inline logic (M2), duplicated here per this file's
// established one-copy-per-milestone-file convention.
void cacheLastModelM4(SessionState& session, const std::string& cobraCmd,
                       const std::vector<std::string>& argv, const ojson& block, const ojson& raw) {
  LastModel lm;
  lm.cobraCmd = cobraCmd;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "--y" && i + 1 < argv.size()) lm.y = argv[i + 1];
    if (argv[i] == "--x" && i + 1 < argv.size()) {
      std::string joined = argv[i + 1];
      std::string cur;
      for (char c : joined) {
        if (c == ',') {
          if (!cur.empty()) lm.x.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      if (!cur.empty()) lm.x.push_back(cur);
    }
    if (argv[i] == "--fe" && i + 1 < argv.size()) {
      std::string joined = argv[i + 1];
      std::string cur;
      for (char c : joined) {
        if (c == ',') {
          if (!cur.empty()) lm.fe.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      if (!cur.empty()) lm.fe.push_back(cur);
    }
    if (argv[i] == "--vcov" && i + 1 < argv.size()) lm.vcov = argv[i + 1];
    // W:P5: logit/probit spell the same selector `--vce` (buildEstimationArgvM4
    // remaps `--vcov hc1|cluster` onto it), so scanning only `--vcov` left
    // lm.vcov at its "iid" default for exactly the models that DID have a
    // robust/cluster VCE — and every post-estimation command then replayed the
    // fit as iid. Normalize back to the linear spelling so this one field
    // means one thing no matter which CLI produced it.
    if (argv[i] == "--vce" && i + 1 < argv.size()) {
      lm.vcov = (argv[i + 1] == "robust") ? std::string("hc1") : argv[i + 1];
    }
    if (argv[i] == "--cluster" && i + 1 < argv.size()) lm.cluster.push_back(argv[i + 1]);
  }
  lm.depvar = block.value("depvar", "");
  captureFitFilterM4(lm, argv);
  lm.dataPath = session.currentHbin().value_or("");
  lm.result = raw;
  // BUG-570, the twin of runRegress's rule: e(N_clust) exists only after a
  // clustered fit, but the engine's feols reports n_clusters 0 for every other
  // VCE, so `xtreg ..., fe` (routed here as "feols") printed e(N_clust) 0 where
  // Stata prints `.` (w2_pweight.log X0_fe PSTR `120|103|.|conventional|||||`,
  // X3_aw_fe alike). logit/probit and xtreg,re were already `.` -- their engines
  // omit the key when unclustered (measured on main 2026-09-28) -- so this is a
  // no-op for them. Dropped from the cached reply only; the emitted block is
  // unchanged.
  if (lm.result.has_value() && lm.result->is_object() && lm.result->contains("n_clusters") &&
      (*lm.result)["n_clusters"].is_number() &&
      (*lm.result)["n_clusters"].get<double>() == 0.0) {
    lm.result->erase("n_clusters");
  }
  session.setLastModel(lm);
  session.lastEstBlock = block;
}

// FU-29: xtreg_fe.ado's weight rule. After `mark touse [weight] if in` and
// `markout touse ivar tvar varlist` it runs `by ivar touse: assert w == w[_N]
// if touse` and stops with "weight must be constant within <ivar>" r(199)
// (w2_pweight.log X10_pw_varies_within_id; aer_b4 predict-e-xbd.log
// G_w_varies, aweight). The engine fits a varying weight without complaint, so
// the check has to happen here, before the fit.
//
// The rows checked are a SUPERSET of Stata's sample: the `if` filter, a
// non-missing panel id, a non-missing and nonzero weight (`mark` drops zero
// weights), and a non-missing depvar and plain/c./i. regressor. Terms behind
// any other operator (L./D./ib2./...) do not narrow the rows. A superset can
// only refuse where Stata would fit (an honest r(199) on a weight that varies
// only on rows Stata drops); it can never fit where Stata refuses.
// Returns nullopt when the weight passes.
std::optional<ojson> xtregWeightWithinPanelM4(const eval::ParsedCommand& parsed,
                                              SessionState& session, const std::string& ivar) {
  const std::string& wvar = *parsed.weightVar;
  const std::string basePath = *session.currentHbin();
  ojson filterErr;
  auto filteredPath = applyIfFilterM4(session, basePath, parsed.ifExpr, filterErr);
  if (!filteredPath.has_value()) return filterErr;
  const bool madeTemp = (*filteredPath != basePath);
  std::optional<ojson> verdict;
  try {
    std::optional<Datastore> tempDs;
    Datastore* ds = nullptr;
    if (madeTemp) {
      tempDs.emplace(*filteredPath);
      ds = &*tempDs;
    } else {
      ds = session.datastore();
    }
    // A weight that is not a column of the data (an expression such as
    // `[aw=1/x]`) is refused by the engine itself ("column not found"), and a
    // panel variable is always a column, so only the type needs a verdict here.
    if (ds != nullptr && ds->has(wvar) && ds->has(ivar)) {
      if (ds->dtype(wvar) != "f64" || ds->dtype(ivar) != "f64") {
        verdict = engineErrorBlockM4("xtreg: could not verify that the weight is constant within " +
                                     ivar + " (weight and panel variable must be numeric)");
      } else {
        std::vector<const double*> need;
        int64_t n = ds->nRows();
        auto add = [&](const std::string& name) {
          if (!ds->has(name) || ds->dtype(name) != "f64") return;
          const cecon::Column& c = ds->col(name);
          const int64_t rows = c.nbytes / static_cast<int64_t>(sizeof(double));
          if (rows < n) n = rows;
          need.push_back(c.f64());
        };
        add(wvar);
        add(ivar);
        const double* w = need[0];  // both are f64 columns (checked just above)
        const double* g = need[1];
        for (const auto& term : parsed.varlist) {
          size_t start = 0;
          while (start <= term.size()) {
            const size_t hash = term.find('#', start);
            const std::string part =
                term.substr(start, hash == std::string::npos ? std::string::npos : hash - start);
            start = (hash == std::string::npos) ? term.size() + 1 : hash + 1;
            if (part.empty()) continue;
            const size_t dot = part.rfind('.');
            if (dot == std::string::npos) {
              add(part);
            } else {
              const std::string op = lowerM4(part.substr(0, dot));
              if (op == "c" || op == "i") add(part.substr(dot + 1));
            }
          }
        }
        std::map<double, double> first;  // panel id -> its first weight in the sample
        for (int64_t i = 0; i < n && !verdict.has_value(); ++i) {
          bool in = !(w[i] == 0.0);
          for (const double* col : need) {
            if (std::isnan(col[i])) in = false;
          }
          if (!in) continue;
          auto it = first.emplace(g[i], w[i]).first;
          if (it->second != w[i]) {
            verdict = ojson(eval::errorBlock("engine_error", "weight must be constant within " + ivar,
                                             std::nullopt, std::nullopt, std::nullopt, "r(199)"));
          }
        }
      }
    }
  } catch (const std::exception& e) {
    verdict = engineErrorBlockM4(std::string("xtreg: could not read the data to check the weight: ") +
                                 e.what());
  }
  if (madeTemp) cecon::native_unlink(*filteredPath);
  return verdict;
}

// FU-76: the e() spellings of the panel R-squared pair. The engine reports them
// as r2_between/r2_overall (feols single-FE, apps/cmds/common.cpp
// add_panel_scalars; xtreg,re, cmds_panel.cpp), and the e() resolver only knew
// `r2_w`, so e(r2_b)/e(r2_o) read `.` beside a number the reply already held
// (w2_pweight.log X0_fe PXT +1.f03574901785fX-001 / +1.5afec63f08acbX-001; the
// engine's 0.9691578317540571 / 0.6777250244088803). Copied into the cached
// reply only, as N_g/df_a are: the emitted block's shape is freeze-pinned. A
// reply without the source key (weighted fits, see runXtreg) stays `.`.
void aliasPanelR2M4(ojson& raw) {
  if (!raw.is_object()) return;
  if (raw.contains("r2_between") && raw["r2_between"].is_number()) raw["r2_b"] = raw["r2_between"];
  if (raw.contains("r2_overall") && raw["r2_overall"].is_number()) raw["r2_o"] = raw["r2_overall"];
}

}  // namespace

// ===========================================================================
// xtreg — fe routes to feols with --fe=panelvar; re routes to cobra xtreg
// with --ivar=panelvar (no vcov support).

ojson runXtreg(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  for (const auto& [k, v] : parsed.opts) {
    static const std::set<std::string> kBad = {"be", "gls", "mle", "ec1", "pa"};
    if (kBad.count(k) && v.isBool && v.boolValue) {
      return engineErrorBlockM4(
          "This version of PIET supports only `xtreg ,fe` (-> feols within) and "
          "`xtreg ,re` (Swamy-Arora GLS). `xtreg ," + k + "` is not available.",
          "unsupported_option");
    }
  }
  // model = str(opts.get("fe", opts.get("re", ""))).lower() — reads fe's
  // value if the key is present (bare flag -> "true"), else falls back to
  // re's value, else "". isFe = (model=="fe") or (opts.get("fe") is True).
  bool fePresent = false, rePresent = false;
  eval::OptValue feVal, reVal;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "fe") {
      fePresent = true;
      feVal = v;
    }
    if (k == "re") {
      rePresent = true;
      reVal = v;
    }
  }
  bool isFe = false;
  if (fePresent) {
    isFe = feVal.isBool && feVal.boolValue;  // opts.get("fe") is True (bare flag)
  } else if (rePresent) {
    isFe = false;
  } else {
    isFe = false;  // model == "" -> not "fe" -> RE branch (oracle errors there on missing panelvar)
  }

  if (isFe) {
    // BUG-657: Stata's own refusal (xtreg.ado -> `_xt, i()`): "must specify
    // panelvar; use xtset" r(459) -- w2_eststo_prefix.log:437-441 and
    // w2d_uncovered.log:3468-3473. It was unsupported_option = r(198), so a
    // do-file branching on `_rc == 459` took the wrong arm.
    if (!session.panelvar.has_value() || session.panelvar->empty()) {
      return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset",
                                    std::nullopt, std::nullopt, std::nullopt, "r(459)"));
    }
    // FU-29: xtreg_fe.ado's `syntax ... [aw fw pw]` -- an iweight is Stata's
    // r(101) "iweights not allowed" (w2_pweight.log X11_iw_fe). The registry
    // row is AllWeights because the RE arm below has a different rule.
    const bool weighted =
        parsed.weightType.has_value() && parsed.weightVar.has_value() && !parsed.weightVar->empty();
    if (weighted) {
      const std::string wt = lowerM4(*parsed.weightType);
      if (wt == "iw" || wt == "iweight") {
        return ojson(eval::errorBlock("engine_error", "iweights not allowed", std::nullopt,
                                      std::nullopt, std::nullopt, "r(101)"));
      }
    }
    EstimationArgvResult built =
        buildEstimationArgvM4("feols", parsed, session, *session.panelvar);
    if (built.errored) return built.errorBlock;
    if (weighted) {
      if (auto refused = xtregWeightWithinPanelM4(parsed, session, *session.panelvar)) {
        return *refused;
      }
    }
    std::vector<std::string> cobraArgs = built.argv;
    // BUG-538 (W2 estimopts): under `xtreg, fe` Stata's vce(robust) / bare
    // `robust` IS vce(cluster panelvar) -- w2_pweight.log X1_fe_vcerobust
    // is byte-identical to X2_fe_clusterid (e(vce)=cluster, e(clustvar)=id,
    // N_clust 15, df_r 14, se_x1 +1.0ba4e5218cb87X-002), where this route
    // forwarded HC1 (n_clusters 0, df_r 103) at rc 0. The shared builder
    // cannot know the panel variable, so the rewrite lives here, after it.
    for (size_t i = 0; i + 1 < cobraArgs.size(); ++i) {
      if (cobraArgs[i] == "--vcov" && cobraArgs[i + 1] == "hc1") {
        cobraArgs[i + 1] = "cluster";
        cobraArgs.insert(cobraArgs.begin() + static_cast<std::ptrdiff_t>(i + 2),
                         {"--cluster", *session.panelvar});
        break;
      }
    }
    cobraArgs.push_back("--data");
    cobraArgs.push_back(*session.currentHbin());
    cobraArgs.push_back("--tidy");
    ojson raw;
    try {
      raw = runCobraJsonM4(cobraArgs);
    } catch (const CobraError& ce) {
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
    // FU-29: e(sigma_e) of a weighted fit. The engine's panel-scalar producer
    // (apps/cmds/common.cpp capture_panel_means) stands down under weights, so
    // the weighted reply has no sigma_e -- but it has rmse, and for the within
    // estimator both are sqrt(rss / (N - N_g - k)) (the unweighted reply
    // carries the two keys bit-identical: 2.4986263477117516). Filled only when
    // the reply's own residual dof IS N - N_g - k. Oracle: w2_pweight.log X3-X9
    // PVAL sigma_e +1.4013601975c1dX+001 = the engine's weighted rmse
    // 2.500591289931903; X14 (fweight) header sigma_e 2.3734216 = rmse with dof
    // 343. sigma_u/rho/corr/r2_b/r2_o have no such identity and stay `.`,
    // named in a note on the block (see below).
    if (raw.is_object() && !raw.contains("sigma_e") && raw.contains("rmse") &&
        raw["rmse"].is_number() && raw.contains("dof_resid") && raw["dof_resid"].is_number() &&
        raw.contains("n_obs") && raw["n_obs"].is_number() && raw.contains("k_vars") &&
        raw["k_vars"].is_number() && raw.contains("absorbed") && raw["absorbed"].is_object() &&
        raw["absorbed"].size() == 1 && raw["absorbed"].contains(*session.panelvar) &&
        raw["absorbed"][*session.panelvar].is_number()) {
      const double dofWithin = raw["n_obs"].get<double>() -
                               raw["absorbed"][*session.panelvar].get<double>() -
                               raw["k_vars"].get<double>();
      if (raw["dof_resid"].get<double>() == dofWithin) raw["sigma_e"] = raw["rmse"];
    }
    ojson block = cobraToEstimationM4(raw, "feols", &session);
    // BUG-538, e() half: [XT] xtreg stores e(N_g) (panels in the fit) and
    // e(df_a) = N_g - 1 (w2_pweight.log X0_fe PXT: 15 and 14; X7 under
    // `if t>2`: still 15 and 14). The engine reports the panel count as the
    // absorbed-levels entry for the panel variable; it is copied into the
    // cached reply under the xtreg names so e(N_g)/e(df_a) resolve (the
    // resolver falls through key==name), and NOT into the emitted block,
    // whose shape the freeze fixtures pin.
    if (raw.is_object() && raw.contains("absorbed") && raw["absorbed"].is_object() &&
        raw["absorbed"].contains(*session.panelvar) &&
        raw["absorbed"][*session.panelvar].is_number()) {
      const double ng = raw["absorbed"][*session.panelvar].get<double>();
      raw["N_g"] = ng;
      raw["df_a"] = ng - 1.0;
    }
    // BUG-383 #1 (off-pin sweep vs live Stata, 2026-09-01): the xtreg,fe route
    // published the underlying feols block untouched, and two of its members
    // are feols conventions, not xtreg ones. [XT] xtreg stores e(r2) as the
    // WITHIN R-squared (captured: 0.07589076642404147 — our raw r2_within
    // matches it to 1e-15), where feols's r2 is the FULL-model LSDV fit
    // (0.9905). And e(df_m) counts the absorbed group dummies alongside the
    // slopes (captured: 48 = 1 slope + 47 dummies), where feols reports the
    // slope count alone. Both raw ingredients are already in the engine reply
    // (r2_within, k_vars, k_absorbed); this route just has to pick the xtreg
    // spellings. The r2_w/r2_b/r2_o trio and the rendered header are untouched.
    if (raw.contains("r2_within")) block["r2"] = raw["r2_within"];
    if (raw.contains("k_vars") && raw.contains("k_absorbed") &&
        raw["k_absorbed"].is_number() && raw["k_absorbed"].get<double>() >= 1.0) {
      block["df_m"] = raw["k_vars"].get<double>() + raw["k_absorbed"].get<double>() - 1.0;
    }
    // FU-76: the same two members, e() side. BUG-383 wrote them into the block
    // only, while e() resolves from the cached reply, so e(r2) read the LSDV fit
    // (0.7235263265398173) and e(df_m) the slope count 2 (w2_pweight.log X0_fe
    // PVAL e(r2) +1.132199730c722X-001 = r2_within 0.5373657181682785).
    // e(df_m) follows xtreg_fe.ado's two arms: conventional VCE posts
    // `df_m = e(df_a) + dfb` (X0_fe and X3_aw_fe: 2 + 14 = 16), a cluster VCE
    // posts _regress's own model df, which the oracle shows as one less than
    // the slope count: X1, X2, X4, X5, X7, X8, X9 (cluster id, 15 clusters)
    // and X6 (cluster cl2, 5 clusters) all 1 with k = 2, X13 2 with k = 3.
    // Other VCE shapes are not witnessed and keep the engine's value. The
    // block's df_m is left as it was (freeze-pinned; see BUG-383 above).
    if (raw.contains("r2_within")) raw["r2"] = raw["r2_within"];
    if (raw.contains("k_vars") && raw["k_vars"].is_number() && raw.contains("df_a") &&
        raw["df_a"].is_number()) {
      const double k = raw["k_vars"].get<double>();
      const std::string vt = raw.value("vcov_type", std::string());
      if (vt == "iid") {
        raw["df_m"] = k + raw["df_a"].get<double>();
      } else if (vt.rfind("cluster(", 0) == 0 && k >= 1.0) {
        raw["df_m"] = k - 1.0;
      }
    }
    aliasPanelR2M4(raw);
    // FU-29 review: a weighted fit's reply has no sigma_u/rho/corr_u_xb/
    // r2_between/r2_overall (the engine's capture_panel_means stands down under
    // weights; engine follow-up FU-126), so those e() fields read `.` where
    // Stata posts numbers (w2_pweight.log X3_aw_fe PXT :352 sigma_u
    // +1.4e5921bc56b86X+000, rho +1.b6f09048fd04cX-003, corr
    // +1.0d40c44e4227aX-001; X4-X9 and X13 alike). The fit itself matches, so
    // it is kept, and the gap is named on the block's `warnings` (rendered as
    // `note:` lines, BUG-112(8)) rather than left as a silent `.`. Only the
    // fields actually absent are named; an unweighted fit carries all five and
    // gets no note (its block stays freeze-identical).
    if (weighted && raw.is_object()) {
      static const std::vector<std::pair<const char*, const char*>> kPanelScalars = {
          {"sigma_u", "sigma_u"}, {"rho", "rho"}, {"corr", "corr_u_xb"},
          {"r2_b", "r2_b"},       {"r2_o", "r2_o"}};
      std::vector<std::string> absent;
      for (const auto& [eName, key] : kPanelScalars) {
        if (!raw.contains(key) || !raw[key].is_number()) absent.push_back(std::string("e(") + eName + ")");
      }
      if (!absent.empty()) {
        std::string list;
        for (size_t i = 0; i < absent.size(); ++i) {
          if (i > 0) list += (i + 1 == absent.size()) ? " and " : ", ";
          list += absent[i];
        }
        if (!block.contains("warnings") || !block["warnings"].is_array()) block["warnings"] = ojson::array();
        block["warnings"].push_back(list + (absent.size() == 1 ? " is" : " are") +
                                    " not computed for weighted xtreg, fe in this engine (left missing)");
      }
    }
    cacheLastModelM4(session, "feols", cobraArgs, block, raw);
    // e(vce) after a default-VCE `xtreg, fe` is `conventional` (w2_pweight.log
    // X0_fe PSTR `120|103|.|conventional|||||`; X3_aw_fe alike). setLastModel()
    // published lm.vcov's engine spelling `iid`, which the replay machinery keys
    // on, so only the macro store is corrected. vce(robust)/cluster keep
    // `cluster` (X1/X2 PSTR). Same KNOWN GAP as runRegress's `ols`: a later
    // setLastModel() on this model (setEstHandleM4) republishes `iid`.
    if (session.lastModel().has_value() && session.lastModel()->vcov == "iid") {
      session.eval.setEString("vce", "conventional");
    }
    return block;
  }

  // Random effects.
  // BUG-657: same `_xt, i()` refusal as the fixed-effects arm above (xtreg.ado
  // calls it before choosing the model), r(459).
  if (!session.panelvar.has_value() || session.panelvar->empty()) {
    return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset", std::nullopt,
                                  std::nullopt, std::nullopt, "r(459)"));
  }
  // FU-29: xtreg_re.ado's `syntax` declares no weight, so any weight type is
  // Stata's r(101) "weights not allowed" (w2_pweight.log X12_pw_re). Until the
  // registry row opened to AllWeights the tokenizer refused it as r(198).
  if (parsed.weightType.has_value() && parsed.weightVar.has_value() &&
      !parsed.weightVar->empty()) {
    return ojson(eval::errorBlock("engine_error", "weights not allowed", std::nullopt, std::nullopt,
                                  std::nullopt, "r(101)"));
  }
  for (const auto& [k, v] : parsed.opts) {
    // BUG-055/P2 side effect: legacy `cluster(v)` (no vce()/robust) now
    // maps to `vce(cluster v)` inside vcovFromOpts (shared by this RE
    // path), so it must be caught HERE too — otherwise it would sail past
    // this guard and reach cobra's RE call as an unsupported --vcov/
    // --cluster flag pair instead of getting this same clean message.
    if ((k == "vce" || k == "vcov" || k == "robust" || k == "cluster") &&
        !(v.isBool && !v.boolValue)) {
      return engineErrorBlockM4("cobra xtreg (random-effects) does not support vcov options.",
                                "unsupported_option");
    }
  }
  EstimationArgvResult built = buildEstimationArgvM4("xtreg", parsed, session);
  if (built.errored) return built.errorBlock;
  std::vector<std::string> cobraArgs = built.argv;
  cobraArgs.push_back("--ivar");
  cobraArgs.push_back(*session.panelvar);
  cobraArgs.push_back("--data");
  cobraArgs.push_back(*session.currentHbin());
  cobraArgs.push_back("--tidy");
  ojson raw;
  try {
    raw = runCobraJsonM4(cobraArgs);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson block = cobraToEstimationM4(raw, "xtreg", &session);
  aliasPanelR2M4(raw);
  cacheLastModelM4(session, "xtreg", cobraArgs, block, raw);
  return block;
}

// ===========================================================================
// P7 — ordered / multinomial / zero-inflated / fractional MLE
//
// These engine verbs take `--vce iid|robust|cluster --cluster C` (the ML family
// spelling), NOT buildEstimationArgvM4's `--vcov`, so they share a small builder
// of their own rather than bending the shared one and risking the frozen output
// of every existing caller.

namespace est_detail {  // R2: reopened — these cross the TU cut (runP7Mle & friends)

struct P7MleArgv {
  bool errored = false;
  ojson errorBlock;
  std::vector<std::string> argv;
};

// Builds `CMD --y Y --x X1,X2 [--keepif E] [--vce ... [--cluster C]]`.
// `vceAllowed` names the vce spellings this verb actually implements; anything
// else is an honest reject rather than a silent downgrade to iid.
// BUG-333: forward Stata's `level(#)` to the engine. All six commands wired
// through this (nl/gmm, mixed, fracreg, ivprobit/ivtobit) register AND consume
// `--level` in cobra today (options_table.cpp:110-118); PIET was simply not
// passing it, so the option was an honest-but-unnecessary reject. Kept as a
// free function rather than a branch inside buildP7MleArgv because that builder
// is shared with commands whose engine half has NO --level (xtlogit), and a
// blanket push there would turn a working call into an E6 unconsumed-flag error.
// Same idiom as runTobit/runQreg/runNbreg: forward the string, let the engine
// own the [10, 99.99] range check and surface it through CobraError.
void pushLevelIfGivenM4(const eval::ParsedCommand& parsed, std::vector<std::string>& argv) {
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      argv.push_back("--level");
      argv.push_back(v.strValue);
      return;
    }
  }
}

P7MleArgv buildP7MleArgv(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                         const std::vector<std::string>& vceAllowed) {
  P7MleArgv r;
  if (parsed.varlist.size() < 2) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(
        "syntax_error: " + cobraCmd + " requires a dependent variable and at least one regressor",
        "syntax_error");
    return r;
  }
  const std::string y = parsed.varlist[0];
  const std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());
  r.argv = {cobraCmd, "--y", y, "--x", joinCommaM4(xs)};
  if (parsed.ifExpr.has_value()) {
    r.argv.push_back("--keepif");
    r.argv.push_back(*parsed.ifExpr);
  }

  if (auto shapeErr = vceOptionShapeErrorM4(parsed)) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(*shapeErr, "unsupported_option");
    return r;
  }
  std::optional<std::string> vceVal, clusterOpt;
  bool robustFlag = false;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !vceVal.has_value())
      vceVal = v.isBool ? std::string() : v.strValue;
    else if (k == "robust" && v.isBool && v.boolValue)
      robustFlag = true;
    else if (k == "cluster" && !v.isBool)
      clusterOpt = v.strValue;
  }
  std::string want;
  std::string cvar = clusterOpt.value_or("");
  if (vceVal.has_value() && !vceVal->empty()) {
    std::string lv = lowerM4(*vceVal);
    if (lv.rfind("cluster", 0) == 0) {
      want = "cluster";
      if (lv.size() > 7) {
        std::string rest = trimM4(lv.substr(7));
        if (!rest.empty()) cvar = rest;
      }
    } else {
      want = lv;
    }
  } else if (!cvar.empty()) {
    // Stata's legacy bare `cluster(v)` is a synonym for vce(cluster v) — the
    // BUG-055 class. Honouring it here keeps these verbs from silently running
    // iid while the user believes they clustered. BUG-537: tested BEFORE the
    // bare `robust` flag -- `robust cluster(v)` together is cluster-robust in
    // Stata (w2_pweight.log R5_pw_robust_AND_cluster_equals_R4); robust-first
    // dropped the cluster. Same ordering as vcovFromOpts above.
    want = "cluster";
  } else if (robustFlag) {
    want = "robust";
  }
  if (want.empty()) return r;  // engine default (iid)

  bool allowed = false;
  for (const auto& s : vceAllowed)
    if (s == want) { allowed = true; break; }
  if (!allowed) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(
        "Option `vce(" + want + ")` is a valid Stata option that this version of PIET does not "
        "implement for `" + cobraCmd + "`.",
        "unsupported_option");
    return r;
  }
  if (want == "cluster") {
    // A second cluster variable would be two-way clustering, which none of these
    // ML estimators implements — rejecting is parity, quietly using the first
    // variable is not.
    std::istringstream iss(cvar);
    std::vector<std::string> toks;
    std::string t;
    while (iss >> t) toks.push_back(t);
    if (toks.empty()) {
      r.errored = true;
      r.errorBlock = engineErrorBlockM4("Cluster variable required: vce(cluster varname)",
                                        "unsupported_option");
      return r;
    }
    if (toks.size() > 1) {
      r.errored = true;
      r.errorBlock = engineErrorBlockM4(
          "Two-way clustering is not implemented for `" + cobraCmd + "`.", "unsupported_option");
      return r;
    }
    r.argv.push_back("--vce");
    r.argv.push_back("cluster");
    r.argv.push_back("--cluster");
    r.argv.push_back(toks[0]);
  } else {
    r.argv.push_back("--vce");
    r.argv.push_back(want);
  }
  return r;
}

// rawOut, when given, receives the engine payload BEFORE cobraToEstimationM4
// narrows it to the known block keys — the ordered models need it because their
// cutpoints ride in a key the mapper does not carry through.
ojson runP7Mle(const std::string& cobraCmd, std::vector<std::string> cobraArgs,
               SessionState& session, ojson* rawOut) {
  cobraArgs.push_back("--data");
  cobraArgs.push_back(*session.currentHbin());
  cobraArgs.push_back("--tidy");
  ojson raw;
  try {
    raw = runCobraJsonM4(cobraArgs);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  if (rawOut) *rawOut = raw;
  ojson block = cobraToEstimationM4(raw, cobraCmd, &session);
  fillCoeftableStatsM4(block);
  cacheLastModelM4(session, cobraCmd, cobraArgs, block, raw);
  return block;
}

}  // namespace est_detail

// `ologit depvar indepvars [if] [, vce(robust|cluster V)]` and its probit twin.
ojson runOrdered(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                 SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildP7MleArgv(cobraCmd, parsed, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  ojson raw;
  ojson block = runP7Mle(cobraCmd, b.argv, session, &raw);
  // Stata prints the cutpoints in the SAME table as the coefficients, under a
  // /cut1, /cut2 … stanza. The engine emits them under their own `cuts` key
  // (they are not slopes and are excluded from the model Wald test), and
  // cobraToEstimationM4 carries only its known key set through — so without
  // this the rendered table had no cutpoints in it at all: an ordered model
  // missing exactly the parameters that make it ordered. Read from the RAW
  // payload for that reason. z and p stay null because Stata leaves those
  // columns blank for cuts.
  if (raw.contains("cuts") && raw["cuts"].is_array() && !raw["cuts"].empty()) {
    if (!block.contains("coeftable") || !block["coeftable"].is_array())
      block["coeftable"] = ojson::array();
    for (const auto& c : raw["cuts"]) {
      ojson row;
      row["name"] = c.value("name", "");
      row["b"] = c.contains("b") ? c["b"] : ojson(nullptr);
      row["se"] = c.contains("se") ? c["se"] : ojson(nullptr);
      row["t"] = nullptr;
      row["p"] = nullptr;
      row["ci_lo"] = c.contains("ci_lo") ? c["ci_lo"] : ojson(nullptr);
      row["ci_hi"] = c.contains("ci_hi") ? c["ci_hi"] : ojson(nullptr);
      block["coeftable"].push_back(row);
    }
  }
  return block;
}

// `mlogit depvar indepvars [if] [, baseoutcome(#) vce(...)]`.
ojson runMlogit(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildP7MleArgv("mlogit", parsed, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "baseoutcome" || k == "base") && !v.isBool) {
      b.argv.push_back("--base");
      b.argv.push_back(trimM4(v.strValue));
    }
  }
  return runP7Mle("mlogit", b.argv, session);
}

// `zip`/`zinb depvar indepvars [if], inflate(...) [vce(robust)]`.
// inflate() is REQUIRED by Stata and only the constant-only form is implemented;
// a covariate list there would be a different model, so it rejects rather than
// quietly fitting the constant-only one.
ojson runZeroInflated(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                      SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildP7MleArgv(cobraCmd, parsed, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  std::optional<std::string> inflate;
  for (const auto& [k, v] : parsed.opts)
    if (k == "inflate" && !v.isBool) inflate = trimM4(v.strValue);
  if (!inflate.has_value()) {
    return engineErrorBlockM4("syntax_error: " + cobraCmd + " requires inflate()", "syntax_error");
  }
  if (*inflate != "_cons") {
    return engineErrorBlockM4(
        "Option `inflate(" + *inflate + ")` is a valid Stata option that this version of PIET does "
        "not implement for `" + cobraCmd + "` — only the constant-only inflation equation "
        "`inflate(_cons)` is available.",
        "unsupported_option");
  }
  b.argv.push_back("--inflate");
  b.argv.push_back("_cons");
  return runP7Mle(cobraCmd, b.argv, session);
}

// `fracreg logit depvar indepvars [if] [, vce(robust)]` — the link is a leading
// positional token, not an option.
ojson runFracreg(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.empty())
    return engineErrorBlockM4("syntax_error: fracreg requires a link and a dependent variable",
                              "syntax_error");
  const std::string link = lowerM4(trimM4(parsed.varlist[0]));
  if (link != "logit" && link != "probit")
    return engineErrorBlockM4("syntax_error: fracreg requires a link (logit or probit)",
                              "syntax_error");
  if (link == "probit")
    return engineErrorBlockM4(
        "`fracreg probit` is a valid Stata command that this version of PIET does not implement "
        "(only the logit link is available).",
        "unsupported_command");
  eval::ParsedCommand shifted = parsed;
  shifted.varlist.erase(shifted.varlist.begin());
  P7MleArgv b = buildP7MleArgv("fracreg", shifted, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  b.argv.push_back("--link");
  b.argv.push_back("logit");
  pushLevelIfGivenM4(parsed, b.argv);
  return runP7Mle("fracreg", b.argv, session);
}

// ===========================================================================
// P7 — vector autoregression: `var`, `vargranger`, `irf create`
//
// ONE engine verb serves all three: it returns the per-equation results, the
// Granger block and the IRF cells in a single payload. `vargranger` is therefore
// a pure re-render of the stored fit — zero new mathematics on this side, which
// is the point — and `irf create` materializes the cells as a `.irf` file (which
// is a Stata `.dta`).

ojson runVar(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // BUG-657: var.ado runs `tsset, noquery` -> `_ts`, whose refusal is "time
  // variable not set, use tsset varname ..." r(111) (_ts.ado); was
  // unsupported_option = r(198).
  if (!session.timevar.has_value() || session.timevar->empty())
    return ojson(eval::errorBlock("engine_error", "time variable not set, use tsset varname ...",
                                  std::nullopt, std::nullopt, std::nullopt, "r(111)"));
  if (parsed.varlist.size() < 2)
    return engineErrorBlockM4("syntax_error: var requires at least two endogenous variables",
                              "syntax_error");
  std::string lags = "1/2";
  for (const auto& [k, v] : parsed.opts) {
    if (k == "lags" && !v.isBool) lags = trimM4(v.strValue);
    else if ((k == "exog" || k == "constraints" || k == "dfk") && !v.isBool)
      return engineErrorBlockM4(
          "Option `" + k + "()` is a valid Stata option that this version of PIET does not "
          "implement for `var`.",
          "unsupported_option");
  }
  std::vector<std::string> argv = {"var", "--y", joinCommaM4(parsed.varlist), "--lags", lags,
                                   "--tsset", *session.timevar, "--irf-steps", "8"};
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  ojson raw;
  ojson block = runP7Mle("var", argv, session, &raw);
  // Keep the whole payload so `vargranger` and `irf create` can re-render it
  // without re-fitting — Stata's own model too is fit once and re-read.
  session.lastVar = raw;
  return block;
}

ojson runVargranger(SessionState& session) {
  if (!session.lastVar.has_value() || !(*session.lastVar).contains("granger"))
    return engineErrorBlockM4("vargranger: no VAR results in memory; run `var` first.",
                              "unsupported_option");
  const ojson& raw = *session.lastVar;
  ojson rows = ojson::array();
  for (const auto& g : raw["granger"]) {
    ojson r;
    r["equation"] = g.value("equation", "");
    r["excluded"] = g.value("excluded", "");
    r["chi2"] = g.contains("chi2") ? g["chi2"] : ojson(nullptr);
    r["df"] = g.contains("df") ? g["df"] : ojson(nullptr);
    r["p"] = g.contains("p") ? g["p"] : ojson(nullptr);
    rows.push_back(r);
  }
  ojson b;
  b["type"] = "vargranger";
  b["title"] = "Granger causality Wald tests";
  b["rows"] = rows;
  return b;
}

// `irf create NAME, step(#) set("PATH", replace)` — materializes the stored IRF
// cells as a Stata `.irf` file, which is an ordinary `.dta`.
ojson runIrf(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.varlist.empty() || lowerM4(trimM4(parsed.varlist[0])) != "create")
    return engineErrorBlockM4(
        "`irf` subcommands other than `create` are valid Stata commands that this version of PIET "
        "does not implement.",
        "unsupported_command");
  if (!session.lastVar.has_value() || !(*session.lastVar).contains("irf"))
    return engineErrorBlockM4("irf create: no VAR results in memory; run `var` first.",
                              "unsupported_option");
  const std::string irfname = parsed.varlist.size() > 1 ? parsed.varlist[1] : "irf1";
  std::string setPath;
  long steps = 8;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "set" && !v.isBool) {
      setPath = trimM4(v.strValue);
      // `set("path", replace)` — strip the option word and the quotes.
      size_t comma = setPath.find(',');
      if (comma != std::string::npos) setPath = trimM4(setPath.substr(0, comma));
      if (setPath.size() >= 2 && setPath.front() == '"' && setPath.back() == '"')
        setPath = setPath.substr(1, setPath.size() - 2);
    } else if (k == "step" && !v.isBool) {
      steps = std::strtol(trimM4(v.strValue).c_str(), nullptr, 10);
    }
  }
  if (setPath.empty())
    return engineErrorBlockM4("syntax_error: irf create requires set(\"filename\")",
                              "syntax_error");
  if (setPath.size() < 4 || setPath.substr(setPath.size() - 4) != ".irf") setPath += ".irf";

  // Build the cell table as CSV, convert it, and export it as the .dta the .irf
  // format actually is. PIET has no dataset writer of its own, so this reuses the
  // engine verbs rather than growing one.
  const ojson& cells = (*session.lastVar)["irf"]["cells"];
  static const char* kNumCols[] = {"irf",     "cirf",     "oirf",   "coirf",  "sirf",
                                   "dm",      "cdm",      "stdirf", "stdcirf", "stdoirf",
                                   "stdcoirf", "stdsirf", "stddm",  "stdcdm", "fevd",
                                   "sfevd",   "mse",      "stdfevd", "stdsfevd"};
  std::ostringstream csv;
  csv << "irfname,step";
  for (const char* c : kNumCols) csv << "," << c;
  csv << ",response,impulse\n";
  int64_t n_written = 0;
  for (const auto& c : cells) {
    if (c.value("step", 0) > steps) continue;
    csv << irfname << "," << c.value("step", 0);
    for (const char* k : kNumCols) {
      csv << ",";
      if (c.contains(k) && !c[k].is_null()) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", c[k].get<double>());
        csv << buf;
      }
    }
    csv << "," << c.value("response", std::string()) << "," << c.value("impulse", std::string())
        << "\n";
    ++n_written;
  }
  const std::string csvPath = nextPathM4(session.workdir(), "irf_cells") + ".csv";
  {
    std::ofstream f(csvPath);
    if (!f) return engineErrorBlockM4("irf create: could not write the intermediate table",
                                      "data_io");
    f << csv.str();
  }
  const std::string hbinPath = nextPathM4(session.workdir(), "irf_frame");
  CobraRunResult cv = runCobraOneShot({"convert", "--data", csvPath, "--out", hbinPath,
                                       "--factor", "irfname,response,impulse"});
  if (!cv.ok)
    return engineErrorBlockM4("irf create: could not build the IRF table (" +
                                  (cv.stderr_text.empty() ? cv.error : cv.stderr_text) + ")",
                              "engine_error");
  CobraRunResult ex = runCobraOneShot({"export-dta", "--data", hbinPath, "--out", setPath});
  if (!ex.ok)
    return engineErrorBlockM4("irf create: could not write " + setPath + " (" +
                                  (ex.stderr_text.empty() ? ex.error : ex.stderr_text) + ")",
                              "engine_error");
  ojson b;
  b["type"] = "irf";
  b["subcommand"] = "create";
  b["irfname"] = irfname;
  b["file"] = setPath;
  b["n_rows"] = n_written;
  b["steps"] = steps;
  b["notes"] = ojson::array({"(file " + setPath + " created)", "(file " + setPath + " now active)",
                             "(file " + setPath + " updated)"});
  return b;
}

// ===========================================================================
// P7 — multiple imputation: `mi set`, `mi register`, `mi impute regress`
//
// The mlong ROWS are real data: the engine writes the whole 4,800 + M×129 frame
// itself (`--out-data`), because it already holds both the source frame and the
// draws — reassembling them here would mean round-tripping the values back
// through a CSV merge, which is more moving parts for the same result and a
// second place for the row order to drift. SessionState keeps the declaration
// only, so `mi impute` can reject honestly on un-set or unregistered data.

ojson runMi(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.varlist.empty())
    return engineErrorBlockM4("syntax_error: mi requires a subcommand", "syntax_error");
  const std::string sub = lowerM4(trimM4(parsed.varlist[0]));

  if (sub == "set") {
    if (parsed.varlist.size() < 2)
      return engineErrorBlockM4("syntax_error: mi set requires a style", "syntax_error");
    const std::string style = lowerM4(trimM4(parsed.varlist[1]));
    if (style != "mlong")
      return engineErrorBlockM4(
          "`mi set " + style + "` is a valid Stata style that this version of PIET does not "
          "implement (only `mlong` is available).",
          "unsupported_command");
    SessionState::MiState st;
    st.style = "mlong";
    session.miState = st;
    ojson b;
    b["type"] = "mi";
    b["subcommand"] = "set";
    b["style"] = "mlong";
    return b;
  }

  if (sub == "register") {
    if (!session.miState.has_value())
      return engineErrorBlockM4("mi register: the data are not mi set; run `mi set mlong` first.",
                                "unsupported_option");
    if (parsed.varlist.size() < 3)
      return engineErrorBlockM4("syntax_error: mi register requires a class and a varlist",
                                "syntax_error");
    const std::string cls = lowerM4(trimM4(parsed.varlist[1]));
    if (cls != "imputed")
      return engineErrorBlockM4(
          "`mi register " + cls + "` is a valid Stata class that this version of PIET does not "
          "implement (only `imputed` is available).",
          "unsupported_command");
    if (!session.currentHbin().has_value()) return eval::noDataLoaded();
    std::vector<std::string> vars(parsed.varlist.begin() + 2, parsed.varlist.end());
    for (const auto& v : vars) session.miState->imputed.push_back(v);
    // Count the rows this marks incomplete — the number Stata prints. Uses the
    // ordinary count path so there is no second missing-value rule here.
    // `eval --summarize` reports N = the NON-missing count per variable, plus the
    // dataset's own row count; the incomplete count Stata prints is the
    // difference. Derived rather than assumed — reading a nonexistent
    // "n_missing" key silently produced 0 here, which is the shape of a count
    // that looks reported but never was.
    int64_t marked = 0;
    for (const auto& v : vars) {
      std::vector<std::string> argv = {"eval", "--data", *session.currentHbin(), "--summarize", v};
      try {
        ojson raw = runCobraJsonM4(argv);
        if (!raw.contains("n_rows") || !raw.contains("summarize") ||
            !raw["summarize"].contains(v) || !raw["summarize"][v].contains("N"))
          return engineErrorBlockM4(
              "mi register: could not determine how many observations of `" + v + "` are missing",
              "engine_error");
        marked += raw["n_rows"].get<int64_t>() - raw["summarize"][v]["N"].get<int64_t>();
      } catch (const CobraError& ce) {
        return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
      }
    }
    ojson b;
    b["type"] = "mi";
    b["subcommand"] = "register";
    b["class"] = "imputed";
    b["vars"] = vars;
    b["n_marked_incomplete"] = marked;
    return b;
  }

  if (sub == "impute") {
    if (!session.currentHbin().has_value()) return eval::noDataLoaded();
    if (!session.miState.has_value())
      return engineErrorBlockM4("mi impute: the data are not mi set; run `mi set mlong` first.",
                                "unsupported_option");
    if (parsed.varlist.size() < 2)
      return engineErrorBlockM4("syntax_error: mi impute requires a method", "syntax_error");
    const std::string method = lowerM4(trimM4(parsed.varlist[1]));
    if (method != "regress")
      return engineErrorBlockM4(
          "`mi impute " + method + "` is a valid Stata method that this version of PIET does not "
          "implement (only `regress` is available).",
          "unsupported_command");
    // `mi impute regress Y = X1 X2` — the tokenizer leaves `=` its own token.
    std::vector<std::string> rest(parsed.varlist.begin() + 2, parsed.varlist.end());
    size_t eq = rest.size();
    for (size_t i = 0; i < rest.size(); ++i)
      if (rest[i] == "=") { eq = i; break; }
    if (eq == rest.size() || eq == 0 || eq + 1 >= rest.size())
      return engineErrorBlockM4("syntax_error: mi impute regress requires `Y = X1 X2`",
                                "syntax_error");
    if (eq != 1)
      return engineErrorBlockM4(
          "`mi impute regress` with more than one imputed variable is a valid Stata form that this "
          "version of PIET does not implement.",
          "unsupported_command");
    const std::string yv = rest[0];
    const std::vector<std::string> xv(rest.begin() + static_cast<long>(eq) + 1, rest.end());
    const auto& reg = session.miState->imputed;
    if (std::find(reg.begin(), reg.end(), yv) == reg.end())
      return engineErrorBlockM4(
          "mi impute regress: `" + yv + "` is not registered as imputed; run "
          "`mi register imputed " + yv + "` first.",
          "unsupported_option");

    std::string add = "5", seed;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "add" && !v.isBool) add = trimM4(v.strValue);
      else if (k == "rseed" && !v.isBool) seed = trimM4(v.strValue);
      else if (k == "update" && !v.isBool)
        return engineErrorBlockM4(
            "Option `update()` is a valid Stata option that this version of PIET does not "
            "implement for `mi impute` (only `add()` is available).",
            "unsupported_option");
    }
    std::string outPath = nextPathM4(session.workdir(), "mi_mlong");
    std::vector<std::string> argv = {"mi-impute-regress", "--data", *session.currentHbin(),
                                     "--y", yv, "--x", joinCommaM4(xv), "--add", add,
                                     "--out-data", outPath};
    if (!seed.empty()) { argv.push_back("--seed"); argv.push_back(seed); }
    // ONE-SHOT deliberately, not the session-routed runner: --out-data writes a
    // dataset, and under the resident-frame serve path commit_frame STAGES the
    // frame instead of writing the file — so the file PIET then loads would never
    // exist. (`--out-data` is not in that path's --data/--out guard, so it took
    // the staging branch silently; measured here as "cannot open <mi_mlong_...>").
    CobraRunResult cr = runCobraOneShot(argv);
    if (!cr.ok) {
      // Prefer the engine's own error envelope when it produced one; fall back to
      // stderr so a launch failure is never reported as an empty message.
      std::string msg, code = "engine_error";
      try {
        ojson e = ojson::parse(cr.stdout_text);
        if (e.contains("error")) {
          msg = e["error"].value("message", std::string());
          code = e["error"].value("code", std::string("engine_error"));
        }
      } catch (const std::exception&) {
      }
      if (msg.empty()) msg = cr.stderr_text.empty() ? cr.error : cr.stderr_text;
      return engineErrorBlockM4("mi impute regress: " + msg, code);
    }
    ojson raw;
    try {
      raw = ojson::parse(cr.stdout_text);
    } catch (const std::exception&) {
      return engineErrorBlockM4("mi impute regress: could not parse the engine response",
                                "engine_error");
    }
    session.load(outPath);
    session.miState->m += static_cast<int>(std::stol(add));
    ojson block = raw;
    block["type"] = "mi";
    block["subcommand"] = "impute";
    block["method"] = "regress";
    return block;
  }

  return engineErrorBlockM4(
      "`mi " + sub + "` is a valid Stata subcommand that this version of PIET does not implement "
      "(set, register and impute regress are available).",
      "unsupported_command");
}

// ===========================================================================
// P7 — dynamic panel: `xtabond depvar indepvars, lags(#) vce(robust)`
//
// Needs the xtset panel context, which the engine takes as `--xtset id,t` (the
// same channel xtreg uses). Only the one-step estimator exists; two-step and the
// conventional (non-robust) GMM VCE reject rather than quietly returning
// one-step numbers under another name.

ojson runXtabond(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // BUG-657: Stata's `_xt, trequired` refusals (the texts of _xt.ado's
  // time-required branch, both r(459)); was unsupported_option = r(198).
  if (!session.panelvar.has_value() || session.panelvar->empty())
    return ojson(eval::errorBlock("engine_error", "must specify panelvar and timevar; use xtset",
                                  std::nullopt, std::nullopt, std::nullopt, "r(459)"));
  if (!session.timevar.has_value() || session.timevar->empty())
    return ojson(eval::errorBlock("engine_error", "must specify timevar; use xtset", std::nullopt,
                                  std::nullopt, std::nullopt, "r(459)"));
  if (parsed.varlist.size() < 2)
    return engineErrorBlockM4(
        "syntax_error: xtabond requires a dependent variable and at least one regressor",
        "syntax_error");
  std::vector<std::string> argv = {
      "xtabond", "--y", parsed.varlist[0], "--x",
      joinCommaM4(std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end())),
      "--xtset", *session.panelvar + "," + *session.timevar};
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  std::string lags = "1";
  for (const auto& [k, v] : parsed.opts) {
    if (k == "lags" && !v.isBool) lags = trimM4(v.strValue);
    else if (k == "twostep" && v.isBool && v.boolValue)
      return engineErrorBlockM4(
          "Option `twostep` is a valid Stata option that this version of PIET does not implement "
          "for `xtabond` (only the one-step estimator is available).",
          "unsupported_option");
  }
  argv.push_back("--lags");
  argv.push_back(lags);
  // BUG-383 #5: the engine now implements BOTH one-step VCEs, and Stata's bare
  // `xtabond` is the CONVENTIONAL one — the old code here hardcoded --vce
  // robust for every call, which kept the bare form red against the capture
  // even after the engine grew the right default. Forward what the user asked:
  // nothing -> engine default (conventional); vce(robust) -> robust.
  std::string vceReq;  // empty = let the engine default (conventional) stand
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !v.isBool) {
      std::string lv = lowerM4(trimM4(v.strValue));
      if (lv == "robust") {
        vceReq = "robust";
        continue;
      }
      if (lv != "robust")
        return engineErrorBlockM4(
            "Option `vce(" + lv + ")` is a valid Stata option that this version of PIET does not "
            "implement for `xtabond` (conventional [the default] and vce(robust) are available).",
            "unsupported_option");
    }
  }
  if (!vceReq.empty()) {
    argv.push_back("--vce");
    argv.push_back(vceReq);
  }
  return runP7Mle("xtabond", argv, session);
}

// ===========================================================================
// P7 — nonlinear: `nl (y = EXPR)` and `gmm (RESID), instruments(...) onestep`
//
// The parenthesised expression is passed to the engine VERBATIM. PIET does not
// parse it: the engine owns the one expression evaluator (promoted out of
// nlcom), and a second parse here is exactly the shadow-evaluator shape that
// makes two layers disagree quietly. Macro expansion has already happened
// upstream, which is correct — Stata expands macros inside nl expressions too.

ojson runNlGmm(const std::string& cobraCmd, const std::string& rawLine,
               const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // Take the expression straight off the raw line: the tokenizer splits on
  // whitespace, so rejoining varlist tokens would silently normalise the
  // spacing inside the user's own formula.
  std::string s = trimM4(rawLine);
  size_t lp = s.find('(');
  if (lp == std::string::npos)
    return engineErrorBlockM4("syntax_error: " + cobraCmd + " requires a parenthesised expression",
                              "syntax_error");
  int depth = 0;
  size_t rp = std::string::npos;
  for (size_t i = lp; i < s.size(); ++i) {
    if (s[i] == '(') ++depth;
    else if (s[i] == ')' && --depth == 0) { rp = i; break; }
  }
  if (rp == std::string::npos)
    return engineErrorBlockM4("syntax_error: " + cobraCmd + " expression has no closing ')'",
                              "syntax_error");
  std::vector<std::string> argv = {cobraCmd, "--expr", s.substr(lp, rp - lp + 1)};
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  if (cobraCmd == "gmm") {
    std::string instruments;
    bool onestep = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "instruments" && !v.isBool) {
        std::istringstream iss(v.strValue);
        std::vector<std::string> toks;
        std::string t;
        while (iss >> t) toks.push_back(t);
        instruments = joinCommaM4(toks);
      }
      else if (k == "onestep" && v.isBool && v.boolValue) onestep = true;
      else if (k == "twostep" && v.isBool && v.boolValue)
        return engineErrorBlockM4(
            "Option `twostep` is a valid Stata option that this version of PIET does not implement "
            "for `gmm` (only the one-step estimator is available).",
            "unsupported_option");
    }
    if (instruments.empty())
      return engineErrorBlockM4("syntax_error: gmm requires instruments()", "syntax_error");
    argv.push_back("--instruments");
    argv.push_back(instruments);
    if (!onestep)
      return engineErrorBlockM4(
          "`gmm` without `onestep` (two-step GMM) is a valid Stata command that this version of "
          "PIET does not implement.",
          "unsupported_command");
    argv.push_back("--onestep");
  }
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "robust") && cobraCmd == "nl")
      return engineErrorBlockM4(
          "Option `vce()` is a valid Stata option that this version of PIET does not implement for "
          "`nl` (the small-sample factor is not fixed by any oracle available here).",
          "unsupported_option");
  }
  pushLevelIfGivenM4(parsed, argv);
  return runP7Mle(cobraCmd, argv, session);
}

// ===========================================================================
// P7 — multilevel: `mixed depvar indepvars || GROUP:, vce(robust)`
//
// Only the random-INTERCEPT form is implemented. A random-slope list after the
// colon, or a second `||` level, is a different model and rejects rather than
// being silently fitted as an intercept-only one.

ojson runMixed(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // splitWs leaves `||` and `id:` as ordinary tokens.
  size_t bar = parsed.varlist.size();
  for (size_t i = 0; i < parsed.varlist.size(); ++i)
    if (parsed.varlist[i] == "||") { bar = i; break; }
  if (bar == parsed.varlist.size())
    return engineErrorBlockM4(
        "syntax_error: mixed requires a random-effects part, e.g. `mixed y x1 x2 || id:`",
        "syntax_error");
  if (bar < 2)
    return engineErrorBlockM4(
        "syntax_error: mixed requires a dependent variable and at least one regressor before `||`",
        "syntax_error");
  std::vector<std::string> after(parsed.varlist.begin() + static_cast<long>(bar) + 1,
                                 parsed.varlist.end());
  if (after.empty())
    return engineErrorBlockM4("syntax_error: mixed requires a grouping variable after `||`",
                              "syntax_error");
  for (const auto& t : after)
    if (t == "||")
      return engineErrorBlockM4(
          "`mixed` with more than one random-effects level is a valid Stata model that this "
          "version of PIET does not implement (only a single random intercept is available).",
          "unsupported_command");
  std::string grp = after[0];
  bool sawColon = false;
  if (!grp.empty() && grp.back() == ':') { grp.pop_back(); sawColon = true; }
  if (!sawColon)
    return engineErrorBlockM4("syntax_error: mixed's grouping variable must be followed by `:`",
                              "syntax_error");
  if (after.size() > 1)
    return engineErrorBlockM4(
        "`mixed` with random slopes after `|| " + grp + ":` is a valid Stata model that this "
        "version of PIET does not implement (only a random intercept is available).",
        "unsupported_command");

  eval::ParsedCommand fixedPart = parsed;
  fixedPart.varlist.assign(parsed.varlist.begin(), parsed.varlist.begin() + static_cast<long>(bar));
  P7MleArgv b = buildP7MleArgv("mixed", fixedPart, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  for (const auto& [k, v] : parsed.opts)
    if (k == "reml" && v.isBool && v.boolValue)
      return engineErrorBlockM4(
          "Option `reml` is a valid Stata option that this version of PIET does not implement for "
          "`mixed` (the estimator fits by maximum likelihood).",
          "unsupported_option");
  b.argv.push_back("--group");
  b.argv.push_back(grp);
  pushLevelIfGivenM4(parsed, b.argv);
  ojson raw;
  ojson block = runP7Mle("mixed", b.argv, session, &raw);
  // The random-effects stanza (var(_cons), var(Residual)) is a table Stata always
  // prints below the coefficients; like the ordered models' cutpoints it is not a
  // slope, so the mapper does not carry it and it is appended from the raw
  // payload. z/p stay null — Stata leaves those columns blank for variances.
  if (raw.contains("random_effects") && raw["random_effects"].is_array()) {
    if (!block.contains("coeftable") || !block["coeftable"].is_array())
      block["coeftable"] = ojson::array();
    for (const auto& re : raw["random_effects"]) {
      ojson row;
      row["name"] = re.value("name", "");
      row["b"] = re.contains("b") ? re["b"] : ojson(nullptr);
      row["se"] = re.contains("se") ? re["se"] : ojson(nullptr);
      row["t"] = nullptr;
      row["p"] = nullptr;
      row["ci_lo"] = re.contains("ci_lo") ? re["ci_lo"] : ojson(nullptr);
      row["ci_hi"] = re.contains("ci_hi") ? re["ci_hi"] : ojson(nullptr);
      block["coeftable"].push_back(row);
    }
  }
  return block;
}

// ===========================================================================
// P7 — survey design: `svyset` and the `svy:` prefix
//
// The design is SESSION state (as it is in Stata), so the engine stays
// stateless and each `svy:` command is rewritten into the ordinary estimation
// call plus --svy --psu --strata and the design's pweight.
//
// The inner-command whitelist fails CLOSED. An unrecognised inner command must
// reject, never fall through to a plain unweighted dispatch — that would return
// a confident number computed under no survey design at all, which is precisely
// the silent-wrong shape this codebase refuses to ship.

ojson runSvyset(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.varlist.empty() && !parsed.weightVar.has_value()) {
    // Bare `svyset` re-displays the current design (Stata's own behaviour).
    if (!session.svyDesign.has_value())
      return engineErrorBlockM4("svyset: no survey design is currently set.", "unsupported_option");
  }
  for (const auto& [k, v] : parsed.opts) {
    if (k == "clear" && v.isBool && v.boolValue) {
      session.svyDesign.reset();
      ojson b;
      b["type"] = "svyset";
      b["cleared"] = true;
      return b;
    }
  }
  if (!parsed.varlist.empty() || parsed.weightVar.has_value()) {
    if (parsed.varlist.size() != 1)
      return engineErrorBlockM4("syntax_error: svyset takes exactly one sampling-unit variable",
                                "syntax_error");
    if (!parsed.weightVar.has_value())
      return engineErrorBlockM4(
          "svyset: this version of PIET requires a probability weight, e.g. "
          "`svyset PSU [pweight=W], strata(S)`.",
          "unsupported_option");
    std::string wt = parsed.weightType.value_or("pweight");
    if (wt != "pweight" && wt != "pw")
      return engineErrorBlockM4(
          "svyset: only [pweight=] is supported for a survey design (got [" + wt + "=]).",
          "unsupported_option");
    std::string strata;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "strata" && !v.isBool) strata = trimM4(v.strValue);
      else if ((k == "fpc" || k == "vce" || k == "singleunit" || k == "poststrata") && !v.isBool)
        return engineErrorBlockM4(
            "Option `" + k + "()` is a valid Stata option that this version of PIET does not "
            "implement for `svyset`.",
            "unsupported_option");
    }
    if (strata.empty())
      return engineErrorBlockM4(
          "svyset: this version of PIET requires strata(), e.g. "
          "`svyset PSU [pweight=W], strata(S)`.",
          "unsupported_option");
    SessionState::SvySettings d;
    d.psu = parsed.varlist[0];
    d.pweight = *parsed.weightVar;
    d.strata = strata;
    session.svyDesign = d;
  }
  const auto& d = *session.svyDesign;
  ojson b;
  b["type"] = "svyset";
  b["pweight"] = d.pweight;
  b["vce"] = "linearized";
  b["strata1"] = d.strata;
  b["su1"] = d.psu;
  b["fpc1"] = nullptr;
  return b;
}

// `svy: CMD ...` — rewrite into the ordinary engine call plus the design flags.
ojson runSvyPrefix(const std::string& innerRaw, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (!session.svyDesign.has_value())
    return engineErrorBlockM4(
        "svy: the data have not been `svyset`; run `svyset PSU [pweight=W], strata(S)` first.",
        "unsupported_option");
  const auto& d = *session.svyDesign;

  eval::ParsedCommand p;
  try {
    p = eval::tokenize(innerRaw);
  } catch (const std::exception&) {
    return engineErrorBlockM4("syntax_error: could not parse the command after `svy:`",
                              "syntax_error");
  }
  const std::string inner = lowerM4(trimM4(p.cmd));
  if (p.weightVar.has_value())
    return engineErrorBlockM4(
        "svy: weights may not be given on the command line — the design's [pweight=] applies.",
        "unsupported_option");

  std::vector<std::string> argv;
  if (inner == "regress" || inner == "reg" || inner == "r" || inner == "feols") {
    if (p.varlist.size() < 2)
      return engineErrorBlockM4("syntax_error: svy: regress requires a dependent variable and at "
                                "least one regressor", "syntax_error");
    argv = {"feols", "--y", p.varlist[0], "--x",
            joinCommaM4(std::vector<std::string>(p.varlist.begin() + 1, p.varlist.end()))};
  } else if (inner == "mean") {
    if (p.varlist.empty())
      return engineErrorBlockM4("syntax_error: svy: mean requires at least one variable",
                                "syntax_error");
    argv = {"mean", "--vars", joinCommaM4(p.varlist)};
  } else if (inner == "logit" || inner == "logistic") {
    if (p.varlist.size() < 2)
      return engineErrorBlockM4("syntax_error: svy: logit requires a dependent variable and at "
                                "least one regressor", "syntax_error");
    argv = {"logit", "--y", p.varlist[0], "--x",
            joinCommaM4(std::vector<std::string>(p.varlist.begin() + 1, p.varlist.end()))};
  } else {
    return engineErrorBlockM4(
        "`svy: " + p.cmd + "` is a valid Stata construction that this version of PIET does not "
        "implement (the survey prefix covers regress, mean and logit).",
        "unsupported_command");
  }
  if (p.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*p.ifExpr);
  }
  argv.push_back("--weight");
  argv.push_back(d.pweight);
  argv.push_back("--wtype");
  argv.push_back("pw");
  argv.push_back("--svy");
  argv.push_back("--psu");
  argv.push_back(d.psu);
  argv.push_back("--strata");
  argv.push_back(d.strata);
  return runP7Mle(argv[0], argv, session);
}

// ===========================================================================
// P7 — survival: stset / stcox / streg
//
// `stset` is an ENGINE verb, not PIET state: it materializes `_t0/_t/_d/_st` as
// real columns, which is data computation, and every later st command then reads
// them as ordinary variables — the same arrangement Stata has. What PIET keeps is
// the DECLARATION (session.stContext), which is what lets stcox/streg reject
// honestly on un-stset data and lets `vce(robust)` cluster on the subject id.

ojson runStset(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() != 1)
    return engineErrorBlockM4("syntax_error: stset requires exactly one time variable",
                              "syntax_error");
  std::string failvar, idvar;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "failure" && !v.isBool) {
      // Stata allows `failure(fail)`, `failure(fail==1)` and `failure(fail!=0)`;
      // only the bare-variable form is implemented, and the comparison forms
      // reject rather than being silently read as the bare one (they select a
      // DIFFERENT failure event).
      std::string fv = trimM4(v.strValue);
      if (fv.find_first_of("=<>!") != std::string::npos)
        return engineErrorBlockM4(
            "Option `failure(" + fv + ")` is a valid Stata option that this version of PIET does "
            "not implement for `stset` — only the bare `failure(varname)` form is available.",
            "unsupported_option");
      failvar = fv;
    } else if (k == "id" && !v.isBool) {
      idvar = trimM4(v.strValue);
    }
  }
  if (failvar.empty())
    return engineErrorBlockM4("syntax_error: stset requires failure(varname)", "syntax_error");

  std::vector<std::string> argv = {"stset", "--data", *session.currentHbin(), "--time",
                                   parsed.varlist[0], "--failure", failvar};
  if (!idvar.empty()) {
    argv.push_back("--id");
    argv.push_back(idvar);
  }
  std::string outPath = nextPathM4(session.workdir(), "stset");
  argv.push_back("--out");
  argv.push_back(outPath);

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  session.load(outPath);
  SessionState::StContext ctx;
  ctx.timevar = parsed.varlist[0];
  ctx.failvar = failvar;
  ctx.idvar = idvar;
  session.stContext = ctx;

  ojson block = raw;
  block["type"] = "stset";
  return block;
}

namespace {

// Shared entry checks + argv prefix for stcox/streg.
P7MleArgv buildStArgv(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                      SessionState& session) {
  P7MleArgv r;
  if (!session.stContext.has_value()) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(
        cobraCmd + ": the data have not been `stset`; run `stset TIMEVAR, failure(FAILVAR)` first.",
        "unsupported_option");
    return r;
  }
  if (parsed.varlist.empty()) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4("syntax_error: " + cobraCmd + " requires at least one regressor",
                                      "syntax_error");
    return r;
  }
  r.argv = {cobraCmd, "--x", joinCommaM4(parsed.varlist)};
  if (parsed.ifExpr.has_value()) {
    r.argv.push_back("--keepif");
    r.argv.push_back(*parsed.ifExpr);
  }
  // vce. Stata's own `vce(robust)` on multi-record st data with an id() is
  // subject-clustered, so PIET supplies the id here rather than letting the
  // engine compute a record-level sandwich that would miss the oracle on the
  // second digit.
  if (auto shapeErr = vceOptionShapeErrorM4(parsed)) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(*shapeErr, "unsupported_option");
    return r;
  }
  std::optional<std::string> vceVal, clusterOpt;
  bool robustFlag = false;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !vceVal.has_value())
      vceVal = v.isBool ? std::string() : v.strValue;
    else if (k == "robust" && v.isBool && v.boolValue)
      robustFlag = true;
    else if (k == "cluster" && !v.isBool)
      clusterOpt = v.strValue;
  }
  std::string want, cvar = clusterOpt.value_or("");
  if (vceVal.has_value() && !vceVal->empty()) {
    std::string lv = lowerM4(*vceVal);
    if (lv.rfind("cluster", 0) == 0) {
      want = "cluster";
      if (lv.size() > 7) {
        std::string rest = trimM4(lv.substr(7));
        if (!rest.empty()) cvar = rest;
      }
    } else {
      want = lv;
    }
  } else if (!cvar.empty()) {
    // BUG-537: cluster() beside `robust` wins (w2_pweight.log R5), as in
    // vcovFromOpts / buildP7MleArgv.
    want = "cluster";
  } else if (robustFlag) {
    want = "robust";
  }
  if (want.empty()) return r;
  if (want != "robust" && want != "cluster" && want != "iid") {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(
        "Option `vce(" + want + ")` is a valid Stata option that this version of PIET does not "
        "implement for `" + cobraCmd + "`.", "unsupported_option");
    return r;
  }
  if (want == "cluster" && cvar.empty()) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4("Cluster variable required: vce(cluster varname)",
                                      "unsupported_option");
    return r;
  }
  r.argv.push_back("--vce");
  r.argv.push_back(want);
  if (want == "cluster") {
    r.argv.push_back("--cluster");
    r.argv.push_back(cvar);
  } else if (want == "robust" && !session.stContext->idvar.empty()) {
    r.argv.push_back("--cluster");
    r.argv.push_back(session.stContext->idvar);
  }
  return r;
}

}  // namespace

namespace {

// Stata's stcox/streg print HAZARD RATIOS by default, with the z, p and CI still
// taken from the RAW coefficient scale. The engine keeps `b`/`se` in the raw
// metric like every other estimator and carries `hr`/`se_hr`/`hr_ci_lo`/
// `hr_ci_hi` alongside — which is right for the contract, but the shared
// tidy_coeftable only copies the seven standard fields, so the rendered table
// showed the raw coefficient where Stata shows the ratio. This re-points the
// table at the hazard-ratio columns (from the RAW payload, since the mapper does
// not carry them through) and keeps the raw coefficient available under `b_raw`.
void applyHazardRatioTableM4(ojson& block, const ojson& raw) {
  if (!raw.contains("coefs") || !raw["coefs"].is_array()) return;
  if (!block.contains("coeftable") || !block["coeftable"].is_array()) return;
  std::map<std::string, const ojson*> byName;
  for (const auto& c : raw["coefs"])
    if (c.contains("name") && c.contains("hr")) byName[c["name"].get<std::string>()] = &c;
  for (auto& row : block["coeftable"]) {
    auto it = byName.find(row.value("name", std::string()));
    if (it == byName.end()) continue;
    const ojson& c = *it->second;
    row["b_raw"] = row.contains("b") ? row["b"] : ojson(nullptr);
    row["se_raw"] = row.contains("se") ? row["se"] : ojson(nullptr);
    row["b"] = c["hr"];
    if (c.contains("se_hr")) row["se"] = c["se_hr"];
    if (c.contains("hr_ci_lo")) row["ci_lo"] = c["hr_ci_lo"];
    if (c.contains("hr_ci_hi")) row["ci_hi"] = c["hr_ci_hi"];
  }
  block["coef_metric"] = "hazard_ratio";
}

}  // namespace

ojson runStcox(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildStArgv("stcox", parsed, session);
  if (b.errored) return b.errorBlock;
  ojson raw;
  ojson block = runP7Mle("stcox", b.argv, session, &raw);
  applyHazardRatioTableM4(block, raw);
  return block;
}

ojson runStreg(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildStArgv("streg", parsed, session);
  if (b.errored) return b.errorBlock;
  std::string dist;
  for (const auto& [k, v] : parsed.opts)
    if ((k == "distribution" || k == "dist" || k == "d") && !v.isBool) dist = lowerM4(trimM4(v.strValue));
  if (dist.empty())
    return engineErrorBlockM4("syntax_error: streg requires distribution()", "syntax_error");
  if (dist != "weibull")
    return engineErrorBlockM4(
        "Option `distribution(" + dist + ")` is a valid Stata option that this version of PIET does "
        "not implement for `streg` — only `distribution(weibull)` is available.",
        "unsupported_option");
  b.argv.push_back("--dist");
  b.argv.push_back("weibull");
  ojson raw;
  ojson block = runP7Mle("streg", b.argv, session, &raw);
  applyHazardRatioTableM4(block, raw);
  // Stata prints /ln_p, p and 1/p in a stanza below the coefficients; like the
  // ordered models' cutpoints, they are not slopes and the mapper drops them, so
  // they are appended here from the raw payload.
  for (const char* key : {"ln_p", "p", "inv_p"}) {
    if (!raw.contains(key) || !raw[key].is_object()) continue;
    const ojson& s = raw[key];
    ojson row;
    row["name"] = std::string(key) == "ln_p" ? "/ln_p" : (std::string(key) == "p" ? "p" : "1/p");
    row["b"] = s.contains("b") ? s["b"] : ojson(nullptr);
    row["se"] = s.contains("se") ? s["se"] : ojson(nullptr);
    row["t"] = s.contains("z") ? s["z"] : ojson(nullptr);
    row["p"] = s.contains("p") ? s["p"] : ojson(nullptr);
    row["ci_lo"] = s.contains("ci_lo") ? s["ci_lo"] : ojson(nullptr);
    row["ci_hi"] = s.contains("ci_hi") ? s["ci_hi"] : ojson(nullptr);
    if (!block.contains("coeftable") || !block["coeftable"].is_array())
      block["coeftable"] = ojson::array();
    block["coeftable"].push_back(row);
  }
  return block;
}

// ===========================================================================
// logit / probit

ojson runLogitProbit(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                      SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  EstimationArgvResult built = buildEstimationArgvM4(cobraCmd, parsed, session);
  if (built.errored) return built.errorBlock;
  std::vector<std::string> cobraArgs = built.argv;
  cobraArgs.push_back("--data");
  cobraArgs.push_back(*session.currentHbin());
  cobraArgs.push_back("--tidy");
  ojson raw;
  try {
    raw = runCobraJsonM4(cobraArgs);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson block = cobraToEstimationM4(raw, cobraCmd, &session);
  // W:P8: cobra's logit/probit raw JSON carries no "depvar" field at all
  // (live-verified: poisson/tobit/feols all do, these two are the only
  // estimators PIET drives that omit it), so the block published an EMPTY
  // depvar. Two user-visible consequences, both rc 0: `estimates dir` listed
  // the model as "p8_logit  ()", and `estimates table` — whose equation
  // grouping is keyed on exactly this name in Stata (logit's e(b) columns are
  // `ybin:x1`, regress's are bare `x1`) — had nothing to label the group with.
  // The name is the user's own `logit YVAR ...` token echoed back, not an
  // engine-computed quantity, so filling it here is the same honest move the
  // tobit/heckman paths in this file already make for the same engine gap.
  if (block.value("depvar", std::string()).empty() && !parsed.varlist.empty()) {
    block["depvar"] = parsed.varlist[0];
  }
  fillCoeftableStatsM4(block);
  // e(k), the number of parameters (= columns of e(b)): w2b_estim.log:706
  // probit_r_nocons e(k) 2. The engine's logit/probit reply carries no `k`, and
  // an e() name with no key map entry resolves from the cached reply's key of
  // the same name (macros.cpp resolveEScalar), so it is posted on that reply
  // only -- the e(df_a) pattern (handlers.cpp absorbedDofA).
  if (!raw.contains("k") && raw.contains("coefs") && raw["coefs"].is_array()) {
    raw["k"] = raw["coefs"].size();
  }
  cacheLastModelM4(session, cobraCmd, cobraArgs, block, raw);
  // FU-30: e(vce) after a default-VCE logit/probit is `oim` -- both are ML
  // estimators, and Stata's ML default VCE is the observed information matrix
  // (tests/eval/golden/aer_w2/w2c_ml_getmata.log:321 ml_pw_vceoim and :356
  // ml_opts_nospace `oim`, the same probit likelihood fitted through `ml`; the
  // engine's default probit SE is the observed-information one already, BUG-140).
  // setLastModel() published lm.vcov's engine spelling `iid`, which the replay
  // machinery keys on, so only the macro store is corrected -- the regress `ols`
  // / xtreg,fe `conventional` pattern. vce(robust)/vce(cluster) keep `robust`/
  // `cluster` with e(vcetype) `Robust` (w2b_estim.log:619 logit_vce_robust).
  if (session.lastModel().has_value() && session.lastModel()->vcov == "iid") {
    session.eval.setEString("vce", "oim");
  }
  return block;
}

// ===========================================================================
// csdid / drdid + estat simple/event/calendar/group/pretrend


}  // namespace piet::handlers
