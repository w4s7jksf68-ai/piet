// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// graph_registration.cpp — the graph registration lifecycle: name() parsing and
// collision, registerGraph, describe metadata and scheme sizes, and the sersets
// a graph owns. See include/piet/serset.h.
#include "piet/serset.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>

#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/session.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "io/win_compat.h"
#include "serset_internal.h"

namespace piet::serset {

using ojson = nlohmann::ordered_json;
using namespace serset_detail;

// ---------------------------------------------------------------------------
// Graph lifecycle

GraphName parseNameOption(const std::string& nameOpt) {
  GraphName g;
  const std::string t = trimText(nameOpt);
  if (t.empty()) return g;
  static const std::regex reSplit(R"([,\s]+)");
  std::sregex_token_iterator it(t.begin(), t.end(), reSplit, -1), endIt;
  bool first = true;
  for (; it != endIt; ++it) {
    const std::string tok = it->str();
    if (tok.empty()) continue;
    if (first) {
      g.name = tok;
      first = false;
    } else if (strutil::lower(tok) == "replace") {
      g.replace = true;
    }
  }
  return g;
}

std::optional<ojson> nameCollision(SessionState& session, const std::string& nameOpt) {
  const GraphName g = parseNameOption(nameOpt);
  if (g.name.empty() || g.replace) return std::nullopt;
  if (session.namedGraphs.count(g.name) == 0) return std::nullopt;
  return refusal("graph " + g.name + " already exists", 110);
}

void registerGraph(SessionState& session, ojson& block, const std::string& nameOpt,
                   std::vector<Serset> sets, const ojson* meta) {
  const GraphName g = parseNameOption(nameOpt);
  const std::string nm = g.name.empty() ? std::string("Graph") : g.name;
  session.sersets.dropOwner(nm);
  for (Serset& s : sets) {
    s.owner = nm;
    session.sersets.add(std::move(s));
  }
  session.sersets.setLastOwner(nm);
  session.lastGraph = block;
  session.namedGraphs[nm] = block;
  // graph-family AI-4: the describe record and the current graph.
  session.graphMeta[nm] = meta != nullptr && meta->is_object() ? *meta : makeGraphMeta("twoway", "", "");
  session.currentGraph = nm;
  if (!g.name.empty()) block["name"] = nm;
}

std::pair<std::string, std::string> schemeGraphSize(const std::string& schemeIn) {
  // {scheme, its #include parent, graphsize y, graphsize x} as shipped in
  // /Applications/StataNow/ado/base/s/scheme-*.scheme ("" = inherited).
  struct Row {
    const char* name;
    const char* parent;
    const char* y;
    const char* x;
  };
  static const Row kRows[] = {
      {"s2color", "", "4", "5.5"},          {"s2color8", "s2color", "", ""},
      {"s2mono", "s2color", "", ""},        {"s1color", "s2color", "", ""},
      {"s1mono", "s1color", "", ""},        {"s1rcolor", "s1color", "", ""},
      {"meta", "s2color", "", ""},          {"economist", "s2color", "2.2", "2.2"},
      {"s1manual", "s1mono", "2.392", "3.12"}, {"s2gcolor", "s2color", "2.392", "3.12"},
      {"s2gmanual", "s2mono", "2.392", "3.12"}, {"s2manual", "s2mono", "2.392", "3.12"},
      {"sj", "s2mono", "2.6", "3.575"},     {"stsj", "s2mono", "2.6", "3.575"},
      {"stcolor", "s2color", "4.5", "7.5"}, {"stcolor_alt", "stcolor", "4", "6"},
      {"stcolor_meta", "stcolor", "", ""},  {"stcolor_mv", "stcolor_alt", "", ""},
      {"stcoloralias", "stcolor", "", ""},  {"stgcolor", "stcolor", "2.392", "3.987"},
      {"stgcolor_alt", "stcolor_alt", "2.392", "3.588"},
      {"stgcolor_meta", "stcolor_meta", "2.392", "3.987"},
      {"stgcolor_mv", "stgcolor_alt", "", ""}, {"stmono1", "s1mono", "4.5", "7.5"},
      {"stmono2", "s2mono", "4.5", "7.5"},  {"sttrans", "s2color", "4.5", "7.5"},
  };
  std::string cur = strutil::lower(schemeIn);
  std::string y, x;
  for (int guard = 0; guard < 16 && !cur.empty(); ++guard) {
    const Row* row = nullptr;
    for (const Row& r : kRows)
      if (cur == r.name) row = &r;
    if (row == nullptr) break;
    if (y.empty() && row->y[0] != '\0') y = row->y;
    if (x.empty() && row->x[0] != '\0') x = row->x;
    if (!y.empty() && !x.empty()) break;
    cur = row->parent;
  }
  if (y.empty()) y = "4.5";
  if (x.empty()) x = "7.5";
  return {y, x};
}

ojson makeGraphMeta(const std::string& family, const std::string& command, const std::string& scheme) {
  ojson m;
  m["family"] = family;
  m["command"] = command;
  const std::string sch = scheme.empty() ? std::string("stcolor") : scheme;
  m["scheme"] = sch;
  const auto sz = schemeGraphSize(sch);
  m["ysize"] = sz.first;
  m["xsize"] = sz.second;
  std::time_t now = std::time(nullptr);
  std::tm tmv{};
#if defined(_WIN32)
  localtime_s(&tmv, &now);
#else
  localtime_r(&now, &tmv);
#endif
  char d[32], t[32];
  std::strftime(d, sizeof d, "%b %Y", &tmv);
  std::strftime(t, sizeof t, "%H:%M:%S", &tmv);
  // The day unpadded: r(command_date) is "3 Nov 2026" (StataBE 19.5, measured
  // 2026-11-03, scratch-laneA2/agents/gfx/g2.do; c(current_date) " 3 Nov 2026").
  m["date"] = std::to_string(tmv.tm_mday) + " " + d;
  m["time"] = std::string(t);
  return m;
}

void dropGraphSersets(SessionState& session, const std::vector<std::string>& names, bool all) {
  if (all) {
    session.sersets.clear();
    return;
  }
  for (const auto& nm : names) session.sersets.dropOwner(nm);
}

std::vector<Serset> ownedBy(SessionState& session, const std::string& owner) {
  std::vector<Serset> out;
  for (const auto& kv : session.sersets.all())
    if (kv.second.owner == owner) out.push_back(kv.second);
  return out;
}

}  // namespace piet::serset
