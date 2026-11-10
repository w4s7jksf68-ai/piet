// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

#include <optional>
#include <string>
#include <vector>

namespace piet {
namespace handlers {

using ojson = nlohmann::ordered_json;

namespace engine_report_detail {

// The twoway graph block rdplot and binscatter build from already-computed
// point layers. Definitions (and their comments) live in graph_point_layers.cpp.
ojson qeGraphLayer(const std::string& kind, ojson points, int layerIdx);
ojson qeGraphBlock(const std::vector<ojson>& layers, const std::string& title,
                   const std::string& xlabel, const std::string& ylabel,
                   SessionState& session, std::optional<double> xlineAt = std::nullopt);
ojson registerQeGraph(ojson block, SessionState& session);

}  // namespace engine_report_detail

}  // namespace handlers
}  // namespace piet
