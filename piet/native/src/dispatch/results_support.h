// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "types.h"

namespace piet::dispatch_detail {

void pushEstStoreMutation(const ojson& stored, ojson& blocks, SessionState& session);

void installMlEsampleAfterFit(const ojson& block, SessionState& session);

void storeUserMatrix(SessionState& session, const std::string& name, eval::EvalMatrix mat);

std::string dequoteEstTitle(std::string t);

std::optional<ojson> refuseIfFitDataGone(const std::string& verb, SessionState& session);

std::optional<ojson> readEststoStoreOpts(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts, EststoStoreOpts* out);

size_t findEststoPrefixColon(const std::string& s);

// BUG-733: run a fit that writes its own e() values (eval.eScalars / eStrings /
// eMatrices) BEFORE it calls setLastModel -- grep-measured, exactly runPca,
// runOaxaca and runTeffects (estimation/principal_components.cpp,
// estimation/oaxaca_decomposition.cpp, estimation/teffects_nnmatch.cpp). setLastModel drops a
// live posting (`ereturn post`, estpost) and the names recorded on top of the
// previous fit (SessionState::eOnFitNames) at its top, which for these three
// would also drop the fit's own values (`ereturn local title "mine"` then
// `pca` would lose pca's own e(title)), so both are ended here, before the fit
// writes anything. A fit that installs no model (refused, or the engine
// failed) leaves e() as it was in Stata, so the stores, the posting flag and
// the record are put back. FU-375: after a successful install every e() name
// the fit left is recorded, so the NEXT install drops it -- the fit's own
// pre-install keys (pca's e(N), oaxaca's posted e(b)) are no longer read for
// a later regress. Recording every name, not a diff, also covers a key the
// fit rewrote with an equal value; setLastModel's own republished macros
// (e(cmd), e(depvar), ...) are republished by the next install anyway.
template <typename Fit>
inline ojson runFitWritingEFirst(SessionState& session, Fit&& fit) {
  auto scalars = session.eval.eScalars;
  auto strings = session.eval.eStrings;
  auto matrices = session.eval.eMatrices;
  const bool posted = session.ePostedLive;
  const unsigned long long postedMask = session.ePostedEsampleSerial;
  auto onFit = session.eOnFitNames;
  const unsigned long long installsBefore = session.lastModelInstalls;
  session.supersedePostedE();
  session.dropEOnFit();
  ojson out = fit();
  if (session.lastModelInstalls == installsBefore) {
    session.eval.eScalars = std::move(scalars);
    session.eval.eStrings = std::move(strings);
    session.eval.eMatrices = std::move(matrices);
    session.ePostedLive = posted;
    session.ePostedEsampleSerial = postedMask;
    session.eOnFitNames = std::move(onFit);
    return out;
  }
  for (const auto& [k, v] : session.eval.eScalars.items()) session.noteEOnFit(k);
  for (const auto& [k, v] : session.eval.eStrings.items()) session.noteEOnFit(k);
  for (const auto& [k, m] : session.eval.eMatrices) session.noteEOnFit(k);
  return out;
}

template <typename Run>
inline ojson runPostestKeepingE(SessionState& session, Run&& run) {
  if (!session.ePostedLive && session.eOnFitNames.empty()) return run();
  auto scalars = session.eval.eScalars;
  auto strings = session.eval.eStrings;
  auto matrices = session.eval.eMatrices;
  const bool posted = session.ePostedLive;
  const unsigned long long postedMask = session.ePostedEsampleSerial;
  auto onFit = session.eOnFitNames;
  ojson out = run();
  session.eval.eScalars = std::move(scalars);
  session.eval.eStrings = std::move(strings);
  session.eval.eMatrices = std::move(matrices);
  session.ePostedLive = posted;
  session.ePostedEsampleSerial = postedMask;
  session.eOnFitNames = std::move(onFit);
  return out;
}

}  // namespace piet::dispatch_detail
