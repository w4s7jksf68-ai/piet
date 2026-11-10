// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Multiple imputation.
#include "piet/handlers/estimation.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/inrange_filter.h"
#include "piet/settings.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "matrix/lapack_dsyevr.h"  // BUG-1482: the clustered V's rank (clusteredVRankM4)
#include "estimation_internal.h"

#include "estimation_families_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace fit_detail {}
using namespace fit_detail;
using namespace estimation_vocab;

ojson runMi(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.varlist.empty())
    return engineErrorBlockM4("syntax_error: mi requires a subcommand", "syntax_error");
  const std::string sub = lowerM4(trimM4(parsed.varlist[0]));

  if (sub == "set") {
    if (parsed.varlist.size() < 2)
      return engineErrorBlockM4("syntax_error: mi set requires a style", "syntax_error");
    const std::string style = lowerM4(trimM4(parsed.varlist[1]));
    if (style != "mlong")
      return engineErrorBlockM4(
          "`mi set " + style + "` is a valid Stata style that this version of PIET does not "
          "implement (only `mlong` is available).",
          "unsupported_command");
    SessionState::MiState st;
    st.style = "mlong";
    session.miState = st;
    ojson b;
    b["type"] = "mi";
    b["subcommand"] = "set";
    b["style"] = "mlong";
    return b;
  }

  if (sub == "register") {
    if (!session.miState.has_value())
      return engineErrorBlockM4("mi register: the data are not mi set; run `mi set mlong` first.",
                                "unsupported_option");
    if (parsed.varlist.size() < 3)
      return engineErrorBlockM4("syntax_error: mi register requires a class and a varlist",
                                "syntax_error");
    const std::string cls = lowerM4(trimM4(parsed.varlist[1]));
    if (cls != "imputed")
      return engineErrorBlockM4(
          "`mi register " + cls + "` is a valid Stata class that this version of PIET does not "
          "implement (only `imputed` is available).",
          "unsupported_command");
    if (!session.currentHbin().has_value()) return eval::noDataLoaded();
    std::vector<std::string> vars(parsed.varlist.begin() + 2, parsed.varlist.end());
    for (const auto& v : vars) session.miState->imputed.push_back(v);
    // Count the rows this marks incomplete — the number Stata prints. Uses the
    // ordinary count path so there is no second missing-value rule here.
    // `eval --summarize` reports N = the NON-missing count per variable, plus the
    // dataset's own row count; the incomplete count Stata prints is the
    // difference. Derived rather than assumed — reading a nonexistent
    // "n_missing" key silently produced 0 here, which is the shape of a count
    // that looks reported but never was.
    int64_t marked = 0;
    for (const auto& v : vars) {
      std::vector<std::string> argv = {"eval", "--data", *session.currentHbin(), "--summarize", v};
      try {
        ojson raw = runCobraJsonM4(argv);
        if (!raw.contains("n_rows") || !raw.contains("summarize") ||
            !raw["summarize"].contains(v) || !raw["summarize"][v].contains("N"))
          return engineErrorBlockM4(
              "mi register: could not determine how many observations of `" + v + "` are missing",
              "engine_error");
        marked += raw["n_rows"].get<int64_t>() - raw["summarize"][v]["N"].get<int64_t>();
      } catch (const CobraError& ce) {
        return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
      }
    }
    ojson b;
    b["type"] = "mi";
    b["subcommand"] = "register";
    b["class"] = "imputed";
    b["vars"] = vars;
    b["n_marked_incomplete"] = marked;
    return b;
  }

  if (sub == "impute") {
    if (!session.currentHbin().has_value()) return eval::noDataLoaded();
    if (!session.miState.has_value())
      return engineErrorBlockM4("mi impute: the data are not mi set; run `mi set mlong` first.",
                                "unsupported_option");
    if (parsed.varlist.size() < 2)
      return engineErrorBlockM4("syntax_error: mi impute requires a method", "syntax_error");
    const std::string method = lowerM4(trimM4(parsed.varlist[1]));
    if (method != "regress")
      return engineErrorBlockM4(
          "`mi impute " + method + "` is a valid Stata method that this version of PIET does not "
          "implement (only `regress` is available).",
          "unsupported_command");
    // `mi impute regress Y = X1 X2` — the tokenizer leaves `=` its own token.
    std::vector<std::string> rest(parsed.varlist.begin() + 2, parsed.varlist.end());
    size_t eq = rest.size();
    for (size_t i = 0; i < rest.size(); ++i)
      if (rest[i] == "=") { eq = i; break; }
    if (eq == rest.size() || eq == 0 || eq + 1 >= rest.size())
      return engineErrorBlockM4("syntax_error: mi impute regress requires `Y = X1 X2`",
                                "syntax_error");
    if (eq != 1)
      return engineErrorBlockM4(
          "`mi impute regress` with more than one imputed variable is a valid Stata form that this "
          "version of PIET does not implement.",
          "unsupported_command");
    const std::string yv = rest[0];
    const std::vector<std::string> xv(rest.begin() + static_cast<long>(eq) + 1, rest.end());
    const auto& reg = session.miState->imputed;
    if (std::find(reg.begin(), reg.end(), yv) == reg.end())
      return engineErrorBlockM4(
          "mi impute regress: `" + yv + "` is not registered as imputed; run "
          "`mi register imputed " + yv + "` first.",
          "unsupported_option");

    std::string add = "5", seed;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "add" && !v.isBool) add = trimM4(v.strValue);
      else if (k == "rseed" && !v.isBool) seed = trimM4(v.strValue);
      else if (k == "update" && !v.isBool)
        return engineErrorBlockM4(
            "Option `update()` is a valid Stata option that this version of PIET does not "
            "implement for `mi impute` (only `add()` is available).",
            "unsupported_option");
    }
    std::string outPath = nextPathM4(session.workdir(), "mi_mlong");
    std::vector<std::string> argv = {"mi-impute-regress", "--data", *session.currentHbin(),
                                     "--y", yv, "--x", joinCommaM4(xv), "--add", add,
                                     "--out-data", outPath};
    if (!seed.empty()) { argv.push_back("--seed"); argv.push_back(seed); }
    // ONE-SHOT deliberately, not the session-routed runner: --out-data writes a
    // dataset, and under the resident-frame serve path commit_frame STAGES the
    // frame instead of writing the file — so the file PIET then loads would never
    // exist. (`--out-data` is not in that path's --data/--out guard, so it took
    // the staging branch silently; measured here as "cannot open <mi_mlong_...>").
    CobraRunResult cr = runCobraOneShot(argv);
    if (!cr.ok) {
      // Prefer the engine's own error envelope when it produced one; fall back to
      // stderr so a launch failure is never reported as an empty message.
      std::string msg, code = "engine_error";
      try {
        ojson e = ojson::parse(cr.stdout_text);
        if (e.contains("error")) {
          msg = e["error"].value("message", std::string());
          code = e["error"].value("code", std::string("engine_error"));
        }
      } catch (const std::exception&) {
      }
      if (msg.empty()) msg = cr.stderr_text.empty() ? cr.error : cr.stderr_text;
      return engineErrorBlockM4("mi impute regress: " + msg, code);
    }
    ojson raw;
    try {
      raw = ojson::parse(cr.stdout_text);
    } catch (const std::exception&) {
      return engineErrorBlockM4("mi impute regress: could not parse the engine response",
                                "engine_error");
    }
    session.load(outPath);
    session.miState->m += static_cast<int>(std::stol(add));
    ojson block = raw;
    block["type"] = "mi";
    block["subcommand"] = "impute";
    block["method"] = "regress";
    return block;
  }

  return engineErrorBlockM4(
      "`mi " + sub + "` is a valid Stata subcommand that this version of PIET does not implement "
      "(set, register and impute regress are available).",
      "unsupported_command");
}

// ===========================================================================
// P7 — dynamic panel: `xtabond depvar indepvars, lags(#) vce(robust)`
//
// Needs the xtset panel context, which the engine takes as `--xtset id,t` (the
// same channel xtreg uses). Only the one-step estimator exists; two-step and the
// conventional (non-robust) GMM VCE reject rather than quietly returning
// one-step numbers under another name.


}  // namespace piet::handlers
