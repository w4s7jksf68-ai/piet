// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// SessionState: process-cache reset at construction and the `clear all` teardown of session objects.
#include "piet/session_state.h"
#ifndef _WIN32
#include <execinfo.h>
#include <dlfcn.h>
#endif
#include "io/win_compat.h"  // cecon::to_native_path (BUG-050 wide open)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdio>   // C4-L1 window trace (stderr fprintf, env-gated)
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>  // std::greater<> (BUG-381 census sort)
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>

#include "piet/bcal.h"  // bcal::clearCache (resetProcessCaches)
#include "piet/cclass.h"  // BUG-1006: resolveCValue (macroCValue)
#include "piet/counters.h"
#include "piet/eval/errors.h"  // eval::coarseRc (the r(N) of a logged error)
#include "piet/eval/macros.h"  // eScalarKey (eNameOrderFor)
#include "piet/display_hardening.h"  // BUG-718: readVarObservation (macroVarObs)
#include "piet/scratch_seq.h"  // BUG-389: scratchMintSeq (sweep throttle)
#include "piet/storage_type.h"  // stataDefaultFormat (displayFormat)

namespace piet {

// AER scope-2 B2-3 (review 2026-09-09). The business-calendar cache is a
// PROCESS singleton (bcal.h explains why: `display bofd(...)` evaluates in a
// process-wide immutable namespace that carries no session). Its key is the
// calendar NAME alone, because that is what Stata's cache is keyed by — so the
// only thing keeping two sessions in one process apart is this reset. It used
// to be claimed by a comment and performed by nobody: bcal::clearCache() had
// zero callers before this change.
void SessionState::resetProcessCaches() { bcal::clearCache(); }

// BUG-392: `clear all` beyond the dataset ─────────────────────────────────────
void SessionState::clearAllSessionObjects() {
  eval.mlState.reset();
  estimatesClear();
  // AER-SCOPE1 bundle G: `clear all` closes the open workbook. A surviving cell
  // map would write the next `putexcel A1 = ...` into a file the PREVIOUS
  // analysis named, at rc 0 -- the same reason every sibling below is dropped.
  putexcel = nlohmann::ordered_json::object();
  estAddedClear();  // AER-SCOPE1 bundle E: the estadd bag belongs to e(), which
                    // `clear all` drops -- leaving it would attach one model's
                    // additions to the next regression after a `clear all`.
  namedGraphs.clear();
  graphMeta.clear();  // graph-family AI-4
  currentGraph.clear();
  sersets.clear();    // serset-twoway SS: `clear all` drops every serset (serset.sthlp)
  postfiles.clear();  // runClear's `all` branch closed (wrote) each one first (BUG-1079)
  for (auto& [n, fh] : fileHandles) {
    (void)n;
    if (fh.fp) std::fclose(fh.fp);
    fh.fp = nullptr;
  }
  fileHandles.clear();
  std::vector<std::string> paths;
  for (const auto& [n, fp] : frames) { (void)n; paths.push_back(fp); }
  frames.clear();
  frameLinks.clear();
  // NOT the preserve stack. Oracle (StataBE 19.5, 2026-09-02): `sysuse auto` → `preserve` →
  // `clear all` → `restore` is rc 0 with N=74 — a preserve survives `clear all` (and `clear`),
  // and `restore` brings the data back. The first cut of this function dropped the stack and
  // the fuzzer caught it within the hour (restore after clear all → "no previous preserve").
  dropUseCaches();
  // Ledgers cleared FIRST, then the files: the reclaim primitive's exclusions
  // (frames, preserve twins, memos) no longer pin them, everything else still does.
  for (const auto& p : paths)
    if (!p.empty()) reclaimSupersededScratch(p);
  if (cobraSession_ && cobraSession_->hasWorker()) (void)cobraSession_->frameDrop();
}

}  // namespace piet
