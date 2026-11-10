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
const piet::eval::OptVocab& kVocabDidImputation = piet::eval::registerOptVocab(
    {"did_imputation"}, {{"horizons", "pretrends", "autosample",
                                                          "level"},
     {"fe", "controls", "unitcontrols", "timecontrols", "cluster", "wtr", "delta",
      "allhorizons", "avgeffectsby", "saveweights", "minn", "shift", "maxit", "tol",
      "loadweights", "autosample_off", "leaveout", "nose"}});
}  // namespace

namespace piet {
namespace handlers {

using ojson = nlohmann::ordered_json;
using namespace engine_report_detail;

// ===========================================================================
// did_imputation y ivar tvar gvar [if], horizons(numlist) pretrends(#) autosample level(#)
//
// Oracle: p3_stata.log:320-332. Note the z column (not t) and the verbatim
// autosample warning line, which comes straight from the engine's own
// `autosample_message` rather than being reconstructed here.
ojson runDidImputation(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() != 4) {
    return qeErrorBlocks(
        "syntax_error: did_imputation takes four variables positionally — "
        "`did_imputation Y ivar tvar gvar`.",
        "syntax_error");
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabDidImputation.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabDidImputation.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }

  std::vector<std::string> argv = {"did_imputation", "--y",    parsed.varlist[0],
                                   "--ivar",         parsed.varlist[1],
                                   "--time",         parsed.varlist[2],
                                   "--gvar",         parsed.varlist[3]};
  pushOpt(argv, parsed, "horizons", "--horizons");
  pushOpt(argv, parsed, "pretrends", "--pretrends");
  if (optFlag(parsed, "autosample")) argv.push_back("--autosample");
  pushLevel(argv, parsed, session);
  pushKeepif(argv, parsed);
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  Printer say;
  const std::string warn = str(r, "autosample_message");
  if (!warn.empty()) {
    say(warn);
    say("");
  }
  // Unlike the eventstudyinteract header, did_imputation has no left-hand
  // title, and Stata right-aligns the whole "Number of obs = N" phrase in the
  // 78-column band (one column further right than the titled form).
  say(padLeft("Number of obs = " + commaGroup(inum(r, "n_obs")), 78));

  const std::string lvl = levelWord(num(r, "level", 95.0));
  const std::string rule78(78, '-');
  say(rule78);
  say(padLeft(str(r, "y"), 12) + " |" + coefHeader("Std. err.", "z", lvl));
  say(std::string(13, '-') + "+" + std::string(64, '-'));

  // Stata's row order is every horizon's tau, THEN the pretrends — not the
  // alphabetical order the engine's key-sorted JSON hands over.
  std::vector<std::string> wanted;
  if (r.contains("horizons") && r.at("horizons").is_array()) {
    for (const auto& h : r.at("horizons")) {
      if (h.is_number()) {
        wanted.push_back("tau" + std::to_string(static_cast<long long>(h.get<double>())));
      }
    }
  }
  for (int64_t k = 1; k <= inum(r, "pretrends"); ++k) {
    wanted.push_back("pre" + std::to_string(k));
  }

  ojson coeftable = ojson::array();
  for (const auto& name : orderedCoefNames(r, wanted)) {
    auto b = mapNum(r, "coef", name);
    auto se = mapNum(r, "se", name);
    auto z = mapNum(r, "zstat", name);
    auto p = mapNum(r, "pvalue", name);
    auto lo = mapNum(r, "ci_lo", name);
    auto hi = mapNum(r, "ci_hi", name);
    say(padLeft(name, 12) + " |" + padLeft(g9cell(b.value_or(0.0)), 11) +
        padLeft(g9cell(se.value_or(0.0)), 11) + padLeft(f2(z.value_or(0.0)), 9) +
        padLeft(f3(p.value_or(0.0)), 8) + padLeft(g9cell(lo.value_or(0.0)), 13) +
        padLeft(g9cell(hi.value_or(0.0)), 12));
    coeftable.push_back(coefRow(name, b, se, z, p, lo, hi));
  }
  say(rule78);

  // e(): the coefficient vector and N. No F/R2 — did_imputation computes
  // neither, so e(F)/e(r2) stay absent rather than reporting a stale model's.
  ojson e;
  e["type"] = "estimation";
  e["estimator"] = "did_imputation";
  e["depvar"] = str(r, "y");
  e["coeftable"] = coeftable;
  e["n_obs"] = inum(r, "n_obs");
  e["n_clusters"] = inum(r, "n_clusters");
  postEClass(session, "did_imputation", str(r, "y"), *session.currentHbin(), e);
  session.lastEstBlock = e;

  ojson out = r;
  out["type"] = "qe_result";
  say.lines.push_back(out);
  return say.lines;
}

}  // namespace handlers
}  // namespace piet
