// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// vce() type-name recognition: two inline functions, defined in this header.
// Called from the estimators under piet/native/src/estimation/ and from
// src/dispatch/stage_tokenization.cpp.
#ifndef PIET_NATIVE_HANDLERS_VCE_TYPES_H
#define PIET_NATIVE_HANDLERS_VCE_TYPES_H

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

// ---------------------------------------------------------------------------
// `vce()`'s ARGUMENT abbreviates, and not uniformly. Measured
// (tests/eval/golden/aer_w1/w1b_vce_abbrev.log, 26 arms on `regress`):
//
//   robust    vce(r) .. vce(robust)        ALL rc 0  -- ONE character is enough
//   cluster   vce(cl x) .. vce(cluster x)  rc 0      -- but vce(c x) is r(198)
//   bootstrap vce(bootstrap) only          vce(boo)/vce(b) are r(198)
//   jackknife vce(jackknife) only          vce(j) is r(198)
//   oim/opg   r(198) OUTRIGHT              regress does not take them at all
//
// `vce(r)` alone is 83 corpus uses in one paper, and this build answered every
// one of them `Unknown vce type: r` while the BARE `robust` option abbreviated
// fine -- the gap was the sub-argument. So the floors are the measured ones, per
// type: a prefix of `robust` at least 1 long, a prefix of `cluster` at least 2.
// Nothing is added on a guess: `bootstrap`/`jackknife`/`hc2`/`hc3` are valid Stata
// this build does not implement and stay refused BY NAME, and `oim`/`opg` are not
// added at all because the oracle refuses them here.
//
// Declared here because THREE translation units recognised vce types
// independently (all in estimation/, one of them twice),
// each with its own `lowerM4(v) == "robust"` and `rfind("cluster", 0)`. That is
// why `vce(r)` could only be fixed in one place by being fixed in one place.
//
// Returns the CANONICAL type for a recognised spelling, or "" otherwise. `rest`
// receives whatever followed the type token (the cluster varlist), trimmed.
//
// The case tolerance is DELIBERATELY unchanged: Stata is case-sensitive here
// (`vce(R)` is r(198)) and this build lowercases, so it accepts a spelling Stata
// refuses. The corpus has ZERO uppercase `vce(` uses, so narrowing it would only
// risk the callers that depend on the tolerance for no measured gain -- named,
// not silently kept.
inline std::string canonicalVceType(const std::string& raw, std::string* rest) {
  if (rest != nullptr) rest->clear();
  size_t a = raw.find_first_not_of(" \t");
  if (a == std::string::npos) return std::string();
  size_t e = raw.find_first_of(" \t", a);
  std::string tok = raw.substr(a, (e == std::string::npos ? raw.size() : e) - a);
  if (rest != nullptr && e != std::string::npos) {
    const size_t b = raw.find_first_not_of(" \t", e);
    if (b != std::string::npos) {
      size_t z = raw.find_last_not_of(" \t");
      *rest = raw.substr(b, z - b + 1);
    }
  }
  for (auto& c : tok) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  auto isPrefixOf = [&](const char* full, size_t minLen) {
    const std::string f(full);
    return tok.size() >= minLen && tok.size() <= f.size() && f.compare(0, tok.size(), tok) == 0;
  };
  if (isPrefixOf("robust", 1)) return "robust";
  if (isPrefixOf("cluster", 2)) return "cluster";
  if (tok == "hc1") return "hc1";
  if (tok == "iid") return "iid";
  return std::string();
}

// The vce types Stata accepts on `regress` that this build does NOT implement.
// Measured rc 0 in the oracle and r(198) here (w1b_vce_abbrev.log v_boot/v_jack/
// v_hc2/v_hc3), so they are a NAMED refusal rather than `Unknown vce type` --
// "unknown" is the wrong diagnosis for a real Stata option, which is the whole
// point of the vocabulary split this campaign runs on.
//
// EXACT spellings only, because that is what the oracle says: `vce(boo)` and
// `vce(b)` are r(198) in Stata too, and `vce(j)` likewise -- unlike `robust`,
// these do not abbreviate at all. Recognising an abbreviation here would invent
// a refusal Stata does not give.
inline std::string unimplementedVceType(const std::string& raw) {
  size_t a = raw.find_first_not_of(" \t");
  if (a == std::string::npos) return std::string();
  size_t e = raw.find_first_of(" \t", a);
  std::string tok = raw.substr(a, (e == std::string::npos ? raw.size() : e) - a);
  for (auto& c : tok) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (tok == "bootstrap" || tok == "jackknife" || tok == "hc2" || tok == "hc3") return tok;
  return std::string();
}

}  // namespace piet::handlers

#endif  // PIET_NATIVE_HANDLERS_VCE_TYPES_H
