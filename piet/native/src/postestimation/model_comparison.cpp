// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Model comparison.
#include "piet/handlers/estimation.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/display_hardening.h"  // exprUnboundNameRefusal (predict, BUG-1248)
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/settings.h"
#include "piet/storage_type.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "estimation/estimation_internal.h"
#include "estimators/postest_names.h"  // parse_level_ref (the engine's one resolver)
#include "estimators/posthest.h"  // margins_grid / margins_parse_at (header-inline)

#include "postestimation_internal.h"
#include "estimation/estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace postest_detail {}
using namespace postest_detail;
using namespace postestimation_vocab;

ojson runSdtest(const std::string& varName, const std::optional<std::string>& againstVal,
                const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                const std::optional<std::string>& ifExpr, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (varName.empty()) {
    return engineErrorBlockM4("syntax_error: `sdtest` requires a variable", "syntax_error");
  }

  std::optional<std::string> byVal;
  for (const auto& [k, v] : opts) {
    if (k == "by" && !v.isBool) byVal = v.strValue;
  }
  bool hasAgainst = againstVal.has_value() && !trimM4(*againstVal).empty();
  if (byVal.has_value() == hasAgainst) {
    return engineErrorBlockM4(
        "sdtest: specify exactly one of `by(groupvar)` or `= #` (two-sample vs one-sample form).",
        "syntax_error");
  }

  std::vector<std::string> argv = {"sdtest", "--var", varName};
  if (byVal.has_value()) {
    argv.push_back("--by");
    argv.push_back(*byVal);
  } else {
    argv.push_back("--against");
    argv.push_back(trimM4(*againstVal));
  }
  if (ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*ifExpr);
  }
  std::optional<std::string> levelVal;
  for (const auto& [k, v] : opts) {
    if (k == "level" && !v.isBool) levelVal = v.strValue;
  }
  if (levelVal.has_value() && !trimM4(*levelVal).empty()) {
    argv.push_back("--level");
    argv.push_back(trimM4(*levelVal));
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabSdtest.known;
  if (auto err = eval::rejectUnknownOptions(opts, kKnown)) {
    return *err;
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, "sdtest", &session);
  block["estimator"] = "sdtest";
  static const char* kSdtestFields[] = {
      "F", "df1", "df2", "p_lower", "p_upper", "mode", "sd", "sd_1", "sd_2",
      "groups", "combined", "level", "chi2", "sd0", "se", "ci_lo", "ci_hi", "var",
  };
  for (const char* k : kSdtestFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  return block;
}

// hausman (BS-083): `hausman NAME1 NAME2` — the classic Stata idiom
// `xtreg,fe` + `estimates store A` + `xtreg,re` + `estimates store B` +
// `hausman A B`. Validates both eststo/estimates-store names exist AND are
// the fe/re pair of the SAME spec (same depvar, same regressor SET; one fe
// one re), then calls cobra's OWN `hausman --y --x --ivar` directly (the
// engine recomputes BOTH sides itself from raw data — it does NOT consume
// the two stored coefficient tables at all, unlike real Stata's own
// hausman, which diffs two ALREADY-FITTED e(b)/e(V)). Oracle D5e: chi2=2.62,
// p=0.2697, per-coef b_fe/b_re/diff (beertax diff=376.6603, unemp
// diff=-.6916189) — byte-for-byte engine match.
//
// DISCLOSED LIMITATION: eststo's own snapshot (results/stored_estimates_snapshot.cpp's
// makeSnapshot) captures only {coeftable, n_obs, r2, depvar, estimator, fe}
// — no x-regressor list, dataPath, or ivar field of its own. x is
// reconstructed from the stored coeftable's own row names (dropping
// "_cons") — lossless for THIS purpose (set membership, not display
// order). depvar is a SECOND, independent gap: live-verified `cobra xtreg
// --ivar ...` (the RE path)'s own raw JSON carries NO "depvar" field at all
// (unlike `cobra feols --fe ...`, which does), so the RE snapshot's depvar
// is ALWAYS empty — the depvar cross-check below only fires when the RE
// side actually has one (never today), leaning on the regressor-SET check
// as the real "same specification" guard in the meantime. The FE store is
// identified by estimator=="feols" AND a non-null
// single-key "fe" (absorbed) object (xtreg,fe routes through
// cobraToEstimationM4(raw,"feols") with --fe=panelvar, which populates
// raw["absorbed"]={panelvar:k}; a PLAIN `feols`/`regress` fit has no
// absorb() and so no "fe" object at all here — this is a real, non-spurious
// signal). The RE store is identified by estimator=="xtreg_re" (cobra's own
// raw "estimator" string for the --ivar RE path, live-verified — NOT
// "xtreg", which is only the block's display default). Neither eststo's
// snapshot NOR runXtreg's own cacheLastModelM4 records the RE fit's ivar
// anywhere at all — this handler uses the CURRENT session.panelvar (the
// active `xtset` context) as the shared ivar for BOTH sides, and requires
// it to equal the FE store's own absorbed-variable name (its one source of
// ground truth), i.e. hausman assumes the user has not re-`xtset`'d to a
// different panel variable since fitting the RE side. Similarly there is no
// stored dataPath in the snapshot either — this uses session.currentHbin()
// (the CURRENTLY loaded dataset), i.e. hausman also assumes the same
// dataset is still loaded. Both are disclosed, reasonable-workflow
// assumptions (matching the D5e oracle's own single-tsset, single-dataset
// session), not silent wrong answers.
ojson runHausman(const std::string& name1, const std::string& name2,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                  SessionState& session) {
  // BS-097: real Stata `hausman` takes constant/sigmamore/sigmaless/force/
  // alleqs/skipeqs()/equations()/df()/tconsistent()/tefficient() ([R]
  // hausman, r.pdf p.1091-1092) — none implemented by this handler, it
  // always recomputes both sides from raw data via cobra's own --tidy
  // hausman, which has no such knobs — every one of them now reads as an
  // honest named reject instead of a silent no-op.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kHausmanKnownOpts = kVocabHausman.known;
  // BUG-105 secondary ⑤: the options above are documented Stata syntax, not
  // typos, so they get the "not yet implemented" wording; anything else
  // (a genuine bogus token) keeps the plain "not allowed" reject.
  const std::unordered_set<std::string>& kHausmanKnownButUnimplemented = kVocabHausman.unimplemented;
  if (auto err =
          eval::rejectUnknownOptions(opts, kHausmanKnownOpts, kHausmanKnownButUnimplemented)) {
    return *err;
  }
  const ojson* s1 = session.estimatesGet(name1);
  const ojson* s2 = session.estimatesGet(name2);
  if (s1 == nullptr) {
    return engineErrorBlockM4(
        "hausman: no stored estimation result named `" + name1 + "`. Run `eststo " + name1 +
        "` (or `estimates store " + name1 + "`) after fitting the model.",
        "unsupported_option");
  }
  if (s2 == nullptr) {
    return engineErrorBlockM4(
        "hausman: no stored estimation result named `" + name2 + "`. Run `eststo " + name2 +
        "` (or `estimates store " + name2 + "`) after fitting the model.",
        "unsupported_option");
  }

  auto xset = [](const ojson& snap) {
    std::set<std::string> xs;
    if (snap.contains("coeftable") && snap["coeftable"].is_array()) {
      for (const auto& row : snap["coeftable"]) {
        std::string nm = row.value("name", std::string());
        if (nm != "_cons" && !nm.empty()) xs.insert(nm);
      }
    }
    return xs;
  };
  auto isFeSnap = [](const ojson& snap) {
    std::string estimator = snap.value("estimator", std::string());
    return (estimator == "feols" || estimator == "reghdfe") && snap.contains("fe") &&
           snap["fe"].is_object() && snap["fe"].size() == 1;
  };
  auto isReSnap = [](const ojson& snap) {
    return snap.value("estimator", std::string()) == "xtreg_re";
  };

  const ojson *feSnap = nullptr, *reSnap = nullptr;
  std::string feName, reName;
  if (isFeSnap(*s1) && isReSnap(*s2)) {
    feSnap = s1;
    reSnap = s2;
    feName = name1;
    reName = name2;
  } else if (isFeSnap(*s2) && isReSnap(*s1)) {
    feSnap = s2;
    reSnap = s1;
    feName = name2;
    reName = name1;
  } else {
    return engineErrorBlockM4(
        "hausman: `" + name1 + "` and `" + name2 + "` must be one `xtreg ,fe` result and one "
        "`xtreg ,re` result of the SAME specification (e.g. `xtreg y x, fe` / `eststo A` / "
        "`xtreg y x, re` / `eststo B` / `hausman A B`) — got estimator=`" +
        s1->value("estimator", std::string()) + "` and estimator=`" +
        s2->value("estimator", std::string()) + "`.",
        "unsupported_option");
  }

  // Depvar cross-check is BEST-EFFORT only: live-verified the engine's own
  // `cobra xtreg --ivar ...` (RE) raw JSON has NO "depvar" field at all
  // (unlike `cobra feols --fe ...`, which does) — cobraToEstimationM4
  // therefore always stores an EMPTY depvar for the RE side, and eststo's
  // snapshot inherits that emptiness. Comparing an always-empty RE depvar
  // against the FE side's real one would reject EVERY legitimate fe/re
  // pair, so this only runs the check when the RE snapshot actually HAS a
  // non-empty depvar (inert today, activates automatically if a future
  // engine build starts populating it) — the regressor-SET check right
  // below is the real safety net for "same specification" in the meantime.
  std::string reDepvar = reSnap->value("depvar", std::string());
  if (!reDepvar.empty() && feSnap->value("depvar", std::string()) != reDepvar) {
    return engineErrorBlockM4(
        "hausman: `" + feName + "` and `" + reName + "` have different dependent variables (`" +
        feSnap->value("depvar", std::string()) + "` vs `" + reDepvar +
        "`) — hausman requires the fe/re pair of the SAME specification.",
        "unsupported_option");
  }
  std::set<std::string> xsFe = xset(*feSnap), xsRe = xset(*reSnap);
  if (xsFe != xsRe) {
    return engineErrorBlockM4(
        "hausman: `" + feName + "` and `" + reName + "` fit different regressor lists — hausman "
        "requires the fe/re pair of the SAME specification.",
        "unsupported_option");
  }

  std::string ivar;
  for (const auto& [k, v] : (*feSnap)["fe"].items()) {
    (void)v;
    ivar = k;  // single-key object, validated by isFeSnap() above.
  }
  if (!session.panelvar.has_value() || session.panelvar->empty() || *session.panelvar != ivar) {
    return engineErrorBlockM4(
        "hausman: run `xtset " + ivar + " ...` (the SAME panel variable `" + feName +
        "` was fit with) before calling `hausman` — the current xtset context (" +
        session.panelvar.value_or(std::string("<none>")) + ") does not match.",
        "unsupported_option");
  }
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  std::vector<std::string> xs(xsFe.begin(), xsFe.end());
  std::vector<std::string> argv = {"hausman",       "--y",     feSnap->value("depvar", std::string()),
                                    "--x",           joinCommaM4(xs), "--ivar", ivar,
                                    "--data",        *session.currentHbin(), "--tidy"};
  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block;
  block["type"] = "estimation";
  block["estimator"] = "hausman";
  block["depvar"] = feSnap->value("depvar", std::string());
  block["n_obs"] = raw.value("n_obs", 0);
  static const char* kHausmanFields[] = {"chi2", "p", "dof", "coefs", "not_pd"};
  for (const char* k : kHausmanFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // BUG-1940: Stata's two notes under the statistic, in its words.
  std::vector<std::string> notes;
  if (raw.value("not_pd", false)) notes.push_back("(V_b-V_B is not positive definite)");
  if (raw.contains("chi2") && raw["chi2"].is_number() && raw["chi2"].get<double>() < 0) {
    notes.push_back("chi2<0 ==> model fitted on these data fails to meet the asymptotic "
                    "assumptions of the Hausman test; see suest for a generalized test");
  }
  if (!notes.empty()) block["notes"] = notes;
  return block;
}

// chow (BS-084): `chow depvar indepvars [if], by(groupvar)` — a PIET-ONLY
// convenience wrapper; real Stata has NO `chow` command at all (the
// oracle's own D5f section computes the classic Chow F test by hand: fit
// pooled + both subsamples, chowF = ((rss_pooled - rss_0 - rss_1)/k) /
// ((rss_0+rss_1)/(n-2k)) — see d45-oracle-stata.do's own comment "Chow test
// income~beertax split by high: pooled + subsamples"). cobra's own `chow`
// CLI computes the IDENTICAL classic homoskedastic SSR-based F test as its
// "classic" block (live-verified: engine classic.F=28.07417107825576,
// df1=2, df2=332 — byte-for-byte the oracle's own manually-computed
// chowF=28.074171, df1=2, df2=332) — plus a bonus heteroskedasticity-robust
// Wald variant (raw.robust, no oracle coverage, not selftest-pinned) this
// handler also surfaces. vce()/cluster() are NOT forwarded this batch (no
// oracle coverage for cobra chow's own --vcov/--cluster surface) — left as
// honest rejects via the unknown-option net.
ojson runChow(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: `chow` requires a dependent variable and at least one regressor",
        "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::optional<std::string> byVal;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "by" && !v.isBool) byVal = v.strValue;
  }
  if (!byVal.has_value() || trimM4(*byVal).empty()) {
    return engineErrorBlockM4("syntax_error: `chow` requires by(groupvar)", "syntax_error");
  }

  std::vector<std::string> argv = {"chow", "--y", y, "--x", joinCommaM4(xs), "--by",
                                    trimM4(*byVal)};
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabChow.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block;
  block["type"] = "estimation";
  block["estimator"] = "chow";
  block["depvar"] = y;
  block["n_obs"] = raw.value("n_obs", 0);
  static const char* kChowFields[] = {"G", "by", "group_sizes", "classic", "robust"};
  for (const char* k : kChowFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  block["note"] =
      "Chow test (real Stata has no built-in `chow` command — this is a PIET convenience wrapper "
      "reproducing the classic manual F-test: pooled vs split-sample SSR).";
  return block;
}

// mkspline (BS-087, part): `mkspline stub = varname [if], cubic [nknots(#) |
// knots(v1,v2,...)]` -> cobra eval --mkspline "stub=var, cubic[, nknots(N)|,
// knots(...)]" --out new.hbin (a data-mutation verb, like egen — adds
// stub1..stubK-1 columns). Oracle D5g: nknots(4) gives 3 columns
// (inc_sp1/2/3, pinned means/sds) — see the fix disclosure below;
// only the CUBIC form is implemented. Real Stata's LINEAR/piecewise-linear
// default (mkspline with NO `cubic` keyword) has NO engine equivalent —
// live-verified the engine's `eval --mkspline` op ALWAYS computes a
// restricted cubic spline basis regardless of whether `cubic` was typed in
// the spec string (omitting it does NOT fall back to a linear basis, it
// silently produces the SAME cubic one). Rather than silently answer
// linear-mkspline with a cubic result, PIET requires `cubic` to be typed
// and has no linear mkspline of its own.
//
// ENGINE FIX (cobra 0.25.0): `eval --mkspline`'s own `nknots(#)` clause
// now works. LOWERCASE `nknots(N)` — the engine's own documented spelling,
// per `cobra eval`'s usage banner — places knots at Harrell's percentile
// method (e.g. nknots(4) -> the 5/35/65/95 percentiles of the source
// variable) and returns the correct N-1 basis columns, verified live
// against StataBE (byte-for-byte knot positions and basis-column summary
// stats). The old failure mode was a paren_arg substring collision at the
// CLI arg-parsing layer, present through 0.24.0: LOWERCASE `nknots(N)`
// hard-errored "mkspline: need at least 3 knots" for EVERY N tried from 1
// to 10 (the value never actually parsed), while UPPERCASE `NKNOTS(N)` was
// silently ACCEPTED but the value N was DISCARDED — it always fell back to
// the same hardcoded 5-knot/4-column default regardless of N. PIET forwards
// `nknots(N)` in LOWERCASE, per the engine's own documented spelling — the
// D5g oracle's own exact invocation (`nknots(4)`) now succeeds cleanly and
// is oracle-pinned in D5g's own selftest leg (selftest.cpp). The explicit
// `knots(v1,v2,...)` form (caller-supplied exact knot positions, bypassing
// nknots() entirely) also works mechanically (live-verified) and is
// independently wired/checked in its own selftest leg (a fresh stub name,
// since it now runs alongside the fixed nknots() leg rather than as its
// sole proof of mechanical wiring).

}  // namespace piet::handlers
