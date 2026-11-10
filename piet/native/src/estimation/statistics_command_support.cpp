// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Definitions of the helpers declared in statistics_command_support.h.
//
// The engine-backed half of E2 (tsfilter hp, lpoly, lowess, pca) refuses
// honestly here until the cobra subcommands land: the handler ASKS the engine
// what it supports rather than assuming, so a PIET built against an older cobra
// says so instead of producing a wrong number or a crash.

#include "piet/eval/tokenizer.h"
#include "vendor/nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/distributions.h"  // betai / betaitail / z_critical (shared with cobra)
#include "core/exact_sum.h"  // ExactSum: summarize's accumulation (centile's mean)
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "piet/twoway_opts.h"
#include "estimation_internal.h"
#include "estimators/postest_names.h"  // factor_vars_of / parse_level_ref (the engine's one resolver)
#include "dispatch/registry.h"  // resolveCanonical: the command word as Stata names it
#include "statistics_command_support.h"

namespace piet::handlers::statcmd_detail {

// Fixed-width table cells count CHARACTERS, not bytes: StataBE pads and
// abbreviates a Unicode variable name by characters (scratch-r150/uni-v/p4.do),
// so a multi-byte name stays aligned and is never cut mid-sequence.
// fitLeft: the first `w` characters, right-aligned in `w`; fitRight: left-aligned.
std::string fitLeft(const std::string& s, size_t w) {
  const std::string c = cecon::ident::truncateCodePoints(s, w);
  const size_t n = cecon::ident::codePointCount(c);
  return n >= w ? c : std::string(w - n, ' ') + c;
}
std::string fitRight(const std::string& s, size_t w) {
  const std::string c = cecon::ident::truncateCodePoints(s, w);
  const size_t n = cecon::ident::codePointCount(c);
  return n >= w ? c : c + std::string(w - n, ' ');
}

// Stata's abbrev(name, w): a name past `w` characters keeps its first w-2, `~`
// and its last character (StataBE centile: `قيمةطويلةج~ا`).
std::string abbrevCp(const std::string& s, size_t w) {
  if (cecon::ident::codePointCount(s) <= w || w < 3) return s;
  size_t last = s.size();
  do {
    --last;
  } while (last > 0 && (static_cast<unsigned char>(s[last]) & 0xC0) == 0x80);
  return cecon::ident::truncateCodePoints(s, w - 2) + "~" + s.substr(last);
}

ojson aerError(const std::string& message, const std::string& code) {
  return ojson(eval::engineError(message, code));
}

// r(198) "invalid syntax". The rc is derived from the error CODE by the BS-100
// taxonomy, not from a reason string -- `engine_error` lands in the generic
// bucket and reads back as rc 1, which is what this wrapper exists to avoid.
ojson aerSyntaxError(const std::string& message) {
  return ojson(eval::engineError(message, "syntax_error"));
}

// A refusal whose Stata rc is known but which the coarse message rules do not
// recognise. `reason` is the only channel carrying an exact rc, and it outranks
// the coarse mapping -- without it these come back as the generic rc 1.
ojson aerErrorReason(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

// Pull one numeric column as doubles, NaN for missing. Returns false when the
// column is absent or is a string/factor the statistic cannot use.
bool numericColumn(const Datastore& ds, const std::string& name, std::vector<double>* out,
                   std::string* err) {
  if (!ds.has(name)) {
    *err = "variable " + name + " not found";
    return false;
  }
  const cecon::Column& c = ds.col(name);
  const size_t n = ds.nRows();
  out->assign(n, std::numeric_limits<double>::quiet_NaN());
  // DType carries exactly two shapes at this layer: F64 and Factor. A factor's
  // code IS its numeric value for these statistics (the same reading `egen rank`
  // takes), and a negative code is the missing sentinel.
  if (c.dtype == cecon::DType::Factor) {
    const int32_t* codes = c.codes();
    if (codes == nullptr) {
      *err = name + ": column has no readable payload";
      return false;
    }
    for (size_t i = 0; i < n; ++i)
      (*out)[i] = (codes[i] < 0) ? std::numeric_limits<double>::quiet_NaN()
                                 : static_cast<double>(codes[i]);
    return true;
  }
  const double* d = c.f64();
  if (d == nullptr) {
    *err = name + ": column has no readable payload";
    return false;
  }
  for (size_t i = 0; i < n; ++i) (*out)[i] = d[i];
  return true;
}

// Does the engine expose this subcommand? Asked, not assumed: a PIET built
// against a cobra without the bundle-H verbs must say so rather than fail
// somewhere less legible.
bool engineHasSubcommand(const std::string& sub) {
  try {
    nlohmann::json opts = runCobraJson({"options", "--json"});
    if (opts.is_object() && opts.contains(sub)) return true;
    if (opts.is_object() && opts.contains("commands") && opts["commands"].is_object())
      return opts["commands"].contains(sub);
    if (opts.is_array()) {
      for (const auto& e : opts)
        if (e.is_string() && e.get<std::string>() == sub) return true;
    }
  } catch (const std::exception&) {
    return false;
  }
  return false;
}

ojson engineTooOld(const std::string& stataCmd, const std::string& sub) {
  return aerError(stataCmd + " requires a cobra build that provides the `" + sub +
                      "` subcommand; this engine does not. Rebuild the engine from a tree that "
                      "includes AER bundle H1, or point $COBRA_BIN at one.",
                  "unsupported_command");
}

// ===========================================================================
// E2 engine-backed — H1 has landed, so these now compute.
//
// Each still ASKS the engine whether its subcommand exists before using it: a
// PIET built against an older cobra says so by name rather than failing
// somewhere less legible.

namespace {

// mintScratchPath always names a ".hbin"; a setcol payload is a CSV. Same body
// and same reason as matrix/matrix_to_variables.cpp's copy -- registering the minted
// .hbin and then writing a .csv beside it would leave the file that actually
// exists unregistered, which is the orphan class the registry exists to close.
std::string estimMintCsv(const std::string& workdir, const std::string& prefix) {
  const std::string hbin = mintScratchPath(workdir, prefix);
  forgetScratchMint(hbin);
  const std::string csv = hbin.substr(0, hbin.size() - 5) + ".csv";
  registerScratchMint(csv);
  return csv;
}

// The headerless one-value-per-row payload `cobra setcol` reads. An EMPTY line
// is how a missing value is spelled there; "nan" would land a literal token in
// a numeric column.
bool estimWriteCsv(const std::string& path, const std::vector<double>& v) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  f.setf(std::ios::fmtflags(0), std::ios::floatfield);
  f.precision(17);
  for (double d : v) {
    if (!std::isnan(d)) f << d;
    f << "\n";
  }
  return static_cast<bool>(f);
}

}  // namespace

// Write one f64 column into the session frame.
bool estimWriteColumn(SessionState& session, const std::string& name,
                      const std::vector<double>& vals, ojson* err) {
  const std::string csv = estimMintCsv(session.workdir(), "aerestim");
  if (!estimWriteCsv(csv, vals)) {
    *err = aerError("could not write the values payload", "data_io");
    return false;
  }
  const std::string out = mintScratchPath(session.workdir(), "aerestim");
  const CobraRunResult r = runCobra({"setcol", "--data", *session.currentHbin(), "--var", name,
                                     "--values", csv, "--dtype", "f64", "--new", "--out", out});
  if (!r.ok) {
    *err = aerError("could not create " + name, "data_io");
    return false;
  }
  // The engine's writer creates --out before it can detect a write failure, and
  // a full scratch volume has reached PIET as a ZERO exit code twice. A
  // session.load on a path that is not there records the head anyway and hands
  // the next open a missing mapping -- a crash rather than a diagnostic.
  if (!cecon::native_exists(out)) {
    *err = aerError("failed writing output dataset (disk full or I/O error)", "data_io");
    return false;
  }
  session.load(out);
  return true;
}

std::vector<double> jsonDoubles(const nlohmann::json& j) {
  std::vector<double> v;
  if (!j.is_array()) return v;
  v.reserve(j.size());
  for (const auto& e : j)
    v.push_back(e.is_number() ? e.get<double>() : std::numeric_limits<double>::quiet_NaN());
  return v;
}

bool smoothIsNumeric(SessionState& session, const std::string& name) {
  Datastore* ds = session.datastore();
  if (ds == nullptr || !ds->has(name)) return false;
  const std::string t = ds->dtype(name);
  return t != "factor" && t.rfind("str", 0) != 0;
}

}  // namespace piet::handlers::statcmd_detail
