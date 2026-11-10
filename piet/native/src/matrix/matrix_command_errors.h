// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER-SCOPE1 bundle B (RESULTS, 2026-09-03) -- r() made visible, and the
// matrix <-> dataset round trip.
//
//   return list      print r() the way `ereturn list` prints e()
//   svmat            matrix columns -> variables
//   mkmat            variables -> matrix
//   matlist          render a matrix (or e(b)/e(V)) as a text table
//
// WHY ALL FOUR SHARE ONE TU. They share three things and nothing else in the
// tree shares them: the r() stores, session.eval.matrices, and the numeric
// rendering rule. Splitting them would put the matrix-store helpers (the
// replace-in-place store, the e(b)/e(V) extraction) in a header nobody else
// needs, or duplicate them, which is exactly how the two-layer divergences in
// this codebase have started before.
//
// THREE DEFECT CLASSES THIS FILE IS WRITTEN AGAINST, all three already shipped
// once in this campaign:
//
//  1. SHAPE. dispatcher.cpp's bundle-B arms are plain
//     `blocks.push_back(ojson(handlers::runX(parsed, session)))` -- they do NOT
//     carry the `if (!(r.is_array() && r.empty()))` filter the `matrix`/`label`
//     arms have. So a handler here returns EITHER exactly one JSON OBJECT or an
//     EMPTY array (silent success). A non-empty array would land where an
//     object belongs and nlohmann throws type_error.306 at the first consumer
//     that reads ["type"].
//  2. RC. eval::coarseRc answers a specific Stata rc only for
//     code=="unsupported_command" (199), a `reason` spelled "r(NNN)", or one of
//     its measured message-substring rules. A refusal built with the default
//     "engine_error" code and a freshly-worded message silently becomes rc 1.
//     So every refusal below either uses a wording coarseRc already maps
//     ("variable X not found" -> 111, "... already defined" -> 110) or states
//     its rc explicitly through errorBlock's `reason` argument.
//  3. VOCABULARY. rejectUnknownOptions resolves a >=2-char unique prefix
//     against `known` and therefore ACCEPTS abbreviations; canonicalizeForCommand
//     is what makes the handler's canonical-key compare see them. It is driven
//     by the REGISTRY, so an unregistered command has its abbreviations accepted
//     by the net and then dropped unread by the compares -- accepted, ignored,
//     no diagnostic (BUG-231/BUG-233). All three option-taking handlers register
//     at namespace scope below.
#pragma once
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

#include <optional>
#include <string>

#include "piet/eval/errors.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace matrix_command_detail {

// ---------------------------------------------------------------------------
// Small shared helpers.

inline ojson aerError(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

// Refusal whose Stata rc is KNOWN but which coarseRc's message rules do not
// recognise. `reason` is the only channel that carries an exact rc (BUG-187),
// and it outranks the coarse mapping -- without it every one of these would
// come back as the generic rc 1 (defect class 2 in this file's header).
inline ojson aerErrorRc(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

}  // namespace matrix_command_detail

}  // namespace piet::handlers
