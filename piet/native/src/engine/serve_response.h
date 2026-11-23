// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Reading one `cobra serve` response line into the request's outputs.
//
// The general reading is nlohmann::json::parse plus the field readers in
// cobra_session.cpp (fillFrameMeta, fillEstMeta, resultFromResponse); it throws
// nlohmann::json::exception on a malformed line, and CobraSession::request turns
// that into "cobra serve: malformed response".
//
// The fast reading exists because the general one cost about 4 us per response
// (2 us of it in nlohmann's lexer alone), and a do-file loop sends one request per
// command: about 11% of PIET's own CPU on the AEA package openICPSR-226844
// (2026-11-20 profile). It reads the response shapes the engine writes
// (apps/cecon_cli.cpp: id, exit, stdout, stderr, frame{rows, cols, changed,
// added, dropped, structure, card_stable, merge_counts, vars_delta, vars, path},
// est{handle, k, n_obs, vcov_type, fits}) and nothing else: an unknown or
// repeated key, a value of another type, a float where an integer is read, a \u
// escape, a byte >= 0x80 or a control byte in a string, or anything after the
// object makes it decline. It writes its outputs only when it has read the whole
// line, so a decline leaves them as they were and the caller takes the general
// reading, which then behaves exactly as it did before (throw included). Where
// it accepts, it gives what the general reading gives; testServeResponseFastRead
// (selftest_language_hand_scans.cpp) holds it to that on generated responses.
#ifndef PIET_NATIVE_ENGINE_SERVE_RESPONSE_H
#define PIET_NATIVE_ENGINE_SERVE_RESPONSE_H

#include <string>

#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"

namespace piet::serve_response {

// The fast reading. `meta` may be null (the request asked for no frame
// metadata). False: declined, nothing written.
bool readFast(const std::string& line, CobraFrameMeta* meta, CobraEstMeta* est,
              CobraRunResult* result);

// The general reading, in CobraSession::request's order (frame metadata, est,
// then the result); throws nlohmann::json::exception on a malformed line, with
// whatever it wrote before the throw left written.
void readGeneral(const std::string& line, CobraFrameMeta* meta, CobraEstMeta* est,
                 CobraRunResult* result);

}  // namespace piet::serve_response

#endif  // PIET_NATIVE_ENGINE_SERVE_RESPONSE_H
