// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once

#include "vendor/nlohmann/json.hpp"

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace graph_extension_detail {

// Shared by graph/grc1leg.cpp, graph/cdfplot.cpp, graph/event_plot.cpp and
// utility/geodesic_distance.cpp; defined in graph/graph_extension_support.cpp.
ojson gxError(const std::string& message, const std::string& code = "engine_error");
ojson gxSyntax(const std::string& message);
bool isErrorBlock(const ojson& b);
ojson renameDelegateError(const ojson& block, const std::string& typedCmd,
                          const std::string& delegate);
void appendBlockNote(ojson& block, const std::string& text);
std::string lowerOf(std::string s);
std::string trimOf(const std::string& s);
std::string dequote(const std::string& s);
std::vector<std::string> splitWs(const std::string& s);
std::string joinWith(const std::vector<std::string>& v, const std::string& sep);
bool parseStrictDouble(const std::string& sIn, double* out);
bool parseStrictInt(const std::string& s, long long* out);
bool hasFlag(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
             const std::string& key);
std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::string& key);
std::vector<std::string> presentOpts(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
    std::initializer_list<const char*> keys);
std::string numCell(double v);
std::vector<double> columnAsDoubles(const Datastore& ds, const std::string& name);
std::vector<std::string> columnAsStrings(const Datastore& ds, const std::string& name);
std::optional<ojson> honourSaving(SessionState& session, const std::string& verb,
                                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const ojson& block);

// ===========================================================================
// Reading
// ===========================================================================

// The shared READ shape: ask the worker for a contract_A section-38 projection
// of just the columns this command needs, and fall back to the session's own
// materialized head whenever the projection is unavailable. A miss must cost a
// fallback, never an answer -- column_fetch.h's own law.
class ReadFrame {
 public:
  ReadFrame(SessionState& session, const std::vector<std::string>& cols) {
    try {
      if (!cols.empty()) {
        std::vector<const std::string*> exprs;
        exprs.reserve(cols.size());
        for (const auto& c : cols) exprs.push_back(&c);
        if (auto want = referencedColumns(session, exprs, "")) {
          std::string projPath;
          owned_ = fetchColumns(session, *want, session.workdir(), &projPath);
        }
      }
      ds_ = owned_ ? owned_.get() : session.datastore();
    } catch (const std::exception& e) {
      ds_ = nullptr;
      why_ = e.what();
    }
  }
  Datastore* get() const { return ds_; }
  const std::string& why() const { return why_; }

 private:
  std::unique_ptr<Datastore> owned_;
  Datastore* ds_ = nullptr;
  std::string why_;
};

}  // namespace graph_extension_detail

}  // namespace piet::handlers
