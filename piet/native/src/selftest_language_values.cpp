// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: language values.
#include "selftest_support.h"
#include "selftest_cases.h"


// --- 8. _b[]/_se[] in scalar/gen expressions (2.0.4 regression pin) ---------
//
// Pins the fix for the silent-missing bug where `_b[name]`/`_se[name]`
// resolved in `display` but evaluated to MISSING (rc=0, no error block) in
// `scalar` assignments, compound expressions, and gen/replace: SessionState's
// setLastModel() must mirror the result JSON into eval.lastModelResult (the
// store macros.cpp's resolveEstScalarRefs reads), and the gen/replace
// dispatch must pre-resolve est-scalar refs before cobra sees the expression.
// The golden here is the coeftable itself: whatever `regress` just reported,
// `scalar x = _b[var]` must store that value (at the resolver's %.9g
// substitution precision), never ".".
void testEstScalarRefsInExpressions() {
  using piet::eval::pythonFloat;
  std::printf("[selftest] --- _b[]/_se[] in scalar/gen expressions (silent-missing pin) ---\n");

#ifdef _WIN32
  std::error_code bwec;
  std::string workdir = (std::filesystem::temp_directory_path() / "piet_native_selftest_bref").u8string();
  std::filesystem::create_directories(workdir, bwec);
#else
  std::string workdir = "/tmp/piet_native_selftest_bref";
  std::string mkcmd = "mkdir -p '" + workdir + "'";
  (void)std::system(mkcmd.c_str());
#endif

  std::string fixture = std::string(selftestSourceFile());
  {
    auto pos = fixture.rfind("/piet/native/src/selftest.cpp");
    fixture = (pos == std::string::npos)
                  ? "tests/validate/auto.hbin"
                  : fixture.substr(0, pos) + "/tests/validate/auto.hbin";
  }
  if (!std::ifstream(cecon::to_native_path(fixture)).good()) {
    // Worktree checkouts don't carry the untracked fixture — fall back to the
    // main checkout, same as run_gate.py's _find_validate().
    fixture = selftestValidateDir() + "/auto.hbin";
  }
  if (!std::ifstream(cecon::to_native_path(fixture)).good()) {
    skip("_b[]/_se[] expression pin (needs tests/validate/auto.hbin)",
         "fixture not found at " + fixture);
    return;
  }

  piet::SessionState session(workdir);
  auto runCmd = [&session](const std::string& cmd) {
    nlohmann::json body{{"cmd", cmd}};
    auto r = piet::routes::command(session, body.dump());
    return nlohmann::json::parse(r.json);
  };

  {
    nlohmann::json body{{"filepath", fixture}};
    (void)piet::routes::open(session, body.dump());
  }

  // regress, then read the coeftable's b/se for mpg — the golden values.
  double bMpg = 0.0, seMpg = 0.0;
  bool sawEstimation = false;
  {
    auto j = runCmd("regress price mpg weight");
    for (const auto& b : j.value("blocks", nlohmann::json::array())) {
      if (b.value("type", std::string()) != "estimation") continue;
      for (const auto& row : b.value("coeftable", nlohmann::json::array())) {
        if (row.value("name", std::string()) == "mpg") {
          bMpg = row.value("b", 0.0);
          seMpg = row.value("se", 0.0);
          sawEstimation = true;
        }
      }
    }
  }
  check(sawEstimation, "_b pin: regress returned an estimation block with an mpg row");
  if (!sawEstimation) return;

  // scalar x = _b[var] stores the coeftable value verbatim at the resolver's
  // %.9g substitution precision — the exact regression that shipped silent.
  {
    (void)runCmd("scalar pb = _b[mpg]");
    std::string stored = session.eval.scalars.get("pb", "<unset>");
    check(stored != "." && stored != "<unset>",
          "_b pin: `scalar pb = _b[mpg]` stores a value, not missing");
    check(pythonFloat(stored).has_value() &&
              std::fabs(*pythonFloat(stored) - bMpg) <= 1e-8 * std::fabs(bMpg),
          "_b pin: stored scalar pb equals the coeftable b (rel err <= 1e-8)");
  }

  // Compound expression: _b/_se — the hand-rolled t-ratio a do-file builds.
  {
    (void)runCmd("scalar pt = _b[mpg]/_se[mpg]");
    std::string stored = session.eval.scalars.get("pt", "<unset>");
    double want = bMpg / seMpg;
    check(pythonFloat(stored).has_value() &&
              std::fabs(*pythonFloat(stored) - want) <= 1e-7 * std::fabs(want),
          "_b pin: `scalar pt = _b[mpg]/_se[mpg]` stores the t-ratio, not missing");
  }

  // BUG-456 (2026-09-08): the ROW surfaces -- `assert` and `count if` -- take
  // `_b[]`/`_se[]`/e() with NO `scalar(` in the text. Before the fix the
  // dispatcher's substitution point ran the resolver only when the text
  // contained `scalar(`, so every one of these reached the engine as a variable
  // subscript / unknown function (Stata capture A2/A3/A7/A11/G8: rc 0/0/0/111,
  // r(N)=74). Each leg was RED on the pre-fix binary (main @ 6e4fb17e,
  // internal notes bug456-20260908 results.json + the per-leg table in the commit).
  {
    auto errReason = [](const nlohmann::json& j) -> std::string {
      for (const auto& b : j.value("blocks", nlohmann::json::array()))
        if (b.value("type", std::string()) == "error") return b.value("reason", std::string("<no-reason>"));
      return "";
    };
    check(errReason(runCmd("assert _se[mpg] > 0")).empty(),
          "BUG-456: `assert _se[mpg] > 0` (no scalar( in the text) passes silently (Stata A2 rc 0)");
    check(errReason(runCmd("assert _b[mpg] < 0")).empty(),
          "BUG-456: `assert _b[mpg] < 0` passes (Stata A3 rc 0)");
    check(errReason(runCmd("assert e(N) == 74")).empty(),
          "BUG-456: `assert e(N) == 74` passes -- an e-SCALAR by the bare form (Stata A7 rc 0)");
    check(errReason(runCmd("assert _b[mpg] > 0")) == "r(9)",
          "BUG-456: `assert _b[mpg] > 0` is a real r(9) assertion failure, not an evaluation error");
    {
      (void)runCmd("count if _b[mpg] < 0");
      const double* n = session.eval.rValues.find("N");
      check(n != nullptr && *n == 74.0, "BUG-456: `count if _b[mpg] < 0` answers r(N)=74 (Stata G8)");
    }
    check(errReason(runCmd("assert _b[nosuch] == 0")) == "r(111)",
          "BUG-456: `assert _b[nosuch] == 0` is r(111) `not found`, not r(9) on a silent `.` (Stata A11)");
    check(errReason(runCmd("assert e(nosuch) == .")).empty(),
          "BUG-456: an absent e() name is numeric missing on the row plane (Stata A8 rc 0)");
    check(errReason(runCmd("assert e(nosuch) == \"\"")) == "r(109)",
          "BUG-456: missing is NOT the empty string -- `assert e(nosuch)==\"\"` is r(109) (Stata A9)");
    check(errReason(runCmd("assert e(N) == \"74\"")) == "r(109)",
          "BUG-456: an e-scalar compared with a string literal is r(109) (Stata D10/M7)");
    // `_b[nosuch]` is r(111) on the macro surfaces too (Stata V3/V4), where the
    // resolver used to answer `.` with rc 0.
    check(errReason(runCmd("local bn = _b[nosuch]")) == "r(111)",
          "BUG-456: `local bn = _b[nosuch]` is r(111) (was a silent `.`)");
    check(errReason(runCmd("scalar sn = _se[nosuch]")) == "r(111)",
          "BUG-456: `scalar sn = _se[nosuch]` is r(111) (was a silent `.`)");
    // A numeric-LOOKING e-macro is a STRING everywhere (StataNow 19.5 BE,
    // refute3_capture.log Z01-Z40): never rule-1.3'd into a number.
    (void)runCmd("estadd local m5 \"5\"");
    check(errReason(runCmd("assert e(m5) == \"5\"")).empty(),
          "BUG-456: `estadd local m5 \"5\"` then `assert e(m5)==\"5\"` is rc 0 -- the macro is a string");
    check(errReason(runCmd("assert e(m5) == 5")) == "r(109)",
          "BUG-456: `assert e(m5) == 5` is r(109) -- a numeric-looking macro is NOT a number (refuter flip of [U] 18.9 rule 1.3)");
    (void)runCmd("local t5 = e(m5)");
    check(session.eval.locals.get("t5", "<unset>") == "5",
          "BUG-456: `local t5 = e(m5)` keeps the TEXT `5`");
    check(errReason(runCmd("local k5 = e(m5) + 1")) == "r(109)",
          "BUG-456: `local k5 = e(m5) + 1` is r(109), never 6 (refute3 Z01)");
    // A macro holding "." is the STRING "."; an empty macro is ABSENT (T10-T15).
    (void)runCmd("estadd local dd \".\"");
    check(errReason(runCmd("assert e(dd) == \".\"")).empty() && errReason(runCmd("assert e(dd) == .")) == "r(109)",
          "BUG-456: `estadd local dd \".\"` is the string `.`: ==\".\" rc 0, ==. r(109) (T10/T11)");
    (void)runCmd("estadd local em \"\"");
    check(session.eval.eStrings.find("em") == nullptr && errReason(runCmd("assert e(em) == .")).empty(),
          "BUG-456: an e-macro set to \"\" does not exist -- `assert e(em)==.` rc 0 (T13)");
    // e-string / e-scalar are ONE namespace: the last write erases its sibling.
    session.eval.setEScalar("dup", 3.0);
    session.eval.setEString("dup", "x");
    check(session.eval.eScalars.find("dup") == nullptr && session.eval.eStrings.get("dup", "") == "x",
          "BUG-456: setEString erases the same-named e-scalar (Stata refuses the collision, rc 110/152)");
    session.eval.setEScalar("dup", 4.0);
    check(session.eval.eStrings.find("dup") == nullptr && session.eval.eScalars.find("dup") != nullptr,
          "BUG-456: setEScalar erases the same-named e-string (last write wins)");
    // BUG-458: after `regress`, e(cmd) is the STATA spelling on every surface,
    // and `ereturn list` reads the same store.
    check(session.eval.eStrings.get("cmd", "") == "regress" && session.eval.eStrings.get("depvar", "") == "price",
          "BUG-458: regress publishes e(cmd)=regress, e(depvar)=price into the e-macro store (was empty; ereturn list said feols)");
    (void)runCmd("local rc0 = e(cmd)");
    check(session.eval.locals.get("rc0", "<unset>") == "regress",
          "BUG-458: `local rc0 = e(cmd)` after regress is `regress`");
    {
      auto j = runCmd("ereturn list");
      bool sawCmd = false;
      for (const auto& b : j.value("blocks", nlohmann::json::array()))
        if (b.value("text", std::string()).find("e(cmd) : \"regress\"") != std::string::npos) sawCmd = true;
      check(sawCmd, "BUG-458: `ereturn list` prints e(cmd) : \"regress\" (the Stata spelling, from the store)");
    }
    // ---------------------------------------------------------------------
    // BUG-456 FOLLOW-UP (2026-09-09, review finding 1): a NUMBER IN A STRING
    // SLOT. Widening the row-plane trigger to hasBareEParen made the resolver
    // substitute a number (`.` for an absent name, the value for an e-scalar)
    // INSIDE string-function arguments, where pythonStr() coerced it and
    // answered 0 / the decimal width at rc 0 -- a silent wrong answer on the
    // exact surface that landing opened. StataNow 19.5 BE, measured 2026-09-09
    // on `sysuse auto; regress price mpg` (scratch oracle.log O1-O23): every
    // one of these is r(109) "type mismatch", while the e-STRING arm keeps
    // answering (O13-O16: 74, 74, 74, 3) and an all-numeric inlist keeps
    // answering (O23: 74).
    //
    // Seeded-RED: comment out the `stringFnNumericArgImpl` block at the end of
    // eval::resolveEstScalarRefs (macros.cpp) and all twelve refusal legs fail
    // (measured: 12 failures, "want r(109) got ''"), which is what the pre-fix
    // branch binary did -- `count if strpos(e(nosuch),"reg")` rc 0 r(N)=0,
    // `local q = strlen(e(N))` rc 0 q=5, `display strlen(e(nosuch))` 22.
    {
      check(errReason(runCmd("count if strpos(e(nosuch),\"reg\")")) == "r(109)",
            "BUG-456f: `count if strpos(e(nosuch),\"reg\")` is r(109), not rc 0 r(N)=0 (Stata O1)");
      check(errReason(runCmd("count if inlist(e(nosuch),\"regress\")")) == "r(109)",
            "BUG-456f: `count if inlist(e(nosuch),\"regress\")` is r(109) (Stata O2)");
      check(errReason(runCmd("count if strlen(e(nosuch))==7")) == "r(109)",
            "BUG-456f: `count if strlen(e(nosuch))==7` is r(109) (Stata O3)");
      check(errReason(runCmd("count if substr(e(nosuch),1,3)==\"reg\"")) == "r(109)",
            "BUG-456f: `count if substr(e(nosuch),1,3)==\"reg\"` is r(109) (Stata O4)");
      check(errReason(runCmd("count if upper(e(nosuch))==\"REG\"")) == "r(109)",
            "BUG-456f: `count if upper(e(nosuch))==\"REG\"` is r(109) (Stata O5)");
      check(errReason(runCmd("local pp = strpos(e(nosuch),\"reg\")")) == "r(109)",
            "BUG-456f: `local pp = strpos(e(nosuch),\"reg\")` is r(109), not p=0 (Stata O6)");
      check(errReason(runCmd("gen zfa = strpos(e(nosuch),\"reg\")")) == "r(109)",
            "BUG-456f: `gen zfa = strpos(e(nosuch),\"reg\")` is r(109), not a column of zeros (Stata O7)");
      check(errReason(runCmd("replace price = 1 if strpos(e(nosuch),\"reg\")")) == "r(109)",
            "BUG-456f: `replace ... if strpos(e(nosuch),...)` is r(109), not a silent no-op (Stata O12)");
      check(errReason(runCmd("count if strpos(e(N),\"7\")")) == "r(109)",
            "BUG-456f: a PRESENT e-scalar in a string slot is r(109) too -- `strpos(e(N),\"7\")` (Stata O8)");
      check(errReason(runCmd("count if inlist(e(N),\"74\")")) == "r(109)",
            "BUG-456f: `count if inlist(e(N),\"74\")` is r(109) -- inlist is polymorphic, a MIXED call is the mismatch (Stata O9)");
      check(errReason(runCmd("local qq = strlen(e(N))")) == "r(109)",
            "BUG-456f: `local qq = strlen(e(N))` is r(109), not the decimal width of the substituted number (Stata O10)");
      check(errReason(runCmd("display strlen(e(nosuch))")) == "r(109)",
            "BUG-456f: `display strlen(e(nosuch))` is r(109), not 22 -- the missing sentinel's width never reaches the user (Stata O11)");
      // The ROW/GEN planes run the test UNGATED, which is load-bearing: the
      // macro expansion layer substitutes a PRESENT e-scalar before the
      // dispatcher sees the line, so `count if strpos(e(N),"2")` arrives as
      // `strpos(20.0,"2")` with no `e(` left to trigger the resolver (measured
      // 2026-09-09 with a debug print at the row call site; the BUG-456 probe
      // do-file is the live-fire leg for it). These two legs pin that entry
      // point directly -- comment out the `eval::stringFnNumericArg` block in
      // dispatcher.cpp substituteRowExprRefs / preResolveGenExpr and only these
      // two fail. Stata: `count if strpos(5,"a")` r(109), `local z = strlen(5)`
      // r(109) (O17/O18).
      check(errReason(runCmd("count if strpos(74.0,\"7\")")) == "r(109)",
            "BUG-456f: the row plane refuses a number in a string slot with no e() left in the text (Stata O17)");
      check(errReason(runCmd("gen zfb = strlen(74)")) == "r(109)",
            "BUG-456f: the gen plane refuses it too (Stata O18 class)");
      // A STORED SCALAR is the same number in the same slot, and it is
      // substituted by a DIFFERENT pass on each surface -- so each surface
      // needs the test after its own substitution. Stata: `local sf =
      // strpos(one,"1")` r(109) (oracle2.log P10), `gen z = (e(cmd)==one)`
      // r(109) (P1) where this engine answered rc 1 "generate: expression
      // evaluation failed at obs 1" with no r() code (review finding 3).
      (void)runCmd("scalar sone = 1");
      check(errReason(runCmd("local sf = strpos(sone,\"1\")")) == "r(109)",
            "BUG-456f: `local sf = strpos(sone,\"1\")` is r(109) -- the macro surface tests AFTER its scalar substitution (Stata P10)");
      check(errReason(runCmd("gen zfc = (e(cmd)==sone)")) == "r(109)",
            "BUG-456f: `gen zfc = (e(cmd)==sone)` is r(109), not the coarse obs-1 failure (Stata P1)");
      // The ACCEPTING arm is untouched: an e-STRING is a string, so the string
      // functions take it, and an all-numeric inlist stays numeric.
      {
        (void)runCmd("count if strpos(e(cmd),\"reg\")");
        const double* n = session.eval.rValues.find("N");
        check(n != nullptr && *n == 74.0,
              "BUG-456f: `count if strpos(e(cmd),\"reg\")` still answers r(N)=74 (Stata O13)");
      }
      {
        (void)runCmd("count if inlist(e(cmd),\"regress\")");
        const double* n = session.eval.rValues.find("N");
        check(n != nullptr && *n == 74.0,
              "BUG-456f: `count if inlist(e(cmd),\"regress\")` still answers r(N)=74 (Stata O14)");
      }
      (void)runCmd("local cc = strpos(e(cmd),\"gres\")");
      check(session.eval.locals.get("cc", "<unset>") == "3",
            "BUG-456f: `local cc = strpos(e(cmd),\"gres\")` still answers 3 (Stata O16)");
      {
        (void)runCmd("count if inlist(e(N),74)");
        const double* n = session.eval.rValues.find("N");
        check(n != nullptr && *n == 74.0,
              "BUG-456f: an ALL-NUMERIC inlist is not a mismatch -- `inlist(e(N),74)` answers 74 (Stata O23)");
      }
      // cond() -- review finding 1 (2026-09-09). cond() was MISSING from
      // stringFnArgSpecs(), so the rule this landing wrote was not enforced for
      // a function this engine registers (exprfns.cpp `ns.fns["cond"]`): the
      // pre-fix branch binary answered `local cd = cond(1,e(nosuch),"x")` rc 0
      // with cd=`.`, `local c6 = cond(1,e(N),"x")` rc 0 with c6=74, and
      // `local c5 = cond(1,5,"x")` rc 0 with c5=5 -- the silent-wrong-answer
      // class, on the string-function surface this landing claims to have
      // closed. Its polymorphic group is slots 1 and 2 ONLY (slot 0 is the
      // numeric condition), which is why the blanket `poly` flag could not be
      // reused: `cond(1,"a","b")` is rc 0 and a whole-arglist test would call
      // it a mismatch on account of the condition.
      //
      // ORACLE, StataNow 19.5 BE, `sysuse auto; regress price mpg`:
      // /private/tmp/bug456rev/cond_oracle.log Q1-Q6 (sentinel CONDQ-ORACLE-END)
      // and my own addendum capture cond2_oracle.log R1-R8 (sentinel
      // CONDR-ORACLE2-END): Q1/Q2/Q3/Q4 r(109) · Q5 rc 0 r(N)=74 · Q6 rc 0 "a" ·
      // R1 rc 0 "2" · R2 rc 0 r(N)=36 · R3 rc 0 "regress" · R4 r(109) ·
      // R5/R6/R7/R8 (count/gen/scalar/assert) r(109).
      //
      // Seeded-RED: delete the `{"cond", {}, false, {1, 2}}` row from
      // stringFnArgSpecs() (macros.cpp) and the eight refusal legs below fail
      // with "want r(109) got ''" while the six accepting legs stay green --
      // which is exactly the pre-fix branch binary, measured.
      check(errReason(runCmd("local cdq = cond(1,e(nosuch),\"x\")")) == "r(109)",
            "BUG-456f/cond: `local cdq = cond(1,e(nosuch),\"x\")` is r(109), not rc 0 cdq=`.` (Stata Q1)");
      check(errReason(runCmd("count if cond(1,e(nosuch),\"x\")==\"x\"")) == "r(109)",
            "BUG-456f/cond: the ROW plane refuses it too -- r(109), not the coarse obs-1 rc 1 (Stata Q2)");
      check(errReason(runCmd("local c5q = cond(1,5,\"x\")")) == "r(109)",
            "BUG-456f/cond: a numeric LITERAL in the pair is r(109), not c5q=5 (Stata Q3)");
      check(errReason(runCmd("local c6q = cond(1,e(N),\"x\")")) == "r(109)",
            "BUG-456f/cond: a PRESENT e-scalar in the pair is r(109), not c6q=74 (Stata Q4)");
      check(errReason(runCmd("local c9q = cond(1,\"a\",5)")) == "r(109)",
            "BUG-456f/cond: the pair is symmetric -- string then number is r(109) too (Stata R4)");
      check(errReason(runCmd("count if cond(1,e(N),\"x\")==\"x\"")) == "r(109)",
            "BUG-456f/cond: the row plane refuses the PRESENT-scalar shape (Stata R5)");
      check(errReason(runCmd("gen zfd = cond(1,e(N),\"x\")==\"x\"")) == "r(109)",
            "BUG-456f/cond: the gen plane refuses it with the code, not the coarse obs-1 rc (Stata R6)");
      check(errReason(runCmd("scalar sqd = cond(1,e(N),\"x\")")) == "r(109)",
            "BUG-456f/cond: the scalar surface refuses it (Stata R7)");
      // ACCEPTING arms -- none of these may become a false r(109).
      {
        (void)runCmd("count if cond(1,e(cmd),\"x\")==\"regress\"");
        const double* n = session.eval.rValues.find("N");
        check(n != nullptr && *n == 74.0,
              "BUG-456f/cond: an e-STRING in the pair still answers -- r(N)=74 (Stata Q5)");
      }
      (void)runCmd("local c7q = cond(1,\"a\",\"b\")");
      check(session.eval.locals.get("c7q", "<unset>") == "a",
            "BUG-456f/cond: an all-STRING pair is rc 0 \"a\" -- the numeric condition is not in the group (Stata Q6)");
      (void)runCmd("local r1q = cond(1,2,3)");
      check(session.eval.locals.get("r1q", "<unset>") == "2",
            "BUG-456f/cond: an all-NUMERIC pair is rc 0 2 (Stata R1)");
      (void)runCmd("local r3q = cond(1,e(cmd),\"x\")");
      check(session.eval.locals.get("r3q", "<unset>") == "regress",
            "BUG-456f/cond: the e-string arm still answers \"regress\" (Stata R3)");
      {
        (void)runCmd("count if cond(mpg>20,1,0)");
        const double* n = session.eval.rValues.find("N");
        check(n != nullptr && *n == 36.0,
              "BUG-456f/cond: an ordinary numeric cond() in a row predicate still answers r(N)=36 (Stata R2)");
      }
    }
  }

  // gen sees the same value display does (pre-resolution before cobra).
  {
    auto j = runCmd("gen bref = _b[mpg]");
    bool genOk = false;
    for (const auto& b : j.value("blocks", nlohmann::json::array())) {
      if (b.value("type", std::string()) == "data_mutation") genOk = true;
      if (b.value("type", std::string()) == "error") genOk = false;
    }
    check(genOk, "_b pin: `gen bref = _b[mpg]` succeeds (no engine error)");
    auto s = runCmd("summarize bref");
    bool meanOk = false;
    // An untyped generate stores Stata's default float (w2e_float_default.log:96,
    // gen_untyped_13: `generate g_u13 = 1/3` -> +1.5555560000000X-002 float). Here
    // b = -49.51222066635501 and float(b) = -49.51222229003906, rel 3.3e-8 apart,
    // so the old full-double column misses this band; 74 equal floats average exactly.
    const double bFloat = static_cast<double>(static_cast<float>(bMpg));
    for (const auto& b : s.value("blocks", nlohmann::json::array())) {
      if (b.value("type", std::string()) != "summarize") continue;
      double mean = b["stats"]["bref"].value("mean", 0.0);
      meanOk = std::fabs(mean - bFloat) <= 1e-12 * std::fabs(bMpg);
    }
    check(meanOk, "_b pin: gen'd column equals float(coeftable b) -- untyped generate stores float "
                  "(w2e_float_default.log:96; rel err <= 1e-12)");
  }

  // clear all drops the model AND the mirrored eval store -- never a stale
  // coefficient. With no e(b) Stata refuses `_b[]` on every surface:
  // `local __v = _b[x1]` after `ereturn clear` is r(111) "_b not allowed when
  // e(b) is not present" (w2v_estpost_ematrix.log:2048-2055 ep15f_local_bx1),
  // and so is `scalar s = _b[x1]` (w2g_ebmat.log:2163-2169 e15c_scalar_bx1).
  // The macro layer's old no-model "." at rc 0 was a named divergence that
  // capture refuted (FU-949); no scalar may be created.
  {
    auto errBlock = [](const nlohmann::json& j) -> nlohmann::json {
      for (const auto& b : j.value("blocks", nlohmann::json::array()))
        if (b.value("type", std::string()) == "error") return b;
      return nlohmann::json::object();
    };
    (void)runCmd("clear all");
    nlohmann::json e = errBlock(runCmd("scalar pz = _b[mpg]"));
    check(e.value("reason", std::string()) == "r(111)" &&
              e.value("message", std::string()) == "_b not allowed when e(b) is not present" &&
              session.eval.scalars.get("pz", "<unset>") == "<unset>",
          "_b pin: after `clear all`, `scalar pz = _b[mpg]` is r(111) `_b not allowed when e(b) is not "
          "present` and stores nothing (w2v_estpost_ematrix.log:2048-2055 ep15f_local_bx1; "
          "w2g_ebmat.log:2163-2169 e15c_scalar_bx1) -- never a stale coefficient, never `.` at rc 0");
  }
}



void testBug341VarSubscript() {
  std::printf("[selftest] --- BUG-341 `display var[i]` over a resident/virtual head ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug341").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto displays = [](const auto& blocks) {
    std::vector<std::string> out;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "display")
        out.push_back(b.value("text", std::string()));
    return out;
  };

  piet::SessionState session(workdir);
  std::string src =
      "clear\nset obs 5\ngen double v = _n * 10\n"
      "display v[1]\ndisplay v[5]\ndisplay v[6]\ndisplay v[0]\n"
      "gen double z2 = v*2\ndisplay z2[1]\n";
  auto r = piet::doexec::executeDoSource(src, session, piet::dispatch);
  auto d = displays(r.blocks);
  int errs = 0;
  for (const auto& b : r.blocks)
    if (b.is_object() && b.value("type", std::string()) == "error") ++errs;
  check(errs == 0, "BUG-341: subscripted reads raise no error (was `matrix v not found`)");
  check(d.size() == 5, "BUG-341: five display lines came back (got " + std::to_string(d.size()) + ")");
  if (d.size() == 5) {
    check(d[0] == "10", "BUG-341: v[1] == 10 (Stata-measured), got `" + d[0] + "`");
    check(d[1] == "50", "BUG-341: v[5] == 50 (the last row), got `" + d[1] + "`");
    check(d[2] == ".", "BUG-341: v[6] is missing, not an error (Stata-measured)");
    check(d[3] == ".", "BUG-341: v[0] is missing — Stata subscripts are 1-based");
    check(d[4] == "20", "BUG-341: a column created THIS session reads through the virtual head "
                        "(z2[1] == 20), got `" + d[4] + "`");
  }
  // The matrix arm must still answer for a name that is neither variable nor matrix:
  // StataBE 19.5's `ZZZNOSUCH not found`, r(111) (BUG-1339 review, BUG-1402, 2026-11-03;
  // this arm pinned the old `matrix ZZZNOSUCH not found` at rc 1).
  {
    piet::SessionState s2(workdir);
    auto r2 = piet::doexec::executeDoSource("display ZZZNOSUCH[1,1]\n", s2, piet::dispatch);
    bool sawErr = false;
    for (const auto& b : r2.blocks)
      if (b.is_object() && b.value("type", std::string()) == "error" &&
          b.value("message", std::string()) == "ZZZNOSUCH not found" &&
          b.value("reason", std::string()) == "r(111)")
        sawErr = true;
    check(sawErr, "BUG-341: an undefined NAME[i,j] reports `NAME not found`, r(111) (BUG-162, BUG-1402)");
  }
}


// ── AER scope 2, bundle B2: strdist ─────────────────────────────────────────
//
// Every expected value is a MEASUREMENT against StataNow 19.5 BE with
// strdist 1.2 installed, captured before the handler was written and committed
// as tests/validate/b2_oracle.{do,json}. `strdist` is the last name that blocked
// paper 100 at the name level.
//
// THE LEGS THAT ARE DISCRIMINATORS RATHER THAN SMOKE:
//   * "a" vs "A" is 1, not 0. The package's Mata kernel compares `ascii()`
//     bytes, so the distance is BYTE-wise; a codepoint- or case-folded
//     implementation answers 0 and looks more helpful while disagreeing.
//   * maxdist() does not clamp, it BLANKS: over the limit is missing, not the
//     limit. `strdist p q, maxdist(2)` on kitten/sitting gives an all-missing
//     column (r(N) == 0), not a column of 2s.
//   * `strdist p` with ONE argument is legal and is not `strdist p p`. It is
//     the distance from the EMPTY string, i.e. the length: 6 for "kitten".
//   * With no gen(), the new variable is called `strdist` -- the literal word.
//     Measured with `ds`, not assumed from the help file.
void testAerB2Strdist() {
  std::printf("[selftest] --- AER scope-2 bundle B2: strdist ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_aerb2").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto run = [&](const std::string& src) {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;
    return piet::doexec::executeDoSource(src, s, piet::dispatch);
  };
  auto lastDisplay = [](const nlohmann::ordered_json& blocks) {
    std::string out;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display")
        out = b.value("text", std::string());
    }
    return out;
  };
  // One pair, through summarize: `d[1]` inside a display reads as a MATRIX
  // reference in this build, which is a separate gap and not this bundle's.
  auto dist = [&](const std::string& a, const std::string& b, const std::string& opts) {
    return lastDisplay(run("clear\nset obs 1\ngen str32 sa = \"" + a +
                           "\"\ngen str32 sb = \"" + b + "\"\nstrdist sa sb, gen(dd)" + opts +
                           "\nquietly summarize dd\ndisplay r(mean)\n")
                           .blocks);
  };
  auto rcOfSrc = [&](const std::string& body) {
    return lastDisplay(run(body + "\nlocal rc = _rc\ndisplay \"`rc'\"\n").blocks);
  };

  check(dist("kitten", "sitting", "") == "3", "B2 strdist: kitten -> sitting is 3");
  check(dist("sitting", "kitten", "") == "3", "B2 strdist: ...and the metric is symmetric");
  check(dist("flaw", "lawn", "") == "2", "B2 strdist: flaw -> lawn is 2");
  check(dist("abcdef", "abcdef", "") == "0", "B2 strdist: identical strings are 0");
  check(dist("", "abc", "") == "3", "B2 strdist: an empty side is the other's length");
  check(dist("abc", "", "") == "3", "B2 strdist: ...from either side");
  check(dist("", "", "") == "0", "B2 strdist: two empties are 0");
  check(dist("a", "A", "") == "1",
        "B2 strdist: `a` vs `A` is 1. BYTE-wise, because the package compares ascii() codes -- a "
        "case-folded implementation answers 0 and is wrong in the helpful direction");
  check(dist("ab cd", "abcd", "") == "1", "B2 strdist: a space is an ordinary character");

  // maxdist blanks rather than clamps.
  check(lastDisplay(run("clear\nset obs 1\ngen str9 p = \"kitten\"\ngen str9 q = \"sitting\"\n"
                        "strdist p q, gen(m1) maxdist(2)\nquietly summarize m1\ndisplay r(N)\n")
                        .blocks) == "0",
        "B2 strdist: maxdist(2) over a distance of 3 leaves MISSING (r(N)==0), it does not clamp "
        "to 2");
  check(dist("kitten", "sitting", " maxdist(3)") == "3",
        "B2 strdist: maxdist(3) admits a distance of exactly 3");
  check(dist("kitten", "sitting", " maxdist(0)") == "3",
        "B2 strdist: maxdist(0) means NO limit (the ado normalises anything below 1)");

  // The single-argument form, the default name, and the two-literal form.
  check(lastDisplay(run("clear\nset obs 1\ngen str9 p = \"kitten\"\nstrdist p\n"
                        "quietly summarize strdist\ndisplay r(mean)\n")
                        .blocks) == "6",
        "B2 strdist: ONE argument is the distance from the empty string, i.e. the length (6). It "
        "is not `strdist p p`, which would be 0");
  check(lastDisplay(run("clear\nset obs 1\ngen str9 p = \"kitten\"\ngen str9 q = \"sitting\"\n"
                        "strdist p q\nds\n")
                        .blocks) == "p q strdist",
        "B2 strdist: with no gen() the new variable is called `strdist` -- the literal word");
  check(lastDisplay(run("clear\nset obs 1\ngen x = 1\nstrdist \"abc\" x\n"
                        "quietly summarize strdist\ndisplay r(mean)\n")
                        .blocks) != "",
        "B2 strdist: a LITERAL paired with a variable is the per-row distance to that literal");
  check(lastDisplay(run("clear\nset obs 1\ngen str9 p = \"kitten\"\nstrdist \"abc\" p\n"
                        "quietly summarize strdist\ndisplay r(mean)\n")
                        .blocks) == "6",
        "B2 strdist: ...abc -> kitten is 6");
  {
    auto r = run("clear\nset obs 1\ngen x = 1\nstrdist \"kitten\" \"sitting\"\ndisplay r(d)\n");
    check(lastDisplay(r.blocks) == "3",
          "B2 strdist: TWO literals set r(d) and create nothing");
  }

  // The refusals, each with the rc Stata answers.
  check(rcOfSrc("clear\nset obs 1\ngen str9 p = \"a\"\ngen str9 q = \"b\"\n"
                "capture strdist p q p") == "103",
        "B2 strdist: three arguments is r(103)");
  check(rcOfSrc("clear\nset obs 1\ngen str9 p = \"a\"\ncapture strdist p nosuchvar, gen(z)") ==
            "111",
        "B2 strdist: an absent variable is r(111)");
  check(rcOfSrc("clear\nset obs 1\ngen str9 p = \"a\"\ngen str9 q = \"b\"\n"
                "capture strdist p q, gen(p)") == "110",
        "B2 strdist: a target that already exists is r(110) -- `confirm new variable`, not this "
        "build's generic bad_input at rc 1");
}


void testBS003DestringTostring() {
  std::printf("[selftest] --- BS-003 destring/tostring (PIET-native, D4) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs003").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("keep in 1/5", session);

  // --- destring: ignore() strips characters before parsing ---
  (void)piet::dispatch("gen str10 bs003dirty = \"1,234\"", session);
  auto destringBlocks =
      piet::dispatch("destring bs003dirty, gen(bs003clean) ignore(\",\")", session);
  bool destringErr = false;
  for (const auto& b : destringBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") destringErr = true;
  }
  check(!destringErr, "BS-003: `destring ..., ignore(\",\")` on a comma-formatted string succeeds");
  piet::Datastore* ds1 = session.datastore();
  bool cleanOk = ds1 != nullptr && ds1->has("bs003clean") && ds1->dtype("bs003clean") == "f64";
  check(cleanOk, "BS-003: destring's target lands as a NUMERIC (f64) column");
  if (cleanOk && ds1->nRows() > 0) {
    check(ds1->col("bs003clean").f64()[0] == 1234.0,
          "BS-003: `ignore(\",\")` strips the comma before parsing (\"1,234\" -> 1234)");
  }

  // --- destring: skip note without force; missing under force ---
  // BUG-664: Stata's destring leaves a variable with nonnumeric content as is and
  // prints "VAR: contains nonnumeric characters; no replace|generate" at rc 0
  // (tests/eval/golden/aer_w2/w2b_dataio.log:350, destring_nodp_ctl). This
  // check used to pin an error block here.
  (void)piet::dispatch("gen str10 bs003bad = \"abc\"", session);
  auto badBlocks = piet::dispatch("destring bs003bad, gen(bs003badnum)", session);
  bool badErr = false;
  bool badNote = false;
  for (const auto& b : badBlocks) {
    if (!b.is_object()) continue;
    const std::string type = b.value("type", std::string());
    if (type == "error") badErr = true;
    if (type == "display" && b.value("text", std::string())
                                     .find("bs003bad: contains nonnumeric characters; no generate") !=
                                 std::string::npos)
      badNote = true;
  }
  check(!badErr && badNote,
        "BS-003: destring without `force` on a non-numeric string is skipped with Stata's note "
        "(w2b_dataio.log:350 destring_nodp_ctl), not an error");
  piet::Datastore* dsBad = session.datastore();
  check(dsBad != nullptr && !dsBad->has("bs003badnum"),
        "BS-003: the skipped destring creates no generate() target");
  (void)piet::dispatch("capture destring bs003bad, gen(bs003badnum)", session);
  std::string badRc;
  for (const auto& b : piet::dispatch("display _rc", session)) {
    if (b.is_object() && b.value("type", std::string()) != "echo")
      badRc += b.value("text", std::string());
  }
  badRc.erase(std::remove_if(badRc.begin(), badRc.end(), ::isspace), badRc.end());
  check(badRc == "0", "BS-003: the skipped destring leaves _rc 0 (got `" + badRc + "')");

  auto forcedBlocks = piet::dispatch("destring bs003bad, gen(bs003badnum2) force", session);
  bool forcedErr = false;
  for (const auto& b : forcedBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") forcedErr = true;
  }
  check(!forcedErr, "BS-003: destring WITH `force` on a non-numeric string succeeds");
  piet::Datastore* ds2 = session.datastore();
  bool forcedOk = ds2 != nullptr && ds2->has("bs003badnum2") && ds2->nRows() > 0;
  check(forcedOk && std::isnan(ds2->col("bs003badnum2").f64()[0]),
        "BS-003: `force` converts the unparseable value to missing, not a silent wrong number");

  // --- tostring: default rendering vs usedisplayformat ---
  (void)piet::dispatch("format year %9.2f", session);
  (void)piet::dispatch("tostring year, gen(bs003yeardef)", session);
  (void)piet::dispatch("tostring year, gen(bs003yearfmt) usedisplayformat", session);
  piet::Datastore* ds3 = session.datastore();
  bool tostringOk = ds3 != nullptr && ds3->has("bs003yeardef") && ds3->has("bs003yearfmt") &&
                    ds3->dtype("bs003yeardef") == "factor" && ds3->dtype("bs003yearfmt") == "factor";
  check(tostringOk, "BS-003: tostring's target lands as a STRING (factor) column, both forms");
  if (tostringOk && ds3->nRows() > 0) {
    int32_t code0 = ds3->col("bs003yeardef").codes()[0];
    int32_t code1 = ds3->col("bs003yearfmt").codes()[0];
    const auto& lv0 = ds3->col("bs003yeardef").levels;
    const auto& lv1 = ds3->col("bs003yearfmt").levels;
    std::string defStr =
        (code0 >= 0 && static_cast<size_t>(code0) < lv0.size()) ? lv0[static_cast<size_t>(code0)] : "";
    std::string fmtStr =
        (code1 >= 0 && static_cast<size_t>(code1) < lv1.size()) ? lv1[static_cast<size_t>(code1)] : "";
    check(defStr.find('.') == std::string::npos,
          "BS-003: tostring's DEFAULT rendering of a whole-number year has no decimal point");
    check(fmtStr.find('.') != std::string::npos,
          "BS-003: tostring `usedisplayformat` honors the variable's own `format ... %9.2f` "
          "(decimal point present)");
  }

  // --- B5-D1: tostring `force` and the `g()` alias for `generate` ---
  // BUG-665: `force` is no longer a no-op -- it overrides tostring.ado's
  // reversibility test. Whole-number years convert reversibly, so the
  // rendering must be the same with or without it, which is what this pins.
  auto forceBlocks = piet::dispatch("tostring year, gen(bs003forcegen) force", session);
  bool forceErr = false;
  for (const auto& b : forceBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") forceErr = true;
  }
  check(!forceErr, "B5-D1: `tostring ..., force` is accepted (rc 0), not r(198)");
  piet::Datastore* ds4 = session.datastore();
  bool forceOk = ds4 != nullptr && ds4->has("bs003forcegen") && ds4->has("bs003yeardef") &&
                ds4->dtype("bs003forcegen") == "factor";
  check(forceOk, "B5-D1: tostring's target under `force` still lands as a STRING (factor) column");
  if (forceOk && ds4->nRows() > 0) {
    auto decode = [&](const char* var) -> std::string {
      int32_t code = ds4->col(var).codes()[0];
      const auto& lv = ds4->col(var).levels;
      return (code >= 0 && static_cast<size_t>(code) < lv.size()) ? lv[static_cast<size_t>(code)]
                                                                   : "";
    };
    check(decode("bs003forcegen") == decode("bs003yeardef"),
          "B5-D1: on a reversible variable `force` does not change the rendered value "
          "byte-for-byte vs. plain tostring (internal capture "
          "tostring_missing_oracle.log:67)");
  }
  auto gAliasBlocks = piet::dispatch("tostring year, g(bs003galias)", session);
  bool gAliasErr = false;
  for (const auto& b : gAliasBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") gAliasErr = true;
  }
  check(!gAliasErr, "B5-D1: `tostring ..., g(newvar)` (the `g` alias of `generate`) is accepted");
  piet::Datastore* ds5 = session.datastore();
  check(ds5 != nullptr && ds5->has("bs003galias") && ds5->dtype("bs003galias") == "factor",
        "B5-D1: the `g()` alias target lands as a STRING (factor) column, same as `gen()`");

  fs::remove_all(workdir, ec);
}


void testBS005FormatDisplayLayer() {
  std::printf("[selftest] --- BS-005 `format VAR %%fmt` display-layer attribute (D6) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs005").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("keep in 1/3", session);

  auto fmtBlocks = piet::dispatch("format year %td", session);
  bool fmtErr = false;
  for (const auto& b : fmtBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") fmtErr = true;
  }
  check(!fmtErr, "BS-005: `format year %td` (a recognized format family) does not error");

  auto listBlocks = piet::dispatch("list year", session);
  bool foundFormat = false;
  for (const auto& b : listBlocks) {
    if (!b.is_object() || b.value("type", std::string()) != "browse") continue;
    for (const auto& col : b.value("columns", nlohmann::ordered_json::array())) {
      if (col.value("name", std::string()) == "year" && col.value("format", std::string()) == "%td") {
        foundFormat = true;
      }
    }
  }
  check(foundFormat,
        "BS-005: `list` honors the session format — `year`'s column-meta carries "
        "\"format\": \"%td\"");

  auto badBlocks = piet::dispatch("format year %notaformat", session);
  bool badErr = false;
  for (const auto& b : badBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") badErr = true;
  }
  check(badErr, "BS-005: an unrecognized %fmt is an honest reject, not a silent accept (D6)");

  // format-display-default-lifecycle: a session that never calls `format`
  // lists each column in the format describe reports for it (the one resolver,
  // SessionState::displayFormat: here the .dta's own format). This arm used to
  // pin the opposite -- NO "format" key, so `list` of a byte or a double never
  // learned its %8.0g/%10.0g (w2p_estim_ev.log:1725/:1793).
  std::string workdir2 = workdir + "_2";
  fs::remove_all(workdir2, ec);
  fs::create_directories(workdir2, ec);
  piet::SessionState session2(workdir2);
  (void)piet::dispatch("sysuse fatalities, clear", session2);
  (void)piet::dispatch("keep in 1/3", session2);
  auto plainListBlocks = piet::dispatch("list year", session2);
  std::string listFmt;
  for (const auto& b : plainListBlocks) {
    if (!b.is_object() || b.value("type", std::string()) != "browse") continue;
    for (const auto& col : b.value("columns", nlohmann::ordered_json::array())) {
      if (col.value("name", std::string()) == "year") listFmt = col.value("format", std::string());
    }
  }
  std::string descFmt;
  for (const auto& b : piet::dispatch("describe year", session2)) {
    if (!b.is_object() || b.value("type", std::string()) != "describe") continue;
    for (const auto& v : b.value("variables", nlohmann::ordered_json::array())) {
      if (v.value("name", std::string()) == "year") descFmt = v.value("format", std::string());
    }
  }
  check(!listFmt.empty() && listFmt == descFmt,
        "BS-005/format-display-default-lifecycle: a session that never calls `format` lists "
        "`year` in the format describe reports (one resolver) -- list [" + listFmt +
            "] describe [" + descFmt + "]");

  fs::remove_all(workdir, ec);
  fs::remove_all(workdir2, ec);
}


// ---------------------------------------------------------------------------
// BETA campaign Phase C batch C3 (macro/program parity): BS-033 gettoken
// 2-arg, BS-034 loop header forms, BS-035 local postfix literal (D10),
// BS-036 do/run echo, BS-037 tempvar, BS-038 scalar list format, BS-039
// ereturn list.

void testC3BS033GettokenTwoArg() {
  std::printf("[selftest] --- C3/BS-033 gettoken 2-arg destructure form ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c3_bs033").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("local line \"alpha beta gamma\"", session);
  auto blocks = piet::dispatch("gettoken tok : line", session);
  bool sawError = false;
  for (const auto& b : blocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") sawError = true;
  }
  check(!sawError, "C3/BS-033: `gettoken tok : line` (2-arg form) no longer syntax-errors");
  check(session.eval.locals.get("tok", "<unset>") == "alpha",
        "C3/BS-033: tok == \"alpha\" (oracle-pinned)");
  check(session.eval.locals.get("line", "<unset>") == "alpha beta gamma",
        "C3/BS-033: line stays UNCHANGED (\"alpha beta gamma\", oracle-pinned -- 2-arg gettoken "
        "is a peek, not a pop)");

  // Regression guard: the 3-arg form is untouched -- its rest keeps the leading
  // blank (BUG-1499; StataBE 19.5: `alpha beta` -> ` beta`).
  (void)piet::dispatch("local line2 \"one two three\"", session);
  (void)piet::dispatch("gettoken first rest : line2", session);
  check(session.eval.locals.get("first", "<unset>") == "one" &&
            session.eval.locals.get("rest", "<unset>") == " two three",
        "C3/BS-033 regression guard: the 3-arg form (`gettoken TOK REST : SRC`) still consumes "
        "the first token from SRC as before");

  fs::remove_all(workdir, ec);
}


void testC3BS035LocalPostfixLiteral() {
  std::printf("[selftest] --- C3/BS-035 `local i++`/`local i--` store the LITERAL text (D10) "
              "---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c3_bs035").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("local i = 7", session);
  (void)piet::dispatch("local i++", session);
  check(session.eval.locals.get("i", "<unset>") == "++",
        "C3/BS-035: `local i++` stores the LITERAL text \"++\" (oracle-pinned, D10 locked "
        "decision) -- previously incremented to \"8\"");

  (void)piet::dispatch("local j = 3", session);
  (void)piet::dispatch("local j--", session);
  check(session.eval.locals.get("j", "<unset>") == "--",
        "C3/BS-035: `local j--` stores the LITERAL text \"--\" (same compat change, decrement "
        "side)");

  // Regression guard: PREFIX increment/decrement is real Stata syntax and
  // stays untouched.
  (void)piet::dispatch("local k = 5", session);
  (void)piet::dispatch("local ++k", session);
  check(session.eval.locals.get("k", "<unset>") == "6",
        "C3/BS-035 regression guard: PREFIX `local ++k` still increments (5 -> 6), unaffected "
        "by the postfix removal");
  (void)piet::dispatch("local --k", session);
  check(session.eval.locals.get("k", "<unset>") == "5",
        "C3/BS-035 regression guard: PREFIX `local --k` still decrements (6 -> 5)");

  fs::remove_all(workdir, ec);
}


// BUG-176/BUG-177 (P2-PREP): the two pre-expansion matchers, reached through
// a macro.
//
// `tempvar`/`tempname`/`tempfile` (dispatcher step c/d) and
// tryCaptureDefinition (step 0a) both match the RAW line, on purpose — but
// macro expansion runs LATER, at step 0b, so a line whose command word
// arrives through a macro (`global L "tempvar t1"` + `capture quietly $L`)
// was invisible to both. Measured before the fix: rc 198 (tempvar hit
// tokenizer.cpp's unconditional "unsupported_macro" keyword net) and rc 199
// (scalar fell past the list/dir-only arm). Step 0b-bis re-applies both
// matchers to the expanded line.
//
// The literal spellings are checked alongside in the SAME session: the fix is
// only correct if it left them behaving exactly as before (testC3BS037Tempvar
// above and pangram.do:163-166 are the wider pins).
void testP2PrepMacroIndirectedDecls() {
  std::printf("[selftest] --- BUG-176/177 macro-indirected tempvar/scalar + encode generate() "
              "---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_p2prep_macroind").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  auto sawError = [](const nlohmann::ordered_json& blocks) {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") return true;
    }
    return false;
  };

  (void)piet::dispatch("sysuse fatalities, clear", session);

  // --- case 1: macro-indirected `tempvar` ---------------------------------
  (void)piet::dispatch("global L \"tempvar t1\"", session);
  check(!sawError(piet::dispatch("$L", session)),
        "BUG-176: `global L \"tempvar t1\"` then `$L` succeeds (was rc 198, "
        "\"unsupported_macro\" — the raw line matched as the literal token `$L`)");
  check(!session.eval.locals.get("t1", "").empty(),
        "BUG-176: the macro-indirected tempvar actually minted a name into `t1'");

  // The literal spelling must be untouched by the re-application.
  check(!sawError(piet::dispatch("tempvar t_literal", session)),
        "BUG-176 non-regression: the LITERAL `tempvar NAME` path still succeeds");
  check(!session.eval.locals.get("t_literal", "").empty(),
        "BUG-176 non-regression: the literal path still mints a name");

  // --- case 2: macro-indirected `scalar` ----------------------------------
  (void)piet::dispatch("global S \"scalar s1 = 42\"", session);
  check(!sawError(piet::dispatch("$S", session)),
        "BUG-176: `global S \"scalar s1 = 42\"` then `$S` succeeds (was rc 199)");
  check(session.eval.scalars.get("s1", "") == "42",
        "BUG-176: the macro-indirected scalar really holds 42, got: " +
            session.eval.scalars.get("s1", "<unset>"));

  check(!sawError(piet::dispatch("scalar s2 = 7", session)) &&
            session.eval.scalars.get("s2", "") == "7",
        "BUG-176 non-regression: the LITERAL `scalar NAME = EXPR` path still captures");

  // A line with NO macro refs must not re-enter step 0b-bis at all; an
  // ordinary command still dispatches normally rather than being swallowed.
  check(!sawError(piet::dispatch("summarize beertax", session)),
        "BUG-176 non-regression: an ordinary macro-free command is unaffected");

  // --- case 3: `encode ..., generate()` -----------------------------------
  // Stata's canonical spelling is `generate`; `gen` is the abbreviation.
  // The option net used to register `gen` AS the canonical, and
  // eval::resolveOptionAbbrev only resolves abbrev -> canonical, so the full
  // spelling was permanently unreachable ("option `generate' not allowed").
  // `jail` is one of fatalities' string columns (2 levels).
  check(!sawError(piet::dispatch("encode jail, generate(jail_full)", session)),
        "BUG-177: `encode jail, generate(NEW)` — Stata's FULL spelling — succeeds "
        "(was \"option `generate' not allowed\")");
  check(session.hasData() && session.datastore()->has("jail_full"),
        "BUG-177: generate() actually created the encoded column");

  (void)piet::dispatch("sysuse fatalities, clear", session);
  check(!sawError(piet::dispatch("encode jail, gen(jail_abbrev)", session)),
        "BUG-177 non-regression: the `gen(NEW)` abbreviation still works");
  check(session.hasData() && session.datastore()->has("jail_abbrev"),
        "BUG-177 non-regression: gen() still creates the encoded column");

  // BUG-337: Stata takes encode's generate()/label() down to ONE character
  // (measured on StataBE 19.5: g/ge/gen/... and l/la/lab/... all rc 0); PIET
  // needed two, because resolveOptionAbbrev has a 2-char floor and encode's
  // vocabulary is a function-local set no minimums row could reach until now.
  // Asserted by CREATION, not by absence-of-error: an encode that silently did
  // nothing would pass an error-only check.
  (void)piet::dispatch("sysuse fatalities, clear", session);
  check(!sawError(piet::dispatch("encode jail, g(jail_g1)", session)) && session.hasData() &&
            session.datastore()->has("jail_g1"),
        "BUG-337: one-char `g()` resolves AND creates the encoded column");
  (void)piet::dispatch("sysuse fatalities, clear", session);
  check(!sawError(piet::dispatch("encode jail, g(jail_g2) l(jail_lbl)", session)) &&
            session.hasData() && session.datastore()->has("jail_g2"),
        "BUG-337: one-char `l()` rides along with it");

  // The net must still REJECT a genuinely unknown option — the fix widens
  // the accepted spellings, it does not disable the reject.
  (void)piet::dispatch("sysuse fatalities, clear", session);
  check(sawError(piet::dispatch("encode jail, bogusopt(z)", session)),
        "BUG-177: an unknown encode option is still honestly rejected");

  fs::remove_all(workdir, ec);
}


void testC3BS038ScalarListFormat() {
  std::printf("[selftest] --- C3/BS-038 `scalar list`/`scalar dir` emit the oracle's column "
              "format ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c3_bs038").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("scalar sca1 = 42", session);
  auto listBlocks = piet::dispatch("scalar list", session);
  bool sawListError = false;
  std::string listText;
  for (const auto& b : listBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") sawListError = true;
    // The listing is a "display" block (BUG-1505); "echo" is the command line.
    if (b.is_object() && b.value("type", std::string()) == "display" && b.contains("text")) {
      listText = b.value("text", std::string());
    }
  }
  check(!sawListError, "C3/BS-038: `scalar list` no longer silently no-ops");
  check(listText.find("      sca1 =         42") != std::string::npos,
        "C3/BS-038: `scalar list` output contains the oracle's exact column-aligned line "
        "(\"      sca1 =         42\"), got: " + listText);

  auto dirBlocks = piet::dispatch("scalar dir", session);
  std::string dirText;
  for (const auto& b : dirBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "display" && b.contains("text")) {
      dirText = b.value("text", std::string());
    }
  }
  check(dirText.find("      sca1 =         42") != std::string::npos,
        "C3/BS-038: `scalar dir` (same sub-arm) gives the SAME formatted output, got: " + dirText);

  fs::remove_all(workdir, ec);
}


void testG_BS062ConfirmTypeCompoundLock() {
  std::printf("[selftest] --- BS-062 lock (Phase G): `confirm numeric variable` / `confirm "
              "string variable` two-word compound ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs062").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen str5 bs062s = \"ok\"", session);

  // `income` is numeric, `bs062s` is string — the MATCHING compound form
  // must succeed silently (empty block array, _rc==0) for BOTH.
  (void)piet::dispatch("capture confirm numeric variable income", session);
  check(session.eval.locals.get("_rc", "") == "0",
        "BS-062(a): `confirm numeric variable income` (income IS numeric) succeeds, _rc==0");

  (void)piet::dispatch("capture confirm string variable bs062s", session);
  check(session.eval.locals.get("_rc", "") == "0",
        "BS-062(b): `confirm string variable bs062s` (bs062s IS a string var) succeeds, _rc==0");

  fs::remove_all(workdir, ec);
}


void testG_BUG369TsInExpressionLock() {
  std::printf("[selftest] --- BUG-369 lock: L./F./D./S. inside EXPRESSIONS "
              "(gen / keep if) and in post-estimation ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug369").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen t = _n", session);
  (void)piet::dispatch("tsset t", session);

  // (a) The idiom the leg-①D probe found broken: an operator on the RHS of gen.
  // Before BUG-369 this returned "unknown variable L" — the expression parser's
  // identifier scan stopped at the dot.
  {
    auto blocks = piet::dispatch("gen ly = L.beertax", session);
    const auto* err = firstBlockOfTypeD1(blocks, "error");
    check(err == nullptr, "BUG-369(a): `gen ly = L.beertax` after `tsset t` does not error");
  }

  // (b) The SAME operator read two ways must agree. `count if L.beertax > 0.5`
  // goes down a different road than the materialised column (handlers_m3.cpp's
  // CiParser bails on the dot and the cobra fallback answers), so equality here
  // is a real cross-check of the wiring, not a restatement of (a).
  std::string cntOp, cntCol;
  {
    auto b1 = piet::dispatch("count if L.beertax > 0.5", session);
    const auto* d1 = firstBlockOfTypeD1(b1, "display");
    if (d1 != nullptr) cntOp = d1->value("text", std::string());
    auto b2 = piet::dispatch("count if ly > 0.5", session);
    const auto* d2 = firstBlockOfTypeD1(b2, "display");
    if (d2 != nullptr) cntCol = d2->value("text", std::string());
  }
  check(!cntOp.empty() && cntOp == cntCol,
        "BUG-369(b): `count if L.beertax>0.5` == `count if ly>0.5` (operator in an `if` and "
        "the same operator materialised by `gen` agree: " + cntOp + " vs " + cntCol + ")");

  // (b2) 2026-08-31 sweep: the SAME predicate through the remaining user-if
  // surfaces. summarize is the sharp one — its if-prefilter runs against a §38
  // column-fetch PROJECTION, and the projection has to carry the tsset time
  // column that appears in no expression text (column_fetch.cpp's TS-aware
  // referencedColumns). N here must agree with the count above.
  {
    auto blocks = piet::dispatch("summarize beertax if L.beertax > 0.5", session);
    const auto* sm = firstBlockOfTypeD1(blocks, "summarize");
    long long n = -1;
    if (sm != nullptr && sm->contains("stats") && (*sm)["stats"].contains("beertax"))
      n = (*sm)["stats"]["beertax"].value("N", -1LL);
    check(!cntOp.empty() && n == std::stoll(cntOp),
          "BUG-369(b2): `summarize beertax if L.beertax>0.5` runs off the projection and its "
          "N equals the count (" + std::to_string(n) + " vs " + cntOp + ")");
  }
  {
    auto blocks = piet::dispatch("codebook beertax if L.beertax > 0.5", session);
    check(firstBlockOfTypeD1(blocks, "error") == nullptr,
          "BUG-369(b3): `codebook ... if L.beertax>0.5` does not error (dispatcher if-prefilter "
          "carries the tsset context — representative of the table/list/graph arms)");
  }
  {
    auto blocks = piet::dispatch("egen __b369mb = mean(L.beertax)", session);
    check(firstBlockOfTypeD1(blocks, "error") == nullptr,
          "BUG-369(b4): `egen m = mean(L.beertax)` (no if) does not error — whole-frame "
          "evaluation, E1 holds by construction");
  }
  {
    auto blocks = piet::dispatch("egen __b369bad = mean(L.beertax) if beertax > 0", session);
    check(firstBlockOfTypeD1(blocks, "error") != nullptr,
          "BUG-369(b5): `egen ... = mean(L.y) if ...` honestly REJECTS — the if-path computes "
          "on a row-filtered copy, where a TS operator would be silently wrong (E1 guard)");
  }

  // (c) Post-estimation refits re-parse the ORIGINAL varlist, so they need the
  // same context the fit got. `regress y L.x` fitted and `test L.x` on that very
  // model then died with ts_no_tsset.
  (void)piet::dispatch("regress fatal beertax L.beertax", session);
  {
    auto blocks = piet::dispatch("test L.beertax", session);
    const auto* err = firstBlockOfTypeD1(blocks, "error");
    check(err == nullptr, "BUG-369(c): `test L.beertax` after `regress fatal beertax L.beertax` "
                          "does not error (post-estimation refit carries the tsset context)");
  }

  // (d) BUG-367: `testparm` is `test`'s varlist spelling, so the two must return
  // the SAME F. A testparm that quietly tested something else would pass a
  // "does it run" check and fail this one.
  {
    auto tb = piet::dispatch("test beertax L.beertax", session);
    auto pb = piet::dispatch("testparm beertax L.beertax", session);
    const auto* te = firstBlockOfTypeD1(tb, "estimation");
    const auto* pe = firstBlockOfTypeD1(pb, "estimation");
    bool same = te != nullptr && pe != nullptr && te->contains("F") && pe->contains("F") &&
                (*te)["F"] == (*pe)["F"] && (*te)["df1"] == (*pe)["df1"];
    check(same, "BUG-367: `testparm beertax L.beertax` returns the identical F/df1 as "
                "`test beertax L.beertax` (one command, two spellings)");
  }

  // (e) An unmatched name is an honest refusal, not an empty test of nothing.
  {
    auto blocks = piet::dispatch("testparm nosuchvar", session);
    const auto* err = firstBlockOfTypeD1(blocks, "error");
    check(err != nullptr, "BUG-367: `testparm nosuchvar` refuses rather than testing an empty "
                          "restriction set");
  }

  fs::remove_all(workdir, ec);
}


void testG_BUG374DisplayLayerLock() {
  std::printf("[selftest] --- BUG-374 lock: the display layer — string r() as a LITERAL, "
              "missing as `.`, out-of-domain math as missing ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug374").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);

  auto disp = [&session](const std::string& expr) {
    auto b = piet::dispatch("display " + expr, session);
    const auto* d = firstBlockOfTypeD1(b, "display");
    if (d == nullptr) {
      const auto* e = firstBlockOfTypeD1(b, "error");
      return std::string("<ERROR: ") + (e ? e->value("message", std::string()) : "?") + ">";
    }
    return d->value("text", std::string());
  };

  // (1) A missing RESULT prints as Stata's `.`, not Python's "nan". These reach
  // the display path as a genuine NaN and used to fall through to
  // pythonFloatRepr, which is faithfully Python — while the `` `=expr' `` twin in
  // the same file always printed ".". The two entry points disagreed.
  check(disp("mod(7,0)") == ".",
        "BUG-374(2): `display mod(7,0)` prints `.` (Stata missing), not \"nan\"");

  // (2) Out-of-domain math is MISSING in Stata. This was reported as a parse gap
  // and is not one: the grammar handles unary minus, the CALL threw Python's
  // ValueError, and only NameError/r(NNN) were forwarded — so display classified
  // an empty error as r(198) "invalid syntax".
  check(disp("sqrt(-1)") == ".",
        "BUG-374(3): `display sqrt(-1)` is MISSING, not r(198) invalid syntax");
  check(disp("ln(0)") == ".", "BUG-374(3): `display ln(0)` is missing too");
  // The domain fix must not swallow a legitimate value.
  check(disp("sqrt(4)") == "2" && disp("2+2") == "4",
        "BUG-374(3): ordinary arithmetic is untouched (sqrt(4)=2, 2+2=4)");

  // (3) A STRING r-class result reached through the BARE form is a VALUE
  // reference and must arrive as a string LITERAL. Spliced raw it was
  // re-evaluated, which failed loudly on characters the arithmetic grammar
  // rejects — and could go SILENT, which is the worse half.
  (void)piet::dispatch("datasignature", session);
  const std::string sig = disp("r(datasignature)");
  check(sig.find(':') != std::string::npos && sig.rfind("<ERROR", 0) != 0,
        "BUG-374(1): `display r(datasignature)` prints the signature instead of erroring on its "
        "own colons and parentheses (got: " + sig + ")");

  // THE SILENT CASE, and the reason this is not merely cosmetic. `ds beertax`
  // sets r(varlist) to a single NAME. Spliced raw, that name was re-read as a
  // DATASET VARIABLE and display printed that column's first observation — a
  // wrong number with no error anywhere. It must print the NAME.
  {
    auto b = piet::dispatch("ds beertax", session);
    check(firstBlockOfTypeD1(b, "error") == nullptr, "BUG-374(1): `ds beertax` runs");
    const std::string vl = disp("r(varlist)");
    check(vl == "beertax",
          "BUG-374(1): `display r(varlist)` prints the NAME `beertax`, not the first observation "
          "of the column of that name (got: " + vl + ")");
  }

  // The DELIMITED form is macro EXPANSION and must stay raw — that is how a
  // varlist gets spliced into a command, and quoting it would break every such
  // use. Both spellings shared one arm before; they must not now.
  {
    (void)piet::dispatch("gen __g374 = mod(_n,3)", session);
    (void)piet::dispatch("levelsof __g374", session);
    auto b = piet::dispatch("display \"x: `r(levels)'\"", session);
    const auto* d = firstBlockOfTypeD1(b, "display");
    check(d != nullptr && d->value("text", std::string()).rfind("x: ", 0) == 0 &&
              d->value("text", std::string()).find(' ') != std::string::npos,
          "BUG-374(1): the `r(name)' macro-expansion form still substitutes RAW inside a string");
  }

  fs::remove_all(workdir, ec);
}


void testG_BS094ModNativeEvaluatorLock() {
  std::printf("[selftest] --- BS-094 lock (Phase G): mod() in PIET's native "
              "(evalArith/pyeval) expression evaluator ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs094").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  // BS-094 (ADJUDICATED, Secretary 2026-07-12): the conflict is two
  // layers — `display`/`scalar`'s numeric-expression path (macros.cpp's
  // evalArith, which internally builds a pyeval PyNamespace via
  // mathNamespace()+addStataFns() — the SAME registry exprfns.cpp's
  // ns.fns["mod"] lives in) genuinely supports mod(); cobra-side `gen ...
  // = mod(...)` (forwarded to `cobra eval --gen`, a DIFFERENT expression
  // grammar with no mod() of its own) does NOT and stays an honest reject
  // (BUG-038b, side queue — see the pre-existing "function mod() is not
  // supported" leg above). This locks the native (`display`) path ONLY.
  auto blocks = piet::dispatch("display mod(7,3)", session);
  std::string text;
  for (const auto& b : blocks) {
    if (b.value("type", "") == "display") text = b.value("text", std::string());
  }
  double val = -999.0;
  try {
    val = std::stod(text);
  } catch (...) {
  }
  check(std::abs(val - 1.0) < 1e-9,
        "BS-094: `display mod(7,3)` == 1 through PIET's native evaluator, got '" + text + "'");

  // Sign-of-divisor convention (Stata semantics, same as fnMod's own
  // direct-call unit check earlier in this file): mod(-7,3) == 2, not -1.
  auto blocks2 = piet::dispatch("display mod(-7,3)", session);
  std::string text2;
  for (const auto& b : blocks2) {
    if (b.value("type", "") == "display") text2 = b.value("text", std::string());
  }
  double val2 = -999.0;
  try {
    val2 = std::stod(text2);
  } catch (...) {
  }
  check(std::abs(val2 - 2.0) < 1e-9,
        "BS-094: `display mod(-7,3)` == 2 (sign of divisor), got '" + text2 + "'");

  fs::remove_all(workdir, ec);
}


// BUG-115: the if/in/weight qualifier scanner (tokenizer.cpp's
// extractQualifiers, twoway_parse.cpp's extractIfIn, dispatcher.cpp's
// bysort-gen if-extraction) ran quote-blind — a double-quoted literal whose
// text happened to contain " in "/" if " was mis-split off as a real
// qualifier. Two independently-observable failure shapes, both fixed by the
// same eval::searchUnquoted mechanism (tokenizer.h): an HONEST-but-wrong
// reject (any command, `in`-shaped text at the end of a quoted literal) and
// a SILENT-wrong break (any `if`-consuming command, `if`-shaped text mid-
// literal feeds a garbage condition to the engine).
void testBUG115QuoteAwareQualifierLock() {
  std::printf(
      "[selftest] --- BUG-115 lock: quote-aware if/in qualifier splitter ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug115").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  // Repro (BUGLOG BUG-115): a trailing quoted literal ending in " in X" must
  // not be mistaken for the `in` row-range qualifier — `display` doesn't
  // support `in` at all, so the pre-fix code rejected this outright.
  {
    auto blocks = piet::dispatch("display \"in failprog\"", session);
    std::string text;
    bool sawError = false;
    for (const auto& b : blocks) {
      if (b.value("type", std::string()) == "display") text = b.value("text", std::string());
      if (b.value("type", std::string()) == "error") sawError = true;
    }
    check(!sawError && text == "in failprog",
          "BUG-115: `display \"in failprog\"` prints the literal string, not an "
          "unsupported `in` row-range rejection — got text='" + text +
          "' sawError=" + (sawError ? "true" : "false"));
  }

  (void)piet::dispatch("sysuse fatalities, clear", session);

  // "if" twin: a quoted literal containing " if " must not be mis-split into
  // a real if-condition for a command that actually consumes ifExpr (`gen`)
  // — pre-fix, this silently fed the garbage tail as a condition and the
  // generate failed with "if condition evaluation failed".
  {
    auto blocks = piet::dispatch("gen str30 bug115note = \"check if this works\"", session);
    bool sawError = false;
    for (const auto& b : blocks) {
      if (b.value("type", std::string()) == "error") sawError = true;
    }
    check(!sawError,
          "BUG-115: `gen ... = \"check if this works\"` does not mis-split the "
          "quoted `if` into a bogus condition");
    auto listBlocks = piet::dispatch("list bug115note in 1/1", session);
    std::string cellText;
    for (const auto& b : listBlocks) {
      if (b.value("type", std::string()) == "browse") {
        const auto& rows = b.value("rows", nlohmann::ordered_json::array());
        if (!rows.empty() && !rows[0].empty()) cellText = rows[0][0].get<std::string>();
      }
    }
    check(cellText == "check if this works",
          "BUG-115: the generated string is the full literal, unsplit — got '" + cellText + "'");
  }

  // Regression guard: legitimate (unquoted) `if`/`in` qualifiers on the same
  // dataset still work exactly as before — the fix must only change
  // in-quote matches, not real qualifier extraction.
  {
    auto blocks = piet::dispatch("summarize beertax if state==\"al\"", session);
    const auto* sm = firstBlockOfTypeD1(blocks, "summarize");
    check(sm != nullptr && sm->at("stats").at("beertax").value("N", -1.0) == 7.0,
          "BUG-115 regression guard: `summarize beertax if state==\"al\"` still if-filters "
          "correctly (N==7)");
  }
  {
    auto blocks = piet::dispatch("summarize beertax in 1/10", session);
    const auto* sm = firstBlockOfTypeD1(blocks, "summarize");
    check(sm != nullptr && sm->at("stats").at("beertax").value("N", -1.0) == 10.0,
          "BUG-115 regression guard: `summarize beertax in 1/10` still in-range-filters "
          "correctly (N==10)");
  }

  fs::remove_all(workdir, ec);
}


// --- BUG-352: a compound-quoted string is not a macro reference --------------
// The macro pre-filter searched the RAW line with `[^']*', which matches Stata's
// compound double quotes `"..."' from the backtick to the first apostrophe — so a
// construct StataBE 19.5 accepts at rc 0 was rejected as a macro and stopped the
// do-file. Masking the compound span removes the WRONG diagnosis; generate/replace
// and `if` qualifiers now read the literal (BUG-352 round 2), and what is still
// unread (a nested compound quote there) is named instead of failing deeper under
// "expression evaluation failed at obs 1".
//
// The three regression arms matter as much as the fix: masking must not blind the
// pre-filter to REAL macros, and must not touch plain strings.
void testBUG352CompoundQuotes() {
  std::printf("[selftest] --- BUG-352: compound double quotes are named, not mis-blamed ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug352").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto firstErr = [](const nlohmann::ordered_json& blocks) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error")
        return b.value("message", std::string());
    return std::string();
  };
  auto has = [](const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
  };

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);

  // EM3b/EM4 (w2af_macro_text_file.log :955, :969): a compound-quoted literal is now
  // READ -- plain (`"plain compound"' is "plain compound") and with an inner quote
  // (display prints the body verbatim). The named refusal stays for the
  // engine-routed commands whose lexer has no such literal, at r(198).
  auto displayText = [](const nlohmann::ordered_json& blocks) {
    std::string t;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "display") t += b.value("text", std::string());
    return t;
  };
  const std::string cqPlain = displayText(piet::dispatch("display `\"plain compound\"'", session));
  check(cqPlain == "plain compound",
        "BUG-352/EM3b: `display `\"plain compound\"'` prints its body -- got: " + cqPlain);
  const std::string cqInner = displayText(piet::dispatch("display `\"say \"hi\" now\"'", session));
  check(cqInner == "say \"hi\" now",
        "BUG-352/EM4: a compound literal holding a quote prints verbatim -- got: " + cqInner);
  // generate/replace and every `if` qualifier now read the literal too (the tokenizer
  // hands the engine "has " + char(34) + ...): StataBE 19.5 gives rc 0, strlen 18.
  const std::string cq =
      firstErr(piet::dispatch("gen str30 c1 = `\"has \"inner\" quotes\"'", session));
  check(cq.empty(), "BUG-352: gen of a compound literal holding a quote is rc 0 -- got: " +
                        cq.substr(0, 70));
  const std::string cqLen = displayText(piet::dispatch("display strlen(c1[1])", session));
  check(cqLen == "18", "BUG-352: the generated value is the literal's body (strlen 18) -- got: " +
                           cqLen);
  // A nested compound quote is not rewritten: the engine-routed reject still NAMES the
  // construct instead of blaming a macro.
  const std::string cqNest =
      firstErr(piet::dispatch("gen str30 c2 = `\"a `\"b\"' c\"'", session));
  check(has(cqNest, "compound double quotes"),
        "BUG-352: the engine-routed reject NAMES the construct instead of blaming a macro — got: " +
            cqNest.substr(0, 70));
  check(!has(cqNest, "does not expand macros"),
        "BUG-352: the wrong diagnosis is gone (it is not a macro, and PIET does expand macros)");

  // Regression 1: an undefined macro must STILL be rejected — masking must not
  // blind the pre-filter to the thing it exists for.
  (void)piet::dispatch("capture count if beertax > `nosuchmacro'", session);
  std::string rc;
  for (const auto& b : piet::dispatch("display _rc", session))
    if (b.is_object() && b.value("type", std::string()) != "echo") rc += b.value("text", std::string());
  check(rc.find('0') != 0,
        "BUG-352 regression: an undefined `local' still rejects (rc=" + rc + ")");

  // Regression 2: a DEFINED macro must still expand and answer.
  (void)piet::dispatch("local thr = .5", session);
  std::string cnt;
  for (const auto& b : piet::dispatch("count if beertax > `thr'", session))
    if (b.is_object() && b.value("type", std::string()) == "display") cnt = b.value("text", std::string());
  check(!cnt.empty() && cnt != "0",
        "BUG-352 regression: a DEFINED macro still expands and answers (count=" + cnt + ")");

  // Regression 3: plain strings are untouched.
  (void)piet::dispatch("gen str10 p1 = \"plain\"", session);
  std::string cnt2;
  for (const auto& b : piet::dispatch("count if p1 == \"plain\"", session))
    if (b.is_object() && b.value("type", std::string()) == "display") cnt2 = b.value("text", std::string());
  check(cnt2 == "336", "BUG-352 regression: plain \"...\" strings are unaffected (got " + cnt2 + ")");

  fs::remove_all(workdir, ec);
}


// ---------------------------------------------------------------------------
// BUG-195 / BUG-192 clause 4 / BUG-196 — the Stata numeric-format battery.
//
// PIET had TWO hand-written renderers for `%w.Pg` and they had drifted: BUG-192
// fixed the `display` one and left eval/exprfns.cpp's applyStataFmt — the one
// serving `string(x, "fmt")` and `tostring VAR, usedisplayformat` — still
// handing the spec to printf, whose `%.0g` means "zero significant digits".
// That twin writes into a string VARIABLE, i.e. stored data that survives save
// and export, so `tostring` on a %9.0g column wrote `4e+01` where Stata writes
// `42`. There is one renderer now (piet/stata_format.h) and this arm exercises
// BOTH call paths against the same rows.
//
// EVERY expectation below is VERBATIM from a live StataBE 19.5 capture that is
// kept internally — capture fmt_g_oracle.{do,log}
// (9 specs x 19 values x 3 surfaces) and fmt_g_oracle_neg.{do,log} (the same
// specs x 15 negatives, added because the original 19 values had no negative
// long enough to catch the clause-4 defect). BUG-192's own capture was LOST,
// which is how its rule spent a release with no on-disk authority behind it;
// re-deriving these numbers from printf or from the manual's prose is exactly
// how the defect got in, so they are copied, never computed.
//
// Two columns per row, because the two layers legitimately differ in one way
// and one way only: `display` right-justifies into the field, string()/tostring
// do not. That is measured, not assumed — over all 306 value-spec pairs the
// capture's string() and tostring sections are the display section with the
// padding stripped, with no exceptions.
void testBUG195FormatOracleBothPaths() {
  std::printf("[selftest] --- BUG-195: one Stata format renderer, both call paths ---\n");

  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_fmt_oracle").u8string();
  std::error_code ec;
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  auto runCmd = [](piet::SessionState& s, const std::string& cmd) {
    nlohmann::json body{{"cmd", cmd}};
    return nlohmann::json::parse(piet::routes::command(s, body.dump()).json);
  };
  auto dispText = [&](const std::string& cmd) -> std::string {
    for (const auto& b : runCmd(session, cmd).value("blocks", nlohmann::json::array())) {
      if (b.value("type", std::string()) == "display") return b.value("text", std::string());
    }
    return std::string("<no display block>");
  };

  struct FmtRow {
    const char* spec;
    const char* literal;  // source text, so the display leg goes through the parser
    double value;         // the same number, for the string()/tostring leg
    const char* wantDisplay;
    const char* wantValue;
  };
  static const FmtRow kRows[] = {
      {"%9.0g", "0", 0, "        0", "0"},
      {"%9.0g", "42", 42, "       42", "42"},
      {"%9.0g", "42.5", 42.5, "     42.5", "42.5"},
      {"%9.0g", "-42.5", -42.5, "    -42.5", "-42.5"},
      {"%9.0g", "2.5", 2.5, "      2.5", "2.5"},
      {"%9.0g", ".5", 0.5, "       .5", ".5"},
      {"%9.0g", ".1", 0.1, "       .1", ".1"},
      {"%9.0g", "-.0004", -0.0004, "   -.0004", "-.0004"},
      {"%9.0g", ".000012345", 0.000012345, " .0000123", ".0000123"},
      {"%9.0g", "1e-20", 1e-20, " 1.00e-20", "1.00e-20"},
      {"%9.0g", "123456.789", 123456.789, " 123456.8", "123456.8"},
      {"%9.0g", "-123456.789", -123456.789, "-123456.8", "-123456.8"},
      {"%9.0g", "1000000", 1000000, "  1000000", "1000000"},
      {"%9.0g", "99999999", 99999999, " 1.00e+08", "1.00e+08"},
      {"%9.0g", "1234567890", 1234567890, " 1.23e+09", "1.23e+09"},
      {"%9.0g", "12345678901234", 12345678901234, " 1.23e+13", "1.23e+13"},
      {"%9.0g", "1e20", 1e20, " 1.00e+20", "1.00e+20"},
      {"%9.0g", "3.14159265358979", 3.14159265358979, " 3.141593", "3.141593"},
      {"%12.0g", "0", 0, "           0", "0"},
      {"%12.0g", "42", 42, "          42", "42"},
      {"%12.0g", "42.5", 42.5, "        42.5", "42.5"},
      {"%12.0g", "-42.5", -42.5, "       -42.5", "-42.5"},
      {"%12.0g", "2.5", 2.5, "         2.5", "2.5"},
      {"%12.0g", ".5", 0.5, "          .5", ".5"},
      {"%12.0g", ".1", 0.1, "          .1", ".1"},
      {"%12.0g", "-.0004", -0.0004, "      -.0004", "-.0004"},
      {"%12.0g", ".000012345", 0.000012345, "  .000012345", ".000012345"},
      {"%12.0g", "1e-20", 1e-20, " 1.00000e-20", "1.00000e-20"},
      {"%12.0g", "123456.789", 123456.789, "  123456.789", "123456.789"},
      {"%12.0g", "-123456.789", -123456.789, " -123456.789", "-123456.789"},
      {"%12.0g", "1000000", 1000000, "     1000000", "1000000"},
      {"%12.0g", "99999999", 99999999, "    99999999", "99999999"},
      {"%12.0g", "1234567890", 1234567890, "  1234567890", "1234567890"},
      {"%12.0g", "12345678901234", 12345678901234, " 1.23457e+13", "1.23457e+13"},
      {"%12.0g", "1e20", 1e20, " 1.00000e+20", "1.00000e+20"},
      {"%12.0g", "3.14159265358979", 3.14159265358979, " 3.141592654", "3.141592654"},
      {"%8.0g", "0", 0, "       0", "0"},
      {"%8.0g", "42", 42, "      42", "42"},
      {"%8.0g", "42.5", 42.5, "    42.5", "42.5"},
      {"%8.0g", "-42.5", -42.5, "   -42.5", "-42.5"},
      {"%8.0g", "2.5", 2.5, "     2.5", "2.5"},
      {"%8.0g", ".5", 0.5, "      .5", ".5"},
      {"%8.0g", ".1", 0.1, "      .1", ".1"},
      {"%8.0g", "-.0004", -0.0004, "  -.0004", "-.0004"},
      {"%8.0g", ".000012345", 0.000012345, " .000012", ".000012"},
      {"%8.0g", "1e-20", 1e-20, " 1.0e-20", "1.0e-20"},
      {"%8.0g", "123456.789", 123456.789, "  123457", "123457"},
      {"%8.0g", "-123456.789", -123456.789, " -123457", "-123457"},
      {"%8.0g", "1000000", 1000000, " 1.0e+06", "1.0e+06"},
      {"%8.0g", "99999999", 99999999, " 1.0e+08", "1.0e+08"},
      {"%8.0g", "1234567890", 1234567890, " 1.2e+09", "1.2e+09"},
      {"%8.0g", "12345678901234", 12345678901234, " 1.2e+13", "1.2e+13"},
      {"%8.0g", "1e20", 1e20, " 1.0e+20", "1.0e+20"},
      {"%8.0g", "3.14159265358979", 3.14159265358979, " 3.14159", "3.14159"},
      {"%10.0g", "0", 0, "         0", "0"},
      {"%10.0g", "42", 42, "        42", "42"},
      {"%10.0g", "42.5", 42.5, "      42.5", "42.5"},
      {"%10.0g", "-42.5", -42.5, "     -42.5", "-42.5"},
      {"%10.0g", "2.5", 2.5, "       2.5", "2.5"},
      {"%10.0g", ".5", 0.5, "        .5", ".5"},
      {"%10.0g", ".1", 0.1, "        .1", ".1"},
      {"%10.0g", "-.0004", -0.0004, "    -.0004", "-.0004"},
      {"%10.0g", ".000012345", 0.000012345, " .00001234", ".00001234"},
      {"%10.0g", "1e-20", 1e-20, " 1.000e-20", "1.000e-20"},
      {"%10.0g", "123456.789", 123456.789, " 123456.79", "123456.79"},
      {"%10.0g", "-123456.789", -123456.789, "-123456.79", "-123456.79"},
      {"%10.0g", "1000000", 1000000, "   1000000", "1000000"},
      {"%10.0g", "99999999", 99999999, "  99999999", "99999999"},
      {"%10.0g", "1234567890", 1234567890, " 1.235e+09", "1.235e+09"},
      {"%10.0g", "12345678901234", 12345678901234, " 1.235e+13", "1.235e+13"},
      {"%10.0g", "1e20", 1e20, " 1.000e+20", "1.000e+20"},
      {"%10.0g", "3.14159265358979", 3.14159265358979, " 3.1415927", "3.1415927"},
      {"%9.3g", "0", 0, "        0", "0"},
      {"%9.3g", "42", 42, "       42", "42"},
      {"%9.3g", "42.5", 42.5, "     42.5", "42.5"},
      {"%9.3g", "-42.5", -42.5, "    -42.5", "-42.5"},
      {"%9.3g", "2.5", 2.5, "      2.5", "2.5"},
      {"%9.3g", ".5", 0.5, "       .5", ".5"},
      {"%9.3g", ".1", 0.1, "       .1", ".1"},
      {"%9.3g", "-.0004", -0.0004, "   -.0004", "-.0004"},
      {"%9.3g", ".000012345", 0.000012345, " .0000123", ".0000123"},
      {"%9.3g", "1e-20", 1e-20, " 1.00e-20", "1.00e-20"},
      {"%9.3g", "123456.789", 123456.789, "   123457", "123457"},
      {"%9.3g", "-123456.789", -123456.789, "  -123457", "-123457"},
      {"%9.3g", "1000000", 1000000, "  1000000", "1000000"},
      {"%9.3g", "99999999", 99999999, " 1.00e+08", "1.00e+08"},
      {"%9.3g", "1234567890", 1234567890, " 1.23e+09", "1.23e+09"},
      {"%9.3g", "12345678901234", 12345678901234, " 1.23e+13", "1.23e+13"},
      {"%9.3g", "1e20", 1e20, " 1.00e+20", "1.00e+20"},
      {"%9.3g", "3.14159265358979", 3.14159265358979, "     3.14", "3.14"},
      {"%9.2g", "0", 0, "        0", "0"},
      {"%9.2g", "42", 42, "       42", "42"},
      {"%9.2g", "42.5", 42.5, "       43", "43"},
      {"%9.2g", "-42.5", -42.5, "      -43", "-43"},
      {"%9.2g", "2.5", 2.5, "      2.5", "2.5"},
      {"%9.2g", ".5", 0.5, "       .5", ".5"},
      {"%9.2g", ".1", 0.1, "       .1", ".1"},
      {"%9.2g", "-.0004", -0.0004, "   -.0004", "-.0004"},
      {"%9.2g", ".000012345", 0.000012345, "  .000012", ".000012"},
      {"%9.2g", "1e-20", 1e-20, "  1.0e-20", "1.0e-20"},
      {"%9.2g", "123456.789", 123456.789, "   123457", "123457"},
      {"%9.2g", "-123456.789", -123456.789, "  -123457", "-123457"},
      {"%9.2g", "1000000", 1000000, "  1000000", "1000000"},
      {"%9.2g", "99999999", 99999999, "  1.0e+08", "1.0e+08"},
      {"%9.2g", "1234567890", 1234567890, "  1.2e+09", "1.2e+09"},
      {"%9.2g", "12345678901234", 12345678901234, "  1.2e+13", "1.2e+13"},
      {"%9.2g", "1e20", 1e20, "  1.0e+20", "1.0e+20"},
      {"%9.2g", "3.14159265358979", 3.14159265358979, "      3.1", "3.1"},
      {"%12.4g", "0", 0, "           0", "0"},
      {"%12.4g", "42", 42, "          42", "42"},
      {"%12.4g", "42.5", 42.5, "        42.5", "42.5"},
      {"%12.4g", "-42.5", -42.5, "       -42.5", "-42.5"},
      {"%12.4g", "2.5", 2.5, "         2.5", "2.5"},
      {"%12.4g", ".5", 0.5, "          .5", ".5"},
      {"%12.4g", ".1", 0.1, "          .1", ".1"},
      {"%12.4g", "-.0004", -0.0004, "      -.0004", "-.0004"},
      {"%12.4g", ".000012345", 0.000012345, "   .00001234", ".00001234"},
      {"%12.4g", "1e-20", 1e-20, "   1.000e-20", "1.000e-20"},
      {"%12.4g", "123456.789", 123456.789, "      123457", "123457"},
      {"%12.4g", "-123456.789", -123456.789, "     -123457", "-123457"},
      {"%12.4g", "1000000", 1000000, "     1000000", "1000000"},
      {"%12.4g", "99999999", 99999999, "    99999999", "99999999"},
      {"%12.4g", "1234567890", 1234567890, "  1234567890", "1234567890"},
      {"%12.4g", "12345678901234", 12345678901234, "   1.235e+13", "1.235e+13"},
      {"%12.4g", "1e20", 1e20, "   1.000e+20", "1.000e+20"},
      {"%12.4g", "3.14159265358979", 3.14159265358979, "       3.142", "3.142"},
      {"%20.0g", "0", 0, "                   0", "0"},
      {"%20.0g", "42", 42, "                  42", "42"},
      {"%20.0g", "42.5", 42.5, "                42.5", "42.5"},
      {"%20.0g", "-42.5", -42.5, "               -42.5", "-42.5"},
      {"%20.0g", "2.5", 2.5, "                 2.5", "2.5"},
      {"%20.0g", ".5", 0.5, "                  .5", ".5"},
      {"%20.0g", ".1", 0.1, " .100000000000000006", ".100000000000000006"},
      {"%20.0g", "-.0004", -0.0004, "              -.0004", "-.0004"},
      {"%20.0g", ".000012345", 0.000012345, "          .000012345", ".000012345"},
      {"%20.0g", "1e-20", 1e-20, " 1.0000000000000e-20", "1.0000000000000e-20"},
      {"%20.0g", "123456.789", 123456.789, " 123456.789000000004", "123456.789000000004"},
      {"%20.0g", "-123456.789", -123456.789, "-123456.789000000004", "-123456.789000000004"},
      {"%20.0g", "1000000", 1000000, "             1000000", "1000000"},
      {"%20.0g", "99999999", 99999999, "            99999999", "99999999"},
      {"%20.0g", "1234567890", 1234567890, "          1234567890", "1234567890"},
      {"%20.0g", "12345678901234", 12345678901234, "      12345678901234", "12345678901234"},
      {"%20.0g", "1e20", 1e20, " 1.0000000000000e+20", "1.0000000000000e+20"},
      {"%20.0g", "3.14159265358979", 3.14159265358979, " 3.14159265358979001", "3.14159265358979001"},
      {"%9.0e", "0", 0, " 0.00e+00", "0.00e+00"},
      {"%9.0e", "42", 42, " 4.20e+01", "4.20e+01"},
      {"%9.0e", "42.5", 42.5, " 4.25e+01", "4.25e+01"},
      {"%9.0e", "-42.5", -42.5, "-4.25e+01", "-4.25e+01"},
      {"%9.0e", "2.5", 2.5, " 2.50e+00", "2.50e+00"},
      {"%9.0e", ".5", 0.5, " 5.00e-01", "5.00e-01"},
      {"%9.0e", ".1", 0.1, " 1.00e-01", "1.00e-01"},
      {"%9.0e", "-.0004", -0.0004, "-4.00e-04", "-4.00e-04"},
      {"%9.0e", ".000012345", 0.000012345, " 1.23e-05", "1.23e-05"},
      {"%9.0e", "1e-20", 1e-20, " 1.00e-20", "1.00e-20"},
      {"%9.0e", "123456.789", 123456.789, " 1.23e+05", "1.23e+05"},
      {"%9.0e", "-123456.789", -123456.789, "-1.23e+05", "-1.23e+05"},
      {"%9.0e", "1000000", 1000000, " 1.00e+06", "1.00e+06"},
      {"%9.0e", "99999999", 99999999, " 1.00e+08", "1.00e+08"},
      {"%9.0e", "1234567890", 1234567890, " 1.23e+09", "1.23e+09"},
      {"%9.0e", "12345678901234", 12345678901234, " 1.23e+13", "1.23e+13"},
      {"%9.0e", "1e20", 1e20, " 1.00e+20", "1.00e+20"},
      {"%9.0e", "3.14159265358979", 3.14159265358979, " 3.14e+00", "3.14e+00"},
      {"%9.0g", "-42", -42, "      -42", "-42"},
      {"%9.0g", "-2.5", -2.5, "     -2.5", "-2.5"},
      {"%9.0g", "-.5", -0.5, "      -.5", "-.5"},
      {"%9.0g", "-.1", -0.1, "      -.1", "-.1"},
      {"%9.0g", "-.000012345", -0.000012345, "-.0000123", "-.0000123"},
      {"%9.0g", "-1e-20", -1e-20, "-1.00e-20", "-1.00e-20"},
      {"%9.0g", "-1000000", -1000000, " -1000000", "-1000000"},
      {"%9.0g", "-9999999", -9999999, " -9999999", "-9999999"},
      {"%9.0g", "-99999999", -99999999, "-1.00e+08", "-1.00e+08"},
      {"%9.0g", "-1234567890", -1234567890, "-1.23e+09", "-1.23e+09"},
      {"%9.0g", "-12345678901234", -12345678901234, "-1.23e+13", "-1.23e+13"},
      {"%9.0g", "-1e20", -1e20, "-1.00e+20", "-1.00e+20"},
      {"%9.0g", "-3.14159265358979", -3.14159265358979, "-3.141593", "-3.141593"},
      {"%9.0g", "-123456.789", -123456.789, "-123456.8", "-123456.8"},
      {"%9.0g", "-1234567.89", -1234567.89, " -1234568", "-1234568"},
      {"%12.0g", "-42", -42, "         -42", "-42"},
      {"%12.0g", "-2.5", -2.5, "        -2.5", "-2.5"},
      {"%12.0g", "-.5", -0.5, "         -.5", "-.5"},
      {"%12.0g", "-.1", -0.1, "         -.1", "-.1"},
      {"%12.0g", "-.000012345", -0.000012345, " -.000012345", "-.000012345"},
      {"%12.0g", "-1e-20", -1e-20, "-1.00000e-20", "-1.00000e-20"},
      {"%12.0g", "-1000000", -1000000, "    -1000000", "-1000000"},
      {"%12.0g", "-9999999", -9999999, "    -9999999", "-9999999"},
      {"%12.0g", "-99999999", -99999999, "   -99999999", "-99999999"},
      {"%12.0g", "-1234567890", -1234567890, " -1234567890", "-1234567890"},
      {"%12.0g", "-12345678901234", -12345678901234, "-1.23457e+13", "-1.23457e+13"},
      {"%12.0g", "-1e20", -1e20, "-1.00000e+20", "-1.00000e+20"},
      {"%12.0g", "-3.14159265358979", -3.14159265358979, "-3.141592654", "-3.141592654"},
      {"%12.0g", "-123456.789", -123456.789, " -123456.789", "-123456.789"},
      {"%12.0g", "-1234567.89", -1234567.89, " -1234567.89", "-1234567.89"},
      {"%8.0g", "-42", -42, "     -42", "-42"},
      {"%8.0g", "-2.5", -2.5, "    -2.5", "-2.5"},
      {"%8.0g", "-.5", -0.5, "     -.5", "-.5"},
      {"%8.0g", "-.1", -0.1, "     -.1", "-.1"},
      {"%8.0g", "-.000012345", -0.000012345, "-.000012", "-.000012"},
      {"%8.0g", "-1e-20", -1e-20, "-1.0e-20", "-1.0e-20"},
      {"%8.0g", "-1000000", -1000000, "-1.0e+06", "-1.0e+06"},
      {"%8.0g", "-9999999", -9999999, "-1.0e+07", "-1.0e+07"},
      {"%8.0g", "-99999999", -99999999, "-1.0e+08", "-1.0e+08"},
      {"%8.0g", "-1234567890", -1234567890, "-1.2e+09", "-1.2e+09"},
      {"%8.0g", "-12345678901234", -12345678901234, "-1.2e+13", "-1.2e+13"},
      {"%8.0g", "-1e20", -1e20, "-1.0e+20", "-1.0e+20"},
      {"%8.0g", "-3.14159265358979", -3.14159265358979, "-3.14159", "-3.14159"},
      {"%8.0g", "-123456.789", -123456.789, " -123457", "-123457"},
      {"%8.0g", "-1234567.89", -1234567.89, "-1.2e+06", "-1.2e+06"},
      {"%10.0g", "-42", -42, "       -42", "-42"},
      {"%10.0g", "-2.5", -2.5, "      -2.5", "-2.5"},
      {"%10.0g", "-.5", -0.5, "       -.5", "-.5"},
      {"%10.0g", "-.1", -0.1, "       -.1", "-.1"},
      {"%10.0g", "-.000012345", -0.000012345, "-.00001234", "-.00001234"},
      {"%10.0g", "-1e-20", -1e-20, "-1.000e-20", "-1.000e-20"},
      {"%10.0g", "-1000000", -1000000, "  -1000000", "-1000000"},
      {"%10.0g", "-9999999", -9999999, "  -9999999", "-9999999"},
      {"%10.0g", "-99999999", -99999999, " -99999999", "-99999999"},
      {"%10.0g", "-1234567890", -1234567890, "-1.235e+09", "-1.235e+09"},
      {"%10.0g", "-12345678901234", -12345678901234, "-1.235e+13", "-1.235e+13"},
      {"%10.0g", "-1e20", -1e20, "-1.000e+20", "-1.000e+20"},
      {"%10.0g", "-3.14159265358979", -3.14159265358979, "-3.1415927", "-3.1415927"},
      {"%10.0g", "-123456.789", -123456.789, "-123456.79", "-123456.79"},
      {"%10.0g", "-1234567.89", -1234567.89, "-1234567.9", "-1234567.9"},
      {"%9.3g", "-42", -42, "      -42", "-42"},
      {"%9.3g", "-2.5", -2.5, "     -2.5", "-2.5"},
      {"%9.3g", "-.5", -0.5, "      -.5", "-.5"},
      {"%9.3g", "-.1", -0.1, "      -.1", "-.1"},
      {"%9.3g", "-.000012345", -0.000012345, "-.0000123", "-.0000123"},
      {"%9.3g", "-1e-20", -1e-20, "-1.00e-20", "-1.00e-20"},
      {"%9.3g", "-1000000", -1000000, " -1000000", "-1000000"},
      {"%9.3g", "-9999999", -9999999, " -9999999", "-9999999"},
      {"%9.3g", "-99999999", -99999999, "-1.00e+08", "-1.00e+08"},
      {"%9.3g", "-1234567890", -1234567890, "-1.23e+09", "-1.23e+09"},
      {"%9.3g", "-12345678901234", -12345678901234, "-1.23e+13", "-1.23e+13"},
      {"%9.3g", "-1e20", -1e20, "-1.00e+20", "-1.00e+20"},
      {"%9.3g", "-3.14159265358979", -3.14159265358979, "    -3.14", "-3.14"},
      {"%9.3g", "-123456.789", -123456.789, "  -123457", "-123457"},
      {"%9.3g", "-1234567.89", -1234567.89, " -1234568", "-1234568"},
      {"%9.2g", "-42", -42, "      -42", "-42"},
      {"%9.2g", "-2.5", -2.5, "     -2.5", "-2.5"},
      {"%9.2g", "-.5", -0.5, "      -.5", "-.5"},
      {"%9.2g", "-.1", -0.1, "      -.1", "-.1"},
      {"%9.2g", "-.000012345", -0.000012345, " -.000012", "-.000012"},
      {"%9.2g", "-1e-20", -1e-20, " -1.0e-20", "-1.0e-20"},
      {"%9.2g", "-1000000", -1000000, " -1000000", "-1000000"},
      {"%9.2g", "-9999999", -9999999, " -9999999", "-9999999"},
      {"%9.2g", "-99999999", -99999999, " -1.0e+08", "-1.0e+08"},
      {"%9.2g", "-1234567890", -1234567890, " -1.2e+09", "-1.2e+09"},
      {"%9.2g", "-12345678901234", -12345678901234, " -1.2e+13", "-1.2e+13"},
      {"%9.2g", "-1e20", -1e20, " -1.0e+20", "-1.0e+20"},
      {"%9.2g", "-3.14159265358979", -3.14159265358979, "     -3.1", "-3.1"},
      {"%9.2g", "-123456.789", -123456.789, "  -123457", "-123457"},
      {"%9.2g", "-1234567.89", -1234567.89, " -1234568", "-1234568"},
      {"%12.4g", "-42", -42, "         -42", "-42"},
      {"%12.4g", "-2.5", -2.5, "        -2.5", "-2.5"},
      {"%12.4g", "-.5", -0.5, "         -.5", "-.5"},
      {"%12.4g", "-.1", -0.1, "         -.1", "-.1"},
      {"%12.4g", "-.000012345", -0.000012345, "  -.00001234", "-.00001234"},
      {"%12.4g", "-1e-20", -1e-20, "  -1.000e-20", "-1.000e-20"},
      {"%12.4g", "-1000000", -1000000, "    -1000000", "-1000000"},
      {"%12.4g", "-9999999", -9999999, "    -9999999", "-9999999"},
      {"%12.4g", "-99999999", -99999999, "   -99999999", "-99999999"},
      {"%12.4g", "-1234567890", -1234567890, " -1234567890", "-1234567890"},
      {"%12.4g", "-12345678901234", -12345678901234, "  -1.235e+13", "-1.235e+13"},
      {"%12.4g", "-1e20", -1e20, "  -1.000e+20", "-1.000e+20"},
      {"%12.4g", "-3.14159265358979", -3.14159265358979, "      -3.142", "-3.142"},
      {"%12.4g", "-123456.789", -123456.789, "     -123457", "-123457"},
      {"%12.4g", "-1234567.89", -1234567.89, "    -1234568", "-1234568"},
      {"%20.0g", "-42", -42, "                 -42", "-42"},
      {"%20.0g", "-2.5", -2.5, "                -2.5", "-2.5"},
      {"%20.0g", "-.5", -0.5, "                 -.5", "-.5"},
      {"%20.0g", "-.1", -0.1, "-.100000000000000006", "-.100000000000000006"},
      {"%20.0g", "-.000012345", -0.000012345, "         -.000012345", "-.000012345"},
      {"%20.0g", "-1e-20", -1e-20, "-1.0000000000000e-20", "-1.0000000000000e-20"},
      {"%20.0g", "-1000000", -1000000, "            -1000000", "-1000000"},
      {"%20.0g", "-9999999", -9999999, "            -9999999", "-9999999"},
      {"%20.0g", "-99999999", -99999999, "           -99999999", "-99999999"},
      {"%20.0g", "-1234567890", -1234567890, "         -1234567890", "-1234567890"},
      {"%20.0g", "-12345678901234", -12345678901234, "     -12345678901234", "-12345678901234"},
      {"%20.0g", "-1e20", -1e20, "-1.0000000000000e+20", "-1.0000000000000e+20"},
      {"%20.0g", "-3.14159265358979", -3.14159265358979, "-3.14159265358979001", "-3.14159265358979001"},
      {"%20.0g", "-123456.789", -123456.789, "-123456.789000000004", "-123456.789000000004"},
      {"%20.0g", "-1234567.89", -1234567.89, " -1234567.8899999999", "-1234567.8899999999"},
      {"%9.0e", "-42", -42, "-4.20e+01", "-4.20e+01"},
      {"%9.0e", "-2.5", -2.5, "-2.50e+00", "-2.50e+00"},
      {"%9.0e", "-.5", -0.5, "-5.00e-01", "-5.00e-01"},
      {"%9.0e", "-.1", -0.1, "-1.00e-01", "-1.00e-01"},
      {"%9.0e", "-.000012345", -0.000012345, "-1.23e-05", "-1.23e-05"},
      {"%9.0e", "-1e-20", -1e-20, "-1.00e-20", "-1.00e-20"},
      {"%9.0e", "-1000000", -1000000, "-1.00e+06", "-1.00e+06"},
      {"%9.0e", "-9999999", -9999999, "-1.00e+07", "-1.00e+07"},
      {"%9.0e", "-99999999", -99999999, "-1.00e+08", "-1.00e+08"},
      {"%9.0e", "-1234567890", -1234567890, "-1.23e+09", "-1.23e+09"},
      {"%9.0e", "-12345678901234", -12345678901234, "-1.23e+13", "-1.23e+13"},
      {"%9.0e", "-1e20", -1e20, "-1.00e+20", "-1.00e+20"},
      {"%9.0e", "-3.14159265358979", -3.14159265358979, "-3.14e+00", "-3.14e+00"},
      {"%9.0e", "-123456.789", -123456.789, "-1.23e+05", "-1.23e+05"},
      {"%9.0e", "-1234567.89", -1234567.89, "-1.23e+06", "-1.23e+06"},
  };

  int okDisplay = 0, okValue = 0;
  std::string firstDisplayFail, firstValueFail;
  for (const auto& r : kRows) {
    // Path 1 — the `display` command, end to end through the real parser.
    const std::string got = dispText(std::string("display ") + r.spec + " (" + r.literal + ")");
    if (got == r.wantDisplay) {
      ++okDisplay;
    } else if (firstDisplayFail.empty()) {
      firstDisplayFail = std::string("display ") + r.spec + " (" + r.literal + ") -> [" + got +
                         "], StataBE says [" + r.wantDisplay + "]";
    }
    // Path 2 — the string()/tostring renderer, called directly.
    const std::string gotv = piet::eval::applyStataFmt(r.value, r.spec);
    if (gotv == r.wantValue) {
      ++okValue;
    } else if (firstValueFail.empty()) {
      firstValueFail = std::string("applyStataFmt(") + r.literal + ", \"" + r.spec + "\") -> [" +
                       gotv + "], StataBE says [" + r.wantValue + "]";
    }
  }
  const int nRows = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));
  check(okDisplay == nRows,
        "BUG-195 display path: " + std::to_string(okDisplay) + "/" + std::to_string(nRows) +
            " against the StataBE capture" +
            (firstDisplayFail.empty() ? "" : " (first: " + firstDisplayFail + ")"));
  check(okValue == nRows,
        "BUG-195 string()/tostring path: " + std::to_string(okValue) + "/" +
            std::to_string(nRows) + " against the StataBE capture" +
            (firstValueFail.empty() ? "" : " (first: " + firstValueFail + ")"));

  // The missing rows are held out of the table because `.` has no double
  // literal. Stata right-justifies a missing into the numeric field on the
  // display side and returns a bare `.` from string() — the one place the two
  // layers differ by more than padding, and both halves are in the capture
  // (rows A|<spec>|19 and B|<spec>|19).
  {
    static const char* kSpecs[] = {"%9.0g", "%12.0g", "%8.0g",  "%10.0g", "%9.3g",
                                   "%9.2g", "%12.4g", "%20.0g", "%9.0e"};
    int okMissing = 0;
    std::string firstFail;
    for (const char* spec : kSpecs) {
      const int w = std::atoi(spec + 1);
      const std::string want = std::string(static_cast<size_t>(w) - 1, ' ') + ".";
      const std::string got = dispText(std::string("display ") + spec + " (.)");
      if (got == want) {
        ++okMissing;
      } else if (firstFail.empty()) {
        firstFail = std::string(spec) + " -> [" + got + "], want [" + want + "]";
      }
    }
    check(okMissing == 9,
          "BUG-195 missing rows: " + std::to_string(okMissing) +
              "/9 — a missing right-justifies into the field on the display side" +
              (firstFail.empty() ? "" : " (first: " + firstFail + ")"));
    for (const char* spec : kSpecs) {
      std::vector<piet::eval::PyValue> args{piet::eval::PyValue::floating(std::nan("")),
                                            piet::eval::PyValue::str(spec)};
      if (piet::eval::fnString(args).strVal() != ".") {
        check(false, std::string("BUG-195 missing rows: string(., \"") + spec +
                         "\") must be a bare `.` with no field");
        break;
      }
    }
    check(true, "BUG-195 missing rows: string(., <spec>) is a bare `.` for all 9 specs");
  }

  // BUG-192 clause 4 was WRONG and this capture is what caught it: the field
  // budget does not count a minus sign, so a negative may fill all w. Under the
  // old flat "never exceeds w-1" reading these three shed significant decimals
  // Stata keeps, and the loss grows with the width. Pinned by name so a
  // regression says which clause broke rather than just "a number moved".
  {
    struct ClauseCase { const char* cmd; const char* want; const char* clause; };
    static const ClauseCase kClause4[] = {
        {"display %9.0g (-123456.789)", "-123456.8", "clause 4: the sign does not spend the budget"},
        {"display %10.0g (-123456.789)", "-123456.79", "clause 4: ...and the loss grew with w"},
        {"display %20.0g (-123456.789)", "-123456.789000000004", "clause 4: ...at every width"},
    };
    for (const auto& c : kClause4) {
      check(dispText(c.cmd) == c.want,
            std::string("BUG-192 ") + c.clause + ": `" + c.cmd + "` -> [" + c.want + "]");
    }
  }

  // LIVE WIRING. The two checks above prove the renderer; this proves the verb
  // actually reaches it. `tostring, usedisplayformat` is the consumer that made
  // BUG-195 MEDIUM-HIGH rather than a display cosmetic — it writes these bytes
  // into a string variable that survives save and export. Every expectation is
  // fmt_g_oracle.log's section C for %9.0g. Before the fix this column read
  // 4e+01 / 1e-05 / 1e+08 / 3.
  {
    (void)runCmd(session, "clear");
    (void)runCmd(session, "set obs 4");
    (void)runCmd(session, "gen double v = 42");
    (void)runCmd(session, "replace v = .000012345 in 2");
    (void)runCmd(session, "replace v = 99999999 in 3");
    (void)runCmd(session, "replace v = 3.14159265358979 in 4");
    (void)runCmd(session, "format v %9.0g");
    // `force`, as in fmt_g_oracle.do:121: these renderings do not read back
    // equal, and tostring.ado leaves the variable as is without it (BUG-665).
    (void)runCmd(session, "tostring v, generate(sv) usedisplayformat force");
    auto j = runCmd(session, "list");
    std::vector<std::string> got;
    for (const auto& b : j.value("blocks", nlohmann::json::array())) {
      if (b.value("type", std::string()) != "browse") continue;
      for (const auto& row : b.value("rows", nlohmann::json::array())) {
        if (row.is_array() && row.size() >= 2 && row[1].is_string()) {
          got.push_back(row[1].get<std::string>());
        }
      }
    }
    const std::vector<std::string> want{"42", ".0000123", "1.00e+08", "3.141593"};
    std::string gotJoined, wantJoined;
    for (const auto& s : got) gotJoined += "[" + s + "]";
    for (const auto& s : want) wantJoined += "[" + s + "]";
    check(got == want,
          "BUG-195 live wiring: `tostring v, usedisplayformat` on a %9.0g column writes the "
          "oracle's bytes into the stored variable — got " +
              (gotJoined.empty() ? "<nothing>" : gotJoined) + ", want " + wantJoined);
  }

  // BUG-196: `display (.)` printed the literal `(.)`. The three-line truth
  // table from the entry, verbatim — it is neither "parentheses are not
  // evaluated" nor "missing is not understood" but the combination, so all
  // three lines have to hold at once for the diagnosis to stay true.
  check(dispText("display (.)") == ".", "BUG-196: `display (.)` evaluates to missing (`.`)");
  check(dispText("display .") == ".", "BUG-196: `display .` still prints `.` (was correct)");
  check(dispText("display (2+3)") == "5",
        "BUG-196: `display (2+3)` still evaluates (parentheses alone were always fine)");
  // Same fix, wider: the fast path never substituted the missing sentinel at
  // all, so arithmetic against a bare dot echoed as source text too. Stata
  // prints `.` for both (missing_range_oracle.log rows dot-plus-two, negated-dot).
  check(dispText("display 2+.") == ".", "BUG-196: `display 2+.` is missing, not the literal text");
  check(dispText("display (-.)") == ".", "BUG-196: `display (-.)` is missing (magnitude test)");
  // The retry must not fire on ordinary decimals: replaceBareDotMissing only
  // rewrites a `.` with no word character or dot on either side.
  check(dispText("display 3.14") == "3.14", "BUG-196 non-regression: `display 3.14` untouched");
  check(dispText("display .5") == ".5", "BUG-196 non-regression: `display .5` untouched");
  check(dispText("display 2*3.5") == "7", "BUG-196 non-regression: ordinary arithmetic untouched");
  // And c(maxdouble) is not missing: Stata prints 8.99e+307 for it
  // (missing_range_oracle.log, sentinel-exact). Missing starts at 2^1023, one
  // ulp above (exprfns.h); c(maxdouble) is the largest real number.
  check(dispText("display (8.988465674311579e+307)") == "8.99e+307",
        "BUG-196 boundary: c(maxdouble) is a NUMBER, not missing");
}


// BUG-208: `local l : variable label x` must return the label the session set.
//
// The extended macro function reads eval.varLabels, whose only writer in the whole
// tree used to be the evalcase test harness — so under the live dispatcher it read
// a permanently empty map and answered "" for a label `describe` was displaying at
// the same moment. A legal value, no diagnostic, and `if "`l'" == ""` is the
// idiomatic use, so a do-file branched wrong in silence. Shipped in B.10.0.
//
// Oracle: StataNow 19.5, StataBE -b, judged by an in-log WSM4_ marker:
//   label variable x "first label"  -> [first label]
//   label variable x "second label" -> [second label]
//   rename x xr                     -> [second label]   (rename CARRIES the label)
//   label variable xr ""            -> []
//
// The rename and save/use legs are the load-bearing ones. eval.varLabels is now a
// PROJECTION of SessionState's live store, not a mirror written by the `label
// variable` handler; had it been a mirror, those two would still return "" because
// rename and the .hbin header are DIFFERENT writers of varMeta. They are here so
// that a future "simplification" back to mirror-on-write fails a check instead of
// quietly re-breaking two thirds of the surface.
void testBUG208VariableLabelExtMacro() {
  std::printf("[selftest] --- BUG-208 `: variable label` reads the LIVE label store ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug208").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  // The `display` text for the last dispatched line, which is where the expanded
  // local lands — read the block rather than the store, so this checks the surface
  // a do-file actually sees.
  auto displayText = [](const nlohmann::ordered_json& blocks) -> std::string {
    std::string out;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display") {
        out = b.value("text", std::string());
      }
    }
    return out;
  };
  auto labelOf = [&](const char* var) -> std::string {
    piet::dispatch(std::string("local l208 : variable label ") + var, session);
    return displayText(piet::dispatch("display \"[`l208']\"", session));
  };

  piet::dispatch("set obs 5", session);
  piet::dispatch("gen double x = _n", session);

  piet::dispatch("label variable x \"first label\"", session);
  check(labelOf("x") == "[first label]",
        "BUG-208: `: variable label x` returns the label the session just set "
        "(was \"\" — the reader was looking at a store only the test harness wrote)");

  piet::dispatch("label variable x \"second label\"", session);
  check(labelOf("x") == "[second label]",
        "BUG-208: a re-issued `label variable` is reflected, i.e. the projection is "
        "refreshed per read and not cached from the first answer");

  piet::dispatch("rename x xr", session);
  check(labelOf("xr") == "[second label]",
        "BUG-208: the label survives `rename` and is still readable under the NEW name "
        "(Stata: [second label]) — a different varMeta writer than `label variable`, so "
        "mirror-on-write in setVarLabel would answer \"\" here");

  // save -> clear -> use: the .hbin header carriage plane (BUG-198's writer), a
  // third independent varMeta writer.
  std::string hb = (fs::path(workdir) / "bug208_rt.hbin").u8string();
  piet::dispatch("label variable xr \"for save\"", session);
  piet::dispatch("save \"" + hb + "\", replace", session);
  piet::dispatch("clear", session);
  piet::dispatch("use \"" + hb + "\"", session);
  check(labelOf("xr") == "[for save]",
        "BUG-208: a label round-tripped through the .hbin header is readable after `use` "
        "— the third varMeta writer, also missed by any mirror-on-write bridge");

  piet::dispatch("label variable xr \"\"", session);
  check(labelOf("xr") == "[]",
        "BUG-208: clearing the label reads back empty (Stata: []) — the projection is "
        "rebuilt wholesale, so a cleared label cannot outlive the fact");

  piet::dispatch("drop xr", session);
  check(labelOf("xr") == "[]",
        "BUG-208: a DROPPED variable's label does not survive in the projection");

  fs::remove_all(workdir, ec);
}


// BUG-216/217: an extended macro function must ANSWER or ERROR — never echo its
// own source text.
//
// Pre-fix, `local f : format x` stored the eight characters `: format x`, and
// `local z : notafunction x` stored `: notafunction x`. That is a third outcome and
// the worst of the three: not the answer, not empty, not an error, and no consumer
// can tell it from data — `format v `f'` applies a garbage format and
// `if "`t'" == "double"` silently takes the false branch. BUG-208's failure at
// least returned "", a value the variable might legitimately have had.
//
// Oracle: StataNow 19.5, `StataBE -b`, judged by an in-log marker and NEVER by exit
// code, on `gen double x = _n` + `format x %9.3f` + `gen gv` labelled `mylbl`:
//   : format x              -> [%9.3f]         : format nosuchvar       -> r(111)
//   : value label x         -> []      rc 0    : value label gv         -> [mylbl]
//   : variable label x      -> [BUG208 label]  : variable label nosuch  -> r(111)
//   : notafunction x        -> r(101), "notafunction not allowed"
// and the messages are Stata's own, captured with `capture noisily`:
// "variable nosuchvar not found" (111) / "notafunction not allowed" (101).
//
// The r(101) leg is the load-bearing one because the fall-through is GENERIC: it
// covers every extended macro function this engine has not implemented, so this
// check is what stops a future unimplemented `: fn` from silently reverting to
// echoing text. The "exists but has no label/value-label" legs are here for the
// opposite reason — they are the cases that must stay "" AT RC 0, and collapsing
// them into the r(111) reject would be the easy wrong way to make the reject work.
void testBUG216ExtMacroFnRejects() {
  std::printf("[selftest] --- BUG-216/217 extended macro fn: answer or error, never echo ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug216").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  piet::dispatch("clear", session);
  piet::dispatch("set obs 5", session);
  piet::dispatch("gen double x = _n", session);
  piet::dispatch("label variable x \"BUG208 label\"", session);
  piet::dispatch("format x %9.3f", session);
  piet::dispatch("gen gv = _n", session);
  piet::dispatch("label define mylbl 1 \"one\" 2 \"two\"", session);
  piet::dispatch("label values gv mylbl", session);

  // Read the macro through the surface a do-file actually sees.
  auto valueOf = [&](const char* rhs) -> std::string {
    piet::dispatch(std::string("local l216 : ") + rhs, session);
    const nlohmann::ordered_json blocks = piet::dispatch("display \"[`l216']\"", session);
    std::string out;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display") {
        out = b.value("text", std::string());
      }
    }
    return out;
  };
  // The rc the dispatcher reports for the definition line itself.
  auto rcOf = [&](const char* rhs) -> std::string {
    const nlohmann::ordered_json blocks = piet::dispatch(std::string("local l216e : ") + rhs,
                                                         session);
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") {
        return piet::eval::coarseRc(b);
      }
    }
    return std::string("0");
  };

  check(valueOf("format x") == "[%9.3f]",
        "BUG-216: `: format x` is the variable's display format, not the macro text");
  check(valueOf("variable label x") == "[BUG208 label]",
        "BUG-216: `: variable label x` still answers the live label (BUG-208 intact)");
  check(valueOf("value label gv") == "[mylbl]",
        "BUG-216: `: value label gv` is the value-label SET NAME");
  check(valueOf("value label x") == "[]",
        "BUG-216: `: value label x` on a variable with no value label is \"\" at rc 0");

  check(rcOf("format nosuchvar") == "111",
        "BUG-217: `: format nosuchvar` is r(111), not a literal echo");
  check(rcOf("variable label nosuchvar") == "111",
        "BUG-217: `: variable label nosuchvar` is r(111), not \"\"");
  check(rcOf("value label nosuchvar") == "111",
        "BUG-217: `: value label nosuchvar` is r(111)");
  check(rcOf("format x") == "0" && rcOf("value label x") == "0",
        "BUG-217: an EXISTING variable does not reject — the reject is missing-only");
  check(rcOf("notafunction x") == "101",
        "BUG-216: an unimplemented `: function` is r(101) (GENERIC fall-through)");
  check(rcOf("nosuchfn") == "101",
        "BUG-216: a bare unimplemented `: function` with no argument is r(101) too");

  // The implemented arms a real do-file uses must be untouched by the new tail —
  // these six are every extended-macro form the PIET-executed corpus contains
  // (internal script p2perf_bench.do:144-149).
  // Values are Stata's, captured this stint on this exact list (StataNow 19.5,
  // in-log WSP_LEN marker): word count -> 4, length local -> 21,
  // list uniq -> "alpha beta gamma". The 21 is recorded because the first draft of
  // this check asserted 26 from arithmetic-in-the-head and FAILED against an engine
  // that was already right — the pin was wrong, not the product. Which is the
  // BUG-217 lesson arriving from the other direction: an asserted number is not
  // evidence, even when you are the one asserting it.
  piet::dispatch("local mylist alpha beta beta gamma", session);
  check(valueOf("word count `mylist'") == "[4]", "BUG-216: `: word count` still answers");
  check(valueOf("word 2 of `mylist'") == "[beta]", "BUG-216: `: word N of` still answers");
  check(valueOf("length local mylist") == "[21]", "BUG-216: `: length local` still answers");
  check(valueOf("list uniq mylist") == "[alpha beta gamma]",
        "BUG-216: `: list uniq` still answers");
  check(rcOf("subinstr local mylist \"beta\" \"BETA\", all") == "0",
        "BUG-216: `: subinstr local` still answers");

  fs::remove_all(workdir, ec);
}



// ── BUG-246..250 expression/macro contract (WS-V, 2026-08-07) ────────────────
//
// Five defects, one layer, one oracle capture
// (internal capture exprfns_contract_oracle.{do,log}, StataBE 19.5,
// marker-judged). Every asserted byte below is a row of that capture:
//
//   BUG-246  `local/global NAME = <unevaluable>` echoed its own source text;
//            Stata errors. Mapped rcs: 1%2 -> 198, notafn(3) -> 133 "unknown
//            function", scalar(UNDEF) -> 111 "UNDEF not found", NAME[i,j] with
//            no matrix -> 111, bare `local x =` -> 198. String-literal RHSs
//            and bare-identifier NameErrors keep the legacy fallback (the two
//            deliberately unclaimed shapes — see classifyFailedEqExpr).
//   BUG-247  `global NAME : <extended fn>` never reached the dispatcher;
//            `: word count a b c` under global answered its own text.
//   BUG-248  applyStataFmt dropped the `-`/`0` flags and rendered %tc/%td as
//            raw numbers. Both string() and tostring ride this one function.
//   BUG-249  string()'s residual contract: %9s/%-9s/%/%%/%t -> "", a STRING
//            first argument WITH a format -> r(109), and `.a`..`.z` carry
//            their kind through the expression layer (string(.a) -> `.a`).
//   BUG-250  declared storage subtypes recorded at creation, answered by
//            describe and `: type`, honored by the .dta sidecar (the writer
//            half already spoke stata_type; creation never recorded it).
//
// Pre-fix RED: every leg here was measured failing on the pre-fix build this
// stint (probe transcripts in the stint report; e.g. `local e1 = 1%2` -> rc 0
// with e1=[1%2], `: type x` -> r(101), string(42.5,"%-9.2f") len 5, %td ->
// "20000", describe dtype "float" for a `gen double`).
// --- BUG-291 / 293 / 294 / 295 (ONESHOT-ZERO WS-N): the WS-V residue -------
//
// Four defects WS-V measured and left OPEN, each closed against a live StataBE
// 19.5 IC capture taken for this stint (scratch wsn_oracle.log; every expected
// value below is a line of that log, not a derivation):
//
//   BUG-291  a dropped/rebuilt column's varMeta survived BY NAME, so
//            `gen double x` -> `drop x` -> `gen x = 1` answered `double`
//            where Stata answers `float`. The entry ALSO claimed collapse
//            defaults to float — the capture FALSIFIES that: Stata's collapse
//            keeps the source type (double source -> double), which PIET
//            already did. Only the drop/keep half was ever a defect.
//   BUG-293  the display arm's own residue: no `0` flag in its spec grammar
//            (%09.2f -> `    42.50`, Stata `000042.50`), no %tc/%td arm
//            (`%td (20000)` -> `20000`, Stata `04oct2014`), a segment scanner
//            that broke on quotes INSIDE parentheses so `string(42.5,"%9.2f")`
//            was never evaluated at all, a default-%9.0g reformat applied to
//            STRING results, an unevaluable expression echoed at rc 0
//            (`display 1%2`, Stata r(198)), and an error that named its own
//            code in the message but answered `_rc == 1`.
//   BUG-294  an undefined macro was a hard error that STOPPED the do-file;
//            Stata expands it to empty at rc 0.
//   BUG-295  `gen z = _n` round-tripped through .dta as `byte` — the same
//            column answering float in-session and byte after a save.
//
// Pre-fix RED, measured on the pre-fix build this stint: `describe x` -> double
// after drop/recreate; `display %09.2f (42.5)` -> `    42.50`; `display %td
// (20000)` -> `20000`; `display string(42.5,"%9.2f")` -> the literal
// `string(42.5,%9.2f)`; `display 1%2` -> the literal `1%2` at rc 0; `display
// "[$UNDEF]"` -> engine_error, run stopped; `use` after save -> byte.
// --- BUG-296 (ONESHOT-ZERO WS-R): the format families outside %tc/%td --------
//
// Every expected value below is a captured Stata 19.5 IC byte, from
// internal capture exprfns_contract_oracle.log (sections GA/GB, the
// rows BUG-296 was filed with) and internal capture wsr_oracle.log
// (sections WSR|F, WSR|G, WSR|T, WSR|D, captured for this stint to close the
// three rules the first log did not pin: the `f` family's overflow arm, the
// other %t calendars, and detail-coded %td). All 94 oracle rows are compared
// against BOTH surfaces by the stint's harness; these are the discriminating
// rows — one per rule, plus the boundaries.
//
// Pre-fix RED, measured: every one of these printed the bare number
// (`%9,0f 42.5` -> `42.5`, `%12.0gc 1234567.89` -> `1234567.89`, `%tq 42.5` ->
// `42.5`, `%tdCCYY-NN-DD 20000` -> `20000`) and `%9.2f 1234567.89` printed the
// unfitting fixed form `1234567.89` where Stata gives ` 1.23e+06`.
void testBUG296FormatTaxonomy() {
  std::printf("[selftest] --- BUG-296 format taxonomy: comma / grouped / %%t calendars (WS-R) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_wsr").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  auto disp = [&](const std::string& line) -> std::string {
    const nlohmann::ordered_json blocks = piet::dispatch(line, session);
    std::string out;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display")
        out = b.value("text", std::string());
    }
    return out;
  };
  using piet::eval::applyStataFmt;

  // ---- the `f` family's OVERFLOW arm (WSR|FA/FB) --------------------------
  check(disp("display %9.2f (1234567.89)") == " 1.23e+06",
        "BUG-296: a fixed form too wide for its field becomes scientific "
        "(WSR|FA|%9.2f|1) — PIET printed the unfitting 1234567.89");
  check(disp("display %9.2f (-1234567.89)") == "-1.23e+06",
        "BUG-296: ...and the sign spends its own reserved column (WSR|FA|%9.2f|2)");
  check(disp("display %9.0f (123456789012.5)") == " 1.23e+11",
        "BUG-296: P=0 takes the w-7 exponent decimals (WSR|FA|%9.0f|4)");
  check(disp("display %9.3f (1234567.89)") == " 1.23e+06",
        "BUG-296: P=3 sheds to the w-1 budget (WSR|FA|%9.3f|1)");
  check(disp("display %6.2f (1234567.89)") == " 1.2e+06",
        "BUG-296: the shed floors at ONE decimal and the field at 8 (WSR|FA|%6.2f|1)");
  check(disp("display %12.2f (1234567.89)") == "  1234567.89",
        "BUG-296 control: a fixed form that FITS is untouched (WSR|FA|%12.2f|1)");
  check(applyStataFmt(1234567.89, "%9.2f") == "1.23e+06",
        "BUG-296: string()/tostring take the same overflow arm, unpadded (WSR|FB|%9.2f|1)");

  // ---- the comma-decimal family (GA/GB) -----------------------------------
  check(disp("display %9,0f (42.5)") == "       42" &&
            disp("display %9,0f (43.5)") == "       44",
        "BUG-296: %w,pf is %w.pf with a ',' decimal, and keeps printf's "
        "half-to-EVEN (42.5->42, 43.5->44 — GA|%9,0f|1,2)");
  check(disp("display %9,2f (1234567.89)") == " 1,23e+06",
        "BUG-296: the comma follows the value into the overflow arm (GA|%9,2f|5)");

  // ---- the `c` grouping suffix on `g` (GA/GB + WSR|GA) --------------------
  check(disp("display %12.0gc (1234.567)") == "   1,234.567",
        "BUG-296: %w.pgc groups (GA|%12.0gc|3)");
  check(disp("display %12.0gc (1234567.89)") == " 1,234,567.9",
        "BUG-296: the separators SPEND FIELD — a decimal is shed so the grouped "
        "form fits w-1 (GA|%12.0gc|5)");
  check(disp("display %9.0gc (1234.567)") == " 1,234.57",
        "BUG-296: ...shedding again at a narrower field (GA|%9.0gc|3)");
  check(disp("display %10.0gc (1234567.89)") == " 1234567.9",
        "BUG-296: but the shed stops at ONE decimal — grouping is dropped rather "
        "than the last digit (WSR|GA|%10.0gc|1)");
  check(disp("display %9.0gc (1234567.89)") == "  1234568",
        "BUG-296: and with no decimals to shed, grouping is dropped outright "
        "(GA|%9.0gc|5)");
  check(disp("display %14.0gc (123456789012.5)") == "  123456789013",
        "BUG-296: same at a wide field when the separators still overrun "
        "(WSR|GA|%14.0gc|4)");
  check(disp("display %8.0gc (1234567.89)") == " 1.2e+06",
        "BUG-296: a scientific mantissa is never chunked (WSR|GA|%8.0gc|1)");

  // ---- European: comma decimal + '.' grouping -----------------------------
  check(disp("display %12,0fc (1234567.89)") == "   1.234.568",
        "BUG-296: %w,pfc groups with '.' and decimals with ',' — the separator "
        "swap must precede grouping or the first GROUP mark is rewritten "
        "(GA|%12,0fc|5)");
  check(disp("display %9,0fc (1234567.89)") == "1.234.568",
        "BUG-296: the `f` family's grouping fits the FIELD (w), not the g rule's "
        "w-1 (WSR|GA|%9,0fc|1)");
  check(disp("display %9,0fc (-1234567.89)") == " -1234568",
        "BUG-296: ...and is dropped when it does not (WSR|GA|%9,0fc|2)");

  // ---- the other %t calendars (WSR|TA/TB) ---------------------------------
  check(disp("display %tq (42.5)") == "1970q3" && disp("display %tq (-3)") == "1959q2",
        "BUG-296: %tq floors and floor-divides across 1960 (WSR|TA|%tq|1,3)");
  check(disp("display %th (-3)") == "1958h2" && disp("display %tm (-3)") == "1959m10" &&
            disp("display %tw (245)") == "1964w38",
        "BUG-296: %th / %tm / %tw, same rule, own period counts (WSR|TA|%th|3, %tm|3, %tw|4)");
  check(disp("display %tm (42.5)") == " 1963m7",
        "BUG-296: display right-justifies into the FAMILY's widest value (1959m10 "
        "is 7), where string() does not pad (WSR|TA vs WSR|TB|%tm|1)");
  check(applyStataFmt(42.5, "%tm") == "1963m7",
        "BUG-296: ...string()/tostring return the value, not the column");
  // %ty took a SECOND capture (reghdfe_absorb_vce_oracle.log section TY, 14
  // values) — the first four looked self-inconsistent and were recorded as a
  // named gap; the wider sample resolves it into two clean regimes.
  check(disp("display %ty (42.5)") == "  43" && disp("display %ty (43.5)") == "  44",
        "BUG-296: %ty ROUNDS half-away-from-zero (42.5 -> 43, 43.5 -> 44 — not "
        "printf's half-even, unlike the f family) into a 4-wide field (TY|42.5)");
  check(disp("display %ty (99)") == "  99" && disp("display %ty (100)") == "0100" &&
            disp("display %ty (245)") == "0245" && disp("display %ty (1959)") == "1959",
        "BUG-296: <=99 is space-padded, 100..9999 is ZERO-padded to 4 — the "
        "boundary is sampled adjacently at 99/100 (TY|99, TY|100)");
  check(disp("display %ty (-3)") == "  -3",
        "BUG-296: ...and a negative year keeps its plain digits (TY|-3)");
  check(applyStataFmt(245.0, "%ty") == "0245",
        "BUG-296: string()/tostring take the zero-padding too — it is part of the "
        "VALUE here, not the field (TY|245|B len=4)");
  check(disp("display %ty (12345)") == "12345",
        "BUG-296 named gap: |year| >= 10000 keeps the raw fall-through. Stata "
        "gives 1.2e+04, but that is ONE sample — a point, not a rule");

  // ---- detail-coded %td (WSR|DA/DB) ---------------------------------------
  check(disp("display %tdCCYY-NN-DD (20000)") == "2014-10-04",
        "BUG-296: a %td detail string renders through the token map (WSR|DA)");
  check(disp("display %tdMonth_dd,_CCYY (20000)") == "   October 4, 2014",
        "BUG-296: `Month`/`dd`/`_`, right-justified into the detail's MAXIMUM "
        "width (September+dd = 18), not this value's (WSR|DA)");
  check(disp("display %tdDay_DD_Mon_CCYY (20000)") == "Sat 04 Oct 2014",
        "BUG-296: `Day`/`Mon` are fixed-width, so this one needs no pad (WSR|DA)");
  check(disp("display %tdnn/dd/YY (20000)") == " 10/4/14",
        "BUG-296: the un-padded token forms, and their max-width field (WSR|DA)");
  check(applyStataFmt(20000.0, "%tdCCYY-NN-DD") == "2014-10-04",
        "BUG-296: one decode, shared with the string()/tostring surface");

  // ---- BUG-292: the setcol transport preserves cell-edge whitespace --------
  // Oracle WSR|292: `format p %-9.2f` then `tostring p, gen(ps) usedisplayformat force`
  // stores all-9 levels. PIET rendered them right and cobra's csv_trim ate the
  // pad on the way in, giving lens {5,6,4,5,9}.
  {
    auto run = [&](const std::string& line) { return piet::dispatch(line, session); };
    (void)run("clear");
    (void)run("set obs 5");
    (void)run("gen double p = 42.5 in 1");
    (void)run("replace p = -42.5 in 2");
    (void)run("replace p = 3.14159 in 3");
    (void)run("replace p = -0.001 in 4");
    (void)run("replace p = 123456.789 in 5");
    (void)run("format p %-9.2f");
    // `force`, as in wsr_oracle.do:68 (BUG-665: without it 3.14159 at %-9.2f
    // is irreversible and tostring.ado leaves p as is).
    (void)run("tostring p, gen(ps) usedisplayformat force");
    const nlohmann::ordered_json blocks = run("list ps, noobs clean");
    std::vector<std::string> got;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "browse") {
        for (const auto& row : b.at("rows")) {
          if (row.is_array() && !row.empty() && row[0].is_string()) got.push_back(row[0]);
        }
      }
    }
    const std::vector<std::string> want = {"42.50    ", "-42.50   ", "3.14     ",
                                           "-0.00    ", "123456.79"};
    bool ok = got.size() == want.size();
    for (size_t i = 0; ok && i < want.size(); ++i) ok = got[i] == want[i];
    check(ok,
          "BUG-292: `tostring, usedisplayformat` under a LEFT-justified format stores "
          "all-9-byte levels (WSR|292) — the setcol CSV transport no longer trims a "
          "QUOTED cell's edge whitespace");
  }
}



void testBUG246to250ExprMacroContract() {
  std::printf("[selftest] --- BUG-246..250 expression/macro contract (WS-V) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_wsv").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  auto rcOfLine = [&](const std::string& line) -> std::string {
    const nlohmann::ordered_json blocks = piet::dispatch(line, session);
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") {
        return piet::eval::coarseRc(b);
      }
    }
    return std::string("0");
  };
  auto displayText = [&](const std::string& line) -> std::string {
    const nlohmann::ordered_json blocks = piet::dispatch(line, session);
    std::string out;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display") {
        out = b.value("text", std::string());
      }
    }
    return out;
  };

  // ---- BUG-246: unevaluable `= expr` errors with Stata's code --------------
  check(rcOfLine("local e1 = 1%2") == "198",
        "BUG-246: `local e1 = 1%2` is r(198) (was: rc 0, e1 = the literal `1%2`)");
  check(rcOfLine("local e3 = 2+*3") == "198", "BUG-246: malformed arithmetic is r(198)");
  check(rcOfLine("local e4 = notafn(3)") == "133",
        "BUG-246: an unknown FUNCTION is r(133) `unknown function` (not 198 — oracle row H|e4)");
  check(rcOfLine("local e5 = scalar(nosuchscalar)") == "111",
        "BUG-246: scalar(UNDEF) is r(111) `UNDEF not found`");
  check(rcOfLine("local e6 = QQ[1,1]") == "111",
        "BUG-246: a subscripted name with no matrix behind it is r(111)");
  check(rcOfLine("local e9 =") == "198",
        "BUG-246: the bare `local x =` is r(198) `invalid syntax` (oracle row H|e9)");
  check(rcOfLine("global ge1 = 1%2") == "198", "BUG-246: the global twin takes the same r(198)");
  // The two deliberately RETAINED fallbacks — the load-bearing shapes:
  piet::dispatch("local e8 = \"a:b c[1]\"", session);
  check(displayText("display \"[`e8']\"") == "[a:b c[1]]",
        "BUG-246: a string-literal RHS still evaluates by unquoting at rc 0");
  check(rcOfLine("local e10 = \"abc\" + \"def\"") == "0" &&
            displayText("display \"[`e10']\"") == "[abcdef]",
        "BUG-246: string concatenation still evaluates (rc 0)");

  // ---- BUG-247: `global NAME : <extended fn>` dispatches -------------------
  check(rcOfLine("global G1 : word count a b c") == "0" &&
            displayText("display \"[$G1]\"") == "[3]",
        "BUG-247: `global G1 : word count a b c` answers 3 (was: the literal colon text)");
  check(rcOfLine("global G4 : notafunction x") == "101",
        "BUG-247: the unimplemented-function tail is reachable under global too (r(101))");
  check(rcOfLine("global G5 : variable label nosuchvar") == "111",
        "BUG-247: the missing-variable reject propagates under global (r(111))");

  // ---- BUG-248: flags + date families in the ONE shared renderer -----------
  using piet::eval::applyStataFmt;
  check(applyStataFmt(42.5, "%-9.2f") == "42.50    ",
        "BUG-248: %-9.2f keeps its trailing pad (len 9 — oracle FB, was len 5)");
  check(applyStataFmt(-42.5, "%-9.0g") == "-42.5    ",
        "BUG-248: %-9.0g left-justifies the g family too");
  check(applyStataFmt(42.5, "%09.2f") == "000042.50" &&
            applyStataFmt(-42.5, "%09.2f") == "-00042.50",
        "BUG-248: the 0 flag zero-fills f, sign leading (oracle FA/FB %09.2f rows)");
  check(applyStataFmt(42.5, "%08.0g") == "42.5",
        "BUG-248: the 0 flag does NOT zero-fill g — %08.0g strips to the bare value on the "
        "string() surface (oracle FB row, len 4)");
  check(applyStataFmt(0.0, "%tc") == "01jan1960 00:00:00" &&
            applyStataFmt(59999.0, "%tc") == "01jan1960 00:00:59" &&
            applyStataFmt(1.5e12, "%tc") == "14jul2007 02:40:00" &&
            applyStataFmt(-86400000.0, "%tc") == "31dec1959 00:00:00",
        "BUG-248: %tc floors ms to seconds and renders ddmonccyy hh:mm:ss (4 oracle rows)");
  check(applyStataFmt(42.9, "%td") == "12feb1960" &&
            applyStataFmt(-0.5, "%td") == "31dec1959" &&
            applyStataFmt(20000.0, "%td") == "04oct2014",
        "BUG-248: %td FLOORS (42.9 -> 12feb, -0.5 -> 31dec1959) — 20000 is 04oct2014, "
        "not the +2 an off-by-epoch would give");
  // BUG-296 CLOSED this boundary: %tq (and %th/%tm/%tw, and detail-coded %td)
  // now render. The row is kept, flipped to the oracle it used to cite —
  // exprfns_contract_oracle.log G|pcttq|[1970q3], corroborated across four
  // values per family in wsr_oracle.log section WSR|T.
  check(applyStataFmt(42.5, "%tq") == "1970q3",
        "BUG-296: %tq renders 1970q3 (was the documented raw-number residue)");

  // ---- BUG-249: string()'s residual contract --------------------------------
  check(rcOfLine("local g1 = string(42.5, \"%9s\")") == "0" &&
            displayText("display \"[`g1']\"") == "[]",
        "BUG-249: string(n, \"%9s\") is the empty string (oracle len 0)");
  check(applyStataFmt(42.5, "%") == "" && applyStataFmt(42.5, "%%") == "" &&
            applyStataFmt(42.5, "%t") == "" && applyStataFmt(42.5, "%-9s") == "",
        "BUG-249: %-prefixed NON-formats render empty (the captured set, nothing wider)");
  check(rcOfLine("local g2 = string(\"abc\", \"%9.0g\")") == "109",
        "BUG-249: a STRING first argument with a format is r(109) type mismatch "
        "(was: returned unchanged at rc 0)");
  check(rcOfLine("local g3 = string(.a, \"%9.0g\")") == "0" &&
            displayText("display \"[`g3']\"") == "[.a]",
        "BUG-249: string(.a, fmt) answers `.a` — the kind survives the expression layer "
        "(was: the whole expression failed and echoed its source)");
  check(rcOfLine("local g4 = string(.z)") == "0" &&
            displayText("display \"[`g4']\"") == "[.z]",
        "BUG-249: the top of the code range round-trips too (.z)");

  // ---- BUG-250: the storage-subtype plane -----------------------------------
  piet::dispatch("clear", session);
  piet::dispatch("set obs 2", session);
  piet::dispatch("gen double x = _n", session);
  piet::dispatch("gen byte b1 = 1", session);
  piet::dispatch("gen str8 s1 = \"ab\"", session);
  piet::dispatch("gen z = _n", session);
  piet::dispatch("egen double em = mean(x)", session);
  auto typeOf = [&](const char* var) -> std::string {
    piet::dispatch(std::string("local wsvt : type ") + var, session);
    return displayText("display \"[`wsvt']\"");
  };
  check(typeOf("x") == "[double]",
        "BUG-250: `: type x` answers the DECLARED double (was: r(101) placeholder, and "
        "describe called it float)");
  check(typeOf("b1") == "[byte]" && typeOf("s1") == "[str8]",
        "BUG-250: byte and str8 declarations recorded at creation");
  check(typeOf("z") == "[float]",
        "BUG-250: an undeclared gen answers float — Stata's own gen default, and exactly "
        "what describe says for the same column");
  check(typeOf("em") == "[double]", "BUG-250: egen's declared type recorded too");
  check(rcOfLine("local wsvm : type nosuchvar") == "111",
        "BUG-250: `: type nosuchvar` is r(111), same reject as the other variable readers");
  // describe reads the same plane:
  {
    const nlohmann::ordered_json blocks = piet::dispatch("describe", session);
    std::string dx, db1;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "describe" &&
          b.contains("variables")) {
        for (const auto& v : b.at("variables")) {
          if (v.value("name", std::string()) == "x") dx = v.value("dtype", std::string());
          if (v.value("name", std::string()) == "b1") db1 = v.value("dtype", std::string());
        }
      }
    }
    check(dx == "double" && db1 == "byte",
          "BUG-250: describe reports the declared subtypes (was: float for every "
          "engine-created column)");
  }
  // rename carries the claim (the varMeta move already existed; pinned so the
  // carriage map's measured `rename keeps` stays true):
  piet::dispatch("rename x xr", session);
  check(typeOf("xr") == "[double]", "BUG-250: rename carries the recorded subtype");

  fs::remove_all(workdir, ec);
}
