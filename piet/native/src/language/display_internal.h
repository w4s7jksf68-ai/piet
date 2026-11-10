// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// display_internal.h — private declarations shared by the three translation units
// that implement `display`: display_command.cpp (the `display`
// statement), display_number_format.cpp (the `%fmt` renderer) and
// scalar_expression.cpp (value references and scalar-expression evaluation).
// Not part of the public API; the public entry points stay in
// piet/display_hardening.h.
#ifndef PIET_NATIVE_LANGUAGE_DISPLAY_INTERNAL_H
#define PIET_NATIVE_LANGUAGE_DISPLAY_INTERNAL_H

#include "piet/display_hardening.h"

#include <map>
#include <optional>
#include <regex>
#include <string>

namespace piet::display_detail {

// defined in display_number_format.cpp
const std::regex& reStringFmtSpec();
std::string fmtDisplayDefaultG(double v);
std::string applyStataFmtLocal(const std::string& fmt, const std::string& valueStr,
                               std::string* err = nullptr);

// defined in scalar_expression.cpp
std::string strip(const std::string& s);
const std::map<std::string, std::string>& eScalarKey();
const std::regex& reBRef();
const std::regex& reSeRef();
const std::regex& reERef();
std::optional<std::string> stringVarSubscript(const std::string& token, SessionState& session);
std::optional<std::string> bindBareVarsToObs1(const std::string& expr, SessionState& session);
std::optional<std::string> bindVarSubscripts(const std::string& expr, SessionState& session);
size_t skipQuotedSpan(const std::string& s, size_t i);
size_t matchingClose(const std::string& s, size_t open);
std::optional<std::string> bindSubscripts(const std::string& expr, SessionState& session,
                                          std::string* unknown = nullptr);
std::string resolveToken(const std::string& token, SessionState& session,
                         std::string* err = nullptr, std::optional<double>* stored = nullptr,
                         bool* strValue = nullptr);
std::optional<std::string> unboundNameRefusal(const std::string& text, SessionState& session);

}  // namespace piet::display_detail

#endif  // PIET_NATIVE_LANGUAGE_DISPLAY_INTERNAL_H
