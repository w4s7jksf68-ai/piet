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
#include "core/distributions.h"  // cecon::t_critical: a replay's interval at its level()
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
    {"mean", "proportion"}, {{"level", "over", "citype", "vce", "noheader", "nolegend", "nolabel", "stdize",
                               "stdweight", "percent"},
     {"nostdrescale"}});
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
// Shared fit-sample filter (linear_estimation.cpp); keeps each row's position.
InRangeFilterResult applyIfAndInFilterKeepingIndex(SessionState&, const std::string&,
                                                   const std::optional<std::string>&,
                                                   const std::optional<std::string>&,
                                                   std::string*);

namespace {

ojson runMeanOrProportion(const eval::ParsedCommand& parsed, SessionState& session,
                          const std::string& stataCmd, const std::string& cobraCmd) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // A replay (`mean, level(80)`, nothing before the comma) redisplays the live fit:
  // its intervals and r(table) at the level asked for, e() unchanged (vs-stata bank
  // 4204171f `mean, level(80)`: ll 5721.835 = b - t(73, .90) se), and proportion's
  // percent (22edcd43). The other display options of a replay (coeflegend, cformat())
  // are not served, nor a replay of an interval type other than logit / wald.
  if (parsed.varlist.empty() && !parsed.ifExpr.has_value() && !parsed.inRange.has_value() &&
      !parsed.weightType.has_value() && session.lastModel().has_value() &&
      session.lastModel()->cobraCmd == cobraCmd && session.lastEstBlock.has_value() &&
      session.lastEstBlock->value("estimator", std::string()) == cobraCmd) {
    double lv = static_cast<double>(session.level);
    for (const auto& [k, v] : parsed.opts) {
      if (k == "level" && !v.isBool) {
        try {
          lv = std::stod(v.strValue);
        } catch (...) {
          return stataRefusalBlock("level() invalid", 198);
        }
        if (!(lv >= 10.0 && lv <= 99.99))
          return stataRefusalBlock("level() must be between 10 and 99.99 inclusive", 198);
        continue;
      }
      if (k == "percent" && v.isBool && cobraCmd == "proportion") continue;
      return engineErrorBlock("option " + k + " on a replay of " + stataCmd +
                                  " is not implemented in this version of PIET.",
                              "unsupported_option");
    }
    ojson blk = *session.lastEstBlock;
    const std::string ct = blk.value("citype", std::string(cobraCmd == "mean" ? "normal" : "logit"));
    const double df = blk.contains("df") && blk["df"].is_number() ? blk["df"].get<double>() : 0.0;
    if ((cobraCmd == "proportion" && ct != "logit" && ct != "normal") || !(df > 0))
      return engineErrorBlock("a replay of " + stataCmd + " at another level() is not implemented "
                              "for this fit in this version of PIET.",
                              "unsupported_option");
    const double tc = cecon::t_critical(1.0 - lv / 100.0, df);
    for (const char* key : {"coeftable", "coefs"}) {
      if (!blk.contains(key) || !blk[key].is_array()) continue;
      for (auto& row : blk[key]) {
        if (!row.is_object() || !row.contains("b") || !row["b"].is_number() || !row.contains("se") ||
            !row["se"].is_number())
          continue;
        const double b = row["b"].get<double>(), se = row["se"].get<double>();
        double lo = b - tc * se, hi = b + tc * se;
        if (cobraCmd == "proportion" && ct == "logit") {
          if (b <= 0.0 || b >= 1.0 || se == 0.0) {
            lo = b;
            hi = b;
          } else {
            const double g = std::log(b / (1.0 - b)), h = tc * se / (b * (1.0 - b));
            lo = 1.0 / (1.0 + std::exp(-(g - h)));
            hi = 1.0 / (1.0 + std::exp(-(g + h)));
          }
        }
        row["ci_lo"] = lo;
        row["ci_hi"] = hi;
      }
    }
    blk["level"] = lv;
    blk.erase("noheader");  // a display option of the fit's own line, not of the replay
    blk.erase("percent");
    for (const auto& [k, v] : parsed.opts)
      if (k == "percent" && v.isBool) blk["percent"] = true;
    // the replay's table is the live block, so r(table) is posted from it at this level
    session.lastEstBlock = blk;
    return blk;
  }
  if (parsed.varlist.empty()) {
    return engineErrorBlock("syntax_error: " + stataCmd + " requires at least one variable",
                            "syntax_error");
  }

  // proportion's variables are categorical: `i.race` is `race` (vs-stata bank
  // 478e5916 `proportion i.race`: e(b) `1bn.race 2.race 3.race`, as `proportion race`).
  std::vector<std::string> fitVars = parsed.varlist;
  if (cobraCmd == "proportion")
    for (auto& v : fitVars)
      if (v.size() > 2 && (v.compare(0, 2, "i.") == 0 || v.compare(0, 2, "I.") == 0) &&
          v.find_first_of(".#", 2) == std::string::npos)
        v = v.substr(2);
  std::optional<int> level;
  std::string overVar;
  std::optional<std::string> citype;
  std::optional<std::string> vceText;
  bool noHeader = false, noLabel = false, percent = false;
  std::string stdizeVar, stdweightVar;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "stdize") stdizeVar = v.isBool ? std::string() : strutil::trim(v.strValue);
    if (k == "stdweight") stdweightVar = v.isBool ? std::string() : strutil::trim(v.strValue);
    if (k == "noheader" && v.isBool) noHeader = true;
    if (k == "nolabel" && v.isBool) noLabel = true;
    if (k == "percent" && v.isBool) percent = true;
    if (k == "level" && !v.isBool) {
      try {
        level = std::stoi(v.strValue);
      } catch (...) {
      }
    }
    if (k == "over") overVar = v.isBool ? std::string() : strutil::trim(v.strValue);
    if (k == "citype") citype = v.isBool ? std::string() : strutil::trim(v.strValue);
    if (k == "vce") vceText = v.isBool ? std::string() : strutil::trim(v.strValue);
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

  // over(varlist): the estimates within each group of one variable (the engine's
  // --over). A factor-variable operator is r(101) and a string variable r(109), as
  // Stata's over() refuses them (vs-stata bank 7da16fac `over(race#union)`, b8424b27
  // `over(s)`). Several over() variables, and over() with aweights, fweights or
  // iweights, are not served: they stay an honest refusal.
  if (!overVar.empty()) {
    if (overVar.find_first_of("#.") != std::string::npos)
      return stataRefusalBlock("factor-variable and time-series operators not allowed", 101);
    if (overVar.find_first_of(" \t,") != std::string::npos)
      return engineErrorBlock(
          "over() with more than one variable is not implemented for " + stataCmd +
              " in this version of PIET.",
          "unsupported_option");
    Datastore* ods = session.datastore();
    if (ods != nullptr && !ods->has(overVar))
      return stataRefusalBlock("variable " + overVar + " not found", 111);
    if (ods != nullptr && ods->dtype(overVar) == "factor")
      return stataRefusalBlock("type mismatch", 109);
    if (parsed.weightVar.has_value() && !parsed.weightVar->empty() && parsed.weightType.has_value()) {
      const std::string t = strutil::lower(*parsed.weightType);
      if (!(t == "pw" || t == "pweight"))
        return engineErrorBlock(
            stataCmd + " with over() and [" + t +
                "=] is not implemented in this version of PIET (only unweighted and [pweight=] "
                "fits are).",
            "unsupported_option");
    }
  }

  // nolabel is proportion's alone (mean: r(198), as Stata's syntax answers); it names
  // the levels by their values, which is how e(b) names them already, and Stata
  // records it as e(nolabel) (vs-stata bank 945bd7ef `proportion race, nolabel`).
  // nolegend leaves the table as it is (bank 6652367d `proportion race, nol`).
  if (noLabel && cobraCmd != "proportion") return stataRefusalBlock("option nolabel not allowed", 198);
  // percent (proportion's): the table in percent; e() keeps the proportions
  if (percent && cobraCmd != "proportion") return stataRefusalBlock("option percent not allowed", 198);
  // stdize(s) stdweight(w): direct standardization of over()'s groups (the engine's
  // --stdize/--stdweight; vs-stata bank 56f2dacb). Served unweighted, with over() and
  // without vce(cluster); the other forms keep a named refusal.
  if (!stdizeVar.empty() || !stdweightVar.empty()) {
    Datastore* sds = session.datastore();
    for (const auto& v : {stdizeVar, stdweightVar})
      if (!v.empty() && sds != nullptr && !sds->has(v))
        return stataRefusalBlock("variable " + v + " not found", 111);
    if (stdizeVar.empty() || stdweightVar.empty() || overVar.empty() ||
        (parsed.weightVar.has_value() && !parsed.weightVar->empty()) || vceText.has_value())
      return engineErrorBlock(stataCmd +
                                  ": stdize() is implemented in this version of PIET only with "
                                  "stdweight() and over(), unweighted and with the default vce().",
                              "unsupported_option");
  }
  // proportion's citype(type [, unadjusted]): logit (the default), normal (at least
  // `norm`) or wald, wilson, exact, agresti (at least `agres`), jeffreys (at least
  // `jeff`), one type; `unadjusted` (at least `unadj`) is refused with the three
  // se-based types as typed (StataBE 19.5's citype check; vs-stata bank 9ded6371
  // `citype(zz)` r(198) "invalid citype() option; / zz is not a recognized CI type").
  // mean has no such option (r(198)).
  bool ciUnadjusted = false;
  if (citype.has_value()) {
    if (cobraCmd != "proportion") return stataRefusalBlock("option citype() not allowed", 198);
    std::string typed = *citype, sub;
    if (const size_t comma = typed.find(','); comma != std::string::npos) {
      sub = strutil::trim(typed.substr(comma + 1));
      typed = typed.substr(0, comma);
    }
    std::vector<std::string> words;
    {
      std::istringstream ws(typed);
      for (std::string w; ws >> w;) words.push_back(w);
    }
    {
      std::istringstream ss(sub);
      for (std::string w; ss >> w;) {
        if (w.size() >= 5 && std::string("unadjusted").compare(0, w.size(), w) == 0)
          ciUnadjusted = true;
        else
          return stataRefusalBlock("option " + w + " not allowed", 198);
      }
    }
    if (words.size() > 1)
      return stataRefusalBlock("option citype() invalid\nonly one CI type is allowed", 198);
    const std::string nm = words.empty() ? std::string("logit") : words[0];
    const auto abbrev = [&nm](const char* full, size_t minLen) {
      return nm.size() >= minLen && std::string(full).compare(0, nm.size(), nm) == 0;
    };
    std::string kind;
    if (nm == "logit") kind = "logit";
    else if (nm == "wald" || abbrev("normal", 4)) kind = "wald";
    else if (nm == "wilson") kind = "wilson";
    else if (nm == "exact") kind = "exact";
    else if (abbrev("agresti", 5)) kind = "agresti";
    else if (abbrev("jeffreys", 4)) kind = "jeffreys";
    else
      return stataRefusalBlock("invalid citype() option;\n" + nm + " is not a recognized CI type", 198);
    if (ciUnadjusted && (nm == "wald" || nm == "normal" || nm == "logit"))
      return stataRefusalBlock("option unadjusted not allowed with " + nm + " confidence interval", 198);
    citype = kind;
    if (*citype != "logit" && *citype != "wald" && parsed.weightVar.has_value() &&
        !parsed.weightVar->empty())
      return engineErrorBlock("proportion citype(" + *citype +
                                  ") with weights is not implemented in this version of PIET.",
                              "unsupported_option");
  }
  // vce(): analytic (the default) or cluster clustvar -- the linearized variance
  // over the clusters' totals, e(df_r) = clusters - 1 (vs-stata bank bcfd65d0,
  // 33bdd87d). bootstrap and jackknife are not served here.
  std::string clusterVar;
  if (vceText.has_value()) {
    std::istringstream is(*vceText);
    std::string kind, var, extra;
    is >> kind >> var >> extra;
    kind = strutil::lower(kind);
    const bool isCluster = !kind.empty() && std::string("cluster").compare(0, kind.size(), kind) == 0 &&
                           kind.size() >= 2;
    const bool isAnalytic = !kind.empty() && std::string("analytic").compare(0, kind.size(), kind) == 0;
    if (isCluster) {
      if (var.empty() || !extra.empty()) return stataRefusalBlock("option vce() misspecified", 198);
      Datastore* cds = session.datastore();
      if (cds != nullptr && !cds->has(var)) return stataRefusalBlock("variable " + var + " not found", 111);
      clusterVar = var;
      if (parsed.weightVar.has_value() && !parsed.weightVar->empty() && parsed.weightType.has_value()) {
        const std::string t = strutil::lower(*parsed.weightType);
        if (!(t == "pw" || t == "pweight"))
          return engineErrorBlock(stataCmd + " with vce(cluster) and [" + t +
                                      "=] is not implemented in this version of PIET.",
                                  "unsupported_option");
      }
    } else if (!(isAnalytic && var.empty())) {
      return engineErrorBlock("vce(" + *vceText + ") is not implemented for " + stataCmd +
                                  " in this version of PIET (vce(analytic) and vce(cluster "
                                  "clustvar) are).",
                              "unsupported_option");
    }
  }
  std::vector<std::string> argv = {cobraCmd, "--vars", joinComma(fitVars)};
  if (!clusterVar.empty()) {
    argv.push_back("--cluster");
    argv.push_back(clusterVar);
  }
  if (citype.has_value() && *citype != "logit") {
    argv.push_back("--citype");
    argv.push_back(*citype);
    if (ciUnadjusted) argv.push_back("--ci-unadjusted");
  }
  if (!stdizeVar.empty()) {
    argv.push_back("--stdize");
    argv.push_back(stdizeVar);
    argv.push_back("--stdweight");
    argv.push_back(stdweightVar);
  }
  if (!overVar.empty()) {
    argv.push_back("--over");
    argv.push_back(overVar);
  }

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

  // The sample: if/in on a copy that keeps each row's position in the working frame,
  // so the engine can write e(sample) back onto it (regress's T2 install).
  InRangeFilterResult filt;
  std::string esampleIndexVar;
  int64_t esampleHeadRows = 0;
  try {
    filt = applyIfAndInFilterKeepingIndex(session, *session.currentHbin(), parsed.ifExpr,
                                          parsed.inRange, &esampleIndexVar);
    if (!esampleIndexVar.empty()) esampleHeadRows = session.macroObsCount();
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  if (!filt.ok) return inRangeRejectError(filt, *parsed.inRange, stataCmd);

  argv.push_back("--data");
  argv.push_back(filt.dataPath);
  argv.push_back("--tidy");
  const std::string esampleValues =
      esampleFitArgs(session, argv, esampleIndexVar, esampleHeadRows);

  ojson raw;
  try {
    raw = runCobraJson(argv);
  } catch (const CobraError& ce) {
    cecon::native_unlink(esampleValues);
    if (ce.detail == "r(2000)")
      return stataRefusalBlock("no observations", 2000);
    // stdize()'s standard weight that varies within a stratum (bank 839d8173, r(466))
    if (ce.detail == "r(466)")
      return stataRefusalBlock("standardization weights must be constant within standard strata",
                               466);
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

  // proportion's levels are factor-variable levels, and so are over()'s groups:
  // StataBE 19.5 refuses a variable with a noninteger or a negative value, r(452)
  // "half:  factor variables may not contain noninteger values" / "neg:  factor
  // variables may not contain negative values" (scratch-laneA2/agents/epost/s11.log),
  // where the engine fits it. A level past 2147483647 is "noninteger" too (2147483648,
  // 1e10, 1e14 and 1e15 alike; epost/s13.log, s14.log). The engine names an integral
  // level by its digits ("-1", "40000") and any other with %g ("1.5", "1e+15").
  const auto levelRefusal = [](const std::string& var, const std::string& lev) -> std::optional<ojson> {
    const bool integral = !lev.empty() && lev.find_first_not_of("-0123456789") == std::string::npos;
    const double v = integral ? std::strtod(lev.c_str(), nullptr) : 0.0;
    if (!integral || v > 2147483647.0)
      return stataRefusalBlock(var + ":  factor variables may not contain noninteger values", 452);
    if (v < 0.0)
      return stataRefusalBlock(var + ":  factor variables may not contain negative values", 452);
    return std::nullopt;
  };
  std::vector<std::string> overLevels;
  if (!overVar.empty() && raw2.contains("over_levels") && raw2["over_levels"].is_array())
    for (const auto& g : raw2["over_levels"])
      if (g.is_string()) overLevels.push_back(g.get<std::string>());
  for (const auto& g : overLevels) {
    if (auto r = levelRefusal(overVar, g)) {
      cecon::native_unlink(esampleValues);
      return *r;
    }
  }
  // A coefficient's name in the reply: `var`, `var:level`, then `@group` under over().
  struct Col {
    std::string var, lev, grp;
  };
  const auto split = [&](const std::string& nm) {
    Col c;
    std::string rest = nm;
    if (!overVar.empty()) {
      const size_t at = rest.rfind('@');
      if (at != std::string::npos) {
        c.grp = rest.substr(at + 1);
        rest = rest.substr(0, at);
      }
    }
    if (cobraCmd == "proportion") {
      const size_t colon = rest.rfind(':');
      if (colon != std::string::npos) {
        c.lev = rest.substr(colon + 1);
        rest = rest.substr(0, colon);
      }
    }
    c.var = rest;
    return c;
  };
  if (cobraCmd == "proportion" && raw2.contains("coeftable") && raw2["coeftable"].is_array()) {
    for (const auto& c : raw2["coeftable"]) {
      const Col cc = split(c.is_object() ? c.value("name", std::string()) : std::string());
      if (cc.lev.empty()) continue;
      if (auto r = levelRefusal(cc.var, cc.lev)) {
        cecon::native_unlink(esampleValues);
        return *r;
      }
    }
  }

  // The coefficients' Stata names (StataBE 19.5, scratch-laneA2/agents/wml/x2.log,
  // epost/s11.log, vs-stata bank 440bd7fb / 0b672af1 / e8d69b99): mean's columns are
  // its variables (`x x2`); a proportion names each level `#.var`, the first level of a
  // variable with two or more `#bn.var` (`0bn.d 1.d 0bn.d3 1.d3 2.d3`, `proportion one`
  // `1.one`); under over(g) each is followed by `@#.g`, the first group `@#bn.g`, and
  // mean's variable is `c.var` (`c.price@0bn.foreign c.price@1.foreign`, `0bn.foreign@1bn.rep78`).
  static const ojson kNoCoefs0 = ojson::array();
  const ojson& coefs0 = raw2.contains("coeftable") && raw2["coeftable"].is_array()
                            ? raw2["coeftable"]
                            : kNoCoefs0;
  const auto levelName = [](const std::string& lev, bool first, size_t count, const std::string& var) {
    return lev + (first && count >= 2 ? "bn." : ".") + var;
  };
  std::vector<std::string> stripe;
  bool stripeOk = !coefs0.empty();
  for (size_t j = 0; j < coefs0.size(); ++j) {
    const Col c = split(coefs0[j].is_object() ? coefs0[j].value("name", std::string()) : std::string());
    std::string nm;
    if (cobraCmd == "mean") {
      stripeOk = stripeOk && cecon::ident::isValidName(c.var);
      nm = overVar.empty() ? c.var : "c." + c.var;
    } else {
      // Stata's largest level is maxlong(), `2147483620.v3`; 2147483647 is r(198)
      // "numlist in operator invalid" (epost/s14.log), and the band between is not posted.
      stripeOk = stripeOk && cecon::ident::isValidName(c.var) && !c.lev.empty() &&
                 c.lev.find_first_not_of("0123456789") == std::string::npos &&
                 std::strtod(c.lev.c_str(), nullptr) <= 2147483620.0;
      // the first level of its variable, and how many levels the variable has
      std::vector<std::string> levs;
      for (const auto& o : coefs0) {
        const Col oc = split(o.value("name", std::string()));
        if (oc.var == c.var && std::find(levs.begin(), levs.end(), oc.lev) == levs.end())
          levs.push_back(oc.lev);
      }
      nm = levelName(c.lev, !levs.empty() && levs.front() == c.lev, levs.size(), c.var);
    }
    if (!overVar.empty()) {
      stripeOk = stripeOk && !c.grp.empty() && c.grp.find_first_not_of("0123456789") == std::string::npos;
      nm += "@" + levelName(c.grp, !overLevels.empty() && overLevels.front() == c.grp,
                            overLevels.size(), overVar);
    }
    stripe.push_back(nm);
  }
  // Under over() the table's rows carry those names (the reply's are positional).
  std::vector<std::string> replyNames;
  for (const auto& c : coefs0) replyNames.push_back(c.is_object() ? c.value("name", std::string()) : std::string());
  if (!overVar.empty() && stripeOk && raw2.contains("coeftable")) {
    for (size_t j = 0; j < stripe.size(); ++j) raw2["coeftable"][j]["name"] = stripe[j];
    raw2["coefs"] = raw2["coeftable"];
  }

  ojson block = cobraToEstimation(raw2, cobraCmd, &session);
  // the level the fit's intervals use: r(table) is built at it; and their type
  block["level"] = raw2.contains("level") ? raw2["level"] : ojson(95);
  if (cobraCmd == "proportion")
    block["citype"] = !citype.has_value() ? std::string("logit")
                      : *citype == "wald"  ? std::string("normal")
                                           : *citype;
  // vce(cluster): the table's lower-case "std. err." and its "(Std. err. adjusted for
  // # clusters in var)" line are drawn from the block's vcov_type and n_clusters
  if (!clusterVar.empty()) block["vcov_type"] = "cluster(" + clusterVar + ")";
  // noheader: the table without its title and Number of obs line (vs-stata bank d85e3236)
  if (noHeader) block["noheader"] = true;
  if (percent) block["percent"] = true;
  // citype(agresti): an upper limit past 1 was cut to 1, and the table says so below it
  if (raw2.value("agresti_truncated", false)) block["agresti_truncated"] = true;
  // stdize(): the header's "N. of std strata = #" line
  if (raw2.contains("n_stdize") && raw2["n_stdize"].is_number())
    block["n_stdize"] = raw2["n_stdize"];

  // e(b)/_b[...] wiring (same mechanism runRegress uses just above): stash
  // the RAW (coeftable-aliased) result on LastModel so macros.cpp's
  // resolveBRef/resolveEScalar can find it via session.lastModelResult, and
  // session.lastEstBlock for esttab/eststo's snapshot path.
  LastModel lm;
  lm.cobraCmd = cobraCmd;
  lm.y = fitVars[0];
  lm.x = std::vector<std::string>(fitVars.begin() + 1, fitVars.end());
  lm.depvar = block.value("depvar", "");
  lm.dataPath = *session.currentHbin();
  // e(wtype)/e(wexp), as Stata posts them (BUG-1286: `mean x [w=wi]` fweight, `= wi`).
  if (!fitWtype.empty()) {
    lm.fitWeight = *parsed.weightVar;
    lm.fitWtype = fitWtype;
  }
  // e(vce) analytic, robust under [pw=] (e(vcetype) stays empty) (StataBE 19.5, BUG-1534).
  lm.eVce = !clusterVar.empty() ? "cluster" : fitWtype == "pw" ? "robust" : "analytic";
  // e(sample): the rows the fit used (installed before setLastModel, BUG-518 order).
  (void)esampleInstallOnHead(session, esampleValues, raw2, lm);
  lm.result = raw2;
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
    auto& store = session.eval.eMatrices;
    const auto putMatrix = [&](const std::string& name, eval::EvalMatrix m) {
      store.erase(std::remove_if(store.begin(), store.end(),
                                 [&](const auto& p) { return p.first == name; }),
                  store.end());
      store.emplace_back(name, std::move(m));
      session.noteEOnFit(name);
    };
    const ojson& coefs = coefs0;
    if (raw2.contains("df") && raw2["df"].is_number()) putScalar("df_r", raw2["df"].get<double>());
    putScalar("N_over", overVar.empty() ? 1.0 : static_cast<double>(overLevels.size()));
    putScalar("k_eq", 1.0);
    putScalar("version", 2.0);
    if (cobraCmd == "proportion") putScalar("k_eform", 0.0);
    // e(b) and e(V) with Stata's stripe, row y1. e(V) is the reply's full covariance,
    // cross-variable terms included; e(rank) is its rank as invsym() counts it (`mean x x3`
    // 1, `proportion d d3` 2, `proportion one` 0). Without the covariance (one
    // observation) nothing is posted and the readers keep their refusal.
    std::vector<double> b, V;
    const ojson* ev = raw2.contains("e_V") && raw2["e_V"].is_object() ? &raw2["e_V"] : nullptr;
    bool vOk = stripeOk && ev != nullptr && ev->contains("names") && (*ev)["names"].is_array() &&
               ev->contains("matrix") && (*ev)["matrix"].is_array() &&
               (*ev)["names"].size() == coefs.size() && (*ev)["matrix"].size() == coefs.size();
    const size_t k = vOk ? coefs.size() : 0;
    for (size_t j = 0; vOk && j < k; ++j) {
      const ojson& c = coefs[j];
      const ojson& row = (*ev)["matrix"][j];
      vOk = c.is_object() && c.contains("b") && c["b"].is_number() && row.is_array() &&
            row.size() == k;
      if (!vOk) break;
      b.push_back(c["b"].get<double>());
      for (size_t l = 0; l < k && vOk; ++l) {
        vOk = row[l].is_number() && (*ev)["matrix"][l].is_array() &&
              (*ev)["matrix"][l].size() == k && (*ev)["matrix"][l][j].is_number();
        if (vOk) V.push_back(0.5 * (row[l].get<double>() + (*ev)["matrix"][l][j].get<double>()));
      }
    }
    if (vOk) {
      eval::EvalMatrix mb, mv;
      mb.rownames = {"y1"};
      mb.colnames = stripe;
      mb.data.push_back(b);
      mv.rownames = stripe;
      mv.colnames = stripe;
      for (size_t j = 0; j < k; ++j)
        mv.data.emplace_back(V.begin() + static_cast<std::ptrdiff_t>(j * k),
                             V.begin() + static_cast<std::ptrdiff_t>((j + 1) * k));
      store.erase(std::remove_if(store.begin(), store.end(),
                                 [](const auto& p) { return p.first == "b" || p.first == "V"; }),
                  store.end());
      store.emplace_back("b", std::move(mb));
      store.emplace_back("V", std::move(mv));
      putScalar("rank", static_cast<double>(glmVceRank(V, k)));
    }
    // The per-column matrices Stata posts beside e(b) (vs-stata bank 85536cc0, 4525943d,
    // 0f5dfbc8, 7df7f233, 440bd7fb): e(_N) the rows (r1), mean's e(sd) (y1),
    // proportion's e(freq) (r1), e(_N_subp) the weight total under fw/iw (r1), e(error)
    // 0 for every column (r1, c1 c2 ...), and the one-stratum design's e(_N_strata) 1,
    // e(_N_strata_single) 0, e(_N_strata_certain) 0 (r1, c1). Under pw mean's e(V_srs).
    const auto rowOf = [&](const char* key, std::vector<double>& out) {
      out.clear();
      if (!raw2.contains(key)) return false;
      const ojson& a = raw2[key];
      if (a.is_number()) {
        out.assign(coefs.size(), a.get<double>());
        return true;
      }
      if (!a.is_array() || a.size() != coefs.size()) return false;
      for (const auto& x : a)
        out.push_back(x.is_number() ? x.get<double>() : std::numeric_limits<double>::quiet_NaN());
      return true;
    };
    // A one-column fit names e(_N)'s and e(_N_subp)'s row after its column (bank
    // f84271c3 `mean big`: e(_N) row `big`; 00b2b569 `mean price [fw=w]`: e(_N_subp)
    // row `price`); e(sd) keeps y1.
    const auto rowMatrix = [&](const std::string& rowName, std::vector<double> vals) {
      eval::EvalMatrix m;
      m.rownames = {rowName == "r1" && stripe.size() == 1 ? stripe[0] : rowName};
      m.colnames = stripe;
      m.data.push_back(std::move(vals));
      return m;
    };
    const auto cNames = [](size_t n) {
      std::vector<std::string> c;
      for (size_t i = 1; i <= n; ++i) c.push_back("c" + std::to_string(i));
      return c;
    };
    if (stripeOk) {
      std::vector<double> vals;
      if (rowOf("e__N", vals)) putMatrix("_N", rowMatrix("r1", vals));
      if (cobraCmd == "mean" && rowOf("e_sd", vals)) putMatrix("sd", rowMatrix("y1", vals));
      if (cobraCmd == "proportion" && rowOf("e_freq", vals)) {
        eval::EvalMatrix f = rowMatrix("r1", vals);
        f.rownames = {"r1"};
        putMatrix("freq", std::move(f));
      }
      if (rowOf("e__N_subp", vals)) putMatrix("_N_subp", rowMatrix("r1", vals));
      // e(error): 0, except a proportion of 0 (a level the group never takes) 1 and
      // a mean whose estimate and variance are both 0, 5 (bank 0b672af1, 63d9e791
      // `proportion ..., over()`; e5beada5 `mean v, over(foreign)` with v all 0). A mean
      // with a variance of 0 and a nonzero estimate has no evidence for its code:
      // e(error) is then not posted.
      std::vector<double> errs;
      bool errOk = true;
      for (const auto& c : coefs) {
        const double bj = c.is_object() && c.contains("b") && c["b"].is_number() ? c["b"].get<double>()
                                                                                 : std::nan("");
        const double sj = c.is_object() && c.contains("se") && c["se"].is_number() ? c["se"].get<double>()
                                                                                   : std::nan("");
        if (cobraCmd == "proportion") {
          errs.push_back(bj == 0.0 ? 1.0 : 0.0);
        } else if (sj == 0.0) {
          errOk = errOk && bj == 0.0;
          errs.push_back(5.0);
        } else {
          errs.push_back(0.0);
        }
      }
      if (errOk) {
        eval::EvalMatrix err;
        err.rownames = {"r1"};
        err.colnames = cNames(coefs.size());
        err.data.push_back(std::move(errs));
        putMatrix("error", std::move(err));
      }
      for (const auto& [nm, v] : std::vector<std::pair<const char*, double>>{
               {"_N_strata", 1.0}, {"_N_strata_single", 0.0}, {"_N_strata_certain", 0.0}}) {
        eval::EvalMatrix m;
        m.rownames = {"r1"};
        m.colnames = {"c1"};
        m.data.push_back({v});
        putMatrix(nm, std::move(m));
      }
      // stdize(): e(_N_stdsum) the rows and e(_p_stdize) the standard weights, strata x
      // over() groups (r1.. x c1..), e(N_stdize) the strata (vs-stata bank 56f2dacb)
      for (const auto& [key, nm] : std::vector<std::pair<const char*, const char*>>{
               {"e__N_stdsum", "_N_stdsum"}, {"e__p_stdize", "_p_stdize"}}) {
        if (!raw2.contains(key) || !raw2[key].is_array()) continue;
        eval::EvalMatrix m;
        bool ok = true;
        for (const auto& row : raw2[key]) {
          std::vector<double> r;
          ok = ok && row.is_array();
          for (size_t c = 0; ok && c < row.size(); ++c) {
            ok = row[c].is_number();
            if (ok) r.push_back(row[c].get<double>());
          }
          m.data.push_back(std::move(r));
        }
        if (!ok || m.data.empty()) continue;
        for (size_t i = 1; i <= m.data.size(); ++i) m.rownames.push_back("r" + std::to_string(i));
        m.colnames = cNames(m.data[0].size());
        putMatrix(nm, std::move(m));
      }
      if (raw2.contains("e_V_srs") && raw2["e_V_srs"].is_object()) {
        const ojson& vs = raw2["e_V_srs"];
        bool ok = vs.contains("matrix") && vs["matrix"].is_array() &&
                  vs["matrix"].size() == coefs.size();
        eval::EvalMatrix m;
        m.rownames = stripe;
        m.colnames = stripe;
        for (size_t i = 0; ok && i < coefs.size(); ++i) {
          const ojson& row = vs["matrix"][i];
          ok = row.is_array() && row.size() == coefs.size();
          std::vector<double> r;
          for (size_t j2 = 0; ok && j2 < coefs.size(); ++j2) {
            ok = row[j2].is_number();
            if (ok) r.push_back(row[j2].get<double>());
          }
          m.data.push_back(std::move(r));
        }
        if (ok) putMatrix("V_srs", std::move(m));
      }
    }
    putString("title", cobraCmd == "mean" ? "Mean estimation" : "Proportion estimation");
    putString("depvar", cobraCmd == "mean" ? "Mean" : "Proportion");
    putString("predict", "_no_predict");
    putString("properties", "b V");
    // the varlist the fit estimated; a proportion names each level as `#.var`
    std::string varlist;
    bool varlistOk = !coefs.empty();
    std::vector<std::string> seen;
    for (const auto& rn : replyNames) {
      const Col cc = split(rn);
      std::string item = cc.var;
      if (cobraCmd == "proportion") {
        varlistOk = varlistOk && !cc.lev.empty() &&
                    cc.lev.find_first_not_of("0123456789") == std::string::npos;
        item = cc.lev + "." + cc.var;
      }
      varlistOk = varlistOk && !cc.var.empty();
      if (std::find(seen.begin(), seen.end(), item) != seen.end()) continue;
      seen.push_back(item);
      varlist += (varlist.empty() ? "" : " ") + item;
    }
    if (varlistOk) putString("varlist", varlist);
    // e(marg_dims): proportion's variables, then over()'s, then `_term` when the
    // fit has more than one variable (`proportion rep78` "rep78", `proportion rep78
    // foreign` "rep78 foreign _term", `mean price mpg` "_term", `mean price, over(foreign)`
    // "foreign"); `mean x` posts none.
    {
      std::string dims;
      const auto add = [&dims](const std::string& d) { dims += (dims.empty() ? "" : " ") + d; };
      if (cobraCmd == "proportion")
        for (const auto& v : fitVars) add(v);
      if (!overVar.empty()) add(overVar);
      if (fitVars.size() > 1) add("_term");
      if (!dims.empty()) putString("marg_dims", dims);
    }
    if (!overVar.empty() && stripeOk) {
      std::string names;
      for (size_t i = 0; i < overLevels.size(); ++i)
        names += (i ? " " : "") + levelName(overLevels[i], i == 0, overLevels.size(), overVar);
      putString("over", overVar);
      putString("over_namelist", names);
      putString("OVER_NAMELIST", names);
    }
    putString("estat_cmd", "estat_vce_only");
    putString("marginsnotok", "_ALL");
    if (noLabel) putString("nolabel", "nolabel");
    if (!stdizeVar.empty()) {
      putString("stdize", stdizeVar);
      putString("stdweight", stdweightVar);
      if (raw2.contains("n_stdize") && raw2["n_stdize"].is_number())
        putScalar("N_stdize", raw2["n_stdize"].get<double>());
    }
    // vce(cluster): e(vce) cluster, e(vcetype) Robust, e(clustvar) (and e(cluster)),
    // e(N_clust) (vs-stata bank bcfd65d0 `mean price mpg, vce(cluster rep78)`).
    if (!clusterVar.empty()) {
      putString("vce", "cluster");
      putString("vcetype", "Robust");
      putString("clustvar", clusterVar);
      putString("cluster", clusterVar);
      if (raw2.contains("n_clusters") && raw2["n_clusters"].is_number())
        putScalar("N_clust", raw2["n_clusters"].get<double>());
    }
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
