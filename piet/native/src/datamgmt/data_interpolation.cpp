// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// data interpolation.
#include "data_io_internal.h"
#include "piet/handlers/data_values.h"
namespace piet::handlers {
using namespace data_io_detail;


ojson runIpolate(const std::vector<std::string>& varlist,
                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                 const std::optional<std::string>& ifExpr, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (varlist.size() != 2)
    return engineErrorBlock("ipolate: syntax is `ipolate yvar xvar, generate(newvar)`",
                            "syntax_error");
  const std::string& yvar = varlist[0];
  const std::string& xvar = varlist[1];
  std::string newvar;
  std::string byVars;
  bool epolate = false;
  for (const auto& [k, v] : opts) {
    if (k == "generate") {
      if (v.isBool)
        return engineErrorBlock("ipolate: generate() requires a variable name", "syntax_error");
      newvar = trim(v.strValue);
    } else if (k == "epolate") {
      epolate = !v.isBool || v.boolValue;
    } else if (k == "by") {
      // BUG-371: `by()` was refused on the grounds that "PIET's by-prefix plumbing
      // does not exist". That was a misreading of the syntax — Stata's ipolate
      // takes by() as an OPTION, not a by: prefix, and the engine's --ipolate has
      // parsed `by(g..)` since it was written. So this was a wiring gap after all,
      // and one line of pass-through closes it.
      if (v.isBool)
        return engineErrorBlock("ipolate: by() requires a variable list", "syntax_error");
      byVars = trim(v.strValue);
    }
  }
  if (newvar.empty())
    return engineErrorBlock("ipolate: generate(newvar) is required", "syntax_error");
  // BUG-1120 review: `ipolate x t, gen(_cons)' and gen(int) are "NAME invalid
  // varname" r(198) (StataBE 19.5).
  if (auto r = strutil::newVarNameRefusal(newvar)) return nameRefusalBlock(*r);
  ojson guard = newVarGuardDV(session, "ipolate", newvar);
  if (!guard.is_null() && !guard.empty()) return guard;
  if (ifExpr.has_value() && !ifExpr->empty()) {
    // Same class as xtile's refusal: the interpolation ANCHORS would come from
    // rows the user excluded, so the interpolated values would not be the ones
    // Stata reports for that sample.
    return engineErrorBlock(
        "ipolate with `if` is not implemented: the interpolation anchors would still be drawn "
        "from excluded rows. Restrict first (`preserve` + `keep if ...`), then ipolate.",
        "unimplemented");
  }
  std::string spec = newvar + "=" + yvar + " " + xvar;
  // The engine's --ipolate reads by() and epolate out of ONE options tail, so both
  // ride the same comma-separated suffix.
  if (!byVars.empty()) spec += ", by(" + byVars + ")";
  if (epolate) spec += ", epolate";
  std::string outPath = nextPath(session.workdir(), "ipolate");
  std::vector<std::string> argv = {"eval",       "--data", *session.currentHbin(),
                                   "--ipolate",  spec,     "--out", outPath};
  SessionState::ResidentEligible resident(session);
  CobraRunResult cr = runCobra(argv);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "gen", {newvar});
}


ojson runTssmoothMa(const std::string& newvar, const std::string& srcvar,
                    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                    SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (srcvar.empty())
    return engineErrorBlock(
        "tssmooth ma: syntax is `tssmooth ma newvar = varname, window(#l #c #f)`", "syntax_error");
  ojson guard = newVarGuardDV(session, "tssmooth ma", newvar);
  if (!guard.is_null() && !guard.empty()) return guard;
  if (!session.timevar.has_value() || session.timevar->empty())
    return engineErrorBlock(
        "tssmooth ma: run `tsset timevar` (or `xtset panelvar timevar`) first so PIET knows the "
        "time variable.",
        "unsupported_option");

  std::string windowSpec;
  bool haveWindow = false;
  for (const auto& [k, v] : opts) {
    if (k == "window") {
      if (v.isBool)
        return engineErrorBlock("tssmooth ma: window() requires #l #c #f", "syntax_error");
      windowSpec = v.strValue;
      haveWindow = true;
    }
  }
  if (!haveWindow)
    return engineErrorBlock("tssmooth ma: window(#l #c #f) is required", "syntax_error");
  std::vector<int> w;
  {
    std::string cur;
    auto flush = [&]() {
      if (cur.empty()) return true;
      int v = 0;
      if (!parseIntOptDV(cur, v)) return false;
      w.push_back(v);
      cur.clear();
      return true;
    };
    bool ok = true;
    for (char c : windowSpec) {
      if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
        if (!flush()) { ok = false; break; }
      } else {
        cur += c;
      }
    }
    if (ok) ok = flush();
    if (!ok)
      return engineErrorBlock("tssmooth ma: window() takes whole numbers", "syntax_error");
  }
  // BUG-378: the short forms are implemented now, with the semantics MEASURED
  // against real StataBE rather than guessed (the earlier refusal existed because
  // the omitted centre weight silently changes the window width, and my own
  // refusal text had the 2-number form wrong — it called it `#l #f`):
  //   window(1 1 1) -> (1/3)*[x(t-1) + 1*x(t) + x(t+1)]
  //   window(1 1)   -> (1/2)*[x(t-1) + 1*x(t)]      l=1, c=1, f=0
  //   window(1)     -> (1/1)*[x(t-1) + 0*x(t)]      l=1, c=0, f=0 — focal EXCLUDED
  // So the SECOND number is the centre, the third the leads, and both default to
  // 0 when omitted.
  if (w.empty() || w.size() > 3)
    return engineErrorBlock(
        "tssmooth ma: window() takes one to three whole numbers (#l [#c [#f]]); got " +
            std::to_string(w.size()),
        "syntax_error");
  const int wl = w[0];
  const int wc = w.size() >= 2 ? w[1] : 0;
  const int wf = w.size() >= 3 ? w[2] : 0;
  if (wl < 0 || wf < 0)
    return engineErrorBlock("tssmooth ma: window() lags and leads must not be negative",
                            "bad_input");
  if (wc != 0 && wc != 1)
    return engineErrorBlock("tssmooth ma: window()'s centre weight is 0 or 1", "bad_input");
  if (wl == 0 && wc == 0 && wf == 0)
    return engineErrorBlock("tssmooth ma: window() would average nothing", "bad_input");

  std::string outPath = nextPath(session.workdir(), "tsma");
  std::vector<std::string> argv = {"eval", "--data", *session.currentHbin(), "--tsma",
                                   newvar + "=" + srcvar + "," + std::to_string(wl) + "," +
                                       std::to_string(wc) + "," + std::to_string(wf),
                                   "--out", outPath};
  // Unconditional, unlike the expression surfaces' gated helper: this command is
  // meaningless without a time variable, and the guard above already proved one.
  argv = translate::injectTsContext(argv, session);
  SessionState::ResidentEligible resident(session);
  CobraRunResult cr = runCobra(argv);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "gen", {newvar});
}
}
