// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Vector time series.
#include "piet/handlers/estimation.h"
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
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/inrange_filter.h"
#include "piet/settings.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "matrix/lapack_dsyevr.h"  // BUG-1482: the clustered V's rank (clusteredVRankM4)
#include "estimation_internal.h"

#include "estimation_families_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace fit_detail {}
using namespace fit_detail;
using namespace estimation_vocab;

ojson runVar(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // BUG-657: var.ado runs `tsset, noquery` -> `_ts`, whose refusal is "time
  // variable not set, use tsset varname ..." r(111) (the time-series helper); was
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


}  // namespace piet::handlers
