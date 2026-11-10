// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once

#include "piet/eval/macros.h"
#include <functional>
#include <regex>

namespace piet::eval::macro_detail {
bool isPySpace(char c);
bool isIdentStart(char c);
bool isIdentChar(char c);
bool globalNameStartOk(const std::string& name);
std::vector<std::pair<std::string, std::string>> findAll(const std::regex& re,
                                                          const std::string& s);
std::string strip(const std::string& s);
std::string protectStringLiterals(const std::string& s, std::vector<std::string>* out);
std::string restoreStringLiterals(const std::string& s, const std::vector<std::string>& spans);
bool hasGlobalRef(const std::regex& re, const std::string& s);
std::string regexSubFn(const std::regex& re, const std::string& input,
                        const std::function<std::string(const std::smatch&)>& fn);
std::string toLower(std::string s);
std::string replaceFirst(const std::string& s, const std::string& from, const std::string& to);
std::vector<std::pair<std::string, std::string>> findAllGlobalRefs(const std::regex& re,
                                                                   const std::string& s);
std::string pyRepr(const std::string& s);
std::string maskParensInQuotes(const std::string& s);
std::vector<std::string> pySplit(const std::string& s);
std::string regexEscape(const std::string& s);
}
