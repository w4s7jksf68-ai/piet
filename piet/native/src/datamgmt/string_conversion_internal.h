// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// String/number conversion commands: encode, decode, destring and tostring.
// Value-label metadata, storage types and resident-column updates remain in
// the original handlers; the shared data-verb helpers retain one definition.
//
// Private declarations shared by datamgmt/destring.cpp and datamgmt/tostring.cpp;
// both helpers are defined in datamgmt/string_conversion_support.cpp.
// csvEscape, inline below, writes the CSV fields of decode.cpp and tostring.cpp.
#pragma once

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers::string_conversion_detail {
nlohmann::ordered_json destringNoteBlock(const std::string& text);
std::optional<nlohmann::ordered_json> placeGeneratedAfterSource(
    SessionState& session, const std::string& verb, const std::string& source,
    const std::string& target, bool resident, nlohmann::ordered_json& mutated);
}  // namespace piet::handlers::string_conversion_detail

namespace piet { namespace handlers { namespace dv_detail {
// CSV-escape a field (RFC4180 doubled-quote) — same convention as
// row_evaluation.cpp's own csvEscape()/fileio/postfile.cpp's, kept as a file-local copy
// rather than a new shared header. Still used by the remaining whole-frame
// CSV writers in this file (runDestring / runTostring / runJoinby), none of
// which has a native cobra verb yet.
inline std::string csvEscape(const std::string& text) {
  std::string esc;
  esc.reserve(text.size() + 2);
  esc += '"';
  for (char c : text) {
    if (c == '"') esc += '"';
    esc += c;
  }
  esc += '"';
  return esc;
}
}}}  // namespace piet::handlers::dv_detail
