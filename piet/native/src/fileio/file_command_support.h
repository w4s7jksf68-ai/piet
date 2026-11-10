// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private to piet/native/src/fileio/: the helper layer shared by the text and
// file I/O commands (insheet, outsheet, infile, infix, outfile, saveold,
// putexcel, filefilter, copy). Defined in file_command_support.cpp; the
// family's doctrine (the five defect classes) is the comment at its top.
#pragma once

#include "vendor/nlohmann/json.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace file_command_detail {

// Errors and blocks
ojson ioError(const std::string& message, const std::string& code = "engine_error");
ojson ioSyntax(const std::string& message);
ojson ioErrorRc(const std::string& message, int rc);
ojson ioRefusal(const std::string& cmd, const std::string& cause, const std::string& message);
ojson displayBlock(const std::string& text);
ojson silent();
bool isErrorBlock(const ojson& b);

// Small text helpers
std::string lowerOf(std::string s);
std::string trimOf(const std::string& s);
std::string dequote(const std::string& s);
bool parseStrictDouble(const std::string& sIn, double* out);
bool parseStrictInt(const std::string& s, long long* out);
bool hasFlag(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
             const std::string& key);
std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::string& key);
std::optional<std::string> optStrAny(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
    std::initializer_list<const char*> keys);

// The pre-comma word list, quote-aware
std::string dropCommandWord(const std::string& in);
std::string preCommaArgs(const std::string& raw);
std::vector<std::string> quoteAwareWords(const std::string& s);
bool splitUsing(const std::vector<std::string>& words, std::vector<std::string>* varlist,
                std::string* pathArg);
std::vector<std::string> dropQualifierWords(const std::vector<std::string>& words);

// Files
bool pathExists(const std::string& p);
bool sameFile(const std::string& a, const std::string& b);
std::optional<ojson> refuseIfSessionHbin(SessionState& session, const std::string& verb,
                                         const std::string& resolved,
                                         const std::string& spelling);
std::optional<ojson> refuseExisting(const std::string& resolved, const std::string& spelling,
                                    bool replace);
void assumeSuffix(std::string* resolved, std::string* spelling, const char* suffix);
std::optional<ojson> commitBytes(const std::string& verb, const std::string& resolved,
                                 const std::string& spelling, const std::string& bytes);
std::optional<std::string> readWholeFile(const std::string& resolved);

// Delimiters
bool readDelimiterOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts, char* out,
                      std::string* err);

}  // namespace file_command_detail

}  // namespace piet::handlers
