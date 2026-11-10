// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// The one definition of each helper shared by the column-writing commands (mvdecode,
// integ, sencode, fillmissing, astile); declared in column_write_support.h.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "vendor/nlohmann/json.hpp"
#include "piet/loops.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"
#include "piet/storage_type.h"
#include "piet/strutil.h"
#include "column_write_support.h"

// integ's spline must round every product and sum separately, as StataBE's expression
// evaluator does; a fused multiply-add changes the last bit (integ_basic is pinned to it).
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace piet::handlers::column_write_detail {

// ===========================================================================
// Blocks and errors
// ===========================================================================

// An error with the exact Stata rc in `reason` (the one channel coarseRc reads first).
ojson rcError(const std::string& message, int rc, const std::string& code) {
  return ojson(eval::errorBlock(code, message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

ojson syntaxError(const std::string& message) { return rcError(message, 198, "syntax_error"); }

ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

namespace {

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

}  // namespace

std::optional<ojson> firstError(const ojson& result) {
  if (isErrorBlock(result)) return result;
  if (result.is_array())
    for (const auto& b : result)
      if (isErrorBlock(b)) return b;
  return std::nullopt;
}

// ===========================================================================
// Options
// ===========================================================================

namespace {

const eval::OptValue* findOpt(const OptList& opts, const std::string& key) {
  for (const auto& [k, v] : opts)
    if (k == key) return &v;
  return nullptr;
}

}  // namespace

bool hasFlag(const OptList& opts, const std::string& key) {
  const eval::OptValue* v = findOpt(opts, key);
  if (v == nullptr) return false;
  return v->isBool ? v->boolValue : true;
}

// A valued option's text, trimmed; nullopt when absent or given bare.
std::optional<std::string> optStr(const OptList& opts, const std::string& key) {
  const eval::OptValue* v = findOpt(opts, key);
  if (v == nullptr || v->isBool) return std::nullopt;
  return strutil::trim(v->strValue);
}

std::vector<std::string> splitWs(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string t;
  while (iss >> t) out.push_back(t);
  return out;
}

std::string joinSpace(const std::vector<std::string>& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += ' ';
    out += v[i];
  }
  return out;
}

bool parseStrictDouble(const std::string& sIn, double* out) {
  const std::string s = strutil::trim(sIn);
  if (s.empty() || s.find_first_of("xXnN") != std::string::npos) return false;
  try {
    size_t pos = 0;
    const double v = std::stod(s, &pos);
    if (pos != s.size()) return false;
    *out = v;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

// A value as a setcol cell: blank = missing, else the shortest round-trip decimal.
std::string numCell(double v) {
  if (std::isnan(v)) return std::string();
  return eval::pythonFloatRepr(v);
}

double toFloat(double v) { return static_cast<double>(static_cast<float>(v)); }

// ===========================================================================
// Variables
// ===========================================================================

// Varlist tokens to dataset names: wildcards, ranges and _all through loops::expandVarlist
// (the one expander), then each token exact or a unique abbreviation, as in any Stata varlist.
std::optional<ojson> resolveVars(SessionState& session, const std::vector<std::string>& toks,
                                 std::vector<std::string>* out) {
  out->clear();
  if (toks.empty()) return std::nullopt;
  loops::VarlistExpandResult ex = loops::expandVarlist(joinSpace(toks), session);
  Datastore* ds = session.datastore();
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  if (ex.isError) {
    // The expander's range error (`x-zz`) carries no rc and names both ends; Stata names the
    // end that is missing, at r(111) (review gk K2).
    const std::string reason = ex.error.value("reason", std::string());
    if (reason.rfind("r(", 0) == 0) return ex.error;
    for (const auto& t : toks) {
      const size_t dash = t.find('-');
      if (dash == std::string::npos || dash == 0) continue;
      for (const std::string& end : {t.substr(0, dash), t.substr(dash + 1)})
        if (!end.empty() && !ds->has(end)) return rcError("variable " + end + " not found", 111);
    }
    return rcError(ex.error.value("message", std::string("variable not found")), 111);
  }
  const std::vector<std::string>& names = ds->names();
  for (const auto& t : ex.tokens) {
    if (ds->has(t)) {
      out->push_back(t);
      continue;
    }
    std::vector<std::string> hits;
    for (const auto& nm : names)
      if (nm.size() > t.size() && nm.compare(0, t.size(), t) == 0) hits.push_back(nm);
    if (hits.size() == 1) {
      out->push_back(hits.front());
      continue;
    }
    if (hits.size() > 1) return rcError(t + " ambiguous abbreviation", 111);
    return rcError("variable " + t + " not found", 111);
  }
  return std::nullopt;
}

bool columnExists(SessionState& session, const std::string& name) {
  Datastore* ds = session.datastore();
  return ds != nullptr && ds->has(name);
}

// Drop scratch columns that exist, through the dispatched `drop` (which also forgets their
// metadata planes). Never fails the caller: a scratch drop that fails leaves a column the
// census will name, which is the louder outcome.
void dropColumns(SessionState& session, const std::vector<std::string>& names) {
  std::vector<std::string> present;
  for (const auto& n : names)
    if (columnExists(session, n)) present.push_back(n);
  if (present.empty()) return;
  try {
    (void)dispatch("quietly drop " + joinSpace(present), session);
  } catch (const std::exception&) {
  }
}

bool hasQualifier(const eval::ParsedCommand& parsed) {
  return (parsed.ifExpr.has_value() && !strutil::trim(*parsed.ifExpr).empty()) ||
         (parsed.inRange.has_value() && !strutil::trim(*parsed.inRange).empty());
}

std::string qualifierText(const eval::ParsedCommand& parsed) {
  std::string q;
  if (parsed.ifExpr.has_value() && !strutil::trim(*parsed.ifExpr).empty())
    q += " if " + *parsed.ifExpr;
  if (parsed.inRange.has_value() && !strutil::trim(*parsed.inRange).empty())
    q += " in " + *parsed.inRange;
  return q;
}

// The if/in sample as a 0/1 row mask, evaluated by the dispatched `generate` (the one
// qualifier evaluator). No qualifier = every row. With `keepAs` the marker column (1 in the
// sample, missing outside) stays on the frame under that name for the caller to use and drop.
std::optional<ojson> sampleMask(SessionState& session, const eval::ParsedCommand& parsed,
                                std::vector<char>* mask, const std::string& keepAs) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();
  mask->assign(static_cast<size_t>(n), 1);
  if (!hasQualifier(parsed)) return std::nullopt;
  const std::string tmp = keepAs.empty() ? "__piet_t1g_touse" : keepAs;
  dropColumns(session, {tmp});
  ojson r = dispatch("quietly generate byte " + tmp + " = 1" + qualifierText(parsed), session);
  if (auto e = firstError(r)) {
    dropColumns(session, {tmp});
    return *e;
  }
  ds = session.datastore();
  if (ds == nullptr || !ds->has(tmp)) return rcError("could not evaluate the if/in qualifier", 198);
  const double* d = ds->col(tmp).f64();
  for (int64_t i = 0; i < n; ++i) (*mask)[static_cast<size_t>(i)] = std::isnan(d[i]) ? 0 : 1;
  if (keepAs.empty()) dropColumns(session, {tmp});
  return std::nullopt;
}

// One column's cells as text ("" = missing): the level text of a string, the round-trip
// decimal of a number. This is the shape a setcol payload takes.
std::vector<std::string> columnCells(const Datastore& ds, const std::string& name) {
  const int64_t n = ds.nRows();
  std::vector<std::string> out;
  out.reserve(static_cast<size_t>(n));
  const cecon::Column& c = ds.col(name);
  if (c.dtype == cecon::DType::Factor) {
    const int32_t* codes = c.codes();
    for (int64_t i = 0; i < n; ++i) {
      const int32_t k = codes[i];
      if (k < 0 || static_cast<size_t>(k) >= c.levels.size())
        out.emplace_back();
      else
        out.push_back(c.levels[static_cast<size_t>(k)]);
    }
  } else {
    const double* d = c.f64();
    for (int64_t i = 0; i < n; ++i) out.push_back(numCell(d[i]));
  }
  return out;
}

std::vector<double> columnDoubles(const Datastore& ds, const std::string& name) {
  const double* d = ds.col(name).f64();
  return std::vector<double>(d, d + ds.nRows());
}

// ===========================================================================
// Writing: one column through `cobra setcol` (in place, or appended with isNew)
// ===========================================================================

std::optional<ojson> runVerb(const std::vector<std::string>& args) {
  CobraRunResult cr = runCobra(args);
  if (cr.ok) return std::nullopt;
  std::string t = cr.stderr_text;
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
  try {
    nlohmann::json payload = nlohmann::json::parse(t);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      CobraError ce(err.value("code", std::string("engine_error")),
                    err.at("message").get<std::string>(), err.value("detail", std::string()));
      return ojson(eval::engineError(cobraErrorMessage(ce), ce.code));
    }
  } catch (const nlohmann::json::exception&) {
  }
  return ojson(eval::engineError(t.empty() ? "cobra failed" : t));
}

// `cells` are rendered values ("" = missing). A string column's cells are quoted so a value's
// own padding survives; an empty line is the payload's missing cell. `storeType` "" = double.
std::optional<ojson> setColumn(SessionState& session, const std::string& var,
                               const std::vector<std::string>& cells, bool isString, bool isNew,
                               const std::string& storeType) {
  const std::string hbinName = mintScratchPath(session.workdir(), "t1gen_vals");
  forgetScratchMint(hbinName);
  const std::string csvPath = hbinName.substr(0, hbinName.size() - 5) + ".csv";
  registerScratchMint(csvPath);
  {
    std::ofstream f(cecon::to_native_path(csvPath));
    if (!f) return rcError("could not stage the values of `" + var + "'", 693, "data_io");
    for (const auto& c : cells) {
      if (c.empty()) {
        f << "\n";
        continue;
      }
      if (isString) {
        f << '"';
        for (char ch : c) {
          if (ch == '"') f << '"';
          f << ch;
        }
        f << '"';
      } else {
        f << c;
      }
      f << "\n";
    }
    f.flush();
    if (!f) {
      cecon::native_unlink(csvPath);
      forgetScratchMint(csvPath);
      return ojson(eval::engineError("could not write the values of `" + var +
                                         "' (disk full or I/O error)",
                                     "data_io"));
    }
  }
  const std::string out = mintScratchPath(session.workdir(), "t1gen_setcol");
  std::vector<std::string> args = {"setcol", "--data", *session.currentHbin(), "--var", var,
                                   "--values", csvPath, "--dtype", isString ? "factor" : "f64"};
  if (!isString) storage::appendStoreTypeArg(args, storeType);
  if (isNew) args.push_back("--new");
  args.push_back("--out");
  args.push_back(out);
  std::optional<ojson> err = runVerb(args);
  cecon::native_unlink(csvPath);
  forgetScratchMint(csvPath);
  if (err) {
    cecon::native_unlink(out);
    return err;
  }
  session.load(out);
  return std::nullopt;
}

// ===========================================================================
// Group keys (by-variables, plus the sample marker where the package groups on it)
// ===========================================================================

KeyCol readKeyCol(const Datastore& ds, const std::string& name) {
  KeyCol k;
  if (ds.col(name).dtype == cecon::DType::Factor) {
    k.isString = true;
    k.str = columnCells(ds, name);
  } else {
    k.num = columnDoubles(ds, name);
    for (double& v : k.num)
      if (v == 0.0) v = 0.0;  // -0 and +0 are one Stata value
  }
  return k;
}

bool keyMissing(const KeyCol& k, size_t i) {
  return k.isString ? k.str[i].empty() : std::isnan(k.num[i]);
}

// Stata's sort order on one key: numbers ascending with missing last, strings by bytes.
int keyCompare(const KeyCol& k, size_t a, size_t b) {
  if (k.isString) {
    const int c = k.str[a].compare(k.str[b]);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
  }
  const double x = k.num[a], y = k.num[b];
  const bool mx = std::isnan(x), my = std::isnan(y);
  if (mx || my) return mx == my ? 0 : (mx ? 1 : -1);
  return x < y ? -1 : (x > y ? 1 : 0);
}

int keysCompare(const std::vector<KeyCol>& keys, size_t a, size_t b) {
  for (const auto& k : keys) {
    const int c = keyCompare(k, a, b);
    if (c != 0) return c;
  }
  return 0;
}

}  // namespace piet::handlers::column_write_detail
