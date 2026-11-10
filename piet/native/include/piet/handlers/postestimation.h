// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command entry points: test, lincom, nlcom, margins, predict, estat, lrtest.
// Implemented under piet/native/src/postestimation/.
#ifndef PIET_NATIVE_HANDLERS_POSTESTIMATION_H
#define PIET_NATIVE_HANDLERS_POSTESTIMATION_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

// `test EXPR` (joint or constraint Wald test), `lincom EXPR`, `nlcom [LABEL:]
// EXPR` — all reconstruct the cobra call from session.lastModel(). Returns
// "No previous estimation result" if lastModel is unset.
nlohmann::ordered_json runTestCmd(const std::string& expr, SessionState& session);
// `test ..., accumulate`: the restrictions of the previous `test` on the SAME
// estimation are tested jointly with these (session state; see runTestCmd).
nlohmann::ordered_json runTestCmd(const std::string& expr, SessionState& session,
                                  bool accumulate);
// BUG-367: Stata `testparm varlist` — the joint zero test written the way a
// researcher writes it. It is `test`'s varlist form, not a second estimator: the
// only thing it adds is expanding wildcards and `i.var` against the LAST MODEL's
// coefficient names (not the dataset's variables — a coefficient is what can be
// constrained), after which it delegates to runTestCmd unchanged.
nlohmann::ordered_json runTestparm(const std::vector<std::string>& varlist,
                                   SessionState& session);
nlohmann::ordered_json runLincom(const std::string& expr, SessionState& session,
                                 const std::vector<std::pair<std::string, eval::OptValue>>& opts = {});
nlohmann::ordered_json runNlcom(const std::string& expr, SessionState& session,
                                const std::vector<std::pair<std::string, eval::OptValue>>& opts = {});

// `margins [marginlist] [, dydx(VAR...) | at(...) atmeans post]`. `marginlist` is
// the command's positional terms (the dispatcher's parsed.varlist): one factor
// variable of a least-squares fit gives the predictive margin at each of its
// levels (FU-94). Every served form replaces r() with r(b)/r(V)/r(N) (FU-179).
nlohmann::ordered_json runMargins(const std::vector<std::string>& marginlist,
                                   const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   SessionState& session);

// What margins needs from a fit to answer over at() points, a marginlist factor
// or atmeans from the fit's own e(b)/e(V) (cecon::margins_grid, posthest.h):
// built by runMargins for a least-squares or logit/probit fit and by
// runGlmMargins for a glm. `names` are e(b)'s column names (no equation prefix,
// the engine's spelling or Stata's), `data` every variable a name's atoms read,
// over the e(sample) rows. `dfR` is e(df_r) when the fit's margins table is
// t-referenced (regress, areg), NaN for z (xtreg, reghdfe, logit, glm).
struct MarginsGridFit {
  std::vector<std::string> names;
  std::vector<double> b, V;  // k, k*k
  int link = 0;              // cecon::GlmLinkFn
  int64_t n = 0;
  std::map<std::string, std::vector<double>> data;
  std::vector<double> weights;  // n fit weights, or empty
  bool fweight = false;         // r(N) is the weight sum
  double dfR = 0.0;
  bool tRef = false;
  bool cluster = false;
  std::string depvar, dataPath;
};

// Serves `margins [factor] [, at() atmeans dydx() post]` through the grid. nullopt
// = a form the grid does not take (dydx(*), a marginlist with at(), several
// marginlist terms); the caller decides whether that is a refusal.
std::optional<nlohmann::ordered_json> runMarginsGrid(
    SessionState& session, const MarginsGridFit& fit, const std::vector<std::string>& marginlist,
    const std::vector<std::pair<std::string, eval::OptValue>>& opts);

// margins' r() for `margins, dydx() [post]` after a glm without an interaction
// (runGlmMargins' own path): r(N) = the block's n_obs, r(b)/r(V) over `names`
// (a factor's base level included, 0 with variance 0), r(table) from
// `tableRows` (one row per name; a base row's se/t/p/ci null), and the
// declared set of the runMargins grid (w2v_margins_siblings C8_gamif_dydx_x).
void postGlmDydxRClass(SessionState& session, const nlohmann::ordered_json& block,
                       const std::vector<std::string>& names, const std::vector<double>& b,
                       const std::vector<std::vector<double>>& V, const nlohmann::ordered_json& tableRows,
                       const std::string& xvars, bool cluster);

// `predict [NEWVAR] [, xb|resid|residuals|fe]` — reconstructs the last model
// and calls cobra predict; unsupported statistics (cooksd/leverage/hat/...)
// are rejected explicitly (mirrors the oracle's _UNSUPPORTED_PREDICT_STATS
// allow-list — never silently defaults to xb).
// BUG-516: computes on the LIVE frame and adds exactly one column to it (it used
// to adopt the estimation-time frame as the new head). BUG-517: `ifExpr` is the
// command's `if` qualifier -- rows that fail it get missing, as in Stata.
nlohmann::ordered_json runPredict(const std::vector<std::string>& varlist,
                                   const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   SessionState& session,
                                   const std::optional<std::string>& ifExpr = std::nullopt);

// `estat SUBCMD [...]` — two families: (i) hettest/ovtest/vif/dwatson/
// bgodfrey/archlm rewrite to the equivalent top-level diagnostic command
// (injecting lastModel's y+x when the estat call has no varlist of its own);
// (ii) simple/event/calendar/group/pretrend read back the cached csdid
// result. `rawVarlistAfterSub` is the token list AFTER the subcommand word
// (e.g. `estat hettest x1 x2` -> ["x1","x2"]).
nlohmann::ordered_json runEstat(const std::string& subcmd,
                                 const std::vector<std::string>& rawVarlistAfterSub,
                                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                 const std::optional<std::string>& ifExpr, SessionState& session);

// `lrtest name1 [name2] [, df() force]` -- sets r(chi2) r(df) r(p).
nlohmann::ordered_json runLrtest(const eval::ParsedCommand& parsed, SessionState& session);

}  // namespace piet::handlers

#endif  // PIET_NATIVE_HANDLERS_POSTESTIMATION_H
