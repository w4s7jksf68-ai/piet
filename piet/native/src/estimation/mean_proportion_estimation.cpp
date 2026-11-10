// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/estimation.h"
#include "piet/eval/exprfns.h"  // B3(c): exprHasRunningSum

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>

#include "estimation_internal.h"  // e(sample) T2: the esample helpers' declarations
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/counters.h"  // PP-L1 use-plane split counters (runUse)
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/storage_type.h"  // BUG-471 T2: --store-type
#include "piet/strmutate.h"      // BUG-1326: assert's string-condition count
#include "core/exact_sum.h"  // cecon::ExactSum: summarize's accumulation (BUG-1013)
#include "estimators/posthest.h"  // fitted design atoms for named absorbed effects
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)
#include "io/columnar.h"  // cecon::hbin_level_unescape: .dta label text (BUG-1372)
#include "piet/strutil.h"
#include "piet/translate.h"
#include "engine/engine_call_internal.h"
#include "linear_estimation_internal.h"

// ---------------------------------------------------------------------------
// BUG-233 / FIX:OPTNET-ABBREV-SWEEP — this file's option vocabularies.
//
// Every set below used to be a function-local `static const` sitting next to its
// reject net, which meant nothing upstream of the handler could see it — and the
// handler's own `k == "fullname"` comparisons therefore ran against RAW keys
// while eval::rejectUnknownOptions had already ACCEPTED an abbreviation of them.
// That is BUG-231's mechanism, and it was live in nets across six files.
//
// Registering here (static init, before any dispatch) lets dispatcher.cpp's single
// resolution point canonicalize the keys BEFORE routing. The sets did not get
// copied: each net now binds a reference back to its registered entry, so there is
// still exactly one place each spelling is written.
namespace {
const piet::eval::OptVocab& kVocabMeanProportion = piet::eval::registerOptVocab(
    {"mean", "proportion"}, {{"level"},
     {}});
}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

using namespace engine_call_detail;
using linear_estimation_detail::stataRefusalBlock;
using linear_estimation_detail::joinComma;
using linear_estimation_detail::cobraToEstimation;

// ---------------------------------------------------------------------------
// P3 weights wave: `mean`/`proportion` — NEW commands, both real Stata
// ECLASS estimators (e(b)/_b[...] access afterward), routed to cobra's own
// `mean`/`proportion` verbs. Modeled on runRegress just above (closest
// existing handler that builds a coefficient-table estimation block from a
// cobra call) and runTtest (descriptive/descriptive_statistics.cpp)'s level()-fallback precedent.
// registry.cpp's PweightOnly row on both means the tokenizer only lets
// [pw=]/[pweight=] through here (real Stata itself restricts both commands
// to probability weights only). NOT declared in piet/handlers.h (out of
// this batch's edit manifest — owned by a different stint); dispatcher.cpp
// forward-declares runMean/runProportion locally, the same trick this file
// uses for runSummarizeAweight above.
namespace {

ojson runMeanOrProportion(const eval::ParsedCommand& parsed, SessionState& session,
                          const std::string& stataCmd, const std::string& cobraCmd) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.empty()) {
    return engineErrorBlock("syntax_error: " + stataCmd + " requires at least one variable",
                            "syntax_error");
  }

  std::optional<int> level;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      try {
        level = std::stoi(v.strValue);
      } catch (...) {
      }
    }
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnownOpts = kVocabMeanProportion.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnownOpts)) {
    return *err;
  }
  if (level.has_value() && (*level < 1 || *level > 99)) {
    return engineErrorBlock("level(#) must be an integer between 1 and 99.",
                            "unsupported_option");
  }

  std::vector<std::string> argv = {cobraCmd, "--vars", joinComma(parsed.varlist)};

  // registry.cpp's PweightOnly row means `parsed.weightVar` only survives the
  // tokenizer's guard here when weightType is pw/pweight — safe to forward
  // unconditionally as --wtype pw (no other weight type can reach this
  // handler at all).
  // QF-MEAN-AW: `mean [aw=]` (the tokenizer admits aweight for mean only) is
  // forwarded as --wtype aw (w2aj_qualifiers.log:1983 qf_mn_aw_sp).
  std::string fitWtype;
  if (parsed.weightVar.has_value() && !parsed.weightVar->empty()) {
    // mean's row admits every weight type (the engine serves all four);
    // proportion's is PweightOnly.
    std::string wt = "pw";
    if (parsed.weightType.has_value()) {
      const std::string t = strutil::lower(*parsed.weightType);
      if (t == "aw" || t == "aweight") wt = "aw";
      else if (t == "fw" || t == "fweight") wt = "fw";
      else if (t == "iw" || t == "iweight") wt = "iw";
    }
    argv.push_back("--weight");
    argv.push_back(*parsed.weightVar);
    argv.push_back("--wtype");
    argv.push_back(wt);
    fitWtype = wt;
  }
  if (level.has_value()) {
    argv.push_back("--level");
    argv.push_back(std::to_string(*level));
  } else if (session.level != 95) {
    // ttest's own session.level fallback precedent (descriptive/descriptive_statistics.cpp,
    // BS-048): the frozen CLI surface documents --level on both mean and
    // proportion, so a prior `set level` carries through the same way.
    argv.push_back("--level");
    argv.push_back(std::to_string(session.level));
  }

  InRangeFilterResult filt;
  try {
    filt = applyIfAndInFilter(session, *session.currentHbin(), parsed.ifExpr, parsed.inRange);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  if (!filt.ok) return inRangeRejectError(filt, *parsed.inRange, stataCmd);

  argv.push_back("--data");
  argv.push_back(filt.dataPath);
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJson(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }

  // The frozen mean/proportion envelope names its per-row array "coefs", not
  // "coeftable" like every other estimator this file wires — cobraToEstimation
  // AND macros.cpp's e()/_b[...]/_se[...] resolver (downstream, keyed off
  // session.lastModelResult's "coeftable" field) both look for "coeftable"
  // specifically. Alias it on a copy before handing off, so both the display
  // block and postestimation access see the SAME row shape ({"name","b","se",
  // "t","p","ci_lo","ci_hi"}, already engine-computed) every other estimator
  // uses. The rest of the envelope (n_obs, weight, level, coef, se, sum_w,
  // ...) passes through cobraToEstimation's existing field lists unchanged.
  ojson raw2 = raw;
  if (raw2.contains("coefs") && !raw2.contains("coeftable")) {
    raw2["coeftable"] = raw2["coefs"];
  }

  // proportion's levels are factor-variable levels: StataBE 19.5 refuses a variable
  // with a noninteger or a negative value, r(452) "half:  factor variables may not
  // contain noninteger values" / "neg:  factor variables may not contain negative
  // values" (scratch-laneA2/agents/epost/s11.log), where the engine fits it. A level
  // past 2147483647 is "noninteger" too (2147483648, 1e10, 1e14 and 1e15 alike;
  // epost/s13.log, s14.log). The engine names an integral level by its digits ("-1",
  // "40000") and any other with %g ("1.5", "1e+15").
  if (cobraCmd == "proportion" && raw2.contains("coeftable") && raw2["coeftable"].is_array()) {
    for (const auto& c : raw2["coeftable"]) {
      const std::string nm = c.is_object() ? c.value("name", std::string()) : std::string();
      const size_t colon = nm.rfind(':');
      if (colon == std::string::npos || colon + 1 == nm.size()) continue;
      const std::string var = nm.substr(0, colon), lev = nm.substr(colon + 1);
      const bool integral = lev.find_first_not_of("-0123456789") == std::string::npos;
      const double v = integral ? std::strtod(lev.c_str(), nullptr) : 0.0;
      if (!integral || v > 2147483647.0)
        return stataRefusalBlock(var + ":  factor variables may not contain noninteger values",
                                 452);
      if (v < 0.0)
        return stataRefusalBlock(var + ":  factor variables may not contain negative values",
                                 452);
    }
  }

  ojson block = cobraToEstimation(raw2, cobraCmd, &session);

  // e(b)/_b[...] wiring (same mechanism runRegress uses just above): stash
  // the RAW (coeftable-aliased) result on LastModel so macros.cpp's
  // resolveBRef/resolveEScalar can find it via session.lastModelResult, and
  // session.lastEstBlock for esttab/eststo's snapshot path.
  LastModel lm;
  lm.cobraCmd = cobraCmd;
  lm.y = parsed.varlist[0];
  lm.x = std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end());
  lm.depvar = block.value("depvar", "");
  lm.dataPath = *session.currentHbin();
  lm.result = raw2;
  // e(wtype)/e(wexp), as Stata posts them (BUG-1286: `mean x [w=wi]` fweight, `= wi`).
  if (!fitWtype.empty()) {
    lm.fitWeight = *parsed.weightVar;
    lm.fitWtype = fitWtype;
  }
  // e(vce) analytic, robust under [pw=] (e(vcetype) stays empty) (StataBE 19.5, BUG-1534).
  lm.eVce = fitWtype == "pw" ? "robust" : "analytic";
  session.setLastModel(lm);
  session.lastEstBlock = block;

  // BUG-1534: mean's and proportion's own e() (StataBE 19.5, 12 rows: `mean x` e(df_r) 11,
  // e(N_over) 1, e(k_eq) 1, e(rank) 1, e(title) "Mean estimation", e(properties) "b V",
  // e(varlist) "x", e(estat_cmd) estat_vce_only, e(marginsnotok) _ALL; `proportion d` the
  // same with "Proportion estimation" and e(varlist) "0.d 1.d"; e(df_r) is the reply's dof,
  // N - 1 or the fw/iw weight total - 1). e(rank) follows e(V) below.
  {
    const auto putScalar = [&session](const std::string& n, double v) {
      session.eval.setEScalar(n, v);
      session.noteEOnFit(n);
    };
    const auto putString = [&session](const std::string& n, const std::string& v) {
      session.eval.setEString(n, v);
      session.noteEOnFit(n);
    };
    static const ojson kNoCoefs = ojson::array();
    const ojson& coefs = raw2.contains("coeftable") && raw2["coeftable"].is_array()
                             ? raw2["coeftable"]
                             : kNoCoefs;
    if (raw2.contains("df") && raw2["df"].is_number()) putScalar("df_r", raw2["df"].get<double>());
    putScalar("N_over", 1.0);
    putScalar("k_eq", 1.0);
    // e(b) and e(V) with Stata's stripe (StataBE 19.5, scratch-laneA2/agents/wml/x2.log and
    // scratch-laneA2/agents/epost/s11.log): row y1; mean's columns are its variables (`x
    // x2`); a proportion names each level `#.var`, the first level of a variable with two
    // or more `#bn.var` (`0bn.d 1.d 0bn.d3 1.d3 2.d3`, `proportion one` `1.one`). e(V) is
    // the reply's full covariance, cross-variable terms included; e(rank) is its rank as
    // invsym() counts it (`mean x x3` 1, `proportion d d3` 2, `proportion one` 0). Without
    // the covariance (one observation) nothing is posted and the readers keep their refusal.
    std::vector<std::string> stripe;
    std::vector<double> b, V;
    const ojson* ev = raw2.contains("e_V") && raw2["e_V"].is_object() ? &raw2["e_V"] : nullptr;
    bool stripeOk = ev != nullptr && ev->contains("names") && (*ev)["names"].is_array() &&
                    ev->contains("matrix") && (*ev)["matrix"].is_array() &&
                    (*ev)["names"].size() == coefs.size() &&
                    (*ev)["matrix"].size() == coefs.size() && !coefs.empty();
    const size_t k = stripeOk ? coefs.size() : 0;
    for (size_t j = 0; stripeOk && j < k; ++j) {
      const ojson& c = coefs[j];
      const ojson& nmj = (*ev)["names"][j];
      const ojson& row = (*ev)["matrix"][j];
      stripeOk = c.is_object() && nmj.is_string() && c.value("name", std::string()) == nmj &&
                 c.contains("b") && c["b"].is_number() && row.is_array() && row.size() == k;
      if (!stripeOk) break;
      const std::string nm = nmj.get<std::string>();
      b.push_back(c["b"].get<double>());
      for (size_t l = 0; l < k && stripeOk; ++l) {
        stripeOk = row[l].is_number() && (*ev)["matrix"][l].is_array() &&
                   (*ev)["matrix"][l].size() == k && (*ev)["matrix"][l][j].is_number();
        if (stripeOk)
          V.push_back(0.5 * (row[l].get<double>() + (*ev)["matrix"][l][j].get<double>()));
      }
      if (cobraCmd == "mean") {
        stripeOk = stripeOk && cecon::ident::isValidName(nm);
        stripe.push_back(nm);
        continue;
      }
      const size_t colon = nm.rfind(':');
      const std::string var = colon == std::string::npos ? std::string() : nm.substr(0, colon);
      const std::string lev = colon == std::string::npos ? std::string() : nm.substr(colon + 1);
      // Stata's largest level is maxlong(), `2147483620.v3`; 2147483647 is r(198) "numlist
      // in operator invalid" (epost/s14.log), and the band between is not posted.
      stripeOk = stripeOk && cecon::ident::isValidName(var) && !lev.empty() &&
                 lev.find_first_not_of("0123456789") == std::string::npos &&
                 std::strtod(lev.c_str(), nullptr) <= 2147483620.0;
      // the first level of its variable, and how many levels the variable has
      const bool first = j == 0 || coefs[j - 1].value("name", std::string()).rfind(var + ":", 0) != 0;
      size_t levels = 0;
      for (const auto& o : coefs)
        if (o.value("name", std::string()).rfind(var + ":", 0) == 0) ++levels;
      stripe.push_back(lev + (first && levels >= 2 ? "bn." : ".") + var);
    }
    if (stripeOk) {
      auto& store = session.eval.eMatrices;
      store.erase(std::remove_if(store.begin(), store.end(),
                                 [](const auto& p) { return p.first == "b" || p.first == "V"; }),
                  store.end());
      eval::EvalMatrix mb, mv;
      mb.rownames = {"y1"};
      mb.colnames = stripe;
      mb.data.push_back(b);
      mv.rownames = stripe;
      mv.colnames = stripe;
      for (size_t j = 0; j < k; ++j)
        mv.data.emplace_back(V.begin() + static_cast<std::ptrdiff_t>(j * k),
                             V.begin() + static_cast<std::ptrdiff_t>((j + 1) * k));
      store.emplace_back("b", std::move(mb));
      store.emplace_back("V", std::move(mv));
      putScalar("rank", static_cast<double>(glmVceRank(V, k)));
    }
    putString("title", cobraCmd == "mean" ? "Mean estimation" : "Proportion estimation");
    putString("properties", "b V");
    // the varlist the fit estimated; a proportion names each level as `#.var`
    std::string varlist;
    bool varlistOk = !coefs.empty();
    for (const auto& c : coefs) {
      const std::string nm = c.is_object() ? c.value("name", std::string()) : std::string();
      std::string item = nm;
      if (cobraCmd == "proportion") {
        const size_t colon = nm.rfind(':');
        const std::string lev = colon == std::string::npos ? std::string() : nm.substr(colon + 1);
        varlistOk = varlistOk && !lev.empty() &&
                    lev.find_first_not_of("0123456789") == std::string::npos;
        item = lev + "." + nm.substr(0, colon == std::string::npos ? 0 : colon);
      }
      varlistOk = varlistOk && !nm.empty();
      varlist += (varlist.empty() ? "" : " ") + item;
    }
    if (varlistOk) putString("varlist", varlist);
    putString("estat_cmd", "estat_vce_only");
    putString("marginsnotok", "_ALL");
  }

  return block;
}

}  // namespace

ojson runMean(const eval::ParsedCommand& parsed, SessionState& session) {
  return runMeanOrProportion(parsed, session, "mean", "mean");
}

ojson runProportion(const eval::ParsedCommand& parsed, SessionState& session) {
  return runMeanOrProportion(parsed, session, "proportion", "proportion");
}

}  // namespace piet::handlers
