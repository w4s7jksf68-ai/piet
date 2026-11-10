// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "piet/handlers.h"
#include "piet/eval/errors.h"
#include "piet/column_fetch.h"
#include "piet/inrange_filter.h"
#include "piet/session_state.h"
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <utility>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace piet::handlers::utility_detail {
using ojson = nlohmann::ordered_json;

ojson utilError(const std::string& message, const std::string& code = "engine_error");

ojson utilSyntax(const std::string& message);

ojson utilErrorRc(const std::string& message, int rc);

bool isErrorBlock(const ojson& b);

const ojson* firstError(const ojson& result);

ojson displayBlock(const std::string& text);

ojson silent();

std::string lowerOf(std::string s);

std::string trimOf(const std::string& s);

std::string upperAscii(std::string s);

std::string dequote(const std::string& s);

std::vector<std::string> splitWs(const std::string& s);

std::string joinWith(const std::vector<std::string>& v, const std::string& sep);

std::string rightAlign(const std::string& s, size_t w);

bool parseStrictDouble(const std::string& sIn, double* out);

bool parseStrictInt(const std::string& s, long long* out);

std::string intText(int64_t v);

std::string numText(double v);

const eval::OptValue* findOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                              const std::string& key);

bool hasFlag(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
             const std::string& key);

std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::string& key);

std::optional<std::string> optStrAny(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
    std::initializer_list<const char*> keys);

ojson refuseQualifiers(const eval::ParsedCommand& parsed, const std::string& why);

std::string numCell(double v);

std::optional<ojson> writeColumn(SessionState& session, const std::string& basePath,
                                 const std::string& var, const std::vector<std::string>& cells,
                                 const std::string& dtype, bool isNew, std::string* outPath);



// A whole table on its way to `cobra convert` (filelist's only write shape).
struct CsvTable {
  std::vector<std::string> names;
  std::vector<char> isFactor;                  // parallel to `names`
  std::vector<std::vector<std::string>> rows;  // rows[r][c]; "" == missing
};

std::optional<ojson> writeAndConvert(SessionState& session, const CsvTable& t,
                                     const std::string& prefix, std::string* outPath);



// ===========================================================================
// Reading
// ===========================================================================

// THE SHARED READ SHAPE, in two flavours.
//
//  - No qualifier: ask the worker for a contract_A section-38 projection of
//    just the columns this command needs, and fall back to the session's own
//    materialized head whenever the projection is unavailable. A miss must cost
//    a fallback, never an answer -- column_fetch.h's own law.
//  - `if`/`in` present: `applyIfAndInFilter` writes a FILTERED scratch hbin and
//    this reads that. Row identity is lost by construction, which is exactly
//    why only the read-only verbs (unique's report form, distinct) accept a
//    qualifier; every verb that WRITES a column refuses one by name instead of
//    quietly producing a short column or a misaligned one.
class ReadScope {
 public:
  ReadScope(SessionState& session, const eval::ParsedCommand& parsed,
            const std::vector<std::string>& cols, const std::string& verb, bool honourQualifiers) {
    const bool wantFilter =
        honourQualifiers && ((parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
                             (parsed.inRange.has_value() && !parsed.inRange->empty()));
    if (wantFilter) {
      if (!session.hasData()) {
        err_ = ojson(eval::noDataLoaded());
        return;
      }
      try {
        InRangeFilterResult f =
            applyIfAndInFilter(session, *session.currentHbin(), parsed.ifExpr, parsed.inRange);
        if (!f.ok) {
          err_ = inRangeRejectError(f, parsed.inRange.value_or(std::string()), verb);
          return;
        }
        if (f.dataPath != *session.currentHbin()) {
          // BUG-381: this filtered copy is READ and abandoned -- it never
          // becomes the head, so only the section-38 projection ledger can
          // reach it.
          session.noteProjectionScratch(f.dataPath);
          owned_ = std::make_unique<Datastore>(f.dataPath);
          ds_ = owned_.get();
        } else {
          ds_ = session.datastore();
        }
      } catch (const std::exception& e) {
        err_ = utilError(verb + ": could not apply the qualifier -- " + std::string(e.what()),
                         "data_io");
      }
      return;
    }
    try {
      if (!cols.empty()) {
        std::vector<const std::string*> exprs;
        exprs.reserve(cols.size());
        for (const auto& c : cols) exprs.push_back(&c);
        if (auto want = referencedColumns(session, exprs, "")) {
          std::string projPath;
          std::unique_ptr<Datastore> proj = fetchColumns(session, *want, session.workdir(),
                                                         &projPath);
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
      }
      ds_ = owned_ ? owned_.get() : session.datastore();
    } catch (const std::exception& e) {
      err_ = utilError(verb + ": could not read the data -- " + std::string(e.what()), "data_io");
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



// A non-owning view of one column, type-erased just enough that the tuple/key
// code below never asks a second time what kind of column it is holding.
struct ColView {
  bool isFactor = false;
  const double* d = nullptr;
  const int32_t* codes = nullptr;
  const std::vector<std::string>* levels = nullptr;
};

ColView viewOf(const Datastore& ds, const std::string& name);

bool cellMissing(const ColView& v, int64_t i);

uint64_t cellKey(const ColView& v, int64_t i);

std::vector<std::string> columnAsStrings(const Datastore& ds, const std::string& name);

ojson newVarGuard(SessionState& session, const std::string& verb, const std::string& newvar);

std::optional<ojson> resolveVarlist(const Datastore& ds, const std::string& verb,
                                    const std::vector<std::string>& asked,
                                    std::vector<std::string>* out);

int64_t missingCountOf(SessionState& session, Datastore& ds, const std::string& name);

std::vector<int64_t> groupIds(const Datastore& ds, const std::vector<std::string>& byVars,
                              int64_t* nGroupsOut);

std::optional<ojson> materializeIndicator(SessionState& session, const std::string& verb,
                                          const std::string& expr, std::string* nameOut);
}
