// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private helpers shared by the statistics and estimation commands that were one
// translation unit: rc-carrying error blocks, fixed-width name cells, the column
// read scope, numeric column reads, engine capability probes and column writes.
#pragma once

#include "piet/eval/tokenizer.h"
#include "vendor/nlohmann/json.hpp"

#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "piet/column_fetch.h"
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/session_state.h"

namespace piet::handlers::statcmd_detail {

using ojson = nlohmann::ordered_json;

std::string fitLeft(const std::string& s, size_t w);
std::string fitRight(const std::string& s, size_t w);
std::string abbrevCp(const std::string& s, size_t w);

ojson aerError(const std::string& message, const std::string& code = "engine_error");
ojson aerSyntaxError(const std::string& message);
ojson aerErrorReason(const std::string& message, int rc);
ojson displayBlock(const std::string& text);

// ---------------------------------------------------------------------------
// Reading

// Open the columns this command needs. Mirrors utility/utility_support.h's
// ReadScope: try the section-38 projection first, fall back to the resident
// head, and never half-succeed -- a projection that is missing any requested
// column is discarded rather than silently answering about fewer columns.
class ColumnScope {
 public:
  ColumnScope(SessionState& session, const eval::ParsedCommand& parsed,
              const std::vector<std::string>& cols, const std::string& verb) {
    if (!session.hasData() || !session.currentHbin().has_value()) {
      err_ = ojson(eval::noDataLoaded());
      return;
    }
    const bool wantFilter = (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
                            (parsed.inRange.has_value() && !parsed.inRange->empty());
    try {
      if (wantFilter) {
        InRangeFilterResult f =
            applyIfAndInFilter(session, *session.currentHbin(), parsed.ifExpr, parsed.inRange);
        if (!f.ok) {
          err_ = inRangeRejectError(f, parsed.inRange.value_or(std::string()), verb);
          return;
        }
        if (f.dataPath != *session.currentHbin()) {
          session.noteProjectionScratch(f.dataPath);
          owned_ = std::make_unique<Datastore>(f.dataPath);
          ds_ = owned_.get();
          return;
        }
        ds_ = session.datastore();
        return;
      }
      std::vector<const std::string*> exprs;
      exprs.reserve(cols.size());
      for (const auto& c : cols) exprs.push_back(&c);
      if (auto want = referencedColumns(session, exprs, "")) {
        std::string projPath;
        std::unique_ptr<Datastore> proj =
            fetchColumns(session, *want, session.workdir(), &projPath);
        if (proj) {
          bool complete = true;
          for (const auto& c : cols) {
            if (!proj->has(c)) {
              complete = false;
              break;
            }
          }
          if (complete) owned_ = std::move(proj);
        }
      }
      ds_ = owned_ ? owned_.get() : session.datastore();
    } catch (const std::exception& e) {
      err_ = aerError(verb + ": could not read the data -- " + std::string(e.what()), "data_io");
    }
  }

  Datastore* get() const { return ds_; }
  const ojson& error() const { return err_; }
  bool failed() const { return !err_.is_null() || ds_ == nullptr; }

 private:
  std::unique_ptr<Datastore> owned_;
  Datastore* ds_ = nullptr;
  ojson err_;
};

bool numericColumn(const Datastore& ds, const std::string& name, std::vector<double>* out,
                   std::string* err);

bool engineHasSubcommand(const std::string& sub);
ojson engineTooOld(const std::string& stataCmd, const std::string& sub);

bool estimWriteColumn(SessionState& session, const std::string& name,
                      const std::vector<double>& vals, ojson* err);
std::vector<double> jsonDoubles(const nlohmann::json& j);
bool smoothIsNumeric(SessionState& session, const std::string& name);

}  // namespace piet::handlers::statcmd_detail
