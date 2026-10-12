// R2 (giant-file split, 2026-09-01): continuation TU of estimation_m4.cpp —
// a MOVE-ONLY cut at pre-existing section banners. Preamble = the original
// file head verbatim minus its option-vocab anon block (single registration
// stays in the first TU); shared helpers come from estimation_m4_internal.h.
// This TU carries -ffp-contract=off like its parent (CMake) — FMA fusion
// here would drift golden numerics.
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

#include <algorithm>
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
#include "piet/display_hardening.h"
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

#include "estimation_m4_internal.h"

// R2: the option-vocab bindings, restored VERBATIM — registering the same
// token twice UNIONS the sets (errors.h, deliberate), and a set unioned with
// itself is itself, so the single-registry law holds across the three TUs.
namespace {
const piet::eval::OptVocab& kVocabTtest = piet::eval::registerOptVocab(
    {"ttest"}, {{"level", "by", "unequal", "welch", "paired", "reverse", "unpaired"},
     {}});
// FU-23 (BUG-757): `vce` and `robust` are accepted and IGNORED by csdid itself,
// whatever their argument: w2l_vce_shape.log csdid_ctl_vcecluster,
// csdid_ctl_vcerobust, csdid_vce_c2, csdid_vce_cluster_novar, csdid_robust_val
// and csdid_vce_robust_x are all rc 0 with the default fit's se11 2.931073697886207.
// Registering the token unions with the two other TUs' copies (see above), so
// adding them here is enough for the dispatcher and for runCsdid's net.
const piet::eval::OptVocab& kVocabCsdid = piet::eval::registerOptVocab(
    {"csdid"}, {{"ivar", "time", "gvar", "method", "comparison", "pscoretrim", "window", "cluster", "long2",
      "vce", "robust"},
     {}});
const piet::eval::OptVocab& kVocabReg3 = piet::eval::registerOptVocab(
    {"reg3"}, {{"sure", "3sls"},
     {}});
// FU-23 (BUG-758): `nograph` suppresses boottest's confidence-curve plot; PIET's
// boottest draws none, so the option has nothing to change. Every boottest arm
// of w2l_vce_shape.log passes it at rc 0, and this net refused it r(198).
const piet::eval::OptVocab& kVocabBoottest = piet::eval::registerOptVocab(
    {"boottest"}, {{"reps", "seed", "weights", "null",
                                                          "cluster", "nograph"},
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
// w2b-iv-options: `first` (w2b_estim.log:879 ivregress_first, :1064
// ivreghdfe_first, both rc 0). Registered on its own rather than edited into
// the verbatim copy above: the registry UNIONS per token, so this lands in the
// same `reghdfe` slot kVocabIvRegress names, which is runIvRegress's net (and,
// for canonicalisation only, the non-IV reghdfe word, whose own hand-written
// net in runRegress still refuses `first`).
const piet::eval::OptVocab& kVocabIvFirst = piet::eval::registerOptVocab(
    {"ivregress", "ivreg", "ivreghdfe", "reghdfe"}, {{"first"}, {}});
// w2b-iv-options: ppmlhdfe's `d` / `d(newvar)` (w2b_estim.log:1305
// ppmlhdfe_d, :1336 ppmlhdfe_absorb_d_named). ppmlhdfe's slot only: plain
// `poisson` has no such option.
const piet::eval::OptVocab& kVocabPpmlD = piet::eval::registerOptVocab(
    {"ppmlhdfe"}, {{"d"}, {}});
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
}  // namespace

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;

namespace {

ojson coefRowM4(const std::string& name, const ojson& att, const ojson& se) {
  ojson r;
  r["name"] = name;
  if (att.is_null() || se.is_null()) {
    r["b"] = att;
    r["se"] = se;
    r["t"] = nullptr;
    r["p"] = nullptr;
    r["ci_lo"] = nullptr;
    r["ci_hi"] = nullptr;
    return r;
  }
  try {
    double attF = att.get<double>();
    double seF = se.get<double>();
    double z = (seF != 0.0) ? attF / seF : std::nan("");
    r["b"] = attF;
    r["se"] = seF;
    r["t"] = z;
    r["p"] = seF != 0.0 ? ojson(normalPvalue(z)) : ojson(nullptr);
    r["ci_lo"] = attF - kZ975 * seF;
    r["ci_hi"] = attF + kZ975 * seF;
  } catch (...) {
    r["b"] = att;
    r["se"] = se;
    r["t"] = nullptr;
    r["p"] = nullptr;
    r["ci_lo"] = nullptr;
    r["ci_hi"] = nullptr;
  }
  return r;
}

// _csdid_to_estimation (dispatcher.py) — normalise a csdid result JSON into
// an estimation block for the requested aggregate.
// csdidToEstimationM4: hoisted below the anonymous block (R2) — TU c consumes it.

void cacheCsdidModel(SessionState& session, const std::vector<std::string>& argv, const ojson& raw) {
  auto optFor = [&](const std::string& flag) -> std::optional<std::string> {
    for (size_t i = 0; i < argv.size(); ++i) {
      if (argv[i] == flag && i + 1 < argv.size()) return argv[i + 1];
    }
    return std::nullopt;
  };
  LastModel lm;
  lm.cobraCmd = "csdid";
  lm.y = optFor("--y").value_or("");
  std::string xStr = optFor("--x").value_or("");
  if (!xStr.empty()) {
    std::string cur;
    for (char c : xStr) {
      if (c == ',') {
        if (!cur.empty()) lm.x.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) lm.x.push_back(cur);
  }
  lm.depvar = lm.y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = session.currentHbin().value_or("");
  lm.ivar = optFor("--ivar");
  lm.timevar = optFor("--time");
  lm.gvar = optFor("--gvar");
  lm.method = optFor("--method");
  lm.comparison = optFor("--comparison");
  lm.pscoretrim = optFor("--pscoretrim");
  auto clusterVar = optFor("--cluster");
  if (clusterVar.has_value()) lm.cluster = {*clusterVar};
  lm.result = raw;
  session.setLastModel(lm);
}

// FU-23 (BUG-756): the SHAPE of the VCE options, read once for the four
// handlers below (csdid, ivprobit/ivtobit, boottest, xtpoisson/xtlogit ,fe).
// Each of them used to read `cluster` only when it carried a value
// (`k == "cluster" && !v.isBool`), so a bare `cluster` never reached their
// checks: xtpoisson/xtlogit ,fe and ivprobit/ivtobit fitted the iid model at
// rc 0, and boottest ran with the fit's clusters. A valued `robust(x)` and a
// trailing token in `vce(robust x)` were skipped the same way. What each
// command does with each shape differs and is the oracle's to say
// (tests/eval/golden/aer_w2/w2l_vce_shape.log), so this only classifies; the
// handlers map a shape to Stata's answer.
enum class OptShapeM4b { kAbsent, kBare, kEmpty, kOne, kMany };

struct VceShapesM4b {
  OptShapeM4b cluster = OptShapeM4b::kAbsent;
  std::string clusterRaw;  // trimmed cluster(...) text
  std::vector<std::string> clusterVars;
  OptShapeM4b robust = OptShapeM4b::kAbsent;  // kBare = the flag; kOne/kMany = robust(x)
  std::string robustRaw;
  bool sawVce = false;                    // vce()/vcov(), first one wins (as every loop here)
  std::string vceRaw;                     // trimmed
  std::string vceType;                    // canonicalVceType(); "" when unrecognised
  std::vector<std::string> vceRest;       // the tokens after the type
};

OptShapeM4b shapeOfM4b(const eval::OptValue& v, std::vector<std::string>* toks) {
  if (v.isBool) return OptShapeM4b::kBare;
  *toks = splitTokensM4(v.strValue);
  if (toks->empty()) return OptShapeM4b::kEmpty;
  return toks->size() == 1 ? OptShapeM4b::kOne : OptShapeM4b::kMany;
}

VceShapesM4b readVceShapesM4b(const eval::ParsedCommand& parsed) {
  VceShapesM4b s;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "cluster") {
      s.cluster = shapeOfM4b(v, &s.clusterVars);
      s.clusterRaw = v.isBool ? std::string() : trimM4(v.strValue);
    } else if (k == "robust") {
      std::vector<std::string> toks;
      s.robust = shapeOfM4b(v, &toks);
      s.robustRaw = v.isBool ? std::string() : trimM4(v.strValue);
    } else if ((k == "vce" || k == "vcov") && !s.sawVce) {
      s.sawVce = true;
      s.vceRaw = v.isBool ? std::string() : trimM4(v.strValue);
      std::string rest;
      s.vceType = canonicalVceType(s.vceRaw, &rest);
      s.vceRest = splitTokensM4(rest);
    }
  }
  return s;
}

// A refusal Stata gives itself, carried with Stata's own rc (coarseRc reads the
// `r(NNN)` reason first) and the oracle's wording.
ojson stataRefusalM4b(const std::string& message, int rc) {
  return ojson(eval::errorBlock("syntax_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

}  // namespace

// R2: hoisted from the anonymous namespace above so estimation_m4c.cpp can link it.
ojson csdidToEstimationM4(const ojson& raw, const std::string& subcmd) {
  ojson block;
  std::string estimator = raw.value("estimator", std::string("csdid"));
  block["type"] = "estimation";
  block["estimator"] = estimator;
  block["estat"] = subcmd;
  block["depvar"] = raw.value("depvar", "");
  block["n_obs"] = raw.contains("n_obs") ? raw["n_obs"] : raw.value("n_units", ojson(0));
  if (raw.contains("method") && !raw["method"].is_null() &&
      !(raw["method"].is_string() && raw["method"].get<std::string>().empty())) {
    block["method"] = raw["method"];
  }
  if (raw.contains("comparison") && !raw["comparison"].is_null() &&
      !(raw["comparison"].is_string() && raw["comparison"].get<std::string>().empty())) {
    block["comparison"] = raw["comparison"];
  }

  if (subcmd == "simple") {
    ojson att = raw.contains("simple_att") ? raw["simple_att"] : ojson(nullptr);
    ojson se = raw.contains("simple_se") ? raw["simple_se"] : ojson(nullptr);
    block["coeftable"] = ojson::array({coefRowM4("ATT", att, se)});
  } else if (subcmd == "event") {
    ojson rows = ojson::array();
    ojson eventStudy = ojson::array();
    for (const auto& ev : raw.value("events", ojson::array())) {
      ojson eVal = ev.value("e", ojson(nullptr));
      std::string name;
      if (eVal.is_number()) {
        double d = eVal.get<double>();
        if (d == std::floor(d)) {
          name = std::to_string(static_cast<long long>(d));
        } else {
          std::ostringstream oss;
          oss << d;
          name = oss.str();
        }
      } else if (eVal.is_string()) {
        name = eVal.get<std::string>();
      }
      ojson row = coefRowM4(name, ev.value("att", ojson(nullptr)), ev.value("se", ojson(nullptr)));
      rows.push_back(row);
      ojson es;
      es["e"] = name;
      es["att"] = row["b"];
      es["se"] = row["se"];
      es["ci_lo"] = row["ci_lo"];
      es["ci_hi"] = row["ci_hi"];
      eventStudy.push_back(es);
    }
    block["coeftable"] = rows;
    block["event_study"] = eventStudy;
    if (raw.contains("window") && !raw["window"].is_null() &&
        !(raw["window"].is_array() && raw["window"].empty())) {
      block["window"] = raw["window"];
    }
    if (raw.contains("pre_avg") && !raw["pre_avg"].is_null()) {
      block["pre_avg"] = raw["pre_avg"];
      block["pre_avg_se"] = raw.contains("pre_avg_se") ? raw["pre_avg_se"] : ojson(nullptr);
      block["post_avg"] = raw.contains("post_avg") ? raw["post_avg"] : ojson(nullptr);
      block["post_avg_se"] = raw.contains("post_avg_se") ? raw["post_avg_se"] : ojson(nullptr);
    }
  } else if (subcmd == "calendar" || subcmd == "group") {
    // calendar: ATT per outcome period, keyed "t". group: ATT per treatment
    // cohort, keyed "g" (distinct aggregations — the tables must differ).
    const char* arrKey = (subcmd == "group") ? "groups" : "calendar";
    const char* entKey = (subcmd == "group") ? "g" : "t";
    ojson rows = ojson::array();
    for (const auto& ent : raw.value(arrKey, ojson::array())) {
      ojson tVal = ent.value(entKey, ojson(nullptr));
      std::string name;
      if (tVal.is_number()) {
        double d = tVal.get<double>();
        if (d == std::floor(d)) {
          name = std::to_string(static_cast<long long>(d));
        } else {
          std::ostringstream oss;
          oss << d;
          name = oss.str();
        }
      } else if (tVal.is_string()) {
        name = tVal.get<std::string>();
      }
      rows.push_back(coefRowM4(name, ent.value("att", ojson(nullptr)), ent.value("se", ojson(nullptr))));
    }
    block["coeftable"] = rows;
  } else if (subcmd == "pretrend") {
    ojson pt = raw.value("pretrend", ojson::object());
    ojson pretrend;
    pretrend["chi2"] = pt.contains("chi2") ? pt["chi2"] : ojson(nullptr);
    pretrend["df"] = pt.contains("df") ? pt["df"] : ojson(nullptr);
    pretrend["p"] = pt.contains("p") ? pt["p"] : ojson(nullptr);
    block["pretrend"] = pretrend;
    block["coeftable"] = ojson::array();
  } else {
    block["coeftable"] = ojson::array();
  }
  return block;
}

ojson runCsdid(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.empty()) {
    return engineErrorBlockM4("syntax_error: csdid requires a depvar", "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::optional<std::string> ivar, time, gvar, method, comparison, pscoretrim, window;
  bool long2 = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "ivar" && !v.isBool) ivar = v.strValue;
    if (k == "time" && !v.isBool) time = v.strValue;
    if (k == "gvar" && !v.isBool) gvar = v.strValue;
    if (k == "method" && !v.isBool) method = v.strValue;
    if (k == "comparison" && !v.isBool) comparison = v.strValue;
    if (k == "pscoretrim" && !v.isBool) pscoretrim = v.strValue;
    if (k == "window" && !v.isBool) window = v.strValue;
    // csdid `long2` — Callaway-Sant'Anna long-difference (fixed g-1 base)
    // event-study parametrization. Bare flag; pass through to the engine.
    if (k == "long2") long2 = true;
  }
  // FU-23 (BUG-757): cluster() by shape, w2l_vce_shape.log. A bare `cluster`
  // and an empty `cluster()` are ignored (csdid_bare_cluster/_empty_cluster rc 0,
  // the default fit); two variables are r(103) "cluster():  too many variables
  // specified" (csdid_opt_c2), where this loop used to forward the FIRST one.
  // vce()/robust are ignored by csdid whatever they say (the vocabulary note at
  // the top of the file), so they are read nowhere.
  const VceShapesM4b vceShape = readVceShapesM4b(parsed);
  std::optional<std::string> cluster;
  if (vceShape.cluster == OptShapeM4b::kMany) {
    return stataRefusalM4b("cluster():  too many variables specified", 103);
  }
  if (vceShape.cluster == OptShapeM4b::kOne) cluster = vceShape.clusterVars[0];
  if (!ivar.has_value() || ivar->empty()) {
    return engineErrorBlockM4(
        "csdid requires `ivar(varname)` — e.g. `csdid y x, ivar(id) time(t) gvar(g) method(dripw)`.",
        "unsupported_option");
  }
  if (!time.has_value() || time->empty()) {
    return engineErrorBlockM4(
        "csdid requires `time(varname)` — e.g. `csdid y x, ivar(id) time(t) gvar(g) method(dripw)`.",
        "unsupported_option");
  }
  if (!gvar.has_value() || gvar->empty()) {
    return engineErrorBlockM4(
        "csdid requires `gvar(varname)` — e.g. `csdid y x, ivar(id) time(t) gvar(g) method(dripw)`.",
        "unsupported_option");
  }

  std::vector<std::string> argv = {"csdid", "--y", y, "--ivar", *ivar, "--time", *time, "--gvar", *gvar};
  if (!xs.empty()) {
    argv.push_back("--x");
    argv.push_back(joinCommaM4(xs));
  }
  if (method.has_value() && !method->empty()) {
    argv.push_back("--method");
    argv.push_back(*method);
  }
  if (comparison.has_value() && !comparison->empty()) {
    argv.push_back("--comparison");
    argv.push_back(*comparison);
  }
  if (pscoretrim.has_value() && !pscoretrim->empty()) {
    argv.push_back("--pscoretrim");
    argv.push_back(*pscoretrim);
  }
  if (window.has_value() && !window->empty()) {
    std::istringstream iss(*window);
    std::vector<std::string> parts;
    std::string tok;
    while (iss >> tok) parts.push_back(tok);
    std::string joined;
    for (size_t i = 0; i < parts.size(); ++i) {
      if (i) joined += ",";
      joined += parts[i];
    }
    argv.push_back("--window");
    argv.push_back(joined.empty() ? *window : joined);
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  if (cluster.has_value()) {
    // csdid_ctl_cluster: `cluster(id)` with `ivar(id)` is r(198) "id may not be
    // both target and by()". The engine refuses the same literal name match
    // (apps/cmds/cmds_panel.cpp, "name the same variable"), but its error left
    // here as rc 1; the rule is on the name token, so it is checked here.
    if (*cluster == trimM4(*ivar)) {
      return stataRefusalM4b(*cluster + " may not be both target and by()", 198);
    }
    argv.push_back("--cluster");
    argv.push_back(*cluster);
  }
  if (long2) argv.push_back("--long2");

  // Audit fix: every neighboring estimator in this file (ivregress/reg3/
  // etc.) closes its option parsing with eval::rejectUnknownOptions —
  // csdid never did, so a typo (e.g. `methdo(dripw)` instead of
  // `method(dripw)`) silently fell through the opts loop above (none of
  // its `if (k == ...)` arms match, so the mistyped key is simply never
  // read) instead of erroring, producing a plausible-looking but WRONG
  // result (a different, unintended specification) rather than naming the
  // bad option.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kCsdidKnownOpts = kVocabCsdid.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kCsdidKnownOpts)) {
    return *err;
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    // csdid_ctl_cluster_id2: a cluster variable that varies within a panel is
    // r(498) "panels are not nested within clusters". The engine detects it
    // (apps/cmds/cmds_panel.cpp, "is not nested within --ivar") but its
    // bad_input code has no rc of its own, so it became rc 1. The engine's
    // message is the only signal this side receives; it is matched on the
    // clause that names the rule.
    if (cluster.has_value() &&
        std::string(ce.what()).find("is not nested within --ivar") != std::string::npos) {
      return stataRefusalM4b("panels are not nested within clusters", 498);
    }
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson block = csdidToEstimationM4(raw, "simple");
  session.lastEstBlock = block;
  cacheCsdidModel(session, argv, raw);
  return block;
}

// ===========================================================================
// ivregress / ivreghdfe / reg3 (BETA D1 batch: BS-063/BS-064/BS-065)

namespace est_detail {  // R2: reopened — splitTokensM4/parseAtNumlistM4 cross to TU c

// Splits a plain string on whitespace AND commas — the same token grammar
// the tokenizer's own splitWs already applied to ParsedCommand::varlist
// (each `(endog = instruments)`/reg3-equation substring below is a plain
// std::string this file reconstructs BY HAND from already-split varlist
// tokens, joined back with single spaces — see parseIvParenM4's doc comment
// — so it needs the identical space-or-comma splitter applied a second
// time, not a paren-aware re-tokenize).
std::vector<std::string> splitTokensM4(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == ',') {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string joinSpaceM4(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += " ";
    s += v[i];
  }
  return s;
}

// P7: Stata's `at()` value-list grammar, `at(v1=(a b c) v2=#)`. The pre-P7 code
// split at() on whitespace and commas, which turns `treated=(0 1)` into the two
// nonsense fragments `treated=(0` and `1)`. Returns ONE engine `--at` spec per
// point in the cartesian product, each spelled `v=val[,v2=val2]`.
std::vector<std::string> parseAtNumlistM4(const std::string& at) {
  struct Entry {
    std::string var;
    std::vector<std::string> vals;
  };
  std::vector<Entry> entries;
  size_t i = 0;
  const size_t n = at.size();
  while (i < n) {
    while (i < n && (std::isspace(static_cast<unsigned char>(at[i])) || at[i] == ',')) ++i;
    if (i >= n) break;
    size_t eq = at.find('=', i);
    if (eq == std::string::npos) break;
    Entry e;
    e.var = trimM4(at.substr(i, eq - i));
    size_t j = eq + 1;
    while (j < n && std::isspace(static_cast<unsigned char>(at[j]))) ++j;
    if (j < n && at[j] == '(') {
      size_t close = at.find(')', j);
      if (close == std::string::npos) close = n;
      for (const auto& t : splitTokensM4(at.substr(j + 1, close - j - 1)))
        if (!t.empty() && t != ",") e.vals.push_back(t);
      i = close < n ? close + 1 : n;
    } else {
      size_t k = j;
      while (k < n && !std::isspace(static_cast<unsigned char>(at[k])) && at[k] != ',') ++k;
      e.vals.push_back(at.substr(j, k - j));
      i = k;
    }
    if (!e.var.empty() && !e.vals.empty()) entries.push_back(e);
  }
  std::vector<std::string> out;
  if (entries.empty()) return out;
  std::vector<size_t> idx(entries.size(), 0);
  while (true) {
    std::string spec;
    for (size_t e = 0; e < entries.size(); ++e) {
      if (!spec.empty()) spec += ",";
      spec += entries[e].var + "=" + entries[e].vals[idx[e]];
    }
    out.push_back(spec);
    size_t pos = entries.size();
    while (pos > 0) {
      --pos;
      if (++idx[pos] < entries[pos].vals.size()) break;
      idx[pos] = 0;
      if (pos == 0) return out;
    }
  }
}

// BS-063: `ivregress 2sls|gmm|liml depvar [exog...] (endog = instruments)` /
// `ivreghdfe depvar [exog...] (endog = instruments), absorb(F)` / the
// `reghdfe depvar (endog=instruments), absorb(F)` paren form all share this
// ONE varlist shape: [depvar, exog...]  "(" endog "=" instruments ")"
// [exog...] (the trailing list only where exogAfterParen says so).
// splitWs() (eval/tokenizer.cpp) is whitespace-only, so it already broke
// "(beertax = spirits unemp)" into SEPARATE varlist tokens ("(beertax", "=",
// "spirits", "unemp)") — this rejoins them with single spaces (Stata's own
// whitespace inside the parens carries no meaning) and re-splits on the
// paren/equals structure instead.
struct IvParenResult {
  std::string y;
  std::vector<std::string> exog;
  std::vector<std::string> endog;
  std::vector<std::string> instruments;
  bool errored = false;
  std::string errorMsg;
};

IvParenResult parseIvParenM4(const std::vector<std::string>& tokens, bool exogAfterParen) {
  IvParenResult r;
  std::string joined = joinSpaceM4(tokens);
  size_t lp = joined.find('(');
  if (lp == std::string::npos) {
    r.errored = true;
    r.errorMsg = "requires an endogenous-regressors block: (endog = instruments)";
    return r;
  }
  size_t rpLast = joined.rfind(')');
  if (rpLast == std::string::npos || rpLast < lp) {
    r.errored = true;
    r.errorMsg = "malformed (endog = instruments) block: missing closing ')'";
    return r;
  }
  std::string beforeStr = joined.substr(0, lp);
  std::string insideStr = joined.substr(lp + 1, rpLast - lp - 1);
  std::string afterStr = joined.substr(rpLast + 1);
  // Only ONE parenthesised block is valid ivregress/ivreghdfe syntax — a
  // stray extra '('/')' anywhere else (before/inside/after) means either a
  // second block or an unbalanced paren; reject rather than silently
  // mis-slicing the wrong substring as the endog/instrument list.
  if (beforeStr.find(')') != std::string::npos || insideStr.find('(') != std::string::npos ||
      insideStr.find(')') != std::string::npos || afterStr.find('(') != std::string::npos ||
      afterStr.find(')') != std::string::npos) {
    r.errored = true;
    r.errorMsg = "malformed syntax: only one (endog = instruments) block is supported";
    return r;
  }
  std::vector<std::string> before = splitTokensM4(beforeStr);
  std::vector<std::string> after = splitTokensM4(afterStr);
  if (before.empty()) {
    r.errored = true;
    r.errorMsg = "requires a dependent variable before the (endog = instruments) block";
    return r;
  }
  if (!after.empty() && !exogAfterParen) {
    r.errored = true;
    r.errorMsg = "unexpected tokens after the (endog = instruments) block: " + joinSpaceM4(after);
    return r;
  }
  size_t eq = insideStr.find('=');
  if (eq == std::string::npos) {
    r.errored = true;
    r.errorMsg = "malformed (endog = instruments) block: missing '='";
    return r;
  }
  if (insideStr.find('=', eq + 1) != std::string::npos) {
    r.errored = true;
    r.errorMsg = "malformed (endog = instruments) block: only one '=' is allowed";
    return r;
  }
  std::vector<std::string> endog = splitTokensM4(insideStr.substr(0, eq));
  std::vector<std::string> instr = splitTokensM4(insideStr.substr(eq + 1));
  if (endog.empty() || instr.empty()) {
    r.errored = true;
    r.errorMsg = "malformed (endog = instruments) block: need at least one endogenous regressor "
                 "and at least one instrument";
    return r;
  }
  r.y = before[0];
  r.exog.assign(before.begin() + 1, before.end());
  // Exogenous regressors typed AFTER the block join the ones typed before it,
  // in typed order: w2b_estim.log:923-945 `ivregress 2sls y (x1 = z1) x2` fits
  // the `y x2 (x1 = z1)` model (b_x1 1.940390224456596 both ways, "Exogenous:
  // x2 z1"), and :1185/:1243 do the same for ivreghdfe.
  r.exog.insert(r.exog.end(), after.begin(), after.end());
  r.endog = endog;
  r.instruments = instr;
  return r;
}

}  // namespace est_detail

namespace {

// ── `first` / ivreg2 `partial()` helpers (task w2b-iv-options) ──────────────

// Stata's coefficient-table name column: 12 wide, right-aligned, long names
// abbreviated with `~` the way the regress family does.
std::string coefStubM4b(const std::string& name) {
  std::string n = (name == "(intercept)") ? std::string("_cons") : name;
  if (n.size() > 12) n = n.substr(0, 10) + "~" + n.substr(n.size() - 1);
  return std::string(12 - n.size(), ' ') + n;
}

std::string padLeftM4b(const std::string& s, size_t w) {
  return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
}

double numberAtM4b(const ojson& obj, const char* key, const std::string& name) {
  if (!obj.is_object() || !obj.contains(key) || !obj[key].is_object()) return std::nan("");
  const ojson& m = obj[key];
  if (!m.contains(name) || !m[name].is_number()) return std::nan("");
  return m[name].get<double>();
}

// One first stage as Stata's 78-column table, from the engine's own
// `--savefirst` estimate (apps/cmds/common.cpp add_first_stage: b, se, t, p and
// the CI off df_r). Only what that estimate carries is printed: the first-stage
// F, R-squared and adj. R-squared of ivregress's header are not in it, so they
// are left out rather than filled in. `names` is the row order.
std::string firstStageTextM4b(const ojson& fs, const std::vector<std::string>& names,
                              const std::string& vcov, const std::string& clustvar) {
  const std::string depvar = fs.value("depvar", std::string());
  std::string out = "first-stage regression of " + depvar + " (option first)\n";
  char buf[160];
  if (fs.contains("n_obs") && fs["n_obs"].is_number()) {
    std::snprintf(buf, sizeof(buf), "%56s%-14s= %6lld\n", "", "Number of obs",
                  static_cast<long long>(fs["n_obs"].get<double>()));
    out += buf;
  }
  if (fs.contains("rmse") && fs["rmse"].is_number()) {
    std::snprintf(buf, sizeof(buf), "%56s%-14s= %6.4f\n", "", "Root MSE", fs["rmse"].get<double>());
    out += buf;
  }
  out += "\n";
  const bool robust = vcov != "iid" && !vcov.empty();
  if (vcov == "cluster" && fs.contains("n_clusters") && fs["n_clusters"].is_number()) {
    const std::string adj = "(Std. err. adjusted for " +
                            std::to_string(static_cast<long long>(fs["n_clusters"].get<double>())) +
                            " clusters in " + clustvar + ")";
    out += padLeftM4b(adj, 78) + "\n";
  }
  out += std::string(78, '-') + "\n";
  if (robust) out += "             |               Robust\n";
  out += coefStubM4b(depvar) + " | Coefficient  " + (robust ? "std. err." : "Std. err.") +
         "      t    P>|t|     [95% conf. interval]\n";
  out += std::string(13, '-') + "+" + std::string(64, '-') + "\n";
  for (const auto& name : names) {
    const double b = numberAtM4b(fs, "coef", name);
    const double se = numberAtM4b(fs, "se", name);
    const double t = numberAtM4b(fs, "tstat", name);
    const double p = numberAtM4b(fs, "pvalue", name);
    const double lo = numberAtM4b(fs, "ci_lo", name);
    const double hi = numberAtM4b(fs, "ci_hi", name);
    std::string row = coefStubM4b(name) + " |";
    if (!std::isfinite(b) || !std::isfinite(se)) {
      row += padLeftM4b("(omitted)", 11);
    } else {
      row += padLeftM4b(fmtStataDefaultG(b), 11) + padLeftM4b(fmtStataDefaultG(se), 11);
      std::snprintf(buf, sizeof(buf), "%9.2f%8.3f", t, p);
      row += buf;
      row += padLeftM4b(fmtStataDefaultG(lo), 13) + padLeftM4b(fmtStataDefaultG(hi), 12);
    }
    out += row + "\n";
  }
  out += std::string(78, '-');
  return out;
}

// Drops `drop` from every per-coefficient map of an iv reply and from its
// coeftable. Used by ivreg2's partial(): by Frisch-Waugh-Lovell the remaining
// coefficients and their standard errors are the full fit's (w2b_estim.log:1013
// ivreg2_partial .1611032742759026 against :1060 ivreg2_ctl .1611032742759016),
// and ivreg2 counts the partialled-out variables in K for every small-sample
// adjustment (its own footnote, :1004-1006), so the full fit's dof are already
// the right ones.
void dropCoefsM4b(ojson& r, const std::set<std::string>& drop) {
  for (const char* key : {"coef", "se", "tstat", "pvalue", "ci_lo", "ci_hi"}) {
    if (!r.contains(key) || !r[key].is_object()) continue;
    for (const auto& name : drop) r[key].erase(name);
  }
  if (r.contains("coeftable") && r["coeftable"].is_array()) {
    ojson kept = ojson::array();
    for (const auto& row : r["coeftable"]) {
      if (row.is_object() && drop.count(row.value("name", std::string()))) continue;
      kept.push_back(row);
    }
    r["coeftable"] = kept;
  }
}

}  // namespace

ojson runIvRegress(const std::string& method, const eval::ParsedCommand& parsed, bool allowAbsorb,
                    const std::string& stataCmd, SessionState& session, bool smallDefault, bool forceSmall) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  IvParenResult iv = parseIvParenM4(parsed.varlist, /*exogAfterParen=*/true);
  if (iv.errored) {
    return engineErrorBlockM4("syntax_error: `" + stataCmd + "` " + iv.errorMsg, "syntax_error");
  }

  bool isLiml = (method == "liml");
  std::string cobraCmd = isLiml ? "liml" : "iv";

  // `first` (w2b_estim.log:879 ivregress_first, :1064 ivreghdfe_first):
  // each first stage is displayed, the fit itself is unchanged (both arms'
  // _b/_se are the option-free fits' -- :919 against ivreg2_ctl :1060, :1185
  // against ivreghdfe_ctl :1301). The stages come from the
  // engine's `--savefirst` estimate: `regress ENDOG EXCLUDED_IVS EXOG` under the
  // parent's vce, small-sample statistics always (cmd_iv's usage text; the
  // FE-absorbed stage reproduces :1078-1079, z1 .2657743 / .1072108). gmm and
  // liml are refused: their first-stage tables are not measured, and liml's
  // engine command has no --savefirst at all.
  bool wantFirst = false;
  // ivreg2's partial(varlist) (w2b_estim.log:969-1013 ivreg2_partial):
  // Frisch-Waugh-Lovell -- see dropCoefsM4b. ivreg2/xtivreg2 only: ivregress and
  // ivreghdfe keep refusing the key through the net below.
  const bool partialAllowed = (stataCmd == "ivreg2" || stataCmd == "xtivreg2");
  bool wantPartial = false;
  std::vector<std::string> partialVars;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "first") {
      if (!v.isBool) return stataRefusalM4b("option first() not allowed", 198);
      wantFirst = true;
    } else if (k == "partial" && partialAllowed) {
      if (v.isBool || splitTokensM4(v.strValue).empty()) {
        return stataRefusalM4b("option partial() requires a varlist", 198);
      }
      wantPartial = true;
      for (const auto& t : splitTokensM4(v.strValue)) partialVars.push_back(t);
    }
  }
  if (wantFirst && method != "2sls") {
    return engineErrorBlockM4(
        "Option `first` is a valid Stata option that this version of PIET implements for the "
        "2SLS estimator only (`" + stataCmd + "` with " + method + " has no measured "
        "first-stage table).",
        "unsupported_option");
  }
  if (wantPartial) {
    // The partialled-out variables must be included exogenous regressors typed
    // as plain names (`_cons` alone partials the constant only). A factor or
    // operator token has no single coefficient to drop, so it is refused rather
    // than guessed at.
    for (const auto& pv : partialVars) {
      if (pv == "_cons") continue;
      if (pv.find_first_of(".#()") != std::string::npos) {
        return engineErrorBlockM4(
            "partial(" + pv + "): this version of PIET partials out plain exogenous "
            "variables only (no factor-variable or time-series operators).",
            "unsupported_option");
      }
      if (std::find(iv.exog.begin(), iv.exog.end(), pv) == iv.exog.end()) {
        return stataRefusalM4b("partial(): " + pv + " is not an included exogenous regressor",
                               198);
      }
    }
    if (method != "2sls") {
      // Efficient two-step GMM re-weights with the partialled-out regressors in
      // the moment conditions; FWL invariance is not measured there.
      return engineErrorBlockM4(
          "Option `partial()` is a valid Stata option that this version of PIET implements for "
          "the 2SLS estimator only.",
          "unsupported_option");
    }
  }

  std::vector<std::string> argv = {cobraCmd,        "--y",      iv.y,
                                    "--endog",       joinCommaM4(iv.endog),
                                    "--instruments", joinCommaM4(iv.instruments)};
  // cobra iv/liml both REJECT an explicit `--x "(intercept)"` placeholder
  // (live-verified: "column not found: (intercept)") — unlike feols/xtreg,
  // which REQUIRE that literal when there are no exogenous controls. Only
  // push --x when there is a genuine exogenous-controls list.
  if (!iv.exog.empty()) {
    argv.push_back("--x");
    argv.push_back(joinCommaM4(iv.exog));
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // --fe via absorb() — ivreghdfe / the reghdfe-paren form ONLY. Real
  // Stata's `ivregress` has no absorb() option of its own at all.
  std::string feRaw;
  bool sawAbsorb = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "absorb") {
      sawAbsorb = true;
      if (!v.isBool) feRaw = v.strValue;
    }
  }
  if (sawAbsorb) {
    if (!allowAbsorb) {
      return engineErrorBlockM4("option `absorb` not allowed", "unsupported_option");
    }
    std::vector<std::string> feToks = splitTokensM4(feRaw);
    if (!feToks.empty()) {
      argv.push_back("--fe");
      argv.push_back(joinCommaM4(feToks));
    }
  }

  // vce()/robust/legacy cluster(). cobra `iv` supports iid/hc1/cluster/
  // twoway (BS-067's splitClusterVarsM4 reused unchanged). cobra `liml` has
  // NO working vcov surface at all — live-verified: `--vcov hc1` is
  // silently ACCEPTED (exit 0, no error) but produces the IDENTICAL
  // classical/asymptotic SE either way, i.e. a true engine no-op rather
  // than a rejection. Forwarding a user's vce()/robust/cluster() to liml
  // would silently claim a robust/clustered SE the engine never actually
  // computed — an honest reject before ever building the argv, matching
  // this batch's "never forward a flag the engine demonstrably ignores"
  // rule (see also noconstant/level below, and the report's cobra_runner.cpp
  // masking-bug finding for the OTHER half of that rule: options the engine
  // hard-errors on but whose stdout gets parsed as a false success anyway).
  bool sawVceRelated = false;
  for (const auto& [k, v] : parsed.opts) {
    (void)v;
    if (k == "vce" || k == "vcov" || k == "robust" || k == "cluster") sawVceRelated = true;
  }
  if (isLiml && sawVceRelated) {
    return engineErrorBlockM4(
        "vce()/robust/cluster() is not supported for `ivregress liml` in this version of PIET — "
        "the engine's liml estimator only ever computes the classical/asymptotic SE, regardless "
        "of what vce() is requested. Use `ivregress 2sls` or `ivregress gmm` for a robust or "
        "clustered VCE.",
        "unsupported_option");
  }
  if (!isLiml) {
    VcovResult vr = vcovFromOpts(parsed);
    if (vr.errored) {
      return engineErrorBlockM4(vr.errorMsg, "unsupported_option");
    }
    for (const auto& a : vr.argv) argv.push_back(a);
  }

  // ivreghdfe (including the `reghdfe ... (endog=instr), absorb()` alias)
  // reports the absorbed/cluster small-sample correction by default. cobra
  // `iv` exposes that convention explicitly as --small; plain ivregress must
  // retain its ASYMPTOTIC default. Until 2026-09-09 that distinction rode on
  // `allowAbsorb`, which made "accepts absorb()" and "reports small-sample SEs"
  // the same bit — so BUG-474's one-line flip moved ivregress's standard errors
  // (D1a/D1f selftest RED). `smallDefault` now carries the spelling's own
  // convention; `forceSmall` is `ivreg2 ..., small` arriving from runIvreg2,
  // which used to parse that flag and throw it away. Either reason is
  // sufficient and the flag is idempotent, so this is an OR.
  if (smallDefault || forceSmall) argv.push_back("--small");
  if (wantFirst) argv.push_back("--savefirst");

  // gmm sub-estimator (Stata `ivregress gmm`): efficient two-step GMM,
  // robust weight matrix + robust SEs by default; combines with --vcov
  // cluster for the cluster-robust GMM weight matrix (cobra iv's own doc).
  if (method == "gmm") argv.push_back("--gmm");

  // keepsingletons (BS-068) — reghdfe's own option name. cobra `iv` DOES
  // expose --keep-singletons (live-verified, despite it not being printed
  // in `cobra iv`'s own usage banner); cobra `liml` has no FE machinery at
  // all (absorb() is already rejected above for it via !allowAbsorb, or
  // simply structurally unreachable for plain ivregress), so
  // keepsingletons is meaningless there — honest reject instead of a
  // silent no-op.
  bool sawKeepSingletons = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "keepsingletons" && v.isBool && v.boolValue) sawKeepSingletons = true;
  }
  if (sawKeepSingletons) {
    if (isLiml) {
      return engineErrorBlockM4("option `keepsingletons` not allowed", "unsupported_option");
    }
    argv.push_back("--keep-singletons");
  }

  // noconstant/level(#) — NEVER forwarded. Live-verified against this exact
  // engine build: `cobra iv --no-intercept` / `--level 90` (and the same
  // for `cobra liml`) are NOT silently no-op'd — they hard-error
  // ("unrecognized option") at cobra's own CLI layer, exit nonzero, YET
  // cobra ALSO already printed a full, valid, WRONG (intercept still
  // included / default 95% CI) result JSON to stdout before that check
  // ran. piet::runCobraJson's BUG-039 fix (cobra_runner.cpp) treats any
  // nonzero exit with parseable stdout as a genuine non-convergence
  // result and returns it as a SUCCESS, discarding the stderr error
  // entirely — so forwarding either option here would silently produce a
  // regression that still has an intercept / ignores the requested CI
  // level while reporting no error at all. This is a real, pre-existing
  // gap in cobra_runner.cpp shared by every estimator that goes through
  // runCobraJson (confirmed also for plain `feols`'s existing noconstant/
  // level handling) — flagged separately (out of this file's edit scope);
  // the safe fix HERE is to never build an argv that can trigger it.
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      return engineErrorBlockM4(
          "Option `" + k + "` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (the engine's IV/LIML estimator does not support "
          "suppressing the constant).",
          "unsupported_option");
    }
    if (k == "level" && !v.isBool) {
      return engineErrorBlockM4(
          "Option `level` is a valid Stata option that this version of PIET does not yet "
          // BUG-286: this used to say the engine "ignores --level", which is a
          // description of a WORSE behaviour than the real one and would send a
          // reader looking for a silent drop. Re-measured 2026-08-08:
          // `cobra iv ... --level 90` answers `unrecognized option '--level' for
          // 'iv'` (bad_input) with an empty result — it has no --level surface at
          // all. The reject PIET gives here is therefore the honest mirror of a
          // missing engine flag, not a guard against a silent drop.
          "implement for `" + stataCmd + "` (the engine's IV/LIML estimator has no --level "
          "surface — it answers `unrecognized option '--level' for 'iv'`).",
          "unsupported_option");
    }
  }

  // kVocabIvFirst is the same registry slot as kVocabIvRegress (see their
  // registrations), so `first` is in this set; partial() joins it for ivreg2.
  std::unordered_set<std::string> known = kVocabIvFirst.known;
  if (partialAllowed) known.insert("partial");
  if (auto err = eval::rejectUnknownOptions(parsed.opts, known)) {
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

  // partial(): drop the partialled-out regressors and the constant from the
  // reported fit (dropCoefsM4b). The model Wald statistic then covers the
  // remaining slopes: with one left it is that coefficient's own z^2 (ivreg2
  // reports F(1,57) 137.81 = 1.94039^2/.1611033^2 * 57/60, :978); with more the
  // joint test needs the covariance the reply does not carry, so it is not
  // reported rather than left describing the full model.
  std::set<std::string> partialled;
  if (wantPartial) {
    for (const auto& pv : partialVars) {
      if (pv == "_cons") continue;
      if (!raw.contains("coef") || !raw["coef"].is_object() || !raw["coef"].contains(pv)) {
        return engineErrorBlockM4("partial(): the fit has no coefficient for `" + pv + "`",
                                  "engine_error");
      }
      partialled.insert(pv);
    }
    partialled.insert("(intercept)");
    dropCoefsM4b(raw, partialled);
    raw.erase("chi2");
    raw.erase("chi2_df");
    if (raw.contains("coeftable") && raw["coeftable"].is_array() &&
        raw["coeftable"].size() == 1 && raw["coeftable"][0].contains("t") &&
        raw["coeftable"][0]["t"].is_number()) {
      const double t = raw["coeftable"][0]["t"].get<double>();
      raw["chi2"] = t * t;
      raw["chi2_df"] = 1;
    }
  }

  // first: one display text per endogenous regressor, built from the engine's
  // stage, then the stage is taken OUT of the cached reply -- `--savefirst`'s
  // first_stage/firsteqs are ivreg2's stored-estimate surface, which `first`
  // does not create (e(firsteqs) stays unset, as after a plain fit).
  ojson firstStages = ojson::array();
  std::vector<std::string> firstTexts;
  if (wantFirst) {
    if (!raw.contains("first_stage") || !raw["first_stage"].is_object()) {
      return engineErrorBlockM4("first: the engine returned no first-stage estimates",
                                "engine_error");
    }
    std::string clustvar;
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
      if (argv[i] == "--cluster") clustvar = argv[i + 1];
    }
    std::string vcovArg = "iid";
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
      if (argv[i] == "--vcov") vcovArg = argv[i + 1];
    }
    const std::set<std::string> instrSet(iv.instruments.begin(), iv.instruments.end());
    for (const auto& endogName : iv.endog) {
      if (!raw["first_stage"].contains(endogName)) continue;
      ojson fs = raw["first_stage"][endogName];
      if (!partialled.empty()) dropCoefsM4b(fs, partialled);
      std::vector<std::string> names;
      for (const auto& n : fs.value("names", ojson::array())) {
        if (n.is_string() && !partialled.count(n.get<std::string>())) {
          names.push_back(n.get<std::string>());
        }
      }
      if (stataCmd == "ivregress") {
        // ivregress lists the stage as exog, excluded instruments, _cons
        // (:894-896 x2 z1 _cons; w2d_uncovered.log:2070-2074 w1 w2 z1 z2 _cons);
        // the engine's order is ivreg2's (excluded instruments first), which is
        // what ivreghdfe prints (:1078-1079 z1 x2).
        std::vector<std::string> ordered;
        for (const auto& n : names)
          if (!instrSet.count(n) && n != "(intercept)") ordered.push_back(n);
        for (const auto& n : names)
          if (instrSet.count(n)) ordered.push_back(n);
        for (const auto& n : names)
          if (n == "(intercept)") ordered.push_back(n);
        names = ordered;
      }
      fs["names"] = names;
      firstTexts.push_back(firstStageTextM4b(fs, names, vcovArg, clustvar));
      firstStages.push_back(fs);
    }
    raw.erase("first_stage");
    raw.erase("firsteqs");
  }

  ojson block = cobraToEstimationM4(raw, cobraCmd, &session);
  // The first stages print ABOVE the IV table, which is where the estimation
  // block's `warnings` render (console.js renderEstimation); the structured
  // stages ride along under `first_stage` for any consumer that reads numbers.
  if (wantFirst || wantPartial) {
    ojson notes = block.contains("warnings") && block["warnings"].is_array() ? block["warnings"]
                                                                             : ojson::array();
    for (const auto& t : firstTexts) notes.push_back(t);
    if (wantPartial) {
      std::string list;
      for (const auto& pv : partialVars)
        if (pv != "_cons") list += pv + " ";
      notes.push_back("Partialled-out: " + list + "_cons (total SS, model F/chi2 and R2s are after "
                      "partialling-out; small-sample adjustments count the partialled-out "
                      "variables in K)");
    }
    if (!notes.empty()) block["warnings"] = notes;
    if (wantFirst) block["first_stage"] = firstStages;
  }
  // Estimator label (task requirement): "ivregress 2sls"/"ivregress gmm"/
  // "ivregress liml"/"ivreghdfe" — independent of cobra's own raw
  // "estimator" field ("iv2sls"/"liml") and independent of whether the
  // ivreghdfe form was typed as `ivreghdfe` or as the `reghdfe (...)` paren
  // form (both compute the identical cobra iv --fe call).
  block["estimator"] = (stataCmd == "ivregress") ? ("ivregress " + method) : std::string("ivreghdfe");

  LastModel lm;
  lm.cobraCmd = cobraCmd;  // "iv" or "liml" — margins' kLinearCmds already
                           // anticipates "iv" (pre-existing) and now "liml"
                           // (this batch's addition, see runMargins above).
  lm.y = iv.y;
  // Stata's own ivregress coefficient table lists ENDOGENOUS regressors
  // first, then exogenous (oracle-verified: D1b's table shows `beertax`
  // before `emppop` even though `emppop` was typed BEFORE the paren block)
  // — matches cobra iv's own coef-dict insertion order too. test/lincom/
  // margins/predict after an IV fit are a KNOWN coarse approximation
  // (cobra's own test/lincom/predict CLIs always re-fit as plain OLS from
  // --y/--x — they have no concept of endog/instruments/gmm at all; this is
  // a pre-existing, disclosed limitation shared by every non-feols
  // estimator already wired into LastModel, not something introduced here).
  lm.x = iv.endog;
  for (const auto& e : iv.exog) lm.x.push_back(e);
  // BETA Phase G (estat endogenous/overid wiring): preserve the role split
  // `lm.x` above discards — see LastModel's own comment (session_state.h).
  lm.ivEndog = iv.endog;
  lm.ivExog = iv.exog;
  lm.ivInstruments = iv.instruments;
  if (sawAbsorb && allowAbsorb) {
    for (const auto& f : splitTokensM4(feRaw)) lm.fe.push_back(f);
  }
  if (!isLiml) {
    for (size_t i = 0; i < argv.size(); ++i) {
      if (argv[i] == "--vcov" && i + 1 < argv.size()) lm.vcov = argv[i + 1];
      if (argv[i] == "--cluster" && i + 1 < argv.size()) {
        for (const auto& c : splitTokensM4(argv[i + 1])) lm.cluster.push_back(c);
      }
    }
  }
  lm.depvar = block.value("depvar", "");
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  // FU-28 (BUG-759): e(vce) after a default-VCE `ivregress 2sls` is
  // `unadjusted` (w2d_uncovered.log:2187 ivregress_first_2endog_ctl, :2323
  // ivregress_factors_ctl, :2403 ivregress_nocons_ctl, :2536
  // ivregress_exog_before_ctl); setLastModel() published lm.vcov's engine
  // spelling `iid`, which the replay machinery keys on, so only the macro store
  // is corrected (regress `ols` and xtreg ,fe `conventional` do the same).
  // robust -> `robust` and cluster -> `cluster` are already Stata's, with
  // e(vcetype) `Robust`; `unadjusted` keeps the empty e(vcetype) setLastModel
  // gave iid. gmm and liml defaults have no captured arm and are left alone.
  if (stataCmd == "ivregress" && method == "2sls" && lm.vcov == "iid") {
    session.eval.setEString("vce", "unadjusted");
  }
  session.lastEstBlock = block;
  return block;
}

// ===========================================================================
// P7 — IV maximum likelihood: ivprobit / ivtobit
//
// `ivprobit ybin x1 (endog=z), vce(robust)` — the same `(endog = instruments)`
// varlist shape ivregress already parses, so parseIvParenM4 is reused rather
// than given a second, subtly different implementation.

ojson runIvMl(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
              SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // ivprobit/ivtobit keep refusing exogenous regressors after the block: no
  // captured arm measures that spelling for them.
  IvParenResult iv = parseIvParenM4(parsed.varlist, /*exogAfterParen=*/false);
  if (iv.errored) return engineErrorBlockM4("syntax_error: " + cobraCmd + " " + iv.errorMsg,
                                            "syntax_error");
  std::vector<std::string> argv = {cobraCmd, "--y", iv.y};
  if (!iv.exog.empty()) {
    argv.push_back("--x");
    argv.push_back(joinCommaM4(iv.exog));
  }
  argv.push_back("--endog");
  argv.push_back(joinCommaM4(iv.endog));
  argv.push_back("--instruments");
  argv.push_back(joinCommaM4(iv.instruments));
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  if (cobraCmd == "ivtobit") {
    bool sawLimit = false;
    for (const auto& [k, v] : parsed.opts) {
      if ((k == "ll" || k == "ul") && !v.isBool) {
        // Stata's bare `ll`/`ul` (censor at the observed extreme) is a different
        // request from `ll(0)`, and the engine has no bare form — rejecting is
        // parity; guessing a limit would answer a different question.
        argv.push_back(k == "ll" ? "--ll" : "--ul");
        argv.push_back(trimM4(v.strValue));
        sawLimit = true;
      } else if ((k == "ll" || k == "ul") && v.isBool) {
        return engineErrorBlockM4(
            "Option `" + k + "` without a value is a valid Stata option that this version of PIET "
            "does not implement for `ivtobit` — give an explicit limit, e.g. ll(0).",
            "unsupported_option");
      }
    }
    if (!sawLimit)
      return engineErrorBlockM4("syntax_error: ivtobit requires at least one of ll()/ul()",
                                "syntax_error");
  }
  // vce
  // FU-23 (BUG-756): ivprobit/ivtobit take the cluster VCE ONLY as
  // vce(cluster var). w2l_vce_shape.log refuses every other spelling r(198):
  // the cluster() option in any shape -- `cluster(id)` (ivprobit_ctl_cluster
  // "option cluster(id) not allowed"), bare `cluster`, `cluster()`,
  // `cluster(id id2)` -- a valued `robust(x1)`, `vce(cluster id id2)`
  // ("multiway cluster VCE not allowed"), `vce(cluster)` ("option vce(cluster
  // clustvar) incorrectly specified") and `vce(robust x1)` ("option vce()
  // incorrectly specified"); ivtobit alike. This loop used to fit a clustered
  // model for cluster(id), skip a bare `cluster` and `robust(x1)` (iid at rc 0),
  // drop the `x1` of vce(robust x1), and hand `id id2` to the engine as ONE
  // column name (rc 1).
  const VceShapesM4b vceShape = readVceShapesM4b(parsed);
  if (vceShape.cluster != OptShapeM4b::kAbsent) {
    return stataRefusalM4b(vceShape.cluster == OptShapeM4b::kBare
                               ? std::string("option cluster not allowed")
                               : "option cluster(" + vceShape.clusterRaw + ") not allowed",
                           198);
  }
  if (vceShape.robust == OptShapeM4b::kEmpty || vceShape.robust == OptShapeM4b::kOne ||
      vceShape.robust == OptShapeM4b::kMany) {
    return stataRefusalM4b("option robust(" + vceShape.robustRaw + ") not allowed", 198);
  }
  if (vceShape.vceType == "cluster" && vceShape.vceRest.size() > 1) {
    return stataRefusalM4b("multiway cluster VCE not allowed", 198);
  }
  if (vceShape.vceType == "cluster" && vceShape.vceRest.empty()) {
    return stataRefusalM4b("option vce(cluster clustvar) incorrectly specified", 198);
  }
  if ((vceShape.vceType == "robust" || vceShape.vceType == "hc1" || vceShape.vceType == "iid") &&
      !vceShape.vceRest.empty()) {
    return stataRefusalM4b("option vce() incorrectly specified", 198);
  }
  std::optional<std::string> vceVal;
  bool robustFlag = false;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !vceVal.has_value())
      vceVal = v.isBool ? std::string() : v.strValue;
    else if (k == "robust" && v.isBool && v.boolValue)
      robustFlag = true;
  }
  std::string want, cvar;
  if (vceVal.has_value() && !vceVal->empty()) {
    // W1b: one canonicaliser (piet/handlers.h), measured abbreviation floors.
    std::string vceRest;
    const std::string lv = canonicalVceType(*vceVal, &vceRest);
    if (lv == "cluster") {
      want = "cluster";
      cvar = vceRest;
    } else {
      want = lv.empty() ? lowerM4(*vceVal) : lv;
    }
  } else if (robustFlag) {
    want = "robust";
  }
  if (!want.empty()) {
    if (want != "iid" && want != "robust" && want != "cluster")
      return engineErrorBlockM4(
          "Option `vce(" + want + ")` is a valid Stata option that this version of PIET does not "
          "implement for `" + cobraCmd + "`.", "unsupported_option");
    argv.push_back("--vce");
    argv.push_back(want);
    if (want == "cluster") {
      argv.push_back("--cluster");
      argv.push_back(cvar);
    }
  }
  pushLevelIfGivenM4(parsed, argv);
  return runP7Mle(cobraCmd, argv, session);
}

// ===========================================================================
// reg3 (BS-065): `reg3 (y1 x1 x2) (y2 x3 y1) [(y3 ...) ...] [, sure]`

ojson runReg3(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

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

  bool sawSure = false, sawExplicit3sls = false;
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
          "implement for `reg3` (the engine's reg3 estimator has no vcov surface yet — classical "
          "SEs only).",
          "unsupported_option");
    }
  }
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      return engineErrorBlockM4(
          "Option `" + k + "` is a valid Stata option that this version of PIET does not yet "
          "implement for `reg3`.",
          "unsupported_option");
    }
    if (k == "level" && !v.isBool) {
      return engineErrorBlockM4(
          "Option `level` is a valid Stata option that this version of PIET does not yet "
          "implement for `reg3`.",
          "unsupported_option");
    }
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabReg3.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
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
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, "reg3", &session);

  LastModel lm;
  lm.cobraCmd = "reg3";
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
  return block;
}

// ===========================================================================
// boottest (BS-066): wild cluster bootstrap post-estimation, requires a
// preceding vce(cluster ...) fit in session.lastModel().

ojson runBoottest(const eval::ParsedCommand& parsed, SessionState& session) {
  const auto& lmOpt = session.lastModel();
  // Inlined rather than sharing test/lincom/nlcom's own `noPreviousEstimation()`
  // (defined further down this file, in the anonymous namespace right before
  // runTestCmd) — same message, just not reordering that pre-existing
  // structure to hoist a one-line helper across a large intervening block.
  if (!lmOpt.has_value()) {
    return engineErrorBlockM4("No previous estimation result. Run a regression first.");
  }
  const LastModel& lm = *lmOpt;
  if (lm.cluster.empty()) {
    return engineErrorBlockM4(
        "boottest requires a preceding regression with vce(cluster ...)", "unsupported_option");
  }
  if (parsed.varlist.empty()) {
    return engineErrorBlockM4("syntax_error: boottest requires a coefficient name", "syntax_error");
  }
  std::string param = parsed.varlist[0];

  // FU-23 (BUG-758): cluster()/robust by shape, w2l_vce_shape.log (each arm
  // after `regress y x1 x2, vce(cluster id)`).
  //  - boottest_bare_cluster / boottest_robust_val: boottest prints "Option
  //    cluster not allowed." / "Option robust(x1) not allowed." and stops at
  //    rc 0 with nothing returned (r(p) ., r(clustvars) empty). This handler
  //    skipped the bare `cluster` and bootstrapped anyway; `robust(x1)` hit the
  //    reject net r(198). Answered as boottest does: the message, rc 0, r()
  //    emptied, no bootstrap.
  //  - boottest_empty_cluster: `cluster()` is the fit's clustering (p 0,
  //    clustvars id); it was compared as an empty list and refused.
  //  - boottest_ctl_cluster_id2: `cluster(id2)` after a fit clustered on id is
  //    rc 0, "Overriding estimator's cluster/robust settings with cluster(id2)",
  //    clustvars id2. It was refused as a mismatch; the engine's wildboot refits
  //    on --cluster, so the override is forwarded and said in the block's note.
  //  - boottest_opt_c2: `cluster(id id2)` is a two-way bootstrap (rc 0, p .).
  //    The engine's wild bootstrap is one-way (src/inference/wildboot.cpp reads
  //    md.cluster.at(cluster_index), index 0): refused by name, never truncated
  //    to the first variable -- and so is a two-way FIT with no cluster(),
  //    which used to be forwarded as `--cluster a,b` and bootstrapped on `a`.
  const VceShapesM4b vceShape = readVceShapesM4b(parsed);
  if (vceShape.cluster == OptShapeM4b::kBare ||
      (vceShape.robust != OptShapeM4b::kAbsent && vceShape.robust != OptShapeM4b::kBare)) {
    session.eval.rValues = eval::OrderedFloatDict();
    session.eval.rStrings.clear();
    session.eval.rMatrices.clear();
    ojson note;
    note["type"] = "display";
    note["text"] = vceShape.cluster == OptShapeM4b::kBare
                       ? std::string("Option cluster not allowed.")
                       : "Option robust(" + vceShape.robustRaw + ") not allowed.";
    return note;
  }
  // A two-way fit records its clusters as ONE comma-joined element (runRegress
  // pushes the `--cluster id,id2` argv value), so the fit's list is split here
  // before it is counted.
  std::vector<std::string> bootClusters;
  for (const auto& c : lm.cluster) {
    std::string spaced = c;
    for (auto& ch : spaced) {
      if (ch == ',') ch = ' ';
    }
    for (const auto& t : splitTokensM4(spaced)) bootClusters.push_back(t);
  }
  if (vceShape.cluster == OptShapeM4b::kOne || vceShape.cluster == OptShapeM4b::kMany) {
    bootClusters = vceShape.clusterVars;
  }
  if (bootClusters.empty()) {
    return engineErrorBlockM4(
        "boottest requires a preceding regression with vce(cluster ...)", "unsupported_option");
  }
  if (bootClusters.size() > 1) {
    return engineErrorBlockM4(
        "boottest over more than one cluster variable (" + joinSpaceM4(bootClusters) +
            ") is multiway clustering, which this version of PIET does not implement: the "
            "engine's wild bootstrap is one-way. Name one variable in cluster().",
        "unsupported_option");
  }

  // BUG-163: wildboot has no --keepif on its CLI, so the fitted sample must
  // arrive as a FILE. fitDataPath already is one (runRegress's materialized
  // if/in filter); a --keepif-style fit is materialized here through the same
  // applyIfAndInFilter every other filtered handler uses.
  std::string bootData = lm.fitDataPath.empty() ? lm.dataPath : lm.fitDataPath;
  if (!lm.fitKeepif.empty()) {
    try {
      auto bfr = piet::handlers::applyIfAndInFilter(session, bootData, lm.fitKeepif, std::nullopt);
      if (bfr.ok) bootData = bfr.dataPath;
    } catch (const CobraError& ce) {
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
  }
  std::vector<std::string> args = {"wildboot",
                                    "--data",
                                    bootData,
                                    "--y",
                                    lm.y,
                                    "--x",
                                    lm.x.empty() ? "(intercept)" : joinCommaM4(lm.x),
                                    "--param",
                                    param,
                                    "--cluster",
                                    bootClusters[0]};
  if (!lm.fe.empty()) {
    args.push_back("--fe");
    args.push_back(joinCommaM4(lm.fe));
  }

  for (const auto& [k, v] : parsed.opts) {
    if (k == "reps" && !v.isBool) {
      args.push_back("--reps");
      args.push_back(v.strValue);
    } else if (k == "seed" && !v.isBool) {
      args.push_back("--seed");
      args.push_back(v.strValue);
    } else if (k == "weights" && !v.isBool) {
      std::string w = lowerM4(v.strValue);
      if (w != "rademacher" && w != "webb") {
        return engineErrorBlockM4("weights() must be rademacher or webb", "unsupported_option");
      }
      args.push_back("--weights");
      args.push_back(w);
    } else if (k == "null" && !v.isBool) {
      args.push_back("--null");
      args.push_back(v.strValue);
    }
    // cluster(): read by shape above (bootClusters). W:P5 refused a cluster()
    // that differed from the fit's on the premise that it "would silently
    // bootstrap over clusters the model never used"; boottest itself overrides
    // the fit and says so (boottest_ctl_cluster_id2), which the note below
    // carries. nograph: nothing to suppress (vocabulary note at file top).
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabBoottest.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
  }

  args.push_back("--tidy");
  ojson raw;
  try {
    raw = runCobraJsonM4(args);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson block = cobraToEstimationM4(raw, "boottest", &session);
  if (vceShape.cluster == OptShapeM4b::kOne) {
    const std::string text =
        "Overriding estimator's cluster/robust settings with cluster(" + bootClusters[0] + ")";
    const std::string cur = block.contains("note") && block["note"].is_string()
                                ? block["note"].get<std::string>()
                                : std::string();
    block["note"] = cur.empty() ? text : (cur + "  " + text);
  }
  // boottest is r-class. The PREC records of w2l_vce_shape.log read r(p) (the
  // symmetric p, "Prob>|t|" in its output), r(N_clust) (missing in every arm,
  // so not set) and r(clustvars); nothing here published any of them.
  session.eval.rValues = eval::OrderedFloatDict();
  session.eval.rStrings.clear();
  session.eval.rMatrices.clear();
  if (raw.contains("p_symmetric") && raw["p_symmetric"].is_number()) {
    session.eval.rValues.set("p", raw["p_symmetric"].get<double>());
  }
  session.eval.rStrings.set("clustvars", bootClusters[0]);
  return block;
}

// ===========================================================================
// tobit (BS-069, BETA D2 batch): `tobit depvar indepvars [if], ll[(#)]
// ul[(#)] [level(#)]` -> `cobra tobit --ll/--ul`. Oracle:
// private-notes/plans/BETA-20260712-campaign/probes/d2-oracle-stata.log
// (D2a: ul(15000), D2b: ll(11000)) — both coefficients AND the reported
// variance/uncensored/censored counts match cobra's `tobit` byte-for-byte
// (live-verified against this exact engine build, no convention gap here
// unlike D1's ivregress SE denominator). At least one of ll/ul is required
// (matches both Stata's own requirement and cobra's own usage banner: "at
// least one of --ll/--ul required").
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

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabTobit.known;
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
ojson runHeckman(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: heckman requires a dependent variable and at least one regressor",
        "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::optional<std::string> selectRaw;
  bool sawTwostep = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "select" && !v.isBool) selectRaw = v.strValue;
    if (k == "twostep" && v.isBool && v.boolValue) sawTwostep = true;
  }
  if (!selectRaw.has_value() || trimM4(*selectRaw).empty()) {
    return engineErrorBlockM4("syntax_error: heckman requires select(depvar = varlist)",
                              "syntax_error");
  }
  size_t eq = selectRaw->find('=');
  if (eq == std::string::npos) {
    return engineErrorBlockM4("syntax_error: malformed select(): missing '='", "syntax_error");
  }
  if (selectRaw->find('=', eq + 1) != std::string::npos) {
    return engineErrorBlockM4("syntax_error: malformed select(): only one '=' is allowed",
                              "syntax_error");
  }
  std::vector<std::string> selDep = splitTokensM4(selectRaw->substr(0, eq));
  std::vector<std::string> selVars = splitTokensM4(selectRaw->substr(eq + 1));
  if (selDep.size() != 1 || selVars.empty()) {
    return engineErrorBlockM4(
        "syntax_error: malformed select(): need exactly one selection-equation dependent "
        "variable and at least one identifying variable",
        "syntax_error");
  }

  if (!sawTwostep) {
    return engineErrorBlockM4(
        "Full maximum-likelihood `heckman` (Stata's default when `twostep` is omitted) is not "
        "supported by this version of PIET — the engine's heckman estimator only implements the "
        "two-step Heckit method. Use `heckman ..., select(...) twostep`.",
        "unsupported_option");
  }

  std::vector<std::string> argv = {"heckman", "--y", y, "--x", joinCommaM4(xs), "--select",
                                    selDep[0] + "=" + joinCommaM4(selVars)};
  // Deliberately no --twostep (see the function-header comment: the flag
  // itself triggers a cobra CLI masking bug; the engine's bare default
  // already computes the two-step estimate the oracle pins).
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // BUG-274: `level()` used to fall through to the generic reject net below
  // ("option `level` not allowed" — `level` was simply absent from kKnown).
  // BUG-202 gave cobra's heckman a real `--level` (ci_lo/ci_hi on BOTH the
  // outcome coeftable and select_coefs, asymptotic z — live-verified on this
  // tree's engine), so it is now in kKnown and forwards, same idiom as
  // runTobit/runNbreg. The engine validates the range itself ([10, 99.99]).
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      argv.push_back("--level");
      argv.push_back(v.strValue);
      continue;
    }
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabHeckman.known;
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

  ojson block = cobraToEstimationM4(raw, "heckman", &session);
  block["depvar"] = y;  // cobra heckman's raw JSON carries no "depvar" field at all (live-verified)
  block["estimator"] = "heckman";  // Stata's own display name is "heckman" regardless of twostep
  static const char* kHeckmanFields[] = {
      "lambda", "se_lambda", "rho", "sigma", "n_selected", "select_coefs",
  };
  for (const char* k : kHeckmanFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // Multi-equation shape (outcome + selection): reuse the SAME
  // {depvar, coeftable, r2, r2_within, dof_resid}-per-entry "outcomes" array
  // shape D1's reg3 adapter already established (cobraToEstimationM4's
  // "equations"->"outcomes" remap above), so any future "iterate
  // block.outcomes[]" frontend renderer picks up heckman's two equations
  // for free. cobra's select_coefs rows carry name/b/se/t always, and — since
  // BUG-202 gave heckman `--level` — ci_lo/ci_hi too WHEN level() was
  // forwarded (live-verified both ways on this tree's engine: absent without
  // --level, populated with it). Carry what the engine sent; p is genuinely
  // never emitted on select rows and stays null, not fabricated (BUG-274
  // un-staled the old "no p/CI ever" wording here).
  if (raw.contains("select_coefs") && raw["select_coefs"].is_array()) {
    ojson selCt = ojson::array();
    for (const auto& c : raw["select_coefs"]) {
      ojson r;
      r["name"] = c.value("name", std::string());
      r["b"] = c.contains("b") ? c["b"] : ojson(nullptr);
      r["se"] = c.contains("se") ? c["se"] : ojson(nullptr);
      r["t"] = c.contains("t") ? c["t"] : ojson(nullptr);
      r["p"] = ojson(nullptr);
      r["ci_lo"] = c.contains("ci_lo") ? c["ci_lo"] : ojson(nullptr);
      r["ci_hi"] = c.contains("ci_hi") ? c["ci_hi"] : ojson(nullptr);
      selCt.push_back(r);
    }
    ojson outcomeEq;
    outcomeEq["depvar"] = y;
    outcomeEq["coeftable"] = block.value("coeftable", ojson::array());
    outcomeEq["r2"] = ojson(nullptr);
    outcomeEq["r2_within"] = ojson(nullptr);
    outcomeEq["dof_resid"] = ojson(nullptr);
    ojson selectEq;
    selectEq["depvar"] = selDep[0];
    selectEq["coeftable"] = selCt;
    selectEq["r2"] = ojson(nullptr);
    selectEq["r2_within"] = ojson(nullptr);
    selectEq["dof_resid"] = ojson(nullptr);
    ojson outcomes = ojson::array();
    outcomes.push_back(outcomeEq);
    outcomes.push_back(selectEq);
    block["outcomes"] = outcomes;
  }

  LastModel lm;
  lm.cobraCmd = "heckman";  // added to runMargins's kLinearCmds below — heckman's outcome
                            // equation is a plain linear fit, so dydx is equally meaningless.
  lm.y = y;
  lm.x = xs;
  lm.depvar = y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  return block;
}

// ===========================================================================
// qreg (BS-071, BETA D2 batch): `qreg depvar indepvars [if] [, quantile(#)
// level(#)]` -> `cobra qreg --tau --level`. Oracle: d2-oracle-stata.log D2e
// (median, no quantile() at all — cobra's own --tau default is 0.5,
// matching Stata's) / D2f (quantile(.25)).
//
// Stata's quantile() accepts EITHER a bare fraction in (0,1) exclusive
// (`.25`/`0.25`) OR a percentage in (1,100) exclusive (`25`) — normalized
// here to cobra's own `--tau` fraction convention (values > 1 are treated
// as a percentage). Unlike tobit/ivregress/reg3's `level()` (never
// forwarded — see above), qreg's own usage banner DOES list `--level`, and
// live-verified it genuinely changes the returned CI (and even the SE/t/p —
// a real per-level rank-based interval, not a cosmetic no-op: `--level 90`
// vs `--level 95` on the same fit produced different se/t/p, not just a
// wider/narrower ci_lo/ci_hi off a fixed se) — so `level()` DOES forward here.
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
  for (const auto& [k, v] : parsed.opts) {
    if (k != "vce") continue;
    const std::string lv = v.isBool ? std::string() : lowerM4(trimM4(v.strValue));
    if (lv == "robust") {
      qregRobust = true;
    } else if (lv != "iid") {
      return engineErrorBlockM4(
          "Option `vce(" + (v.isBool ? std::string() : v.strValue) +
              ")` is not implemented for `qreg` in this version of PIET; vce(iid) and "
              "vce(robust) are.",
          "unsupported_option");
    }
  }
  if (qregRobust) {
    argv.push_back("--vce");
    argv.push_back("robust");
  }
  // [pw=] is honoured only beside vce(robust), the one captured pair. No
  // Stata record says which VCE a pweighted qreg WITHOUT vce() reports, so
  // that form is refused rather than answered under a guessed VCE.
  if (parsed.weightType.has_value() && parsed.weightVar.has_value() &&
      !parsed.weightVar->empty()) {
    const std::string wt = lowerM4(*parsed.weightType);
    if (wt != "pw" && wt != "pweight") {
      return engineErrorBlockM4("`qreg` supports only [pweight=] in this version of PIET.",
                                "unsupported_option");
    }
    if (!qregRobust) {
      return engineErrorBlockM4(
          "`qreg ... [pweight=]` without vce(robust) is not supported in this version of "
          "PIET; specify vce(robust).",
          "unsupported_option");
    }
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
namespace {

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

  std::vector<std::string> yToks = splitTokensM4(pg.groups[0]);
  std::vector<std::string> mToks = splitTokensM4(pg.groups[1]);
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
// nbreg, xtpoisson, xtlogit): private-notes/plans/BETA-20260712-campaign/
// probes/d3-oracle-stata.log (StataBE 19.5 on piet/packaging/sample/
// fatalities.csv, `gen cnt=round(spirits*3)` + `encode state, gen(snum)` +
// `xtset snum year`; sections D3a-D3g). Oracle (drdid): tests/validate/
// drdid2x2.{do,dta,log}.

namespace est_detail {  // R2: reopened — fillCoeftableStatsM4 is declared in TU 1

// nbreg/xtpois/xtlogit's own raw "coeftable" always carries p=ci_lo=ci_hi=
// null (live-verified against all three CLIs — only "b"/"se"/"t" are ever
// populated, t==b/se already). fepois's own coeftable, by contrast, DOES
// compute p/ci itself (also live-verified) so is never routed through this.
// Backfills the null fields from b/se/t the SAME Wald-normal way coefRowM4
// (csdid, above) already does, so the PIET UI doesn't ship a coeftable with
// three permanently-blank columns for these three estimator families.
void fillCoeftableStatsM4(ojson& block) {
  if (!block.contains("coeftable") || !block["coeftable"].is_array()) return;
  for (auto& row : block["coeftable"]) {
    if (!row.contains("b") || row["b"].is_null() || !row.contains("se") || row["se"].is_null()) {
      continue;
    }
    double b = row["b"].get<double>();
    double se = row["se"].get<double>();
    double z;
    if (row.contains("t") && row["t"].is_number()) {
      z = row["t"].get<double>();
    } else if (se != 0.0) {
      z = b / se;
      row["t"] = z;
    } else {
      continue;
    }
    if (!row.contains("p") || row["p"].is_null()) row["p"] = normalPvalue(z);
    if (!row.contains("ci_lo") || row["ci_lo"].is_null()) row["ci_lo"] = b - kZ975 * se;
    if (!row.contains("ci_hi") || row["ci_hi"].is_null()) row["ci_hi"] = b + kZ975 * se;
  }
}

}  // namespace est_detail

// poisson / ppmlhdfe / fepois (BS-073): count-model wiring -> cobra fepois.
// Oracle D3a (bare poisson b/se), D3b (offset()), D3c (vce(robust)), D3d
// (ppmlhdfe/absorb()).
//
// D0 finding (pre-existing, re-confirmed live): cobra's `fepois` WITHOUT
// --fe reproduces Stata's bare `poisson` exactly (D3a: beertax b=-.0537468
// se=.0514636, unemp b=-.0349375 se=.0097193, _cons b=1.940487 se=.076412 —
// all byte-for-byte once `cnt` is built with Stata's own round()
// convention, which cobra's `eval --gen round(...)` already matches
// live-verified). Real Stata's `poisson` has NO absorb() option at all;
// `ppmlhdfe` and cobra's own `fepois` command name both DO (-> --fe).
//
// offset() (D3b): LIVE-VERIFIED literal passthrough. Stata's own offset()
// option already expects an ln-scale variable (offset(lpop) where
// lpop=ln(income) — that IS what Stata's offset() has always meant); cobra's
// `--offset` expects the exact same thing. beertax b=.0753822 se=.0495923,
// _cons b=-7.914716 se=.0343278 — byte-for-byte with NO PIET-side ln()
// transform. exposure() (Stata's OTHER, raw-exposure count-model option) has
// no oracle coverage and no engine CLI equivalent — honest reject.
//
// ppmlhdfe/fepois DEFAULT to vce(robust) when the user gives no vce() at
// all — matching real ppmlhdfe's own default (the D3d oracle's own
// `ppmlhdfe cnt beertax, absorb(snum)` never types vce() yet its Stata
// table header reads "Robust std. err.", live-verified). Plain `poisson`
// keeps cobra's own iid default (matches D3a, which likewise never types
// vce() and gets the plain "Std. err." table).
//
// noconstant/level() are NEVER forwarded — live-verified the SAME
// cobra_runner.cpp stdout/exit-code masking bug the D1 report already
// flagged for ivregress/tobit: `cobra fepois --no-intercept` / `--level 90`
// BOTH hard-error on stderr ("unrecognized option", exit 3) while STILL
// printing a full, valid, WRONG (intercept-included / default-CI) result to
// stdout first.
//
// ENGINE FINDING (cobra-side, forbidden this batch — not fixed, reported):
// vce(robust)'s HC1 small-sample correction has a real numeric gap vs
// Stata's own poisson/ppmlhdfe robust SE. Point estimates (b) match Stata
// EXACTLY in every case tried. For the bare fepois case (D3c) the SE gap is
// small (~0.3% relative: engine .0321597 vs Stata .0320636). For the --fe
// (ppmlhdfe, D3d) case the gap is MATERIAL (~8% relative: engine .1238974
// vs Stata .1146782) — apparently from how absorbed-FE degrees of freedom
// enter cobra's small-sample correction factor (dof_resid IS reported
// correctly as 287 = N-k-FE in both the b and se paths, yet the se scale
// doesn't reflect it the way Stata's does). Selftest pins to the ENGINE's
// OWN actual value (not the Stata figure) with a comment, so the gate stays
// meaningful without permanently failing on a gap this batch cannot touch.
namespace {

// ppmlhdfe's `d`: the sum of the fixed effects, built on the head the fit read
// and written to a new head whose path is returned ("" and `err` on refusal).
//
// ppmlhdfe.mata:473-486 stores d = z - resid - x*b, demeaned with the IRLS
// weights mu: at convergence z - resid is eta = ln(mu), so
//   d_i = ln(mu_i) - x_i'b - c,   c = sum mu (ln mu - x'b) / sum mu,
// and c is exactly the reported _cons (:461-465 recovers _cons as the
// mu-weighted mean of ln(mu) - x'b). With ONE absorbed variable the FE score
// equations close the form: within group g, exp(eta) = exp(x'b) * Y_g / E_g with
// Y_g = sum y and E_g = sum exp(x'b), and sum_g mu = Y_g, so
//   d_i = ln(Y_g) - ln(E_g) - c,   c = sum_g Y_g (ln Y_g - ln E_g) / sum Y.
// Measured on the w2b_estim fixture (float-stored, absorb(id)): c from the
// engine's b is -2.0577731961463126, the engine's _cons -2.057773196146313 and
// the oracle's table -2.057773 (w2b_estim.log:1357). Without absorb() the only
// absorbed term is the constant, so d is 0 on the estimation sample.
// The sample is rebuilt here (nonmissing, if, singleton and all-zero-y groups
// out, as the engine drops them); a count that differs from the fit's N refuses
// rather than writing d on the wrong rows.
// The closed form is the UNWEIGHTED one (every sum below is unweighted). Weights never
// reach it: runPoisson refuses d/d() together with any weight before the fit.
std::string ppmlhdfeSaveDM4b(SessionState& session, const ojson& raw, const std::string& y,
                             const std::vector<std::string>& xs,
                             const std::vector<std::string>& fes,
                             const eval::ParsedCommand& parsed, const std::string& dName,
                             ojson& err) {
  auto refuse = [&](const std::string& why) {
    err = ojson(eval::errorBlock("unsupported_option", "ppmlhdfe d: " + why, std::nullopt,
                                 std::nullopt, std::nullopt, "r(198)"));
    return std::string();
  };
  Datastore* ds = session.datastore();
  if (ds == nullptr) return refuse("no dataset in memory");
  for (const auto& f : fes) {
    if (!ds->has(f) || ds->dtype(f) != "f64") {
      err = engineErrorBlockM4(
          "Option `d` is a valid ppmlhdfe option that this version of PIET implements for a "
          "numeric absorb() variable only (`" + f + "`).",
          "unsupported_option");
      return std::string();
    }
  }
  char buf[64];
  std::string xb;
  for (const auto& x : xs) {
    const double b = numberAtM4b(raw, "coef", x);
    if (!std::isfinite(b)) return refuse("the fit has no coefficient for `" + x + "`");
    std::snprintf(buf, sizeof(buf), "%.17g", b);
    xb += (xb.empty() ? "" : "+") + std::string("(") + buf + ")*(" + x + ")";
  }

  std::string sample = "!missing(" + y + ")";
  for (const auto& x : xs) sample += "&!missing(" + x + ")";
  for (const auto& f : fes) sample += "&!missing(" + f + ")";
  if (parsed.weightVar.has_value() && !parsed.weightVar->empty()) {
    sample += "&!missing(" + *parsed.weightVar + ")";
  }
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    sample += "&(" + *parsed.ifExpr + ")";
  }

  // Scratch column names (design decision: no shared helper mints one); the
  // double-underscore shape of handlers.cpp's residual mask.
  const std::string kS = "__piet_ppmld_s__", kN = "__piet_ppmld_n__", kY = "__piet_ppmld_y__",
                    kK = "__piet_ppmld_k__", kE = "__piet_ppmld_e__", kEs = "__piet_ppmld_es__",
                    kA = "__piet_ppmld_a__", kT1 = "__piet_ppmld_t1__", kT2 = "__piet_ppmld_t2__";
  std::vector<std::string> temps;
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin()};
  auto gen = [&](const std::string& name, const std::string& expr) {
    args.push_back("--gen");
    args.push_back(name + "=" + expr);
    if (name != dName) temps.push_back(name);
  };
  auto egen = [&](const std::string& name, const std::string& expr) {
    args.push_back("--egen");
    args.push_back(name + "=" + expr);
    temps.push_back(name);
  };
  gen(kS, "(" + sample + ")");
  if (fes.empty()) {
    gen(dName, "cond(" + kS + "==1,0,.)");
  } else {
    const std::string& g = fes[0];
    egen(kN, "sum(" + kS + "), by(" + g + ")");
    egen(kY, "sum(cond(" + kS + "==1," + y + ",0)), by(" + g + ")");
    gen(kK, "(" + kS + "==1)&(" + kN + ">1)&(" + kY + ">0)");
    gen(kE, "cond(" + kK + "==1,exp(" + xb + "),0)");
    egen(kEs, "sum(" + kE + "), by(" + g + ")");
    gen(kA, "cond(" + kK + "==1,ln(" + kY + ")-ln(" + kEs + "),.)");
    egen(kT1, "sum(cond(" + kK + "==1," + y + "*" + kA + ",0))");
    egen(kT2, "sum(cond(" + kK + "==1," + y + ",0))");
    gen(dName, "cond(" + kK + "==1," + kA + "-" + kT1 + "/" + kT2 + ",.)");
  }
  for (const auto& t : temps) {
    args.push_back("--drop");
    args.push_back(t);
  }
  args.push_back("--summarize");
  args.push_back(dName);
  const std::string outPath = nextPathM4(session.workdir(), "ppmlhdfe_d");
  args.push_back("--out");
  args.push_back(outPath);

  nlohmann::json res;
  try {
    res = runCobraJsonM4(args);
  } catch (const CobraError& ce) {
    err = engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    return std::string();
  }
  double nD = std::nan("");
  try {
    nD = res.at("summarize").at(dName).at("N").get<double>();
  } catch (...) {
  }
  const double nFit = raw.contains("n_obs") && raw["n_obs"].is_number()
                          ? raw["n_obs"].get<double>()
                          : std::nan("");
  if (!std::isfinite(nD) || !std::isfinite(nFit) || nD != nFit) {
    cecon::native_unlink(outPath);
    return refuse("could not reproduce the estimation sample (" +
                  (std::isfinite(nD) ? std::to_string(static_cast<long long>(nD)) : "?") +
                  " rows against the fit's " +
                  (std::isfinite(nFit) ? std::to_string(static_cast<long long>(nFit)) : "?") +
                  "); d is not saved.");
  }
  return outPath;
}

}  // namespace

ojson runPoisson(const std::string& stataCmd, const eval::ParsedCommand& parsed,
                  SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: `" + stataCmd + "` requires a dependent variable and at least one "
        "regressor (a no-covariate model is not supported — the engine's fepois CLI has no "
        "intercept-only escape hatch, unlike feols/logit's `(intercept)` placeholder).",
        "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  bool allowFe = (stataCmd == "ppmlhdfe" || stataCmd == "fepois");

  std::string feRaw;
  bool sawAbsorb = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "absorb") {
      sawAbsorb = true;
      if (!v.isBool) feRaw = v.strValue;
    }
  }
  if (sawAbsorb && !allowFe) {
    return engineErrorBlockM4(
        "option `absorb` not allowed — real Stata's `poisson` has no absorb() option; use "
        "`ppmlhdfe` for a fixed-effects Poisson (PPML) fit.",
        "unsupported_option");
  }

  // ppmlhdfe `d` / `d(newvar)`: save the sum of the fixed effects
  // (w2b_estim.log:1305 ppmlhdfe_d, :1336 ppmlhdfe_absorb_d_named, both rc 0
  // with the fit of the option-free command). ppmlhdfe.ado:188-196: bare `d`
  // DROPS any existing _ppmlhdfe_d and names the column that; d(newvar) is
  // `confirm new variable` (r(110) when it exists), checked before the fit.
  // The column is built after the fit by ppmlhdfeSaveDM4b. Only the shapes it
  // reproduces exactly are taken; the rest are refused by name.
  std::string dName;
  if (stataCmd == "ppmlhdfe") {
    for (const auto& [k, v] : parsed.opts) {
      if (k != "d") continue;
      if (v.isBool) {
        dName = "_ppmlhdfe_d";
        // ppmlhdfe.ado:191 drops the old column before the fit reads its
        // variables, so a model that USES _ppmlhdfe_d no longer finds it.
        const std::vector<std::string> feToks = splitTokensM4(feRaw);
        if (y == dName || std::find(xs.begin(), xs.end(), dName) != xs.end() ||
            std::find(feToks.begin(), feToks.end(), dName) != feToks.end()) {
          return stataRefusalM4b("variable " + dName + " not found", 111);
        }
        continue;
      }
      dName = trimM4(v.strValue);
      if (dName.empty() || !strutil::isValidStataIdentifier(dName)) {
        return stataRefusalM4b("d(): invalid name `" + dName + "'", 198);
      }
      Datastore* ds = session.datastore();
      if (ds != nullptr && ds->has(dName)) {
        return stataRefusalM4b("variable " + dName + " already defined", 110);
      }
    }
  }
  if (!dName.empty()) {
    std::string why;
    if (splitTokensM4(feRaw).size() > 1) {
      why = "more than one absorbed fixed effect (the sum of several fixed effects has no "
            "closed form here)";
    }
    for (const auto& [k, v] : parsed.opts) {
      if (k == "offset" || k == "exposure") why = "offset()/exposure()";
    }
    if (parsed.inRange.has_value() && !parsed.inRange->empty()) why = "an `in` range";
    for (const auto& x : xs) {
      if (x.find_first_of(".#()") != std::string::npos) {
        why = "factor-variable or time-series regressors";
      }
    }
    // Weights (last, so it wins). Corpus: 0 of 81 ppmlhdfe statements carry a weight. The
    // weighted closed form is source-derived (ppmlhdfe.mata:464,478-479, reghdfe5.mata:893:
    // the FE sum is demeaned with W = w*mu) but uncaptured (draft w2q_ppml_d_weights), and
    // ppmlhdfeSaveDM4b only knows the unweighted sums. The engine refuses weighted --fe
    // (src/estimators/fepois.cpp:30-33), so no weighted absorb() fit could exercise it.
    if (parsed.weightVar.has_value() && !parsed.weightVar->empty()) why = "weights";
    if (!why.empty()) {
      return ojson(eval::errorBlock(
          "unsupported_option",
          "Option `d` is a valid ppmlhdfe option that this version of PIET does not implement "
          "together with " + why + ".",
          std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    }
  }

  std::vector<std::string> argv = {"fepois", "--y", y, "--x", joinCommaM4(xs)};

  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // W:P5: `poisson … [pweight=w]`. registry.cpp's row for this command decides
  // which weight types get this far; the engine's fepois honours --weight/
  // --wtype. Same normalization the shared builder and runRegress use.
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

  if (sawAbsorb) {
    std::vector<std::string> feToks = splitTokensM4(feRaw);
    if (!feToks.empty()) {
      argv.push_back("--fe");
      argv.push_back(joinCommaM4(feToks));
    }
  }

  // offset(varname) — D3b oracle-verified LITERAL passthrough (see header comment).
  std::optional<std::string> offsetVar;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "offset" && !v.isBool) offsetVar = v.strValue;
  }
  if (offsetVar.has_value() && !offsetVar->empty()) {
    argv.push_back("--offset");
    argv.push_back(*offsetVar);
  }
  for (const auto& [k, v] : parsed.opts) {
    if (k == "exposure" && !v.isBool) {
      return engineErrorBlockM4(
          "Option `exposure()` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (no oracle coverage and no engine CLI equivalent — "
          "use offset() with a pre-computed ln(exposure) variable instead).",
          "unsupported_option");
    }
  }

  VcovResult vr = vcovFromOpts(parsed);
  if (vr.errored) {
    return engineErrorBlockM4(vr.errorMsg, "unsupported_option");
  }
  bool sawExplicitVce = !vr.argv.empty();
  // ppmlhdfe/fepois DEFAULT to robust when the user specified no vce() at
  // all (see header comment) — plain `poisson` keeps cobra's iid default.
  if (!sawExplicitVce && allowFe) {
    vr.argv = {"--vcov", "hc1"};
  }
  for (const auto& a : vr.argv) argv.push_back(a);

  // noconstant/level() — NEVER forwarded (masking-bug precedent, see header comment).
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      return engineErrorBlockM4(
          "Option `" + k + "` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (live-verified: cobra fepois's `--no-intercept` "
          "hard-errors on stderr while still printing a full WRONG result to stdout — never "
          "forwarded).",
          "unsupported_option");
    }
    if (k == "level" && !v.isBool) {
      return engineErrorBlockM4(
          "Option `level` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (same stdout/exit-code masking gap as noconstant "
          "above).",
          "unsupported_option");
    }
  }

  // kVocabPpmlD is ppmlhdfe's own slot: the shared count-model set plus `d`.
  const std::unordered_set<std::string>& known =
      (stataCmd == "ppmlhdfe") ? kVocabPpmlD.known : kVocabPoisson.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, known)) {
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

  // d: built on the head the fit read, BEFORE e() is posted, so a column that
  // cannot be reproduced refuses the whole command instead of leaving e()
  // describing a fit whose d is absent.
  std::string dPath;
  if (!dName.empty()) {
    ojson err;
    dPath = ppmlhdfeSaveDM4b(session, raw, y, xs, splitTokensM4(feRaw), parsed, dName, err);
    if (dPath.empty()) return err;
  }

  ojson block = cobraToEstimationM4(raw, "fepois", &session);
  block["estimator"] = stataCmd;  // literal Stata command word typed, not cobra's internal name.
  block["depvar"] = y;
  if (!dName.empty()) {
    ojson notes = block.contains("warnings") && block["warnings"].is_array() ? block["warnings"]
                                                                             : ojson::array();
    notes.push_back("e(d) is not posted by this version of PIET; the sum of the fixed effects is in " +
                    dName + ".");
    block["warnings"] = notes;
  }

  LastModel lm;
  lm.cobraCmd = sawAbsorb ? "fepois" : "poisson";  // margins' kModelMap already maps BOTH keys
                                                    // to "--model poisson" for AME dydx.
  lm.y = y;
  lm.x = xs;
  if (sawAbsorb) {
    for (const auto& f : splitTokensM4(feRaw)) lm.fe.push_back(f);
  }
  for (size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "--vcov" && i + 1 < argv.size()) lm.vcov = argv[i + 1];
    if (argv[i] == "--cluster" && i + 1 < argv.size()) {
      for (const auto& c : splitTokensM4(argv[i + 1])) lm.cluster.push_back(c);
    }
  }
  lm.depvar = y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  if (!dPath.empty()) {
    // The head the fit (and lm.dataPath) read stays untouched until e() is
    // posted; the new column lands now (the reghdfe residuals() order).
    // ppmlhdfe.ado:366 also posts e(d), which is NOT published here (FU-153): setLastModel
    // erases e(resid) for every new fit but has no such reset for e(d), so the
    // macro would outlive this model and name its column after the next fit. The
    // estimation block's warnings say so instead.
    session.load(dPath);
  }
  return block;
}

// nbreg (BS-074): negative-binomial (NB2, "dispersion(mean)" — Stata's
// default) regression -> cobra nbreg. Oracle D3e.
//
// ENGINE FIX (cobra 0.25.0): the Newton optimizer's convergence gap
// described below is fixed. cobra's nbreg now converges on the D3e oracle's
// own data (`nbreg cnt beertax unemp` on fatalities.csv) — the true MLE
// sits at the alpha->0 boundary (the Poisson limit: this data has no
// detectable overdispersion once beertax/unemp are controlled for), and
// cobra now detects and reports that boundary cleanly via a Poisson
// warm-start feeding the joint (beta, ln-alpha) Newton fit plus explicit
// boundary reporting, instead of the previous "no_convergence" failure.
// (History: through 0.24.x, cobra's nbreg Newton optimizer FAILED TO
// CONVERGE on this and several independently-constructed synthetic
// overdispersed count datasets whenever the fitted intercept sat
// appreciably away from zero — a genuine intercept-initialization/step-size
// gap in the engine's joint Newton fit, not a PIET-wiring problem.) The
// wiring below is complete and correct; selftest now pins b/se/alpha/
// lnalpha against the small SYNTHETIC dataset (argv/JSON round-trip proof,
// not Stata-oracle-pinned) AND separately oracle-pins the D3e leg's own
// exact call (`nbreg cnt beertax unemp`) against its Stata coefficients,
// ll, and the alpha=0 boundary (selftest.cpp).
ojson runNbreg(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4("syntax_error: `nbreg` requires a dependent variable and at least "
                              "one regressor",
                              "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::vector<std::string> argv = {"nbreg", "--y", y, "--x", joinCommaM4(xs)};

  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  bool noConstant = false;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) noConstant = true;
  }
  // Unlike fepois above, cobra nbreg's `--no-intercept` is DOCUMENTED and
  // live-verified genuinely honored (not a masking no-op) — safe to forward.
  if (noConstant) argv.push_back("--no-intercept");

  std::optional<std::string> offsetVar;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "offset" && !v.isBool) offsetVar = v.strValue;
  }
  if (offsetVar.has_value() && !offsetVar->empty()) {
    argv.push_back("--offset");
    argv.push_back(*offsetVar);
  }
  for (const auto& [k, v] : parsed.opts) {
    if (k == "exposure" && !v.isBool) {
      return engineErrorBlockM4(
          "Option `exposure()` is a valid Stata option that this version of PIET does not yet "
          "implement for `nbreg` (no oracle coverage and no engine CLI equivalent — use offset() "
          "with a pre-computed ln(exposure) variable instead).",
          "unsupported_option");
    }
    if (k == "dispersion" && !v.isBool) {
      return engineErrorBlockM4(
          "Option `dispersion()` is a valid Stata option that this version of PIET does not yet "
          "implement for `nbreg` (only the default NB2 \"mean\" dispersion is wired — no oracle "
          "coverage for dispersion(constant)/NB1).",
          "unsupported_option");
    }
  }

  // vce(): nbreg's own CLI shape is `--vce iid|robust|cluster [--cluster C]`
  // (NOT feols/fepois's `--vcov`/hc1/twoway shape) — reuse vcovFromOpts for
  // the Stata-side parsing, then translate its {"--vcov",TYPE,...} output.
  // "twoway" (2 cluster vars) has no nbreg equivalent (usage banner: a
  // single --cluster slot only) — honest reject. A missing cluster var
  // inside vce(cluster) already fails inside vcovFromOpts itself (shared
  // discipline with feols/logit) before ever reaching here, so nbreg's own
  // live-verified `--vce cluster`-with-no-`--cluster` silent-no-op shape
  // (identical numbers to vce(robust)) is structurally unreachable from
  // this handler.
  VcovResult vr = vcovFromOpts(parsed);
  if (vr.errored) {
    return engineErrorBlockM4(vr.errorMsg, "unsupported_option");
  }
  if (vr.argv.size() >= 2 && vr.argv[0] == "--vcov") {
    const std::string& type = vr.argv[1];
    if (type == "hc1") {
      argv.push_back("--vce");
      argv.push_back("robust");
    } else if (type == "cluster") {
      argv.push_back("--vce");
      argv.push_back("cluster");
      argv.push_back("--cluster");
      argv.push_back(vr.argv[3]);  // vr.argv == {"--vcov","cluster","--cluster",VAR}
    } else if (type == "twoway") {
      return engineErrorBlockM4(
          "vce(cluster A B) (two-way clustering) is not supported for `nbreg` — the engine's "
          "nbreg CLI has only a single --cluster slot.",
          "unsupported_option");
    } else if (type == "iid") {
      argv.push_back("--vce");
      argv.push_back("iid");
    }
  }

  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      // BUG-274: this used to reject, on the premise that "cobra nbreg's --level hard-errors
      // on stderr while still printing a full WRONG result to stdout". That premise is doubly
      // expired: BUG-202 gave nbreg a real `--level` (per-coef ci_lo/ci_hi at the asked level,
      // z-based like Stata's own nbreg table — live-verified `--level 90` on this tree's
      // engine), and even the PRE-fix half was stale against the current pin (WS-S re-measured:
      // stdout was EMPTY on the hard-error, not a masked result). Forward, same idiom as
      // runTobit/runQreg above. The engine validates the range itself ([10, 99.99]) and its
      // error envelope surfaces through CobraError below.
      argv.push_back("--level");
      argv.push_back(v.strValue);
      continue;
    }
  }

  const std::unordered_set<std::string>& known = kVocabNbreg.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, known)) {
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

  ojson block = cobraToEstimationM4(raw, "nbreg", &session);
  block["estimator"] = "nbreg";
  block["depvar"] = y;
  fillCoeftableStatsM4(block);
  if (raw.contains("alpha")) block["alpha"] = raw["alpha"];
  if (raw.contains("lnalpha")) block["lnalpha"] = raw["lnalpha"];
  if (raw.contains("se_lnalpha_iid")) block["se_lnalpha"] = raw["se_lnalpha_iid"];
  // ll: cobra emits `loglik`; the block key is `ll` like every other MLE block
  // (logit/probit) — was omitted here, leaving nbreg the odd one out.
  if (raw.contains("loglik")) block["ll"] = raw["loglik"];

  LastModel lm;
  lm.cobraCmd = "nbreg";
  lm.y = y;
  lm.x = xs;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "--vce" && i + 1 < argv.size()) lm.vcov = argv[i + 1];
    if (argv[i] == "--cluster" && i + 1 < argv.size()) lm.cluster = {argv[i + 1]};
  }
  lm.depvar = y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  return block;
}

// xtpoisson / xtlogit, fe (BS-075): conditional fixed-effects panel count/
// binary MLE. Oracle D3f (xtpoisson,fe), D3g (xtlogit,fe; depvar
// hi2=(spirits>1.5)). Both reproduce the oracle's b/se EXACTLY (bit-for-bit
// at 8+ significant digits, including D3g's 37-groups-omitted/n_obs=77
// all-positive-or-negative-outcome dropping and D3f's loglik=-460.6417) — no
// engine gap found for either, unlike BS-073/074 above.
//
// Real Stata's DEFAULT (bare `xtpoisson`/`xtlogit`, no fe/re/pa) is
// RANDOM-EFFECTS, not fixed — this engine has NO xtpoisson/xtlogit RE (or
// PA) estimator at all (only the conditional-FE MLE), so `,re`/`,pa` and
// the bare/no-option form ALL honest-reject naming `,fe` as the only
// supported form, matching runXtreg's own fe/re model-select precedent.
//
// vce(robust) genuinely changes the SE for BOTH (live-verified: se_robust
// != se_iid, real numbers, not a no-op). vce(cluster ...)/cluster() is a
// SILENT NO-OP for both — live-verified on the compiled engine directly:
// neither CLI's usage banner even lists a --cluster flag, yet passing
// `--vce cluster` to EITHER is accepted with exit 0 and produces the SAME
// numbers as the non-robust default (xtlogit's case is worse: se==se_iid,
// not even se_robust) — honest reject rather than forward a flag that would
// silently claim a clustered SE the engine never computed. noconstant/
// level() are NEVER forwarded on either CLI (same stdout/exit-code
// masking-bug shape as BS-073/074 above, live-verified on both).
ojson runXtPoissonLogit(const std::string& cobraCmd, const std::string& stataCmd,
                         const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  bool fePresent = false, rePresent = false, paPresent = false;
  eval::OptValue feVal;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "fe") {
      fePresent = true;
      feVal = v;
    }
    if (k == "re") rePresent = true;
    if (k == "pa") paPresent = true;
  }
  bool isFe = fePresent && feVal.isBool && feVal.boolValue;
  if (!isFe) {
    std::string which = rePresent ? ",re" : (paPresent ? ",pa" : "the default (random-effects)");
    return engineErrorBlockM4(
        "This version of PIET supports only `" + stataCmd + " ,fe` (conditional fixed-effects "
        "MLE). `" + stataCmd + "` with " + which + " is not available — the engine has no "
        "random-effects or population-averaged " + stataCmd + " estimator.",
        "unsupported_option");
  }

  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: `" + stataCmd + "` requires a dependent variable and at least one "
        "regressor",
        "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  // BUG-657: xtlogit.ado / xtpoisson.ado call `_xt, i()`, whose refusal is
  // "must specify panelvar; use xtset" r(459) (as xtreg: w2_eststo_prefix.log:
  // 437-441); was unsupported_option = r(198).
  if (!session.panelvar.has_value() || session.panelvar->empty()) {
    return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset", std::nullopt,
                                  std::nullopt, std::nullopt, "r(459)"));
  }

  std::vector<std::string> argv = {cobraCmd, "--y",    y,
                                    "--x",    joinCommaM4(xs), "--ivar", *session.panelvar};

  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // offset() — xtpoisson only (cobra's xtpois CLI has --offset; xtlogit's
  // does not, per its own usage banner — an offset() on xtlogit falls
  // through to the leftover unknown-option reject below).
  if (cobraCmd == "xtpois") {
    std::optional<std::string> offsetVar;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "offset" && !v.isBool) offsetVar = v.strValue;
    }
    if (offsetVar.has_value() && !offsetVar->empty()) {
      argv.push_back("--offset");
      argv.push_back(*offsetVar);
    }
  }

  // FU-23 (BUG-756): the cluster VCE is refused by Stata itself for both ,fe
  // estimators, in every spelling but one, and this loop read `cluster` only
  // when valued -- so a bare `cluster` fitted the iid model at rc 0.
  // w2l_vce_shape.log, r(198) unless noted (xtpoisson message / xtlogit message):
  //   cluster(id), cluster(id2), cluster(id id2)  "option cluster() not allowed" (both)
  //   bare `cluster`                "option cluster not allowed" / "cluster invalid"
  //   vce(cluster id), vce(cluster) "option cluster not allowed" / "vcetype cluster not allowed"
  //   vce(cluster id id2)           "multiway cluster VCE not allowed" (both)
  //   robust(x1)                    "option robust() not allowed" / "robust(x1) invalid"
  //   vce(robust x1)                "option x1 not allowed" / "vcetype robust not allowed"
  //   xtlogit vce(robust)           "vcetype robust not allowed" (xtlogit_fe_ctl_vcerobust;
  //                                 xtpoisson_fe_ctl_vcerobust fits, rc 0)
  //   EMPTY `cluster()`             rc 0, IGNORED: the default fit (xtpoisson e(vce) oim;
  //                                 xtlogit runs as clogit, e(cmd) clogit)
  // The engine could not honour a cluster VCE anyway (its conditional-FE CLIs
  // accept `--vce cluster` and silently compute the non-clustered SE), so no
  // refusal here hides a capability.
  const bool isXtlogit = (cobraCmd == "xtlogit");
  const VceShapesM4b vceShape = readVceShapesM4b(parsed);
  if (vceShape.cluster == OptShapeM4b::kBare) {
    return stataRefusalM4b(isXtlogit ? "cluster invalid" : "option cluster not allowed", 198);
  }
  if (vceShape.cluster == OptShapeM4b::kOne || vceShape.cluster == OptShapeM4b::kMany) {
    return stataRefusalM4b("option cluster() not allowed", 198);
  }
  if (vceShape.robust != OptShapeM4b::kAbsent && vceShape.robust != OptShapeM4b::kBare) {
    return stataRefusalM4b(isXtlogit ? "robust(" + vceShape.robustRaw + ") invalid"
                                     : std::string("option robust() not allowed"),
                           198);
  }
  if (vceShape.vceType == "cluster") {
    if (vceShape.vceRest.size() > 1) return stataRefusalM4b("multiway cluster VCE not allowed", 198);
    return stataRefusalM4b(isXtlogit ? "vcetype cluster not allowed" : "option cluster not allowed",
                           198);
  }
  const bool vceRobustFamily = vceShape.vceType == "robust" || vceShape.vceType == "hc1";
  if (vceRobustFamily && !vceShape.vceRest.empty()) {
    return stataRefusalM4b(isXtlogit ? std::string("vcetype robust not allowed")
                                     : "option " + vceShape.vceRest[0] + " not allowed",
                           198);
  }
  // xtlogit ,fe takes no robust VCE at all. The bare `robust` flag is the same
  // request as vce(robust) in this handler (and in Stata's vce parsing); it has
  // no arm of its own in the oracle, so refusing it rests on vce(robust)'s.
  if (isXtlogit && (vceRobustFamily || vceShape.robust == OptShapeM4b::kBare)) {
    return stataRefusalM4b("vcetype robust not allowed", 198);
  }
  bool sawRobustFlag = false;
  std::optional<std::string> vceVal;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !vceVal.has_value()) {
      vceVal = v.isBool ? std::string() : v.strValue;
    }
    if (k == "robust" && v.isBool && v.boolValue) sawRobustFlag = true;
  }
  bool wantsRobust =
      sawRobustFlag ||
      (vceVal.has_value() && (canonicalVceType(*vceVal, nullptr) == "robust" ||
                              canonicalVceType(*vceVal, nullptr) == "hc1"));
  if (vceVal.has_value() && !vceVal->empty()) {
    const std::string lv = canonicalVceType(*vceVal, nullptr);
    if (lv != "robust" && lv != "hc1" && lv != "iid") {
      if (!unimplementedVceType(*vceVal).empty()) {
        return engineErrorBlockM4("Option `vce(" + unimplementedVceType(*vceVal) + ")` is a valid Stata option that this version of PIET does not implement", "unsupported_option");
      }
      return engineErrorBlockM4("Unknown vce type: " + *vceVal, "unsupported_option");
    }
  }
  if (wantsRobust) {
    argv.push_back("--vce");
    argv.push_back("robust");
  }

  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      return engineErrorBlockM4(
          "Option `" + k + "` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (conditional-FE MLE has no separate constant to "
          "suppress — live-verified the engine's CLI hard-errors while still masking a WRONG "
          "result to stdout, same precedent as the other D3 estimators above).",
          "unsupported_option");
    }
    if (k == "level" && !v.isBool) {
      // BUG-274, xtpois half: BUG-202 gave cobra's `xtpois` CLI a real `--level`
      // (live-verified on this tree's engine: `--level 90` emits per-coef
      // ci_lo/ci_hi), so the old shared refusal's premise expired for xtpoisson
      // and it forwards, same idiom as runTobit/runNbreg. The xtlogit half of
      // the premise is still TRUE — re-measured on the same build: `cobra
      // xtlogit --level 90` hard-errors "unrecognized option" (exit 3) with a
      // CLEAN empty stdout (the old "masking gap" wording was stale too: no
      // masked result prints), so xtlogit keeps refusing, now for the measured
      // reason.
      if (cobraCmd == "xtpois") {
        argv.push_back("--level");
        argv.push_back(v.strValue);
        continue;
      }
      return engineErrorBlockM4(
          "Option `level` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (the engine's xtlogit CLI has no --level surface — "
          "live-verified: `cobra xtlogit --level` hard-errors 'unrecognized option', exit 3, "
          "with a clean empty stdout).",
          "unsupported_option");
    }
  }

  // BUG-233: the base set is the file-scope literal; `offset` stays conditional
  // here because xtlogit must keep REJECTING it. The registered vocabulary is
  // the union of the two, which is correct for canonicalization: resolution
  // only rewrites a key to a canonical spelling, and the reject net below is
  // still the authority on whether that spelling is allowed for THIS command.
  std::unordered_set<std::string> known = kXtPoissonLogitBase;
  if (cobraCmd == "xtpois") known.insert("offset");
  if (auto err = eval::rejectUnknownOptions(parsed.opts, known)) {
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

  ojson block = cobraToEstimationM4(raw, cobraCmd, &session);
  block["estimator"] = stataCmd + "_fe";
  block["depvar"] = y;
  fillCoeftableStatsM4(block);
  static const char* kXtFields[] = {"n_groups", "n_dropped_const", "n_dropped_missing",
                                    "n_dropped_size"};
  for (const char* k : kXtFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }

  LastModel lm;
  lm.cobraCmd = cobraCmd;  // "xtpois"/"xtlogit" — already anticipated in margins' kModelMap.
  // e(cmd) is Stata's name, not the engine's: `xtpoisson` (w2l_vce_shape.log
  // xtpoisson_fe_ctl_vcerobust, where main published `xtpois`), and `clogit`
  // when xtlogit ,fe ran with an empty cluster() (xtlogit_fe_empty_cluster).
  lm.stataCmd = (isXtlogit && vceShape.cluster == OptShapeM4b::kEmpty) ? std::string("clogit")
                                                                       : stataCmd;
  lm.y = y;
  lm.x = xs;
  lm.ivar = *session.panelvar;
  if (wantsRobust) lm.vcov = "hc1";
  lm.depvar = y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  return block;
}

ojson runXtPoisson(const eval::ParsedCommand& parsed, SessionState& session) {
  return runXtPoissonLogit("xtpois", "xtpoisson", parsed, session);
}

ojson runXtLogitFe(const eval::ParsedCommand& parsed, SessionState& session) {
  return runXtPoissonLogit("xtlogit", "xtlogit", parsed, session);
}

// drdid (BS-076): the classic 2-period/2-group doubly-robust DiD estimator
// -> the engine's csdid --method dripw machinery, ONE (g,t) cell. Oracle:
// tests/validate/drdid2x2.{do,log} — `drdid Y x1 x2, ivar(id) time(t)
// treatment(treat) dripw` (the fixture's EXACT invocation; this is the ONLY
// drdid surface wired — other method()s (reg/ipw/std_ipw/trad) and the
// bare/no-`dripw` form have no oracle coverage and honest-reject).
//
// CSDID_CLEANROOM_WALL (docs/legal/CSDID_CLEANROOM_WALL.md) compliance:
// this wiring touches ZERO cobra-side code — src/estimators/csdid.cpp's
// estimator runs completely unmodified, exactly as `csdid`/`estat` already
// call it. The ONLY new logic is PIET-side: constructing a synthetic `gvar`
// column from the fixture's own treatment()/time() variables
// (treatment()!=0 -> gvar=max(time), the post period; treatment()==0 ->
// gvar=0, CS21's "never-treated" sentinel — csdid_dripw_run only ever forms
// a cohort from gvar>0 rows, so 0 is a safe comparison-group marker
// regardless of how the panel's own time variable happens to be coded) via
// cobra's GENERIC `eval --gen` data-prep verb (the same verb `generate`/
// `egen` already route through elsewhere in this codebase — not
// csdid-specific), then calling the UNMODIFIED `cobra csdid --method dripw
// --comparison never` on the augmented file.
//
// LIVE-VERIFIED against the oracle (drdid2x2.dta, 600 obs, 300 units x 2
// periods): ATT = 2.0561083418597157 (oracle 2.05610834185971), SE =
// 0.12400508894658192 (oracle .124005088944043) — matches to ~1e-13
// relative, i.e. this construction is mathematically exact, not an
// approximation.
//
// Restricted to the fixture's own 2-period design (time() must have EXACTLY
// 2 distinct values) — matching the command's own name ("2x2") and its real
// Stata namesake, which is likewise a strictly-2-period estimator.
namespace {

// Reads a numeric (f64) column's distinct non-missing values and max —
// mirrors computeYMinMaxM4's direct-Datastore-read pattern above (BS-069),
// minus the if-filter machinery (the caller here already staged the exact
// sample it wants read into `ds`).
bool numericColumnStatsM4(Datastore& ds, const std::string& name, std::set<double>& distinct,
                          double& outMax, ojson& errBlockOut, const std::string& ctx) {
  if (!ds.has(name) || ds.dtype(name) != "f64") {
    errBlockOut =
        engineErrorBlockM4(ctx + ": `" + name + "` is not a numeric variable", "engine_error");
    return false;
  }
  const cecon::Column& c = ds.col(name);
  const double* d = c.f64();
  int64_t count = c.nbytes / static_cast<int64_t>(sizeof(double));
  double mx = -std::numeric_limits<double>::infinity();
  bool any = false;
  for (int64_t i = 0; i < count; ++i) {
    double v = d[i];
    if (std::isnan(v)) continue;
    any = true;
    if (v > mx) mx = v;
    distinct.insert(v);
  }
  if (!any) {
    errBlockOut =
        engineErrorBlockM4(ctx + ": `" + name + "` has no non-missing values", "engine_error");
    return false;
  }
  outMax = mx;
  return true;
}

}  // namespace

ojson runDrdid(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.empty()) {
    return engineErrorBlockM4("syntax_error: drdid requires a dependent variable", "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::optional<std::string> ivar, timeVar, treatVar;
  bool sawDripw = false;
  static const std::set<std::string> kOtherMethods = {"reg", "ipw", "std_ipw", "trad"};
  std::vector<std::string> otherMethodsSeen;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "ivar" && !v.isBool) ivar = v.strValue;
    if (k == "time" && !v.isBool) timeVar = v.strValue;
    if (k == "treatment" && !v.isBool) treatVar = v.strValue;
    if (k == "dripw" && v.isBool && v.boolValue) sawDripw = true;
    if (kOtherMethods.count(k) && v.isBool && v.boolValue) otherMethodsSeen.push_back(k);
  }
  if (!ivar.has_value() || ivar->empty()) {
    return engineErrorBlockM4(
        "drdid requires `ivar(varname)` — e.g. `drdid y x, ivar(id) time(t) treatment(d) dripw`.",
        "unsupported_option");
  }
  if (!timeVar.has_value() || timeVar->empty()) {
    return engineErrorBlockM4(
        "drdid requires `time(varname)` — e.g. `drdid y x, ivar(id) time(t) treatment(d) dripw`.",
        "unsupported_option");
  }
  if (!treatVar.has_value() || treatVar->empty()) {
    return engineErrorBlockM4(
        "drdid requires `treatment(varname)` — e.g. `drdid y x, ivar(id) time(t) treatment(d) "
        "dripw`.",
        "unsupported_option");
  }
  if (!otherMethodsSeen.empty()) {
    return engineErrorBlockM4(
        "drdid, " + otherMethodsSeen[0] + ": only the doubly-robust IPW estimator (`dripw`) is "
        "wired to the engine in this version of PIET — reg/ipw/std_ipw/trad have no engine "
        "equivalent.",
        "unsupported_option");
  }
  if (!sawDripw) {
    return engineErrorBlockM4(
        "drdid requires the explicit `dripw` option in this version of PIET (the engine only "
        "implements the doubly-robust IPW estimator) — e.g. `drdid y x, ivar(id) time(t) "
        "treatment(d) dripw`.",
        "unsupported_option");
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabDrdid.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
  }

  // If-filter first (mirrors applyIfFilterM4's established idiom, BS-069)
  // so the gvar construction below reads/derives from the SAME sample cobra
  // will actually estimate on.
  ojson ifErr;
  auto basePathOpt = applyIfFilterM4(session, *session.currentHbin(), parsed.ifExpr, ifErr);
  if (!basePathOpt.has_value()) return ifErr;
  std::string basePath = *basePathOpt;

  bool madeIfTemp = (basePath != *session.currentHbin());
  std::optional<Datastore> tempDs;
  Datastore* ds = nullptr;
  if (madeIfTemp) {
    tempDs.emplace(basePath);
    ds = &*tempDs;
  } else {
    ds = session.datastore();
  }
  if (ds == nullptr) {
    if (madeIfTemp) cecon::native_unlink(basePath);
    return engineErrorBlockM4("drdid: could not read the current dataset", "engine_error");
  }

  std::set<double> timeLevels;
  double tmax = 0;
  ojson statErr;
  if (!numericColumnStatsM4(*ds, *timeVar, timeLevels, tmax, statErr, "drdid")) {
    if (madeIfTemp) cecon::native_unlink(basePath);
    return statErr;
  }
  if (timeLevels.size() != 2) {
    if (madeIfTemp) cecon::native_unlink(basePath);
    return engineErrorBlockM4(
        "drdid: `" + *timeVar + "` has " + std::to_string(timeLevels.size()) +
        " distinct values — this version of PIET's drdid only supports the classic two-period "
        "(2x2) panel design (exactly 2 time() values), matching the command's own real-Stata "
        "restriction.",
        "unsupported_option");
  }
  if (!ds->has(*treatVar) || ds->dtype(*treatVar) != "f64") {
    if (madeIfTemp) cecon::native_unlink(basePath);
    return engineErrorBlockM4("drdid: `" + *treatVar + "` is not a numeric variable", "engine_error");
  }

  char tmaxBuf[64];
  std::snprintf(tmaxBuf, sizeof(tmaxBuf), "%.17g", tmax);
  static const char* kGvarName = "__piet_drdid_gvar__";
  std::string genExpr =
      std::string(kGvarName) + "=cond(" + *treatVar + "!=0," + tmaxBuf + ",0)";
  std::string gvarPath = nextPathM4(session.workdir(), "drdid_gvar");
  std::vector<std::string> genArgs = {"eval", "--data", basePath, "--gen", genExpr, "--out",
                                      gvarPath};
  try {
    runCobraJsonM4(genArgs);
  } catch (const CobraError& ce) {
    if (madeIfTemp) cecon::native_unlink(basePath);
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  if (madeIfTemp) cecon::native_unlink(basePath);

  std::vector<std::string> argv = {"csdid",  "--y", y,        "--ivar", *ivar,
                                    "--time", *timeVar, "--gvar", kGvarName};
  if (!xs.empty()) {
    argv.push_back("--x");
    argv.push_back(joinCommaM4(xs));
  }
  argv.push_back("--method");
  argv.push_back("dripw");
  argv.push_back("--comparison");
  argv.push_back("never");
  argv.push_back("--data");
  argv.push_back(gvarPath);
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = csdidToEstimationM4(raw, "simple");
  block["estimator"] = "drdid";
  block["depvar"] = y;
  // BUG-112(5): the drdid ATET is a SINGLE-row table (csdid simple aggregation) and
  // that lone row is renamed to the treat var. Guard on size==1 so that if the
  // underlying aggregation ever grows to multiple rows, we don't silently mislabel
  // row 0 by position — the rename only applies while the one-row invariant holds.
  if (block.contains("coeftable") && block["coeftable"].is_array() &&
      block["coeftable"].size() == 1) {
    block["coeftable"][0]["name"] = *treatVar;  // matches Stata's own ATET table row label.
  }

  // NOT cacheCsdidModel() (above): that helper hardcodes lm.dataPath =
  // session.currentHbin(), which is the ORIGINAL dataset — missing the
  // synthesized gvar column. A later `estat simple/event/calendar/group/
  // pretrend` re-run needs `gvarPath` (this drdid call's own augmented
  // file), or it would hard-error "column not found" instead of
  // reproducing this result.
  LastModel lm;
  lm.cobraCmd = "csdid";  // it really IS the same underlying engine call — `estat` after `drdid`
                          // keeps working, same as after a genuine `csdid` call.
  lm.y = y;
  lm.x = xs;
  lm.depvar = y;
  lm.dataPath = gvarPath;
  lm.ivar = *ivar;
  lm.timevar = *timeVar;
  lm.gvar = std::string(kGvarName);
  lm.method = std::string("dripw");
  lm.comparison = std::string("never");
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  return block;
}


}  // namespace piet::handlers
