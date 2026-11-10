// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: treatment estimators.
#include "selftest_support.h"
#include "selftest_cases.h"


void testAerB2Oaxaca() {
  std::printf("[selftest] --- AER scope-2 bundle B2: oaxaca ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_aerb2ox").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  auto close = [](double a, double b, double rtol) {
    return std::fabs(a - b) <= rtol * std::max(1.0, std::fabs(b));
  };
  auto anyError = [](const nlohmann::ordered_json& blocks) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return true;
    return false;
  };
  auto firstError = [](const nlohmann::ordered_json& blocks) -> nlohmann::ordered_json {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return b;
    return nlohmann::ordered_json::object();
  };
  piet::SessionState s(workdir);
  s.eval.cwd = workdir;
  s.cwd = workdir;
  // tests/validate/b2_oracle.do's fixture, verbatim: 200 rows, no RNG.
  (void)piet::doexec::executeDoSource(
      "clear\nset obs 200\n"
      "gen double x1 = mod(_n * 7, 23) / 23\n"
      "gen double x2 = mod(_n * 13, 17) / 17\n"
      "gen byte   g  = mod(_n, 2)\n"
      "gen byte   t  = mod(floor((_n - 1) / 3), 2)\n"
      "gen double y  = 1 + 2*x1 - 0.5*x2 + 0.3*g + 0.25*t + mod(_n * 11, 31)/31 - 0.5\n",
      s, piet::dispatch);
  auto scalarOf = [&](const std::string& expr) {
    (void)piet::dispatch("scalar ox_tmp = " + expr, s);
    const std::string v = s.eval.scalars.get("ox_tmp", "");
    (void)piet::dispatch("scalar drop ox_tmp", s);
    try {
      return v.empty() || v == "." ? std::nan("") : std::stod(v);
    } catch (const std::exception&) {
      return std::nan("");  // an unparsable store value is a FAIL below, never an abort
    }
  };
  auto reasonOf = [&](const nlohmann::ordered_json& blocks) {
    return firstError(blocks).value("reason", std::string());
  };

  {
    auto out = piet::dispatch("oaxaca y x1 x2, by(g) pooled relax", s);
    check(!anyError(out), "B2 oaxaca: the corpus form (pooled relax) runs against the engine");
    const double* N = s.eval.eScalars.find("N");
    const double* N1 = s.eval.eScalars.find("N_1");
    const double* N2 = s.eval.eScalars.find("N_2");
    check(N != nullptr && *N == 200 && N1 != nullptr && *N1 == 100 && N2 != nullptr && *N2 == 100,
          "B2 oaxaca: e(N)=200, e(N_1)=e(N_2)=100 (oracle ox_N/ox_N_1/ox_N_2)");
    check(s.eval.eStrings.get("cmd", "") == "oaxaca" && s.eval.eStrings.get("model", "") == "linear",
          "B2 oaxaca: e(cmd)=oaxaca, e(model)=linear (oracle ox_cmd/ox_model)");
    // BUG-456 (2026-09-08): the e-STRING on every expression surface, driven
    // through doString (the BUG-382 lesson: dispatch() shortcuts the real
    // substitution path). Stata truth: capture.log L1/L6/S1/G1/G9/G10/I1/A1/A6/
    // A10/D1/D7 on the regress twin, and the corpus lines of probe_b2.do on
    // oaxaca itself. Every leg here was RED on the pre-fix binary: the store
    // held `oaxaca` (the check just above passed) while `local c = e(cmd)`
    // answered `.`, `if e(cmd)=="oaxaca" {` took the else branch, and every
    // assert/gen/count stopped with "function e() is not supported".
    {
      auto runDo = [&](const std::string& src) {
        nlohmann::json body{{"source", src}};
        auto r = piet::routes::doString(s, body.dump());
        return nlohmann::json::parse(r.json);
      };
      auto firstReason = [](const nlohmann::json& j) -> std::string {
        for (const auto& b : j.value("blocks", nlohmann::json::array()))
          if (b.value("type", std::string()) == "error") return b.value("reason", std::string("<no-reason>"));
        return "";
      };
      (void)runDo("local c = e(cmd)\nlocal eq = (e(cmd)==\"oaxaca\")\nscalar s2 = (e(cmd)==\"oaxaca\")\n"
                  "local hit 0\nif e(cmd)==\"oaxaca\" {\n  local hit 1\n}\n");
      check(s.eval.locals.get("c", "<unset>") == "oaxaca", "BUG-456: `local c = e(cmd)` is `oaxaca` (Stata L1), not `.`");
      check(s.eval.locals.get("eq", "<unset>") == "1", "BUG-456: `local eq = (e(cmd)==\"oaxaca\")` is 1 (Stata L6)");
      check(s.eval.scalars.get("s2", "<unset>") == "1", "BUG-456: `scalar s2 = (e(cmd)==\"oaxaca\")` is 1 (Stata S1)");
      check(s.eval.locals.get("hit", "0") == "1", "BUG-456: `if e(cmd)==\"oaxaca\" { }` takes the branch (Stata I1)");
      check(firstReason(runDo("assert e(cmd) == \"oaxaca\"\nassert _se[overall:group_1] > 0\n")).empty(),
            "BUG-456: the two probe_b2.do corpus asserts pass (Stata rc 0 / rc 0)");
      check(firstReason(runDo("assert e(cmd) == \"nope\"\n")) == "r(9)",
            "BUG-456: `assert e(cmd) == \"nope\"` is a real r(9), the claim was evaluated (Stata A6)");
      check(firstReason(runDo("assert e(cmd) == 1\n")) == "r(109)",
            "BUG-456: `assert e(cmd) == 1` is r(109) type mismatch, never a folded 0 (Stata A10)");
      {
        auto j = runDo("gen byte z456 = (e(cmd)==\"oaxaca\")\nsummarize z456\n");
        double mean = -1;
        for (const auto& b : j.value("blocks", nlohmann::json::array()))
          if (b.value("type", std::string()) == "summarize" && b.contains("stats") && b["stats"].contains("z456"))
            mean = b["stats"]["z456"].value("mean", -1.0);
        check(mean == 1.0, "BUG-456: `gen z = (e(cmd)==\"oaxaca\")` is all ones (Stata G1 mean 1)");
        (void)runDo("count if e(cmd)==\"nope\"\n");
        const double* n0 = s.eval.rValues.find("N");
        check(n0 != nullptr && *n0 == 0.0, "BUG-456: `count if e(cmd)==\"nope\"` is 0 (Stata G10)");
        (void)runDo("count if e(cmd)==\"oaxaca\"\n");
        const double* n1 = s.eval.rValues.find("N");
        check(n1 != nullptr && *n1 == 200.0, "BUG-456: `count if e(cmd)==\"oaxaca\"` is every row (Stata G9)");
        check(firstReason(runDo("count if e(cmd) == 1\n")) == "r(109)",
              "BUG-456: `count if e(cmd) == 1` is r(109) on the row plane too (refute3 M1), not 0");
      }
      {
        // The display twin: the dual-layer discriminator -- `display e(cmd)`
        // and `local c = e(cmd)` must agree ([[two-evaluator-layers-predicate-split]]).
        auto j = runDo("display e(cmd)\ndisplay length(e(cmd))\n");
        std::vector<std::string> shown;
        for (const auto& b : j.value("blocks", nlohmann::json::array()))
          if (b.value("type", std::string()) == "display") shown.push_back(b.value("text", std::string()));
        check(shown.size() == 2 && shown[0] == "oaxaca" && shown[1] == "6",
              "BUG-456: `display e(cmd)` prints oaxaca and `display length(e(cmd))` prints 6 (was `.` and 22 -- the missing sentinel's strlen)");
      }
    }
    check(s.eval.eStrings.get("vce", "") == "robust" && s.eval.eStrings.get("suest", "") == "suest",
          "B2 oaxaca: e(vce)=robust and e(suest)=suest -- `pooled` IMPLIES robust suest, "
          "the fact the first pass never read");
    const piet::eval::EvalMatrix* b = s.eval.findClassMatrix('e', "b");
    const piet::eval::EvalMatrix* V = s.eval.findClassMatrix('e', "V");
    check(b != nullptr && b->data.size() == 1 && b->data[0].size() == 10,
          "B2 oaxaca: e(b) is 1 x 10 (twofold: 5 overall + 2 explained + 3 unexplained)");
    check(V != nullptr && V->data.size() == 10 && V->data[0].size() == 10, "B2 oaxaca: e(V) is 10 x 10");
    static const char* kNames[] = {"overall:group_1", "overall:group_2", "overall:difference",
                                   "overall:explained", "overall:unexplained", "explained:x1",
                                   "explained:x2", "unexplained:x1", "unexplained:x2",
                                   "unexplained:_cons"};
    bool namesOk = b != nullptr && b->colnames.size() == 10;
    for (int j = 0; namesOk && j < 10; ++j) namesOk = b->colnames[static_cast<size_t>(j)] == kNames[j];
    check(namesOk, "B2 oaxaca: e(b) column names are Stata's eq:name spellings in Stata's order "
                   "(ox_coleq x ox_colnames)");
    // %21x pins (b2_oracle.json ox_*/oxse_*), decimal here for legibility.
    check(close(scalarOf("_b[overall:group_1]"), 1.8743234881610429, 1e-9),
          "B2 oaxaca: _b[overall:group_1] == 1.87432348816104 (ox_1)");
    check(close(scalarOf("_b[overall:difference]"), -0.22631857932513788, 1e-9),
          "B2 oaxaca: _b[overall:difference] == -0.226318579325138 (ox_3)");
    check(close(scalarOf("_b[explained:x1]"), -0.011416864937049324, 1e-9),
          "B2 oaxaca: _b[explained:x1] == -0.0114168649370493 (ox_6)");
    check(close(scalarOf("_b[unexplained:_cons]"), -0.1438085260555051, 1e-9),
          "B2 oaxaca: _b[unexplained:_cons] == -0.143808526055505 (ox_10)");
    check(close(scalarOf("_se[overall:group_1]"), 0.06460215610069572, 1e-9),
          "B2 oaxaca: _se[overall:group_1] == 0.0646021561006957 (oxse_1) -- the number five "
          "guessed formulas missed by 5.5e-4 .. 4.5e-3");
    check(close(scalarOf("_se[overall:difference]"), 0.09456790593139917, 1e-9),
          "B2 oaxaca: _se[overall:difference] == 0.0945679059313992 (oxse_3)");
    check(close(scalarOf("_se[explained:x2]"), 0.014662373922876901, 1e-9),
          "B2 oaxaca: _se[explained:x2] == 0.0146623739228769 (oxse_7)");
    check(close(scalarOf("_se[unexplained:_cons]"), 0.10626846783547478, 1e-9),
          "B2 oaxaca: _se[unexplained:_cons] == 0.106268467835475 (oxse_10)");
    if (b != nullptr && b->data[0].size() == 10) {
      const auto& r = b->data[0];
      check(close(r[2], r[0] - r[1], 1e-12) && close(r[2], r[3] + r[4], 1e-12),
            "B2 oaxaca: difference == group_1 - group_2 == explained + unexplained (identity)");
    }
  }
  {
    // The threefold DEFAULT is a different table; a named refusal, never the
    // twofold numbers under a different name.
    auto out = piet::dispatch("oaxaca y x1 x2, by(g)", s);
    check(reasonOf(out) == "refused:oaxaca_threefold_default",
          "B2 oaxaca: without `pooled` the refusal names the threefold default -- got `" +
              reasonOf(out) + "`");
    auto out2 = piet::dispatch("oaxaca y x1 x2, by(g) pooled swap", s);
    check(firstError(out2).value("code", std::string()) == "unsupported_option",
          "B2 oaxaca: `swap` is a real option this build does not implement -> option-refuse");
    auto out3 = piet::dispatch("oaxaca y x1 x2, pooled", s);
    check(reasonOf(out3) == "r(198)", "B2 oaxaca: by() is required, r(198) -- got `" + reasonOf(out3) + "`");
  }
  {
    // relax: a duplicate regressor is r(499) without it (edges.log RC 499) and
    // a ZERO (not missing) coefficient with it; overall:* is unchanged because
    // the group means do not depend on the design.
    (void)piet::dispatch("gen double dup = 2*x1", s);
    auto out = piet::dispatch("oaxaca y x1 x2 dup, by(g) pooled", s);
    check(reasonOf(out) == "r(499)",
          "B2 oaxaca: dropped coefficient without relax is r(499) -- got `" + reasonOf(out) + "`");
    auto out2 = piet::dispatch("oaxaca y x1 x2 dup, by(g) pooled relax", s);
    check(!anyError(out2), "B2 oaxaca: relax runs with the duplicate present");
    check(scalarOf("_b[explained:x1]") == 0.0 && scalarOf("_se[explained:x1]") == 0.0,
          "B2 oaxaca: relax drops the FIRST duplicate (x1) to 0 with SE 0 -- Stata keeps the "
          "later one (rank_oracle.txt CASE 13), the opposite of regress");
    check(close(scalarOf("_b[overall:group_1]"), 1.8743234881610429, 1e-9),
          "B2 oaxaca: overall:group_1 is invariant to the design (means only)");
  }
  {
    // The corpus writes a RANGE (`sch2-sch531`) inside the estimation varlist.
    (void)piet::dispatch("gen byte sch2 = mod(_n, 4) == 0", s);
    (void)piet::dispatch("gen byte sch3 = mod(_n, 4) == 1", s);
    auto out = piet::dispatch("oaxaca y x1 sch2-sch3, by(g) pooled relax noisily", s);
    check(!anyError(out), "B2 oaxaca: `x1 sch2-sch3` (a varlist range, the paper-034 shape) runs");
    const piet::eval::EvalMatrix* b = s.eval.findClassMatrix('e', "b");
    check(b != nullptr && b->colnames.size() == 12 && b->colnames[7] == "explained:sch3",
          "B2 oaxaca: the range expanded to sch2 sch3 (12 columns, explained:sch3 present)");
    bool note = false;
    for (const auto& blk : out)
      if (blk.value("type", std::string()) == "display" &&
          blk.value("text", std::string()).find("noisily") != std::string::npos)
        note = true;
    check(note, "B2 oaxaca: `noisily` is an accept-NOTE, never a silent no-op");
  }
  {
    auto out = piet::dispatch("oaxaca y x1 x2 if _n <= 150, by(g) pooled relax", s);
    check(!anyError(out), "B2 oaxaca: an if qualifier runs");
    const double* N = s.eval.eScalars.find("N");
    check(N != nullptr && *N == 150, "B2 oaxaca: e(N) == 150 under `if _n <= 150` (the qualifier reached the engine)");
  }
  {
    (void)piet::dispatch("gen byte g3 = mod(_n, 3)", s);
    auto out = piet::dispatch("oaxaca y x1 x2, by(g3) pooled relax", s);
    check(anyError(out) && reasonOf(out) == "r(420)",
          "B2 oaxaca: three groups is an error, r(420) as Stata answers -- got `" + reasonOf(out) + "`");
  }
  fs::remove_all(workdir, ec);
}


void testAerB2Nnmatch() {
  std::printf("[selftest] --- AER scope-2 bundle B2-4: teffects nnmatch ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_aerb2nn").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  auto close = [](double a, double b, double rtol) {
    return std::fabs(a - b) <= rtol * std::max(1.0, std::fabs(b));
  };
  auto anyError = [](const nlohmann::ordered_json& blocks) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return true;
    return false;
  };
  auto firstError = [](const nlohmann::ordered_json& blocks) -> nlohmann::ordered_json {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return b;
    return nlohmann::ordered_json::object();
  };
  auto reasonOf = [&](const nlohmann::ordered_json& blocks) {
    return firstError(blocks).value("reason", std::string());
  };
  piet::SessionState s(workdir);
  s.eval.cwd = workdir;
  s.cwd = workdir;
  auto scalarOf = [&](const std::string& expr) {
    (void)piet::dispatch("scalar nn_tmp = " + expr, s);
    const std::string v = s.eval.scalars.get("nn_tmp", "");
    (void)piet::dispatch("scalar drop nn_tmp", s);
    try {
      return v.empty() || v == "." ? std::nan("") : std::stod(v);
    } catch (const std::exception&) {
      return std::nan("");
    }
  };
  // `mm1[1]` is a SUBSCRIPTED reference, which this build resolves on `display`
  // but not inside a scalar/assert expression (a pre-existing limitation the
  // bcal probe hit too). `summarize v in i/i, meanonly` is the idiom that reads
  // one cell through a surface that works.
  auto cellOf = [&](const std::string& var, int row) {
    (void)piet::dispatch("summarize " + var + " in " + std::to_string(row) + "/" +
                             std::to_string(row) + ", meanonly",
                         s);
    const double* v = s.eval.rValues.find("min");
    return v != nullptr ? *v : std::nan("");
  };
  auto countIf = [&](const std::string& cond) {
    (void)piet::dispatch("count if " + cond, s);
    const double* v = s.eval.rValues.find("N");
    return v != nullptr ? *v : std::nan("");
  };

  // tests/validate/b2_oracle.do's fixture, verbatim -- the same 200 rows whose
  // te_* keys the B2 answer key already carried, and REPORT.md's fixture F1.
  (void)piet::doexec::executeDoSource(
      "clear\nset obs 200\n"
      "gen double x1 = mod(_n * 7, 23) / 23\n"
      "gen double x2 = mod(_n * 13, 17) / 17\n"
      "gen byte   g  = mod(_n, 2)\n"
      "gen byte   t  = mod(floor((_n - 1) / 3), 2)\n"
      "gen double y  = 1 + 2*x1 - 0.5*x2 + 0.3*g + 0.25*t + mod(_n * 11, 31)/31 - 0.5\n",
      s, piet::dispatch);
  {
    auto out = piet::dispatch("teffects nnmatch (y x1 x2) (t), gen(mm)", s);
    check(!anyError(out), "B2-4 nnmatch: the corpus form runs against the engine");
    const double* N = s.eval.eScalars.find("N");
    const double* n0 = s.eval.eScalars.find("n0");
    const double* n1 = s.eval.eScalars.find("n1");
    check(N != nullptr && *N == 200 && n0 != nullptr && *n0 == 101 && n1 != nullptr && *n1 == 99,
          "B2-4 nnmatch: e(N)=200 e(n0)=101 e(n1)=99 (oracle te_N + capture.log F1)");
    check(s.eval.eStrings.get("cmd", "") == "teffects" &&
              s.eval.eStrings.get("subcmd", "") == "nnmatch",
          "B2-4 nnmatch: e(cmd)=teffects and e(subcmd)=nnmatch -- the ENGINE verb is `nnmatch', "
          "and publishing that as e(cmd) would be a wrong answer to `if e(cmd)==\"teffects\"'");
    check(s.eval.eStrings.get("vce", "") == "robust" &&
              s.eval.eStrings.get("vcetype", "") == "AI robust" &&
              s.eval.eStrings.get("stat", "") == "ate" &&
              s.eval.eStrings.get("metric", "") == "mahalanobis",
          "B2-4 nnmatch: e(vce)=robust e(vcetype)=`AI robust' e(stat)=ate e(metric)=mahalanobis "
          "(INFO|nn| lines of aer_estim2_oracle.log)");
    check(s.eval.eStrings.get("title", "") == "Treatment-effects estimation" &&
              s.eval.eStrings.get("tvar", "") == "t" &&
              s.eval.eStrings.get("depvar", "") == "y",
          "B2-4 nnmatch: e(title)/e(tvar)/e(depvar)");
    // b2_oracle.json te_ate__hex +1.a3491f36cbd54X-006 / te_se__hex.
    check(close(scalarOf("_b[ATE:r1vs0.t]"), 0.025591164095371689, 1e-9),
          "B2-4 nnmatch: _b[ATE:r1vs0.t] == 0.0255911640953717 (te_ate__hex) -- and the "
          "reference RESOLVES: the `.' in the eq:name is what `_b[' used to choke on");
    check(close(scalarOf("_se[ATE:r1vs0.t]"), 0.058322183303489719, 1e-9),
          "B2-4 nnmatch: _se[ATE:r1vs0.t] == 0.0583221833034897 (te_se__hex)");
    const piet::eval::EvalMatrix* b = s.eval.findClassMatrix('e', "b");
    const piet::eval::EvalMatrix* V = s.eval.findClassMatrix('e', "V");
    check(b != nullptr && b->data.size() == 1 && b->data[0].size() == 1 &&
              b->colnames.size() == 1 && b->colnames[0] == "ATE:r1vs0.t" &&
              b->rownames.size() == 1 && b->rownames[0] == "y1",
          "B2-4 nnmatch: e(b) is 1x1, colname `ATE:r1vs0.t', rowname `y1' (te_bcolnames)");
    check(V != nullptr && V->data.size() == 1 && V->data[0].size() == 1 &&
              close(V->data[0][0], 0.0034014770652858553, 1e-9),
          "B2-4 nnmatch: e(V) is 1x1 and equals te_v11__hex (+1.bdd6a181ca555X-009)");
    const double* kmin = s.eval.eScalars.find("k_nnmin");
    const double* kmax = s.eval.eScalars.find("k_nnmax");
    const double* krob = s.eval.eScalars.find("k_robust");
    const double* dtol = s.eval.eScalars.find("dtolerance");
    check(kmin != nullptr && *kmin == 1 && kmax != nullptr && *kmax == 2 && krob != nullptr &&
              *krob == 2,
          "B2-4 nnmatch: e(k_nnmin)=1 e(k_nnmax)=2 e(k_robust)=2 (the default is TWO matches for "
          "the conditional variance, causal.pdf p.637)");
    check(dtol != nullptr && close(*dtol, 1.4901161193847656e-8, 1e-15),
          "B2-4 nnmatch: e(dtolerance) == sqrt(c(epsdouble)) (+1.fffffffff8f7aX-01b)");
    check(s.eval.eStrings.get("indexvar", "") == "mm1 mm2",
          "B2-4 nnmatch: gen(mm) minted TWO columns and e(indexvar) names them (te_mm_list)");
    check(cellOf("mm1", 1) == 90 && cellOf("mm1", 2) == 130 && cellOf("mm1", 5) == 133 &&
              cellOf("mm1", 200) == 124,
          "B2-4 nnmatch: mm1[1]=90 mm1[2]=130 mm1[5]=133 mm1[200]=124 (te_mm1_* oracle keys) -- "
          "these ids, not the estimate, are what paper 076 consumes");
    check(cellOf("mm1", 104) == 130 && cellOf("mm2", 104) == 78,
          "B2-4 nnmatch: row 104's NONZERO-distance tie is listed DESCENDING (130 then 78). "
          "Always-ascending and first-member-only are both falsified arms (REPORT.md §5); the "
          "REFUTED anchored-group rule would also put a different id first here");
    check(countIf("!missing(mm1)") == 200,
          "B2-4 nnmatch: every row of the estimation sample got an id");
  }
  {
    // gen() refuses BEFORE it estimates, exactly as Stata does (r(110),
    // INFO|nn_edge|gen_collision_rc).
    auto out = piet::dispatch("teffects nnmatch (y x1 x2) (t), gen(mm)", s);
    check(reasonOf(out) == "r(110)",
          "B2-4 nnmatch: a gen() stub whose columns already exist is r(110) `variable mm1 "
          "already defined' -- got `" + reasonOf(out) + "`");
    auto o2 = piet::dispatch("teffects nnmatch (y x1 x2) (t), vce(robust, nn(1))", s);
    check(reasonOf(o2) == "r(198)",
          "B2-4 nnmatch: vce(robust, nn(1)) is r(198) -- Stata's nn(#) floor is 2 (probe.log:189) "
          "-- got `" + reasonOf(o2) + "`");
    auto o3 = piet::dispatch("teffects nnmatch (y x1 x2) (t), nneighbor(500)", s);
    check(reasonOf(o3) == "r(198)",
          "B2-4 nnmatch: nneighbor(#) above the smallest group is r(198) (ParseNNeighbor) -- got `" +
              reasonOf(o3) + "`");
    auto o4 = piet::dispatch("teffects nnmatch (y x1 x2) (t), biasadj(x1 x2)", s);
    check(firstError(o4).value("code", std::string()) == "unsupported_option",
          "B2-4 nnmatch: biasadj() is a real Stata option this build does not implement -> "
          "option-refuse, never a silent default-OFF answer under a name the user typed");
    auto o5 = piet::dispatch("teffects nnmatch (y x1 x2) (t), vce(iid)", s);
    check(reasonOf(o5) == "refused:teffects_nnmatch_vce_iid",
          "B2-4 nnmatch: vce(iid) is a DIFFERENT variance (captured once for the record, never "
          "verified) and is refused BY NAME -- got `" + reasonOf(o5) + "`");
    auto o6 = piet::dispatch("teffects nnmatch (y x1 x2) (t), metric(matrix M)", s);
    check(reasonOf(o6) == "refused:teffects_nnmatch_metric_matrix",
          "B2-4 nnmatch: metric(matrix ...) is refused by name -- got `" + reasonOf(o6) + "`");
    auto o7 = piet::dispatch("teffects nnmatch (y i.x1#i.x2) (t)", s);
    check(reasonOf(o7) == "refused:teffects_nnmatch_interaction_terms",
          "B2-4 nnmatch: an interaction term is refused by name (no fixture ever expanded one) "
          "-- got `" + reasonOf(o7) + "`");
    auto o8 = piet::dispatch("teffects nnmatch (y L.x1) (t)", s);
    check(reasonOf(o8) == "refused:teffects_nnmatch_fv_operator",
          "B2-4 nnmatch: a time-series/base-level operator is refused by name -- got `" +
              reasonOf(o8) + "`");
    auto o9 = piet::dispatch("teffects nnmatch (y x1 x2) (t), ate atet", s);
    check(firstError(o9).value("code", std::string()) == "syntax_error",
          "B2-4 nnmatch: `ate atet' together is a syntax error (r(198) class), not a silent "
          "last-one-wins");
    (void)piet::dispatch("gen byte t3 = mod(_n, 3)", s);
    auto o10 = piet::dispatch("teffects nnmatch (y x1 x2) (t3)", s);
    check(reasonOf(o10) == "r(198)",
          "B2-4 nnmatch: a three-level treatment is r(198) in this build (control()/tlevel() were "
          "never measured) -- got `" + reasonOf(o10) + "`");
    // vce(robust, nn(#)) AT the smallest group (n1 == 99 here) is NOT a parse
    // error -- Stata accepts it and then dies at runtime, because Psi(i) draws
    // from the 98 OTHER treated rows. Measured on this exact fixture, StataBE
    // 2026-09-09: nn(99) -> r(459) "fewer than 99 nearest-neighbor matches for
    // observation 4 ..."; nn(100)/nn(101) -> r(198) (ParseVCE). Before this
    // check the engine truncated h silently and answered rc 0 with SE
    // .1046884 -- a wrong number under a number the user typed.
    auto o11 = piet::dispatch("teffects nnmatch (y x1 x2) (t), vce(robust, nn(99))", s);
    check(reasonOf(o11) == "r(459)",
          "B2-4 nnmatch: vce(robust, nn(#)) AT the smallest group is Stata's RUNTIME r(459), not "
          "a silently truncated h -- got `" + reasonOf(o11) + "`");
    check(firstError(o11).value("message", std::string()).find(
              "fewer than 99 nearest-neighbor matches for observation 4") != std::string::npos,
          "B2-4 nnmatch: ... carrying Stata's own per-observation text (it names the first "
          "deficient FRAME row, _n 4 -- the first treated row of this fixture)");
    auto o12 = piet::dispatch("teffects nnmatch (y x1 x2) (t), vce(robust, nn(100))", s);
    check(reasonOf(o12) == "r(198)",
          "B2-4 nnmatch: one ABOVE the smallest group is still ParseVCE's r(198) -- the two sides "
          "of the bound carry different rc -- got `" + reasonOf(o12) + "`");
  }
  {
    // The LEGAL boundary, h == nmin - 1: the arm that proves the r(459) above
    // refuses only what Stata refuses. StataBE 2026-09-09 on this fixture:
    // e(k_robust) 98, sqrt(e(V)) 1.04580722228670e-01.
    auto out = piet::dispatch("teffects nnmatch (y x1 x2) (t), vce(robust, nn(98))", s);
    check(!anyError(out), "B2-4 nnmatch: vce(robust, nn(nmin-1)) RUNS (the bound is off-by-one "
                          "tight, not one too wide)");
    const double* krob = s.eval.eScalars.find("k_robust");
    check(krob != nullptr && *krob == 98, "B2-4 nnmatch: nn(98) sets e(k_robust)=98");
    check(close(scalarOf("_se[ATE:r1vs0.t]"), 0.104580722228670, 1e-9),
          "B2-4 nnmatch: nn(98) SE == 0.104580722228670 (StataBE oracle) -- the largest h Stata "
          "accepts, and it is parity-clean");
  }
  {
    // The gen() collision rule as MEASURED, not as assumed (StataBE 19.5,
    // 2026-09-09, this fixture: default nneighbor(1), k_nnmax 2). Stata does NOT
    // scan the whole stub* family before estimating -- it refuses on the stubs
    // it can name (stub1..stub(nneighbor)) with one text, and on the ones only
    // the fitted K reveals with another. The middle leg is the one that was
    // WRONG before this pass: mm3 present with K == 2 runs in Stata.
    (void)piet::dispatch("drop mm1 mm2", s);
    (void)piet::dispatch("gen double mm3 = 1", s);
    auto g1 = piet::dispatch("teffects nnmatch (y x1 x2) (t), gen(mm)", s);
    check(!anyError(g1),
          "B2-4 nnmatch: gen(mm) with only mm3 present and k_nnmax 2 RUNS -- Stata rc 0, mm1/mm2 "
          "created, mm3 untouched (the 1..64 pre-scan used to refuse this)");
    (void)piet::dispatch("drop mm1 mm2", s);
    auto g2 = piet::dispatch("teffects nnmatch (y x1 x2) (t), nneighbor(3) gen(mm)", s);
    check(reasonOf(g2) == "r(110)" &&
              firstError(g2).value("message", std::string()).find("variable mm3 already defined") !=
                  std::string::npos,
          "B2-4 nnmatch: the SAME mm3 with nneighbor(3) IS r(110) `variable mm3 already defined' "
          "-- three stubs are guaranteed before a single distance is computed -- got `" +
              reasonOf(g2) + "`");
    (void)piet::dispatch("drop mm3", s);
    (void)piet::dispatch("gen double mm2 = 1", s);
    auto g3 = piet::dispatch("teffects nnmatch (y x1 x2) (t), gen(mm)", s);
    check(reasonOf(g3) == "r(110)" &&
              firstError(g3).value("message", std::string())
                      .find("maximum number of nearest neighbors is 2, but variable mm2 already "
                            "exists") != std::string::npos,
          "B2-4 nnmatch: a stub only the fitted K names carries Stata's OTHER text -- got `" +
              firstError(g3).value("message", std::string()) + "`");
    auto probe = piet::dispatch("display mm1[1]", s);
    check(anyError(probe),
          "B2-4 nnmatch: ... and it refuses BEFORE minting mm1, so a collision at stub2 leaves no "
          "half-written stub family behind");
    (void)piet::dispatch("drop mm2", s);
  }
  {
    // A REPEATED matching variable is a collinear design, not a syntax error.
    // The first landing refused `(y x1 x1)` with an invented r(198) "duplicate
    // matching variable x1"; StataBE 19.5 (2026-09-09, this fixture) answers
    // `note: x1 omitted because of collinearity.` with ATE 1.47459772296015e-01,
    // SE 5.11611461378533e-02, e(k_nnmin) 3, e(k_nnmax) 6 -- and the kernel's
    // own _rmcoll sweep reproduces every one of those to the last digit once the
    // CLI stops preempting it.
    auto out = piet::dispatch("teffects nnmatch (y x1 x1) (t)", s);
    check(!anyError(out),
          "B2-4 nnmatch: a repeated matching variable RUNS -- got `" + reasonOf(out) + "`");
    bool note = false;
    for (const auto& blk : out)
      if (blk.value("type", std::string()) == "display" &&
          blk.value("text", std::string()).find("collinear") != std::string::npos)
        note = true;
    check(note, "B2-4 nnmatch: ... and the repeat is NAMED as omitted for collinearity");
    check(close(scalarOf("_b[ATE:r1vs0.t]"), 0.147459772296015, 1e-9) &&
              close(scalarOf("_se[ATE:r1vs0.t]"), 0.0511611461378533, 1e-9),
          "B2-4 nnmatch: `(y x1 x1)' ATE/SE == 0.147459772296015 / 0.0511611461378533 (StataBE "
          "oracle) -- matching on x1 alone, which is what dropping the repeat means");
    const double* kmin = s.eval.eScalars.find("k_nnmin");
    const double* kmax = s.eval.eScalars.find("k_nnmax");
    check(kmin != nullptr && *kmin == 3 && kmax != nullptr && *kmax == 6,
          "B2-4 nnmatch: ... e(k_nnmin)=3 e(k_nnmax)=6 -- one covariate ties far more widely than "
          "two, so the tie WIDTH is the discriminating leg here");
  }
  {
    // vce(robust, nn(3)) changes h, and the SE with it: the DOC arm captured on
    // this very fixture (oracle.txt `DOC F1 vce(robust,nn(3))`, SE
    // +1.fef018e6e2ca4X-005), reproduced by the verified rule with h=3.
    auto out = piet::dispatch("teffects nnmatch (y x1 x2) (t), vce(robust, nn(3))", s);
    check(!anyError(out), "B2-4 nnmatch: vce(robust, nn(3)) runs");
    const double* krob = s.eval.eScalars.find("k_robust");
    check(krob != nullptr && *krob == 3, "B2-4 nnmatch: vce(robust, nn(3)) sets e(k_robust)=3");
    check(close(scalarOf("_se[ATE:r1vs0.t]"), 0.06237034667636834, 1e-9),
          "B2-4 nnmatch: nn(3) SE == 0.0623703466763683 (DOC F1 vce(robust,nn(3)) "
          "+1.fef018e6e2ca4X-005) -- the conditional-variance neighbour count is REAL, not "
          "decorative");
  }
  {
    // `if`: the sample shrinks but the ids stay _n of the FULL frame. MEASURED
    // on an ODD-ROWS-only subsample (INFO|nn_edge|ifodd_*): Stata answers 47 and
    // 131, both odd, and ZERO even ids -- so a handler that renumbered to the
    // kept subsample would pair paper 076 with the wrong people at rc 0.
    auto out = piet::dispatch("teffects nnmatch (y x1 x2) (t) if mod(_n,2)==1, gen(ifm)", s);
    check(!anyError(out), "B2-4 nnmatch: an `if' qualifier runs");
    const double* N = s.eval.eScalars.find("N");
    check(N != nullptr && *N == 100, "B2-4 nnmatch: e(N)==100 under `if mod(_n,2)==1'");
    check(cellOf("ifm1", 1) == 47 && cellOf("ifm1", 3) == 131,
          "B2-4 nnmatch: under `if', ifm1[1]=47 and ifm1[3]=131 (INFO|nn_edge|ifodd_mm1_*)");
    check(countIf("!missing(ifm1)") == 100 && countIf("!missing(ifm1) & mod(ifm1,2)==0") == 0,
          "B2-4 nnmatch: exactly the 100 kept rows carry ids and EVERY id is odd -- the ids are "
          "row numbers of the CURRENT data, not of the kept subsample");
  }
  {
    // The production shape of clean_15_matching.do:366 and the consumer's join.
    (void)piet::doexec::executeDoSource(
        "clear\nset obs 600\n"
        "gen byte f1 = mod(_n, 2)\n"
        "gen byte f2 = mod(floor(_n / 3), 3)\n"
        "gen byte f3 = mod(floor(_n / 5), 5)\n"
        "gen byte f4 = mod(floor(_n / 11), 4)\n"
        "gen byte f5 = mod(floor(_n / 13), 2)\n"
        "gen byte f6 = mod(floor(_n / 17), 3)\n"
        "gen byte treatment = (mod(_n, 5) == 0)\n"
        "gen double outcome = 1 + 0.5*f1 + 0.3*f2 - 0.2*f3 + 0.1*f4 - 0.4*f5 + 0.15*f6 + "
        "0.25*treatment + mod(_n * 13, 37)/37\n",
        s, piet::dispatch);
    auto out = piet::dispatch(
        "teffects nnmatch (outcome i.f1 i.f2 i.f3 i.f4 i.f5 i.f6) (treatment), gen(match_id)", s);
    check(!anyError(out), "B2-4 nnmatch: the paper-076 shape (six i. factors + gen()) runs");
    check(close(scalarOf("_b[ATE:r1vs0.treatment]"), 0.22694069069068998, 1e-9),
          "B2-4 nnmatch: production-shape ATE == 0.226940690690690 (+1.d0c647e52f724X-003)");
    const double* kmax = s.eval.eScalars.find("k_nnmax");
    check(kmax != nullptr && *kmax == 3 && s.eval.eStrings.get("indexvar", "") ==
                                               "match_id1 match_id2 match_id3",
          "B2-4 nnmatch: K = e(k_nnmax) = 3 gen columns -- the count comes from the WIDEST tie, "
          "not from nneighbor()");
    check(cellOf("match_id1", 1) == 575 && cellOf("match_id1", 5) == 309 &&
              cellOf("match_id1", 365) == 319 && cellOf("match_id1", 530) == 132 &&
              cellOf("match_id1", 600) == 554,
          "B2-4 nnmatch: match_id1 at rows 1/5/365/530/600 == 575/309/319/132/554. Rows 365 and "
          "530 are EXACT-CELL (distance 0) ties, where the order is ASCENDING observation "
          "number -- the opposite of the nonzero-distance rule, and the whole reason `sort PNR' "
          "matters to the paper");
    check(countIf("treatment==1 & !missing(match_id1)") == 120,
          "B2-4 nnmatch: every one of the 120 treated rows has a match_id1 -- the paper's "
          "`keep if treatment==1 & !missing(match_id1)' then `merge 1:m' is lossless");
  }
  {
    // atet on indicator cells: the CONTROL rows carry ids too, because Omega is
    // built for every row and not only the treated ones.
    (void)piet::doexec::executeDoSource(
        "clear\nset obs 90\n"
        "gen byte a = mod(_n, 2)\n"
        "gen byte b = mod(floor(_n / 2), 3)\n"
        "gen byte c = mod(floor(_n / 5), 5)\n"
        "gen byte t = (mod(_n, 5) != 0)\n"
        "gen double y = 1 + 0.5*a + 0.3*b - 0.2*c + 0.25*t + mod(_n * 11, 31)/31\n",
        s, piet::dispatch);
    auto out = piet::dispatch("teffects nnmatch (y i.a i.b i.c) (t), atet nneighbor(2) gen(am)", s);
    check(!anyError(out), "B2-4 nnmatch: `atet nneighbor(2)' runs");
    check(s.eval.eStrings.get("stat", "") == "atet",
          "B2-4 nnmatch: e(stat)=atet");
    check(close(scalarOf("_b[ATET:r1vs0.t]"), 0.21807795698924731, 1e-9),
          "B2-4 nnmatch: ATET == 0.218077956989247 and the e(b) column is `ATET:r1vs0.t'");
    check(close(scalarOf("_se[ATET:r1vs0.t]"), 0.12234534456996479, 1e-9),
          "B2-4 nnmatch: ATET SE == 0.122345344569965. The manual's TYPESET sigma^2_delta puts "
          "t_i on the whole bracket and is FALSIFIED here; Abadie-Imbens (2006) Thm 7, with the "
          "K-term on the CONTROLS, is what Stata computes");
    check(countIf("t==0 & !missing(am1)") == 18,
          "B2-4 nnmatch: all 18 control rows carry ids under atet (control_rows_filled)");
    check(cellOf("am1", 1) == 25 && cellOf("am1", 5) == 59 && cellOf("am1", 90) == 18,
          "B2-4 nnmatch: atet ids at rows 1/5/90 == 25/59/18");
  }
  {
    // NESTED FACTORS: 1.a == 2.b + 3.b exactly, so _rmcoll must drop a column
    // before the pooled covariance is formed. MEASURED (INFO|nn_rmcoll):
    // e(fvdvarlist) = `0b.a 1.a 0b.b 1.b 2.b 3o.b' -- Stata keeps 1.a and drops
    // the LATER redundant 3.b.
    (void)piet::doexec::executeDoSource(
        "clear\nset obs 120\n"
        "gen byte b = mod(_n, 4)\n"
        "gen byte a = (b >= 2)\n"
        "gen byte t = mod(floor((_n - 1) / 3), 2)\n"
        "gen double y = 1 + 0.5*a + 0.3*b + 0.25*t + mod(_n * 11, 31)/31\n",
        s, piet::dispatch);
    auto out = piet::dispatch("teffects nnmatch (y i.a i.b) (t), gen(rm)", s);
    check(!anyError(out),
          "B2-4 nnmatch: a nested-factor design runs -- without _rmcoll the pooled covariance is "
          "singular and there is no answer at all");
    bool note = false;
    for (const auto& blk : out)
      if (blk.value("type", std::string()) == "display" &&
          blk.value("text", std::string()).find("collinear") != std::string::npos)
        note = true;
    check(note, "B2-4 nnmatch: the dropped column is NAMED in a note, never dropped silently");
    check(close(scalarOf("_b[ATE:r1vs0.t]"), 0.27701612903225808, 1e-9) &&
              close(scalarOf("_se[ATE:r1vs0.t]"), 0.058824155719795081, 1e-9),
          "B2-4 nnmatch: nested-factor ATE/SE == 0.277016129032258 / 0.0588241557197951 "
          "(+1.1baa1dbaa1dbaX-002) -- drop the WRONG column and the metric, hence every id, "
          "moves");
    check(cellOf("rm1", 1) == 5 && cellOf("rm1", 2) == 6 && cellOf("rm1", 120) == 8,
          "B2-4 nnmatch: nested-factor ids at rows 1/2/120 == 5/6/8");
  }
  {
    // ---- B2-4 review pass (2026-09-09) -----------------------------------
    // Five axes NO gate read before: the reported interval, level(#), e(level),
    // e(mvarlist)'s operators, e(tlevels)'s coding, and the data_mutation
    // envelope for gen(). Every pin below is a StataBE 19.5 capture in
    // tests/eval/golden/aer_estim2_oracle.log (ORACLEX|...ci_lo/ci_hi and the
    // INFO|nn_level| block); all six legs are RED on the pre-review binary.
    (void)piet::doexec::executeDoSource(
        "clear\nset obs 200\n"
        "gen double x1 = mod(_n * 7, 23) / 23\n"
        "gen double x2 = mod(_n * 13, 17) / 17\n"
        "gen byte   g  = mod(_n, 2)\n"
        "gen byte   t  = mod(floor((_n - 1) / 3), 2)\n"
        "gen double y  = 1 + 2*x1 - 0.5*x2 + 0.3*g + 0.25*t + mod(_n * 11, 31)/31 - 0.5\n",
        s, piet::dispatch);
    auto ciOf = [](const nlohmann::ordered_json& blocks, const char* field) {
      for (const auto& b : blocks) {
        if (!b.is_object() || b.value("type", std::string()) != "estimation") continue;
        if (!b.contains("coeftable") || !b["coeftable"].is_array() || b["coeftable"].empty())
          continue;
        const auto& row = b["coeftable"][0];
        if (row.contains(field) && row[field].is_number()) return row[field].get<double>();
      }
      return std::nan("");
    };
    auto out = piet::dispatch("teffects nnmatch (y x1 x2) (t)", s);
    check(close(ciOf(out, "ci_lo"), -0.088718214679211385, 1e-9) &&
              close(ciOf(out, "ci_hi"), 0.13990054286995485, 1e-9),
          "B2-4 nnmatch: the DEFAULT interval is Stata's 95% one, [-.0887182, .1399005] "
          "(ORACLEX ci_lo -1.6b63ca69b5201X-004 / ci_hi +1.1e842d028d856X-003). z_critical takes "
          "the WHOLE two-sided tail: passing 0.025 built a 97.5% interval wearing a 95% label");
    check(std::isnan(scalarOf("e(level)")),
          "B2-4 nnmatch: e(level) is NOT posted -- `display e(level)' answers `.' in StataBE "
          "(INFO|nn_level|default_e_level) and the 14-fixture ereturn capture has no such scalar");
    auto o90 = piet::dispatch("teffects nnmatch (y x1 x2) (t), level(90)", s);
    check(close(ciOf(o90, "ci_lo"), -0.070340290643101957, 1e-9) &&
              close(ciOf(o90, "ci_hi"), 0.12152261883384541, 1e-9),
          "B2-4 nnmatch: level(90) MOVES the interval to [-.07034029, .12152262] "
          "(INFO|nn_level|l90_lo/l90_hi) -- a value-carrying reporting option, not an "
          "accept-note");
    check(std::isnan(scalarOf("e(level)")),
          "B2-4 nnmatch: level(90) still posts no e(level) (INFO|nn_level|l90_e_level)");
    auto l9 = piet::dispatch("teffects nnmatch (y x1 x2) (t), level(9)", s);
    auto l101 = piet::dispatch("teffects nnmatch (y x1 x2) (t), level(101)", s);
    auto labc = piet::dispatch("teffects nnmatch (y x1 x2) (t), level(abc)", s);
    auto l9999 = piet::dispatch("teffects nnmatch (y x1 x2) (t), level(99.99)", s);
    check(reasonOf(l9) == "r(198)" && reasonOf(l101) == "r(198)" && reasonOf(labc) == "r(198)" &&
              !anyError(l9999),
          "B2-4 nnmatch: level()'s band is Stata's MEASURED one -- 9/101/abc are r(198) and "
          "99.99 runs (INFO|nn_level|l9_rc l101_rc labc_rc l9999_rc = 198 198 198 0)");
    (void)piet::dispatch("gen byte a = mod(_n, 2)", s);
    (void)piet::dispatch("gen byte b = mod(floor((_n - 1) / 2), 3)", s);
    (void)piet::dispatch("teffects nnmatch (y x1 i.a i.b) (t)", s);
    const std::string mvMixed = s.eval.eStrings.get("mvarlist", "");
    (void)piet::dispatch("teffects nnmatch (y c.x1 i.a) (t)", s);
    const std::string mvCdot = s.eval.eStrings.get("mvarlist", "");
    (void)piet::dispatch("teffects nnmatch (y i.a i.b) (t)", s);
    const std::string mvFac = s.eval.eStrings.get("mvarlist", "");
    check(mvMixed == "x1 a b" && mvCdot == "x1 a" && mvFac == "a b",
          "B2-4 nnmatch: e(mvarlist) is the TYPED order with the operator STRIPPED -- Stata "
          "answers `x1 a b' / `x1 a' / `a b' (INFO|nn_level|mixed_mvarlist, |cdot_mvarlist, "
          "INFO|nn_rmcoll|mvarlist), got `" + mvMixed + "' / `" + mvCdot + "' / `" + mvFac + "'");
    (void)piet::dispatch("gen byte t12 = t + 1", s);
    (void)piet::dispatch("teffects nnmatch (y x1 x2) (t12)", s);
    const std::string tl12 = s.eval.eStrings.get("tlevels", "");
    (void)piet::dispatch("teffects nnmatch (y x1 x2) (t)", s);
    const std::string tl01 = s.eval.eStrings.get("tlevels", "");
    check(tl12 == "1 2" && tl01 == "0 1",
          "B2-4 nnmatch: e(tlevels) FOLLOWS the coding -- `1 2' on a 1/2-coded treatment "
          "(INFO|nn_level|t12_tlevels) and `0 1' on the 0/1 one. The literal `0 1' contradicted "
          "e(treated)=2/e(control)=1 and the colname `ATE:r2vs1.t12' in its own e() set; got `" +
              tl12 + "' / `" + tl01 + "'");
    auto og = piet::dispatch("teffects nnmatch (y x1 x2) (t), gen(revmm)", s);
    bool sawMut = false;
    for (const auto& b : og) {
      if (!b.is_object() || b.value("type", std::string()) != "data_mutation") continue;
      if (!b.contains("varlist_after") || !b["varlist_after"].is_array()) continue;
      bool m1 = false, m2 = false;
      for (const auto& v : b["varlist_after"]) {
        const std::string nm = v.value("name", std::string());
        if (nm == "revmm1") m1 = true;
        if (nm == "revmm2") m2 = true;
      }
      if (m1 && m2) sawMut = true;
    }
    check(sawMut,
          "B2-4 nnmatch: gen() returns the data_mutation envelope naming revmm1/revmm2 -- "
          "mutation_result.h's contract, and the ONLY source routes.cpp has for the /api/command "
          "`varlist_after' the console's Variables pane reads (app.js:1007)");
  }
  {
    // Every OTHER teffects estimator is a different method with its own
    // inference, and each says so under its OWN name -- not the shared
    // `e3_deferred` (which now means "not routed at all") and not a wrong
    // number under a name the user typed.
    static const char* kSubs[] = {"ra", "ipw", "ipwra", "aipw", "psmatch"};
    int named = 0;
    for (const char* sub : kSubs) {
      auto out = piet::dispatch(std::string("teffects ") + sub + " (y a) (t b)", s);
      if (reasonOf(out) == std::string("refused:teffects_") + sub + "_unimplemented") ++named;
    }
    check(named == 5,
          "B2-4 nnmatch: ra/ipw/ipwra/aipw/psmatch each refuse under their OWN name "
          "(`refused:teffects_<sub>_unimplemented') -- got " + std::to_string(named) + "/5");
    auto out = piet::dispatch("teffects", s);
    check(firstError(out).value("code", std::string()) == "syntax_error",
          "B2-4 nnmatch: a bare `teffects' is a syntax error naming the estimators, not a crash");
    auto o2 = piet::dispatch("teffects nnmatch (y a)", s);
    check(firstError(o2).value("code", std::string()) == "syntax_error",
          "B2-4 nnmatch: one parenthesised group is a syntax error");
  }
  fs::remove_all(workdir, ec);
}
