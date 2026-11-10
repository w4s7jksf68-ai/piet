// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once

#include "piet/handlers/graph.h"

namespace piet::handlers::graph_detail {
using ojson = nlohmann::ordered_json;

bool isKernelName(const std::string& word);

double viaMacroText(double d);

struct HistOpts {
  HistGenSpec spec;
  std::optional<ojson> err;
};

HistOpts parseHistOpts(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                       const std::string& varname, bool genFloat, bool firstWins = false);

ojson barList(const HistGenResult& g);

serset::Serset barSerset(const HistGenResult& g, bool genFloat);

serset::Serset curveWithVariableSerset(SessionState& session, Datastore& full, const std::string& var,
                                       const std::vector<double>& curve, const std::vector<double>& grid);

std::vector<double> accumulatedGrid(double lo, double hi, int64_t count);

struct KernelChoice {
  std::string name = "epanechnikov";
  std::optional<ojson> err;
};

KernelChoice chooseKernel(const std::vector<std::pair<std::string, eval::OptValue>>& opts, bool strictWords);

struct BandwidthChoice {
  double h = 0.0;
  std::optional<ojson> err;
};

BandwidthChoice chooseBandwidth(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                const std::string& conflictMessage);
}  // namespace piet::handlers::graph_detail
