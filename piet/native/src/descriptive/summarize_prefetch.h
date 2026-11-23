// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
// One engine answer to `eval --summarize VAR` over the dataset at PATH, handed from the
// request that computed it to the `summarize` that would otherwise ask for it again.
//
// Why it exists. egen leaves in r() what egen.ado leaves there, and for mean/total/sum/
// pc/min/max that is a quiet `summarize ARG, meanonly` (egenPublishResidue). That
// summarize used to be its own engine request, right after the egen request over the
// same frame. The egen request now asks for ARG's summary too (`--summarize ARG`; the
// engine computes it with the same function, on the final frame, which has ARG's exact
// bytes), and the summarize takes that answer instead of asking. Everything that turns
// the answer into r() is unchanged; only the duplicate round trip is gone. On the AEA
// package openICPSR-226844 that is two of the thirteen requests of each pass of a loop
// that runs 39,000 times.
//
// Why it is safe to hand over. The answer is a function of the dataset and VAR alone,
// and PATH names the dataset: every mutation of the session's data mints a new head
// path (a resident run's --out, a load, a restore), so a head still at PATH holds the
// bytes the answer was computed from. The answer is taken at most once, only by a plain
// unweighted summarize of exactly VAR over exactly PATH, and is dropped by
// summarizePrefetchClear(), which egen calls before its request and after its r() step.
#include <optional>
#include <string>

#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

void summarizePrefetchSet(const std::string& path, const std::string& var, ojson stats);
// The stored answer when it is for exactly (path, var), and it is then gone.
std::optional<ojson> summarizePrefetchTake(const std::string& path, const std::string& var);
void summarizePrefetchClear();

}  // namespace piet::handlers
