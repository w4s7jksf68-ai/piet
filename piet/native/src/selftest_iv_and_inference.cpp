// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: iv and inference.
#include "selftest_support.h"
#include "selftest_cases.h"


void testD1BS063IvRegress() {
  std::printf("[selftest] --- D1/BS-063 ivregress 2sls/gmm dispatch (IV & systems wirings) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d1_bs063").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);

  // D1a — just-identified: `ivregress 2sls income (beertax = spirits)`.
  // Oracle: beertax b=-23827.7. Coefficients match Stata byte-for-byte
  // (verified live against this exact engine build) — the ARGV mapping is
  // correct. SE convention: cobra's `iv --vcov iid` default is now
  // Stata-asymptotic (N denominator), matching Stata's ivregress default
  // (no `small`) exactly — fixed in cobra 0.25.0 (was N-K small-sample
  // through 0.24.x; `--small` still restores the old N-K convention, see
  // f-oracle-pins.md's ivregress `small` cross-check). This leg now pins
  // the Stata `ivregress` oracle value itself.
  {
    auto blocks = piet::dispatch("ivregress 2sls income (beertax = spirits)", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D1a: `ivregress 2sls income (beertax = spirits)` returns an estimation block");
    if (est != nullptr) {
      check(est->value("estimator", std::string()) == "ivregress 2sls",
            "D1a: estimator label is \"ivregress 2sls\", got \"" +
                est->value("estimator", std::string()) + "\"");
      auto b = coefFieldD1(*est, "beertax", "b");
      check(b.has_value() && std::abs(*b - (-23827.7)) < 1.0,
            "D1a oracle: beertax b ~= -23827.7, got " + (b.has_value() ? std::to_string(*b) : "MISSING"));
      auto se = coefFieldD1(*est, "beertax", "se");
      check(se.has_value() && std::abs(*se - 13643.678807996115) < 1e-3,
            "D1a oracle: beertax se ~= 13643.678807996115 (Stata ivregress asymptotic default, "
            "cobra 0.25.0+), got " + (se.has_value() ? std::to_string(*se) : "MISSING"));
    }
  }

  // D1b — over-identified + exog control: `ivregress 2sls income emppop
  // (beertax = spirits unemp)`. Oracle: beertax b=8230.191, emppop
  // b=384.0945. Endogenous-first coeftable ordering (Stata's own display
  // convention, oracle-verified: beertax listed before emppop even though
  // emppop was typed BEFORE the paren block).
  {
    auto blocks =
        piet::dispatch("ivregress 2sls income emppop (beertax = spirits unemp)", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D1b: over-identified ivregress 2sls returns an estimation block");
    if (est != nullptr) {
      auto bBeertax = coefFieldD1(*est, "beertax", "b");
      auto bEmppop = coefFieldD1(*est, "emppop", "b");
      check(bBeertax.has_value() && std::abs(*bBeertax - 8230.191) < 0.5,
            "D1b oracle: beertax b ~= 8230.191, got " +
                (bBeertax.has_value() ? std::to_string(*bBeertax) : "MISSING"));
      check(bEmppop.has_value() && std::abs(*bEmppop - 384.0945) < 0.5,
            "D1b oracle: emppop b ~= 384.0945, got " +
                (bEmppop.has_value() ? std::to_string(*bEmppop) : "MISSING"));
      // Stata's own display order lists the ENDOGENOUS regressor (beertax)
      // before the exogenous one (emppop), even though emppop was typed
      // FIRST in the command (cobra's own coeftable puts "(intercept)"
      // first regardless — this checks beertax-before-emppop among the
      // two REGRESSOR rows, not absolute index 0).
      if (est->contains("coeftable") && (*est)["coeftable"].is_array()) {
        int idxBeertax = -1, idxEmppop = -1, idx = 0;
        for (const auto& row : (*est)["coeftable"]) {
          std::string nm = row.value("name", std::string());
          if (nm == "beertax") idxBeertax = idx;
          if (nm == "emppop") idxEmppop = idx;
          ++idx;
        }
        check(idxBeertax >= 0 && idxEmppop >= 0 && idxBeertax < idxEmppop,
              "D1b: coeftable lists the ENDOGENOUS regressor (beertax, idx=" +
                  std::to_string(idxBeertax) + ") before the exogenous one (emppop, idx=" +
                  std::to_string(idxEmppop) + "), matching Stata's own display order");
      }
    }
  }

  // D1c — robust: `ivregress 2sls income emppop (beertax = spirits
  // unemp), vce(robust)`. Oracle coef unchanged from D1b (8230.191). SE is
  // NOT pinned here: unlike D1a's default vcov (a genuine Stata-oracle
  // pin as of cobra 0.25.0, see the comment there — this was stale, it
  // used to describe D1a as an unresolved "engine-value pin" before the
  // G-rebase fixed that convention gap), this leg's HC1-robust SE has no
  // Stata oracle cross-check yet, so only vcov_type is asserted below.
  {
    auto blocks = piet::dispatch(
        "ivregress 2sls income emppop (beertax = spirits unemp), vce(robust)", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D1c: `vce(robust)` ivregress 2sls returns an estimation block");
    if (est != nullptr) {
      auto b = coefFieldD1(*est, "beertax", "b");
      check(b.has_value() && std::abs(*b - 8230.191) < 0.5,
            "D1c oracle: beertax b unchanged by vce(robust), ~= 8230.191, got " +
                (b.has_value() ? std::to_string(*b) : "MISSING"));
      // cobra iv's own vcov_type spelling for hc1 is "HC1" (uppercase, live-
      // verified) — not the lowercase "hc1" the --vcov ARGV value uses.
      check(est->value("vcov_type", std::string()) == "HC1",
            "D1c: vce(robust) forwards --vcov hc1, got vcov_type=\"" +
                est->value("vcov_type", std::string()) + "\"");
    }
  }

  // D1d — cluster: `..., vce(cluster state)`. Oracle coef unchanged
  // (8230.191); "Std. err. adjusted for 48 clusters in state".
  {
    auto blocks = piet::dispatch(
        "ivregress 2sls income emppop (beertax = spirits unemp), vce(cluster state)", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D1d: `vce(cluster state)` ivregress 2sls returns an estimation block");
    if (est != nullptr) {
      auto b = coefFieldD1(*est, "beertax", "b");
      check(b.has_value() && std::abs(*b - 8230.191) < 0.5,
            "D1d oracle: beertax b unchanged by vce(cluster state), ~= 8230.191, got " +
                (b.has_value() ? std::to_string(*b) : "MISSING"));
      check(est->value("n_clusters", 0) == 48,
            "D1d oracle: 48 clusters in state, got n_clusters=" +
                std::to_string(est->value("n_clusters", 0)));
    }
  }

  // D1e — gmm: `ivregress gmm income emppop (beertax = spirits unemp)`.
  // Oracle: beertax b=4675.645, se=3034.074 (over-identified GMM genuinely
  // DIFFERS from 2SLS — not a re-display of D1b/c/d). Unlike D1a-d, gmm's
  // SE matches the oracle to full display precision (no engine convention
  // gap here) — a real, tight oracle pin.
  {
    auto blocks =
        piet::dispatch("ivregress gmm income emppop (beertax = spirits unemp)", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D1e: `ivregress gmm` returns an estimation block");
    if (est != nullptr) {
      check(est->value("estimator", std::string()) == "ivregress gmm",
            "D1e: estimator label is \"ivregress gmm\", got \"" +
                est->value("estimator", std::string()) + "\"");
      auto b = coefFieldD1(*est, "beertax", "b");
      auto se = coefFieldD1(*est, "beertax", "se");
      check(b.has_value() && std::abs(*b - 4675.645) < 0.5,
            "D1e oracle: beertax b ~= 4675.645 (differs from 2SLS's 8230.191 — genuine "
            "over-identified GMM), got " + (b.has_value() ? std::to_string(*b) : "MISSING"));
      check(se.has_value() && std::abs(*se - 3034.074) < 0.5,
            "D1e oracle: beertax se ~= 3034.074 (tight match, no engine convention gap for "
            "gmm), got " + (se.has_value() ? std::to_string(*se) : "MISSING"));
    }
  }

  // Malformed (endog = instruments) block -> honest r(198) syntax reject,
  // not a crash/mis-parse (BS-063's "parse the paren block robustly"
  // requirement's negative-space check).
  {
    (void)piet::dispatch("capture ivregress 2sls income (beertax spirits)", session);  // no '='
    check(session.eval.locals.get("_rc", "") == "198",
          "D1/BS-063: `ivregress 2sls income (beertax spirits)` (missing '=') rejects with rc=198");
    (void)piet::dispatch("capture ivregress 2sls income (= spirits)", session);  // no endog
    check(session.eval.locals.get("_rc", "") == "198",
          "D1/BS-063: `ivregress 2sls income (= spirits)` (missing endog) rejects with rc=198");
    (void)piet::dispatch("capture ivregress 2sls income (beertax =)", session);  // no instruments
    check(session.eval.locals.get("_rc", "") == "198",
          "D1/BS-063: `ivregress 2sls income (beertax =)` (missing instruments) rejects with "
          "rc=198");
  }

  // Unknown estimator word -> honest r(198), not a silent fall-through.
  {
    (void)piet::dispatch("capture ivregress bogus income (beertax = spirits)", session);
    check(session.eval.locals.get("_rc", "") == "198",
          "D1/BS-063: `ivregress bogus ...` (unrecognized estimator) rejects with rc=198");
  }

  // noconstant/level -> honest reject, NEVER silently forwarded (BS-063's
  // own report: cobra iv hard-errors "unrecognized option" on both, but
  // ALSO already prints a full valid-looking stdout JSON before that error
  // — piet::runCobraJson's BUG-039 fix would otherwise mask that error as
  // a false success, silently keeping the intercept / ignoring the
  // requested CI level. Forwarding either flag risks exactly that.
  {
    // noconstant is served since the engine's iv took --no-intercept
    // (w2d_uncovered.log ivregress_nocons_ctl rc 0): the fit has no constant.
    auto ncBlocks = piet::dispatch("ivregress 2sls income (beertax = spirits), noconstant", session);
    const auto* ncEst = firstBlockOfTypeD1(ncBlocks, "estimation");
    bool ncHasCons = false;
    if (ncEst != nullptr && ncEst->contains("coeftable") && (*ncEst)["coeftable"].is_array())
      for (const auto& r : (*ncEst)["coeftable"]) {
        const std::string nm = r.value("name", std::string());
        if (nm == "(intercept)" || nm == "_cons") ncHasCons = true;
      }
    check(ncEst != nullptr && !ncHasCons,
          "D1/BS-063: `ivregress 2sls ..., noconstant` fits without a constant (oracle "
          "w2d_uncovered ivregress_nocons_ctl)");
    (void)piet::dispatch("capture ivregress 2sls income (beertax = spirits), level(90)", session);
    check(session.eval.locals.get("_rc", "") == "198",
          "D1/BS-063: `ivregress 2sls ..., level(90)` honestly rejects (never silently "
          "forwarded to the engine)");
  }

  // `reghdfe y (endog=instr), absorb(F)`: reghdfe 6.x has no IV syntax and
  // stops with "parentheses unbalanced" r(132) (w2_esample.log:967-975
  // D6_reghdfe_iv_aw_cluster); the IV fit is `ivreghdfe` (below).
  {
    (void)piet::dispatch("capture reghdfe income (beertax = spirits), absorb(state)", session);
    check(session.eval.locals.get("_rc", "") == "132",
          "D1/BS-063: `reghdfe y (d=z), absorb(F)` is r(132) \"parentheses unbalanced\" "
          "(w2_esample.log:967-975), got _rc=" + session.eval.locals.get("_rc", ""));
  }

  // Plain `regress`/`feols` with an inline paren block still honestly
  // rejects (old pre-ivregress Stata syntax) — regression guard that this
  // batch's reghdfe-routing change did NOT also open up regress/feols.
  {
    (void)piet::dispatch("capture regress income (beertax = spirits)", session);
    check(session.eval.locals.get("_rc", "") == "198",
          "D1/BS-063 regression guard: plain `regress y (d=z)` still honestly rejects "
          "(re-pointed at `ivregress`, not silently accepted)");
  }

  // `ivreghdfe` (the literal command word) — same handler, allowAbsorb path.
  {
    auto blocks = piet::dispatch("ivreghdfe income (beertax = spirits), absorb(state)", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D1/BS-063: `ivreghdfe y (d=z), absorb(F)` returns an estimation block");
    if (est != nullptr) {
      check(est->value("estimator", std::string()) == "ivreghdfe",
            "D1/BS-063: `ivreghdfe`'s own estimator label is \"ivreghdfe\"");
    }
  }

  // ivreghdfe's default finite-sample convention maps to cobra iv --small.
  // The pre-fix SE was 10003.01652769641; the exact missing factor was
  // sqrt(48/47 * 335/333), yielding the Stata oracle below.
  {
    auto blocks = piet::dispatch(
        "ivreghdfe income emppop (beertax = spirits unemp), absorb(state) cluster(state)",
        session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D1/BS-063: clustered ivreghdfe small-sample probe succeeds");
    if (est != nullptr) {
      auto se = coefFieldD1(*est, "beertax", "se");
      check(se.has_value() && std::abs(*se - 10139.183029120097) < 1e-6,
            "D1/BS-063: ivreghdfe beertax SE uses cobra --small and matches Stata "
            "10139.183029120097, got " +
                (se.has_value() ? std::to_string(*se) : "MISSING"));
    }
  }

  // BUG-474 (User ruling 2026-09-09: match Stata; the mirror of BUG-473).
  // `ivregress` IS a real Stata command with its OWN absorb() option --
  // `which ivregress` resolves to StataNow's shipped base/i/ivregress.ado
  // (tests/eval/golden/aer_b4/b5_which.log), so this is Stata's contract,
  // not a third-party rule. Measured
  // tests/eval/golden/aer_b4/b5_absorb_class.log:38-62,
  // `B5REC|ivregress|absorb|0|`: rc 0, estimates. The stale reject this
  // leg used to pin is now the over-rejection defect BUG-474 filed.
  {
    auto blocks = piet::dispatch(
        "ivregress 2sls income (beertax = spirits), absorb(state)", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr,
          "BUG-474: `ivregress 2sls ..., absorb(state)` now accepts absorb() (Stata's own "
          "ivregress.ado has the option) and returns an estimation block");
    if (est != nullptr) {
      auto b = coefFieldD1(*est, "beertax", "b");
      check(b.has_value() && std::isfinite(*b),
            "BUG-474: ...with a finite `beertax` coefficient -- got " +
                (b.has_value() ? std::to_string(*b) : std::string("MISSING")));
      // BUG-474 landed by flipping ONE bit that meant two things: `allowAbsorb` also
      // switched cobra's `--small`. So accepting absorb() here silently moved ivregress
      // off Stata's ASYMPTOTIC default (D1a/D1f went RED at 13643.68 -> 13684.47). The
      // parameter is split now; this arm pins the CONSEQUENCE, not the plumbing.
      //
      // Measured (StataBE 19.5, tests/eval/golden/aer_b4/b5_ivregress_absorb_se.log,
      // `sysuse auto`, g = rep78): `ivregress 2sls price (mpg = weight), absorb(g)` gives
      //   B5VAL|b_mpg|-1.bb71cfd7f6b89X+008|se_mpg|+1.485f88522804aX+006  (= se 82.09329346)
      //   B5VAL|N|69|df_r|.        <- df_r MISSING: asymptotic, not small-sample
      // and the same fit WITH `small` gives
      //   B5VAL|se_mpg_small|+1.57a78870c2ba8X+006                        (= se 85.91360642)
      // This build answers 82.0932935 on that fixture, i.e. Stata's asymptotic value.
      //
      // The invariant asserted here needs no extra fixture: `ivreghdfe` reports the
      // small-sample correction BY CONVENTION and `ivregress` does not, so on the same
      // specification their standard errors must NOT coincide. Re-merging the two flags
      // makes them coincide, and that is exactly what this fires on.
      auto seIvregress = coefFieldD1(*est, "beertax", "se");
      auto hdfeBlocks = piet::dispatch(
          "ivreghdfe income (beertax = spirits), absorb(state)", session);
      const auto* hdfeEst = firstBlockOfTypeD1(hdfeBlocks, "estimation");
      auto seIvreghdfe =
          hdfeEst != nullptr ? coefFieldD1(*hdfeEst, "beertax", "se") : std::nullopt;
      check(seIvregress.has_value() && seIvreghdfe.has_value() &&
                std::isfinite(*seIvregress) && std::isfinite(*seIvreghdfe) &&
                std::abs(*seIvregress - *seIvreghdfe) >
                    1e-9 * std::max(std::abs(*seIvregress), std::abs(*seIvreghdfe)),
            "BUG-474: `ivregress ..., absorb()` keeps Stata's ASYMPTOTIC SE convention, so it "
            "must NOT equal `ivreghdfe`'s small-sample SE on the same specification -- got "
            "ivregress " +
                (seIvregress.has_value() ? std::to_string(*seIvregress) : std::string("MISSING")) +
                " vs ivreghdfe " +
                (seIvreghdfe.has_value() ? std::to_string(*seIvreghdfe) : std::string("MISSING")));
    }
  }

  fs::remove_all(workdir, ec);
}


void testD1BS064Liml() {
  std::printf("[selftest] --- D1/BS-064 ivregress liml -> cobra liml ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d1_bs064").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);

  // D1f — `ivregress liml income emppop (beertax = spirits unemp)`.
  // Oracle: beertax b=22287.68, se=16577.55; emppop b=614.4468,
  // se=301.7193. Tight oracle pin on BOTH b and se (liml has no engine
  // convention gap — matches Stata byte-for-byte, unlike D1a-d's iid/hc1/
  // cluster classical-VCE mismatch).
  auto blocks = piet::dispatch("ivregress liml income emppop (beertax = spirits unemp)", session);
  const auto* est = firstBlockOfTypeD1(blocks, "estimation");
  check(est != nullptr, "D1f: `ivregress liml` returns an estimation block");
  if (est != nullptr) {
    check(est->value("estimator", std::string()) == "ivregress liml",
          "D1f: estimator label is \"ivregress liml\", got \"" +
              est->value("estimator", std::string()) + "\"");
    auto bBeertax = coefFieldD1(*est, "beertax", "b");
    auto seBeertax = coefFieldD1(*est, "beertax", "se");
    auto bEmppop = coefFieldD1(*est, "emppop", "b");
    auto seEmppop = coefFieldD1(*est, "emppop", "se");
    check(bBeertax.has_value() && std::abs(*bBeertax - 22287.68) < 0.5,
          "D1f oracle: beertax b ~= 22287.68, got " +
              (bBeertax.has_value() ? std::to_string(*bBeertax) : "MISSING"));
    check(seBeertax.has_value() && std::abs(*seBeertax - 16577.55) < 0.5,
          "D1f oracle: beertax se ~= 16577.55 (tight match, liml has no engine convention "
          "gap), got " + (seBeertax.has_value() ? std::to_string(*seBeertax) : "MISSING"));
    check(bEmppop.has_value() && std::abs(*bEmppop - 614.4468) < 0.5,
          "D1f oracle: emppop b ~= 614.4468, got " +
              (bEmppop.has_value() ? std::to_string(*bEmppop) : "MISSING"));
    check(seEmppop.has_value() && std::abs(*seEmppop - 301.7193) < 0.5,
          "D1f oracle: emppop se ~= 301.7193, got " +
              (seEmppop.has_value() ? std::to_string(*seEmppop) : "MISSING"));
  }

  // vce(robust) on `ivregress liml`: the engine's liml now computes the robust
  // sandwich of Stata's ivregress liml (it used to ignore --vcov, so this
  // was refused). Its oracle value is pinned in testW2bIvFamily (Stata
  // .0809669411165636 on the w2t fixture); here: served, and not the classical SE.
  {
    auto rb = piet::dispatch(
        "ivregress liml income emppop (beertax = spirits unemp), vce(robust)", session);
    const auto* rest = firstBlockOfTypeD1(rb, "estimation");
    auto seRobust = rest != nullptr ? coefFieldD1(*rest, "beertax", "se") : std::nullopt;
    check(seRobust.has_value() && std::isfinite(*seRobust) && *seRobust > 0 &&
              std::abs(*seRobust - 16577.55) > 1.0,
          "D1/BS-064: `ivregress liml ..., vce(robust)` is served with a robust SE (not the "
          "classical 16577.55), got " +
              (seRobust.has_value() ? std::to_string(*seRobust) : std::string("MISSING")));
  }

  // keepsingletons on liml -> honest reject (no FE machinery at all).
  (void)piet::dispatch(
      "capture ivregress liml income emppop (beertax = spirits unemp), keepsingletons", session);
  check(session.eval.locals.get("_rc", "") == "198",
        "D1/BS-064: `ivregress liml ..., keepsingletons` honestly rejects (liml has no FE/"
        "singleton concept)");

  fs::remove_all(workdir, ec);
}


void testD1BS065Reg3() {
  std::printf("[selftest] --- D1/BS-065 reg3 (3SLS/SURE multi-equation systems) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d1_bs065").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);

  // D1g — 3SLS default: `reg3 (income beertax spirits) (emppop unemp
  // income)`. Oracle: beertax b=-1564.062, se=198.693; unemp
  // b=-1.378884, se=.0964493. Stata's OWN default is 3SLS — but cobra's
  // reg3 CLI default is the OPPOSITE (SURE unless --method 3sls) — the
  // handler must ALWAYS send --method explicitly (verified live: cobra's
  // no-flag default byte-matches D1h/sure, NOT D1g/3sls).
  {
    auto blocks =
        piet::dispatch("reg3 (income beertax spirits) (emppop unemp income)", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D1g: `reg3 (...) (...)` (no options -> Stata default 3SLS) returns "
                          "an estimation block");
    if (est != nullptr) {
      check(est->value("method", std::string()) == "3sls",
            "D1g: no-option reg3 forwards --method 3sls (Stata's own default), got method=\"" +
                est->value("method", std::string()) + "\"");
      check(est->contains("outcomes") && est->at("outcomes").is_array() &&
                est->at("outcomes").size() == 2,
            "D1g: block has a 2-equation \"outcomes\" array (reused from cobra's own "
            "\"equations\" key, same shape the frontend's multi-outcome renderer already "
            "expects)");
      if (est->contains("outcomes") && est->at("outcomes").is_array() &&
          !est->at("outcomes").empty()) {
        const auto& eq0 = est->at("outcomes")[0];
        check(eq0.value("depvar", std::string()) == "income",
              "D1g: first equation's depvar is \"income\"");
        double bBeertax = 0.0, seBeertax = 0.0;
        bool found = false;
        if (eq0.contains("coeftable") && eq0["coeftable"].is_array()) {
          for (const auto& row : eq0["coeftable"]) {
            if (row.value("name", std::string()) == "beertax") {
              bBeertax = row.value("b", 0.0);
              seBeertax = row.value("se", 0.0);
              found = true;
            }
          }
        }
        check(found && std::abs(bBeertax - (-1564.062)) < 0.5,
              "D1g oracle: income eq's beertax b ~= -1564.062, got " + std::to_string(bBeertax));
        check(found && std::abs(seBeertax - 198.693) < 0.5,
              "D1g oracle: income eq's beertax se ~= 198.693, got " + std::to_string(seBeertax));
      }
    }
  }

  // D1h — explicit `sure`: same equations, `, sure`. Oracle: beertax
  // b=-1732.268, se=206.824 (DIFFERENT from D1g's 3SLS numbers — genuine
  // method divergence, not a re-display).
  {
    auto blocks =
        piet::dispatch("reg3 (income beertax spirits) (emppop unemp income), sure", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D1h: `reg3 (...) (...), sure` returns an estimation block");
    if (est != nullptr) {
      check(est->value("method", std::string()) == "sure",
            "D1h: `sure` option forwards --method sure, got method=\"" +
                est->value("method", std::string()) + "\"");
      double bBeertax = 0.0, seBeertax = 0.0;
      bool found = false;
      if (est->contains("outcomes") && est->at("outcomes").is_array() &&
          !est->at("outcomes").empty()) {
        const auto& eq0 = est->at("outcomes")[0];
        if (eq0.contains("coeftable") && eq0["coeftable"].is_array()) {
          for (const auto& row : eq0["coeftable"]) {
            if (row.value("name", std::string()) == "beertax") {
              bBeertax = row.value("b", 0.0);
              seBeertax = row.value("se", 0.0);
              found = true;
            }
          }
        }
      }
      check(found && std::abs(bBeertax - (-1732.268)) < 0.5,
            "D1h oracle: income eq's beertax b ~= -1732.268 (differs from D1g's 3SLS "
            "-1564.062 — genuine SURE-vs-3SLS divergence), got " + std::to_string(bBeertax));
      check(found && std::abs(seBeertax - 206.824) < 0.5,
            "D1h oracle: income eq's beertax se ~= 206.824, got " + std::to_string(seBeertax));
    }
  }

  // Single equation -> honest reject (reg3 is inherently a >= 2-equation
  // system; the D0/D1 oracle never exercises this negative case directly).
  (void)piet::dispatch("capture reg3 (income beertax spirits)", session);
  check(session.eval.locals.get("_rc", "") == "198",
        "D1/BS-065: `reg3 (single equation)` honestly rejects — reg3 requires >= 2 equations");

  // vce()/noconstant/level -> honest reject (cobra reg3 has NO vcov/
  // no-intercept/level surface at all — live-verified).
  (void)piet::dispatch(
      "capture reg3 (income beertax spirits) (emppop unemp income), vce(cluster state)", session);
  check(session.eval.locals.get("_rc", "") == "198",
        "D1/BS-065: `reg3 ..., vce(cluster state)` honestly rejects (reg3 has no vcov "
        "surface yet)");

  fs::remove_all(workdir, ec);
}


void testD1BS068Keepsingletons() {
  std::printf("[selftest] --- D1/BS-068 reghdfe keepsingletons -> --keep-singletons ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d1_bs068").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);

  // fatalities has no singleton FE groups, so n_obs is unchanged either
  // way — this leg asserts the flag reaches the engine and is CONSUMED
  // without error (live-verified: cobra feols/iv both accept
  // --keep-singletons despite it not being printed in either's own usage
  // banner), not that it changes the answer on this particular fixture
  // (limitation noted per the task brief's own fallback instruction).
  {
    auto blocks =
        piet::dispatch("reghdfe income beertax, absorb(state) keepsingletons", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D1/BS-068: `reghdfe ..., keepsingletons` is consumed without error "
                          "(n_obs=336 either way on this singleton-free fixture — see comment)");
    if (est != nullptr) {
      check(est->value("n_obs", 0) == 336,
            "D1/BS-068: n_obs unchanged at 336 (fatalities has zero singleton FE groups)");
    }
  }

  // keepsingletons on plain regress/feols (no absorb) -> honest reject:
  // real Stata has no such option outside reghdfe.
  (void)piet::dispatch("capture regress income beertax, keepsingletons", session);
  check(session.eval.locals.get("_rc", "") == "198",
        "D1/BS-068: `regress ..., keepsingletons` honestly rejects (real Stata option is "
        "reghdfe-only)");

  // keepsingletons on the ivreghdfe path also reaches cobra iv cleanly.
  {
    auto blocks = piet::dispatch(
        "ivreghdfe income (beertax = spirits), absorb(state) keepsingletons", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D1/BS-068: `ivreghdfe ..., keepsingletons` is consumed without error");
  }

  fs::remove_all(workdir, ec);
}


void testD1BS066Boottest() {
  std::printf("[selftest] --- D1/BS-066 boottest (wild cluster bootstrap, engine-parity) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d1_bs066").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);

  // No preceding model at all: boottest.ado's own `No estimates detected` at
  // rc 0 (ado:37 inside its `cap noi` block; w2t_boottest_family.log B1
  // :216/:221). This was a refusal at rc 1 (FU-240); it is no bootstrap either way.
  {
    auto noest = piet::dispatch("capture noisily boottest beertax", session);
    const auto* msg = firstBlockOfTypeD1(noest, "display");
    check(session.eval.locals.get("_rc", "") == "0" && msg != nullptr &&
              msg->value("text", std::string()) == "No estimates detected" &&
              firstBlockOfTypeD1(noest, "estimation") == nullptr,
          "D1/BS-066: `boottest beertax` with no preceding model prints `No estimates "
          "detected` at rc 0 and bootstraps nothing (w2t B1)");
  }

  // Honest reject after a plain (non-clustered) regression — a real
  // lastModel exists, but with no cluster var, so boottest still refuses.
  (void)piet::dispatch("regress income beertax", session);
  (void)piet::dispatch("capture boottest beertax, reps(50)", session);
  check(session.eval.locals.get("_rc", "") != "0",
        "D1/BS-066: `boottest` after a plain (non-clustered) `regress` still honestly "
        "rejects — boottest requires vce(cluster ...)");

  (void)piet::dispatch("regress income beertax, vce(cluster state)", session);
  auto blocks1 = piet::dispatch("boottest beertax, reps(200) seed(42)", session);
  const auto* est1 = firstBlockOfTypeD1(blocks1, "estimation");
  check(est1 != nullptr, "D1/BS-066: `boottest beertax, reps(200) seed(42)` after a "
                        "vce(cluster) regress returns an estimation block");

  // Dossier decision (fixed-seed reproducibility, no StataBE oracle for
  // boottest — SSC-only command): the SAME seed must give the IDENTICAL
  // p-value across two independent runs.
  auto blocks2 = piet::dispatch("boottest beertax, reps(200) seed(42)", session);
  const auto* est2 = firstBlockOfTypeD1(blocks2, "estimation");
  check(est2 != nullptr, "D1/BS-066: second `boottest ..., seed(42)` run also returns an "
                        "estimation block");
  if (est1 != nullptr && est2 != nullptr) {
    check(est1->contains("p_equaltail") && est2->contains("p_equaltail") &&
              est1->at("p_equaltail") == est2->at("p_equaltail"),
          "D1/BS-066: same seed(42) -> IDENTICAL p_equaltail across two independent runs "
          "(fixed-seed reproducibility)");
    check(est1->value("reps", -1) == 200 && est2->value("reps", -1) == 200,
          "D1/BS-066: reps(200) is forwarded and echoed back by the engine");
    check(est1->value("seed", -1) == 42 && est2->value("seed", -1) == 42,
          "D1/BS-066: seed(42) is forwarded and echoed back by the engine");
  }

  // A DIFFERENT seed must NOT collide with the same p-value (guards
  // against a no-op seed forwarding bug masquerading as "reproducible").
  auto blocks3 = piet::dispatch("boottest beertax, reps(200) seed(7)", session);
  const auto* est3 = firstBlockOfTypeD1(blocks3, "estimation");
  if (est1 != nullptr && est3 != nullptr) {
    check(est3->value("seed", -1) == 7,
          "D1/BS-066: a different seed(7) is forwarded and echoed back distinctly from seed(42)");
  }

  fs::remove_all(workdir, ec);
}


// ---------------------------------------------------------------------------
// AER scope-2 B4 P0-C — `ivreg2`'s `robust` and `small`, which the W2 sweep put
// in the KNOWN set while the product measurably dropped both.
//
// This is the case the option census cannot see and an rc-only test cannot
// catch: rc was 0, the option was "supported", and the STANDARD ERROR was the
// one you get with no option at all. So the assertion here is the NUMBER, and
// the discriminator is that the three arms must DIFFER — a build that drops
// both flags returns one value three times and passes any rc check ever
// written.
//
// The three targets are real StataBE 19.5, `ivreg2 price (mpg = weight
// length)` on auto, se[mpg] captured as %21x
// (the internal pin-oracle capture oracle_p.log:250, :297, :341):
//   bare    +1.05536330e584fX+006  = 65.33143307114482
//   robust  +1.1856c0ef6dd05X+006  = 70.08472036464453
//   small   +1.08ee2e64fdfc7X+006  = 66.23259885597427
//
// SEEDED RED: against the pre-fix binary all three legs read 65.3314331, so
// the `robust` and `small` value legs fail and the "three arms differ" leg
// fails with them.
void testAerB4Ivreg2Inference() {
  std::printf("[selftest] --- AER scope-2 B4 P0-C: ivreg2 robust/small reach the engine ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_b4iv").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  const std::string autoCsv = selftestValidateDir() + "/auto.csv";
  if (!fs::exists(autoCsv)) {
    skip("B4 P0-C ivreg2 robust/small", "fixture not found at " + autoCsv);
    fs::remove_all(workdir, ec);
    return;
  }

  // se[mpg] off the estimation block's own coeftable — the same field the
  // renderer prints and `_se[mpg]` resolves against, so this is the number a
  // user sees, not a private one.
  auto seOf = [&](const std::string& opts) -> double {
    piet::SessionState session(workdir);
    (void)piet::dispatch("import delimited using \"" + autoCsv + "\", clear", session);
    double se = std::numeric_limits<double>::quiet_NaN();
    for (const auto& b : piet::dispatch("ivreg2 price (mpg = weight length)" + opts, session)) {
      if (!b.is_object() || b.value("type", std::string()) != "estimation") continue;
      for (const auto& row : b.value("coeftable", nlohmann::json::array())) {
        if (row.value("name", std::string()) == "mpg") se = row.value("se", 0.0);
      }
    }
    return se;
  };

  struct Arm {
    std::string opts;
    double stata;
    const char* logLine;
  };
  // rtol 1e-9 is TOLERANCE.md's cross-engine rung. The Stata side is exact (a
  // %21x capture IS the double), so anything looser would let a real drift by.
  const std::vector<Arm> kArms = {
      {"", 65.33143307114482, "oracle_p.log:250"},
      {", robust", 70.08472036464453, "oracle_p.log:297"},
      {", small", 66.23259885597427, "oracle_p.log:341"},
  };
  std::vector<double> got;
  for (const auto& a : kArms) {
    const double se = seOf(a.opts);
    got.push_back(se);
    const std::string arm = a.opts.empty() ? std::string("bare") : a.opts.substr(2);
    check(std::isfinite(se),
          "B4 P0-C: `ivreg2 price (mpg = weight length)" + a.opts + "` returns an estimation "
          "block with an mpg row");
    check(std::isfinite(se) && std::fabs(se - a.stata) <= 1e-9 * std::fabs(a.stata),
          "B4 P0-C: ivreg2 " + arm + " se[mpg] == StataBE's " + std::to_string(a.stata) + " (" +
              a.logLine + ", rtol 1e-9) -- got " + std::to_string(se));
  }

  // THE DISCRIMINATOR. Certifying an option the product ignores is the one
  // outcome that is not allowed, and this is the leg that can see it: before
  // the fix these three were the same number.
  check(got.size() == 3 && std::isfinite(got[0]) && got[0] != got[1] && got[0] != got[2] &&
            got[1] != got[2],
        "B4 P0-C: the three arms return three DIFFERENT standard errors -- an ivreg2 that "
        "accepts `robust`/`small` and drops them returns one value three times, which is what "
        "this build did and what no rc-only test could see");

  fs::remove_all(workdir, ec);
}
