// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// serset_store.cpp — the serset store, the column constructors, the generic
// data-plot serset builder and the .gph (de)serialisation of sersets. See
// include/piet/serset.h for the contract and the oracle lines it rests on.
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

namespace {

constexpr int kMaxSersetId = 1999;

// Rows of `s` reordered by `order` (indices into the current rows).
void permuteRows(Serset& s, const std::vector<size_t>& order) {
  for (Column& c : s.cols) {
    if (c.isStr) {
      std::vector<std::string> v;
      v.reserve(order.size());
      for (size_t i : order) v.push_back(i < c.str.size() ? c.str[i] : std::string());
      c.str = std::move(v);
    } else {
      std::vector<double> v;
      v.reserve(order.size());
      for (size_t i : order) v.push_back(i < c.num.size() ? c.num[i] : kNaN);
      c.num = std::move(v);
    }
  }
}

// Stable ascending sort by column `k`, missing last (Stata's sort order).
void sortByColumn(Serset& s, int k) {
  if (k < 0 || static_cast<size_t>(k) >= s.cols.size()) return;
  const Column& key = s.cols[static_cast<size_t>(k)];
  std::vector<size_t> order(static_cast<size_t>(s.n));
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  if (key.isStr) {
    std::stable_sort(order.begin(), order.end(),
                     [&key](size_t a, size_t b) { return key.str[a] < key.str[b]; });
  } else {
    std::stable_sort(order.begin(), order.end(), [&key](size_t a, size_t b) {
      const double x = key.num[a], y = key.num[b];
      const bool mx = std::isnan(x), my = std::isnan(y);
      if (mx || my) return !mx && my;
      return x < y;
    });
  }
  permuteRows(s, order);
}

bool hasColumn(const Serset& s, const std::string& name) {
  for (const Column& c : s.cols)
    if (c.name == name) return true;
  return false;
}

// The name a duplicate claim appends under: <name>1, <name>2, ... first free.
std::string freshName(const Serset& s, const std::string& base) {
  for (int k = 1;; ++k) {
    std::string cand = base + std::to_string(k);
    if (!hasColumn(s, cand)) return cand;
  }
}


// Adds a data plot's columns to `s` (rule (c) of serset.h): each variable takes
// the first column stored under its name that this plot has not used yet, else
// a new one (StataBE 19.5, BUG-1543: `(line y y) (line y1 x)` stores `y y y1 x`
// and loads `y y1 y11 x`, the real y1 in y11).
void mergePlotColumns(Serset& s, const std::vector<Column>& cols) {
  std::vector<bool> claimed(s.cols.size(), false);
  for (const Column& c : cols) {
    size_t k = 0;
    while (k < claimed.size() && (claimed[k] || storedName(s.cols[k]) != c.name)) ++k;
    if (k < claimed.size()) {
      claimed[k] = true;
      continue;
    }
    Column copy = c;
    if (hasColumn(s, c.name)) {
      copy.stored = c.name;
      copy.name = freshName(s, c.name);
    }
    s.cols.push_back(std::move(copy));
    claimed.push_back(true);
  }
}

int64_t cellsOf(const Serset& s) {
  return s.n * static_cast<int64_t>(std::max<size_t>(1, s.cols.size()));
}

// Drops the values of an oversize serset (its shape stays).
void enforceRetention(Serset& s) {
  if (cellsOf(s) <= kMaxRetainedCells) return;
  s.retained = false;
  for (Column& c : s.cols) {
    std::vector<double>().swap(c.num);
    std::vector<std::string>().swap(c.str);
  }
}
}  // namespace

// ---------------------------------------------------------------------------
// Store

int Store::add(Serset s) {
  int id = 0;
  while (id <= kMaxSersetId && sets_.count(id) != 0) ++id;
  if (id > kMaxSersetId) return -1;
  enforceRetention(s);
  sets_[id] = std::move(s);
  current_ = id;
  return id;
}

bool Store::select(int id) {
  if (sets_.count(id) == 0) return false;
  current_ = id;
  return true;
}

const Serset* Store::get(int id) const {
  auto it = sets_.find(id);
  return it == sets_.end() ? nullptr : &it->second;
}

void Store::drop(int id) {
  sets_.erase(id);
  if (id == current_) current_ = -1;
}

void Store::dropOwner(const std::string& owner) {
  for (auto it = sets_.begin(); it != sets_.end();) {
    if (it->second.owner == owner) {
      if (it->first == current_) current_ = -1;
      it = sets_.erase(it);
    } else {
      ++it;
    }
  }
}

void Store::renameOwner(const std::string& from, const std::string& to) {
  for (auto& kv : sets_)
    if (kv.second.owner == from) kv.second.owner = to;
  if (lastOwner_ == from) lastOwner_ = to;
}

void Store::clear() {
  sets_.clear();
  current_ = -1;
  lastOwner_.clear();
}

int64_t Store::bytes() const {
  int64_t total = 0;
  for (const auto& kv : sets_) {
    for (const Column& c : kv.second.cols) {
      total += static_cast<int64_t>(c.name.size() + c.type.size());
      total += static_cast<int64_t>(c.num.size()) * 8;
      for (const auto& s : c.str) total += static_cast<int64_t>(s.size());
    }
  }
  return total;
}

// ---------------------------------------------------------------------------
// Column helpers

std::string tempName(int i) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "__%06d", i);
  return buf;
}

double toFloat(double v) {
  if (std::isnan(v)) return v;
  return static_cast<double>(static_cast<float>(v));
}

Column numericColumn(std::string name, std::string type, std::vector<double> values) {
  Column c;
  c.name = std::move(name);
  c.type = std::move(type);
  if (c.type == "float") {
    for (double& v : values) v = toFloat(v);
  }
  c.num = std::move(values);
  return c;
}

Column stringColumn(std::string name, std::vector<std::string> values, std::string type) {
  Column c;
  c.name = std::move(name);
  c.isStr = true;
  if (type.empty()) {
    size_t w = 1;
    for (const auto& s : values) w = std::max(w, s.size());
    type = "str" + std::to_string(w);
  }
  c.type = std::move(type);
  c.str = std::move(values);
  return c;
}

std::string storageType(SessionState& session, Datastore& ds, const std::string& var) {
  auto it = session.varMeta.find(var);
  if (it != session.varMeta.end() && it->second.stataType.has_value() &&
      !it->second.stataType->empty()) {
    return *it->second.stataType;
  }
  if (ds.has(var) && ds.dtype(var) == "f64") return "float";
  size_t w = 1;
  if (ds.has(var)) {
    for (const auto& lvl : ds.col(var).levels) w = std::max(w, lvl.size());
  }
  return "str" + std::to_string(w);
}

Column variableColumn(SessionState& session, Datastore& ds, const std::string& var,
                      const std::vector<int64_t>& rows, const std::string& name) {
  Column c;
  c.name = name.empty() ? var : name;
  c.type = storageType(session, ds, var);
  if (!ds.has(var)) {
    c.num.assign(rows.size(), kNaN);
    return c;
  }
  const cecon::Column& col = ds.col(var);
  const int64_t n = ds.nRows();
  if (ds.dtype(var) == "f64") {
    const double* d = col.f64();
    c.num.reserve(rows.size());
    for (int64_t r : rows) c.num.push_back(r >= 0 && r < n ? d[r] : kNaN);
  } else {
    c.isStr = true;
    const int32_t* codes = col.codes();
    c.str.reserve(rows.size());
    for (int64_t r : rows) {
      int32_t code = (r >= 0 && r < n) ? codes[r] : -1;
      if (code >= 0 && static_cast<size_t>(code) < col.levels.size()) {
        c.str.push_back(col.levels[static_cast<size_t>(code)]);
      } else {
        c.str.emplace_back();
      }
    }
  }
  return c;
}

// ---------------------------------------------------------------------------
// The generic builder

std::vector<Serset> buildPlotSersets(std::vector<PlotSpec> plots) {
  struct Group {
    Serset s;
    std::string key;
    bool shareable = false;
  };
  std::vector<Group> groups;
  for (PlotSpec& p : plots) {
    if (p.computed.has_value()) {
      Group g;
      g.s = std::move(*p.computed);
      groups.push_back(std::move(g));
      continue;
    }
    const int64_t n = p.cols.empty() ? 0 : static_cast<int64_t>(p.cols.front().size());
    if (p.sorted) {
      Group g;
      g.s.n = n;
      mergePlotColumns(g.s, p.cols);
      sortByColumn(g.s, p.sortCol);
      groups.push_back(std::move(g));
      continue;
    }
    Group* target = nullptr;
    for (Group& g : groups) {
      if (g.shareable && g.key == p.key && g.s.n == n) {
        target = &g;
        break;
      }
    }
    if (target == nullptr) {
      Group g;
      g.key = p.key;
      g.shareable = true;
      g.s.n = n;
      groups.push_back(std::move(g));
      target = &groups.back();
    }
    mergePlotColumns(target->s, p.cols);
  }
  std::vector<Serset> out;
  out.reserve(groups.size());
  for (Group& g : groups) out.push_back(std::move(g.s));
  return out;
}

// ---------------------------------------------------------------------------
// .gph persistence

ojson toJson(const std::vector<Serset>& sets) {
  ojson arr = ojson::array();
  for (const Serset& s : sets) {
    ojson js;
    js["n"] = s.n;
    js["retained"] = s.retained;
    ojson cols = ojson::array();
    for (const Column& c : s.cols) {
      ojson jc;
      jc["name"] = c.name;
      if (!c.stored.empty()) jc["stored"] = c.stored;  // BUG-1543
      jc["type"] = c.type;
      if (c.isStr) {
        jc["str"] = c.str;
      } else {
        ojson vals = ojson::array();
        for (double v : c.num) {
          if (std::isnan(v)) vals.push_back(nullptr);
          else vals.push_back(v);
        }
        jc["num"] = vals;
      }
      cols.push_back(jc);
    }
    js["cols"] = cols;
    arr.push_back(js);
  }
  return arr;
}

std::vector<Serset> fromJson(const ojson& j) {
  std::vector<Serset> out;
  if (!j.is_array()) return out;
  for (const auto& js : j) {
    if (!js.is_object()) continue;
    Serset s;
    s.n = js.value("n", static_cast<int64_t>(0));
    s.retained = js.value("retained", true);
    for (const auto& jc : js.value("cols", ojson::array())) {
      Column c;
      c.name = jc.value("name", std::string());
      c.stored = jc.value("stored", std::string());
      c.type = jc.value("type", std::string("double"));
      if (jc.contains("str")) {
        c.isStr = true;
        for (const auto& v : jc["str"]) c.str.push_back(v.is_string() ? v.get<std::string>() : "");
      } else {
        for (const auto& v : jc.value("num", ojson::array()))
          c.num.push_back(v.is_number() ? v.get<double>() : kNaN);
      }
      s.cols.push_back(std::move(c));
    }
    out.push_back(std::move(s));
  }
  return out;
}

}  // namespace piet::serset
