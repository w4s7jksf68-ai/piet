// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// serset_internal.h — private helpers shared by the serset store, the `serset`
// command, graph registration and `graph dir` (piet::serset::serset_detail).
#pragma once

#include "piet/serset.h"

#include <cctype>
#include <limits>
#include <string>

#include "piet/eval/errors.h"

namespace piet::serset {

using ojson = nlohmann::ordered_json;

namespace serset_detail {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

inline ojson refusal(const std::string& message, int rc) {
  return ojson(eval::errorBlock(rc == 198 ? "syntax_error" : "engine_error", message, std::nullopt,
                                std::nullopt, std::nullopt, "r(" + std::to_string(rc) + ")"));
}

inline ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

inline std::string trimText(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

inline const std::string& storedName(const Column& c) { return c.stored.empty() ? c.name : c.stored; }

}  // namespace serset_detail

}  // namespace piet::serset
