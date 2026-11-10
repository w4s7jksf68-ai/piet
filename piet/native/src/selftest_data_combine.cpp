// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: data combine.
#include "selftest_support.h"
#include "selftest_cases.h"



// ── AER-SCOPE1 bundle F (RESHAPE, 2026-09-04) ────────────────────────────────
//
// Seventeen verbs that RESHAPE OR REPLACE the frame. Their whole failure class
// is rc 0 with the wrong frame, and every wrong implementation this bundle can
// plausibly ship returns 0: a `split` that counts three parts and writes three
// empty columns, an `insobs` that copies the last row forward instead of
// appending missing, a `pctile` that broadcasts one percentile down the whole
// column, a `bsample, cluster()` that resamples ROWS and calls them clusters,
// an `input` that reads two rows of text as two rows of nothing, a `recast`
// that round-trips the column through a CSV and moves the ninth digit. So NO
// leg below stops at a return code: each one reads a VALUE back out of the
// frame the verb produced, and the value chosen is one the wrong
// implementation named in its comment would get wrong.
//
// TWO HOUSE RULES ARE FOLLOWED LITERALLY HERE BECAUSE BOTH HAVE COST TIME:
//   - EVERY session sets `cwd` as well as `eval.cwd`. resolveUserPath reads
//     SessionState::cwd (fileio/user_paths.cpp); a session that seeds only
//     eval.cwd writes leg (xi)'s .dta into the PROCESS cwd -- the repo root --
//     and the round-trip half of that leg fails for a reason that has nothing
//     to do with `notes`.
//   - Numeric comparisons report through `fullPrec` (%.17g), never
//     std::to_string, which prints six decimals and renders a genuine
//     8th-digit difference as two identical strings.
//
// Nothing here asserts a PATH: the workdir is this process's temp directory.
// What is asserted is row counts, column contents and metadata.
void testAerReshape() {
  std::printf("[selftest] --- AER-SCOPE1 bundle F: split/xpose/insobs/stack/cumul/pctile + the "
              "three sampling verbs + input/notes/recast + the refusals ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_aerreshape").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto exec = [](const std::string& src, piet::SessionState& session) {
    return piet::doexec::executeDoSource(src, session, piet::dispatch);
  };
  // Returns a NULL json when there is no error block rather than throwing --
  // the BUG-129 trap, and check()'s message argument is built EAGERLY (on the
  // passing path too), so every message below reads through these helpers.
  auto firstError = [](const nlohmann::ordered_json& blocks) -> nlohmann::ordered_json {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return b;
    return nlohmann::ordered_json();
  };
  auto errText = [](const nlohmann::ordered_json& e) {
    return e.is_object() ? e.value("message", std::string()) : std::string("<none>");
  };
  auto errField = [](const nlohmann::ordered_json& e, const std::string& f) {
    return e.is_object() ? e.value(f, std::string()) : std::string("<none>");
  };
  auto rcOf = [](const nlohmann::ordered_json& e) {
    return e.is_object() ? piet::eval::coarseRc(e) : std::string("<none>");
  };
  // See the header: %.17g, because `-77.551724` twice is not a failure report.
  auto fullPrec = [](double v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return std::string(buf);
  };
  auto has = [](const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
  };
  auto displays = [](const auto& blocks) {
    std::vector<std::string> v;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "display")
        v.push_back(b.value("text", std::string()));
    return v;
  };
  auto displayValue = [&](const auto& blocks, const std::string& prefix) -> double {
    for (const auto& s : displays(blocks)) {
      if (s.rfind(prefix, 0) != 0) continue;
      try {
        return std::stod(s.substr(prefix.size()));
      } catch (...) {
        return std::nan("");
      }
    }
    return std::nan("");
  };
  auto varNames = [](piet::SessionState& s) {
    std::vector<std::string> out;
    const nlohmann::ordered_json blocks = piet::dispatch("describe", s);
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "describe" && b.contains("variables"))
        for (const auto& v : b["variables"]) out.push_back(v.value("name", std::string()));
    return out;
  };
  auto hasName = [](const std::vector<std::string>& v, const std::string& n) {
    return std::find(v.begin(), v.end(), n) != v.end();
  };
  // describe's per-variable `dtype` IS varMeta.stataType when one is recorded
  // and the "str#"/"float" default otherwise, which is exactly what legs (i)
  // and (xii) need to distinguish.
  auto dtypeOf = [](piet::SessionState& s, const std::string& name) -> std::string {
    const nlohmann::ordered_json blocks = piet::dispatch("describe", s);
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "describe" && b.contains("variables"))
        for (const auto& v : b["variables"])
          if (v.value("name", std::string()) == name) return v.value("dtype", std::string());
    return std::string("<absent>");
  };
  // r() is REPLACED wholesale by a summarize (macros.cpp's captureRFromBlock),
  // so every read below happens immediately after the command that published
  // it -- never two commands later.
  auto rOf = [](piet::SessionState& s, const char* key) -> double {
    const double* p = s.eval.rValues.find(key);
    return p ? *p : std::nan("");
  };
  auto runR = [&](piet::SessionState& s, const std::string& cmd, const char* key) -> double {
    (void)piet::dispatch(cmd, s);
    return rOf(s, key);
  };
  // `count` with no `if` is _N. Used instead of `display _N` so the row count
  // and the conditional counts below come off the same surface.
  auto nRows = [&](piet::SessionState& s) { return runR(s, "count", "N"); };
  auto countIf = [&](piet::SessionState& s, const std::string& expr) {
    return runR(s, "count if " + expr, "N");
  };

  // ── (i) `input` builds a dataset out of a BLOCK. ───────────────────────────
  // The block is folded into one line at preprocess time, so this leg also
  // proves captureInputBlocks ran: an unfolded block dispatches `1 2` as a
  // command and the run halts. N==3 alone is true of an input that read three
  // rows of garbage, so the SUM is asserted too -- that is the half that says
  // the digits survived the fold, the split and the CSV staging.
  {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp).
    auto r = exec("clear\ninput x y\n1 2\n3 4\n5 6\nend\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-RESHAPE (i): `input x y` ... `end` runs clean into an empty session. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(nRows(s) == 3.0,
          "AER-RESHAPE (i): ...and leaves THREE rows. Got " + fullPrec(nRows(s)));
    const double sx = runR(s, "summarize x", "sum");
    check(sx == 9.0,
          "AER-RESHAPE (i): ...x sums to 9 (1+3+5). A row count is satisfied by an input that "
          "created three EMPTY rows, which is what a fold that dropped the data lines produces. "
          "Got " + fullPrec(sx));
    const double sy = runR(s, "summarize y", "sum");
    check(sy == 12.0,
          "AER-RESHAPE (i): ...and y sums to 12 (2+4+6), so the SECOND column got the second "
          "field of each row rather than a copy of the first. Got " + fullPrec(sy));
    check(dtypeOf(s, "x") != "str#",
          "AER-RESHAPE (i): ...and x is NUMERIC. A bare varlist declares numbers; storing them as "
          "strings makes every downstream `summarize`/`regress` fail on a dataset that lists "
          "correctly. Got dtype `" + dtypeOf(s, "x") + "`");

    // The `str#` prefix form: `s` must come back a string column.
    auto r2 = exec("clear\ninput str5 s n\n\"aa\" 1\n\"bb\" 2\nend\n", s);
    check(firstError(r2.blocks).is_null(),
          "AER-RESHAPE (i/str): `input str5 s n` runs clean. Got `" +
              errText(firstError(r2.blocks)) + "`");
    check(nRows(s) == 2.0,
          "AER-RESHAPE (i/str): ...two rows. Got " + fullPrec(nRows(s)));
    check(dtypeOf(s, "s") != "float" && dtypeOf(s, "s") != "<absent>",
          "AER-RESHAPE (i/str): ...and `str5 s` produced a STRING column, not a numeric one full "
          "of missing -- which is what an input that ignored the type prefix produces, and it "
          "reads as a legitimate all-missing variable. Got dtype `" + dtypeOf(s, "s") + "`");
    check(countIf(s, "s == \"bb\"") == 1.0,
          "AER-RESHAPE (i/str): ...carrying the QUOTED literal, unquoted, in the right row. Got " +
              fullPrec(countIf(s, "s == \"bb\"")));
    check(runR(s, "summarize n", "sum") == 3.0,
          "AER-RESHAPE (i/str): ...and the numeric column beside it still parsed (1+2). Got " +
              fullPrec(runR(s, "summarize n", "sum")));
  }

  // ── (ii) `split` decomposes a string column into k real columns. ───────────
  // r(nvars)==3 is satisfied by a handler that counted the parts and wrote
  // nothing (or wrote them all into stub1), so all three parts are read back.
  {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp).
    auto r = exec("clear\nset obs 6\ngen str9 sv = \"a-b-c\"\nsplit sv, parse(\"-\") gen(p)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-RESHAPE (ii): `split sv, parse(\"-\") gen(p)` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const double nv = rOf(s, "nvars");
    check(nv == 3.0,
          "AER-RESHAPE (ii): r(nvars) == 3 -- three parts, so the parse() delimiter was honoured "
          "rather than the whitespace default (which would find ONE part in \"a-b-c\" and is the "
          "exact reason `gen = word(v,j)` was rejected as the implementation). Got " +
              fullPrec(nv));
    const std::vector<std::string> names = varNames(s);
    check(hasName(names, "p1") && hasName(names, "p2") && hasName(names, "p3"),
          "AER-RESHAPE (ii): ...and the columns are named p1 p2 p3 off gen()'s stub, not off the "
          "source variable");
    check(countIf(s, "p1 == \"a\"") == 6.0 && countIf(s, "p2 == \"b\"") == 6.0 &&
              countIf(s, "p3 == \"c\"") == 6.0,
          "AER-RESHAPE (ii): ...and every row of every part carries its OWN piece. This is the "
          "check r(nvars) cannot make: a split that created three columns and left them empty, or "
          "wrote the whole string into p1, satisfies the count and the names. Got p1=" +
              fullPrec(countIf(s, "p1 == \"a\"")) + " p2=" + fullPrec(countIf(s, "p2 == \"b\"")) +
              " p3=" + fullPrec(countIf(s, "p3 == \"c\"")));

    // `destring`: parts that are entirely numeric become f64.
    auto r2 = exec("clear\nset obs 4\ngen str9 sn = \"1-2\"\n"
                   "split sn, parse(\"-\") gen(q) destring\n", s);
    check(firstError(r2.blocks).is_null(),
          "AER-RESHAPE (ii/destring): `split ..., destring` runs clean. Got `" +
              errText(firstError(r2.blocks)) + "`");
    check(dtypeOf(s, "q1") != "str#",
          "AER-RESHAPE (ii/destring): ...and q1 came back NUMERIC. A destring that silently left "
          "the parts as strings still lists as `1` and fails every arithmetic use of the column. "
          "Got dtype `" + dtypeOf(s, "q1") + "`");
    const double q2sum = runR(s, "summarize q2", "sum");
    check(q2sum == 8.0,
          "AER-RESHAPE (ii/destring): ...and it holds the NUMBER (4 rows x 2 == 8), so the "
          "conversion read the digits rather than producing an all-missing numeric column. Got " +
              fullPrec(q2sum));
  }

  // ── (iii) ONE percentile rule: `_pctile` == `summarize, detail`. ───────────
  // The dataset is 1..10 -- EVEN N with distinct central values -- on purpose.
  // At even N the three plausible rules separate:
  //     Stata / Frame::xtile / winsor2 : p50 = (5+6)/2 = 5.5, p25 = 3
  //     "order statistic"              : p50 = 5,           p25 = 3
  //     numpy linear interpolation     : p50 = 5.5,         p25 = 3.25
  // so p50 falsifies the order-statistic rule and p25 falsifies the numpy one.
  // An odd-N fixture makes all three agree and this leg would assert nothing.
  //
  // BOTH halves are asserted, and separately: the EQUALITY says the product has
  // one percentile rule instead of two, and the ABSOLUTE value says that one
  // rule is Stata's. Equality alone is satisfied by two identical wrong rules,
  // which is the failure that ships quietly.
  {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp).
    auto r = exec("clear\nset obs 10\ngen double x = _n\n_pctile x, p(25 50)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-RESHAPE (iii): `_pctile x, p(25 50)` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    // Read r(r1)/r(r2) BEFORE the summarize below: captureRFromBlock REPLACES
    // the whole r() store on a summarize block, so reading them afterwards
    // would read nothing and the equality would compare two NaNs.
    const double r1 = rOf(s, "r1");
    const double r2v = rOf(s, "r2");
    check(r2v == 5.5,
          "AER-RESHAPE (iii): _pctile's p50 over 1..10 is 5.5 -- Stata averages the two straddling "
          "order statistics when N*p/100 lands on an integer. 5 means the handler took the lower "
          "order statistic (the `winsor` rule, which bundle A had to keep SEPARATE from winsor2 "
          "for exactly this reason). Got " + fullPrec(r2v));
    check(r1 == 3.0,
          "AER-RESHAPE (iii): ...and its p25 is 3, not 3.25. 3.25 is numpy's linear-interpolation "
          "percentile, which is what handlers.cpp:1096's FALLBACK summarize path computes -- so "
          "this is the number that catches a handler wired to that lambda instead of to the "
          "xtile/winsor2 rule the bundle contracted for. Got " + fullPrec(r1));
    (void)piet::dispatch("summarize x, detail", s);
    const double p50 = rOf(s, "p50");
    const double p25 = rOf(s, "p25");
    check(!std::isnan(p50) && p50 == r2v,
          "AER-RESHAPE (iii): `_pctile`'s p50 EQUALS `summarize, detail`'s p50, BIT-EXACT. Two "
          "percentile rules in one product is a silently wrong number in whichever surface the "
          "user did not check; this is the leg that says there is one. Got _pctile " +
              fullPrec(r2v) + " vs summarize,detail " + fullPrec(p50));
    check(!std::isnan(p25) && p25 == r1,
          "AER-RESHAPE (iii): ...and so does p25, which is the percentile where the two candidate "
          "rules actually differ at this N -- p50 agrees between Stata's rule and numpy's, so p50 "
          "alone would not have separated them. Got _pctile " + fullPrec(r1) +
              " vs summarize,detail " + fullPrec(p25));
  }

  // ── (iv) `cumul` is the empirical CDF, and `equal` changes the answer. ─────
  {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp).
    auto r = exec("clear\nset obs 10\ngen double x = _n\ncumul x, gen(cx)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-RESHAPE (iv): `cumul x, gen(cx)` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    (void)piet::dispatch("summarize cx", s);
    const double cmax = rOf(s, "max");
    const double cmin = rOf(s, "min");
    check(cmax == 1.0,
          "AER-RESHAPE (iv): the CDF reaches EXACTLY 1 at the top. 0.9 means the denominator was "
          "N+1, 1.1 means it was N-1; both look like a CDF and neither is one. Got " +
              fullPrec(cmax));
    // cumul's ECDF column is a FLOAT, as cumul.ado makes it
    // (w2as_summary_stats_family.log:7931 `float|%9.0g|ECDF of x`, and :7938 reads
    // float-rounded values back), so 1/N arrives as float(0.1), not the double 0.1.
    check(cmin == static_cast<double>(0.1f),
          "AER-RESHAPE (iv): ...and starts at 1/N == float(0.1), so the ranks are 1-based. A "
          "0-based rank gives 0 here and shifts every value in the column. Got " +
              fullPrec(cmin));

    // `equal`: tied observations all take the MAXIMUM rank. Without it the
    // ties take their own positions, so the two halves of `tt` would read
    // 0.1..0.5 and 0.6..1.0 instead of a flat 0.5 and 1.0.
    auto r2 = exec("clear\nset obs 10\ngen double tt = 1\nreplace tt = 2 in 6/10\n"
                   "cumul tt, gen(ct) equal\n", s);
    check(firstError(r2.blocks).is_null(),
          "AER-RESHAPE (iv/equal): `cumul tt, gen(ct) equal` runs clean. Got `" +
              errText(firstError(r2.blocks)) + "`");
    check(countIf(s, "ct == 0.5") == 5.0 && countIf(s, "ct == 1") == 5.0,
          "AER-RESHAPE (iv/equal): with `equal`, all five tied rows share the MAXIMUM rank -- 0.5 "
          "for the first level and 1 for the second. An implementation that accepted `equal` and "
          "ignored it produces ten distinct values, every one of them a plausible CDF number, and "
          "no rc says so. Got n(0.5)=" + fullPrec(countIf(s, "ct == 0.5")) + " n(1)=" +
              fullPrec(countIf(s, "ct == 1")));
  }

  // ── (v) `insobs 2, before(1)` puts two EMPTY rows at the FRONT. ───────────
  {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp).
    auto r = exec("clear\nset obs 5\ngen double x = _n\ninsobs 2, before(1)\n"
                  "display \"RS_INS1=\" x[1]\n"
                  "display \"RS_INS3=\" x[3]\n"
                  "display \"RS_INS7=\" x[7]\n",
                  s);
    check(firstError(r.blocks).is_null(),
          "AER-RESHAPE (v): `insobs 2, before(1)` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(nRows(s) == 7.0,
          "AER-RESHAPE (v): ...and the frame grew by exactly 2, to 7. Got " +
              fullPrec(nRows(s)));
    (void)piet::dispatch("summarize x", s);
    const double nNonMiss = rOf(s, "N");
    const double xSum = rOf(s, "sum");
    check(nNonMiss == 5.0 && xSum == 15.0,
          "AER-RESHAPE (v): ...and exactly the ORIGINAL five values are still there (1+..+5 == "
          "15) with nothing invented in the new rows. An insobs that copied a neighbouring row "
          "forward -- the cheapest way to make an append `work` -- lands 7 non-missing here and "
          "satisfies the row count above. Got N " + fullPrec(nNonMiss) + " sum " + fullPrec(xSum));
    // The POSITION half, read through subscripts rather than `summarize ... in`
    // so a missing cell is observable as one: x[1] must be missing (which is
    // what a non-numeric display renders as, hence the NaN), and the original
    // block must have moved down by exactly two -- x[3] == 1 and x[7] == 5.
    // x[3] is what proves the display path itself works, so the NaN above is
    // evidence of a missing cell and not of a broken subscript.
    const double ins1 = displayValue(r.blocks, "RS_INS1=");
    const double ins3 = displayValue(r.blocks, "RS_INS3=");
    const double ins7 = displayValue(r.blocks, "RS_INS7=");
    check(ins3 == 1.0 && ins7 == 5.0,
          "AER-RESHAPE (v): the two new rows went to the FRONT: the original block starts at row "
          "3 (x[3] == 1) and ends at row 7 (x[7] == 5). This is the entire content of `before(1)` "
          "-- an insobs that took the default end-append path passes every other check in this "
          "leg. Got x[3] " + fullPrec(ins3) + " x[7] " + fullPrec(ins7));
    check(std::isnan(ins1),
          "AER-RESHAPE (v): ...and row 1 is MISSING, not 0 and not a copy. Got x[1] `" +
              fullPrec(ins1) + "`");
  }

  // ── (vi) `xpose, clear varname` transposes the whole frame. ───────────────
  {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp).
    auto r = exec("clear\nset obs 3\ngen double a = _n\ngen double b = _n*10\n"
                  "xpose, clear varname\n"
                  "display \"RS_XP11=\" %22.17f v1[1]\n"
                  "display \"RS_XP32=\" %22.17f v3[2]\n",
                  s);
    check(firstError(r.blocks).is_null(),
          "AER-RESHAPE (vi): `xpose, clear varname` over 3 obs x 2 vars runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(nRows(s) == 2.0,
          "AER-RESHAPE (vi): ...and the 3x2 frame became a 2-ROW frame -- rows and columns really "
          "swapped. Got " + fullPrec(nRows(s)));
    const std::vector<std::string> names = varNames(s);
    check(hasName(names, "v1") && hasName(names, "v2") && hasName(names, "v3"),
          "AER-RESHAPE (vi): ...with three columns named v1 v2 v3, one per original OBSERVATION");
    check(hasName(names, "_varname"),
          "AER-RESHAPE (vi): ...and `varname` added the `_varname` column. Without it the "
          "transpose is irreversible: nothing in the result says which row was `a`");
    check(countIf(s, "_varname == \"a\"") == 1.0 && countIf(s, "_varname == \"b\"") == 1.0,
          "AER-RESHAPE (vi): ..._varname carries the ORIGINAL NAMES, one row each -- not v1/v2 or "
          "an index. Got a=" + fullPrec(countIf(s, "_varname == \"a\"")) + " b=" +
              fullPrec(countIf(s, "_varname == \"b\"")));
    const double xp11 = displayValue(r.blocks, "RS_XP11=");
    const double xp32 = displayValue(r.blocks, "RS_XP32=");
    check(xp11 == 1.0,
          "AER-RESHAPE (vi): ...and the CELLS moved with the shape: v1[1] is a[1] == 1. The shape "
          "checks above are all satisfied by a transpose that filled the result with zeros. Got " +
              fullPrec(xp11));
    check(xp32 == 30.0,
          "AER-RESHAPE (vi): ...and v3[2] is b[3] == 30, which is the OPPOSITE corner -- a "
          "transpose that transposed the index but not the data, or transposed it twice, gets "
          "this one wrong while getting v1[1] right. Got " + fullPrec(xp32));
  }

  // ── (vii) `stack a b, into(v) clear` piles k columns into one. ────────────
  {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp).
    auto r = exec("clear\nset obs 4\ngen double a = _n\ngen double b = _n + 100\n"
                  "stack a b, into(sv) clear\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-RESHAPE (vii): `stack a b, into(sv) clear` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(nRows(s) == 8.0,
          "AER-RESHAPE (vii): ...and 4 rows x 2 variables became 8 rows. Got " +
              fullPrec(nRows(s)));
    // Read through `summarize _stack` rather than two `count if`s: _stack is
    // numeric, and N/min/max/sum together pin the 4/4 split exactly (8 values
    // in {1,2} summing to 12 is 4 of each) without putting an underscore-led
    // name through the expression parser.
    (void)piet::dispatch("summarize _stack", s);
    const double stN = rOf(s, "N"), stMin = rOf(s, "min"), stMax = rOf(s, "max"),
                 stSum = rOf(s, "sum");
    check(stN == 8.0 && stMin == 1.0 && stMax == 2.0 && stSum == 12.0,
          "AER-RESHAPE (vii): ..._stack splits them 4/4 (8 values, min 1, max 2, sum 12), so the "
          "index says WHICH variable each row came from. A stack that appended the frame to "
          "itself gives 8 rows with _stack all 1 -- max 1, sum 8 -- and every shape check but "
          "this one passes. Got N " + fullPrec(stN) + " min " + fullPrec(stMin) + " max " +
              fullPrec(stMax) + " sum " + fullPrec(stSum));
    const double svSum = runR(s, "summarize sv", "sum");
    check(svSum == 420.0,
          "AER-RESHAPE (vii): ...and `sv` carries BOTH columns' values: (1+2+3+4) + "
          "(101+102+103+104) == 420. 20 means b was dropped and a was duplicated; 820 means the "
          "opposite. Got " + fullPrec(svSum));
  }

  // ── (viii) `sample` is SEEDED, exact, and not a head/tail shortcut. ───────
  // Four assertions, each aimed at a different wrong implementation:
  //   exact count  -> a sample that rounded the other way, or returned a range
  //   same-seed    -> a global/time-seeded RNG (the failure that makes a
  //                   replication package irreproducible and reports nothing)
  //   not-head/tail-> a "sample" that just kept the first or last k rows
  //   seed matters -> a handler that ACCEPTS `set seed` and ignores it
  // The fingerprint is sum(id^3), not sum(id): distinct 50-subsets of 1..100
  // collide on their plain sum often enough to make the last check flaky, and
  // essentially never on the cube.
  {
    const std::string kMk100 =
        "clear\nset obs 100\ngen double id = _n\ngen double w = id*id*id\n";
    auto sampleSum = [&](int seed, double& n) {
      piet::SessionState s(workdir);
      s.eval.cwd = workdir;
      s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp).
      auto r = exec(kMk100 + "set seed " + std::to_string(seed) + "\nsample 50\n", s);
      if (!firstError(r.blocks).is_null()) {
        n = std::nan("");
        return std::nan("");
      }
      n = nRows(s);
      return runR(s, "summarize w", "sum");
    };
    double nA = 0.0, nB = 0.0, nC = 0.0;
    const double sumA = sampleSum(7, nA);
    const double sumB = sampleSum(7, nB);
    const double sumC = sampleSum(8, nC);
    check(nA == 50.0,
          "AER-RESHAPE (viii): `sample 50` over 100 rows keeps EXACTLY 50 -- an exact count, not a "
          "range: `sample #` is a percentage and round-half-up of 100*50/100 is 50 on the nose. Got "
          + fullPrec(nA));
    check(nB == 50.0 && sumB == sumA && !std::isnan(sumA),
          "AER-RESHAPE (viii): ...and the SAME SEED twice gives the SAME ROWS, bit-exact. This is "
          "the property the Philox-keyed design exists for: a global or time-seeded generator "
          "passes every other check in this leg and makes every replication package that calls "
          "`sample` irreproducible, silently. Got " + fullPrec(sumA) + " vs " + fullPrec(sumB));
    check(sumA != 1625625.0,
          "AER-RESHAPE (viii): ...and it is not the FIRST 50 rows (whose w sums to 1625625). A "
          "`sample` that returned the head is deterministic, has the right count, and is not a "
          "sample. Got " + fullPrec(sumA));
    check(sumA != 23876875.0,
          "AER-RESHAPE (viii): ...nor the LAST 50 (23876875). Got " + fullPrec(sumA));
    check(!std::isnan(sumC) && sumC != sumA,
          "AER-RESHAPE (viii): ...and a DIFFERENT seed gives a different draw, so `set seed` is "
          "actually read rather than accepted and dropped -- which is how a handler passes the "
          "determinism check above while being deterministic for the wrong reason. Got seed7 " +
              fullPrec(sumA) + " vs seed8 " + fullPrec(sumC));
  }

  // ── (ix) `bsample, cluster(g)` resamples CLUSTERS, whole. ────────────────
  // The clusters are deliberately RAGGED (sizes 1,2,3,4). The load-bearing
  // assertion is the modulus: whatever survives of cluster k must be a whole
  // multiple of k, because clusters are drawn intact. A `bsample` that
  // resampled ROWS and merely carried g along -- the single most likely wrong
  // implementation, and one that gets the row count, the value domain and the
  // cluster count all right -- produces counts that are not multiples, and
  // this is the only check here that sees it.
  {
    const std::string kMkG =
        "clear\nset obs 10\ngen double g = 1\nreplace g = 2 in 2/3\n"
        "replace g = 3 in 4/6\nreplace g = 4 in 7/10\n";
    auto draw = [&](int seed, std::vector<double>& per, double& ncMin, double& ncMax) {
      piet::SessionState s(workdir);
      s.eval.cwd = workdir;
      s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp).
      auto r = exec(kMkG + "set seed " + std::to_string(seed) +
                        "\nbsample, cluster(g) idcluster(nc)\n",
                    s);
      per.clear();
      ncMin = std::nan("");
      ncMax = std::nan("");
      if (!firstError(r.blocks).is_null()) return std::nan("");
      for (int k = 1; k <= 4; ++k) per.push_back(countIf(s, "g == " + std::to_string(k)));
      const double outOfDomain = countIf(s, "g < 1 | g > 4");
      (void)piet::dispatch("summarize nc", s);
      ncMin = rOf(s, "min");
      ncMax = rOf(s, "max");
      if (outOfDomain != 0.0) return -1.0;
      return nRows(s);
    };
    std::vector<double> perA, perB;
    double ncMinA = 0.0, ncMaxA = 0.0, ncMinB = 0.0, ncMaxB = 0.0;
    const double nA = draw(11, perA, ncMinA, ncMaxA);
    const double nB = draw(11, perB, ncMinB, ncMaxB);
    check(nA >= 4.0 && nA <= 16.0,
          "AER-RESHAPE (ix): `bsample, cluster(g)` over four clusters of sizes 1,2,3,4 draws FOUR "
          "clusters with replacement, so the result holds between 4 and 16 rows. Got " +
              fullPrec(nA));
    check(nA != -1.0,
          "AER-RESHAPE (ix): ...and every surviving g value is one of the original four -- a "
          "resample that renumbered the cluster variable instead of copying it destroys the join "
          "key every clustered SE downstream needs");
    bool multiples = perA.size() == 4;
    std::string perTxt;
    for (size_t k = 0; k < perA.size(); ++k) {
      const double sz = static_cast<double>(k + 1);
      perTxt += (k ? ", " : "") + std::to_string(k + 1) + ":" + fullPrec(perA[k]);
      if (std::isnan(perA[k]) || std::fmod(perA[k], sz) != 0.0) multiples = false;
    }
    check(multiples,
          "AER-RESHAPE (ix): ...and each cluster survives in WHOLE COPIES: the rows left carrying "
          "g==k number 0, k, 2k, ... because a cluster bootstrap draws the cluster, not its rows. "
          "This is the check that separates a real cluster bootstrap from one that resampled rows "
          "and kept g as a passenger -- that version gets the row count, the value domain and the "
          "cluster count right and is wrong in the only way that matters. Got counts {" + perTxt +
              "}");
    double perSum = 0.0;
    for (double v : perA) perSum += v;
    check(perSum == nA,
          "AER-RESHAPE (ix): ...and those four counts account for every row in the result, so no "
          "row arrived with a missing or invented cluster. Got " + fullPrec(perSum) + " vs _N " +
              fullPrec(nA));
    check(ncMinA == 1.0 && ncMaxA == 4.0,
          "AER-RESHAPE (ix): ...and `idcluster(nc)` numbers the DRAWS 1..4, not the original "
          "cluster ids -- that renumbering is the whole point of the option, because the same "
          "cluster drawn twice must be two different units to the estimator downstream. Got min " +
              fullPrec(ncMinA) + " max " + fullPrec(ncMaxA));
    check(nB == nA && perB == perA,
          "AER-RESHAPE (ix): ...and the same seed draws the same clusters twice. Got _N " +
              fullPrec(nA) + " vs " + fullPrec(nB));
  }

  // ── (x) `drawnorm` reproduces the requested correlation. ─────────────────
  {
    const std::string kDraw =
        "clear\nmatrix C = (1, 0.6 \\ 0.6, 1)\ndrawnorm u v, n(2000) corr(C) seed(3)\n";
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp).
    auto r = exec(kDraw, s);
    check(firstError(r.blocks).is_null(),
          "AER-RESHAPE (x): `drawnorm u v, n(2000) corr(C) seed(3)` runs clean with no data in "
          "memory. Got `" + errText(firstError(r.blocks)) + "`");
    check(nRows(s) == 2000.0,
          "AER-RESHAPE (x): ...and n() is the row count, exactly. Got " + fullPrec(nRows(s)));
    (void)piet::dispatch("summarize u", s);
    const double mu = rOf(s, "mean");
    const double sdu = rOf(s, "sd");
    check(std::fabs(mu) < 0.1,
          "AER-RESHAPE (x): ...u has mean ~0 (default means()). At n=2000 the standard error of "
          "the mean is ~0.022, so 0.1 is a four-sigma band and NOT a cushion hiding a shifted "
          "distribution. Got " + fullPrec(mu));
    check(std::fabs(sdu - 1.0) < 0.1,
          "AER-RESHAPE (x): ...and sd ~1 (default sds()). A Cholesky applied to the wrong side, or "
          "to the covariance when a correlation was given, scales this away from 1. Got " +
              fullPrec(sdu));
    const double rho = runR(s, "correlate u v", "rho");
    check(!std::isnan(rho) && std::fabs(rho - 0.6) < 0.1,
          "AER-RESHAPE (x): ...and the SAMPLE correlation is the requested 0.6. This is the only "
          "check that reaches the Cholesky itself: an identity L (or a transposed one) leaves two "
          "perfectly good standard normals with rho ~ 0, and every other assertion in this leg "
          "passes. Got " + fullPrec(rho));
    piet::SessionState s2(workdir);
    s2.eval.cwd = workdir;
    s2.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp).
    (void)exec(kDraw, s2);
    const double rho2 = runR(s2, "correlate u v", "rho");
    check(!std::isnan(rho2) && rho2 == rho,
          "AER-RESHAPE (x): ...and `seed(3)` twice draws the IDENTICAL sample -- compared "
          "bit-exact, not within a band, because the same seed must give the same numbers and not "
          "merely the same distribution. Got " + fullPrec(rho) + " vs " + fullPrec(rho2));
  }

  // ── (xi) `notes` writes characteristics, and .dta round-trips them free. ──
  {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp);
    // eval.cwd alone puts rs_notes.dta in the PROCESS cwd -- the repo root --
    // and the round trip below then reads a file this leg never wrote.
    auto hasChar = [](piet::SessionState& ss, const std::string& vn, const std::string& cn,
                      const std::string& val) {
      for (const auto& c : ss.characteristics)
        if (c.varname == vn && c.charname == cn && c.value == val) return true;
      return false;
    };
    auto nNotes = [](piet::SessionState& ss) {
      int n = 0;
      for (const auto& c : ss.characteristics)
        if (c.charname.rfind("note", 0) == 0) ++n;
      return n;
    };
    auto r = exec("clear\nset obs 4\ngen double x = _n\n"
                  "notes: hi\nnotes x: there\n",
                  s);
    check(firstError(r.blocks).is_null(),
          "AER-RESHAPE (xi): `notes: hi` and `notes x: there` run clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(hasChar(s, "_dta", "note1", "hi"),
          "AER-RESHAPE (xi): the dataset note lands as _dta[note1] == \"hi\" -- Stata's own "
          "characteristic spelling, which is what makes it survive a .dta write with no new code");
    check(hasChar(s, "x", "note1", "there"),
          "AER-RESHAPE (xi): ...and the VARIABLE note lands as x[note1], in the variable's own "
          "namespace. A handler with only the dataset store puts both under _dta and `notes list` "
          "still renders something plausible");
    check(hasChar(s, "_dta", "note0", "1") && hasChar(s, "x", "note0", "1"),
          "AER-RESHAPE (xi): ...each with its note0 COUNTER set to 1. note0 is what the next "
          "`notes:` reads to pick its number; a missing counter makes note 2 overwrite note 1, "
          "which loses data and reports nothing");
    check(nNotes(s) == 4,
          "AER-RESHAPE (xi): ...and exactly four note characteristics exist, so nothing was "
          "written twice. Got " + std::to_string(nNotes(s)));
    auto r2 = exec("save \"rs_notes.dta\", replace\nuse \"rs_notes.dta\", clear\n", s);
    check(firstError(r2.blocks).is_null(),
          "AER-RESHAPE (xi/roundtrip): save -> use runs clean. Got `" +
              errText(firstError(r2.blocks)) + "`");
    check(hasChar(s, "_dta", "note1", "hi") && hasChar(s, "x", "note1", "there") &&
              nNotes(s) == 4,
          "AER-RESHAPE (xi/roundtrip): ...and BOTH notes came back off disk. This is the claim "
          "the design rests on -- characteristics are written verbatim by the .dta writer and "
          "restored by `use`, so notes persist for free. If they do not, `notes` is a "
          "session-only feature wearing a persistent feature's name, and no in-session check sees "
          "it. Got " + std::to_string(nNotes(s)) + " note characteristics after the round trip");
  }

  // ── (xii) `recast` is METADATA. The frame must not move. ─────────────────
  {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp).
    auto r = exec("clear\nset obs 6\ngen double x = _n / 3\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-RESHAPE (xii): setup runs clean. Got `" + errText(firstError(r.blocks)) + "`");
    (void)piet::dispatch("summarize x", s);
    const double meanBefore = rOf(s, "mean");
    const std::string hbinBefore =
        s.currentHbin().has_value() ? *s.currentHbin() : std::string("<none>");
    auto r2 = exec("recast double x\n", s);
    check(firstError(r2.blocks).is_null(),
          "AER-RESHAPE (xii): `recast double x` runs clean. Got `" +
              errText(firstError(r2.blocks)) + "`");
    check(dtypeOf(s, "x") == "double",
          "AER-RESHAPE (xii): ...and the declared storage type IS `double` afterwards -- the only "
          "observable effect the command has, and the one a .dta write carries out to Stata. Got "
          "dtype `" + dtypeOf(s, "x") + "`");
    const std::string hbinAfter =
        s.currentHbin().has_value() ? *s.currentHbin() : std::string("<none>");
    check(hbinAfter == hbinBefore,
          "AER-RESHAPE (xii): ...and the FRAME DID NOT MOVE: currentHbin is the same file it was. "
          "recast is metadata-only by contract, so a handler that round-tripped the column through "
          "a CSV to `change the type` would land here with a new hbin -- and, if that CSV were "
          "written at anything under 17 digits, with 1/3 re-encoded on the way, which the mean "
          "check below then also catches. (This used to cite BUG-406 as a 9 s.f. loss inside "
          "`cobra convert`; REDIAGNOSED 2026-09-06 -- the storage is exact, the nine digits were "
          "`display %fmt <varref>`, and this leg reads r(mean), not a display.) "
          "Got `" + hbinAfter + "` vs `" + hbinBefore + "`");
    (void)piet::dispatch("summarize x", s);
    const double meanAfter = rOf(s, "mean");
    check(!std::isnan(meanBefore) && meanAfter == meanBefore,
          "AER-RESHAPE (xii): ...and the VALUES are bit-identical across the recast. x = _n/3 is "
          "chosen for that: it is not representable in binary, so any re-encoding shows up here. "
          "Got " + fullPrec(meanAfter) + " vs " + fullPrec(meanBefore));
  }

  // ── (xiii) three refusals -- by name, and having changed nothing. ────────
  {
    // (a) `split` on a NUMERIC column. Stata's split is a string operation;
    // silently stringifying the number would produce a column that looks right
    // and compares wrong.
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp).
    (void)exec("clear\nset obs 4\ngen double n = _n\n", s);
    const size_t varsBefore = varNames(s).size();
    auto r = exec("split n, parse(\"-\") gen(zz)\n", s);
    auto err = firstError(r.blocks);
    check(!err.is_null(),
          "AER-RESHAPE (xiii/a): `split` on a NUMERIC variable is an ERROR. Coercing it to text "
          "and splitting that is the silently-wrong path: the parts exist, they are strings, and "
          "every comparison against them fails for a reason nothing reports");
    check(has(errText(err), "split"),
          "AER-RESHAPE (xiii/a): ...and the message names the command. Got `" + errText(err) + "`");
    check(varNames(s).size() == varsBefore,
          "AER-RESHAPE (xiii/a): ...having created NO columns. A refusal reached after the first "
          "`setcol --new` leaves a half-built stub set behind, and the next run of the same "
          "do-file then fails on a name collision instead of on the real cause. Got " +
              std::to_string(varNames(s).size()) + " variables vs " + std::to_string(varsBefore));

    // (b) `xpose` without `clear`. The frame is destroyed by a transpose, so
    // Stata makes the caller say so.
    piet::SessionState s2(workdir);
    s2.eval.cwd = workdir;
    s2.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp).
    (void)exec("clear\nset obs 3\ngen double a = _n\ngen double b = _n*10\n", s2);
    auto r2 = exec("xpose\n", s2);
    auto err2 = firstError(r2.blocks);
    check(!err2.is_null(),
          "AER-RESHAPE (xiii/b): `xpose` without `clear` is an ERROR -- the option is Stata's "
          "confirmation that the caller knows the dataset is about to be replaced");
    check(has(errText(err2), "clear"),
          "AER-RESHAPE (xiii/b): ...and the message names `clear`, so the fix is in the message "
          "rather than in the manual. Got `" + errText(err2) + "`");
    check(nRows(s2) == 3.0 && varNames(s2).size() == 2,
          "AER-RESHAPE (xiii/b): ...and the frame is UNTOUCHED: still 3 rows and 2 variables. A "
          "refusal raised after the CSV round trip has already replaced the data, which is the "
          "worst version of this defect -- the command reports failure and did the thing. Got " +
              fullPrec(nRows(s2)) + " rows, " + std::to_string(varNames(s2).size()) + " variables");

    // (c) `sample ..., by()` -- an option this build does not implement. It
    // must be refused as an OPTION, not accepted and dropped.
    piet::SessionState s3(workdir);
    s3.eval.cwd = workdir;
    s3.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp).
    (void)exec("clear\nset obs 20\ngen double g = 1\nreplace g = 2 in 11/20\n", s3);
    auto r3 = exec("sample 10, by(g)\n", s3);
    auto err3 = firstError(r3.blocks);
    check(!err3.is_null(),
          "AER-RESHAPE (xiii/c): `sample 10, by(g)` is REFUSED. by() changes the ANSWER -- it "
          "makes the draw stratified -- so accepting and ignoring it returns a plain sample under "
          "a stratified sample's name, with rc 0 and no way to tell");
    check(errField(err3, "code") == "unsupported_option",
          "AER-RESHAPE (xiii/c): ...under `unsupported_option`, not `unsupported_command`: the "
          "command exists and one option does not, and a caller branching on the code is entitled "
          "to that difference. Got `" + errField(err3, "code") + "`");
    check(rcOf(err3) == "198",
          "AER-RESHAPE (xiii/c): ...at r(198), Stata's option error. Asserted separately from the "
          "code because a refusal built with the default engine_error code arrives as rc 1 and "
          "every do-file branching on 198 takes the wrong branch. Got r(" + rcOf(err3) + ")");
    check(nRows(s3) == 20.0,
          "AER-RESHAPE (xiii/c): ...and it sampled NOTHING. The reject net has to fire before the "
          "handler, or the user gets a refusal message and a frame that has already lost half its "
          "rows. Got " + fullPrec(nRows(s3)));
  }

  fs::remove_all(workdir, ec);
}


void testBS004JoinbyInnerKeyJoin() {
  std::printf("[selftest] --- BS-004 `joinby` inner key-join (PIET-native, D5) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs004").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 4", session);
  (void)piet::dispatch("gen double gid = cond(_n<=2, 1, 2)", session);
  // NB: mod-free by historical accident: written when cobra-eval lacked mod()
  // (BS-094 / BUG-038b — since LANDED engine-side; `gen z=mod(...)` works now).
  // The fixture keeps its original shape for freeze stability, not because this
  // leg's SETUP and mask the joinby behaviour under test. _n==1|_n==3 gives 1,2,1,2.
  (void)piet::dispatch("gen double side = cond(_n==1 | _n==3, 1, 2)", session);

  (void)piet::dispatch("tempfile bs004using", session);
  (void)piet::dispatch("preserve", session);
  (void)piet::dispatch("rename side other_side", session);
  (void)piet::dispatch("label variable other_side \"Using side label\"", session);
  (void)piet::dispatch("save `bs004using', replace", session);
  (void)piet::dispatch("restore", session);

  // --- the core case: BOTH sides have 2 rows per key -> full 2x2 cross ---
  auto blocks = piet::dispatch("joinby gid using `bs004using'", session);
  bool sawError = false;
  for (const auto& b : blocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") sawError = true;
  }
  check(!sawError, "BS-004: bare `joinby gid using FILE` does not error");

  piet::Datastore* ds = session.datastore();
  bool structureOk = ds != nullptr && ds->has("gid") && ds->has("side") && ds->has("other_side");
  check(structureOk, "BS-004: joined dataset carries master's `side` + using's `other_side`");
  (void)piet::dispatch("local bs004label : variable label other_side", session);
  check(session.eval.locals.get("bs004label") == "Using side label",
        "BS-004: genuine DTA tempfile keeps the using variable label");
  if (structureOk) {
    check(ds->nRows() == 8,
          "BS-004: 2 master rows x 2 using rows per gid-group, 2 groups -> 8 rows (the full "
          "within-key Cartesian cross, not a 1:1 pairing)");
    std::set<std::pair<int, int>> g1Pairs, g2Pairs;
    const double* gidv = ds->col("gid").f64();
    const double* sidev = ds->col("side").f64();
    const double* otherv = ds->col("other_side").f64();
    for (int64_t i = 0; i < ds->nRows(); ++i) {
      auto p = std::make_pair(static_cast<int>(sidev[i]), static_cast<int>(otherv[i]));
      if (gidv[i] == 1.0) g1Pairs.insert(p);
      else if (gidv[i] == 2.0) g2Pairs.insert(p);
    }
    std::set<std::pair<int, int>> expected = {{1, 1}, {1, 2}, {2, 1}, {2, 2}};
    check(g1Pairs == expected, "BS-004: gid=1 group produces all 4 (side,other_side) combinations");
    check(g2Pairs == expected, "BS-004: gid=2 group produces all 4 (side,other_side) combinations");
  }

  // --- unmatched(none) default: a master row with no using match is DROPPED ---
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 1", session);
  (void)piet::dispatch("gen double gid = 999", session);
  auto noMatchBlocks = piet::dispatch("joinby gid using `bs004using'", session);
  bool noMatchErr = false;
  for (const auto& b : noMatchBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") noMatchErr = true;
  }
  check(!noMatchErr, "BS-004: a master row with no using match does not error");
  piet::Datastore* dsEmpty = session.datastore();
  check(dsEmpty != nullptr && dsEmpty->nRows() == 0,
        "BS-004: unmatched(none) (the only mode this build implements) drops the non-matching "
        "master row (0 rows out)");

  // --- D5: ANY joinby option is an honest reject (bare form only) ---
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 4", session);
  (void)piet::dispatch("gen double gid = cond(_n<=2, 1, 2)", session);
  // NB: mod-free by historical accident: written when cobra-eval lacked mod()
  // (BS-094 / BUG-038b — since LANDED engine-side; `gen z=mod(...)` works now).
  // The fixture keeps its original shape for freeze stability, not because this
  // leg's SETUP and mask the joinby behaviour under test. _n==1|_n==3 gives 1,2,1,2.
  (void)piet::dispatch("gen double side = cond(_n==1 | _n==3, 1, 2)", session);
  auto optBlocks = piet::dispatch("joinby gid using `bs004using', update", session);
  bool optErr = false;
  for (const auto& b : optBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") optErr = true;
  }
  check(optErr,
        "BS-004: `joinby ..., update` (an unimplemented option) is an honest reject, not a "
        "silently-ignored option (D5 — unmatched()/update/_merge()/... -> post-beta WP-18)");

  fs::remove_all(workdir, ec);
}
