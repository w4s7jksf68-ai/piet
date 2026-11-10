// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Did two period.
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

namespace extended_fit_detail {

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

  return drdidFitCoreM4(session, y, xs, *ivar, *timeVar, *treatVar, *treatVar, parsed.ifExpr);
}

namespace extended_fit_detail {

// runDrdid's fit after its option parsing, shared with csdid's failure path
// (runCsdid, cluster()==ivar): `treatExpr` is the treatment as an expression
// (a variable name for drdid itself) and `treatName` is the name the ATET
// column carries (`ATET:r1vs0.<treatName>`, drdid).
ojson drdidFitCoreM4(SessionState& session, const std::string& y,
                     const std::vector<std::string>& xs, const std::string& ivarName,
                     const std::string& timeVarName, const std::string& treatExpr,
                     const std::string& treatName, const std::optional<std::string>& ifExpr) {
  const std::optional<std::string> ivar = ivarName;
  const std::optional<std::string> timeVar = timeVarName;
  const bool treatIsVar = treatExpr == treatName;
  // If-filter first (mirrors applyIfFilterM4's established idiom, BS-069)
  // so the gvar construction below reads/derives from the SAME sample cobra
  // will actually estimate on.
  ojson ifErr;
  auto basePathOpt = applyIfFilterM4(session, *session.currentHbin(), ifExpr, ifErr);
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
  if (treatIsVar && (!ds->has(treatExpr) || ds->dtype(treatExpr) != "f64")) {
    if (madeIfTemp) cecon::native_unlink(basePath);
    return engineErrorBlockM4("drdid: `" + treatExpr + "` is not a numeric variable", "engine_error");
  }

  char tmaxBuf[64];
  std::snprintf(tmaxBuf, sizeof(tmaxBuf), "%.17g", tmax);
  static const char* kGvarName = "__piet_drdid_gvar__";
  std::string genExpr =
      std::string(kGvarName) + "=cond((" + treatExpr + ")!=0," + tmaxBuf + ",0)";
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
  // that lone row is renamed. Guard on size==1 so that if the underlying
  // aggregation ever grows to multiple rows, we don't silently mislabel row 0 by
  // position — the rename only applies while the one-row invariant holds.
  // drdid-ematrix-post: Stata's name is `ATET:r1vs0.<treat>` (drdid;
  // w2ag log:1810 ag_drdid `drdid|120|1|ATET:r1vs0.treat|dripw|`), so `_b[treat]`
  // is r(111) (log:1821) and `_b[ATET:r1vs0.treat]` answers.
  const std::string atetName = "ATET:r1vs0." + treatName;
  std::optional<CsdidPostingM4> drPost;
  if (block.contains("coeftable") && block["coeftable"].is_array() &&
      block["coeftable"].size() == 1) {
    block["coeftable"][0]["name"] = atetName;
    // The one cell's b and V (the engine's attgt cell 1; the simple aggregate
    // of one cell is that cell).
    if (auto cells = csdidPostingM4(raw, "attgt"); cells.has_value() && !cells->names.empty()) {
      CsdidPostingM4 p;
      p.names = {atetName};
      p.b = {cells->b[0]};
      p.V = {cells->V[0]};
      drPost = p;
    }
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
  // drdid-ematrix-post: e(cmd) drdid (w2ag log:1810); cobraCmd stays "csdid" so
  // estat keeps replaying the engine call. No e(vce)/e(vcetype) is posted.
  lm.stataCmd = "drdid";
  lm.vcov = "";
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  if (drPost.has_value()) postCsdidPostingM4(session, *drPost);
  session.eval.setEString("method", "dripw");
  session.noteEOnFit("method");  // dropped by the next install
  return block;
}

}  // namespace


}  // namespace piet::handlers
