// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/data_values.h"
#include "piet/handlers/variables.h"
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
#include "piet/strutil.h"
#include "estimation/estimation_internal.h"  // esampleHeadMask / esampleRewriteHeadMask
#include "engine/scratch_columns.h"          // cumul's if/in restriction and weight columns
#include "dataset_shape_support.h"
#include "dataset_shape_options.h"

namespace piet::handlers {

using namespace dataset_shape_detail;

namespace {

// A number the way a Stata macro holds it: 15 significant digits, no leading zero on a
// fraction (`.5`), a missing value as `.`.
std::string macroNum(double v) {
  if (std::isnan(v)) return ".";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.15g", v);
  std::string t = buf;
  if (t.rfind("0.", 0) == 0) t.erase(0, 1);
  else if (t.rfind("-0.", 0) == 0) t.erase(1, 1);
  return t;
}

// The type `compress` leaves a column in: the smallest integer type that holds every
// value when they are all whole numbers, never wider than the type it started as. A
// column of non-whole values keeps its type: compress turns doubles, floats and longs only
// into integer types, never a double into a float.
std::string compressedNumericType(const std::vector<double>& vals, const std::string& start) {
  auto width = [](const std::string& t) {
    if (t == "byte") return 1;
    if (t == "int") return 2;
    if (t == "long" || t == "float") return 4;
    return 8;
  };
  double lo = 0.0, hi = 0.0;
  bool whole = true;
  for (double x : vals) {
    if (std::isnan(x)) continue;
    if (x != std::floor(x) || !std::isfinite(x)) whole = false;
    lo = std::min(lo, x);
    hi = std::max(hi, x);
  }
  std::string best = start;
  if (whole) {
    std::string t;
    if (lo >= -127.0 && hi <= 100.0) t = "byte";
    else if (lo >= -32767.0 && hi <= 32740.0) t = "int";
    else if (lo >= -2147483647.0 && hi <= 2147483620.0) t = "long";
    if (!t.empty() && width(t) < width(start)) best = t;
  }
  return best;
}

}  // namespace

// ===========================================================================
// separate
// ===========================================================================

// separate.ado: one new variable per group of the by() variable, holding varname's value
// in that group and missing elsewhere. A by() that is no variable is an expression that
// must be 0 / 1 / missing (then the labels read `!(expr)` and `expr`, and indicator()
// names the expression for them). The names are generate() or varname plus the group's
// value (every group value a non-negative whole number below 9999) or its running number
// (sequential, or any other by values); a missing group is named `<stub>_` plus its code.
// Each new variable is compressed, keeps varname's display format and value label, and
// the new names come back in r(varlist), in group order, at the end of the data.
ojson runSeparate(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSeparate.known,
                                            kVocabSeparate.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.size() != 1)
    return rsSyntax("separate: syntax is `separate varname, by(groupvar) [generate(stub) "
                    "shortlabel]`");
  const std::string varArg = parsed.varlist[0];
  const std::optional<std::string> byOpt = optStr(parsed.opts, "by");
  if (!byOpt.has_value() || byOpt->empty())
    return rsErrorRc("option by() required", 198);
  const bool shortLabel = hasFlag(parsed.opts, "shortlabel");
  const bool veryShort = hasFlag(parsed.opts, "veryshortlabel");
  const bool useMissing = hasFlag(parsed.opts, "missing");
  const bool sequential = hasFlag(parsed.opts, "sequential");
  if (shortLabel && veryShort)
    return rsErrorRc("options shortlabel and veryshortlabel may not be specified together", 198);
  std::string indicat = optStr(parsed.opts, "indicator").value_or(std::string());
  const std::optional<std::string> genOpt = optStrAny(parsed.opts, {"generate", "gen"});

  std::vector<std::string> vn = {varArg};
  if (auto err = resolveByKeyNames(session, vn)) return *err;
  const std::string var = vn[0];
  {
    Datastore* d0 = session.datastore();
    if (d0 == nullptr) return ojson(eval::noDataLoaded());
    if (!d0->has(var)) return rsErrorRc("variable " + varArg + " not found", 111);
  }

  // by(varname) or by(expression)
  std::string byName;              // the by variable's full name; empty for an expression
  std::vector<std::string> byWords = splitWs(*byOpt);
  if (byWords.size() == 1) {
    std::vector<std::string> bn = {byWords[0]};
    Datastore* d0 = session.datastore();
    if (d0 != nullptr && (d0->has(bn[0]) || [&] {
          for (const auto& nm : d0->names())
            if (nm.size() > bn[0].size() && nm.compare(0, bn[0].size(), bn[0]) == 0) return true;
          return false;
        }())) {
      if (auto err = resolveByKeyNames(session, bn)) return *err;
      byName = bn[0];
    }
  } else if (byWords.size() > 1) {
    bool allVars = true;
    Datastore* d0 = session.datastore();
    for (const auto& w : byWords) allVars = allVars && d0 != nullptr && d0->has(w);
    if (allVars) return rsErrorRc("too many variables specified", 103);
  }
  const std::string byTemp = "__piet_separate_by";
  std::vector<double> byNum;
  std::vector<std::string> byStr;
  bool byIsString = false;
  {
    std::string srcName = byName;
    if (byName.empty()) {
      indicat = *byOpt;
      ojson r = dispatch("quietly generate byte " + byTemp + " = " + *byOpt, session);
      for (const auto& b : r)
        if (b.is_object() && b.value("type", std::string()) == "error") return b;
      srcName = byTemp;
    }
    Datastore* d1 = session.datastore();
    if (d1 == nullptr || !d1->has(srcName)) return rsErrorRc("by(" + *byOpt + ") invalid", 198);
    byIsString = d1->dtype(srcName) == "factor";
    if (byIsString) byStr = columnAsStrings(*d1, srcName);
    else byNum = columnAsDoubles(*d1, srcName);
    if (byName.empty()) {
      ojson dropped = dispatch("quietly drop " + byTemp, session);
      (void)dropped;
      for (double v : byNum)
        if (!std::isnan(v) && v != 0.0 && v != 1.0)
          return rsErrorRc("by(" + *byOpt + ") invalid", 9);
    }
  }

  std::vector<char> inSample;
  if (auto err = ifInMask(session, parsed, &inSample)) return *err;
  Datastore* ds = session.datastore();
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();
  const bool varIsString = ds->dtype(var) == "factor";
  const std::vector<double> srcNum = varIsString ? std::vector<double>() : columnAsDoubles(*ds, var);
  const std::vector<std::string> srcStr = varIsString ? columnAsStrings(*ds, var)
                                                       : std::vector<std::string>();

  // marksample + markout by (a missing by value, or "" for a string, leaves the sample
  // unless missing is given)
  std::vector<char> touse(static_cast<size_t>(n), 0);
  int64_t nUse = 0;
  for (int64_t i = 0; i < n; ++i) {
    const size_t k = static_cast<size_t>(i);
    bool ok = inSample[k] != 0;
    if (ok && !useMissing) ok = byIsString ? !byStr[k].empty() : !std::isnan(byNum[k]);
    touse[k] = ok ? 1 : 0;
    nUse += ok ? 1 : 0;
  }
  if (nUse == 0) return rsErrorRc("no observations satisfy conditions", 2000);

  // The groups, in the order the ado sorts them: by value, a missing value last.
  struct Group { double num = 0.0; std::string str; bool isMissing = false; };
  std::vector<Group> groups;
  std::vector<int64_t> gOf(static_cast<size_t>(n), -1);
  {
    std::set<std::pair<int, std::string>> strKeys;
    std::set<std::pair<int, double>> numKeys;
    for (int64_t i = 0; i < n; ++i) {
      const size_t k = static_cast<size_t>(i);
      if (!touse[k]) continue;
      if (byIsString) strKeys.insert({byStr[k].empty() ? 1 : 0, byStr[k]});
      else numKeys.insert({std::isnan(byNum[k]) ? 1 : 0, std::isnan(byNum[k]) ? 0.0 : byNum[k]});
    }
    std::map<std::pair<int, std::string>, int64_t> sIdx;
    std::map<std::pair<int, double>, int64_t> nIdx;
    if (byIsString) {
      for (const auto& kv : strKeys) {
        sIdx[kv] = static_cast<int64_t>(groups.size());
        groups.push_back({0.0, kv.second, kv.first == 1});
      }
    } else {
      for (const auto& kv : numKeys) {
        nIdx[kv] = static_cast<int64_t>(groups.size());
        groups.push_back({kv.second, std::string(), kv.first == 1});
      }
    }
    for (int64_t i = 0; i < n; ++i) {
      const size_t k = static_cast<size_t>(i);
      if (!touse[k]) continue;
      gOf[k] = byIsString ? sIdx[{byStr[k].empty() ? 1 : 0, byStr[k]}]
                          : nIdx[{std::isnan(byNum[k]) ? 1 : 0, std::isnan(byNum[k]) ? 0.0 : byNum[k]}];
    }
  }
  const int64_t nGroups = static_cast<int64_t>(groups.size());
  if (nGroups > 1500) return rsErrorRc("too many groups", 134);

  // The stub, and whether the groups' own values can name the variables.
  std::string stub = genOpt.value_or(varArg);
  stub = genOpt.has_value() ? strutil::trim(*genOpt) : var;
  const bool mayTrim = !genOpt.has_value();
  bool useOrig = false;
  double maxv = static_cast<double>(nGroups);
  if (!sequential && !byIsString) {
    bool ok = true;
    double mx = -1e300;
    for (const auto& g : groups) {
      if (g.isMissing) continue;
      if (g.num < 0 || g.num != std::floor(g.num) || g.num >= 9999) ok = false;
      mx = std::max(mx, g.num);
    }
    if (ok) {
      useOrig = true;
      maxv = mx < -1e299 ? static_cast<double>(nGroups) : mx;
    }
  }
  {
    const std::string maxText = macroNum(maxv);
    // Characters, not bytes: the length limit is 32 Unicode characters.
    auto ulen = [](const std::string& t) {
      size_t c = 0;
      for (unsigned char ch : t) c += (ch & 0xC0) != 0x80;
      return c;
    };
    if (ulen(stub + maxText) > 32) {
      if (!mayTrim) return rsErrorRc(stub + ":  name too long", 198);
      size_t keep = 32 > ulen(maxText) ? 32 - ulen(maxText) : 0, chars = 0, bytes = 0;
      while (bytes < stub.size() && chars < keep) {
        ++bytes;
        while (bytes < stub.size() && (static_cast<unsigned char>(stub[bytes]) & 0xC0) == 0x80) ++bytes;
        ++chars;
      }
      stub = stub.substr(0, bytes);
    }
  }

  // The source's type, display format, value label.
  std::string srcType = "float";
  {
    auto mit = session.varMeta.find(var);
    if (mit != session.varMeta.end() && mit->second.stataType.has_value())
      srcType = *mit->second.stataType;
    else if (varIsString)
      srcType = "str1";
  }
  std::string fmt;
  {
    auto mit = session.varMeta.find(var);
    if (mit != session.varMeta.end() && mit->second.format.has_value() && !mit->second.format->empty()) {
      fmt = *mit->second.format;
    } else {
      std::vector<VarInfo> fc = ds->formatCard(std::vector<std::string>{var});
      if (!fc.empty()) fmt = session.displayFormat(fc.front());
    }
  }
  std::string vallab;
  if (auto lit = session.varLblname.find(var); lit != session.varLblname.end()) vallab = lit->second;
  std::string bylab;
  if (!byName.empty())
    if (auto lit = session.varLblname.find(byName); lit != session.varLblname.end()) bylab = lit->second;

  // Names, validated before anything is written.
  std::vector<std::string> newVars;
  for (int64_t gi = 0; gi < nGroups; ++gi) {
    const Group& g = groups[static_cast<size_t>(gi)];
    std::string vname;
    if (g.isMissing) {
      std::string code = byIsString ? g.str : macroNum(std::numeric_limits<double>::quiet_NaN());
      code.erase(std::remove(code.begin(), code.end(), '.'), code.end());
      vname = stub + "_" + code;
    } else if (useOrig) {
      vname = stub + macroNum(g.num);
    } else {
      vname = stub + std::to_string(gi + 1);
    }
    ojson guard = newVarGuard(session, "separate", vname);
    if (!guard.is_null()) return guard;
    if (std::find(newVars.begin(), newVars.end(), vname) != newVars.end())
      return rsErrorRc("variable " + vname + " already defined", 110);
    newVars.push_back(vname);
  }

  std::string cur = *session.currentHbin();
  struct Made { std::string type, label; bool hasLabel = false; };
  std::vector<Made> made(newVars.size());
  for (int64_t gi = 0; gi < nGroups; ++gi) {
    const size_t gk = static_cast<size_t>(gi);
    std::vector<std::string> cells(static_cast<size_t>(n));
    Made& m = made[gk];
    if (varIsString) {
      size_t longest = 1;
      for (int64_t i = 0; i < n; ++i)
        if (gOf[static_cast<size_t>(i)] == gi) {
          cells[static_cast<size_t>(i)] = srcStr[static_cast<size_t>(i)];
          longest = std::max(longest, srcStr[static_cast<size_t>(i)].size());
        }
      m.type = "str" + std::to_string(longest);
    } else {
      std::vector<double> vals;
      for (int64_t i = 0; i < n; ++i)
        if (gOf[static_cast<size_t>(i)] == gi) {
          cells[static_cast<size_t>(i)] = numCell(srcNum[static_cast<size_t>(i)]);
          vals.push_back(srcNum[static_cast<size_t>(i)]);
        }
      m.type = compressedNumericType(vals, srcType);
    }
    const Group& g = groups[gk];
    const std::string byval = byIsString ? g.str : macroNum(g.isMissing ? std::numeric_limits<double>::quiet_NaN() : g.num);
    if (!indicat.empty()) {
      const bool brief = shortLabel || veryShort;
      if (!g.isMissing && !byIsString && g.num == 0.0) {
        m.label = brief ? "!(" + indicat + ")" : var + ", !(" + indicat + ")";
        m.hasLabel = true;
      } else if (!g.isMissing && !byIsString && g.num == 1.0) {
        m.label = brief ? indicat : var + ", " + indicat;
        m.hasLabel = true;
      }
    } else {
      std::string shown = byval;
      if (!bylab.empty() && !byIsString && !g.isMissing) {
        auto ld = session.labelDefs.find(bylab);
        if (ld != session.labelDefs.end()) {
          auto t = ld->second.find(macroNum(g.num));
          if (t != ld->second.end()) shown = t->second;
        }
      }
      m.label = veryShort ? shown : shortLabel ? byName + " == " + shown
                                               : var + ", " + byName + " == " + shown;
      m.hasLabel = true;
    }
    if (m.label.size() > 80) m.label.resize(80);
    std::string out;
    if (auto err = appendColumn(session, cur, newVars[gk], cells, varIsString ? "factor" : "f64", &out))
      return *err;
    cur = out;
  }
  session.load(cur);
  for (size_t k = 0; k < newVars.size(); ++k) {
    session.implicitFloatVars.erase(newVars[k]);
    session.varMeta.erase(newVars[k]);
    VarMeta& vm = session.varMeta[newVars[k]];
    vm.stataType = made[k].type;
    if (!fmt.empty()) vm.format = fmt;
    if (made[k].hasLabel) vm.label = made[k].label;
    if (indicat.empty() && !vallab.empty()) session.varLblname[newVars[k]] = vallab;
  }
  ojson mut = mutationResult(session, "gen", newVars);
  // The ado ends with `describe newvars`; its r() does not survive, the program's own
  // `return local varlist` does.
  ojson desc = dispatch("describe " + joinWith(newVars, " "), session);
  eval::replaceRClass(session.eval);
  session.eval.rStrings.set("varlist", joinWith(newVars, " "));
  eval::declareRPosted(session.eval, "separate", {"varlist"});
  ojson blocks = ojson::array();
  blocks.push_back(mut);
  if (desc.is_array()) for (const auto& b : desc) blocks.push_back(b);
  else if (desc.is_object()) blocks.push_back(desc);
  return blocks;
}

}  // namespace piet::handlers
