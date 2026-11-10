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
const piet::eval::OptVocab& kVocabSdid = piet::eval::registerOptVocab(
    {"sdid"}, {{"vce", "reps", "seed", "level"},
     {"covariates", "method", "graph", "g1on", "g1_opt", "g2_opt", "graph_export",
      "msize", "unstandardized", "mattitles"}});
}  // namespace

namespace piet {
namespace handlers {

using ojson = nlohmann::ordered_json;
using namespace engine_report_detail;

// ===========================================================================
// sdid y ivar tvar treated [if], vce(bootstrap|noinference) reps(#) seed(#) level(#)
//
// Oracle: p3_stata.log:366-378.
//
// OUR SE IS OURS. The bootstrap draws come from this project's own RNG stream,
// so the SE / t / CI cells legitimately differ from Stata's. The ATT and the
// table shape are the parity claim.
ojson runSdid(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() != 4) {
    return qeErrorBlocks(
        "syntax_error: sdid takes four variables positionally — `sdid Y ivar tvar treated`.",
        "syntax_error");
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabSdid.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabSdid.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }

  std::vector<std::string> argv = {"sdid",      "--y",    parsed.varlist[0],
                                   "--ivar",    parsed.varlist[1],
                                   "--time",    parsed.varlist[2],
                                   "--treated", parsed.varlist[3]};
  std::string vceKind;
  if (auto vce = optStr(parsed, "vce"); vce.has_value() && !vce->empty()) {
    std::vector<std::string> toks = splitWsOrComma(*vce);
    if (!toks.empty()) vceKind = toks[0];
    argv.push_back("--vce");
    argv.push_back(vceKind);
  }
  pushOpt(argv, parsed, "reps", "--reps");
  pushOpt(argv, parsed, "seed", "--seed");
  pushLevel(argv, parsed, session);
  pushKeepif(argv, parsed);
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  // Stata prints the bootstrap progress ruler BEFORE the replications run;
  // ours is emitted around the (synchronous) engine call, which is the same
  // place in the transcript.
  Printer say;
  const int64_t repsRequested =
      [&]() -> int64_t {
        auto v = optStr(parsed, "reps");
        if (!v.has_value()) return 0;
        try { return std::stoll(*v); } catch (...) { return 0; }
      }();

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  const std::string vceUsed = str(r, "vce", vceKind);
  if (vceUsed == "bootstrap" || vceUsed == "placebo" || vceUsed == "jackknife") {
    const int64_t done = inum(r, "reps_done", repsRequested);
    say("Bootstrap replications (" + std::to_string(done) + "). This may take some time.");
    // Stata's fixed 50-column ruler, one dot per completed replication,
    // wrapping at 50.
    const std::string ruler = "----+--- 1 ---+--- 2 ---+--- 3 ---+--- 4 ---+--- 5";
    say(ruler);
    for (int64_t i = 0; i < done; i += 50) {
      const int64_t n = std::min<int64_t>(50, done - i);
      say(std::string(static_cast<size_t>(n), '.'));
    }
  }
  say("");
  say("Synthetic Difference-in-Differences Estimator");
  say("");

  const std::string lvl = levelWord(num(r, "level", 95.0));
  const std::string rule77(77, '-');
  say(rule77);
  say(padLeft(str(r, "y"), 12) + " |" + "     ATT     Std. Err.     t      P>|t|    [" + lvl +
      "% Conf. Interval]");
  say(std::string(13, '-') + "+" + std::string(63, '-'));
  say(padLeft(parsed.varlist[3], 12) + " |" + padLeft(f5(num(r, "att")), 10) +
      padLeft(f5(num(r, "se")), 11) + padLeft(f2(num(r, "t")), 9) + padLeft(f3(num(r, "p")), 9) +
      padLeft(f5(num(r, "ci_lo")), 12) + padLeft(f5(num(r, "ci_hi")), 12));
  say(rule77);
  say(lvl + "% CIs and p-values are based on large-sample approximations.");
  say("Refer to Arkhangelsky et al., (2021) for theoretical derivations.");

  ojson e;
  e["type"] = "estimation";
  e["estimator"] = "sdid";
  e["depvar"] = str(r, "y");
  e["coeftable"] = ojson::array({coefRow(parsed.varlist[3], num(r, "att"), num(r, "se"),
                                          num(r, "t"), num(r, "p"), num(r, "ci_lo"),
                                          num(r, "ci_hi"))});
  e["n_obs"] = inum(r, "n_obs");
  e["n_units"] = inum(r, "n_units");
  postEClass(session, "sdid", str(r, "y"), *session.currentHbin(), e);
  session.lastEstBlock = e;

  ojson out = r;
  out["type"] = "qe_result";
  say.lines.push_back(out);
  return say.lines;
}

}  // namespace handlers
}  // namespace piet
