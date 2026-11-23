// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/data_combine.h"
#include "piet/handlers/variables.h"  // isValidDisplayFormat: xpose, format(%fmt)
#include "piet/dispatcher.h"  // dispatch(): cumul marks its if/in sample with generate

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <regex>
#include <ctime>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data/frame_internal.h"  // canonical numeric storage rounding for recast
#include "data/missing.h"         // cumul's by() groups: each missing code is its own
#include "core/rng.h"         // Philox4x32-10, the engine's own counter-based RNG
#include "core/rng_mt64.h"    // Stata's session stream: bsample's fast path
#include "estimation/bsample_draws.h"    // the fast path's draw, shared with the bootstrap prefix
#include "core/rng_normal.h"  // uniform01 / normal_icdf (AS241) -- drawnorm
#include "io/win_compat.h"    // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"  // piet::fmtStataDefaultG — Stata's %9.0g default
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"  // pythonFloatRepr -- round-trippable CSV numerics
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/storage_type.h"  // FU-944: input's c(type) and float store
#include "piet/serset.h"        // serset::storageType: xpose, promote
#include "piet/strutil.h"
#include "estimation/estimation_internal.h"  // esampleHeadMask / esampleRewriteHeadMask
#include "engine/scratch_columns.h"          // cumul's if/in restriction and weight columns
#include "dataset_shape_support.h"
#include "dataset_shape_options.h"

namespace piet::handlers {

using namespace dataset_shape_detail;

namespace {

ojson echoBlock(const std::string& text) {
  ojson b;
  b["type"] = "echo";
  b["text"] = text;
  return b;
}

}  // namespace

// ===========================================================================
// xpose
// ===========================================================================
//
// base/x/xpose.ado (StataBE 19.5), and the vs-stata bank (2026-11-10):
//  - `clear` is required, r(198); no observations r(2000); no variables
//    "(no variables)" r(2000).
//  - EVERY variable becomes an observation, in dataset order; a string variable
//    becomes a row of missing values (Read_out: `drop v ; gen byte v = .`), it is
//    not dropped (auto `keep make price` -> 2 observations).
//  - The new variables are v1..vN, of type c(type) -- or, with `promote`, the
//    narrowest type that holds every numeric source type -- so under the default
//    `set type float` the values are stored as floats.
//  - An existing `_varname` names the new variables (row i's value names v_i)
//    and is not itself transposed; `varname` adds `_varname` (the source names)
//    as the LAST variable.
//  - r() is what xpose's last r-class step left: `file read` -> r(eof) = 1.
//  - `format(%fmt)` formats every new variable %fmt (`format _all %fmt`, so a
//    text that is not a format is read as variable names: `format(abc)` is
//    r(111) "variable abc not found" and the data are left as they were);
//    bare `format` uses the format of the widest numeric, non-date, non-string
//    source variable (the first of equal width), and none when there is none.

namespace {

bool xposeValidName(const std::string& nm) {
  if (nm.empty() || nm.size() > 32) return false;
  const unsigned char c0 = static_cast<unsigned char>(nm[0]);
  if (!(std::isalpha(c0) || nm[0] == '_')) return false;
  for (unsigned char c : nm)
    if (!(std::isalnum(c) || c == '_')) return false;
  return true;
}

double xposeStore(double v, const std::string& type) {
  if (std::isnan(v)) return v;
  if (type == "float") {
    if (std::fabs(v) > static_cast<double>(std::numeric_limits<float>::max()))
      return std::numeric_limits<double>::quiet_NaN();
    return static_cast<double>(static_cast<float>(v));
  }
  return v;
}

}  // namespace

ojson runXpose(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabXpose.known,
                                            kVocabXpose.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "xpose")) return *q;
  if (!hasFlag(parsed.opts, "clear")) return rsErrorRc("you must specify ', clear'", 198);
  if (!session.hasData()) return rsErrorRc("no observations", 2000);
  const bool wantVarname = hasFlag(parsed.opts, "varname");
  const bool promote = hasFlag(parsed.opts, "promote");
  // format / format(): xpose.ado's `syntax` reads both into one name pair; given
  // twice (once bare, once with a value) they are r(198).
  bool formatBare = false;
  std::optional<std::string> formatText;
  for (const auto& [k, v] : parsed.opts) {
    if (k != "format") continue;
    if (v.isBool) formatBare = true;
    else formatText = strutil::trim(v.strValue);
  }
  if (formatBare && formatText.has_value() && !formatText->empty())
    return rsErrorRc("format() and format are mutually exclusive", 198);

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("xpose: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return rsErrorRc("no observations", 2000);
  const int64_t n = ds->nRows();
  if (n == 0) return rsErrorRc("no observations", 2000);
  const std::vector<std::string> before = ds->names();
  if (before.empty()) return rsErrorRc("(no variables)", 2000);

  // An existing _varname supplies the new names, one per observation.
  std::vector<std::string> newNames;
  bool haveVns = false;
  for (const auto& nm : before) haveVns = haveVns || nm == "_varname";
  if (haveVns) {
    const cecon::Column& c = ds->col("_varname");
    for (int64_t i = 0; i < n; ++i) {
      std::string vn;
      if (c.dtype == cecon::DType::Factor) {
        const int32_t code = c.codes()[i];
        if (code >= 0 && static_cast<size_t>(code) < c.levels.size())
          vn = c.levels[static_cast<size_t>(code)];
      } else {
        vn = numCell(c.f64()[i]);
      }
      newNames.push_back(strutil::trim(vn));
    }
  }
  std::vector<std::string> invars;
  for (const auto& nm : before)
    if (nm != "_varname") invars.push_back(nm);
  if (invars.empty()) return rsErrorRc("varlist required", 100);

  // The storage type of the new variables.
  std::string newType = session.defaultStorageType.empty() ? std::string("float")
                                                           : session.defaultStorageType;
  if (promote) {
    bool hasByte = false, hasInt = false, hasLong = false, hasFloat = false, hasDouble = false;
    for (const auto& nm : invars) {
      if (ds->dtype(nm) != "f64") continue;
      const std::string t = serset::storageType(session, *ds, nm);
      hasDouble = hasDouble || t == "double";
      hasFloat = hasFloat || t == "float";
      hasLong = hasLong || t == "long";
      hasInt = hasInt || t == "int";
      hasByte = hasByte || t == "byte";
    }
    if (hasDouble || (hasFloat && hasLong)) newType = "double";
    else if (hasFloat) newType = "float";
    else if (hasLong) newType = "long";
    else if (hasInt) newType = "int";
    else newType = "byte";
  }

  std::vector<std::string> outNames;
  for (int64_t i = 0; i < n; ++i) {
    std::string nm = "v" + std::to_string(i + 1);
    if (haveVns && i < static_cast<int64_t>(newNames.size()) && !newNames[static_cast<size_t>(i)].empty())
      nm = newNames[static_cast<size_t>(i)];
    outNames.push_back(nm);
  }
  {
    std::set<std::string> seen;
    for (const auto& nm : outNames) {
      if (!xposeValidName(nm)) return rsErrorRc(nm + " invalid name", 198);
      if (!seen.insert(nm).second) return rsErrorRc("variable " + nm + " already defined", 110);
    }
  }

  // The format the new variables get (applied after the transpose, as the ado's
  // `format _all fmt`).
  std::string newFormat;
  if (formatBare) {
    long long widest = 0;
    const auto fmts = session.displayFormatMap(ds->formatCard(invars));
    for (const auto& [nm, f] : fmts) {
      if (ds->dtype(nm) != "f64") continue;  // a string source is a str format
      if (f.find_first_of("sdDmMyY") != std::string::npos) continue;
      std::string w = f;
      w.erase(std::remove(w.begin(), w.end(), '-'), w.end());
      w = w.size() > 1 ? w.substr(1, 2) : std::string();
      w.erase(std::remove(w.begin(), w.end(), '.'), w.end());
      long long width = 0;
      if (!parseStrictInt(w, &width)) continue;
      if (widest < width) {
        widest = width;
        newFormat = f;
      }
    }
  } else if (formatText.has_value() && !formatText->empty()) {
    if ((*formatText)[0] == '%') {
      if (!isValidDisplayFormat(*formatText))
        return rsErrorRc("invalid %format", 120);
      newFormat = *formatText;
    } else {
      // `format _all <text>`: the words are variable names of the transposed data.
      std::istringstream words(*formatText);
      for (std::string w; words >> w;) {
        const bool known = std::find(outNames.begin(), outNames.end(), w) != outNames.end() ||
                           (wantVarname && w == "_varname");
        if (!known) return rsErrorRc("variable " + w + " not found", 111);
      }
    }
  }

  // The transpose: one row per ORIGINAL variable, one column per original
  // observation; `varname` appends the source names as `_varname`.
  CsvTable t;
  for (const auto& nm : outNames) {
    t.names.push_back(nm);
    t.isFactor.push_back(0);
  }
  if (wantVarname) {
    t.names.push_back("_varname");
    t.isFactor.push_back(1);
  }
  t.rows.reserve(invars.size());
  for (const auto& nm : invars) {
    std::vector<std::string> row;
    row.reserve(t.names.size());
    if (ds->dtype(nm) == "f64") {
      const double* d = ds->col(nm).f64();
      for (int64_t i = 0; i < n; ++i) row.push_back(numCell(xposeStore(d[i], newType)));
    } else {
      for (int64_t i = 0; i < n; ++i) row.emplace_back();
    }
    if (wantVarname) row.push_back(nm);
    t.rows.push_back(std::move(row));
  }

  std::string outPath;
  if (auto err = writeAndConvert(session, t, "xpose", &outPath)) return *err;
  session.load(outPath);
  if (auto bad = assertRowCount(session, "xpose", static_cast<int64_t>(invars.size())))
    return *bad;
  for (const auto& nm : outNames) {
    session.varMeta.erase(nm);
    storage::recordCreateType(session, nm, newType, newType);
    if (!newFormat.empty()) session.varMeta[nm].format = newFormat;
  }
  if (wantVarname) {
    // `gen str8 _varname = ""`, then each name replaced in: str8, or the
    // longest name's width (auto: str12 for displacement).
    size_t w = 8;
    for (const auto& nm : invars) w = std::max(w, nm.size());
    session.varMeta.erase("_varname");
    session.varMeta["_varname"].stataType = "str" + std::to_string(w);
    session.implicitFloatVars.erase("_varname");
  }

  eval::replaceRClass(session.eval);
  session.eval.rValues.set("eof", 1.0);
  eval::declareRPosted(session.eval, "xpose", {"eof"});
  return mutationResult(session, "xpose", t.names, before);
}

}  // namespace piet::handlers
