// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once

#include "piet/eval/macros.h"
#include <functional>
#include <regex>

namespace piet::eval::macro_detail {
bool isRegexSpace(char c);
const std::regex& reGlobalRefBrace();
const std::regex& reGlobalRefBare();
std::string substituteScalarCalls(const std::string& in, const EvalSession& session);
const std::regex& reArithMacro();
bool hasLocalMacroRef(const std::string& s);
const std::regex& reSRefDelim();
const std::regex& reCRefDelim();
const std::regex& reERef();
const std::regex& reRRef();
bool isSafeArith(const std::string& s);
bool isSafeArithExt(const std::string& s);
std::string liftDateLiterals(const std::string& s, std::string* bad);
const std::regex& rePostfileSafeArith();
const std::regex& reRRefDelim();
const std::regex& reERefDelim();
const std::regex& reLocalRef();
const std::regex& reSafeArith();
const std::regex& reSafeArithExt();
bool matchScalarDef(const std::string& s, std::string& name, std::string& rest);
const std::regex& reScalarDef();
bool matchScalarDrop(const std::string& s, std::string& rest);
bool matchMacroDef(const std::string& s, const char* kw, size_t kwLen, std::string& name,
                   std::string& rest);
const std::regex& reLocalDef();
const std::regex& reGlobalDef();
bool macroDefNameTokenHasMacroChar(const std::string& s);
}
