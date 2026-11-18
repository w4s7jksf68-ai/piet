// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/data_values.h"
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
#include "dataset_shape_options.h"

namespace piet::handlers {

using namespace dataset_shape_detail;

// ===========================================================================
// mvencode
// ===========================================================================

ojson runMvencode(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMvencode.known,
                                            kVocabMvencode.unimplemented))
    return ojson(*err);
  // `if` restricts both the override check and the recode to the marked sample
  // (mvencode marks the sample without requiring a varlist, then every
  // count/replace is restricted to it); 007's Wave 2 cleaning runs `mvencode ... if consentall == 1`.
  // `in` stays refused.
  std::string ifCond;
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ifCond = *parsed.ifExpr;
  {
    eval::ParsedCommand noIf = parsed;
    noIf.ifExpr.reset();
    if (auto q = refuseQualifiers(noIf, "mvencode")) return *q;
  }
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty())
    return rsSyntax("mvencode: syntax is `mvencode varlist | _all, mv(#) [override]`");

  const std::optional<std::string> mvOpt = optStr(parsed.opts, "mv");
  if (!mvOpt.has_value())
    return rsSyntax("mvencode: mv(#) is required");
  // The per-missing-code rule list, `mv(. = 9 \ .a = 8)`, encodes EACH extended
  // missing separately. This build's frame collapses every extended missing to
  // plain `.` (the .dta reader/writer Q5 ruling), so honouring only the first
  // rule would silently recode `.a` as if it were `.`.
  if (mvOpt->find('=') != std::string::npos || mvOpt->find('\\') != std::string::npos)
    return rsError("mvencode: the per-code rule list `mv(" + *mvOpt +
                       ")` is not implemented — this build stores one missing value (`.`), so a "
                       "rule list that distinguishes .a/.b/... cannot be honoured.",
                   "unimplemented");
  double mvValue = 0.0;
  if (!parseStrictDouble(*mvOpt, &mvValue))
    return rsSyntax("mvencode: mv(" + *mvOpt + ") requires a number");
  const bool allowOverride = hasFlag(parsed.opts, "override");

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("mvencode: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());

  std::vector<std::string> targets;
  std::vector<std::string> ignoredStrings;  // the ado names each string variable it skips
  const bool all = parsed.varlist.size() == 1 && lowerOf(parsed.varlist[0]) == "_all";
  if (all) {
    for (const auto& nm : ds->names()) {
      if (ds->dtype(nm) == "f64") targets.push_back(nm);
      else ignoredStrings.push_back(nm);
    }
  } else {
    for (const auto& v : parsed.varlist) {
      if (!ds->has(v)) return rsError("variable " + v + " not found");
      // Stata skips string variables rather than failing the command.
      if (ds->dtype(v) == "f64") targets.push_back(v);
      else ignoredStrings.push_back(v);
    }
  }
  // mvencode.ado's r(N) is the result of the last `count` it runs: the sample size
  // when it only skipped strings, else the missing values of the last numeric variable
  // (counted just before they are recoded).
  auto abbrev12 = [](const std::string& name) {
    return name.size() <= 12 ? name : name.substr(0, 10) + "~" + name.substr(name.size() - 1);
  };
  auto countQuietly = [&](const std::string& cond) -> std::optional<ojson> {
    ojson r = dispatch("quietly count" + (cond.empty() ? std::string() : " if " + cond), session);
    for (const auto& b : r)
      if (b.is_object() && b.value("type", std::string()) == "error") return b;
    return std::nullopt;
  };
  if (targets.empty()) {
    std::string text;
    if (!allowOverride)
      for (const auto& v : ignoredStrings)
        text += std::string(abbrev12(v).size() < 12 ? 12 - abbrev12(v).size() : 0, ' ') +
                abbrev12(v) + ": string variable ignored\n";
    if (auto e = countQuietly(ifCond.empty() ? std::string() : "(" + ifCond + ")")) return *e;
    return text.empty() ? ojson::array() : ojson::array({displayBlock(text)});
  }

  // PRE-SCAN, before any write: encoding into a value the data already holds
  // makes the two indistinguishable afterwards, which is why Stata demands
  // `override` for it. The ado counts the sample's rows holding the value (a
  // float variable is compared with float(#)), reports each variable that does,
  // and stops with r(9) after the last one; r(N) keeps that last count.
  std::string ignoredText;
  const std::string mvText = strutil::trim(*mvOpt);  // as the user typed it
  if (!allowOverride) {
    for (const auto& v : ignoredStrings)
      ignoredText += std::string(abbrev12(v).size() < 12 ? 12 - abbrev12(v).size() : 0, ' ') +
                     abbrev12(v) + ": string variable ignored\n";
    std::string clashText;
    for (const auto& v : targets) {
      const bool isFloat = storage::resolveReplaceType(session, v) == "float";
      const std::string lit = eval::pythonFloatRepr(mvValue);
      const std::string rhs = isFloat ? "float(" + lit + ")" : lit;
      if (auto e = countQuietly(v + " == " + rhs + (ifCond.empty() ? "" : " & (" + ifCond + ")")))
        return *e;
      const double* nn = session.eval.rValues.find("N");
      const int64_t hits = nn != nullptr ? static_cast<int64_t>(*nn) : 0;
      if (hits > 0) {
        std::string cnt = std::to_string(hits);
        if (cnt.size() < 4) cnt.insert(0, 4 - cnt.size(), ' ');
        clashText += std::string(abbrev12(v).size() < 12 ? 12 - abbrev12(v).size() : 0, ' ') +
                     abbrev12(v) + ": already " + mvText + " in " + cnt +
                     (hits == 1 ? " observation" : " observations") + "\n";
      }
    }
    if (!clashText.empty()) {
      ojson err = rsErrorRc("no action taken", 9);
      ojson blocks = ojson::array();
      blocks.push_back(displayBlock(ignoredText + clashText));
      blocks.push_back(err);
      return blocks;
    }
  }

  // One `--gen` per engine call, each step adopting its own output as the head
  // before the next reads it — byte-for-byte the shape a sequence of `replace`
  // commands already takes (data_generate.cpp's runReplace), which is what makes the
  // resident-frame opt-in safe here: no intermediate is ever read as a file
  // behind the session's back.
  if (auto e = countQuietly("missing(" + targets.back() + ")" +
                            (ifCond.empty() ? std::string() : " & (" + ifCond + ")")))
    return *e;
  SessionState::ResidentEligible resident(session);
  for (const auto& v : targets) {
    const std::string out = nextPath(session.workdir(), "mvencode");
    const std::string cond =
        ifCond.empty() ? "missing(" + v + ")" : "missing(" + v + ") & (" + ifCond + ")";
    const std::string expr = v + "=cond(" + cond + "," + eval::pythonFloatRepr(mvValue) + "," + v + ")";
    if (auto err = runVerb({"eval", "--data", *session.currentHbin(), "--gen", expr, "--out", out}))
      return *err;
    session.load(out);
  }
  // In-place rewrites: they belong in the rewrittenVars slot (the LAST
  // parameter), not droppedVars (the third) — mutation_result.h's own warning.
  ojson mut = mutationResult(session, "gen", {}, {}, targets);
  if (ignoredText.empty()) return mut;
  return ojson::array({displayBlock(ignoredText), mut});
}

}  // namespace piet::handlers
