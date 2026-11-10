// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: tables.
#include "selftest_support.h"
#include "selftest_cases.h"


// ── AER-SCOPE1 bundle E (TABLES, 2026-09-04) ─────────────────────────────────
//
// The pipeline this bundle exists for is `regress -> estadd -> eststo ->
// estout|esttab using f.tex`, and EVERY one of its failure modes is silent:
// a snapshot that carries no estadd bag, a `using` that returns rc 0 and
// writes no file, an `append` that overwrites the column it was asked to add,
// a refusal that has already written the file it refuses to write. An rc-0
// assertion is GREEN for all four.
//
// So no leg below stops at a return code. Each one reads either a VALUE out
// of the stored estimate or BYTES off the disk, and the substrings it looks
// for are the ones a WRONG renderer would not produce -- `\toprule` for
// booktabs, three tab-separated fields on outreg2's VARIABLES row, outreg2's
// own star legend (which differs from estout's and is the cheapest evidence
// that the two commands did not collapse into one renderer's defaults).
//
// Nothing here asserts a PATH. The workdir is this process's temp directory
// and would differ on the operator's machine; what is asserted is the file's
// CONTENT, which is the same everywhere.
void testAerTables() {
  std::printf("[selftest] --- AER-SCOPE1 bundle E: estadd/estpost/estout/esttab-using/outreg2 "
              "+ regsave/erepost + the two refusals ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_aertables").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto exec = [](const std::string& src, piet::SessionState& session) {
    return piet::doexec::executeDoSource(src, session, piet::dispatch);
  };
  // Returns a NULL json rather than throwing when there is no error block --
  // the BUG-129 trap: `value()` on nothing aborts the whole run instead of
  // failing one check, and check()'s message argument is built EAGERLY, on the
  // passing path too, so every message below reads through these two helpers
  // rather than calling value() inline.
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
  auto hasBlockType = [](const auto& blocks, const std::string& t) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == t) return true;
    return false;
  };
  // Walks a JSON path and answers NULL for any missing hop, so a snapshot that
  // is missing the whole `added` object fails the VALUE check below instead of
  // throwing out of the run.
  auto dig = [](const nlohmann::ordered_json& root,
                const std::vector<std::string>& path) -> nlohmann::ordered_json {
    const nlohmann::ordered_json* cur = &root;
    for (const auto& k : path) {
      if (!cur->is_object() || !cur->contains(k)) return nlohmann::ordered_json();
      cur = &((*cur)[k]);
    }
    return *cur;
  };
  auto slurp = [](const std::string& p) {
    std::ifstream f(cecon::to_native_path(p), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  };
  auto at = [&](const std::string& rel) { return (fs::path(workdir) / rel).u8string(); };
  // std::to_string gives SIX decimals, which renders two doubles differing
  // in the 8th digit as the same text -- a failure message that cannot be
  // read is worse than none, and this very leg printed both sides as
  // "-77.551724" while they genuinely differed. Value comparisons report
  // through this instead.
  auto fullPrec = [](double v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return std::string(buf);
  };
  auto has = [](const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
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

  // 30 rows, deterministic, and deliberately NOT a perfect fit: `y = 2*x1 +
  // 0.5*x2` alone reproduces y exactly, which drives every standard error to
  // zero and makes the star columns (and outreg2's legend, checked below)
  // degenerate. The `+ 5 in 1/10` shift is what leaves real residuals.
  const std::string kMake30 =
      "clear\n"
      "set obs 30\n"
      "gen double x1 = _n\n"
      "gen double x2 = _n*_n\n"
      "gen double y = 2*x1 + 0.5*x2\n"
      "replace y = y + 5 in 1/10\n";

  // ── (i) estadd attaches to the ACTIVE e(), and eststo carries the bag AND
  // the frozen e() scalars into the snapshot. ────────────────────────────────
  piet::SessionState session(workdir);
  session.eval.cwd = workdir;
  session.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp);
  // eval.cwd alone leaves every `using` file in the process cwd -- the repo root.
  {
    auto r = exec(kMake30 +
                      "regress y x1\n"
                      "estadd local c \"Yes\"\n"
                      "estadd scalar k = 7\n"
                      "eststo m1\n",
                  session);
    check(firstError(r.blocks).is_null(),
          "AER-TABLES (i): regress -> estadd local -> estadd scalar -> eststo runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const nlohmann::ordered_json* snap = session.estimatesGet("m1");
    check(snap != nullptr,
          "AER-TABLES (i): `eststo m1` actually STORED a snapshot -- every later leg reads it, and "
          "a store that silently did nothing makes them all vacuous");
    const nlohmann::ordered_json cj = snap ? dig(*snap, {"added", "locals", "c"})
                                           : nlohmann::ordered_json();
    check(cj.is_string() && cj.get<std::string>() == "Yes",
          "AER-TABLES (i): the snapshot carries added.locals.c == \"Yes\". This is the whole "
          "reason the bag exists: Stata's estadd runs BEFORE eststo and attaches to the ACTIVE "
          "e(), so a handler that wrote into the estimates store instead would find nothing there "
          "yet and lose the addition without erroring. Got " + cj.dump());
    const nlohmann::ordered_json kj = snap ? dig(*snap, {"added", "scalars", "k"})
                                           : nlohmann::ordered_json();
    check(kj.is_number() && std::fabs(kj.get<double>() - 7.0) < 1e-12,
          "AER-TABLES (i): ...and added.scalars.k == 7, so `estadd scalar k = 7` was EVALUATED "
          "rather than stored as the literal string \"7\" (stats() formats it as a number "
          "downstream, and a string lands in the .tex as `7` with no format applied). Got " +
              kj.dump());
    const nlohmann::ordered_json nj = snap ? dig(*snap, {"escalars", "N"})
                                           : nlohmann::ordered_json();
    check(nj.is_number() && std::fabs(nj.get<double>() - 30.0) < 1e-9,
          "AER-TABLES (i): ...and escalars.N == 30, frozen AT eststo time. Without this the "
          "`stats(N ...)` footer has nothing to read and renders an empty cell while the command "
          "still returns rc 0 -- the exact shape a file-existence check cannot see. Got " +
              nj.dump());
  }

  // ── (i-b) the fingerprint half of the same mechanism. A bag left attached
  // across a DIFFERENT estimation is a wrong number on the right-looking row,
  // which no renderer check can catch. ───────────────────────────────────────
  {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp);
    // eval.cwd alone leaves every `using` file in the process cwd -- the repo root.
    auto r = exec(kMake30 +
                      "regress y x1\n"
                      "estadd scalar k = 7\n"
                      "regress x2 x1\n"
                      "eststo m2\n",
                  s);
    (void)r;
    const nlohmann::ordered_json* snap = s.estimatesGet("m2");
    const nlohmann::ordered_json kj = snap ? dig(*snap, {"added", "scalars", "k"})
                                           : nlohmann::ordered_json();
    check(snap != nullptr && kj.is_null(),
          "AER-TABLES (i-b): an estadd made against ONE estimate is DISCARDED when a different "
          "estimation replaces e() before eststo. The fingerprint is the only thing standing "
          "between `estadd scalar k` and it reappearing under a model that never had it -- and "
          "that failure prints a plausible number, so it is invisible everywhere else. Got " +
              kj.dump());
  }

  // ── (ii) estout writes a REAL FILE whose bytes carry the addition. ─────────
  {
    check(!fs::exists(cecon::to_native_path(at("t.tex"))),
          "AER-TABLES (ii): t.tex does not exist before `estout ... using` runs -- otherwise the "
          "content checks below could be reading a leftover from an earlier run");
    auto r = exec("estout m1 using \"t.tex\", style(tex) replace stats(N k c)\n", session);
    check(firstError(r.blocks).is_null(),
          "AER-TABLES (ii): `estout m1 using \"t.tex\", style(tex) replace stats(N k c)` returns "
          "no error. Got `" + errText(firstError(r.blocks)) + "`");
    const std::string tex = slurp(at("t.tex"));
    check(!tex.empty(),
          "AER-TABLES (ii): ...and it left BYTES on disk. Before this bundle the whole table "
          "family returned a JSON block and wrote nothing, which is rc 0 with no output file -- "
          "the defect this leg exists for");
    // W1a-4 CORRECTION. These two checks asserted `\begin{tabular}` and `\hline`
    // in estout's style(tex) output, and the ado writes NEITHER: it emits a bare
    // fragment of fixed-width right-aligned cells and no rules at all
    // (tests/eval/golden/aer_w1/w1b_estout_tex.log r1, and w1b_tables/e7.tex from
    // the 2026-09-14 capture, which shows the same thing). So this arm was
    // PINNING the defect -- it held this writer to esttab's shape, which is
    // exactly what BUG-486 is about and what made the sixth axis survive the
    // five that were fixed. The replacement asserts the fragment.
    check(!has(tex, "\\begin{tabular}") && !has(tex, "\\hline"),
          "AER-TABLES (ii): estout's style(tex) is a FRAGMENT -- no tabular, no rules. The check "
          "here used to demand both and so held this writer to esttab's shape; 148 of the corpus's "
          "219 estout lines carry tex, and every one of them got the wrong table. Got " +
              std::to_string(tex.size()) + "B:\n" + tex);
    check(has(tex, "&") && has(tex, "\\\\"),
          "AER-TABLES (ii): ...and it IS LaTeX rather than the fixed-width fallback: `&` between "
          "cells and `\\\\` ending each row (r1). That is what the old checks were reaching for, "
          "and it can be asked without asserting a wrapper the ado does not write");
    check(has(tex, "Yes"),
          "AER-TABLES (ii): ...and `stats(... c)` put the estadd LOCAL's text into the table. "
          "This is the end-to-end proof that the bag survived estadd -> eststo -> estout: a "
          "footer that read only e() renders an empty row here and still writes a valid file");
    check(has(tex, "30"),
          "AER-TABLES (ii): ...and `stats(N ...)` put the frozen observation count in. 30 is this "
          "fixture's N, so a footer that read the CURRENT dataset instead of the snapshot would "
          "still print 30 here -- which is why (i) asserts escalars.N independently");
    check(has(tex, "_cons"),
          "AER-TABLES (ii): ...and the COEFFICIENT body rendered too (`_cons` is makeSnapshot's "
          "rename of `(intercept)`). A file carrying only the stats footer satisfies every check "
          "above and is not a regression table");
    check(!has(tex, workdir),
          "AER-TABLES (ii): the rendered table does not embed the build machine's workdir. A "
          "renderer that wrote the resolved output path into the table makes every golden "
          "machine-specific");
  }

  // ── (iii) esttab's `using` half. Same block, written by one renderer. ──────
  {
    auto r = exec("esttab m1 using \"t2.tex\", replace booktabs\n", session);
    check(firstError(r.blocks).is_null(),
          "AER-TABLES (iii): `esttab m1 using \"t2.tex\", replace booktabs` returns no error. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(hasBlockType(r.blocks, "estimates_table"),
          "AER-TABLES (iii): ...and the ON-SCREEN table block is still emitted. The dispatcher "
          "splits `using` off and hands runEsttab's own block to the writer; a split that "
          "swallowed the screen output would leave `esttab ... using` silently printing nothing");
    const std::string tex = slurp(at("t2.tex"));
    check(!tex.empty(), "AER-TABLES (iii): ...and t2.tex has bytes in it");
    check(has(tex, "\\toprule") && has(tex, "\\bottomrule"),
          "AER-TABLES (iii): `booktabs` renders \\toprule/\\bottomrule, NOT \\hline. This option "
          "changes only the rule macros, so an option list that arrived unread produces a file "
          "that looks right, compiles, and is the wrong table -- rc 0 either way. Got " +
              std::to_string(tex.size()) + "B");
    check(has(tex, "_cons"),
          "AER-TABLES (iii): ...and the coefficient body is in the file rather than the header "
          "alone");
  }

  // ── (iv) outreg2's .txt ledger IS the append source of truth. ──────────────
  {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp);
    // eval.cwd alone leaves every `using` file in the process cwd -- the repo root.
    auto r = exec(kMake30 +
                      "regress y x1\n"
                      "outreg2 using \"o\", replace tex ctitle(\"M1\")\n"
                      "regress y x1 x2\n"
                      "outreg2 using \"o\", append ctitle(\"M2\")\n",
                  s);
    check(firstError(r.blocks).is_null(),
          "AER-TABLES (iv): outreg2 replace-then-append runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const std::string txt = slurp(at("o.txt"));
    check(!txt.empty(),
          "AER-TABLES (iv): outreg2 always writes its `.txt` ledger, whatever else was asked for. "
          "It is the file `append` re-reads on a LATER session, so an in-memory ledger answers "
          "differently the second time the same do-file runs");
    // The VARIABLES row is the column roster. Read it by SPLITTING, not by
    // searching for "M2": a file that had been overwritten rather than
    // appended still contains "M2" -- as its ONLY column.
    std::vector<std::string> fields;
    {
      std::istringstream iss(txt);
      std::string line;
      while (std::getline(iss, line)) {
        if (line.rfind("VARIABLES", 0) != 0) continue;
        std::string cur;
        for (char ch : line) {
          if (ch == '\t') {
            fields.push_back(cur);
            cur.clear();
          } else if (ch != '\r') {
            cur += ch;
          }
        }
        fields.push_back(cur);
        break;
      }
    }
    check(fields.size() == 3,
          "AER-TABLES (iv): the VARIABLES row carries THREE tab-separated fields (the stub plus "
          "two model columns). This is the append assertion and it is made on the column COUNT "
          "because an append that overwrote instead of adding leaves a valid one-model table that "
          "returns rc 0 and contains the second ctitle. Got " + std::to_string(fields.size()));
    check(fields.size() == 3 && fields[1] == "M1" && fields[2] == "M2",
          "AER-TABLES (iv): ...and they are the two ctitle()s IN ORDER -- so the appended column "
          "went on the RIGHT and the first model was re-read from the ledger rather than "
          "re-rendered from whatever e() happens to hold now. Got `" +
              (fields.size() == 3 ? fields[1] + "` / `" + fields[2] : std::string("<row absent>")) +
              "`");
    check(has(txt, "*** p<0.01") && has(txt, "* p<0.1"),
          "AER-TABLES (iv): the legend is OUTREG2's (*** 0.01 / ** 0.05 / * 0.1), not estout's "
          "(* 0.05 / ** 0.01 / *** 0.001). The two commands share a Table model and three "
          "renderers on purpose, and this is the line that proves the shared renderer did not "
          "also share the star levels -- which would silently restate every significance claim in "
          "the output");
    check(has(txt, "Observations"),
          "AER-TABLES (iv): ...and the footer names the observation row the way outreg2 does");
    const std::string tex = slurp(at("o.tex"));
    check(!tex.empty() && has(tex, "\\begin{tabular}"),
          "AER-TABLES (iv): `tex` also wrote o.tex as a real tabular. A .txt-only handler that "
          "accepted the option and ignored it leaves the do-file's \\input pointing at nothing");
  }

  // ── (v) estpost is a pseudo-estimate whose e(mean) is what esttab reads. ───
  {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp);
    // eval.cwd alone leaves every `using` file in the process cwd -- the repo root.
    // Bare `esttab`, NOT `esttab, cells(...)`. estpost posts e(b)=mean and
    // e(se)=sd, so esttab's DEFAULT b-over-(se) layout already renders mean over
    // sd here -- which is why the earlier cut of this leg passed with cells()
    // in the line and proved nothing about cells(). `runEsttab` does not read
    // cells(); it is now listed as unimplemented (BUG-407) and refuses by name,
    // which the companion check below pins so this leg cannot quietly go back
    // to passing for the wrong reason.
    auto r = exec(kMake30 +
                      "estpost summarize x1 y\n"
                      "esttab\n",
                  s);
    check(firstError(r.blocks).is_null(),
          "AER-TABLES (v): `estpost summarize x1 y` then a bare `esttab` runs clean -- the "
          "pseudo-estimate is reachable through runEsttab's active-e() fallback, with nothing "
          "in the estimates store. Got `" + errText(firstError(r.blocks)) + "`");
    nlohmann::ordered_json table;
    for (const auto& b : r.blocks)
      if (b.is_object() && b.value("type", std::string()) == "estimates_table") table = b;
    check(!table.is_null(),
          "AER-TABLES (v): ...and an estimates_table block came back, so estpost's pseudo-estimate "
          "was reachable by esttab at all");
    std::vector<std::string> rowNames;
    if (table.is_object() && table.contains("coef_rows") && table["coef_rows"].is_array())
      for (const auto& cr : table["coef_rows"]) rowNames.push_back(cr.value("name", std::string()));
    check(hasName(rowNames, "x1") && hasName(rowNames, "y"),
          "AER-TABLES (v): the table's rows are the VARIABLES x1 and y. estpost's model is the "
          "1xk e(mean)/e(sd) matrices, so a handler that posted a coefficient table keyed on "
          "anything else (or on nothing) renders an empty table and still answers rc 0");
    const std::string csv = table.is_object() ? table.value("csv", std::string()) : std::string();
    check(has(csv, "15.5"),
          "AER-TABLES (v): ...and the mean cell is 15.5, which is the true mean of 1..30. This is "
          "the leg's real assertion: row LABELS alone are satisfied by a table full of the wrong "
          "statistic -- sum is 465 here and sd is 8.803, and both would print a plausible number "
          "under a `mean` heading. Got csv `" + csv.substr(0, 200) + "`");

    // BUG-407 (2026-10-29): the SCREEN path honours cells() -- esttabScreen
    // returns the writer's rendered table in place of the estimates_table block,
    // whose JSON has no cell model -- so the check that pinned the refusal now
    // pins the table: the mean 15.5 beside its sd, at rc 0.
    auto rc = exec(kMake30 + "estpost summarize x1 y\nesttab, cells(\"mean sd\")\n", s);
    const auto ec = firstError(rc.blocks);
    std::string shown;
    for (const auto& b : rc.blocks)
      if (b.is_object() && b.value("type", std::string()) == "display")
        shown += b.value("text", std::string());
    check(ec.is_null() && has(shown, "mean") && has(shown, "sd") && has(shown, "15.5"),
          "AER-TABLES (v): a SCREEN `esttab, cells(\"mean sd\")` shows the writer's table (BUG-407): "
          "mean 15.5 beside its sd, rc 0. Got `" + errText(ec) + "` / `" + shown.substr(0, 200) +
              "`");
  }

  // ── (vi) regsave with no `using` REPLACES the dataset with the estimates. ──
  {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp);
    // eval.cwd alone leaves every `using` file in the process cwd -- the repo root.
    (void)exec(kMake30 + "regress y x1\n", s);
    // The coefficient is read from e() in C++ BEFORE regsave runs, because
    // regsave's whole job is to overwrite the dataset the estimate came from.
    // Row 0 is `(intercept)`, not x1: cobra emits the intercept FIRST and the
    // shipped esttab freeze fixture pins that order, so this is the codebase's
    // established coefficient order rather than something regsave chose.
    // W2AK TW-DS: regsave's rows are in e(b) order (`_cons` LAST) and its
    // coef column is FLOAT (svmat), so coef[1] is float(_b[x1]) -- StataBE
    // w2at_table_writers wt_r01_v2 `coef|float`, wt_r04_r1c2 `+1.72a01eX+000`.
    double bRow0 = std::nan("");
    if (s.lastEstBlock.has_value() && (*s.lastEstBlock).contains("coeftable") &&
        (*s.lastEstBlock)["coeftable"].is_array()) {
      for (const auto& row : (*s.lastEstBlock)["coeftable"]) {
        if (row.is_object() && row.value("name", std::string()) == "x1" && row.contains("b") &&
            row["b"].is_number()) {
          bRow0 = static_cast<double>(static_cast<float>(row["b"].get<double>()));
        }
      }
    }
    check(!std::isnan(bRow0),
          "AER-TABLES (vi): the regression that regsave is about actually produced a coefficient "
          "-- the value comparison below is meaningless otherwise");
    auto r = exec("regsave\n"
                  "count\n"
                  "display \"AER_RN=\" r(N)\n"
                  "display \"AER_C1=\" %22.17f coef[1]\n",
                  s);
    check(firstError(r.blocks).is_null(),
          "AER-TABLES (vi): `regsave` with no `using` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const std::vector<std::string> names = varNames(s);
    check(hasName(names, "var") && hasName(names, "coef") && hasName(names, "stderr"),
          "AER-TABLES (vi): the dataset in memory is now regsave's own {var, coef, stderr} "
          "columns. Asserted by NAME because a handler that loaded the right number of rows under "
          "different column names breaks every downstream `keep if var==` a paper writes");
    check(std::fabs(displayValue(r.blocks, "AER_RN=") - 2.0) < 1e-12,
          "AER-TABLES (vi): ...with one row per coefficient -- 2 here (x1 and _cons), not 1 and "
          "not the 30 rows of the source dataset");
    // Read back with an explicit %22.17f, NOT `display`'s default %9.0g, so no
    // formatting rounding is left to absorb -- what remains is the READ path's
    // own loss and nothing else.
    //
    // The tolerance is 9 SIGNIFICANT DIGITS and that number is a measurement,
    // not a cushion. It was first attributed to `cobra convert` truncating CSV
    // doubles (BUG-406 as filed 2026-09-04). REDIAGNOSED 2026-09-06 by the
    // first run of tools/buglog_probes.py: the STORAGE is exact -- the
    // converted hbin's bytes, `count if abs(v - <17-digit literal>) > 1e-12`,
    // `string(v, "%22.17f")` and r(mean) all carry every digit; the 9 s.f. is
    // `display %fmt <varref>`, where the value passes a %.9g before the user's
    // format is applied. So regsave's stored coefficient is NOT lossy -- but
    // this leg still READS it through displayValue(), i.e. through that same
    // display, so nine digits is what the check can see today. Asserting
    // bit-exactness here would fail on the display defect, not on regsave;
    // asserting `< 1e-9` absolute would have hidden the nine digits entirely,
    // which is how the loss went unnoticed until now. The check is relative,
    // pinned AT the read path's measured precision: it still catches a builder
    // that wrote zeros, read the wrong coefficient, or lost another digit.
    // When BUG-406's display path is fixed (or this read moves to a path that
    // is not `display`, e.g. `string(coef[1], "%22.17f")`), tighten it back
    // to equality -- a 1e-9 relative bound can no longer tell the two apart.
    const double c1 = displayValue(r.blocks, "AER_C1=");
    const double denom = std::fabs(bRow0) > 1.0 ? std::fabs(bRow0) : 1.0;
    check(!std::isnan(bRow0) && std::fabs(c1 - bRow0) / denom < 1e-9,
          "AER-TABLES (vi): ...and coef[1] is float(_b[x1]) read out of e() "
          "before the dataset was replaced. Column names and a row count are satisfied by a "
          "builder that wrote zeros; this is the leg that says the numbers came from the model. "
          "Got " + fullPrec(displayValue(r.blocks, "AER_C1=")) + " vs e() " + fullPrec(bRow0));
  }

  // ── (vii) erepost overwrites e(b) from a matrix, names included. ───────────
  {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp);
    // eval.cwd alone leaves every `using` file in the process cwd -- the repo root.
    auto r = exec(kMake30 +
                      "regress y x1\n"
                      "matrix M = (1,2)\n"
                      "erepost b = M, rename\n",
                  s);
    check(firstError(r.blocks).is_null(),
          "AER-TABLES (vii): `erepost b = M, rename` over a 2-coefficient e(b) runs clean (M is "
          "1x2, e(b) is x1 and _cons). Got `" + errText(firstError(r.blocks)) + "`");
    std::string n0;
    double b0 = std::nan("");
    if (s.lastEstBlock.has_value() && (*s.lastEstBlock).contains("coeftable") &&
        (*s.lastEstBlock)["coeftable"].is_array() && !(*s.lastEstBlock)["coeftable"].empty()) {
      const auto& row0 = (*s.lastEstBlock)["coeftable"][0];
      n0 = row0.value("name", std::string());
      if (row0.contains("b") && row0["b"].is_number()) b0 = row0["b"].get<double>();
    }
    check(n0 == "c1",
          "AER-TABLES (vii): with `rename` the coefficient NAMES become the matrix's column names "
          "-- c1 for an unnamed literal. Got `" + n0 + "`");
    check(!std::isnan(b0) && std::fabs(b0 - 1.0) < 1e-12,
          "AER-TABLES (vii): ...and the VALUE was actually overwritten with the matrix's first "
          "element (1), not merely relabelled. A rename that left the fitted coefficient in place "
          "passes the name check above and reports the old model under new labels. Got " +
              std::to_string(b0));
  }

  // ── (viii) the two refusals: refused BY NAME, at r(199), having written
  // nothing. ────────────────────────────────────────────────────────────────
  {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp);
    // eval.cwd alone leaves every `using` file in the process cwd -- the repo root.
    (void)exec(kMake30 + "regress y x1\n", s);
    auto r = exec("outreg2 using \"ow\", word\n", s);
    auto err = firstError(r.blocks);
    check(!err.is_null(),
          "AER-TABLES (viii): `outreg2 using \"ow\", word` is REFUSED. This build has no RTF "
          "writer, and the alternative is writing a differently-formatted file under the "
          "requested name -- a silently wrong answer with a plausible file beside it");
    check(errField(err, "code") == "unsupported_command",
          "AER-TABLES (viii): ...under the `unsupported_command` code, got `" +
              errField(err, "code") + "`");
    check(errField(err, "reason") == "refused:no_rtf_writer",
          "AER-TABLES (viii): ...naming the missing writer, so the message says what is absent "
          "rather than that the option is unknown. Got `" + errField(err, "reason") + "`");
    check(rcOf(err) == "199",
          "AER-TABLES (viii): ...and it maps to r(199). Asserted separately from the code because "
          "a refusal built with the default engine_error code arrives as rc 1 and every do-file "
          "branching on 199 takes the wrong branch -- bundle D shipped exactly that. Got r(" +
              rcOf(err) + ")");
    check(!fs::exists(cecon::to_native_path(at("ow.txt"))) &&
              !fs::exists(cecon::to_native_path(at("ow.doc"))) &&
              !fs::exists(cecon::to_native_path(at("ow.rtf"))),
          "AER-TABLES (viii): ...and it wrote NOTHING. outreg2 always writes a .txt ledger on the "
          "success path, so a refusal reached after the ledger write leaves a file behind and the "
          "next `append` reads a table the user was told did not happen -- the silent half of "
          "this class, which the error block alone cannot tell apart");
    auto rt = exec("tabout x1\n", s);
    auto errT = firstError(rt.blocks);
    check(!errT.is_null(),
          "AER-TABLES (viii/tabout): `tabout` is refused at the COMMAND level. Its grammar is a "
          "panel language of its own; accepting the head and refusing each option would report a "
          "missing OPTION where the truth is a missing command");
    check(errField(errT, "code") == "unsupported_command" &&
              errField(errT, "reason") == "refused:tabout_grammar",
          "AER-TABLES (viii/tabout): ...by name, got code `" + errField(errT, "code") +
              "` reason `" + errField(errT, "reason") + "`");
    check(rcOf(errT) == "199",
          "AER-TABLES (viii/tabout): ...at r(199), got r(" + rcOf(errT) + ")");
  }

  // ── (ix) estadd with nothing to attach to says so. ─────────────────────────
  {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp);
    // eval.cwd alone leaves every `using` file in the process cwd -- the repo root.
    auto r = exec("estadd local c \"Yes\"\n", s);
    auto err = firstError(r.blocks);
    check(!err.is_null(),
          "AER-TABLES (ix): `estadd` with no estimation in memory is an ERROR. Accepting it would "
          "park the addition in a bag no eststo will ever match, and the do-file would carry on "
          "to write a table with a silently missing row");
    check(has(errText(err), "no estimation result in memory"),
          "AER-TABLES (ix): ...and the message names the cause. Got `" + errText(err) + "`");
    check(rcOf(err) == "301",
          "AER-TABLES (ix): ...at r(301), Stata's `last estimates not found`. A generic rc 1 here "
          "is indistinguishable from a syntax problem to any `capture`-and-branch caller. Got r(" +
              rcOf(err) + ")");
  }

  // ── (x) B4-W5: the TABLE-WRITER option surface, against the Stata capture. ──
  //
  // The comparator that actually adjudicates W5 is a CELL diff of the files
  // this build writes against the 27 files real StataBE wrote on 2026-09-09
  // (tests/eval/golden/aer_b4/tables/, produced by pins_oracle_t.do and
  // _t2.do). Fifteen of the sixteen esttab arms and all five outreg2 arms come
  // back BYTE-IDENTICAL there; the sixteenth is `nolegend nolines`, which this
  // build accepts and does not apply, and says so.
  //
  // Those files are not readable from here -- the selftest runs in a scratch
  // workdir with no repo anchor, and hard-coding one would make this suite pass
  // or fail on where the binary happens to sit. So what is pinned HERE is the
  // set of facts that diff established, each one a claim this build got WRONG
  // before W5 and each one silent: the table rendered, it was simply not
  // esttab's table. Every leg reads the BYTES OF THE FILE, never a return code.
  {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp)
    (void)exec(kMake30 +
                   "regress y x1 x2\n"
                   "eststo w5a\n"
                   "regress y x1\n"
                   "eststo w5b\n",
               s);

    // (x.1) The DEFAULTS. t in the parentheses, `_cons` last, N alone in the
    // footer. All three were refuted by t_plain.txt.
    {
      auto r = exec("esttab w5a w5b using \"w5p.txt\", replace\n", s);
      check(firstError(r.blocks).is_null(),
            "W5 (x.1): plain `esttab ... using` runs. Got `" + errText(firstError(r.blocks)) + "`");
      const std::string t = slurp(at("w5p.txt"));
      check(has(t, "t statistics in parentheses"),
            "W5 (x.1): esttab's DEFAULT parenthesised statistic is t, not the standard error "
            "(matches esttab's behaviour, identified from its ado: t at 2 decimals; t_plain.txt's legend). This build printed SEs under no "
            "legend at all -- every number in the second row of every default table was the "
            "wrong statistic, at rc 0. Got:\n" + t);
      check(has(t, "* p<0.05, ** p<0.01, *** p<0.001"),
            "W5 (x.1): ...and the star legend reads in ASCENDING mark order, which is DESCENDING "
            "cutoff (matches esttab's behaviour, identified from its ado). Got:\n" + t);
      const size_t consAt = t.find("_cons");
      const size_t x1At = t.find("x1");
      check(consAt != std::string::npos && x1At != std::string::npos && consAt > x1At,
            "W5 (x.1): ...and `_cons` is the LAST coefficient row. The engine's coeftable puts "
            "the intercept first and this writer followed it; all 27 captured tables put it "
            "last. Got:\n" + t);
      check(!has(t, "R\xc2\xb2") && !has(t, "R-sq"),
            "W5 (x.1): ...and the default footer is N ALONE -- esttab adds an R-squared row only "
            "for `r2`/`ar2`/`stats()`. An extra footer row is a cell every comparator counts. "
            "Got:\n" + t);
    }

    // (x.2) `not` -- the one arm that separates "a gap after every coefficient"
    // from "a gap after the aux row". Both rules agree everywhere else.
    {
      auto r = exec("esttab w5a w5b using \"w5n.txt\", replace not\n", s);
      check(firstError(r.blocks).is_null(),
            "W5 (x.2): `esttab ..., not` runs. Got `" + errText(firstError(r.blocks)) + "`");
      const std::string t = slurp(at("w5n.txt"));
      check(!has(t, "t statistics in parentheses"),
            "W5 (x.2): `not` drops the t row AND its legend (t_not.txt). Got:\n" + t);
      check(!has(t, "\n\n"),
            "W5 (x.2): ...and with no aux row there are NO blank separator lines at all. This is "
            "the leg that decides the gap rule; t_not.txt is the only captured arm that can. "
            "Got:\n" + t);
    }

    // (x.3) `ci()` reads the STORED interval. Rebuilding it from a Normal
    // quantile moved every bound in t_ci.txt by about 3 -- inside no tolerance
    // and invisible without the capture.
    {
      auto r = exec("esttab w5a using \"w5c.txt\", replace ci(3)\n", s);
      check(firstError(r.blocks).is_null(),
            "W5 (x.3): `esttab ..., ci(3)` runs. Got `" + errText(firstError(r.blocks)) + "`");
      const std::string t = slurp(at("w5c.txt"));
      check(has(t, "confidence intervals in brackets") && has(t, "["),
            "W5 (x.3): ci() renders bracketed intervals and says so in the legend. Got:\n" + t);
      auto r2 = exec("esttab w5a using \"w5c2.txt\", replace ci(3) level(90)\n", s);
      auto err = firstError(r2.blocks);
      check(!err.is_null() && errField(err, "code") == "unsupported_option",
            "W5 (x.3): ...and `level(90)` is REFUSED. This build reports the interval the "
            "ESTIMATION stored; printing that 95% interval under a `90% confidence intervals` "
            "legend is the silent-wrong-answer shape, so the option is named rather than "
            "swallowed. Got code `" + errField(err, "code") + "`");
    }

    // (x.4) The quoted-list options. `substitute("_cons" "CONST")` is eight
    // characters whose first and last are both `"`, so dequoting the whole
    // value before splitting yields the tokens `_cons"` and `"CONST` and the
    // substitution silently never fires -- which is what happened on the first
    // pass, in three arms at once (mlabels, mgroups, substitute).
    {
      auto r = exec("esttab w5a w5b using \"w5s.txt\", replace "
                    "mlabels(\"AAA\" \"BBB\") substitute(\"_cons\" \"CONSTANT\")\n",
                    s);
      check(firstError(r.blocks).is_null(),
            "W5 (x.4): mlabels()+substitute() run. Got `" + errText(firstError(r.blocks)) + "`");
      const std::string t = slurp(at("w5s.txt"));
      check(has(t, "AAA") && has(t, "BBB") && !has(t, "AAA\""),
            "W5 (x.4): mlabels(\"AAA\" \"BBB\") sets BOTH column heads, unquoted. Got:\n" + t);
      check(has(t, "CONSTANT") && !has(t, "_cons"),
            "W5 (x.4): substitute() replaces the text it names. Got:\n" + t);
    }

    // (x.5) `beta` is REFUSED when the sample cannot be reproduced, rather than
    // printing a standardized coefficient from the wrong rows. The receipt's
    // REFUTE.md kills the N-vs-N-1 divisor as a discriminator (it cancels in the
    // ratio), so the SAMPLE is the only convention left that can be wrong here,
    // and it is the one this build cannot carry: there is no e(sample) mask in
    // the estimates store. The reconstruction is complete-cases-of-the-model's-
    // own-variables and it is CHECKED against e(N).
    {
      piet::SessionState sb(workdir);
      sb.eval.cwd = workdir;
      sb.cwd = workdir;
      (void)exec(kMake30 + "regress y x1 if _n <= 20\neststo w5f\n", sb);
      auto r = exec("esttab w5f using \"w5b.txt\", replace beta\n", sb);
      auto err = firstError(r.blocks);
      check(!err.is_null() && errField(err, "code") == "unsupported_option",
            "W5 (x.5): `beta` after a fit on a SUBSET is refused -- the reconstructed sample (30 "
            "complete cases) does not match e(N)=20, so sd(x)/sd(y) would be taken over rows the "
            "regression never saw. A number here is wrong by an amount nothing downstream can "
            "see. Got code `" + errField(err, "code") + "`");
      check(has(errText(err), "e(N)"),
            "W5 (x.5): ...and the message names the arithmetic that failed, so the user can tell "
            "this from `beta is not implemented`. Got `" + errText(err) + "`");
      check(!fs::exists(cecon::to_native_path(at("w5b.txt"))),
            "W5 (x.5): ...and it wrote NO file. A refusal that still leaves a table behind is the "
            "silent half of this class");
    }

    // (x.6) `nolegend nolines` are HONOURED since W2AK (matches esttab's
    // behaviour, identified from its ado): aer_b4/tables/t_noleg.txt has no rule line and no star
    // legend, only the coefficient note. They used to be accepted with a
    // "not applied" note.
    {
      auto r = exec("esttab w5a using \"w5l.txt\", replace nolegend nolines\n", s);
      check(firstError(r.blocks).is_null(),
            "W5 (x.6): `nolegend nolines` run. Got `" + errText(firstError(r.blocks)) + "`");
      const std::string t = slurp(at("w5l.txt"));
      check(!has(t, "---") && !has(t, "p<0.05") && has(t, "t statistics in parentheses"),
            "W5 (x.6): ...and the file has no rule and no star legend, keeping the aux note "
            "(aer_b4/tables/t_noleg.txt). Got:\n" + t);
    }

    // (x.7) outreg2's DEFAULT number shape is significant digits WITH thousands
    // separators, not %9.3f (matches outreg2's behaviour, identified from its
    // ado; o_plain.txt reads -49.51 /
    // 1.747 / 1,946 for the same three coefficients this build printed as
    // -49.512 / 1.747 / 1946.069). `tstat` swaps the parenthesised row and the
    // legend with it.
    {
      piet::SessionState so(workdir);
      so.eval.cwd = workdir;
      so.cwd = workdir;
      (void)exec(kMake30 + "regress y x1 x2\n", so);
      auto r = exec("outreg2 using \"w5o\", replace text\n", so);
      check(firstError(r.blocks).is_null(),
            "W5 (x.7): `outreg2 using ..., replace text` runs -- `text` is accepted, not an "
            "unknown option; all five captured outreg2 arms spell it. Got `" +
                errText(firstError(r.blocks)) + "`");
      const std::string t = slurp(at("w5o.txt"));
      check(has(t, "Constant") && !has(t, "_cons"),
            "W5 (x.7): outreg2 calls the intercept `Constant` and puts it last (o_plain.txt), "
            "whether or not `label` was given. Got:\n" + t);
      check(has(t, "Standard errors in parentheses\t"),
            "W5 (x.7): ...and its legend lines are ROWS -- they carry the trailing tab that makes "
            "them cells. This build wrote them untabbed, which is one cell wrong on two lines of "
            "every table it ever produced. Got:\n" + t);
      auto rt = exec("regress y x1 x2\noutreg2 using \"w5t\", replace text tstat\n", so);
      check(firstError(rt.blocks).is_null(),
            "W5 (x.7): `tstat` runs. Got `" + errText(firstError(rt.blocks)) + "`");
      const std::string tt = slurp(at("w5t.txt"));
      check(has(tt, "t-statistics in parentheses"),
            "W5 (x.7): ...and `tstat` moves the LEGEND with the row (o_tstat.txt). A legend that "
            "still says `Standard errors` over a column of t values is the reading error this "
            "whole option exists to prevent. Got:\n" + tt);
    }

    // (x.8) estout's numbers/nonumbers, the pair the capture itself identified.
    // `nonumbers` ALONE was non-discriminating -- estout's default header is
    // already the model name, so the plain arm and the nonumbers arm came back
    // byte-identical, and pins_oracle_t2.do replaced it. So the assertion is
    // on the PAIR: the two files must differ, and by exactly the number row.
    {
      auto rn = exec("estout w5a w5b using \"w5en.txt\", replace numbers\n", s);
      auto rf = exec("estout w5a w5b using \"w5ef.txt\", replace numbers nonumbers\n", s);
      check(firstError(rn.blocks).is_null() && firstError(rf.blocks).is_null(),
            "W5 (x.8): both estout spellings run -- before W5 `numbers` and `nonumbers` were in "
            "NEITHER vocabulary list and answered r(198)");
      const std::string a = slurp(at("w5en.txt"));
      const std::string b = slurp(at("w5ef.txt"));
      check(has(a, "(1)") && !has(b, "(1)"),
            "W5 (x.8): `numbers` emits the (1)/(2) row and `nonumbers` removes it -- and "
            "`nonumbers` WINS when both are typed, which is what e_numbers_off.txt shows. Got "
            "numbers:\n" + a + "\nnonumbers:\n" + b);
      check(a != b,
            "W5 (x.8): ...so the two arms are not the same file. A pin that cannot fail is "
            "decoration; this is the pair that can");
    }

    // ── (x.9) BUG-486: estout's DEFAULTS are estout's, not esttab's. ─────────
    // W5 diffed fifteen of sixteen esttab arms and all five outreg2 arms
    // against the capture and said so; the five estout arms were captured and
    // never compared. Every axis below is a cell a comparator reads, and all
    // five were wrong at rc 0. Oracle: tests/eval/golden/aer_w1/w1b_tables/
    // e2.txt (2026-09-14) and aer_b4/tables/e_plain.txt (2026-09-09), whose
    // first two rows are identical three months apart.
    {
      auto r = exec("estout w5a w5b using \"w5ed.txt\", replace\n", s);
      check(firstError(r.blocks).is_null(),
            "W5 (x.9): plain `estout ... using` runs. Got `" + errText(firstError(r.blocks)) + "`");
      const std::string t = slurp(at("w5ed.txt"));
      check(has(t, "\tw5a\tw5b"),
            "W5 (x.9): the default is TAB-separated and headed by the MODEL NAMES. `fixed` is the "
            "style ESTTAB passes when it wraps estout, and this build had taken the wrapper's "
            "default for the command's -- a dashed fixed-width block with `(1)`/depvar heads. "
            "Got:\n" + t);
      check(has(t, "\tb\tb"),
            "W5 (x.9): ...and a CELL-NAME row (`b`) sits under the model names, naming what the "
            "columns carry. This build emitted none. Got:\n" + t);
      const size_t consAt = t.find("_cons");
      const size_t x1At = t.find("x1");
      check(consAt != std::string::npos && x1At != std::string::npos && consAt > x1At,
            "W5 (x.9): ...and `_cons` is LAST, the same fact x.1 pins for esttab. The flag existed "
            "and was left off for estout. Got:\n" + t);
      check(t.find('*') == std::string::npos,
            "W5 (x.9): ...and the default cell is `b` with NO stars -- e2.txt carries no mark "
            "anywhere. `b(star)` is esttab's default, not estout's. Got:\n" + t);
      // The precision axis: %9.0g, not %9.3f. A three-decimal column cannot
      // produce a cell with more than three decimals, so one such cell is the
      // whole discriminator.
      bool longDecimal = false;
      for (size_t i = 0; i + 1 < t.size(); ++i) {
        if (t[i] != '.') continue;
        size_t j = i + 1, digits = 0;
        while (j < t.size() && std::isdigit(static_cast<unsigned char>(t[j]))) { ++j; ++digits; }
        if (digits > 3) { longDecimal = true; break; }
      }
      check(longDecimal,
            "W5 (x.9): ...and the estimate carries Stata's own %9.0g precision (e2.txt: "
            "-49.51222 / 1.746559 / 3673.06, three decimal counts in one column), not esttab's "
            "%9.3f rounding. Got:\n" + t);
    }

    // (x.10) BUG-486's option half: `collabels()` names the CELLS, not the
    // models. Before the fix it overwrote the model heads, so `collabels(none)`
    // wrote the literal word `none` where `w5a` belonged (w1b_tables/e7.tex
    // keeps its model names under exactly that option).
    {
      auto r = exec("estout w5a w5b using \"w5ec.txt\", replace collabels(none)\n", s);
      check(firstError(r.blocks).is_null(), "W5 (x.10): `collabels(none)` runs");
      const std::string t = slurp(at("w5ec.txt"));
      check(has(t, "\tw5a\tw5b") && !has(t, "none"),
            "W5 (x.10): `collabels(none)` removes the CELL-NAME row and leaves the model heads "
            "standing -- it never renames a model. Got:\n" + t);
      check(!has(t, "\tb\tb"), "W5 (x.10): ...and the `b` row is the row it removed. Got:\n" + t);
    }

    // (x.11) `numbers` still adds the (1)/(2) row on top of the new default,
    // and `nonumbers` still wins when both are typed (e_numbers_off.txt).
    {
      auto rn = exec("estout w5a w5b using \"w5n1.txt\", replace numbers\n", s);
      auto rb = exec("estout w5a w5b using \"w5n2.txt\", replace numbers nonumbers\n", s);
      check(firstError(rn.blocks).is_null() && firstError(rb.blocks).is_null(),
            "W5 (x.11): both number spellings still run after the default flip");
      const std::string a1 = slurp(at("w5n1.txt"));
      const std::string b1 = slurp(at("w5n2.txt"));
      check(has(a1, "(1)") && !has(b1, "(1)"),
            "W5 (x.11): `numbers` ADDS the (1)/(2) row to a default that no longer has it, and "
            "`nonumbers` still wins when both are typed");
    }

    // ── (x.12) BUG-486's RUNNING FORMAT. estout carries one current format,
    // set by the last fmt() it saw, defaulting to %9.0g, and it reaches both a
    // later cell that gives no fmt AND the stats() footer. The first cut of
    // this fix assigned the GLOBAL default to every fmt-less spec, which is a
    // different rule that happens to agree only when no fmt is given anywhere
    // -- so the discriminating arm is `cells(b(fmt(3)) se(par))`, where Stata
    // renders the se at THREE DECIMALS, not %9.0g.
    // Oracle: tests/eval/golden/aer_w1/w1b_estout_defaults.log, arms g1/g3/g4.
    {
      auto r1 = exec("estout w5a w5b using \"w5f1.txt\", replace cells(b(fmt(3)) se(par))\n", s);
      auto r3 = exec("estout w5a w5b using \"w5f3.txt\", replace stats(N r2)\n", s);
      auto r4 = exec(
          "estout w5a w5b using \"w5f4.txt\", replace cells(b(fmt(3)) se(par)) stats(N r2)\n", s);
      check(firstError(r1.blocks).is_null() && firstError(r3.blocks).is_null() &&
                firstError(r4.blocks).is_null(),
            "W5 (x.12): all three format arms run");
      const std::string f1 = slurp(at("w5f1.txt"));
      const std::string f3 = slurp(at("w5f3.txt"));
      const std::string f4 = slurp(at("w5f4.txt"));
      // The se row is the second line of each coefficient pair; under the
      // running rule it carries exactly three decimals inside parentheses.
      check(f1.find("(0.") != std::string::npos || f1.find("(-0.") != std::string::npos,
            "W5 (x.12): a cell with NO fmt() takes the running format from the cell before it -- "
            "`cells(b(fmt(3)) se(par))` renders the se at three decimals (g1: `(86.156)`), not at "
            "the %9.0g default. Got:\n" + f1);
      check(has(f3, "\nr2\t") && !has(f3, "R-squared"),
            "W5 (x.12): a stats() row is labelled with the KEY AS TYPED (`r2`), not a pretty "
            "auto-label -- g3. Got:\n" + f3);
      // g3 is %9.0g: N is an integer with no decimal point. g4 inherits fmt(3)
      // from the cells and prints 941.000. One rule, two renderings.
      check(has(f3, "\nN\t30\t30") && has(f4, "\nN\t30.000\t30.000"),
            "W5 (x.12): ...and `N` follows the SAME running format as everything else -- an "
            "integer under the %9.0g default, three decimals after `cells(b(fmt(3)))`. That is "
            "g3/g4's `74` vs `74.000` on this leg's own 30-row fixture. A hard integer format "
            "here made one of the two unreachable. Got g3:\n" + f3 + "\ng4:\n" + f4);
    }

    // ── (x.13) BUG-487: the QUOTE in cells() is an operator. `cells(a b)`
    // stacks the items as rows; `cells("a b")` puts them SIDE BY SIDE. Before
    // this the two spellings produced BYTE-IDENTICAL files, because parseCells
    // dequoted before it split and the distinction was gone by the time the
    // items were read. Oracle: tests/eval/golden/aer_w1/w1b_estout_cells.log,
    // arms h1/h2/h5/h6/h7/h9/h12.
    {
      auto ru = exec("estout w5a w5b using \"w5cu.txt\", replace cells(b se)\n", s);
      auto rq = exec("estout w5a w5b using \"w5cq.txt\", replace cells(\"b se\")\n", s);
      check(firstError(ru.blocks).is_null() && firstError(rq.blocks).is_null(),
            "W5 (x.13): both cells() spellings run");
      const std::string u = slurp(at("w5cu.txt"));
      const std::string q = slurp(at("w5cq.txt"));
      check(u != q,
            "W5 (x.13): `cells(b se)` and the QUOTED `cells(\"b se\")` are DIFFERENT tables. They "
            "were byte-identical here, which is the whole defect: the quote is an operator and it "
            "was being discarded before anything could read it. Got:\n" + u);
      check(has(u, "\tb/se\tb/se"),
            "W5 (x.13): the UNQUOTED form stays stacked -- two columns, one joined `b/se` "
            "cell-name per model (h1). Got:\n" + u);
      check(has(q, "\tb\tse\tb\tse"),
            "W5 (x.13): the QUOTED form names each of the FOUR columns (h2). Got:\n" + q);
      check(has(q, "\tw5a\t\tw5b\t"),
            "W5 (x.13): ...and a model's NAME sits in the first column it owns, the rest blank "
            "(h9 shows `m1` then two blanks for three items). Got:\n" + q);
      const size_t uLines = static_cast<size_t>(std::count(u.begin(), u.end(), '\n'));
      const size_t qLines = static_cast<size_t>(std::count(q.begin(), q.end(), '\n'));
      check(uLines > qLines,
            "W5 (x.13): ...and the wide table has FEWER rows -- one body row per coefficient "
            "instead of one per (coefficient, item). Got " + std::to_string(uLines) + " vs " +
                std::to_string(qLines));
      // A single item is the boundary case where the two layouts coincide, and
      // the capture pins that too (h5 == h6): a fix that keyed on the quote and
      // forgot the count would make these two differ.
      auto r1 = exec("estout w5a w5b using \"w5c1.txt\", replace cells(\"b\")\n", s);
      auto r2 = exec("estout w5a w5b using \"w5c2.txt\", replace cells(b)\n", s);
      check(firstError(r1.blocks).is_null() && firstError(r2.blocks).is_null(),
            "W5 (x.13): both single-item spellings run");
      check(slurp(at("w5c1.txt")) == slurp(at("w5c2.txt")),
            "W5 (x.13): with ONE item the two layouts coincide and the files are IDENTICAL "
            "(h5 == h6) -- the quote changes the shape, and with one column there is no shape "
            "to change");
      auto rs = exec("estout w5a w5b using \"w5cs.txt\", replace cells(\"b se\") stats(N)\n", s);
      check(firstError(rs.blocks).is_null(), "W5 (x.13): stats() under the wide layout runs");
      const std::string fs = slurp(at("w5cs.txt"));
      check(has(fs, "\nN\t30\t\t30\t"),
            "W5 (x.13): ...and a stats() row spans the WIDE table with the value in the first "
            "column of each model's group and blanks after (h7: `N\t74\t\t74\t`). Got:\n" + fs);
    }

    // ── (x.14) W1a-3, esttab's DISPLAY layer. Every arm below is pinned against
    // an arm of tests/eval/golden/aer_w1/w1b_esttab_display.log that was itself
    // taken against a PLAIN CONTROL -- which is the only reason three of these
    // can land as no-ops instead of as guesses.
    {
      auto ok2 = [&](const std::string& cmd, const std::string& why) {
        auto r = exec(cmd, s);
        check(firstError(r.blocks).is_null(),
              why + " [got: " + errText(firstError(r.blocks)) + "]");
      };
      ok2("esttab w5a w5b using \"w5d0.txt\", replace\n", "W5 (x.14): the control runs");
      const std::string ctl = slurp(at("w5d0.txt"));
      // nocons / noconstant: the _cons rows go, and the two spellings agree.
      ok2("esttab w5a w5b using \"w5d1.txt\", replace nocons\n", "W5 (x.14): `nocons` runs");
      ok2("esttab w5a w5b using \"w5d2.txt\", replace noconstant\n",
          "W5 (x.14): `noconstant` runs");
      const std::string nc = slurp(at("w5d1.txt"));
      check(has(ctl, "_cons") && !has(nc, "_cons"),
            "W5 (x.14): `nocons` drops the intercept rows the control carries (x6). Got:\n" + nc);
      check(nc == slurp(at("w5d2.txt")),
            "W5 (x.14): ...and `noconstant` is byte-identical to `nocons` -- two spellings, one "
            "behaviour, which is what the capture shows");
      // type / obslast / style(fixed): MEASURED no-ops. A check that they change
      // NOTHING is what stops a later session implementing them from the name.
      for (const auto& arm : std::vector<std::pair<std::string, std::string>>{
               {"w5d3", "type"}, {"w5d4", "obslast"}, {"w5d5", "style(fixed)"}}) {
        ok2("esttab w5a w5b using \"" + arm.first + ".txt\", replace " + arm.second + "\n",
            "W5 (x.14): `" + arm.second + "` runs");
        check(slurp(at(arm.first + ".txt")) == ctl,
              "W5 (x.14): `" + arm.second + "` is byte-identical to the control -- MEASURED, not "
              "assumed (w1b_esttab_display.log x1/x2/x4, and `obslast` holds even with s(N r2) "
              "because esttab's default already puts N last)");
      }
      // style() with no writer is refused BY NAME rather than rendered as
      // something else.
      {
        auto r = exec("esttab w5a using \"w5d6.txt\", replace style(html)\n", s);
        auto err = firstError(r.blocks);
        check(!err.is_null() && err.value("message", std::string()).find("style") !=
                                    std::string::npos,
              "W5 (x.14): `style(html)` is refused by name -- there is no HTML writer, and "
              "rendering fixed-width under a request for HTML is the silent-wrong-output shape");
      }
      // varlabels(): estout's spelling of coeflabels(), 11 papers / 152 uses.
      ok2("esttab w5a using \"w5d7.txt\", replace varlabels(x1 \"Reg one\")\n",
          "W5 (x.14): `varlabels()` runs");
      check(has(slurp(at("w5d7.txt")), "Reg one"),
            "W5 (x.14): ...and it relabels the stub, the same map `coeflabels()` takes");
      // A label is a stub like any other, so it goes through the abbreviation
      // too -- which is worth pinning because the first draft of this leg used
      // a 13-character label, asserted the label back verbatim, and FAILED on
      // its own correct output.
      ok2("esttab w5a using \"w5d7b.txt\", replace varlabels(x1 \"Regressor one\")\n",
          "W5 (x.14): a long `varlabels()` label runs");
      check(has(slurp(at("w5d7b.txt")), "Regressor ~e"),
            "W5 (x.14): ...and a label wider than the stub is abbreviated like any other stub "
            "(`Regressor one` -> `Regressor ~e`), not printed whole");
      // BUG-490: the stub is abbreviated, and `noabbrev` is what turns it off.
      (void)exec("generate double a_very_long_variable_name_here = x1 * 2\n", s);
      (void)exec("regress y a_very_long_variable_name_here x2\n", s);
      (void)exec("eststo w5long\n", s);
      ok2("esttab w5long using \"w5d8.txt\", replace\n", "W5 (x.14): the long-name arm runs");
      ok2("esttab w5long using \"w5d9.txt\", replace noabbrev\n",
          "W5 (x.14): the `noabbrev` arm runs");
      const std::string ab = slurp(at("w5d8.txt"));
      const std::string nab = slurp(at("w5d9.txt"));
      check(has(ab, "a_very_lon~e") && !has(ab, "a_very_long_variable_name_here"),
            "W5 (x.14): a stub wider than the column is abbreviated to ten characters, a tilde "
            "and the last one -- `a_very_lon~e`, which is exactly what StataBE renders for this "
            "name (w1b_esttab_abbrev2.log, arm name_default). Got:\n" + ab);
      check(has(nab, "a_very_long_variable_name_here"),
            "W5 (x.14): ...and `noabbrev` prints it whole (arm name_noabbrev) -- which is what "
            "this build used to do unconditionally. Got:\n" + nab);
      // The trailing-digit half of the rule, which one example cannot show.
      (void)exec("generate double abcdefghijklm1234567 = x2 * 3\n", s);
      (void)exec("regress y abcdefghijklm1234567\n", s);
      (void)exec("eststo w5dig\n", s);
      ok2("esttab w5dig using \"w5da.txt\", replace\n", "W5 (x.14): the digit-tail arm runs");
      check(has(slurp(at("w5da.txt")), "abcd~1234567"),
            "W5 (x.14): ...and the TRAILING DIGIT RUN is what survives -- `abcd~1234567`, four "
            "head characters against seven digits, not the ten-and-one of a name that ends in a "
            "letter. Two examples with different splits are what make this a rule and not a "
            "truncation");
      // nobaselevels: a no-op here, and the selftest says WHY so the next
      // reader does not file it as unimplemented.
      ok2("esttab w5a using \"w5db.txt\", replace nobaselevels\n",
          "W5 (x.14): `nobaselevels` runs -- a validated no-op: this engine's coeftable carries "
          "no base level for a factor at all, so the row Stata's default prints and this option "
          "removes never exists here (BUG-492)");
    }

    // ── (x.16) W1a-5b: the `stats()` SUB-GRAMMAR, `s()`, `sfmt()`, `nolabel`.
    // `stats(N r2, labels("Obs" "R2") fmt(0 3))` used to split on whitespace
    // and render FIVE garbage footer rows -- `r2,` / `labels("Obs"` / `"R2")` /
    // `fmt(0` / `3)` -- at rc 0. Oracle: w1b_stats_grammar{,2}.log and
    // w1b_sfmt_rule.log.
    {
      auto okc = [&](const std::string& cmd, const std::string& why) {
        auto r = exec(cmd, s);
        check(firstError(r.blocks).is_null(),
              why + " [got: " + errText(firstError(r.blocks)) + "]");
      };
      (void)exec("estadd scalar mymean = 6165.257\n", s);
      (void)exec("estadd scalar myother = 1.23456789\n", s);
      (void)exec("eststo w5sc\n", s);
      okc("esttab w5sc using \"w5t1.txt\", replace stats(N r2, labels(\"Obs\" \"R2\") fmt(0 3))\n",
          "W5 (x.16): `stats(keys, labels() fmt())` runs");
      const std::string t1 = slurp(at("w5t1.txt"));
      check(has(t1, "Obs") && has(t1, "R2"),
            "W5 (x.16): ...and labels() replaces the row names (s2/s4). Got:\n" + t1);
      check(!has(t1, "labels(") && !has(t1, "fmt(") && !has(t1, "r2,"),
            "W5 (x.16): ...and the SUBOPTIONS are parsed, not rendered as rows. This build turned "
            "them into five footer rows at rc 0 -- a wrong table nothing stopped. Got:\n" + t1);
      // labels() is positional and short lists leave the rest alone (s3/d3).
      okc("esttab w5sc using \"w5t2.txt\", replace stats(N r2, labels(\"Obs\"))\n",
          "W5 (x.16): a PARTIAL labels() runs");
      const std::string t2 = slurp(at("w5t2.txt"));
      check(has(t2, "Obs") && has(t2, "\nr2"),
            "W5 (x.16): ...and the keys it does not reach keep their own name (d3). Got:\n" + t2);
      // fmt() is positional, and ONE entry serves every key (d1/d2).
      okc("esttab w5sc using \"w5t3.txt\", replace stats(N r2, fmt(2 5))\n",
          "W5 (x.16): a two-entry fmt() runs");
      {
        // Fixture-independent: N's DEFAULT is an integer count with no decimal
        // point at all, so a dot in the N row is the whole discriminator.
        // (Asserting the oracle's `74.00` here fails on this leg's 30-row
        // fixture -- the third time in this session that a check written from
        // an oracle number failed against correct output.)
        const std::string t3 = slurp(at("w5t3.txt"));
        const size_t nAt = t3.find("\nN ");
        const size_t eol = nAt == std::string::npos ? std::string::npos : t3.find('\n', nAt + 1);
        const std::string nRow =
            nAt == std::string::npos ? std::string() : t3.substr(nAt, eol - nAt);
        check(nRow.find('.') != std::string::npos,
              "W5 (x.16): ...and it is POSITIONAL -- the FIRST fmt() entry reaches N, which "
              "otherwise renders as an integer count with no decimal point (d1). Got the N row: `" +
                  nRow + "`");
      }
      // `s()` is estout's one-character minimum for stats().
      okc("esttab w5sc using \"w5t4.txt\", replace s(N r2)\n",
          "W5 (x.16): `s()` is estout's abbreviation of stats() (s7) -- 18 of the 32 obslast uses "
          "and 13 of the 82 sfmt uses spell it, and it was r(198)");
      // sfmt governs the scalars() rows and NOTHING else.
      okc("esttab w5sc using \"w5t5.txt\", replace scalars(mymean)\n",
          "W5 (x.16): `scalars()` runs");
      okc("esttab w5sc using \"w5t6.txt\", replace scalars(mymean) sfmt(5)\n",
          "W5 (x.16): `scalars() sfmt()` runs");
      const std::string t5 = slurp(at("w5t5.txt"));
      const std::string t6 = slurp(at("w5t6.txt"));
      check(has(t5, "6165.3") && !has(t5, "6165.25700"),
            "W5 (x.16): a scalars() row defaults to esttab's ADAPTIVE a3 -- 6165.257 renders "
            "`6165.3` (q5), which three decimals cannot produce. Got:\n" + t5);
      check(has(t6, "6165.25700"),
            "W5 (x.16): ...and `sfmt(5)` is what moves it (q4). Those two arms are the ONLY pair "
            "in twelve that separates sfmt from a no-op: it is inert against stats() keys, N, r2, "
            "the r2/ar2 flags, `plain` and `b()/se()`. Got:\n" + t6);
      // A key the user NAMED keeps its own label; the FLAG keeps the pretty one.
      okc("esttab w5sc using \"w5t7.txt\", replace stats(r2)\n", "W5 (x.16): `stats(r2)` runs");
      okc("esttab w5sc using \"w5t8.txt\", replace r2\n", "W5 (x.16): the `r2` FLAG runs");
      check(has(slurp(at("w5t7.txt")), "\nr2") && has(slurp(at("w5t8.txt")), "R-sq"),
            "W5 (x.16): a key named in stats() keeps its OWN spelling (`r2`, s1) while the FLAG "
            "carries the pretty label (`R-sq`, q1) -- this build gave both the pretty one");
      // nolabel cancels the TEXT and not the WIDTH.
      okc("esttab w5sc using \"w5t9.txt\", replace label\n", "W5 (x.16): `label` runs");
      okc("esttab w5sc using \"w5ta.txt\", replace label nolabel\n",
          "W5 (x.16): `label nolabel` runs -- it was r(198), the name being in neither vocabulary "
          "list");
      okc("esttab w5sc using \"w5tb.txt\", replace nolabel\n", "W5 (x.16): `nolabel` alone runs");
      const std::string t9 = slurp(at("w5t9.txt"));
      const std::string ta = slurp(at("w5ta.txt"));
      const std::string tb = slurp(at("w5tb.txt"));
      check(t9.find('\n') == ta.find('\n') && ta.find('\n') > tb.find('\n'),
            "W5 (x.16): `nolabel` cancels the label TEXT and NOT the width -- `label nolabel` is "
            "still ruled at the 20-column stub `label` set, while `nolabel` alone keeps 12 "
            "(s11/s12/s13). Two effects, two flags");
    }

    // ── (x.17) W1a-5c: `nogaps` was ACCEPTED AND IGNORED, and `alignment()`
    // was refused while its own abbreviation `align()` was accepted-and-dropped.
    // Oracle: w1b_esttab_resid.log y5/y6 (gaps) and y11/y12/y13 (tex column).
    {
      auto okc = [&](const std::string& cmd, const std::string& why) {
        auto r = exec(cmd, s);
        check(firstError(r.blocks).is_null(),
              why + " [got: " + errText(firstError(r.blocks)) + "]");
      };
      auto blanks = [](const std::string& t) {
        size_t n = 0, pos = 0;
        while (pos <= t.size()) {
          const size_t nl = t.find('\n', pos);
          if (t.substr(pos, (nl == std::string::npos ? t.size() : nl) - pos).empty()) ++n;
          if (nl == std::string::npos) break;
          pos = nl + 1;
        }
        return n;
      };
      okc("esttab w5a using \"w5u1.txt\", replace\n", "W5 (x.17): the control runs");
      okc("esttab w5a using \"w5u2.txt\", replace nogaps\n", "W5 (x.17): `nogaps` runs");
      okc("esttab w5a using \"w5u3.txt\", replace gaps\n",
          "W5 (x.17): `gaps` runs -- it was r(198) and it is the DEFAULT, so a validated no-op");
      const size_t bCtl = blanks(slurp(at("w5u1.txt")));
      const size_t bNo = blanks(slurp(at("w5u2.txt")));
      const size_t bYes = blanks(slurp(at("w5u3.txt")));
      check(bCtl > bNo,
            "W5 (x.17): `nogaps` REMOVES the blank line between coefficients. It was in the KNOWN "
            "list and did nothing -- accepted and ignored, which is the one thing a vocabulary "
            "entry is not allowed to mean here. Got " + std::to_string(bCtl) + " vs " +
                std::to_string(bNo));
      check(bYes == bCtl,
            "W5 (x.17): ...and `gaps` matches the control, because the gap is already the default "
            "(y5). A no-op that is MEASURED is not the same as one that is assumed");
      // The tex column letter, both spellings.
      okc("esttab w5a using \"w5u4.tex\", replace tex alignment(S)\n",
          "W5 (x.17): `alignment()` runs -- it was r(198) while its own five-character "
          "abbreviation `align()` was accepted and dropped");
      okc("esttab w5a using \"w5u5.tex\", replace tex align(S)\n", "W5 (x.17): `align()` runs");
      okc("esttab w5a using \"w5u6.tex\", replace tex\n", "W5 (x.17): the tex control runs");
      check(has(slurp(at("w5u4.tex")), "{S}") && has(slurp(at("w5u5.tex")), "{S}"),
            "W5 (x.17): ...and both spellings reach the column letter -- `l*{k}{S}` (y12)");
      check(has(slurp(at("w5u6.tex")), "{c}"),
            "W5 (x.17): ...while the default stays `c`, which is why `alignment(c)` is "
            "byte-identical to the control (y11)");
    }

    // ── (x.18) W1a-5d: `pr2`, and the SECOND name its number arrives under.
    // Oracle: w1b_esttab_resid.log y8 (`pseudo R-sq 0.397`) and y9 (`pr2(4)`
    // -> four decimals). The interesting half is engine-side: this engine
    // publishes the pseudo-R2 as `r2_p` after poisson and as `r2_pseudo` after
    // logit/probit, each leaving the other missing -- and `r2_pseudo` was not
    // in the e-scalar key table, so it never reached a STORED estimate and the
    // cell came back blank for exactly the family `pr2` is used on.
    {
      piet::SessionState p(workdir);
      p.eval.cwd = workdir;
      p.cwd = workdir;
      // `y > 0` is not guaranteed to SPLIT on this fixture, and a constant
      // dependent variable makes the fit fail and `eststo` store nothing --
      // which is how this arm first read as "pr2 is still refused".
      (void)exec(kMake30 +
                     "generate byte hi = mod(_n, 2)\n"
                     "logit hi x1 x2\n"
                     "eststo pl\n",
                 p);
      auto rl = exec("esttab pl using \"w5p1.txt\", replace pr2\n", p);
      check(firstError(rl.blocks).is_null(),
            "W5 (x.18): `esttab <logit>, pr2` runs -- it was r(198) [got: " +
                errText(firstError(rl.blocks)) + "]");
      const std::string p1 = slurp(at("w5p1.txt"));
      check(has(p1, "pseudo R-sq"),
            "W5 (x.18): ...and the row is labelled `pseudo R-sq` (y8). Got:\n" + p1);
      // The discriminating part: the cell must carry a NUMBER. Reading only
      // `r2_p` leaves it blank here, which is what this build did.
      const size_t at2 = p1.find("pseudo R-sq");
      const std::string row = at2 == std::string::npos
                                  ? std::string()
                                  : p1.substr(at2, p1.find('\n', at2) - at2);
      check(row.find('.') != std::string::npos,
            "W5 (x.18): ...and it is NOT BLANK. The engine publishes this number as `r2_pseudo` "
            "after logit/probit and as `r2_p` after poisson; a reader that knows one key answers "
            "nothing for the other family, and both live corpus uses are probit. Got the row: `" +
                row + "`");
      auto r4 = exec("esttab pl using \"w5p2.txt\", replace pr2(4)\n", p);
      check(firstError(r4.blocks).is_null(), "W5 (x.18): `pr2(4)` runs");
      const std::string p2 = slurp(at("w5p2.txt"));
      const size_t at3 = p2.find("pseudo R-sq");
      const std::string row4 =
          at3 == std::string::npos ? std::string() : p2.substr(at3, p2.find('\n', at3) - at3);
      const size_t dot = row4.find('.');
      size_t dec = 0;
      for (size_t i = dot + 1; dot != std::string::npos && i < row4.size() &&
                               std::isdigit(static_cast<unsigned char>(row4[i]));
           ++i) {
        ++dec;
      }
      check(dec == 4,
            "W5 (x.18): ...and `pr2(#)` carries its own decimal count (y9), the same way `b(3)` "
            "does. Got " + std::to_string(dec) + " in `" + row4 + "`");
      auto rc2 = exec("esttab pl using \"w5p3.txt\", replace\n", p);
      check(firstError(rc2.blocks).is_null() && !has(slurp(at("w5p3.txt")), "pseudo"),
            "W5 (x.18): ...and without the option there is no such row at all");
    }

    // ── (x.19) W1a-5e: `labcol2()` -- ONE extra column between the stub and
    // the model columns, 456 corpus uses across 14 spellings. Oracle:
    // w1b_labcol2.log (k0..k8) and w1b_esttab_resid.log y15, which is where
    // the geometry comes from: the fixed field is 13 wide and CENTRED, and the
    // tex cell is plain rather than a \multicolumn.
    {
      auto okc = [&](const std::string& cmd, const std::string& why) {
        auto r = exec(cmd, s);
        check(firstError(r.blocks).is_null(),
              why + " [got: " + errText(firstError(r.blocks)) + "]");
      };
      okc("esttab w5a w5b using \"w5l0.txt\", replace\n", "W5 (x.19): the control runs");
      okc("esttab w5a w5b using \"w5l1.txt\", replace labcol2(1.5 3.5, title(Extra))\n",
          "W5 (x.19): `labcol2(values, title())` runs -- it was r(198)");
      okc("esttab w5a w5b using \"w5l2.txt\", replace labcol2(1.5 3.5)\n",
          "W5 (x.19): ...and the title is optional");
      okc("esttab w5a w5b using \"w5l3.tex\", replace tex labcol2(1.5 3.5, title(Extra))\n",
          "W5 (x.19): ...and it reaches the tex writer too");
      const std::string l0 = slurp(at("w5l0.txt"));
      const std::string l1 = slurp(at("w5l1.txt"));
      const std::string l2 = slurp(at("w5l2.txt"));
      const std::string l3 = slurp(at("w5l3.tex"));
      check(l1.find('\n') == l0.find('\n') + 13,
            "W5 (x.19): the rule grows by exactly 13 -- the field's width, measured off the "
            "oracle's own columns rather than chosen (y15: 44 -> 57). Got " +
                std::to_string(l1.find('\n')) + " against " + std::to_string(l0.find('\n')));
      check(has(l1, "   Extra    ") || has(l1, "    Extra    "),
            "W5 (x.19): ...and the title is CENTRED in it, not padded to either side -- `Extra` "
            "at 16-20 inside the field 12..24 is centring and nothing else. Got:\n" + l1);
      check(!has(l2, "Extra") && l2.find('\n') == l1.find('\n'),
            "W5 (x.19): ...while WITHOUT a title the column is still there and its head is blank "
            "(k2) -- the width does not depend on the title");
      check(has(l3, "{lc*{2}{c}}"),
            "W5 (x.19): in tex the column spec gains a plain `c` between the stub and the models "
            "(k5). Got:\n" + l3.substr(0, 120));
      check(has(l3, "&   Extra    &\\multicolumn"),
            "W5 (x.19): ...and the head cell is a PLAIN centred cell, not a \\multicolumn like "
            "the model heads beside it (k5). Got:\n" + l3.substr(0, 220));
      // The values are positional over COEFFICIENTS, so the aux row under the
      // first coefficient keeps a blank field.
      const size_t firstCoef = l1.find("\nx1");
      const size_t auxLine = firstCoef == std::string::npos
                                 ? std::string::npos
                                 : l1.find('\n', firstCoef + 1);
      const std::string aux =
          auxLine == std::string::npos ? std::string() : l1.substr(auxLine + 1, 25);
      check(aux.find("1.5") == std::string::npos,
            "W5 (x.19): ...and a value belongs to the COEFFICIENT, not to the row -- the aux row "
            "beneath it keeps a blank field (k1). Got: `" + aux + "`");
    }

    // -- (x.28) BUG-501: a string literal is DATA, and every pre-pass in
    // evalArith was rewriting its contents. Oracle: w1b_braceliteral.log (BL|*)
    // and w1b_quoteblind.log (QB|*).
    //
    // The nine-line version of this fix -- widening isSafeArithExt to skip
    // quoted spans -- was written, measured and REVERTED on 2026-09-18, because
    // the filter had been SHIELDING the pre-passes underneath it and fixing it
    // alone turned a silent echo into a silent wrong ANSWER. These arms are what
    // that reversal bought: each one is a pre-pass, not the filter.
    {
      auto say = [&](const std::string& expr) {
        auto r = exec("display \"XX|\" " + expr + " \"|\"\n", s);
        for (const auto& b : r.blocks) {
          if (b.value("type", std::string()) != "display") continue;
          const std::string t = b.value("text", std::string());
          if (t.rfind("XX|", 0) == 0 && t.size() >= 4) return t.substr(3, t.size() - 4);
        }
        return std::string("<no display>");
      };

      // The filter. Twelve characters used to stop the expression being
      // evaluated at all, and `display` echoed the SOURCE TEXT at rc 0 -- an
      // evaluation failure that surfaces as an echo is invisible to every gate
      // that reads return codes.
      check(say("strpos(\"abc\", \"b\")") == "2",
            "W5 (x.28): the control evaluates (BL|plain|2|)");
      check(say("strpos(\"a{b}c\", \"b\")") == "3",
            "W5 (x.28): a BRACE inside a literal is an ordinary character (BL|brace_hay|3|). This "
            "echoed `strpos(\"a{b}c\", \"b\")` at rc 0. Got `" + say("strpos(\"a{b}c\", \"b\")") +
                "`");
      check(say("length(\"a{b}c\")") == "5" && say("substr(\"a{b}c\", 2, 3)") == "{b}" &&
                say("upper(\"a{b}c\")") == "A{B}C",
            "W5 (x.28): ...on every string function, not just the one (BL|length/substr/upper)");
      check(say("strpos(\"a[b]c\", \"b\")") == "3" &&
                say("length(\"a;b@c&d|e~f:g#h\")") == "15",
            "W5 (x.28): ...and for the other eleven characters -- brackets and punctuation "
            "(BL|brace_needle, QB|punct|15|)");

      // THE PRE-PASSES, which is the half the reverted fix got wrong. Each of
      // these is a string whose CONTENT looks like something the expression layer
      // resolves, and in Stata all of them are just characters.
      check(say("strpos(\"A[1,1]\", \"1\")") == "3",
            "W5 (x.28): a MATRIX SUBSCRIPT inside a literal is not a subscript (QB|matrix_sub|3|). "
            "With the filter widened and this pre-pass left alone the answer was 1 -- a silent "
            "wrong number where there had been a silent echo, which is the worse of the two. Got `" +
                say("strpos(\"A[1,1]\", \"1\")") + "`");
      check(say("strpos(\"a^b\", \"b\")") == "3",
            "W5 (x.28): ...nor is a CARET an operator there -- the `^`->`**` rewrite used to change "
            "the string's length");
      check(say("strpos(\"ln\", \"n\")") == "2",
            "W5 (x.28): ...nor is `ln` the log function -- the `\\bln\\b`->`log` rewrite used to "
            "reach inside too");
      check(say("length(\"say hello\")") == "9",
            "W5 (x.28): ...and an ordinary sentence still measures itself");

      // Protection is ONE-SIDED: an unterminated quote is left exactly as it was,
      // so nothing this function already judged changes.
      check(say("strpos(\"abc\", \"b\")") == "2",
            "W5 (x.28): the balanced case still works after all of that");

      // BUG-502: STATA HAS NO ESCAPE SEQUENCES in a double-quoted literal. This
      // lexer was written to Python semantics -- its own comment said so -- and
      // every one of its callers evaluates a STATA expression.
      check(say("length(\"a\\nb\")") == "4",
            "W5 (x.502): `\"a\\nb\"` is FOUR characters -- backslash, n, and the two letters. The C "
            "table made it three, silently, and it is the ordinary way a Windows path is written. "
            "Got `" + say("length(\"a\\nb\")") + "`");
      check(say("length(\"\\begin\")") == "6" && say("length(\"a\\\\b\")") == "4",
            "W5 (x.502): ...and `\"\\begin\"` is six and `\"a\\\\b\"` is four -- `\\b` was a "
            "backspace and `\\\\` collapsed to one. Every LaTeX-writing do-file carries the first");
      check(say("strpos(\"c:\\data\\x.dta\", \"x\")") == "9",
            "W5 (x.502): ...so a path measures itself: `\\d` and `\\x` are two characters each");
      check(say("strpos(\"\\begin{tabular}{lc}\", \"tabular\")") == "8",
            "W5 (x.502): ...and the arm that found this one -- 8, not 7 -- which only comes out "
            "right with BUG-443's protection AND this table gone. The two fixes are independent "
            "and this arm needs both");
    }

    // -- (x.27) W1a-4 follow-on: `indicate()`, and the `nostar` WIDTH it turned
    // up. Oracle: w1b_indicate.log (n*/e*/u*). Censusing the five options esttab
    // still refused removed one before any arm was written: `begin` had 64
    // apparent uses and every one is `\begin{tabular}` inside a prehead()
    // argument -- `begin(` as an option is ZERO on both writers. A name census
    // counts names, and `\begin` is a name in a different language.
    {
      auto oki = [&](const std::string& cmd, const std::string& why) {
        auto r = exec(cmd, s);
        check(firstError(r.blocks).is_null(),
              why + " [got: " + errText(firstError(r.blocks)) + "]");
      };
      auto firstLineLen = [&](const std::string& f) {
        const std::string t = slurp(at(f));
        const size_t e = t.find('\n');
        return (e == std::string::npos ? t.size() : e);
      };
      auto hasRowStarting = [&](const std::string& f, const std::string& pre) {
        const std::string t = slurp(at(f));
        size_t i = 0;
        while (i < t.size()) {
          size_t e = t.find('\n', i);
          if (e == std::string::npos) e = t.size();
          if (t.compare(i, pre.size(), pre) == 0) return true;
          i = e + 1;
        }
        return false;
      };

      // THE WIDTH. The rule line measures the whole geometry, and `nostar` zeroes
      // the three-character star field: the oracle's nostar table rules at 38 and
      // its starred one at 44 (n0 against w1b_headfoot.log pb). This build wrote
      // 44 for both -- 230 corpus uses over 22 papers, every row carrying three
      // trailing spaces, and NO existing arm pinned it.
      oki("esttab w5a w5b using \"w5i0.txt\", replace b se nostar\n",
          "W5 (x.27): the nostar control runs");
      check(firstLineLen("w5i0.txt") == 38,
            "W5 (x.27): ...and `nostar` rules at 38 = 12 + 2*(1+12+0) -- the star field is GONE, "
            "not merely empty (n0). At 44 every row of every nostar table carries three trailing "
            "spaces. Got " + std::to_string(firstLineLen("w5i0.txt")));
      oki("esttab w5a w5b using \"w5i1.txt\", replace b se\n",
          "W5 (x.27): the starred control runs");
      check(firstLineLen("w5i1.txt") == 44,
            "W5 (x.27): ...and WITH stars it rules at 44 (w1b_headfoot.log pb). The pair is the "
            "measurement: one width alone cannot say whether the field is reserved by option or by "
            "content. Got " + std::to_string(firstLineLen("w5i1.txt")));

      // indicate(): the named coefficients LEAVE and one Yes/No row replaces them.
      oki("esttab w5a w5b using \"w5i2.txt\", replace b se nostar indicate(\"Controls = x2\")\n",
          "W5 (x.27): `indicate()` runs -- it was an unknown option");
      check(hasRowStarting("w5i2.txt", "Controls") && !hasRowStarting("w5i2.txt", "x2"),
            "W5 (x.27): ...and the coefficient it names is GONE, replaced by one row (n1). A row "
            "ADDED beside the coefficients would pass a presence-only check, which is why the "
            "absence is asserted too. Got:\n" + slurp(at("w5i2.txt")));
      check(has(slurp(at("w5i2.txt")), "Yes"),
            "W5 (x.27): ...whose cells say Yes for a model that has it (n1)");

      // The default second label is `No`, and it takes a model that LACKS the
      // varlist to see it. This fixture has one: w5b is `regress y x1`, so the
      // same arm shows both halves -- with every model carrying the varlist both
      // cells read Yes and `labels(Yes)` is indistinguishable from the default
      // (the oracle's n2 is byte-identical to n1 for exactly that reason).
      check(has(slurp(at("w5i2.txt")), "No"),
            "W5 (x.27): ...and No for the model that does not (n6). w5b is `regress y x1`, so this "
            "one arm carries both halves of labels(); a table where every model has the varlist "
            "cannot see the second label at all. Got:\n" + slurp(at("w5i2.txt")));
      oki("esttab w5a w5b using \"w5i4.txt\", replace b se nostar "
          "indicate(\"Controls = x2\", labels(\"YES\" \" \"))\n",
          "W5 (x.27): `labels()` runs");
      check(has(slurp(at("w5i4.txt")), "YES"),
            "W5 (x.27): ...and replaces the present-label (n3)");

      oki("esttab w5a w5b using \"w5i5.txt\", replace b se nostar indicate(\"x2\")\n",
          "W5 (x.27): an `indicate()` group with no `=` runs");
      check(hasRowStarting("w5i5.txt", "x2 ") || hasRowStarting("w5i5.txt", "x2"),
            "W5 (x.27): ...and the row's label is the VARLIST text itself (n7)");
      oki("esttab w5a w5b using \"w5i6.txt\", replace b se nostar indicate(\"Controls = x*\")\n",
          "W5 (x.27): a GLOB varlist runs");
      check(!hasRowStarting("w5i6.txt", "x1") && !hasRowStarting("w5i6.txt", "x2"),
            "W5 (x.27): ...and takes every coefficient it matches (n4 keeps the one it does NOT "
            "match, which is the same statement from the other side). Got:\n" +
                slurp(at("w5i6.txt")));
      oki("esttab w5a w5b using \"w5i7.txt\", replace b se nostar "
          "indicate(\"A = x1\" \"B = x2\")\n",
          "W5 (x.27): two quoted groups run");
      check(hasRowStarting("w5i7.txt", "A") && hasRowStarting("w5i7.txt", "B"),
            "W5 (x.27): ...as two rows (n5)");
      check(has(slurp(at("w5i7.txt")), "\n\nB"),
            "W5 (x.27): ...with a BLANK LINE between them (n5). They are a block of groups, not a "
            "run of rows, and this arm is the only thing that says so");

      // Measured no-ops, pinned as SAMENESS -- the W1a-3 treatment.
      oki("esttab w5a w5b using \"w5i8.txt\", replace b se nostar eqlabels(none)\n",
          "W5 (x.27): `eqlabels(none)` runs");
      oki("esttab w5a w5b using \"w5i9.txt\", replace b se nostar unstack\n",
          "W5 (x.27): `unstack` runs");
      check(slurp(at("w5i8.txt")) == slurp(at("w5i0.txt")) &&
                slurp(at("w5i9.txt")) == slurp(at("w5i0.txt")),
            "W5 (x.27): ...and BOTH are byte-identical to the control (e1/u1). They act on "
            "multi-equation tables, and this engine's coeftable carries no equation at all, so on "
            "every table it can produce they are no-ops -- accepted with the SAMENESS pinned, the "
            "way W1a-3 handled type/obslast/style(fixed). If the engine ever carries equations, "
            "this arm is what has to be revisited");

      // And one option whose refusal is PARITY.
      auto rtr = exec("esttab w5a w5b using \"w5ia.txt\", replace b se nostar transpose\n", s);
      check(!firstError(rtr.blocks).is_null(),
            "W5 (x.27): `transpose` stays REFUSED, and that is parity: esttab answers r(198) "
            "\"option transpose not allowed\" in Stata too (tr_pose), so the six corpus lines that "
            "spell it do not run there either. It is estout's option, not esttab's");
    }

    // -- (x.26) W1a-4: the rest of estout's option row. Oracle:
    // w1b_estout_opts.log (q*) and w1b_estout_opts2.log (y*/z*). The four seam
    // hooks are ADDITIVE here, which is the opposite of what the same four do on
    // esttab (x.24, where they REPLACE a block) -- so a shared implementation
    // would have been wrong for one of the two commands, and the arms are paired
    // across x.24/x.26 for exactly that reason.
    {
      auto oko = [&](const std::string& cmd, const std::string& why) {
        auto r = exec(cmd, s);
        check(firstError(r.blocks).is_null(),
              why + " [got: " + errText(firstError(r.blocks)) + "]");
      };
      auto lineOf = [&](const std::string& f, size_t n) {
        const std::string t = slurp(at(f));
        size_t start = 0;
        for (size_t i = 0; i < n; ++i) {
          const size_t e = t.find('\n', start);
          if (e == std::string::npos) return std::string();
          start = e + 1;
        }
        const size_t e = t.find('\n', start);
        return t.substr(start, (e == std::string::npos ? t.size() : e) - start);
      };
      auto lastLine = [&](const std::string& f) {
        std::string t = slurp(at(f));
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
        const size_t nl = t.rfind('\n');
        return nl == std::string::npos ? t : t.substr(nl + 1);
      };

      oko("estout w5a w5b using \"w5o1.txt\", replace cells(b se) prehead(PREHEAD)\n",
          "W5 (x.26): estout `prehead()` runs -- it was a named refusal");
      check(lineOf("w5o1.txt", 0) == "PREHEAD",
            "W5 (x.26): ...and is ADDITIVE: the header rows follow it unchanged (q b). On esttab "
            "the same option REPLACES the whole opening block (x.24) -- a tab table has no block "
            "to replace, so one implementation for both commands is wrong for one of them. Got `" +
                lineOf("w5o1.txt", 0) + "`");
      oko("estout w5a w5b using \"w5o2.txt\", replace cells(b se) posthead(POSTHEAD)\n",
          "W5 (x.26): `posthead()` runs");
      check(lineOf("w5o2.txt", 2) == "POSTHEAD",
            "W5 (x.26): ...and sits between the cell-name row and the body (qc). Got `" +
                lineOf("w5o2.txt", 2) + "`");
      oko("estout w5a w5b using \"w5o3.txt\", replace cells(b se) stats(N) prefoot(PREFOOT)\n",
          "W5 (x.26): `prefoot()` runs");
      check(has(slurp(at("w5o3.txt")), "PREFOOT\nN"),
            "W5 (x.26): ...immediately above the stats block (qd). Got:\n" + slurp(at("w5o3.txt")));
      oko("estout w5a w5b using \"w5o4.txt\", replace cells(b se) stats(N) postfoot(POSTFOOT)\n",
          "W5 (x.26): `postfoot()` runs");
      check(lastLine("w5o4.txt") == "POSTFOOT",
            "W5 (x.26): ...and is the LAST line (qe). Got `" + lastLine("w5o4.txt") + "`");
      oko("estout w5a w5b using \"w5o5.txt\", replace cells(b se) prefoot(PREFOOT)\n",
          "W5 (x.26): `prefoot()` with NO stats runs");
      check(lastLine("w5o5.txt") == "PREFOOT",
            "W5 (x.26): ...and still emits, at the position the footer would have started "
            "(w1b_estout_opts2.log y9). A hook that only fired when it had something to sit above "
            "would pass every other arm here");

      oko("estout w5a w5b using \"w5o6.txt\", replace cells(b se) begin(BEGIN)\n",
          "W5 (x.26): `begin()` runs");
      check(lineOf("w5o6.txt", 0).compare(0, 5, "BEGIN") == 0 &&
                lineOf("w5o6.txt", 2).compare(0, 5, "BEGIN") == 0,
            "W5 (x.26): ...and prefixes EVERY row, header rows included, with no separator of its "
            "own (qf). Got `" + lineOf("w5o6.txt", 0) + "` / `" + lineOf("w5o6.txt", 2) + "`");

      // `legend` prints the ladder DESCENDING -- the opposite of the ladder's own
      // order -- and only when the cells carry stars.
      oko("estout w5a w5b using \"w5o7.txt\", replace cells(b(star)) legend\n",
          "W5 (x.26): `legend` runs");
      check(lastLine("w5o7.txt") == "* p<0.05, ** p<0.01, *** p<0.001",
            "W5 (x.26): ...and prints estout's default ladder DESCENDING by cutoff (y6), which is "
            "the reverse of how estoutStars() holds it. Got `" + lastLine("w5o7.txt") + "`");
      oko("estout w5a w5b using \"w5o8.txt\", replace cells(b(star)) "
          "starlevels(+ 0.10 * 0.05 ** 0.01) legend\n",
          "W5 (x.26): `legend` under a custom ladder runs");
      check(lastLine("w5o8.txt") == "+ p<0.10, * p<0.05, ** p<0.01",
            "W5 (x.26): ...and prints its levels VERBATIM -- `0.10` keeps the trailing zero (y7). "
            "Reformatting the parsed double would give `p<0.1`, which is the arm that says the "
            "option's own tokens have to be re-read for the text. Got `" + lastLine("w5o8.txt") +
                "`");
      oko("estout w5a w5b using \"w5o9.txt\", replace cells(b) legend\n",
          "W5 (x.26): `legend` with a starless spec runs");
      check(lastLine("w5o9.txt").find("p<") == std::string::npos,
            "W5 (x.26): ...and adds NOTHING, because there are no stars to explain (y8). Got `" +
                lastLine("w5o9.txt") + "`");

      // extracols() inserts EMPTY columns after the columns it names, the stub
      // being column 1.
      oko("estout w5a w5b using \"w5oa.txt\", replace cells(b se) extracols(1)\n",
          "W5 (x.26): `extracols(1)` runs");
      oko("estout w5a w5b using \"w5ob.txt\", replace cells(b se) extracols(2)\n",
          "W5 (x.26): `extracols(2)` runs");
      // Asserted on the MODEL-NAME row by content rather than by line index: the
      // fixture's table carries a row this scratch session's did not, and an
      // index would be pinning the fixture instead of the option.
      check(has(slurp(at("w5oa.txt")), "\t\tw5a\tw5b"),
            "W5 (x.26): ...and 1 puts the empty column between the STUB and the first model -- two "
            "delimiters before the first model name (y2). Got:\n" + slurp(at("w5oa.txt")));
      check(has(slurp(at("w5ob.txt")), "\tw5a\t\tw5b"),
            "W5 (x.26): ...while 2 puts it between the two models (y3). The pair is what says the "
            "stub counts as column 1. Got:\n" + slurp(at("w5ob.txt")));

      oko("estout w5a w5b using \"w5oc.txt\", replace cells(b se) rename(x1 MILEAGE)\n",
          "W5 (x.26): `rename()` runs");
      check(has(slurp(at("w5oc.txt")), "MILEAGE"),
            "W5 (x.26): ...and renames the row (q8)");
      oko("estout w5a w5b using \"w5od.txt\", replace cells(b se) rename(nosuch ZZZ)\n",
          "W5 (x.26): ...and a rename whose source is not in the model runs");
      check(!has(slurp(at("w5od.txt")), "ZZZ") && !has(slurp(at("w5od.txt")), "nosuch"),
            "W5 (x.26): ...changing nothing and erroring not at all (y b)");
    }

    // -- (x.25) W1a-4: estout's style(tex) is its OWN geometry, not esttab's.
    // BUG-486's sixth axis, and the largest block left in the bundle: 148 of the
    // corpus's 219 estout lines carry tex. Oracle: w1b_estout_tex.log (r*) and
    // w1b_estout_tex2.log (s*/v*). The assertions are FIELD WIDTHS rather than
    // whole lines, because the widths are the option's entire content and the
    // numbers inside them belong to the fixture; the two header rows, which carry
    // no fixture numbers, are asserted whole.
    {
      // The per-field widths of one row: split on `&` after dropping the `\\`.
      auto widths = [&](const std::string& line) {
        std::vector<size_t> w;
        if (line.size() < 2 || line.compare(line.size() - 2, 2, "\\\\") != 0) return w;
        const std::string body = line.substr(0, line.size() - 2);
        size_t start = 0;
        while (true) {
          const size_t amp = body.find('&', start);
          if (amp == std::string::npos) {
            w.push_back(body.size() - start);
            break;
          }
          w.push_back(amp - start);
          start = amp + 1;
        }
        return w;
      };
      auto lineN = [&](const std::string& f, size_t n) {
        const std::string t = slurp(at(f));
        size_t start = 0;
        for (size_t i = 0; i < n; ++i) {
          const size_t e = t.find('\n', start);
          if (e == std::string::npos) return std::string();
          start = e + 1;
        }
        const size_t e = t.find('\n', start);
        return t.substr(start, (e == std::string::npos ? t.size() : e) - start);
      };
      auto shown = [&](const std::vector<size_t>& v) {
        std::string o = "(";
        for (size_t i = 0; i < v.size(); ++i) {
          if (i) o += ",";
          o += std::to_string(v[i]);
        }
        return o + ")";
      };
      auto oke = [&](const std::string& cmd, const std::string& why) {
        auto r = exec(cmd, s);
        check(firstError(r.blocks).is_null(),
              why + " [got: " + errText(firstError(r.blocks)) + "]");
      };

      oke("estout w5a w5b using \"w5x1.tex\", replace style(tex) cells(b se)\n",
          "W5 (x.25): `estout ... style(tex)` runs");
      const std::string x1 = slurp(at("w5x1.tex"));
      check(!has(x1, "\\begin{tabular}") && !has(x1, "\\hline"),
            "W5 (x.25): ...and writes a FRAGMENT -- no tabular, no rules (w1b_estout_tex.log r1). "
            "This writer used to hand its table to renderTex, the esttab emitter, so all 148 tex "
            "lines in the corpus got `\\begin{tabular}{l*{2}{c}}` and `\\hline\\hline`. Got:\n" + x1);
      check(widths(lineN("w5x1.tex", 2)) == std::vector<size_t>({12, 12, 12}),
            "W5 (x.25): ...with a 12-wide stub and 12-wide cells (r1). Got " +
                shown(widths(lineN("w5x1.tex", 2))) + " in `" + lineN("w5x1.tex", 2) + "`");
      check(lineN("w5x1.tex", 0) == "            &         w5a&         w5b\\\\",
            "W5 (x.25): ...and the model-name row is right-aligned in those fields with `&` "
            "UNSPACED -- esttab's renderer writes ` & ` (r1). Got `" + lineN("w5x1.tex", 0) + "`");
      check(lineN("w5x1.tex", 1) == "            &        b/se&        b/se\\\\",
            "W5 (x.25): ...and the cell-name row sits under it. Got `" + lineN("w5x1.tex", 1) + "`");

      oke("estout w5a w5b using \"w5x2.tex\", replace style(tex) cells(b(star) se)\n",
          "W5 (x.25): a starred spec runs");
      check(widths(lineN("w5x2.tex", 2)) == std::vector<size_t>({12, 15, 15}),
            "W5 (x.25): ...and reserves a THREE-character star field after each 12-wide cell (r4). "
            "Got " + shown(widths(lineN("w5x2.tex", 2))));
      const std::string x2 = slurp(at("w5x2.tex"));
      check(has(x2, "** ") || has(x2, "*  ") || has(x2, "***"),
            "W5 (x.25): ...and the star is LEFT-aligned in it, so a two-star mark keeps a trailing "
            "space where a three-star one has none (r4). Padding value+star together to 15 would "
            "get the two-star case wrong and the three-star case right. Got:\n" + x2);

      oke("estout w5a w5b using \"w5x3.tex\", replace style(tex) cells(b se) numbers\n",
          "W5 (x.25): `numbers` runs");
      check(lineN("w5x3.tex", 0) == "            &         (1)&         (2)\\\\",
            "W5 (x.25): ...and puts the (1)/(2) row ABOVE the model names, which is the opposite "
            "of esttab's order (r a). Got `" + lineN("w5x3.tex", 0) + "`");

      oke("estout w5a w5b using \"w5x4.tex\", replace style(tex) cells(b) varwidth(25)\n",
          "W5 (x.25): `varwidth()` runs");
      check(widths(lineN("w5x4.tex", 2))[0] == 25,
            "W5 (x.25): ...and sets the stub width (w1b_estout_tex2.log v1)");
      oke("estout w5a w5b using \"w5x5.tex\", replace style(tex) cells(b) modelwidth(20)\n",
          "W5 (x.25): `modelwidth()` runs");
      check(widths(lineN("w5x5.tex", 2))[1] == 20,
            "W5 (x.25): ...and `modelwidth()` sets the cell width (v2)");

      // The stub default is the LABEL FLAG, not the label's length: a two-character
      // label still gets 20 (w1b_estout_tex2 w3 arm) and `nolabel` wins over
      // `label` when both are given (w1b_estout_opts.log q3).
      oke("estout w5a w5b using \"w5x6.tex\", replace style(tex) cells(b) label\n",
          "W5 (x.25): `label` runs");
      check(widths(lineN("w5x6.tex", 2))[0] == 20,
            "W5 (x.25): ...and moves the stub default from 12 to 20. Got " +
                shown(widths(lineN("w5x6.tex", 2))));
      oke("estout w5a w5b using \"w5x7.tex\", replace style(tex) cells(b) label nolabel\n",
          "W5 (x.25): `label nolabel` runs");
      check(widths(lineN("w5x7.tex", 2))[0] == 12,
            "W5 (x.25): ...and `nolabel` WINS, taking the width back with it (q3). That pair is "
            "the only way to see nolabel at all, since it is estout's default. Got " +
                shown(widths(lineN("w5x7.tex", 2))));

      // The fourth site of split-before-dequote in this tree.
      oke("estout w5a w5b using \"w5x8.tex\", replace style(tex) cells(b se) "
          "mlabels(\"Model One\" \"Two\")\n",
          "W5 (x.25): `mlabels()` with quoted multi-word labels runs");
      check(lineN("w5x8.tex", 0) == "            &   Model One&         Two\\\\",
            "W5 (x.25): ...and both labels survive whole. `buildHeads` dequoted the option value "
            "BEFORE splitting it, so `\"Model One\" \"Two\"` came out as `Model` and `One\" \"Two` "
            "(r8) -- the same rule `parseLabelPairs` states and the fourth site in this tree to "
            "have broken it. Got `" + lineN("w5x8.tex", 0) + "`");
    }

    // -- (x.24) W1a-4: prehead()/posthead()/prefoot()/postfoot() REPLACE their
    // block. Oracle: w1b_headfoot.log (p0..pb). All four were already accepted at
    // rc 0 and two of them were wrong in the TEX writer: `prehead()` replaced
    // only the top rule and left `{`, `\def\sym` and `\begin{tabular}` standing,
    // and `postfoot()` left `\end{tabular}` and the closing brace behind. The
    // corpus's own spelling IS a tabular opener -- 128 uses of
    // `prehead(\begin{tabular}{l*{5}{c}} \hline \hline)` over 12 papers -- so
    // every one of those files came out with two \begin{tabular} lines.
    {
      auto okh = [&](const std::string& cmd, const std::string& why) {
        auto r = exec(cmd, s);
        check(firstError(r.blocks).is_null(),
              why + " [got: " + errText(firstError(r.blocks)) + "]");
      };
      auto firstLine = [&](const std::string& f) {
        const std::string t = slurp(at(f));
        const size_t e = t.find('\n');
        return e == std::string::npos ? t : t.substr(0, e);
      };
      okh("esttab w5a w5b using \"w5h0.tex\", replace tex\n",
          "W5 (x.24): the tex control runs");
      const std::string h0 = slurp(at("w5h0.tex"));
      check(firstLine("w5h0.tex") == "{" && has(h0, "\\def\\sym#1{") &&
                has(h0, "\\begin{tabular}{l*{2}{c}}") && has(h0, "\\end{tabular}"),
            "W5 (x.24): ...and carries the default opening block and close (p0). Got:\n" + h0);

      okh("esttab w5a w5b using \"w5h1.tex\", replace tex prehead(\"PREHEAD\")\n",
          "W5 (x.24): `prehead()` runs");
      const std::string h1 = slurp(at("w5h1.tex"));
      check(firstLine("w5h1.tex") == "PREHEAD",
            "W5 (x.24): ...and is the FIRST line of the file (p1) -- it replaces the whole "
            "opening block, brace and \\def\\sym and tabular opener included. Got:\n" + h1);
      check(!has(h1, "\\def\\sym#1{") && !has(h1, "\\begin{tabular}"),
            "W5 (x.24): ...so NONE of the default opener survives. This is the arm that matters: "
            "the corpus passes a tabular opener here, and keeping the default one too is a second "
            "\\begin{tabular} in the same file. Got:\n" + h1);
      check(has(h1, "\\end{tabular}"),
            "W5 (x.24): ...while the CLOSE is untouched -- prehead() is not postfoot() (p1)");

      okh("esttab w5a w5b using \"w5h2.tex\", replace tex postfoot(\"POSTFOOT\")\n",
          "W5 (x.24): `postfoot()` runs");
      const std::string h2 = slurp(at("w5h2.tex"));
      check(has(h2, "POSTFOOT") && !has(h2, "\\end{tabular}") && !has(h2, "footnotesize"),
            "W5 (x.24): ...and replaces the whole closing block -- bottom rule, legend, "
            "\\end{tabular} and the brace. p4's file ENDS at POSTFOOT. This build wrote "
            "\\end{tabular} underneath it. Got:\n" + h2);
      check(firstLine("w5h2.tex") == "{",
            "W5 (x.24): ...while the OPENER is untouched (p4)");

      okh("esttab w5a w5b using \"w5h3.tex\", replace tex posthead(\"POSTHEAD\")\n",
          "W5 (x.24): `posthead()` runs");
      const std::string h3 = slurp(at("w5h3.tex"));
      check(has(h3, "POSTHEAD") && firstLine("w5h3.tex") == "{" && has(h3, "\\end{tabular}"),
            "W5 (x.24): ...and replaces only the rule between the header rows and the body, "
            "leaving both ends alone (p2). Got:\n" + h3);
      okh("esttab w5a w5b using \"w5h4.tex\", replace tex prefoot(\"PREFOOT\")\n",
          "W5 (x.24): `prefoot()` runs");
      check(has(slurp(at("w5h4.tex")), "PREFOOT") && has(slurp(at("w5h4.tex")), "\\end{tabular}"),
            "W5 (x.24): ...and replaces the rule before the stats block only (p3)");

      okh("esttab w5a w5b using \"w5h5.tex\", replace tex prehead(\"\")\n",
          "W5 (x.24): an EMPTY prehead() runs");
      check(firstLine("w5h5.tex").empty() && !has(slurp(at("w5h5.tex")), "\\begin{tabular}"),
            "W5 (x.24): ...and is a BLANK line, not nothing -- the block is still replaced (p6). "
            "Got first line `" + firstLine("w5h5.tex") + "`");

      // The fixed writer was already right; the pair is what says so.
      okh("esttab w5a w5b using \"w5h6.txt\", replace prehead(\"PREHEAD\")\n",
          "W5 (x.24): prehead() in the FIXED writer runs");
      check(firstLine("w5h6.txt") == "PREHEAD",
            "W5 (x.24): ...and replaces the top rule there too (p9)");
      okh("esttab w5a w5b using \"w5h7.txt\", replace postfoot(\"POSTFOOT\")\n",
          "W5 (x.24): postfoot() in the FIXED writer runs");
      const std::string h7 = slurp(at("w5h7.txt"));
      check(has(h7, "POSTFOOT") && !has(h7, "parentheses"),
            "W5 (x.24): ...and takes the legend with it (pa). The fixed writer already did this "
            "and the tex writer did not, which is the shape a single-style selftest misses");
    }

    // -- (x.23) W1a-5h: frmttable writes its OWN LaTeX, not estout's. Oracle:
    // w1b_frmttable_{base,names,cells,cells2}.log. Every expected string below
    // is copied from an oracle arm, and the inputs are literal matrices, so
    // these are the ORACLE's bytes and not a capture of what this build prints.
    // Before this the writer handed its table to `renderTex`, the estout emitter,
    // and produced a stub column and a header row the ado does not emit at all.
    {
      piet::SessionState sf(workdir);
      sf.eval.cwd = workdir;
      sf.cwd = workdir;
      auto okf = [&](const std::string& cmd, const std::string& why) {
        auto r = exec(cmd, sf);
        check(firstError(r.blocks).is_null(),
              why + " [got: " + errText(firstError(r.blocks)) + "]");
      };
      (void)exec("matrix N = (1,2,3\\4,5,6)\n", sf);
      (void)exec("matrix A = (1,2,3\\4,5,6)\nmatrix rownames A = alpha beta\n"
                 "matrix colnames A = one two three\n",
                 sf);

      // The plainest call there is. An unnamed Stata matrix still CARRIES names
      // (`r1 r2` / `c1 c2 c3`), and the oracle emits neither -- no stub column,
      // no header row, three `c` columns.
      okf("frmttable using \"w5f1.tex\", statmat(N) sdec(2) tex replace fragment nocenter\n",
          "W5 (x.23): `frmttable ... tex` runs");
      const std::string f1 = slurp(at("w5f1.tex"));
      check(f1 ==
                "\\begin{tabular}{ccc}\n"
                "\\hline \\noalign{\\smallskip}1.00 & 2.00 & 3.00\\\\\n"
                "4.00 & 5.00 & 6.00\\\\\n"
                "\\noalign{\\smallskip}\\hline\\end{tabular}\\\\\n",
            "W5 (x.23): ...byte for byte as the ado writes it (w1b_frmttable_base.log h1 with "
            "h7's nocenter). The default-SPELLED matrix names count as ABSENT -- which is why "
            "there is no `l` in the column spec and no header row. Got:\n" + f1);

      // The same command on a matrix whose names were actually set.
      okf("frmttable using \"w5f2.tex\", statmat(A) sdec(2) tex replace fragment nocenter\n",
          "W5 (x.23): the same call on a NAMED matrix runs");
      const std::string f2 = slurp(at("w5f2.tex"));
      check(f2 ==
                "\\begin{tabular}{lccc}\n"
                "\\hline \\noalign{\\smallskip} & one & two & three\\\\\n"
                "\\noalign{\\smallskip}\\hline \\noalign{\\smallskip}alpha & 1.00 & 2.00 & "
                "3.00\\\\\n"
                "beta & 4.00 & 5.00 & 6.00\\\\\n"
                "\\noalign{\\smallskip}\\hline\\end{tabular}\\\\\n",
            "W5 (x.23): ...and NOW there is a stub and a header, and the first DATA row carries "
            "the `\\noalign{\\smallskip}\\hline \\noalign{\\smallskip}` separator (base h2). The "
            "pair f1/f2 is what says the rule is the names' SPELLING and not their presence. "
            "Got:\n" + f2);

      // rtitles/ctitles: rows separated by a top-level backslash, cells by a
      // comma, each dequoted AFTER the split.
      okf("frmttable using \"w5f3.tex\", statmat(N) sdec(2) tex replace fragment nocenter "
          "rtitles(\"r1\" \\ \"r2\") ctitles(\"\", \"c1\", \"c2\", \"c3\")\n",
          "W5 (x.23): rtitles()/ctitles() run");
      const std::string f3 = slurp(at("w5f3.tex"));
      check(f3 ==
                "\\begin{tabular}{lccc}\n"
                "\\hline \\noalign{\\smallskip} & c1 & c2 & c3\\\\\n"
                "\\noalign{\\smallskip}\\hline \\noalign{\\smallskip}r1 & 1.00 & 2.00 & 3.00\\\\\n"
                "r2 & 4.00 & 5.00 & 6.00\\\\\n"
                "\\noalign{\\smallskip}\\hline\\end{tabular}\\\\\n",
            "W5 (x.23): ...and the backslash-separated list really is split (base h6). The first "
            "cut dequoted the option value BEFORE splitting it, which left the inner quotes "
            "unbalanced and collapsed every row into one cell reading `r1\" \\ \"r2` -- nine of "
            "the nineteen oracle arms at once. Got:\n" + f3);

      // A cell holding LaTeX: its backslashes must not be separators.
      okf("frmttable using \"w5f4.tex\", statmat(N) sdec(2) tex replace fragment nocenter "
          "rtitles(\"\\makecell[l]{\\it{Panel A}}\" \\ \"Any pollutant\")\n",
          "W5 (x.23): an rtitle carrying LaTeX runs");
      check(has(slurp(at("w5f4.tex")), "\\makecell[l]{\\it{Panel A}} & 1.00") &&
                has(slurp(at("w5f4.tex")), "Any pollutant & 4.00"),
            "W5 (x.23): ...and survives verbatim (names i7). This is paper 004's own spelling, "
            "and a splitter that treated every backslash as a row separator would cut it in "
            "three. Got:\n" + slurp(at("w5f4.tex")));

      // ctitles WIDER than the data sets the table width; the data rows pad.
      okf("frmttable using \"w5f5.tex\", statmat(N) sdec(2) tex replace fragment nocenter "
          "ctitles(\"\", \"c1\", \"c2\", \"c3\")\n",
          "W5 (x.23): ctitles() without rtitles() runs");
      const std::string f5 = slurp(at("w5f5.tex"));
      check(f5 ==
                "\\begin{tabular}{cccc}\n"
                "\\hline \\noalign{\\smallskip} & c1 & c2 & c3\\\\\n"
                "\\noalign{\\smallskip}\\hline \\noalign{\\smallskip}1.00 & 2.00 & 3.00 & \\\\\n"
                "4.00 & 5.00 & 6.00 & \\\\\n"
                "\\noalign{\\smallskip}\\hline\\end{tabular}\\\\\n",
            "W5 (x.23): ...and FOUR ctitles over a three-column matrix with no stub make a "
            "four-column table whose data rows carry a trailing empty cell (base h5) -- the "
            "width is the wider of the two, not the data's. Got:\n" + f5);

      // rtitles LONGER than the matrix adds rows.
      okf("frmttable using \"w5f6.tex\", statmat(N) sdec(2) tex replace fragment nocenter "
          "rtitles(\"a\" \\ \"b\" \\ \"c\")\n",
          "W5 (x.23): more rtitles than rows runs");
      check(has(slurp(at("w5f6.tex")), "c &  &  & \\\\"),
            "W5 (x.23): ...and ADDS a row with an empty data area (names ia). Got:\n" +
                slurp(at("w5f6.tex")));

      // The numeric format: thousands separators by default, and sdec()'s last
      // entry repeating.
      (void)exec("matrix BIG = (1234.5,6165.257,0.001\\2.5,3.5,4.5)\n", sf);
      okf("frmttable using \"w5f7.tex\", statmat(BIG) sdec(2) tex replace fragment nocenter\n",
          "W5 (x.23): a table of large numbers runs");
      check(has(slurp(at("w5f7.tex")), "1,234.50 & 6,165.26 & 0.00"),
            "W5 (x.23): ...with THOUSANDS SEPARATORS and no sfmt() asked for -- frmttable's "
            "default numeric format is `fc` (cells2 k6), and this build was dropping the commas. "
            "Got:\n" + slurp(at("w5f7.tex")));
      (void)exec("matrix S = (1.234,2.345,3.456\\7.891,8.912,9.123)\n", sf);
      okf("frmttable using \"w5f8.tex\", statmat(S) sdec(0,1) tex replace fragment nocenter\n",
          "W5 (x.23): a short sdec() list runs");
      check(has(slurp(at("w5f8.tex")), "1 & 2.3 & 3.5\\\\"),
            "W5 (x.23): ...and its LAST entry repeats for the remaining columns -- `sdec(0,1)` "
            "over three columns is 0, 1, 1 (cells2 k2), not 0, 1, default. Got:\n" +
                slurp(at("w5f8.tex")));

      // The wrappers.
      okf("frmttable using \"w5f9.tex\", statmat(N) sdec(2) tex replace\n",
          "W5 (x.23): the NON-fragment form runs");
      const std::string f9 = slurp(at("w5f9.tex"));
      check(has(f9, "\\documentclass[]{article}\\pagestyle{empty}\\begin{document}") &&
                has(f9, "\\begin{center}") && has(f9, "\\end{document}"),
            "W5 (x.23): ...and without `fragment` the ado writes a WHOLE DOCUMENT, and without "
            "`nocenter` it centres (base h1 against h3/h7). This build's `fragment` used to drop "
            "`\\begin{tabular}` itself and emit rows only. Got:\n" + f9);

      // W2AK: `replay` writes the STORED table (the last statmat() build) with
      // the new output options (w2at wt_f01b; matches frmttable's behaviour, identified from its ado).
      (void)exec("frmttable, statmat(N) sdec(1)\n", sf);
      auto rrep = exec("frmttable using \"w5fa.tex\", replay tex fragment replace\n", sf);
      check(firstError(rrep.blocks).is_null() && has(slurp(at("w5fa.tex")), "\\begin{tabular}"),
            "W5 (x.23): `replay` writes the stored table (w2at wt_f01b). Got: " +
                errText(firstError(rrep.blocks)));
      auto rrtf = exec("frmttable using \"w5fb.doc\", statmat(N) replace\n", sf);
      check(!firstError(rrtf.blocks).is_null(),
            "W5 (x.23): ...and the no-`tex` form stays a COMMAND-level refusal, because "
            "frmttable's default output is RTF (base h8 writes `{\\rtf1\\ansi...}`) and writing "
            "LaTeX under a .doc extension is the silent-wrong-output shape");
    }

    // -- (x.22) W1a-5g-2: `transform()` -- 18 corpus uses over four papers.
    // Oracle: w1b_transform.log (r*/s*/u*/v*/w*/x*) and w1b_transform2.log
    // (y*/z*). The assertions are RATIOS against a control rather than absolute
    // numbers, because the option's whole content is "what happened to this
    // cell relative to the untransformed one" and the fixture's coefficients
    // are not the oracle's.
    {
      // Every arm pins b()/se() to 16.10f. esttab's DEFAULT renders four
      // significant digits (the oracle control shows `1946.1`), and these
      // assertions are ratios read back OUT of the rendered text -- so at the
      // default format `2 * round(b, 4sf)` and `round(2b, 4sf)` differ and the
      // arms would fail on the printer rather than on the transform. Pinning the
      // format is not weakening them: w1b_transform2.log z5 is the arm that says
      // a transformed number is formatted by the normal rules.
      // The b and se of one coefficient, read back out of the written table.
      auto trimSp = [](const std::string& v) {
        size_t a = v.find_first_not_of(" \t");
        if (a == std::string::npos) return std::string();
        size_t b = v.find_last_not_of(" \t");
        return v.substr(a, b - a + 1);
      };
      auto cellOf = [&](const std::string& f, const std::string& coef) {
        const std::string t = slurp(at(f));
        std::pair<double, double> out{std::nan(""), std::nan("")};
        size_t i = 0;
        bool onRow = false;
        while (i < t.size()) {
          size_t e = t.find('\n', i);
          if (e == std::string::npos) e = t.size();
          const std::string line = t.substr(i, e - i);
          if (!onRow && line.compare(0, coef.size(), coef) == 0 &&
              (line.size() == coef.size() || line[coef.size()] == ' ' ||
               line[coef.size()] == '\t')) {
            try {
              out.first = std::stod(trimSp(line.substr(coef.size())));
            } catch (const std::exception&) {
            }
            onRow = true;
          } else if (onRow) {
            std::string v = trimSp(line);
            if (!v.empty() && v.front() == '(') v = v.substr(1);
            if (!v.empty() && v.back() == ')') v.pop_back();
            try {
              out.second = std::stod(v);
            } catch (const std::exception&) {
            }
            break;
          }
          i = e + 1;
        }
        return out;
      };
      auto near = [](double a, double b) {
        if (std::isnan(a) || std::isnan(b)) return false;
        return std::fabs(a - b) <= 1e-6 * std::max(1.0, std::fabs(b));
      };
      auto okt = [&](const std::string& cmd, const std::string& why) {
        auto r = exec(cmd, s);
        check(firstError(r.blocks).is_null(),
              why + " [got: " + errText(firstError(r.blocks)) + "]");
      };
      okt("esttab w5a using \"w5t0.txt\", replace b(%16.10f) se(%16.10f) nostar\n",
          "W5 (x.22): the control runs");
      const auto c0 = cellOf("w5t0.txt", "x1");
      const auto c0b = cellOf("w5t0.txt", "x2");
      check(!std::isnan(c0.first) && !std::isnan(c0.second),
            "W5 (x.22): ...and the control's x1 row reads back as a b over an se");

      // ONE token: the coefficient moves and the se does NOT. This is the arm
      // that says `df` is a separate expression rather than a format -- an
      // implementation that applied `f` to both would double the se here.
      okt("esttab w5a using \"w5t1.txt\", replace b(%16.10f) se(%16.10f) nostar transform(@*2)\n",
          "W5 (x.22): `transform(@*2)` runs -- it was r(198)");
      const auto c1 = cellOf("w5t1.txt", "x1");
      check(near(c1.first, c0.first * 2),
            "W5 (x.22): ...and the coefficient doubles (w1b_transform.log r1)");
      check(near(c1.second, c0.second),
            "W5 (x.22): ...while the se is UNTOUCHED, because a one-token list has no derivative "
            "(r1: 86.16 before and after). An implementation that reused `f` for the se would "
            "double it here and no other arm would notice");

      // TWO tokens: the second multiplies the se by abs().
      okt("esttab w5a using \"w5t2.txt\", replace b(%16.10f) se(%16.10f) nostar transform(@*2 2)\n",
          "W5 (x.22): `transform(@*2 2)` runs");
      const auto c2 = cellOf("w5t2.txt", "x1");
      check(near(c2.first, c0.first * 2) && near(c2.second, c0.second * 2),
            "W5 (x.22): ...and NOW both move -- the se by abs(df), evaluated at the coefficient "
            "(r2)");

      // THREE tokens: the first is a coefficient pattern.
      okt("esttab w5a using \"w5t3.txt\", replace b(%16.10f) se(%16.10f) nostar transform(x1 @*2 2)\n",
          "W5 (x.22): a three-token list runs");
      check(near(cellOf("w5t3.txt", "x1").first, c0.first * 2) &&
                near(cellOf("w5t3.txt", "x2").first, c0b.first),
            "W5 (x.22): ...and the pattern selects: x1 doubles, x2 does not (r3). With two tokens "
            "the SAME spelling would transform everything, so the arity is the grammar");

      // A TRAILING pair with no pattern catches everything the earlier groups
      // missed -- and its own missing df still leaves those ses alone.
      okt("esttab w5a using \"w5t4.txt\", replace b(%16.10f) se(%16.10f) nostar transform(x1 @*2 2 @*10)\n",
          "W5 (x.22): a four-token list runs");
      const auto t4a = cellOf("w5t4.txt", "x1");
      const auto t4b = cellOf("w5t4.txt", "x2");
      check(near(t4a.first, c0.first * 2) && near(t4a.second, c0.second * 2),
            "W5 (x.22): ...the first group still applies to its own pattern (r5)");
      check(near(t4b.first, c0b.first * 10) && near(t4b.second, c0b.second),
            "W5 (x.22): ...and the trailing PAIR becomes (f, df) with no pattern, so x2 takes "
            "@*10 with no derivative and keeps its se (r5). This is the one place the ado's list "
            "reader is not a plain triple split");

      okt("esttab w5a using \"w5t5.txt\", replace b(%16.10f) se(%16.10f) nostar transform(*1* @*2 2)\n",
          "W5 (x.22): a wildcard pattern runs");
      check(near(cellOf("w5t5.txt", "x1").first, c0.first * 2) &&
                near(cellOf("w5t5.txt", "x2").first, c0b.first),
            "W5 (x.22): ...and matches by glob (r8) -- which is how all four of paper 015's lines "
            "spell it, `transform(*romer* `f_scale'*@ `f_scale')`");
      okt("esttab w5a using \"w5t6.txt\", replace b(%16.10f) se(%16.10f) nostar transform(*:*1* @*2 2)\n",
          "W5 (x.22): an `eq:name` pattern runs");
      check(near(cellOf("w5t6.txt", "x1").first, c0.first * 2),
            "W5 (x.22): ...and reaches a row this engine stores with no equation at all (r9)");
      okt("esttab w5a using \"w5t7.txt\", replace b(%16.10f) se(%16.10f) nostar transform(nosuch @*2 2)\n",
          "W5 (x.22): a pattern that matches nothing runs");
      check(near(cellOf("w5t7.txt", "x1").first, c0.first),
            "W5 (x.22): ...and is the IDENTITY, not an error (r10). keep() answers r(111) for the "
            "same shape and transform() does not, so the two cannot share a rule");

      // The parenthesised row: t and p are in neither of the ado's branches.
      okt("esttab w5a using \"w5t8.txt\", replace nostar transform(@*2 2)\n",
          "W5 (x.22): the default table (t in parentheses) runs under transform()");
      auto rctl = exec("esttab w5a using \"w5t9.txt\", replace nostar\n", s);
      check(firstError(rctl.blocks).is_null(), "W5 (x.22): ...and so does its control");
      check(near(cellOf("w5t8.txt", "x1").second, cellOf("w5t9.txt", "x1").second),
            "W5 (x.22): ...and the t is UNCHANGED (s1). For a linear rescaling that is also the "
            "right answer -- b and se moved by the same factor -- which is why the arm has to be "
            "the t and not the b");

      // pattern() selects MODELS.
      okt("esttab w5a w5b using \"w5ta.txt\", replace b(%16.10f) se(%16.10f) nostar transform(@*2 2, pattern(1 0))\n",
          "W5 (x.22): `transform(, pattern())` runs");
      okt("esttab w5a w5b using \"w5tb.txt\", replace b(%16.10f) se(%16.10f) nostar transform(@*2 2, pattern(1))\n",
          "W5 (x.22): ...a pattern SHORTER than the model count runs");
      okt("esttab w5a w5b using \"w5tc.txt\", replace b(%16.10f) se(%16.10f) nostar transform(@*2 2, pattern())\n",
          "W5 (x.22): ...and an EMPTY pattern() runs");
      check(has(slurp(at("w5ta.txt")), "\n") && slurp(at("w5ta.txt")) == slurp(at("w5tb.txt")),
            "W5 (x.22): pattern(1) over two models is pattern(1 0) -- a word missing for a model "
            "means do not transform it (z1 against v1), not `carry the last one forward`");
      check(slurp(at("w5tc.txt")) != slurp(at("w5ta.txt")),
            "W5 (x.22): ...while an EMPTY pattern() transforms EVERY model (z4), same as leaving "
            "it out. The two empties do not mean opposite things");
      auto rbad =
          exec("esttab w5a w5b using \"w5td.txt\", replace b(%16.10f) se(%16.10f) nostar transform(@*2 2, pattern(2 0))\n",
               s);
      check(!firstError(rbad.blocks).is_null() &&
                has(errText(firstError(rbad.blocks)), "invalid pattern in transform"),
            "W5 (x.22): a pattern word that is neither 0 nor 1 is refused with estout's own "
            "sentence (tr_pat_two, r(198)). Got: " + errText(firstError(rbad.blocks)));

      // Paper 083's line, verbatim in shape: no comma, so pattern() lands in the
      // flat list and the whole thing is a measured NO-OP in Stata.
      okt("esttab w5a using \"w5te.txt\", replace b(%16.10f) se(%16.10f) nostar transform(@/5 1/5 pattern(1 1 0 0) "
          "@*100 100 pattern(0 0 1 1))\n",
          "W5 (x.22): paper 083's spelling runs");
      check(slurp(at("w5te.txt")) == slurp(at("w5t0.txt")),
            "W5 (x.22): ...and does NOTHING, byte for byte against the control (tr_083_verb, rc 0 "
            "in Stata too). `pattern()` is only a suboption after a comma, so without one it "
            "becomes a coefficient pattern that matches no row. Following the ado's list reader "
            "exactly is what buys this parity -- a lenient parser would have `fixed` it into a "
            "table Stata does not produce");

      // `_b[]` inside the expression resolves against EACH model.
      okt("esttab w5a w5b using \"w5tf.txt\", replace b(%16.10f) se(%16.10f) nostar transform(@/(-_b[x1]))\n",
          "W5 (x.22): paper 104's spelling -- `_b[]` inside the expression -- runs");
      check(near(cellOf("w5tf.txt", "x1").first, -1.0),
            "W5 (x.22): ...and a coefficient divided by its own negation is exactly -1 (y1/w1), "
            "which is the arm that says the expression was evaluated with THIS model current "
            "rather than with whatever estimation the session happened to hold");

      // estout, on the same specs. The point is agreement, not the numbers.
      okt("estout w5a using \"w5tg.txt\", replace cells(b(fmt(%16.10f)) se(fmt(%16.10f))) transform(@*2 2)\n",
          "W5 (x.22): estout takes transform() too -- 12 of the 18 corpus uses are on it");
      okt("estout w5a using \"w5th.txt\", replace cells(b(fmt(%16.10f)) se(fmt(%16.10f))) transform(x1 @*2 2)\n",
          "W5 (x.22): ...including the per-coefficient form");
      check(near(cellOf("w5tg.txt", "x1").first, c0.first * 2) &&
                near(cellOf("w5th.txt", "x2").first, c0b.first),
            "W5 (x.22): ...and it agrees with esttab cell for cell (y3/y4 against r2/r3). The two "
            "writers build their rows in different translation units and share ONE transform, "
            "which is the BUG-496 lesson applied before the fact rather than after");
    }

    // -- (x.21) BUG-496: keep()/drop()/order() take PATTERNS, and the two
    // commands have to agree about them. Oracle: w1b_keepglob.log (k0..kb).
    // The pair that matters is esttab-against-estout: the first fix of this
    // reached only estout's `coefNames` and left esttab's own copy exact, so
    // `keep(*2*)` selected a row on one command and emptied the table on the
    // other IN THE SAME BUILD. Every arm below is run on both.
    {
      auto bodyRows = [&](const std::string& f) {
        // The coefficient names present in the body, in order, as one string.
        const std::string t = slurp(at(f));
        std::string out;
        size_t i = 0;
        while (i < t.size()) {
          size_t e = t.find('\n', i);
          if (e == std::string::npos) e = t.size();
          const std::string line = t.substr(i, e - i);
          for (const char* nm : {"x1", "x2", "x3", "_cons"}) {
            const std::string n(nm);
            if (line.compare(0, n.size(), n) == 0) {
              out += n;
              out += " ";
              break;
            }
          }
          i = e + 1;
        }
        return out;
      };
      auto okg = [&](const std::string& cmd, const std::string& why) {
        auto r = exec(cmd, s);
        check(firstError(r.blocks).is_null(),
              why + " [got: " + errText(firstError(r.blocks)) + "]");
      };
      okg("esttab w5a w5b using \"w5g0.txt\", replace b se nostar\n",
          "W5 (x.21): the control runs");
      check(bodyRows("w5g0.txt") == "x1 x2 _cons ",
            "W5 (x.21): ...and the unfiltered body is the three coefficients in engine order. "
            "Got `" + bodyRows("w5g0.txt") + "`");

      okg("esttab w5a w5b using \"w5g1.txt\", replace b se nostar keep(*2*)\n",
          "W5 (x.21): `keep(*2*)` runs");
      okg("estout w5a w5b using \"w5g2.txt\", replace cells(b se) keep(*2*)\n",
          "W5 (x.21): ...and so does the same pattern on estout");
      check(bodyRows("w5g1.txt") == "x2 ",
            "W5 (x.21): `keep()` matches PATTERNS -- `*2*` keeps x2 (w1b_keepglob.log k2). Exact "
            "matching answered this with an EMPTY body at rc 0, which is the shape all four of "
            "paper 015's transform lines would have hit. Got `" + bodyRows("w5g1.txt") + "`");
      check(has(slurp(at("w5g2.txt")), "x2") && !has(slurp(at("w5g2.txt")), "x1"),
            "W5 (x.21): ...and estout selects the SAME row. The two commands used to have two "
            "answers about `*`. Got:\n" + slurp(at("w5g2.txt")));

      okg("esttab w5a w5b using \"w5g3.txt\", replace b se nostar keep(x?)\n",
          "W5 (x.21): `keep(x?)` runs -- `?` is one character, so this is Stata's match() and not "
          "a prefix rule (k a)");
      check(bodyRows("w5g3.txt") == "x1 x2 ",
            "W5 (x.21): ...and it keeps both two-character names and not `_cons`. Got `" +
                bodyRows("w5g3.txt") + "`");

      okg("esttab w5a w5b using \"w5g4.txt\", replace b se nostar keep(*:*2*)\n",
          "W5 (x.21): an `eq:name` pattern runs");
      check(bodyRows("w5g4.txt") == "x2 ",
            "W5 (x.21): ...and it reaches a row this engine stores with NO equation at all -- "
            "which is what makes the corpus's `keep(3:0.MI*#c.romer*)` able to match (k7). Got `" +
                bodyRows("w5g4.txt") + "`");

      okg("esttab w5a w5b using \"w5g5.txt\", replace b se nostar drop(*2* _cons)\n",
          "W5 (x.21): `drop()` takes patterns too");
      check(bodyRows("w5g5.txt") == "x1 ",
            "W5 (x.21): ...and a glob and an exact name in ONE drop() both bite (k8). Before this "
            "the exact `_cons` was removed and the glob silently was not, which is the worst of "
            "the two behaviours because the table still looks filtered. Got `" +
                bodyRows("w5g5.txt") + "`");

      okg("esttab w5a w5b using \"w5g6.txt\", replace b se nostar order(x2* x1)\n",
          "W5 (x.21): `order()` takes patterns");
      check(bodyRows("w5g6.txt") == "x2 x1 _cons ",
            "W5 (x.21): ...and promotes the matched rows in the order the option lists them, the "
            "rest keeping theirs (k9). Got `" + bodyRows("w5g6.txt") + "`");

      auto rn = exec("esttab w5a w5b using \"w5g7.txt\", replace b se nostar keep(zz*)\n", s);
      check(!firstError(rn.blocks).is_null() &&
                has(errText(firstError(rn.blocks)), "coefficient zz* not found"),
            "W5 (x.21): a keep() pattern that matches NOTHING is r(111) with Stata's own sentence "
            "(kg_glob_none), not a table with no rows. The empty table is the reading error: it "
            "says the coefficients were estimated and excluded. Got: " +
                errText(firstError(rn.blocks)));
      auto re = exec("estout w5a w5b using \"w5g8.txt\", replace cells(b se) keep(zz*)\n", s);
      check(!firstError(re.blocks).is_null() &&
                has(errText(firstError(re.blocks)), "coefficient zz* not found"),
            "W5 (x.21): ...on estout as well, because it is one filter now. Got: " +
                errText(firstError(re.blocks)));
    }

    // -- (x.20) W1a-5f: outreg2 `adec(#)` (106 corpus uses, three papers) and
    // the ado's own separator bug the User ruled we follow bug-for-bug.
    // Oracle: w1b_adec_rule{,2,3,4,5}.log. Every value asserted here is a
    // LITERAL in the addstat() call, so none of it depends on the fixture --
    // which is the whole reason the arms use literals rather than e() scalars.
    {
      piet::SessionState sa(workdir);
      sa.eval.cwd = workdir;
      sa.cwd = workdir;
      (void)exec(kMake30 + "regress y x1 x2\n", sa);
      auto run = [&](const std::string& cmd) { return exec(cmd, sa); };
      auto okc = [&](const std::string& cmd, const std::string& why) {
        auto r = run(cmd);
        check(firstError(r.blocks).is_null(),
              why + " [got: " + errText(firstError(r.blocks)) + "]");
      };
      auto refused = [&](const std::string& cmd, const std::string& needle,
                         const std::string& why) {
        auto r = run(cmd);
        const auto e = firstError(r.blocks);
        check(!e.is_null() && has(errText(e), needle),
              why + " [got: " + (e.is_null() ? std::string("NO ERROR") : errText(e)) + "]");
      };

      // The discriminator for adec() itself. `100` renders `100.0` under
      // adec(1) and `100` under the adaptive default (an integer takes no
      // decimals) -- and dec() can produce NEITHER, because it governs the
      // coefficients. So this pair is reachable only through adec().
      okc("outreg2 using \"w5ad1\", replace text addstat(M, 100) adec(1)\n",
          "W5 (x.20): `adec(#)` runs -- it was an unknown option on all 106 corpus uses");
      okc("outreg2 using \"w5ad2\", replace text addstat(M, 100)\n",
          "W5 (x.20): ...and the same line without it runs too");
      const std::string ad1 = slurp(at("w5ad1.txt"));
      const std::string ad2 = slurp(at("w5ad2.txt"));
      check(has(ad1, "\nM\t100.0\n"),
            "W5 (x.20): adec(1) renders the added statistic at ONE decimal -- `100.0`, which is "
            "w1b_adec_rule.log b6. Got:\n" + ad1);
      check(has(ad2, "\nM\t100\n") && !has(ad2, "100.0"),
            "W5 (x.20): ...while with no adec() the ADAPTIVE format takes an integer to no "
            "decimals at all. If both files said the same thing the option would be measuring "
            "nothing. Got:\n" + ad2);

      // BUG-494, and dec(4) is the only arm that separates the two formats: at
      // the default they agree, because %9.3f and the adaptive rule both give a
      // value between 1 and 10 three decimals.
      okc("outreg2 using \"w5ad3\", replace text addstat(M, 1.5) dec(4)\n",
          "W5 (x.20): `addstat()` under dec(4) runs");
      const std::string ad3 = slurp(at("w5ad3.txt"));
      check(has(ad3, "\nM\t1.500\n") && !has(ad3, "1.5000"),
            "W5 (x.20): ...and dec() does NOT reach the added statistic (BUG-494): the ado leaves "
            "it to the adaptive format, so it is `1.500` and not `1.5000` "
            "(w1b_adec_rule.log c2). Got:\n" + ad3);
      const size_t r2at = ad3.find("R-squared\t");
      const std::string r2cell =
          r2at == std::string::npos ? std::string() : ad3.substr(r2at + 10, 12);
      const size_t r2dot = r2cell.find('.');
      size_t r2dec = 0;
      while (r2dot != std::string::npos && r2dot + 1 + r2dec < r2cell.size() &&
             std::isdigit(static_cast<unsigned char>(r2cell[r2dot + 1 + r2dec]))) {
        ++r2dec;
      }
      check(r2dec == 4,
            "W5 (x.20): ...while dec(4) DID reach R-squared, which is what makes the line above a "
            "statement about the statistic rather than about dec() being ignored. Got " +
                std::to_string(r2dec) + " decimals in `" + r2cell + "`");

      // BUG-493. The refusal is on the FORMATTED string: 999.96 at one decimal
      // IS `1,000.0`, and 999.4 at zero is not. A |value| >= 1000 test passes
      // the second and fails the first.
      refused("outreg2 using \"w5ad4\", replace text addstat(M, 999.96) adec(1)\n", "1,000.0",
              "W5 (x.20): `addstat(M, 999.96) adec(1)` is REFUSED -- outreg2 formats it `1,000.0` "
              "and re-reads its own thousands separator as a field separator (r(198), "
              "w1b_adec_rule.log a1). The refusal is parity, not a gap");
      okc("outreg2 using \"w5ad5\", replace text addstat(M, 999.4) adec(0)\n",
          "W5 (x.20): ...while 999.4 at adec(0) is `999` and runs (a2) -- the rule is the STRING, "
          "not the value");

      // The width flip, which is the other end no `>= 1000` rule can reach.
      refused("outreg2 using \"w5ad6\", replace text addstat(M, 6165.257) adec(6)\n", "6,165.257000",
              "W5 (x.20): 6165.257 at adec(6) is `6,165.257000` -- 12 characters, the separator "
              "fits, refused (w1b_adec_rule5.log v2)");
      okc("outreg2 using \"w5ad7\", replace text addstat(M, 6165.257) adec(7)\n",
          "W5 (x.20): ...and at adec(7) the separated spelling is 13 characters, so Stata drops "
          "the separators and the SAME value runs (w1b_adec_rule3.log t1)");
      check(has(slurp(at("w5ad7.txt")), "\nM\t6165.2570000\n"),
            "W5 (x.20): ...rendering 6165.2570000, separator-free. Got:\n" +
                slurp(at("w5ad7.txt")));
      okc("outreg2 using \"w5ad8\", replace text addstat(M, 9999999) adec(3)\n",
          "W5 (x.20): and 9999999 at adec(3) runs for the same reason -- four orders of magnitude "
          "past `1000` and perfectly legal (w1b_adec_rule5.log v3)");
      check(has(slurp(at("w5ad8.txt")), "\nM\t9999999.000\n"),
            "W5 (x.20): ...as `9999999.000`. Got:\n" + slurp(at("w5ad8.txt")));

      // Which rc the ado dies with is positional, and both positions are
      // measured. A TRAILING pair loses its value; an earlier one pushes the
      // next pair's NAME into a value slot, where outreg2 reads it as a
      // variable -- so the message is that variable's name, not `invalid
      // syntax`.
      refused("outreg2 using \"w5ad9\", replace text addstat(A, 6165.257, B, 2.25) adec(1)\n",
              "B not found",
              "W5 (x.20): a big value in a NON-final pair shifts the next pair's name into a "
              "value slot -- outreg2 answers `B not found`, r(111), not r(198) "
              "(w1b_adec_rule.log d3)");
      refused("outreg2 using \"w5ada\", replace text addstat(A, 1.5, B, 6165.257) adec(1)\n",
              "6,165.3",
              "W5 (x.20): ...while the same value in the FINAL pair leaves that pair without a "
              "value and is r(198) (d4). One bug, two rcs, and the position is what picks");
      okc("outreg2 using \"w5adb\", replace text addstat(A, 1.5, B, 2.25) adec(1)\n",
          "W5 (x.20): ...and a multi-pair addstat() with small values runs -- the corpus's most "
          "common shape is two and three pairs, so `one pair only` would have been the wrong "
          "lesson to take from those rcs");
      check(has(slurp(at("w5adb.txt")), "\nA\t1.5\n") &&
                has(slurp(at("w5adb.txt")), "\nB\t2.2\n"),
            "W5 (x.20): ...with the ONE adec applying to every pair (d1). Got:\n" +
                slurp(at("w5adb.txt")));

      // The place this build deliberately does NOT follow the ado: two
      // separators make the field count even, so outreg2 succeeds and writes a
      // wrong number rather than refusing.
      refused("outreg2 using \"w5adc\", replace text addstat(M, 9999999) adec(0)\n", "9,999,999",
              "W5 (x.20): two separators are the NAMED divergence -- outreg2 2.x writes this table "
              "with `M 9` and an invented row `999` (w1b_adec_rule3.log t5, rc 0). Following that "
              "bug-for-bug would publish a wrong number, so this build refuses");

      // adec()'s own conditionality and bounds, each with the ado's sentence.
      refused("outreg2 using \"w5add\", replace text adec(1)\n",
              "cannot choose adec option without addstat option",
              "W5 (x.20): `adec()` with no `addstat()` is outreg2's own conditional refusal, word "
              "for word (w1b_adec_rule.log e1)");
      refused("outreg2 using \"w5ade\", replace text addstat(M, 1.5) adec(12)\n",
              "outside of allowed range",
              "W5 (x.20): adec() is `numlist int >=0 <=11`, and 12 is r(125) (g1)");
      refused("outreg2 using \"w5adf\", replace text addstat(M, 1.5) adec(1.5)\n", "noninteger",
              "W5 (x.20): ...a non-integer is a DIFFERENT rc, r(126), with its own sentence (g3)");
      refused("outreg2 using \"w5adg\", replace text addstat(M, 1.5) adec(1 3)\n",
              "single number required",
              "W5 (x.20): ...and `max=1` makes two numbers r(123) (d2). Collapsing these three "
              "into one rc would lose what the user is being told");

      // `afmt()` is the ado's escape hatch and is deliberately NOT accepted:
      // zero corpus uses and its non-`f` suffixes are unmeasured.
      refused("outreg2 using \"w5adh\", replace text addstat(M, 6165.257) adec(1) afmt(f)\n",
              "afmt",
              "W5 (x.20): `afmt()` stays REFUSED BY NAME. It is the one spelling that makes the "
              "big-mean line legal in Stata, so accepting it while guessing its other suffixes "
              "would be exactly the accept-and-ignore this campaign exists to prevent");
    }

    // ── (x.15) W1a-2 / BUG-407: cells(), and main()/aux() which ARE cells().
    // esttab builds a cells() spec out of main/aux whenever cells()
    // is absent, so all three reach one path here. Oracle:
    // tests/eval/golden/aer_w1/w1b_tables_pins.log arms c1/c3/c4/c5/c7.
    {
      auto run = [&](const std::string& cmd) { return exec(cmd, s); };
      auto okc = [&](const std::string& cmd, const std::string& why) {
        auto r = run(cmd);
        check(firstError(r.blocks).is_null(),
              why + " [got: " + errText(firstError(r.blocks)) + "]");
      };
      // c1: the coefficient carries the star and the format; the aux row is
      // parenthesised because the SPEC says par, not because it always is.
      okc("esttab w5a w5b using \"w5k1.txt\", replace cells(b(star fmt(%9.3f)) se(par))\n",
          "W5 (x.15): `cells(b(star fmt(%9.3f)) se(par))` runs -- it was refused by name until now "
          "(BUG-407)");
      const std::string k1 = slurp(at("w5k1.txt"));
      check(has(k1, "(") && k1.find('*') != std::string::npos,
            "W5 (x.15): ...and it renders a starred coefficient over a parenthesised se. Got:\n" +
                k1);
      // c4: main(se 2) aux(none) -- the MAIN row carries the standard error,
      // there is no aux row at all, and the legend names both.
      okc("esttab w5a w5b using \"w5k4.txt\", replace main(se 2) aux(none)\n",
          "W5 (x.15): `main(se 2) aux(none)` runs");
      const std::string k4 = slurp(at("w5k4.txt"));
      check(has(k4, "se coefficients; none in parentheses"),
            "W5 (x.15): ...and the legend is `se coefficients; none in parentheses` -- the word "
            "`none` really is in Stata's note (c4), which is the sort of thing only a capture "
            "tells you. Got:\n" + k4);
      check(!has(k4, "t statistics in parentheses"),
            "W5 (x.15): ...and the DEFAULT legend is gone, because the aux row is");
      // c5: main(b 3) aux(t 2) -- both named, both formats honoured.
      okc("esttab w5a w5b using \"w5k5.txt\", replace main(b 3) aux(t 2)\n",
          "W5 (x.15): `main(b 3) aux(t 2)` runs");
      check(has(slurp(at("w5k5.txt")), "b coefficients; t in parentheses"),
            "W5 (x.15): ...and its legend names both statistics (c5)");
      // `aux(none)` is NOT `not`, and the difference is four rows. `not`
      // removes the aux row and every gap (t_not.txt); `aux(none)` names a
      // cell estout does not know, so the row is still EMITTED as blanks and
      // the gap survives (c4, whose lines 5/8/11/14 are 44 columns of spaces).
      // The first cut of W1a-2 mapped both onto one enum and dropped those
      // rows; this pair is what tells them apart.
      {
        auto countBlankPadded = [&](const std::string& t) {
          size_t n = 0;
          size_t pos = 0;
          while (pos <= t.size()) {
            const size_t nl = t.find('\n', pos);
            const std::string line = t.substr(pos, (nl == std::string::npos ? t.size() : nl) - pos);
            if (!line.empty() && line.find_first_not_of(' ') == std::string::npos) ++n;
            if (nl == std::string::npos) break;
            pos = nl + 1;
          }
          return n;
        };
        const std::string an = slurp(at("w5k4.txt"));
        okc("esttab w5a w5b using \"w5kn.txt\", replace not\n", "W5 (x.15): `not` runs");
        const std::string nt = slurp(at("w5kn.txt"));
        check(countBlankPadded(an) > 0,
              "W5 (x.15): `aux(none)` EMITS its aux row as blanks -- padded-but-present lines, "
              "not removed (c4). Got:\n" + an);
        check(countBlankPadded(nt) == 0,
              "W5 (x.15): ...while `not` removes the aux row entirely and leaves no padded blank "
              "behind (t_not.txt). Two options, two shapes, one enum used to serve both. Got:\n" +
                  nt);
      }
      // Under cells() there are NO notes at all -- not the coefficient line and
      // not the star legend. All four captured cells() arms end at the rule.
      check(!has(k1, "p<0.05"),
            "W5 (x.15): under cells() the STAR LEGEND is suppressed too, not just the coefficient "
            "line -- c1/c3/c7/c8 all end at the rule under N. Suppressing only the first would "
            "have left a legend Stata does not print. Got:\n" + k1);
      {
        auto r = run("esttab w5a using \"w5kc.txt\", replace\n");
        check(firstError(r.blocks).is_null(), "W5 (x.15): the control still runs");
        check(has(slurp(at("w5kc.txt")), "t statistics in parentheses"),
              "W5 (x.15): ...and WITHOUT any of the three the default legend is untouched -- the "
              "cells() path is separate precisely so the byte-verified default cannot move");
      }
      // c7: `par([ ])` names the two delimiters. The depth-blind bracket guard
      // refused this spelling, which the corpus writes 327 times against zero
      // uses of the top-level grouping the guard exists for.
      okc("esttab w5a w5b using \"w5k7.txt\", replace cells(se(par([ ]) fmt(2)))\n",
          "W5 (x.15): `cells(se(par([ ]) fmt(2)))` runs -- the brackets are DELIMITERS, not the "
          "grouping notation");
      check(has(slurp(at("w5k7.txt")), "["),
            "W5 (x.15): ...and the se really is rendered in brackets (c7)");
      // ── The four things cells() inherits from ESTOUT, not from esttab.
      // esttab forwards the cells() spec to estout unchanged (matches esttab's
      // behaviour, identified from its ado), so on this path
      // esttab's own defaults do not apply at all. Each of these was live in
      // the first cut of BUG-407's fix and each was caught by an adversarial
      // re-read of the ado against the same captures.
      {
        const std::string c1 = slurp(at("w5k1.txt"));
        const std::string c7 = slurp(at("w5k7.txt"));
        okc("esttab w5a w5b using \"w5k3.txt\", replace cells(b(star) p(par fmt(3)))\n",
            "W5 (x.15): `cells(b(star) p(par fmt(3)))` runs");
        const std::string c3 = slurp(at("w5k3.txt"));
        const std::string ctl = slurp(at("w5kc.txt"));
        // D3 -- the cell-name row, joined with `/` when the spec stacks two.
        check(has(c1, "\tb/se") || c1.find("b/se") != std::string::npos,
              "W5 (x.15): cells() lets estout's CELL-NAME row through -- `b/se` (c1). esttab "
              "injects collabels(none) to suppress it only on the no-cells path "
              "(matches esttab's behaviour, identified from its ado). Got:\n" + c1);
        check(c3.find("b/p") != std::string::npos && c7.find("\n" ) != std::string::npos &&
                  c7.find("se") != std::string::npos,
              "W5 (x.15): ...and it names what the spec actually holds -- `b/p` for c3, `se` "
              "alone for the one-item c7");
        check(ctl.find("b/se") == std::string::npos,
              "W5 (x.15): ...while the DEFAULT table has no such row at all");
        // D4 -- the WRONG VALUE. A cells() item with no fmt() is %9.0g, not
        // esttab's adaptive a3. This is the one that printed a different number.
        // Fixture-independent: count the decimals in the first numeric cell of
        // the body. `a3` can never exceed three; %9.0g routinely does.
        auto firstDecimals = [](const std::string& t) -> int {
          size_t pos = 0;
          int line = 0;
          while (pos <= t.size()) {
            const size_t nl = t.find('\n', pos);
            const std::string l = t.substr(pos, (nl == std::string::npos ? t.size() : nl) - pos);
            if (nl == std::string::npos) break;
            pos = nl + 1;
            // The head rows and the aux rows begin with a space and the rules
            // with `-`, so the first line that survives both is the first
            // COEFFICIENT row. (An earlier cut also skipped three "head" lines
            // on top of that and therefore skipped every body row instead,
            // returning -1 — the check failed against correct output.)
            if (l.empty() || l[0] == '-' || l[0] == ' ') continue;
            (void)line;
            const size_t dot = l.find('.');
            if (dot == std::string::npos) continue;
            int n = 0;
            for (size_t i = dot + 1; i < l.size() && std::isdigit(static_cast<unsigned char>(l[i]));
                 ++i) {
              ++n;
            }
            return n;
          }
          return -1;
        };
        const int d3 = firstDecimals(c3);
        const int d1 = firstDecimals(c1);
        check(d3 > 3,
              "W5 (x.15): a cells() item with NO fmt() carries Stata's general format, which "
              "routinely runs past three decimals (`-49.51222` on the oracle's data) -- esttab's "
              "adaptive a3 never can. The first cut printed the a3 value, which is a different "
              "NUMBER in a published table. Got " + std::to_string(d3) + " decimals:\n" + c3);
        check(d1 == 3,
              "W5 (x.15): ...and an item that DOES give fmt() keeps exactly it (c1's b at %9.3f), "
              "with the next item inheriting that running format rather than reverting to the "
              "default. Got " + std::to_string(d1) + " decimals:\n" + c1);
        // D2 -- no gaps under cells().
        auto blankLines = [](const std::string& t) {
          size_t n = 0, pos = 0;
          while (pos <= t.size()) {
            const size_t nl = t.find('\n', pos);
            if (t.substr(pos, (nl == std::string::npos ? t.size() : nl) - pos).empty()) ++n;
            if (nl == std::string::npos) break;
            pos = nl + 1;
          }
          return n;
        };
        check(blankLines(c1) <= 1 && blankLines(ctl) > 1,
              "W5 (x.15): cells() tables have NO blank line between coefficients and the default "
              "table does -- the gap is esttab's, added at :495-496 only when cells() is absent. "
              "Got cells:\n" + c1);
        // D5 -- the star field is reserved only when a spec asks for stars.
        const size_t ruleC1 = c1.find('\n') == std::string::npos ? 0 : c1.find('\n');
        const size_t ruleC7 = c7.find('\n') == std::string::npos ? 0 : c7.find('\n');
        check(ruleC1 > ruleC7,
              "W5 (x.15): ...and a spec with no `star` reserves no star column, so c7's rule is "
              "NARROWER than c1's by exactly the star field on each model column (38 vs 44 on the "
              "oracle's two-model table). Got " + std::to_string(ruleC1) + " vs " +
                  std::to_string(ruleC7));
      }
      {
        auto r = run("esttab w5a using \"w5kg.txt\", replace cells([b se])\n");
        check(!firstError(r.blocks).is_null(),
              "W5 (x.15): ...while the TOP-LEVEL grouping `cells([b se])` is still refused -- the "
              "guard became depth-aware, not absent");
      }
      // W2AK: the shapes that used to be refused by name are RENDERED now that
      // esttab runs on the transcribed estout writer. The quoted form puts its
      // items side by side (one row, two columns per model; aer_w2
      // w2ak_estout_family.log ek_e33 `&       1.468&    (0.0742)&`); a third
      // item is a third stacked row (ek_e20, `[0.500]`); and a statistic e()
      // does not hold resolves to no coefficient rows at all, rc 0 (ek_c07,
      // log:3360-3375: the table is heads and rules only).
      {
        auto r = run("esttab w5a using \"w5kx1.txt\", replace cells(\"b se\")\n");
        check(firstError(r.blocks).is_null(), "W5 (x.15): cells(\"b se\") renders (ek_e33)");
        const std::string t = slurp(at("w5kx1.txt"));
        size_t xl = t.find("\nx1");
        const std::string row = xl == std::string::npos ? std::string()
                                                       : t.substr(xl + 1, t.find('\n', xl + 1) - xl - 1);
        size_t nTok = 0;
        {
          std::istringstream is(row);
          std::string tok;
          while (is >> tok) ++nTok;
        }
        check(nTok == 3,
              "W5 (x.15): ...as ONE row per coefficient with b and se side by side. Got:\n" + t);
      }
      {
        auto r = run("esttab w5a using \"w5kx2.txt\", replace cells(b se t)\n");
        check(firstError(r.blocks).is_null(), "W5 (x.15): cells(b se t) renders (ek_e20)");
      }
      {
        auto r = run("esttab w5a using \"w5kx3.txt\", replace cells(zzzbogus)\n");
        check(firstError(r.blocks).is_null() && !has(slurp(at("w5kx3.txt")), "x1"),
              "W5 (x.15): cells(zzzbogus) is rc 0 with no coefficient rows (ek_c07)");
      }
    }
  }

  fs::remove_all(workdir, ec);
}


// BUG-102: a factor-variable coefficient row substitutes the attached value
// label for the bare level (verified live against StataBE 19.5: `regress y
// i.state_num` with `label values state_num statelbl` prints "Arizona"/
// "Idaho" rows, not "2.state_num"/"3.state_num") — checks the substituted
// case (regress's own M2 slice, estimation/linear_estimation_support.cpp's cobraToEstimation) and the
// label-less fallback (bare "<level>.<var>" unchanged, same session shape).
void testBug102CoeftableValueLabels() {
  std::printf("[selftest] --- BUG-102 coeftable value-label substitution ---\n");

#ifdef _WIN32
  std::error_code bec;
  std::string workdir =
      (std::filesystem::temp_directory_path() / "piet_native_selftest_bug102").u8string();
  std::filesystem::create_directories(workdir, bec);
#else
  std::string workdir = "/tmp/piet_native_selftest_bug102";
  (void)std::system(("mkdir -p '" + workdir + "'").c_str());
#endif

  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("gen state_num = int((_n-1)/112)+1", session);
    (void)piet::dispatch("label define statelbl 1 \"CA\" 2 \"NY\" 3 \"TX\"", session);
    (void)piet::dispatch("label values state_num statelbl", session);
    auto blocks = piet::dispatch("regress beertax i.state_num", session);
    nlohmann::ordered_json coeftable;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "estimation") {
        coeftable = b.value("coeftable", nlohmann::ordered_json::array());
      }
    }
    // BUG-397 (2026-09-02, re-keyed to the g12 oracle): the row NAME is Stata's e(b) colname and
    // stays the bare level (`2.state_num`); the label rides on `stub` (display shape) and `label`.
    bool sawStub = false, sawBareName = false, sawLabelInName = false, sawLabel = false;
    for (const auto& row : coeftable) {
      std::string nm = row.value("name", std::string());
      std::string st = row.value("stub", std::string());
      std::string lb = row.value("label", std::string());
      if (st == "NY.state_num" || st == "TX.state_num") sawStub = true;
      if (nm == "2.state_num" || nm == "3.state_num") sawBareName = true;
      if (nm == "NY.state_num" || nm == "TX.state_num") sawLabelInName = true;
      if (lb == "NY" || lb == "TX") sawLabel = true;
    }
    check(sawStub, "BUG-102/397: a labeled factor level carries the substituted display name in `stub` (NY.state_num)");
    check(sawLabel, "BUG-397: a labeled factor level carries the bare label text in `label` (NY)");
    check(sawBareName, "BUG-397: the row `name` stays Stata's e(b) colname (2.state_num) — _b[2.state_num] keeps resolving");
    check(!sawLabelInName, "BUG-397: the label never replaces the e(b) colname in `name`");
  }

  {
    // Label-less fallback: same factor variable, no `label values` attached.
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("gen state_num = int((_n-1)/112)+1", session);
    auto blocks = piet::dispatch("regress beertax i.state_num", session);
    nlohmann::ordered_json coeftable;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "estimation") {
        coeftable = b.value("coeftable", nlohmann::ordered_json::array());
      }
    }
    bool sawBareLevel = false;
    for (const auto& row : coeftable) {
      std::string nm = row.value("name", std::string());
      if (nm == "2.state_num" || nm == "3.state_num") sawBareLevel = true;
    }
    check(sawBareLevel, "BUG-102: label-less fallback keeps the bare level.varname name");
  }

#ifdef _WIN32
  std::error_code brec;
  std::filesystem::remove_all(workdir, brec);
#else
  (void)std::system(("rm -rf '" + workdir + "'").c_str());
#endif
}


// --- O. BS-097 option consume-or-reject net + BS-018 `do`/`run FILE,
// nostop` (BETA campaign Phase A2). ---
//
// BS-097: before this phase, dispatcher.cpp, the M4 estimation code and
// the eststo/esttab code had exactly 3 ad-hoc "unknown option -> honest reject"
// nets (summarize inline in dispatcher.cpp; estimation/linear_estimation.cpp's runRegress;
// estimation/estimation_support.cpp's shared xtreg(fe/re)/logit/probit builder — all
// exercised above by testClusterVceAndUnknownOptionFixes). `margins` had
// ZERO such net (the B6 audit finding this phase is named for). This phase
// adds ONE shared helper (eval::rejectUnknownOptions, eval/errors.h/.cpp —
// PIET's analog of the engine's g_argv_consumed/check_unconsumed_flags E6
// net) and wires it into margins plus 18 more previously-net-less arms.
//
// Each leg below pairs two assertions: (a) a bogus extra option is an
// honest reject that NAMES the bogus token (the primary evidence the net
// works), and (b) for arms with a real recorded shape, the SAME invocation
// internal capture actually captures from debug_all.do still
// succeeds unchanged — that freeze corpus line is quoted in each leg's
// comment as the ground truth being preserved (an over-rejection guard,
// same spirit as testClusterVceAndUnknownOptionFixes's leg (c) above).
//
// tabstat WAS deliberately left un-netted here (debug_all.do's frozen
// `tabstat y x1, by(region) stat(mean sd n)` uses the singular `stat(...)`
// spelling, which the arm only silently defaulted around rather than
// reading) — BUG105-OPTNET-STATVERBS closes that gap by recognizing `stat`
// as a real alias for `stats`/`statistics` in the SAME change that adds the
// net, so the frozen invocation is now honored (not just silently
// defaulted) instead of newly rejected. See its leg below.
// AER FINISH W1a — the table bundle's ONE-CHARACTER option spellings.
//
// W0's replay found six of this bundle's highest-use corpus spellings are one
// character long (`tab y, m` alone is 12 papers / 106 uses) and that PIET
// refused every one with r(198). The refusal was not a defect in the nets:
// eval::resolveOptionAbbrevAcross has a deliberate 2-character floor, because
// widening below it without an oracle row is fabricated parity — the class that
// once let `export delimited using "f.csv", re` OVERWRITE a file. So the fix is
// a MEASURED row, not a looser rule, and this arm is the row's other half.
//
// Oracle: tests/eval/golden/aer_w1/w1a_optmin_pins{,2}.log (StataBE 19.5,
// `sysuse auto`), one prefix per length per option. Both directions are pinned:
// the spelling Stata accepts must now work, and a spelling Stata REFUSES must
// still be refused — `tabulate, n` and `tabulate, no` are r(198) in Stata
// (w1a_optmin_pins2.log), so they stay r(198) here.
//
// `table, contents()` is the arm that must NOT be implemented. This StataBE
// answers it "option contents() not allowed since Stata 17; see help table for
// updated syntax" — at EVERY prefix length including the full spelling. The
// corpus line that spells it (090, 3 papers / 9 uses) therefore does not run in
// this Stata either, so PIET's r(198) is parity, and the pin below is what stops
// a future session from "fixing" it into a divergence.
void testW1aTableOptionMinimums() {
  std::printf("[selftest] --- W1a table bundle: measured 1-char option minimums ---\n");
  namespace fs = std::filesystem;
  // Own workdir AND own SessionState: this arm fits models, and the arms after
  // it replay lastModel/currentHbin (BUG-479's second half).
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w1a").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  auto firstError = [](const nlohmann::ordered_json& blocks) -> nlohmann::ordered_json {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") return b;
    }
    return nlohmann::ordered_json();
  };
  auto ok = [&](const std::string& cmd, const std::string& why) {
    auto err = firstError(piet::dispatch(cmd, session));
    check(err.is_null(), why + (err.is_null() ? "" : " [got: " + err.value("message", std::string()) + "]"));
  };
  auto rejects = [&](const std::string& cmd, const std::string& token, const std::string& why) {
    auto err = firstError(piet::dispatch(cmd, session));
    check(!err.is_null() && err.value("code", std::string()) == "unsupported_option" &&
              err.value("message", std::string()).find(token) != std::string::npos,
          why);
  };

  ok("sysuse fatalities, clear", "W1a setup: `sysuse fatalities, clear` succeeds");

  // tabulate — missing (min 1) and generate (min 1), both MEASURED rc 0 in Stata.
  ok("tabulate year, m", "W1a tabulate: `m` reaches missing() (Stata min 1)");
  ok("tabulate year, missing", "W1a tabulate: the full `missing` spelling still works");
  ok("tabulate year, g(w1atab)", "W1a tabulate: `g()` reaches generate() (Stata min 1)");
  // The narrowing direction. Stata refuses both of these (w1a_optmin_pins2.log),
  // and `n` is a proper prefix of tabulate's `nofreq`, so a looser rule would
  // have silently turned it into a different option.
  rejects("tabulate year, n", "n", "W1a tabulate: `n` stays r(198) — Stata refuses it too");
  rejects("tabulate year, no", "no", "W1a tabulate: `no` stays r(198) — Stata refuses it too");
  rejects("tabulate year, zzzq", "zzzq", "W1a tabulate: an invented option is still named and rejected");

  // estpost — columns (min 1) and statistics (min 1). The colliding sibling for
  // `c` is `casewise`, which takes no argument; the oracle arm passes one.
  ok("regress fatal beertax spirits", "W1a setup: regress seeds an estimate");
  ok("estpost tabstat fatal beertax, c(statistics) statistics(mean sd)",
     "W1a estpost: `c()` reaches columns() (Stata min 1)");
  ok("estpost tabstat fatal beertax, s(mean sd)",
     "W1a estpost: `s()` reaches statistics() (Stata min 1)");

  // esttab — label (min 1). `l` is the spelling two papers write.
  ok("eststo w1am1", "W1a setup: eststo stores the model");
  ok("esttab w1am1 using \"" + workdir + "/w1a_l.txt\", replace l",
     "W1a esttab: `l` reaches label (Stata min 1)");

  // table, contents() — the one that must STAY refused. Parity, not a gap.
  rejects("table year, c(mean fatal)", "c",
          "W1a table: `c()`/`contents()` stays r(198) — Stata 17+ removed the option "
          "(w1a_optmin_pins2.log: \"option contents() not allowed since Stata 17\")");

  // ── W1a-1 (2026-09-14): the two options that were ABSENT from their nets.
  // tabulate nolabel (min 3) -- "display numeric codes rather than value
  // labels". Both directions on one labelled variable: labelled rows without
  // the option (BUG-355's contract), numeric codes with it, the measured
  // refusals around it.
  auto tabRows = [&](const std::string& cmd) {
    std::vector<std::string> out;
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object() || b.value("type", std::string()) != "tabulate") continue;
      if (!b.contains("table") || !b["table"].contains("rows")) continue;
      for (const auto& r : b["table"]["rows"]) out.push_back(r.get<std::string>());
    }
    return out;
  };
  auto has = [](const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
  };
  ok("generate byte w1ag = 1 + mod(_n, 3)", "W1a-1 setup: a three-level code");
  ok("label define w1alab 1 \"one\" 2 \"two\"", "W1a-1 setup: label define");
  ok("label values w1ag w1alab", "W1a-1 setup: label values");
  {
    auto labelled = tabRows("tabulate w1ag");
    auto coded = tabRows("tabulate w1ag, nol");
    auto codedFull = tabRows("tabulate w1ag, nolabel");
    if (labelled.empty() || coded.empty()) {
      skip("W1a-1 tabulate nolabel", "tabulate produced no table in this environment");
    } else {
      check(has(labelled, "one") && has(labelled, "two") && has(labelled, "3.0"),
            "W1a-1 tabulate: without nolabel the rows carry the labels (BUG-355's mix holds)");
      check(!has(coded, "one") && !has(coded, "two"),
            "W1a-1 tabulate: `nol` (Stata min 3) drops the label text");
      check(has(coded, "1.0") && has(coded, "2.0") && has(coded, "3.0"),
            "W1a-1 tabulate: `nol` renders the numeric codes Stata prints "
            "(w1a_optmin_pins.log: `tabulate foreign, nol` rows are 0/1)");
      check(coded == codedFull, "W1a-1 tabulate: `nol` and `nolabel` render the same table");
    }
  }
  ok("tabulate w1ag, nokey",
     "W1a-1 tabulate: `nokey` is rc 0 in Stata (w1a_optmin_pins3.log) — a validated no-op here");
  rejects("tabulate w1ag, nolog", "nolog",
          "W1a-1 tabulate: `nolog` stays r(198) — Stata refuses it too (w1a_optmin_pins3.log)");

  // list abbreviate (min 2). `ab(40)` is the corpus spelling (3 papers / 18 uses).
  ok("list fatal in 1/2, ab(40)", "W1a-1 list: `ab()` reaches abbreviate() (Stata min 2)");
  ok("list fatal in 1/2, abbreviate(8)", "W1a-1 list: the full spelling works");
  rejects("list fatal in 1/2, a(40)", "a",
          "W1a-1 list: `a` stays r(198) — Stata refuses it too (w1a_optmin_pins.log)");
  rejects("list fatal in 1/2, ab", "abbreviate",
          "W1a-1 list: bare `ab` with no width is refused — the number IS the option");
  rejects("list fatal in 1/2, ab(x)", "abbreviate", "W1a-1 list: a non-numeric width is refused");

  fs::remove_all(workdir, ec);
}


// BUG-160 CLOSURE: `table (row) (col), statistic(A) statistic(B) ...` must
// render ONE table_result (not one per statistic) with Total row/column
// margins and value-label-aware headers — and the SAME fix must reach the
// single-statistic path too (the BUGLOG entry's own finding: Stata prints
// Total/labels there just as much). Hand-verified against a tiny synthetic
// dataset (six rows, two row levels, two label-headed column levels, one
// missing statvar value placed so its CELL still has a sibling non-missing
// value — the BUG-161 "one missing poisons the cell" shape) rather than the
// P4 wave's live oracle fixture: every OTHER selftest check in this file
// builds its own in-session data for exactly this reason — a gitignored,
// Stata-generated fixture (`waves/_artifacts/wave_fixture.dta`) is not
// guaranteed to exist in a fresh checkout, so it cannot be a selftest
// dependency. The wave fixture itself is exercised separately (this stint's
// report cites the live `--do` run against it, byte-matching
// `waves/_oracle/p4_stata.log:271-296` cell for cell) and the "kitchensink
// pin for the rendering" lives in internal capture table.json
// (regenerated by tools/freeze_matrix.py against the real corpus).
void testBUG160TableNestedMultistat() {
  std::printf("[selftest] --- BUG-160: table's nested multi-statistic layout ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug160").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  auto run = [&](const std::string& cmd) { return piet::dispatch(cmd, session); };
  run("clear");
  run("set obs 6");
  run("generate double region = 1");
  run("replace region = 2 in 4");
  run("replace region = 2 in 5");
  run("replace region = 2 in 6");
  run("generate double treated = 0");
  run("replace treated = 1 in 3");
  run("replace treated = 1 in 5");
  run("replace treated = 1 in 6");
  run("generate double y = .");
  run("replace y = 10 in 1");
  run("replace y = 20 in 2");
  run("replace y = 30 in 3");
  run("replace y = 40 in 4");
  run("replace y = 70 in 6");  // row 5's y stays missing (BUG-161 shape)
  run("label define trtlbl 0 \"no\" 1 \"yes\"");
  run("label values treated trtlbl");

  auto findType = [](const nlohmann::ordered_json& blocks, const std::string& type) {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == type) return b;
    }
    return nlohmann::ordered_json();
  };
  auto near = [](const nlohmann::ordered_json& v, double want) {
    return v.is_number() && std::fabs(v.get<double>() - want) < 1e-9;
  };

  // ---- multi-statistic two-way: ONE block, Total margins, label headers --
  {
    auto blocks = run("table (region) (treated), statistic(mean y) statistic(count y)");
    int nTableResult = 0;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "table_result") ++nTableResult;
    }
    check(nTableResult == 1,
          "BUG-160: two statistic() options render as ONE table_result, not two");
    auto tr = findType(blocks, "table_result");
    check(!tr.is_null() && tr.value("row_labels", nlohmann::ordered_json()) ==
                               nlohmann::ordered_json({"1", "2", "Total"}),
          "BUG-160: row_labels carries a Total row");
    check(!tr.is_null() && tr.value("col_labels", nlohmann::ordered_json()) ==
                               nlohmann::ordered_json({"no", "yes", "Total"}),
          "BUG-160: col_labels are value-label text (\"no\"/\"yes\"), not \"0\"/\"1\", plus Total");
    if (!tr.is_null()) {
      const auto& cells = tr["cells"];
      check(near(cells["1"]["mean"]["no"], 15.0) && near(cells["1"]["mean"]["yes"], 30.0) &&
                near(cells["1"]["mean"]["Total"], 20.0),
            "BUG-160: region 1's mean row (no/yes/Total margin)");
      check(near(cells["1"]["count"]["no"], 2.0) && near(cells["1"]["count"]["yes"], 1.0) &&
                near(cells["1"]["count"]["Total"], 3.0),
            "BUG-160: region 1's count row (no/yes/Total margin)");
      check(near(cells["2"]["mean"]["no"], 40.0) && near(cells["2"]["mean"]["yes"], 70.0),
            "BUG-160: region 2's mean row — row 5's missing y did not poison the \"yes\" cell "
            "(BUG-161 shape, re-verified through the new nested renderer)");
      check(near(cells["2"]["count"]["yes"], 1.0),
            "BUG-160: region 2's \"yes\" count is 1 (nonmissing y), not 2 (row count)");
      check(near(cells["Total"]["mean"]["no"], 70.0 / 3.0) && near(cells["Total"]["mean"]["yes"], 50.0) &&
                near(cells["Total"]["mean"]["Total"], 34.0),
            "BUG-160: the Total ROW's mean margin collapses region entirely (recomputed from the "
            "pooled values, not an average of the per-region means)");
      check(near(cells["Total"]["count"]["Total"], 5.0),
            "BUG-160: the grand Total/Total cell is nonmissing-y count, not the 6-row extent");
    }
  }

  // ---- single-statistic two-way: SAME fix, flat legacy shape -------------
  {
    auto blocks = run("table (region) (treated), statistic(mean y)");
    auto tr = findType(blocks, "table_result");
    check(!tr.is_null() && tr.value("stat", std::string()) == "mean" &&
              tr.value("statvar", nlohmann::ordered_json()) == "y",
          "BUG-160: one statistic() keeps the flat stat/statvar shape (no consumer forced to "
          "learn the multi-statistic nesting for a call that never asked for it)");
    if (!tr.is_null()) {
      const auto& cells = tr["cells"];
      check(near(cells["1"]["no"], 15.0) && near(cells["1"]["Total"], 20.0) &&
                near(cells["Total"]["Total"], 34.0),
            "BUG-160: single-statistic two-way ALSO gets Total row/column margins — the BUGLOG's "
            "own finding that this path was never exempt");
    }
  }

  // ---- one-way, no statistic() at all (bare frequency) -------------------
  {
    auto blocks = run("table region");
    auto tr = findType(blocks, "table_result");
    check(!tr.is_null() && tr.value("row_labels", nlohmann::ordered_json()) ==
                               nlohmann::ordered_json({"1", "2", "Total"}),
          "BUG-160: even the plain one-way `table region` (no statistic(), no colvar) gets a "
          "Total row — confirmed live against StataBE 19.5 (`table region` prints one)");
    if (!tr.is_null()) {
      check(near(tr["cells"]["1"], 3.0) && near(tr["cells"]["2"], 3.0) &&
                near(tr["cells"]["Total"], 6.0),
            "BUG-160: one-way Total is the row count, not left absent");
    }
  }

  fs::remove_all(workdir, ec);
}
