// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cf -- compare the data in memory with a Stata dataset on disk (variable-metadata family;
// shared helpers in datamgmt/variable_metadata_support.h).
//
//   cf        the using file is read the way merge reads one (cobra import-dta into a
//             scratch hbin), never by preserve/use/restore of the session frame; Stata's
//             `!=` (extended missings included) is cecon::stata_cmp.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "data/missing.h"  // cecon::stata_cmp / missing_kind -- Stata's `!=`, extended missings
#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"  // handleDisplay (display's default number text), resolveVarName
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // applyStataFmt, fnAbbrev
#include "piet/eval/macros.h"   // captureRFromBlock
#include "piet/eval/pyvalue.h"  // pythonFloatRepr / PyValue
#include "piet/eval/session.h"  // replaceRClass / declareRPosted
#include "piet/eval/tokenizer.h"  // scanOneToken / quotedSpanMask
#include "piet/handlers/descriptive.h"
#include "piet/user_paths.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // expandVarlist -- the ONE varlist wildcard/range expander
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "vendor/nlohmann/json.hpp"

#include "datamgmt/variable_metadata_support.h"

namespace {

// cf: `tolerance()` (undocumented) compares by reldif instead of `!=`.
const piet::eval::OptVocab& kVocabCf =
    piet::eval::registerOptVocab({"cf"}, {{"verbose", "all"}, {"tolerance"}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace variable_metadata_detail;

// ===========================================================================
// cf
// ===========================================================================
//
// Implements cf per its help file; identified against StataBE probes
// (w2ax_t1_official.log cf_*, mx3.log CF|*):
//   * `cf varlist using file [, verbose all]`; no `using` is r(100) "using required", no
//     varlist r(100) "varlist required"; the using file takes .dta when it has no extension
//     and a missing one is r(601) "file <name>.dta not found" (cf_nofile);
//   * a different _N is r(9) "master has # obs, using #" before any variable (cf_nobs);
//   * per variable, in varlist order, the label is abbrev(name,16) right-aligned in 18:
//     absent from using -> "does not exist in using" (cf_notinusing); string against
//     numeric -> "<type> in master but <type> in using" (CF|typemis); otherwise the
//     count of observations with master != using under Stata's `!=` (float against
//     double differs, .b against .a differs: CF|dblvsfloat, CF|verbose) -> "# mismatch(es)",
//     with `verbose` one "obs #. <master> in master; <using> in using" line each; `all`
//     also prints "match" for an identical variable (CF|all);
//   * any difference -> r(9); r(Nsum) (a macro, as cf posts it) is the total mismatch count
//     whenever the variable loop ran (cf_diff 9|2, CF|typemis 9|0) and absent when cf
//     stopped before it.
// The current data are never touched: the using file is converted into a scratch frame of
// its own (merge's using reader), read, and dropped.
namespace {

// One cell as Stata compares it: a string, or a double carrying cecon's missing kinds.
struct Cell {
  bool isStr = false;
  double d = 0;
  std::string s;
};

std::vector<Cell> columnCells(const Datastore& ds, const std::string& name) {
  std::vector<Cell> out;
  const int64_t n = ds.nRows();
  out.reserve(static_cast<size_t>(n));
  const cecon::Column& c = ds.col(name);
  if (c.dtype == cecon::DType::Factor) {
    const int32_t* codes = c.codes();
    for (int64_t i = 0; i < n; ++i) {
      Cell cell;
      cell.isStr = true;
      const int32_t k = codes[i];
      if (k >= 0 && static_cast<size_t>(k) < c.levels.size()) cell.s = c.levels[static_cast<size_t>(k)];
      out.push_back(std::move(cell));
    }
  } else {
    const double* d = c.f64();
    for (int64_t i = 0; i < n; ++i) {
      Cell cell;
      cell.d = d[i];
      out.push_back(std::move(cell));
    }
  }
  return out;
}

// The storage type `describe`/`: type` report for a master variable.
std::string masterType(SessionState& session, const Datastore& ds, const std::string& v) {
  auto it = session.varMeta.find(v);
  if (it != session.varMeta.end() && it->second.stataType.has_value() &&
      !it->second.stataType->empty())
    return *it->second.stataType;
  if (ds.dtype(v) != "factor") return "float";
  size_t w = 1;
  for (const auto& c : columnCells(ds, v)) w = std::max(w, c.s.size());
  return "str" + std::to_string(w);
}

bool fileExistsAt(const std::string& path) {
  std::ifstream f(cecon::to_native_path(path), std::ios::binary);
  return static_cast<bool>(f);
}

bool looksLikeDta(const std::string& path) {
  std::ifstream input(cecon::to_native_path(path), std::ios::binary);
  char header[11] = {};
  input.read(header, sizeof header);
  const auto size = input.gcount();
  const bool xml = size == 11 && std::string(header, 11) == "<stata_dta>";
  const auto version = static_cast<unsigned char>(header[0]);
  const bool legacy = size >= 3 && (version == 113 || version == 114 || version == 115) &&
                      (header[1] == 1 || header[1] == 2) && header[2] == 1;
  return xml || legacy;
}

}  // namespace

ojson runCf(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabCf.known, kVocabCf.unimplemented))
    return ojson(*err);
  const bool verbose = hasFlag(parsed, "verbose");
  const bool all = hasFlag(parsed, "all");
  // cf is r-class: a stop before its `return` leaves r() empty (mx3.log CF|novar `.`).
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "cf", {});

  // `varlist using filename`: split at the first unquoted `using` word.
  const std::string text = strutil::trim(parsed.argText);
  std::string vlText, fileText;
  bool haveUsing = false;
  {
    const std::vector<bool> quoted = eval::quotedSpanMask(text);
    for (size_t i = 0; i + 5 <= text.size(); ++i) {
      if (quoted[i] || text.compare(i, 5, "using") != 0) continue;
      const bool leftOk = i == 0 || std::isspace(static_cast<unsigned char>(text[i - 1]));
      const bool rightOk = i + 5 == text.size() || std::isspace(static_cast<unsigned char>(text[i + 5]));
      if (!leftOk || !rightOk) continue;
      vlText = strutil::trim(text.substr(0, i));
      fileText = strutil::trim(text.substr(i + 5));
      haveUsing = true;
      break;
    }
    if (!haveUsing) vlText = text;
  }
  if (vlText.empty()) return rcError("varlist required", 100);
  std::vector<std::string> vars;
  if (auto e = unabVarlist(vlText, session, &vars)) return *e;
  {
    // A variable named twice is r(198) (review p3 cf_dup `cf x x using f`).
    std::set<std::string> seen;
    for (const auto& v : vars)
      if (!seen.insert(v).second) return rcError("nothing found where name expected", 198);
  }
  if (!haveUsing || fileText.empty()) return rcError("using required", 100);

  // The using file, as `describe using` finds it.
  std::string typed = fileText;
  {
    std::vector<std::string> w = stataWords(typed);
    if (w.size() != 1) return rcError("invalid file specification", 198);
    typed = w.front();
  }
  std::string path = resolveUserPath(session, typed);
  session.materializeVirtualSave(path);
  std::string shown = typed;
  {
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    const bool noExt = dot == std::string::npos || (slash != std::string::npos && dot < slash);
    if (noExt && !fileExistsAt(path)) {
      path += ".dta";
      shown += ".dta";
      session.materializeVirtualSave(path);
    }
  }
  if (!fileExistsAt(path)) return rcError("file " + shown + " not found", 601);
  if (!looksLikeDta(path)) return rcError("file " + shown + " not Stata format", 610);

  const std::string conv = mintScratchPath(session.workdir(), "t1cf_using");
  const std::string metaPath = conv + ".meta.json";
  registerScratchMint(metaPath);
  CobraRunResult cr = runCobra({"import-dta", path, conv, "--out-meta", metaPath});
  if (!cr.ok) {
    dropScratch(conv);
    dropScratch(metaPath);
    return rcError("file " + shown + " could not be read: " +
                       strutil::trim(cr.stderr_text.empty() ? cr.error : cr.stderr_text),
                   610);
  }
  std::map<std::string, std::string> usingTypes;
  try {
    std::ifstream mf(cecon::to_native_path(metaPath), std::ios::binary);
    const nlohmann::json meta = nlohmann::json::parse(mf);
    if (meta.contains("variables") && meta["variables"].is_array())
      for (const auto& v : meta["variables"])
        usingTypes[v.value("name", std::string())] = v.value("stata_type", std::string());
  } catch (const std::exception&) {
    // The type text is only printed on a string/numeric clash; absent, it falls back below.
  }
  dropScratch(metaPath);

  std::unique_ptr<Datastore> usingDs;
  try {
    usingDs = std::make_unique<Datastore>(conv);
  } catch (const std::exception& e) {
    dropScratch(conv);
    return rcError("file " + shown + " could not be read: " + e.what(), 610);
  }
  session.noteProjectionScratch(conv);

  ColumnScope scope(session, vars, std::nullopt, std::nullopt, "cf");
  if (scope.failed()) return scope.error();
  Datastore& master = *scope.get();
  const int64_t nM = master.nRows(), nU = usingDs->nRows();
  if (nM != nU)
    return rcError("master has " + std::to_string(nM) + " obs, using " + std::to_string(nU), 9);

  std::vector<std::string> lines;
  long long nsum = 0;
  bool dif = false;
  const size_t maxObsLen = std::to_string(nM).size();
  for (const auto& v : vars) {
    const std::string stub = rightAlign(stataAbbrev(v, 16) + ": ", 18);
    if (!usingDs->has(v)) {
      lines.push_back(stub + "does not exist in using");
      dif = true;
      continue;
    }
    const std::vector<Cell> m = columnCells(master, v);
    const std::vector<Cell> u = columnCells(*usingDs, v);
    const bool mStr = master.dtype(v) == "factor", uStr = usingDs->dtype(v) == "factor";
    if (mStr != uStr) {
      std::string ut = usingTypes.count(v) ? usingTypes[v] : std::string();
      if (ut.empty()) ut = uStr ? "str" : "float";
      lines.push_back(stub + masterType(session, master, v) + " in master but " + ut + " in using");
      dif = true;
      continue;
    }
    std::vector<int64_t> diffs;
    for (int64_t i = 0; i < nM; ++i) {
      const Cell& a = m[static_cast<size_t>(i)];
      const Cell& b = u[static_cast<size_t>(i)];
      const bool differ = mStr ? a.s != b.s : cecon::stata_cmp(a.d, b.d) != 0;
      if (differ) diffs.push_back(i);
    }
    if (diffs.empty()) {
      if (all) lines.push_back(stub + "match");
      continue;
    }
    lines.push_back(stub + std::to_string(diffs.size()) + " mismatch" + (diffs.size() > 1 ? "es" : ""));
    nsum += static_cast<long long>(diffs.size());
    dif = true;
    if (verbose) {
      for (int64_t i : diffs) {
        const Cell& a = m[static_cast<size_t>(i)];
        const Cell& b = u[static_cast<size_t>(i)];
        const std::string mv = mStr ? a.s : displayNumber(a.d, session);
        const std::string uv = mStr ? b.s : displayNumber(b.d, session);
        lines.push_back(std::string(18, ' ') + "obs " + rightAlign(std::to_string(i + 1), maxObsLen) +
                        ". " + mv + " in master; " + uv + " in using");
      }
    }
  }
  session.eval.rStrings.set("Nsum", std::to_string(nsum));
  eval::declareRPosted(session.eval, "cf", {"Nsum"});
  ojson out = ojson::array();
  if (!lines.empty()) out.push_back(displayBlock(joinWords(lines, "\n")));
  if (dif) out.push_back(rcError("cf: master and using data differ", 9));
  return out;
}

}  // namespace piet::handlers
