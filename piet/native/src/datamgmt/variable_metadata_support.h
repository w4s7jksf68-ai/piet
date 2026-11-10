// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private helpers shared by the variable-metadata commands (findname, xtdescribe, cf,
// fvunab, numlabel, labvars, renpfix). Include only from those command files.
#pragma once

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers::variable_metadata_detail {

using ojson = nlohmann::ordered_json;

ojson rcError(const std::string& message, int rc);
ojson notImplemented(const std::string& message);
ojson displayBlock(const std::string& text);
ojson silent();
bool isErrorBlock(const ojson& b);
const ojson* firstError(const ojson& result);
bool hasFlag(const eval::ParsedCommand& p, const std::string& name);
std::optional<std::string> optStr(const eval::ParsedCommand& p, const std::string& name);
std::optional<long long> intOption(const eval::ParsedCommand& p, const std::string& name,
                                   long long fallback, ojson* err);
std::vector<std::string> stataWords(const std::string& text);
std::string joinWords(const std::vector<std::string>& v, const std::string& sep = " ");
std::string padTo(std::string s, size_t col);
std::string rightAlign(const std::string& s, size_t w);
std::string stataAbbrev(const std::string& s, int n);
std::string displayNumber(double v, SessionState& session);
std::vector<std::string> allVarNames(SessionState& session);
std::optional<ojson> unabVarlist(const std::string& spec, SessionState& session,
                                 std::vector<std::string>* out);
void dropScratch(const std::string& path);

// Line width: PIET has no `set linesize`; the column count uses 134, the c(linesize) of
// the StataBE batch session the oracle and the probes ran under (mx1.log LINESIZE|134|).
constexpr int kLineSize = 134;

// Read scope over a few named columns: the qualifier (when present) through
// applyIfAndInFilter's filtered scratch copy; otherwise a section-38 projection of just
// those columns, with the materialized head as the fallback (column_fetch.h's law).
class ColumnScope {
 public:
  ColumnScope(SessionState& session, const std::vector<std::string>& cols,
              const std::optional<std::string>& ifExpr, const std::optional<std::string>& inRange,
              const std::string& verb) {
    if (!session.hasData()) {
      err_ = ojson(eval::noDataLoaded());
      return;
    }
    const bool qualified = (ifExpr.has_value() && !strutil::trim(*ifExpr).empty()) ||
                           (inRange.has_value() && !strutil::trim(*inRange).empty());
    try {
      if (qualified) {
        InRangeFilterResult f = applyIfAndInFilter(session, *session.currentHbin(), ifExpr, inRange);
        if (!f.ok) {
          err_ = inRangeRejectError(f, inRange.value_or(std::string()), verb);
          return;
        }
        if (f.dataPath != *session.currentHbin()) {
          session.noteProjectionScratch(f.dataPath);  // BUG-381: read once, abandoned
          owned_ = std::make_unique<Datastore>(f.dataPath);
          ds_ = owned_.get();
        } else {
          ds_ = session.datastore();
        }
        return;
      }
      std::vector<const std::string*> exprs;
      for (const auto& c : cols) exprs.push_back(&c);
      if (auto want = referencedColumns(session, exprs, "")) {
        std::string projPath;
        std::unique_ptr<Datastore> proj = fetchColumns(session, *want, session.workdir(), &projPath);
        if (proj) {
          bool complete = true;
          for (const auto& c : cols)
            if (!proj->has(c)) complete = false;
          if (complete) owned_ = std::move(proj);
        }
      }
      ds_ = owned_ ? owned_.get() : session.datastore();
    } catch (const std::exception& e) {
      err_ = ojson(eval::engineError(verb + ": could not read the data -- " + e.what(), "data_io"));
    }
  }
  Datastore* get() const { return ds_; }
  bool failed() const { return !err_.is_null() || ds_ == nullptr; }
  ojson error() const { return err_.is_null() ? ojson(eval::noDataLoaded()) : err_; }

 private:
  std::unique_ptr<Datastore> owned_;
  Datastore* ds_ = nullptr;
  ojson err_;
};

}  // namespace piet::handlers::variable_metadata_detail
