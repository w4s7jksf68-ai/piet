// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "types.h"
#include "piet/handlers/estimation.h"
namespace piet {
namespace handlers {
ojson runSummarizeAweight(const std::optional<std::vector<std::string>>& varlist, bool detail,
                          SessionState& session, const std::optional<std::string>& ifExpr,
                          const std::optional<std::string>& weightVar,
                          const std::optional<std::string>& inRange,
                          const char* weightFlag = "--aweight");
ojson runMean(const eval::ParsedCommand& parsed, SessionState& session);
ojson runProportion(const eval::ParsedCommand& parsed, SessionState& session);
// BUG-1015 (datamgmt/data_quantiles.cpp): xtile with if/in, weights and an expression.
ojson runXtileStata(const std::string& newvar, const std::string& expText,
                    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                    const std::optional<std::string>& ifExpr,
                    const std::optional<std::string>& inRange,
                    const std::optional<std::string>& weightVar, SessionState& session,
                    const std::optional<std::string>& weightType);
// BUG-1014 (descriptive/descriptive_statistics.cpp): correlate/pwcorr with covariance and weights.
ojson runCorrStata(const std::vector<std::string>& varlistIn, bool pairwise, bool sig, bool covar,
                   const std::string& weightFlag, const std::string& weightVar,
                   SessionState& session, const std::optional<std::string>& ifExpr,
                   const std::optional<std::string>& inRange);
// BUG-1014 (descriptive/descriptive_statistics.cpp): tabstat with [aw=]/[fw=].
ojson runTabstatWeighted(const std::vector<std::string>& varlist,
                         const std::optional<std::string>& byVal,
                         const std::optional<std::string>& statsVal, SessionState& session,
                         const std::optional<std::string>& ifExpr, bool wantGroups, bool noTotal,
                         const std::string& weightFlag, const std::string& weightVar);
// BUG-1520: the weighted per-group calls of the by-prefix driver -- descriptive/summarize.cpp
// (summarize; `weightFlag` --aweight/--fweight/--iweight) and
// descriptive/descriptive_statistics.cpp (tabstat on a group slice;
// --aweight/--fweight).
ojson summarizeCoreStatsWeighted(Datastore& ds,
                                 const std::optional<std::vector<std::string>>& varlist,
                                 bool detail, const std::optional<std::string>& weightVar,
                                 const char* weightFlag);
ojson runTabstatOnDataWeighted(const std::vector<std::string>& varlist,
                               const std::optional<std::string>& statsVal,
                               const std::string& dataPath, const std::string& weightFlag,
                               const std::string& weightVar);
// W2 esttabnames (BUG-552): `estimates dir NAMELIST` -- results/stored_estimates_commands.cpp,
// declared here on the same pattern (handlers.h is outside that lane).
ojson runEststoDirNames(const std::vector<std::string>& namelist, SessionState& session);
// e(sample) T3 (results/stored_estimates_commands.cpp): `estimates restore NAME` with its mask, and
// `estimates esample[:]`.
ojson runEstimatesRestore(const std::string& name, SessionState& session);
ojson runEstimatesEsample(bool colon, const std::vector<std::string>& varlist,
                          const std::optional<std::string>& ifExpr,
                          const std::optional<std::string>& inRange, bool hasWeight, bool replace,
                          SessionState& session);
// e(sample) T4 (estimation/count_estimation.cpp): the ML fits' mask install.
bool esampleInstallMlMask(SessionState& session, LastModel& lm, ojson& raw);
// BUG-1251 (estimation/count_estimation.cpp): runPoisson with ppmlhdfe d's data_mutation hand-off.
ojson runPoisson(const std::string& stataCmd, const eval::ParsedCommand& parsed,
                 SessionState& session, ojson* dMutationOut);
// BUG-1175 (postestimation/bootstrap_inference.cpp): runBoottest with boottest's printed report.
ojson runBoottest(const eval::ParsedCommand& parsed, SessionState& session, ojson* reportOut);
// BUG-1227 (estimation/instrumental_variables.cpp): runIvRegress with ivregress's `first` report.
ojson runIvRegress(const std::string& method, const eval::ParsedCommand& parsed, bool allowAbsorb,
                   const std::string& stataCmd, SessionState& session, bool smallDefault,
                   bool forceSmall, ojson* reportOut);
}  // namespace handlers
}  // namespace piet
