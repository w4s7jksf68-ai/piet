// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// time fill.
#include "data_io_internal.h"
namespace piet::handlers {
using namespace data_io_detail;


// ---------------------------------------------------------------------------
// tsfill (BS-081, BETA campaign Phase D batch D4): `tsfill [, full]` -> cobra
// tsfill --timevar T [--panelvar ID] [--delta D] --out new.hbin. Requires a
// prior `tsset T` (or `xtset ID T`) — reads session.timevar/panelvar/delta,
// the SAME tsset/xtset spine BS-075's xtpoisson/xtlogit read
// session.panelvar from (estimation_m4.cpp's runXtPoissonLogit) and this
// batch's own newey/prais/dfuller/pperron read session.timevar from
// (estimation_m4.cpp, same file as those D4 estimators — this one lives
// here instead because it is a data-mutation verb like gsort/order/recode
// above, not an estimation command). Oracle D4f
// (internal capture d45-oracle-stata.log):
// after `drop if mod(t,7)==0` (48 of 336 rows dropped — INCLUDING t=336
// itself, since 336 is itself a multiple of 7), `tsfill` restores the full
// grid from the SURVIVING min/max t, i.e. 1..335 (not 1..336 — tsfill only
// fills gaps WITHIN the observed range, it never extrapolates past the
// highest t still present) — live-verified byte-for-byte: 335 rows out,
// matching the oracle's own `count` = 335 exactly, with every OTHER column
// (income, etc.) missing/NaN on the newly-filled rows (also live-verified).
// BUG-897: `full` (every panel over the overall min..max of the
// time variable, on the delta grid) is cobra tsfill --full; it was refused by
// name before the engine had it. tsfill's own refusals carry its rc
// (w2w_tsset_tsfill.log rf_tsfill_none r(111), rf_tsfill_if r(101),
// rf_tsfill_full_tmiss r(451)); the registry's allowIf=false row is
// descriptive only, so an `if` reached this arm and was silently ignored.
ojson runTsfill(const eval::ParsedCommand& parsed, SessionState& session) {
  auto refuse = [](const std::string& msg, int rc) {
    return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                  "r(" + std::to_string(rc) + ")"));
  };
  // `syntax [, Full]` runs first.
  if (!parsed.varlist.empty()) return refuse("varlist not allowed", 101);
  if (parsed.ifExpr.has_value()) return refuse("if not allowed", 101);
  if (parsed.inRange.has_value()) return refuse("in range not allowed", 101);
  if (parsed.weightType.has_value()) return refuse("weights not allowed", 101);
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file).
  const std::unordered_set<std::string>& kKnown = kVocabTsfill.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
  }
  bool full = false;
  for (const auto& [k, v] : parsed.opts)
    if (k == "full" && v.isBool && v.boolValue) full = true;
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  // tsfill declares no further options here.
  if (!session.timevar.has_value() || session.timevar->empty()) {
    return refuse("time variable not set, use tsset varname ...", 111);
  }
  if (full) {
    Datastore* ds = session.datastore();
    if (ds->has(*session.timevar) && ds->dtype(*session.timevar) == "f64") {
      const double* tv = ds->col(*session.timevar).f64();
      const int64_t n = ds->nRows();
      for (int64_t i = 0; i < n; ++i)
        if (std::isnan(tv[i]))
          return refuse("timevar (" + *session.timevar +
                            ") may not contain missing values when option full is specified",
                        451);
    }
  }

  std::vector<std::string> argv = {"tsfill", "--data", *session.currentHbin(), "--timevar",
                                    *session.timevar};
  if (session.panelvar.has_value() && !session.panelvar->empty()) {
    argv.push_back("--panelvar");
    argv.push_back(*session.panelvar);
  }
  if (session.delta.has_value()) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", *session.delta);
    argv.push_back("--delta");
    argv.push_back(buf);
  }
  if (full) argv.push_back("--full");
  std::string outPath = nextPath(session.workdir(), "tsfill");
  argv.push_back("--out");
  argv.push_back(outPath);

  CobraRunResult cr;
  {
    // BUG-394 (2026-09-02): onto the resident plane. tsfill was one of eight verbs
    // still writing a whole --out file per call with no note saying why; its engine
    // command already uses the shared plane (acquire_data / commit_frame), so the
    // scope is all it needed.
    SessionState::ResidentEligible residentTsfill(session);
    cr = runCobra(argv);
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    // tsfill.ado's own check after a declaration the data no longer satisfies
    // (rows appended since `tsset`): r(451), as tsset's.
    if (cobraErrorStr(ce).find("repeated time value") != std::string::npos)
      return refuse(session.panelvar.has_value() && !session.panelvar->empty()
                        ? "repeated time values within panel"
                        : "repeated time values in sample",
                    451);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "tsfill");
}
}
