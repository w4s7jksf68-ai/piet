// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/estimation.h"

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

#include "engine_report_support.h"

namespace {
const piet::eval::OptVocab& kVocabEventstudyinteract = piet::eval::registerOptVocab(
    {"eventstudyinteract"}, {{"absorb", "cohort", "control_cohort",
                                                          "vce", "level", "cluster"},
     {"covariates", "weights",
                                                                  "residualize"}});
}  // namespace

namespace piet {
namespace handlers {

using ojson = nlohmann::ordered_json;
using namespace engine_report_detail;

// ===========================================================================
// eventstudyinteract y rel1 rel2 ... [if], absorb(F1 F2) cohort(G)
//                                          control_cohort(NEVER) vce(cluster ID) level(#)
//
// Oracle: p3_stata.log:290-309. Stata's `absorb()` is space-separated and the
// engine wants a comma list; `vce(cluster id)` carries the cluster variable
// INSIDE the option and has to be split into `--vce cluster --cluster id`.
ojson runEventstudyinteract(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() < 2) {
    return qeErrorBlocks(
        "syntax_error: eventstudyinteract requires a depvar followed by at least one "
        "relative-period indicator — `eventstudyinteract y F2 L2, absorb(id t) cohort(g) "
        "control_cohort(never)`.",
        "syntax_error");
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabEventstudyinteract.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabEventstudyinteract.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }

  auto absorb = optStr(parsed, "absorb");
  auto cohort = optStr(parsed, "cohort");
  auto controlCohort = optStr(parsed, "control_cohort");
  if (!absorb.has_value() || absorb->empty()) {
    return qeErrorBlocks("eventstudyinteract requires `absorb(varlist)`.", "unsupported_option");
  }
  if (!cohort.has_value() || cohort->empty()) {
    return qeErrorBlocks("eventstudyinteract requires `cohort(varname)`.", "unsupported_option");
  }
  if (!controlCohort.has_value() || controlCohort->empty()) {
    return qeErrorBlocks("eventstudyinteract requires `control_cohort(varname)`.",
                          "unsupported_option");
  }

  std::vector<std::string> rel(parsed.varlist.begin() + 1, parsed.varlist.end());
  std::vector<std::string> argv = {"eventstudyinteract",
                                   "--y",
                                   parsed.varlist[0],
                                   "--rel",
                                   joinComma(rel),
                                   "--absorb",
                                   joinComma(splitWsOrComma(*absorb)),
                                   "--cohort",
                                   *cohort,
                                   "--control_cohort",
                                   *controlCohort};

  // vce(cluster id) -> --vce cluster --cluster id. A bare cluster(id) is
  // accepted too (some call sites spell it that way).
  std::string vceKind, clusterVar;
  if (auto vce = optStr(parsed, "vce"); vce.has_value() && !vce->empty()) {
    std::vector<std::string> toks = splitWsOrComma(*vce);
    if (!toks.empty()) {
      vceKind = toks[0];
      if (toks.size() > 1) clusterVar = toks[1];
    }
  }
  if (auto cl = optStr(parsed, "cluster"); cl.has_value() && !cl->empty()) {
    std::vector<std::string> toks = splitWsOrComma(*cl);
    if (!toks.empty()) {
      clusterVar = toks[0];
      if (vceKind.empty()) vceKind = "cluster";
    }
  }
  if (vceKind == "cluster" && clusterVar.empty()) {
    return qeErrorBlocks("eventstudyinteract: vce(cluster ...) requires a cluster variable — "
                          "e.g. `vce(cluster id)`.",
                          "syntax_error");
  }
  if (!vceKind.empty()) {
    argv.push_back("--vce");
    argv.push_back(vceKind);
  }
  if (!clusterVar.empty()) {
    argv.push_back("--cluster");
    argv.push_back(clusterVar);
  }
  // [aw=]/[pw=] (BUG-1283): the engine weights every regression of the IW
  // estimator (StataBE 19.5 e(N) 180, e(b_iw) .86176929 .12436249 1.1556403).
  std::string wtype;
  if (parsed.weightVar.has_value() && !parsed.weightVar->empty()) {
    const std::string wt = strutil::lower(parsed.weightType.value_or("aw"));
    wtype = wt == "pw" || wt == "pweight" ? "pw" : "aw";
    argv.push_back("--weight");
    argv.push_back(*parsed.weightVar);
    argv.push_back("--wtype");
    argv.push_back(wtype);
  }
  pushLevel(argv, parsed, session);
  pushKeepif(argv, parsed);
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  Printer say;
  // The weights regression's own note comes first (`(sum of wgt is 162)`).
  if (!wtype.empty() && r.contains("weight") && r["weight"].is_object() &&
      r["weight"].contains("sum_w_weights") && r["weight"]["sum_w_weights"].is_number()) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "(sum of wgt is %.16g)",
                  r["weight"]["sum_w_weights"].get<double>());
    say(buf);
  }
  say("(obs=" + commaGroup(inum(r, "n_obs_weights")) + ")");
  say("");
  const int64_t nAbsorb =
      (r.contains("absorb") && r.at("absorb").is_array()) ? static_cast<int64_t>(r.at("absorb").size()) : 0;
  // Stata's standard 78-column estimation header: title padded to 55, then a
  // 14-wide key, "=", and the value right-aligned in 8 (StataBE 19.5, BUG-1488:
  // `IW estimates for dynamic effects` + 23 blanks + `Number of obs =     180`,
  // `Adj R-squared = -0.0990`).
  auto hdr = [&](const std::string& title, const std::string& key, const std::string& val) {
    return padRight(title, 55) + padRight(key, 14) + "=" + padLeft(val, 8);
  };
  say(hdr("IW estimates for dynamic effects", "Number of obs", commaGroup(inum(r, "n_obs"))));
  say(hdr("Absorbing " + std::to_string(nAbsorb) + " HDFE groups",
          "F(" + std::to_string(inum(r, "df1")) + ", " + std::to_string(inum(r, "df2")) + ")",
          f2(num(r, "F"))));
  say(hdr("", "Prob > F", f4(num(r, "prob_F"))));
  say(hdr("", "R-squared", f4(num(r, "r2"))));
  say(hdr("", "Adj R-squared", f4(num(r, "r2_a"))));
  say(hdr("", "Root MSE", f4(num(r, "rmse"))));
  const int64_t nClust = inum(r, "n_clusters");
  if (nClust > 0) {
    say(padLeft("(Std. err. adjusted for " + commaGroup(nClust) + " clusters in " +
                    str(r, "cluster") + ")",
                78));
  }

  const std::string lvl = levelWord(num(r, "level", 95.0));
  const std::string rule78(78, '-');
  say(rule78);
  say(padLeft("", 12) + " |" + padLeft("Robust", 21));
  say(padLeft(str(r, "y"), 12) + " |" + coefHeader("std. err.", "t", lvl));
  say(std::string(13, '-') + "+" + std::string(64, '-'));

  ojson coeftable = ojson::array();
  for (const auto& name : orderedCoefNames(r, rel)) {
    auto b = mapNum(r, "coef", name);
    auto se = mapNum(r, "se", name);
    auto t = mapNum(r, "tstat", name);
    auto p = mapNum(r, "pvalue", name);
    auto lo = mapNum(r, "ci_lo", name);
    auto hi = mapNum(r, "ci_hi", name);
    // A period no row has: the engine's coefficient is missing and Stata lists the
    // row as omitted (StataBE 19.5: `          r9 |          0  (omitted)`).
    if (!b.has_value()) {
      say(padLeft(name, 12) + " |" + padLeft("0", 11) + "  (omitted)");
      coeftable.push_back(coefRow(name, b, se, t, p, lo, hi));
      continue;
    }
    say(padLeft(name, 12) + " |" + padLeft(g9cell(b.value_or(0.0)), 11) +
        padLeft(g9cell(se.value_or(0.0)), 11) + padLeft(f2(t.value_or(0.0)), 9) +
        padLeft(f3(p.value_or(0.0)), 8) + padLeft(g9cell(lo.value_or(0.0)), 13) +
        padLeft(g9cell(hi.value_or(0.0)), 12));
    coeftable.push_back(coefRow(name, b, se, t, p, lo, hi));
  }
  say(rule78);

  // ONE object serves both consumers: `e()`/`_b[]` (macros.cpp reads it as
  // LastModel::result) and eststo/esttab (which snapshots lastEstBlock and
  // wants an estimation-shaped block with coeftable/n_obs/r2/depvar).
  ojson e;
  e["type"] = "estimation";
  e["estimator"] = "eventstudyinteract";
  e["depvar"] = str(r, "y");
  e["coeftable"] = coeftable;
  e["n_obs"] = inum(r, "n_obs");
  e["n_clusters"] = nClust;
  e["F"] = num(r, "F");
  e["p_F"] = num(r, "prob_F");
  e["r2"] = num(r, "r2");
  e["adj_r2"] = num(r, "r2_a");
  e["rmse"] = num(r, "rmse");
  e["df_m"] = inum(r, "df1");
  e["df_r"] = inum(r, "df2");
  postEClass(session, "eventstudyinteract", str(r, "y"), *session.currentHbin(), e, true,
             wtype.empty() ? std::string() : *parsed.weightVar, wtype);
  session.lastEstBlock = e;
  // e(b_iw) and e(V_iw), named by the relative-period indicators (StataBE 19.5,
  // BUG-1524: `matrix list e(b_iw)` is 1 x 3, row r1, columns rm2 r0 r1; e(V_iw)
  // 3 x 3 on the same names; `ereturn list` shows e(V_iw) before e(b_iw)). A
  // period with no observation (the engine's missing coefficient) is 0 there, as
  // Stata's omitted column.
  if (r.contains("rel") && r.at("rel").is_array() && r.contains("V_iw") &&
      r.at("V_iw").is_array() && r.at("V_iw").size() == r.at("rel").size()) {
    eval::EvalMatrix bIw, vIw;
    bIw.rownames = {"r1"};
    bIw.data.emplace_back();
    for (const auto& nm : r.at("rel")) {
      const std::string name = nm.is_string() ? nm.get<std::string>() : std::string();
      bIw.colnames.push_back(name);
      bIw.data[0].push_back(mapNum(r, "coef", name).value_or(0.0));
    }
    vIw.rownames = vIw.colnames = bIw.colnames;
    for (const auto& row : r.at("V_iw")) {
      std::vector<double> vals;
      if (row.is_array())
        for (const auto& v : row) vals.push_back(v.is_number() ? v.get<double>() : std::nan(""));
      vals.resize(bIw.colnames.size(), std::nan(""));
      vIw.data.push_back(std::move(vals));
    }
    // Recorded with noteEOnFit, so the next fit's install drops them.
    auto put = [&session](const std::string& name, eval::EvalMatrix m) {
      auto& store = session.eval.eMatrices;
      store.erase(std::remove_if(store.begin(), store.end(),
                                 [&name](const auto& p) { return p.first == name; }),
                  store.end());
      store.emplace_back(name, std::move(m));
      session.noteEOnFit(name);
    };
    put("V_iw", std::move(vIw));
    put("b_iw", std::move(bIw));
  }

  ojson out = r;
  out["type"] = "qe_result";
  say.lines.push_back(out);
  return say.lines;
}

}  // namespace handlers
}  // namespace piet
