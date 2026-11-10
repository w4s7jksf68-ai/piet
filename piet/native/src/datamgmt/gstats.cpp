// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/data_values.h"
#include "piet/handlers/descriptive.h"
#include "piet/handlers/gtools_aliases.h"
#include "piet/handlers/variables.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path, cecon::native_exists
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "gtools_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace gtools_detail;

// ---------------------------------------------------------------------------
// (4) gstats multiplex.
// ---------------------------------------------------------------------------

namespace {

ojson gstatsRefusal(const std::string& sub, eval::UnsupportedCommandKind kind) {
  return ojson(eval::unsupportedCommand("gstats " + sub, std::nullopt, std::nullopt,
                                         "refused:gstats_" + sub, kind));
}

// Winsorizing at the 1st and 99th percentile is winsor2's documented default
// and gstats winsor's. It is passed to runWinsor2 EXPLICITLY on every call --
// this handler owns the default, because "whatever the callee does when the
// argument is absent" is how an engine default silently replaces a Stata one
// (the xtile nq 2-vs-4 class, BUG-370).
constexpr double kGstatsWinsorCutLo = 1.0;
constexpr double kGstatsWinsorCutHi = 99.0;
const char* const kGstatsWinsorSuffix = "_w";

}  // namespace

ojson runGstats(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.varlist.empty()) {
    return gtErr("gstats: a sub-command is required (`winsor` or `sum`)", "syntax_error");
  }
  const std::string sub = strutil::lower(parsed.varlist[0]);
  const std::vector<std::string> vars(parsed.varlist.begin() + 1, parsed.varlist.end());

  if (sub == "winsor") {
    // Per-sub reject nets, not one union net over both sub-commands: `gstats
    // sum x, cuts(1 99)` has to be an error, and a union net would accept it
    // and then never read it.
    if (auto err = eval::rejectUnknownOptions(parsed.opts,
                                              {"cuts", "suffix", "trim", "replace", "gen"},
                                              {"by"})) {
      return ojson(*err);
    }
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
      // runWinsor2 has no `if` channel at all, so forwarding the call would
      // winsorize every row at cutpoints drawn from every row while the user
      // asked for a subsample. Refused rather than answered.
      return ojson(eval::unsupportedCommand(
          "gstats winsor with `if` (the cutpoints would come from the full sample)",
          std::nullopt, std::nullopt, "refused:gstats_winsor_if",
          eval::UnsupportedCommandKind::kNotImplemented));
    }
    if (vars.empty()) return gtErr("gstats winsor: no variables specified", "syntax_error");

    double cutLo = kGstatsWinsorCutLo;
    double cutHi = kGstatsWinsorCutHi;
    std::string suffix = kGstatsWinsorSuffix;
    bool trim = false;
    bool replace = false;
    std::vector<std::string> genNames;
    for (const auto& kv : parsed.opts) {
      const std::string& k = kv.first;
      const eval::OptValue& v = kv.second;
      if (k == "cuts") {
        if (v.isBool) return gtErr("gstats winsor: cuts() requires two numbers", "syntax_error");
        const std::vector<std::string> parts = splitWords(v.strValue);
        if (parts.size() != 2) {
          return gtErr("gstats winsor: cuts() requires exactly two values, got '" + v.strValue +
                       "'");
        }
        // Strict consume both, mirroring BUG-112(4) on the winsor2 arm:
        // `cuts(5x 95)` must reject, not read "5x" as 5.
        if (!parseDoubleStrict(parts[0], &cutLo) || !parseDoubleStrict(parts[1], &cutHi)) {
          return gtErr("gstats winsor: cuts(" + v.strValue + ") -- expected two numbers", "syntax_error");
        }
      } else if (k == "suffix") {
        if (v.isBool || v.strValue.empty()) {
          return gtErr("gstats winsor: suffix() requires a suffix string", "syntax_error");
        }
        suffix = v.strValue;
      } else if (k == "trim") {
        trim = true;
      } else if (k == "replace") {
        replace = true;
      } else if (k == "gen") {
        if (v.isBool) {
          return gtErr("gstats winsor: gen() requires one new variable name per winsorized "
                       "variable",
                       "syntax_error");
        }
        genNames = splitWords(v.strValue);
      }
    }
    if (!genNames.empty() && replace) {
      // replace writes back over the source; gen() names a new column. Doing
      // both is two different answers to "where does the result go".
      return gtErr("gstats winsor: gen() and replace are alternatives -- replace writes the "
                   "winsorized values back over the source variables, gen() puts them in new "
                   "ones.");
    }
    if (!genNames.empty() && genNames.size() != vars.size()) {
      return gtErr("gstats winsor: gen() names " + std::to_string(genNames.size()) +
                       " variable(s) for " + std::to_string(vars.size()) +
                       " winsorized variable(s); the two lists must line up one for one.");
    }

    ojson result = runWinsor2(session, vars, cutLo, cutHi, trim, /*by=*/std::string(), suffix,
                              replace);
    if (result.is_object() && result.value("type", std::string()) == "error") return result;
    if (genNames.empty()) return result;

    // gen() is spelled as a rename of the suffixed results because runWinsor2
    // names its outputs `src + suffix` and has no output-name channel. Doing it
    // here rather than teaching winsor2 a new parameter keeps the shim's reach
    // inside this file.
    for (size_t i = 0; i < genNames.size(); ++i) {
      ojson renamed = runRename(vars[i] + suffix, genNames[i], session);
      if (renamed.is_object() && renamed.value("type", std::string()) == "error") {
        // Reported, not swallowed: the earlier renames already landed, so the
        // dataset is half-renamed and the user has to be told which one failed.
        return renamed;
      }
      // The LAST block is what the caller gets: each mutation block carries
      // varlist_after, and only the last one describes the dataset that exists
      // when this handler returns.
      result = std::move(renamed);
    }
    return result;
  }

  if (sub == "sum" || sub == "summarize") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {"detail"})) {
      return ojson(*err);
    }
    bool detail = false;
    for (const auto& kv : parsed.opts) {
      if (kv.first == "detail") detail = true;
    }
    std::optional<std::vector<std::string>> varsOpt;
    if (!vars.empty()) varsOpt = vars;
    // ifExpr is forwarded, not dropped: `gstats sum x if y > 0` is a legal line
    // (the registry row allows `if`), and summarizing every row instead would
    // be a wrong number reported as a right one. runSummarize has the channel.
    return runSummarize(varsOpt, detail, session, parsed.ifExpr);
  }

  // Known gtools sub-commands with no native twin here. Named individually so
  // the reason field distinguishes them and so a later wave can find them.
  static const std::unordered_set<std::string> kKnownSubs = {"tab", "transform", "hdfe", "range",
                                                             "moving"};
  if (kKnownSubs.count(sub) != 0) {
    return gstatsRefusal(sub, eval::UnsupportedCommandKind::kNotImplemented);
  }
  // Not a gstats sub-command at all. kUnknownSubcommand says "the command is
  // implemented, this word is not one of its sub-commands", which is a
  // different sentence from "PIET has not built this yet" and the only one that
  // points at a typo.
  return gstatsRefusal(sub, eval::UnsupportedCommandKind::kUnknownSubcommand);
}

}  // namespace piet::handlers
