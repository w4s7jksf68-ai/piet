// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Categorical estimation.
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

// `ologit depvar indepvars [if] [, vce(robust|cluster V)]` and its probit twin.
ojson runOrdered(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                 SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildP7MleArgv(cobraCmd, parsed, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  b.argv.push_back("--emit-v");  // BUG-1196: the fit's e(V), posted below
  ojson raw;
  ojson block = runP7Mle(cobraCmd, b.argv, session, &raw);
  // BUG-1196: Stata's e(b) is the depvar's equation, then the cutpoints
  // (StataBE 19.5 `yo:x1 yo:x2 /cut1 /cut2`); e(V) is the fit's, in that order.
  if (raw.is_object() && raw.contains("e_V") && raw.contains("coefs") && raw.contains("cuts") &&
      raw["cuts"].is_array()) {
    std::vector<AncillaryEbColM4> cols;
    bool ok = depvarEqColsM4(parsed.varlist[0], raw["coefs"], "b", cols);
    for (const auto& c : raw["cuts"]) {
      if (!ok || !c.is_object() || !c.contains("name") || !c["name"].is_string() ||
          !c.contains("b") || !c["b"].is_number()) {
        ok = false;
        break;
      }
      const std::string nm = c["name"].get<std::string>();
      cols.push_back({nm, nm, c["b"].get<double>()});
    }
    if (ok) (void)postAncillaryEbM4(session, raw, cols);
  }
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
  b.argv.push_back("--emit-v");  // BUG-1196: the fit's e(V), posted below
  ojson raw;
  ojson block = runP7Mle("mlogit", b.argv, session, &raw);
  // BUG-1196: Stata's e(b) has one equation per outcome, ascending, named by the
  // outcome's value; the base outcome's columns are `o.` zeros, as are its rows
  // and columns of e(V) (StataBE 19.5 `1:o.x1 1:o.x2 1:o._cons 2:x1 2:x2 2:_cons
  // 3:x1 3:x2 3:_cons`). A labeled outcome, or one whose value is not a
  // nonnegative integer, names its equation otherwise; nothing is posted then.
  const std::string& y = parsed.varlist[0];
  const auto lbl = session.varLblname.find(y);
  if (raw.is_object() && raw.contains("e_V") && raw.contains("equations") &&
      raw["equations"].is_array() && (lbl == session.varLblname.end() || lbl->second.empty())) {
    std::vector<std::string> coefNames;  // a non-base equation's names, engine spelling
    for (const auto& e : raw["equations"])
      if (e.is_object() && !e.value("base", false) && e.contains("coefs") && e["coefs"].is_array()) {
        for (const auto& c : e["coefs"])
          if (c.is_object() && c.contains("name") && c["name"].is_string())
            coefNames.push_back(c["name"].get<std::string>());
        break;
      }
    std::vector<AncillaryEbColM4> cols;
    bool ok = !coefNames.empty();
    for (const auto& e : raw["equations"]) {
      const double v = e.is_object() && e.contains("outcome") && e["outcome"].is_number()
                           ? e["outcome"].get<double>()
                           : -1.0;
      if (!ok || !(v >= 0.0 && v < 1e9 && v == std::floor(v))) {
        ok = false;
        break;
      }
      char eq[64];
      std::snprintf(eq, sizeof(eq), "%.17g", v);  // the engine's `e_V` spelling of the outcome
      const bool isBase = e.value("base", false);
      ojson coefs = isBase ? ojson::array() : e.value("coefs", ojson::array());
      if (isBase)
        for (const auto& n : coefNames) coefs.push_back({{"name", n}, {"b", 0.0}});
      std::vector<AncillaryEbColM4> ec;  // `y:name`, re-keyed to the outcome's equation
      if (!depvarEqColsM4(y, coefs, "b", ec)) {
        ok = false;
        break;
      }
      for (auto& c : ec) {
        c.stata = std::string(eq) + (isBase ? ":o." : ":") + c.stata.substr(y.size() + 1);
        c.engine = isBase ? std::string() : std::string(eq) + ":" + c.engine;
        cols.push_back(std::move(c));
      }
    }
    if (ok) (void)postAncillaryEbM4(session, raw, cols);
  }
  return block;
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


}  // namespace piet::handlers
