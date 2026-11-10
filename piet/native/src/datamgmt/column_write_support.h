// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private to datamgmt/: the helpers shared by the five commands that write a column --
// mvdecode.cpp, integ.cpp, sencode.cpp, fillmissing.cpp, astile.cpp -- each defined once in
// column_write_support.cpp (namespace column_write_detail). Include it from no other family:
// several names here (displayBlock, firstError, splitWs, optStr, ...) also exist, with other
// bodies, in utility/utility_support.h. The family notes below are carried over verbatim from
// the single file this family was split from; "this file" there means the family.
//
// AER tier-1 (2026-10-28), lane GEN: verbs that write a column -- mvdecode integ sencode
// fillmissing astile. Registry rows, dispatcher arms, handlers.h declarations, effects/floors
// rows, NOTICE lines and pangram lines landed with the shared-surface unit; this file is the
// lane's alone. Answers: tests/eval/golden/aer_w2/w2ax_t1_official.log and
// w2ax_t1_community.log (StataBE 19.5), plus the lane's own StataBE probes cited by arm.
//
// WHAT EACH COMMAND REUSES. Nothing here computes a statistic the engine already computes:
//
//   (each command's own note now heads its own file)
//
// TEMPORARY COLUMNS. Every scratch variable this file makes is named `__piet_t1g_*` and is
// dropped before the reply is built, so the card in the reply and the exit census never see
// one (probe ORPHAN 0).
//
// DEFECT CLASSES (the list in dataset_shape_support.h's header): every return is a block object or an array of
// block objects; every rc Stata gives that coarseRc cannot infer rides `reason` as "r(NNN)";
// every option read is registered below at namespace scope, real-but-unread options sit in
// `unimplemented`.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "piet/datastore.h"
#include "piet/eval/tokenizer.h"
#include "vendor/nlohmann/json.hpp"
#include "piet/session_state.h"

namespace piet::handlers::column_write_detail {

using ojson = nlohmann::ordered_json;

// Blocks and errors
ojson rcError(const std::string& message, int rc, const std::string& code = "engine_error");
ojson syntaxError(const std::string& message);
ojson displayBlock(const std::string& text);
std::optional<ojson> firstError(const ojson& result);

// Options and scalar text
using OptList = std::vector<std::pair<std::string, eval::OptValue>>;

bool hasFlag(const OptList& opts, const std::string& key);
std::optional<std::string> optStr(const OptList& opts, const std::string& key);
std::vector<std::string> splitWs(const std::string& s);
std::string joinSpace(const std::vector<std::string>& v);
bool parseStrictDouble(const std::string& sIn, double* out);
std::string numCell(double v);
double toFloat(double v);

// Variables, scratch columns, the if/in sample, column readers
std::optional<ojson> resolveVars(SessionState& session, const std::vector<std::string>& toks,
                                 std::vector<std::string>* out);
bool columnExists(SessionState& session, const std::string& name);
void dropColumns(SessionState& session, const std::vector<std::string>& names);

// The scratch columns a handler made, dropped on every return path. `clear()` runs before
// the reply's card is built; the destructor is the net for an exception.
class ScratchCols {
 public:
  explicit ScratchCols(SessionState& s) : s_(s) {}
  ~ScratchCols() {
    try {
      clear();
    } catch (...) {
    }
  }
  void add(const std::string& n) { names_.push_back(n); }
  void clear() {
    if (!names_.empty()) dropColumns(s_, names_);
    names_.clear();
  }

 private:
  SessionState& s_;
  std::vector<std::string> names_;
};

bool hasQualifier(const eval::ParsedCommand& parsed);
std::string qualifierText(const eval::ParsedCommand& parsed);
std::optional<ojson> sampleMask(SessionState& session, const eval::ParsedCommand& parsed,
                                std::vector<char>* mask, const std::string& keepAs = "");
std::vector<std::string> columnCells(const Datastore& ds, const std::string& name);
std::vector<double> columnDoubles(const Datastore& ds, const std::string& name);

// Writing: one column through `cobra setcol`
std::optional<ojson> runVerb(const std::vector<std::string>& args);
std::optional<ojson> setColumn(SessionState& session, const std::string& var,
                               const std::vector<std::string>& cells, bool isString, bool isNew,
                               const std::string& storeType);

// Group keys
struct KeyCol {
  bool isString = false;
  std::vector<double> num;
  std::vector<std::string> str;  // "" = missing
};

KeyCol readKeyCol(const Datastore& ds, const std::string& name);
bool keyMissing(const KeyCol& k, size_t i);
int keyCompare(const KeyCol& k, size_t a, size_t b);
int keysCompare(const std::vector<KeyCol>& keys, size_t a, size_t b);

}  // namespace piet::handlers::column_write_detail
