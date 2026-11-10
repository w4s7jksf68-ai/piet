// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "vendor/nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "io/win_compat.h"  // to_native_path / native_exists: binscatter's savedata files
#include "piet/cobra_runner.h"
#include "piet/dispatcher.h"  // dispatch -- binscatter reruns the ado's marksample/regressions
#include "piet/display_hardening.h"  // fmtStataDefaultG — Stata's %9.0g default rendering
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"  // findUnquotedComma — binscatter reads its options as typed
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/settings.h"
#include "piet/stata_format.h"  // fmt::general — %w.0g text of file write / regress
#include "piet/strutil.h"
#include "piet/twoway_opts.h"  // twoway::layerStyle — Arc G's graph half reuses it verbatim

#include "binscatter_internal.h"
#include "estimation/engine_report_support.h"

namespace piet {
namespace handlers {

namespace engine_report_detail {

namespace {

// Runs `line`; false when it answers an error.
bool bsRun(SessionState& session, const std::string& line) {
  for (const auto& b : dispatch(line, session))
    if (b.is_object() && b.value("type", std::string()) == "error") return false;
  return true;
}

// Drops the helper variables `names` as an ado's tempvars go: r() and the
// data's changed flag are left as they were.
void bsDropHelpers(SessionState& session, const std::vector<std::string>& names, bool changed) {
  Datastore* ds = session.datastore();
  std::string list;
  for (const auto& n : names)
    if (ds != nullptr && ds->has(n)) list += " " + n;
  if (!list.empty()) {
    SessionState::TempDropScope td(session);
    (void)dispatch("quietly drop" + list, session);
  }
  session.datasetChanged = changed;
}

// The ado's `touse` (ado) as the byte variable `name`: `marksample
// touse` (if/in, every y and x nonmissing, a missing or zero weight out), then
// `markout `touse' `by' `controls' `absorb', strok` -- every variable of a
// control term (`a#c.b` marks out a and b; a time-series operator marks out
// where the operated value is missing). False, with nothing added, when the
// marker cannot be built.
bool bsMarkTouse(SessionState& session, const eval::ParsedCommand& parsed, const BsSpec& sp,
                 const std::string& name) {
  Datastore* ds = session.datastore();
  if (ds == nullptr || ds->has(name)) return false;
  const bool changed = session.datasetChanged;
  std::string cond;
  auto markout = [&](const std::string& e) { cond += (cond.empty() ? "!missing(" : " & !missing(") + e + ")"; };
  for (const auto& v : sp.ys) markout(v);
  markout(sp.x);
  if (!sp.by.empty()) markout(sp.by);
  for (const auto& term : splitWsOrComma(sp.controls)) {
    for (size_t pos = 0; pos < term.size();) {
      size_t hash = term.find('#', pos);
      if (hash == std::string::npos) hash = term.size();
      const std::string part = term.substr(pos, hash - pos);
      pos = hash + 1;
      if (part.empty()) continue;
      const size_t dot = part.rfind('.');
      std::string ts;  // the time-series operators of the term, the factor ones dropped
      if (dot != std::string::npos) {
        std::string seg;
        for (char c : part.substr(0, dot + 1)) {
          if (c != '.') {
            seg += c;
            continue;
          }
          if (!seg.empty() && std::strchr("LFDSlfds", seg[0]) != nullptr) ts += seg + ".";
          seg.clear();
        }
      }
      markout(ts + part.substr(dot == std::string::npos ? 0 : dot + 1));
    }
  }
  if (!sp.absorb.empty()) markout(sp.absorb);
  if (!sp.wvar.empty()) cond += " & !missing(" + sp.wvar + ") & (" + sp.wvar + ") != 0";
  std::string qual;
  if (parsed.ifExpr.has_value() && !strutil::trim(*parsed.ifExpr).empty()) qual += " if " + *parsed.ifExpr;
  if (parsed.inRange.has_value() && !strutil::trim(*parsed.inRange).empty()) qual += " in " + *parsed.inRange;
  if (!bsRun(session, "quietly generate byte " + name + " = 0") ||
      !bsRun(session, "quietly replace " + name + " = " + cond + qual)) {
    bsDropHelpers(session, {name}, changed);
    return false;
  }
  return true;
}

}  // namespace

// `ereturn post, esample(`touse')` (ado): e() replaced whole, e(sample) the
// ado's touse (StataBE 19.5, BUG-1293: `count if e(sample)` answers e(N), 59 and
// 29 with an if). The marker must hold the engine's N rows; false, with e() and
// the data as they were, when it cannot be built or does not.
bool bsPostEsample(SessionState& session, const eval::ParsedCommand& parsed, const BsSpec& sp, double N) {
  const bool changed = session.datasetChanged;
  const std::string touse = "__000001";
  if (!bsMarkTouse(session, parsed, sp, touse)) return false;
  double ones = 0;
  if (Datastore* ds = session.datastore(); ds != nullptr && ds->has(touse)) {
    const double* v = ds->col(touse).f64();
    for (int64_t i = 0; v != nullptr && i < ds->nRows(); ++i) ones += v[i] == 1.0 ? 1 : 0;
  }
  const bool posted = ones == N && bsRun(session, "quietly ereturn post, esample(" + touse + ")");
  bsDropHelpers(session, {touse}, changed);
  return posted;
}

// e() after a binscatter that stops at its bins -- discrete over N/2 unique
// values r(134), fastxtile's nquantiles() bounds r(198) (ado). The ado
// has run its regressions by then and e() is the last of them (StataBE 19.5,
// BUG-1294: `binscatter x y, discrete` r(134) leaves e(cmd) regress, e(depvar)
// x, colnames `y _cons`): the last fit line (ado: `reg y_r [x_r2] x_r [wt]
// if touse [& by==byval]`, the last y of the last by-group), or with no fit line
// the last residualization (ado: `reg|areg y controls [wt] if touse`). They
// are rerun on the ado's tempvars, which are dropped after.
void bsReplayRegressions(SessionState& session, const eval::ParsedCommand& parsed, const BsSpec& sp) {
  if (!sp.fit && !sp.residualized) return;
  const bool changed = session.datasetChanged;
  const std::string touse = "__000001";
  if (!bsMarkTouse(session, parsed, sp, touse)) return;
  std::vector<std::string> made = {touse};
  const std::string wt = sp.wtype.empty() ? std::string() : " [" + sp.wtype + "=" + sp.wvar + "]";
  bool ok = true;
  auto run = [&](const std::string& line) { ok = ok && bsRun(session, line); };
  auto fresh = [&](const std::string& n) {
    Datastore* ds = session.datastore();
    ok = ok && ds != nullptr && !ds->has(n);
    if (ok) made.push_back(n);
  };
  if (sp.residualized) {
    std::vector<std::string> src = {sp.x}, dst = {sp.xReg};
    src.insert(src.end(), sp.ys.begin(), sp.ys.end());
    dst.insert(dst.end(), sp.yReg.begin(), sp.yReg.end());
    for (size_t i = 0; i < src.size() && ok; ++i) {
      fresh(dst[i]);
      run("quietly " + std::string(sp.absorb.empty() ? "regress " : "areg ") + src[i] + " " + sp.controls + wt +
          " if " + touse + (sp.absorb.empty() ? std::string() : ", absorb(" + sp.absorb + ")"));
      run("quietly predict " + dst[i] + " if e(sample), residuals");
      if (!sp.noaddmean) {
        run("quietly summarize " + src[i] + wt + " if " + touse + ", meanonly");
        run("quietly replace " + dst[i] + " = " + dst[i] + " + r(mean)");
      }
    }
  }
  if (sp.fit && ok) {
    std::string conds = touse;
    if (!sp.by.empty()) {
      run("quietly summarize " + sp.by + " if " + touse + ", meanonly");
      const double* mx = session.eval.rValues.find("max");
      ok = ok && mx != nullptr;
      if (ok) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.17g", *mx);
        conds += " & " + sp.by + " == " + buf;
      }
    }
    if (sp.qfit) {
      fresh(sp.x2Reg);
      run("quietly generate " + sp.x2Reg + " = " + sp.xReg + "^2");
    }
    run("quietly regress " + sp.yReg.back() + " " + (sp.qfit ? sp.x2Reg + " " : std::string()) + sp.xReg + wt +
        " if " + conds);
  }
  bsDropHelpers(session, made, changed);
}

}  // namespace engine_report_detail

}  // namespace handlers
}  // namespace piet
