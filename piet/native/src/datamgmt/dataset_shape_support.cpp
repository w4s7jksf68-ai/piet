// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "vendor/nlohmann/json.hpp"
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

namespace piet::handlers::dataset_shape_detail {

// ===========================================================================
// Errors
// ===========================================================================

ojson rsError(const std::string& message, const std::string& code) {
  return ojson(eval::engineError(message, code));
}

// A USER SYNTAX error. "syntax_error" is what coarseRc maps to rc 198; the
// default "engine_error" maps to rc 1, which is bundle C's shipped defect.
ojson rsSyntax(const std::string& message) {
  return ojson(eval::engineError(message, "syntax_error"));
}

// An error whose Stata rc is KNOWN but which coarseRc's message rules cannot
// recognise. `reason` is the only channel carrying an exact rc (BUG-187) and it
// outranks the coarse mapping.
ojson rsErrorRc(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

// "This command printed nothing" -- an EMPTY array, which `pushEnvResult`
// flattens into zero blocks. Never a bare object without a "type" key
// (defect class 1).
ojson silent() { return ojson::array(); }

// ===========================================================================
// cobra plumbing
// ===========================================================================

std::string nextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);
}

namespace {
CobraError errorFromRaw(const CobraRunResult& r) {
  std::string t = r.stderr_text;
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
  try {
    nlohmann::json payload = nlohmann::json::parse(t);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                        err.at("message").get<std::string>(), err.value("detail", std::string()));
    }
  } catch (const nlohmann::json::exception&) {
  }
  return CobraError("engine_error", t.empty() ? "cobra failed" : t);
}
}  // namespace

// Run ONE cobra verb. nullopt = success; otherwise the error BLOCK to return.
// Never a bare parse of stdout_text and never a swallowed rc (IDIOMS).
std::optional<ojson> runVerb(const std::vector<std::string>& args) {
  CobraRunResult cr = runCobra(args);
  if (cr.ok) return std::nullopt;
  CobraError ce = errorFromRaw(cr);
  return rsError(cobraErrorMessage(ce), ce.code);
}

// ===========================================================================
// CSV transport
// ===========================================================================

namespace {
// One CSV field, quoted only when it must be (delimiter, quote, CR or LF) --
// the same convention fileio/delimited_export.cpp's csvField uses.
std::string csvField(const std::string& s, char delim) {
  bool needQuote = false;
  for (char c : s) {
    if (c == delim || c == '"' || c == '\n' || c == '\r') {
      needQuote = true;
      break;
    }
  }
  if (!needQuote) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"')
      out += "\"\"";
    else
      out += c;
  }
  out += "\"";
  return out;
}
}  // namespace

// A numeric cell: BLANK for missing (`Frame::read_csv_column` and
// `Frame::from_csv` both read an empty cell as missing), else the shortest
// round-trippable decimal, so the value that comes back is bit-identical.
// Missing is `.`, never 0 -- in every cell this file writes.
std::string numCell(double v) {
  if (std::isnan(v)) return std::string();
  return eval::pythonFloatRepr(v);
}

namespace {
// The one-column HEADERLESS payload `cobra setcol` reads: one raw value per
// row, row count == n_rows exactly. String cells are quoted even when they do
// not need it, so a value's own leading/trailing padding survives the trip
// (BUG-292); an EMPTY cell is written as a bare blank line, which is the only
// spelling read_csv_column accepts for a missing factor cell.
bool writePayloadCsv(const std::string& path, const std::vector<std::string>& cells, bool quoted) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  for (const auto& c : cells) {
    if (c.empty()) {
      f << "\n";
      continue;
    }
    if (quoted) {
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
  return static_cast<bool>(f);
}
}  // namespace

// Write `t` and convert it to an hbin. nullopt = success and *outPath holds the
// new frame; otherwise the error block to return. A torn file is removed rather
// than left where a later `load()` could mmap it (BUG-124).
std::optional<ojson> writeAndConvert(SessionState& session, const CsvTable& t,
                                     const std::string& prefix, std::string* outPath) {
  const std::string csvPath = nextPath(session.workdir(), prefix) + ".csv";
  registerScratchMint(csvPath);  // BUG-389
  {
    std::ofstream f(cecon::to_native_path(csvPath), std::ios::binary);
    if (!f) return rsError(prefix + ": cannot stage " + csvPath, "data_io");
    for (size_t c = 0; c < t.names.size(); ++c) {
      if (c) f << ',';
      f << csvField(t.names[c], ',');
    }
    f << "\n";
    for (const auto& row : t.rows) {
      // BUG-1078: a row whose every cell is missing would be a BLANK line, and
      // cobra's CSV reader skips blank lines (BUG-697's pad rows, same reader),
      // so a one-variable `input` with a `""` or `.` row lost it and stopped on
      // the row-count assert. A quoted empty first field keeps the row; convert
      // reads it as missing for a factor and for an f64 column alike.
      bool allEmpty = true;
      for (size_t c = 0; c < t.names.size() && allEmpty; ++c) {
        if (c < row.size() && !row[c].empty()) allEmpty = false;
      }
      for (size_t c = 0; c < t.names.size(); ++c) {
        if (c) f << ',';
        if (c == 0 && allEmpty) {
          f << "\"\"";
        } else if (c < row.size()) {
          f << csvField(row[c], ',');
        }
      }
      f << "\n";
    }
    f.flush();
    if (!f) {
      cecon::native_unlink(csvPath);
      return rsError(prefix + ": failed staging " + csvPath + " (disk full or I/O error)",
                     "data_io");
    }
  }
  std::string factorList;
  for (size_t c = 0; c < t.names.size(); ++c) {
    if (!t.isFactor[c]) continue;
    if (!factorList.empty()) factorList += ",";
    factorList += t.names[c];
  }
  const std::string hbin = nextPath(session.workdir(), prefix);
  std::vector<std::string> args = {"convert", "--data", csvPath, "--out", hbin, "--delim", ","};
  if (!factorList.empty()) {
    args.push_back("--factor");
    args.push_back(factorList);
  }
  if (auto err = runVerb(args)) {
    cecon::native_unlink(hbin);
    return err;
  }
  *outPath = hbin;
  return std::nullopt;
}

// Append ONE new column to `basePath` from a payload written here. `cells` are
// the rendered values ("" == missing); `dtype` is "f64" or "factor".
std::optional<ojson> appendColumn(SessionState& session, const std::string& basePath,
                                  const std::string& var, const std::vector<std::string>& cells,
                                  const std::string& dtype, std::string* outPath) {
  const std::string csvPath = nextPath(session.workdir(), "rsvals") + ".csv";
  registerScratchMint(csvPath);  // BUG-389
  if (!writePayloadCsv(csvPath, cells, dtype == "factor")) {
    cecon::native_unlink(csvPath);
    return rsError("could not write the column payload (disk full or I/O error)", "data_io");
  }
  const std::string out = nextPath(session.workdir(), "setcol");
  if (auto err = runVerb({"setcol", "--data", basePath, "--var", var, "--values", csvPath,
                          "--dtype", dtype, "--new", "--out", out})) {
    cecon::native_unlink(out);
    return err;
  }
  *outPath = out;
  return std::nullopt;
}

// ===========================================================================
// Small text / number helpers
// ===========================================================================

std::string lowerOf(std::string s) { return strutil::lower(std::move(s)); }
std::string trimOf(const std::string& s) { return strutil::trim(s); }

namespace {
std::string dequote(const std::string& s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}
}  // namespace

std::vector<std::string> splitWs(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string tok;
  while (iss >> tok) out.push_back(tok);
  return out;
}

std::string joinWith(const std::vector<std::string>& v, const std::string& sep) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += sep;
    out += v[i];
  }
  return out;
}

// STRICT-CONSUME numeric parsing (IDIOMS, C1-F11): the WHOLE token must parse,
// and C's hex forms are rejected. A bare std::stod lies about `5foo` (-> 5),
// which is exactly how a mistyped option value becomes a silently wrong number.
bool parseStrictDouble(const std::string& sIn, double* out) {
  const std::string s = trimOf(sIn);
  if (s.empty()) return false;
  if (s.find('x') != std::string::npos || s.find('X') != std::string::npos) return false;
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

bool parseStrictInt(const std::string& s, long long* out) {
  double d = 0.0;
  if (!parseStrictDouble(s, &d)) return false;
  if (d != std::floor(d)) return false;
  *out = static_cast<long long>(d);
  return true;
}

namespace {
const eval::OptValue* findOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                              const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key) return &v;
  }
  return nullptr;
}
}  // namespace

bool hasFlag(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
             const std::string& key) {
  const eval::OptValue* v = findOpt(opts, key);
  if (v == nullptr) return false;
  // A bare flag parses as isBool/true; a valued spelling still means "given".
  return v->isBool ? v->boolValue : true;
}

std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::string& key) {
  const eval::OptValue* v = findOpt(opts, key);
  if (v == nullptr || v->isBool) return std::nullopt;
  return trimOf(dequote(trimOf(v->strValue)));
}

// The first spelling of `keys` that is present, as a string.
std::optional<std::string> optStrAny(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
    std::initializer_list<const char*> keys) {
  for (const char* k : keys) {
    if (auto v = optStr(opts, k)) return v;
  }
  return std::nullopt;
}

// ===========================================================================
// Randomness (design decision 7)
// ===========================================================================

// The seed a sampling command draws on: its own option first, then the session
// (`set seed #`), then Stata's own conventional default. Never a clock and
// never a global generator -- `set seed 7` plus the same command twice must
// give the identical rows.
uint64_t drawSeed(SessionState& session, const std::optional<std::string>& optSeed) {
  if (optSeed.has_value()) {
    long long v = 0;
    if (parseStrictInt(*optSeed, &v)) return static_cast<uint64_t>(v);
  }
  if (session.seed.has_value()) return static_cast<uint64_t>(*session.seed);
  return 123456789ull;
}

// Row j's key: a pure function of (seed, j), so it does not depend on the order
// the rows were visited in or on how many draws came before.
uint32_t rowKey32(uint64_t seed, int64_t j) {
  return cecon::Philox::draw32(seed, 0, static_cast<uint64_t>(j));
}

// ===========================================================================
// Column reading helpers
// ===========================================================================

// A column's values as DISPLAY strings ("" = missing): the level text for a
// factor, the round-trip decimal for a number.
std::vector<std::string> columnAsStrings(const Datastore& ds, const std::string& name) {
  std::vector<std::string> out;
  const int64_t n = ds.nRows();
  out.reserve(static_cast<size_t>(n));
  const cecon::Column& c = ds.col(name);
  if (c.dtype == cecon::DType::Factor) {
    const int32_t* codes = c.codes();
    for (int64_t i = 0; i < n; ++i) {
      const int32_t code = codes[i];
      if (code < 0 || static_cast<size_t>(code) >= c.levels.size())
        out.emplace_back();
      else
        out.push_back(c.levels[static_cast<size_t>(code)]);
    }
  } else {
    const double* d = c.f64();
    for (int64_t i = 0; i < n; ++i) out.push_back(numCell(d[i]));
  }
  return out;
}

std::vector<double> columnAsDoubles(const Datastore& ds, const std::string& name) {
  const int64_t n = ds.nRows();
  std::vector<double> out(static_cast<size_t>(n), std::numeric_limits<double>::quiet_NaN());
  const cecon::Column& c = ds.col(name);
  if (c.dtype == cecon::DType::Factor) return out;  // callers reject factors first
  const double* d = c.f64();
  for (int64_t i = 0; i < n; ++i) out[static_cast<size_t>(i)] = d[i];
  return out;
}

// A new variable name has to be legal AND free. Both checks, once, so every
// handler in this file refuses the same things for the same reasons.
ojson newVarGuard(SessionState& session, const std::string& verb, const std::string& newvar) {
  if (!strutil::isValidStataIdentifier(newvar))
    return rsSyntax(verb + ": invalid variable name `" + newvar + "' — " +
                    strutil::stataIdentifierRuleText());
  // BUG-1120: nor is a reserved name one: `pctile _cons = x', `cumul x,
  // gen(_cons)', `drawnorm a _cons' and `splitsample, gen(_cons)' are each
  // "_cons invalid varname" r(198) (StataBE 19.5).
  if (strutil::isReservedVarName(newvar)) return rsErrorRc(newvar + " invalid varname", 198);
  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception&) {
    ds = nullptr;
  }
  if (ds != nullptr && ds->has(newvar))
    return rsError(verb + ": variable " + newvar + " already defined", "bad_input");
  return ojson();  // null => no objection
}

// The row-count agreement the plan makes a STOP-and-report: after a command
// that changes the number of observations, what `mutationResult` will publish
// (the reloaded frame's own n_rows) must equal what the command decided to
// produce. `mutationResult` reads the frame, so a disagreement means the write
// did not do what the handler asked -- an honest error beats a wrong `_N`.
std::optional<ojson> assertRowCount(SessionState& session, const std::string& verb,
                                    int64_t expected) {
  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string(verb) + ": could not reopen the result frame — " + e.what(),
                   "data_io");
  }
  if (ds == nullptr) return std::nullopt;
  if (ds->nRows() == expected) return std::nullopt;
  return rsError(verb + ": internal row-count disagreement — the result frame has " +
                     std::to_string(ds->nRows()) + " observation(s) where " +
                     std::to_string(expected) + " were written. Refusing to report a `_N` the "
                     "data does not have.",
                 "internal");
}

// ===========================================================================
// `if`/`in` honesty
// ===========================================================================
//
// Every registry row in this bundle carries allowIf=false / allowInRange=false,
// so the TOKENIZER refuses a qualifier before dispatch ever reaches a handler
// here. This guard is the belt to that suspenders: the reason a qualifier is
// refused (a new column has to be as long as the frame; a percentile's
// cutpoints would come from a different sample than the user asked for) belongs
// next to the code that would otherwise silently do the wrong thing.
std::optional<ojson> refuseQualifiers(const eval::ParsedCommand& parsed, const std::string& verb) {
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    return ojson(eval::unsupportedOption(
        "if " + *parsed.ifExpr,
        "`" + verb + "` in this build computes over every observation. Restrict first "
        "(`preserve` + `keep if ...`), then run " + verb + "."));
  }
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    return ojson(eval::unsupportedOption(
        "in " + *parsed.inRange,
        "`" + verb + "` in this build computes over every observation. Restrict first "
        "(`preserve` + `keep in ...`), then run " + verb + "."));
  }
  return std::nullopt;
}

}  // namespace piet::handlers::dataset_shape_detail
