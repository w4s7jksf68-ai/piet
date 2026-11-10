// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: stored estimates.
#include "selftest_support.h"
#include "selftest_cases.h"



// ---------------------------------------------------------------------------
// B4/W3b — `estimates store/save/use` and the small data verbs.
//
// Every number below is a cell of the captured StataBE 19.5 oracle
// (tests/eval/golden/aer_b4/), cited per leg. rc-only assertions are
// deliberately absent from the load-bearing legs: the whole reason BUG-470's
// probe fits a SECOND model between save and use, and the whole reason the
// `tabulate, generate()` leg asserts r(N)=69 and not "five variables
// appeared", is that rc 0 arrives for the wrong reason in both.
void testB4W3bEstimatesAndDataVerbs() {
  std::printf("[selftest] --- B4/W3b: estimates save/use + the small data verbs ---\n");
  namespace fs = std::filesystem;
  std::error_code ec;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_b4w3b").u8string();
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  const std::string autoFix = selftestValidateDir() + "/auto.hbin";
  if (!std::ifstream(cecon::to_native_path(autoFix)).good()) {
    skip("B4/W3b (needs tests/validate/auto.hbin)", "fixture not found at " + autoFix);
    fs::remove_all(workdir, ec);
    return;
  }

  auto firstError = [](const nlohmann::ordered_json& blocks) -> nlohmann::ordered_json {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return b;
    return nlohmann::ordered_json::object();
  };
  auto rcOf = [&](const nlohmann::ordered_json& blocks) -> std::string {
    nlohmann::ordered_json e = firstError(blocks);
    return e.empty() ? std::string("0") : piet::eval::coarseRc(e);
  };

  // ══ 1. estimates store, title() ══════════════════════════════════════════
  // oracle pins_oracle_g.log: `estimates store m2, title("A title")` rc 0.
  // Before this delta `title` was in kVocabEstimates's UNIMPLEMENTED set and
  // the statement was r(198).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    (void)piet::dispatch("quietly regress price mpg weight", session);
    check(rcOf(piet::dispatch("estimates store m1", session)) == "0",
          "W3b: `estimates store m1` is rc 0 (regression guard)");
    check(rcOf(piet::dispatch("estimates store m2, title(\"A title\")", session)) == "0",
          "W3b: `estimates store m2, title(\"A title\")` is rc 0 (Stata rc 0; PIET was r(198))");
    const nlohmann::ordered_json* snap = session.estimatesGet("m2");
    check(snap != nullptr && snap->contains("title") &&
              (*snap)["title"].get<std::string>() == "A title",
          "W3b: ...and the title LANDS on the stored snapshot, de-quoted — an accepted-and-"
          "dropped option is the silent-wrong this option net exists to prevent");
    check(session.estimatesGet("m1") != nullptr && !session.estimatesGet("m1")->contains("title"),
          "W3b: ...while a store WITHOUT the option carries no title key at all (absent and "
          "empty are different: `title(\"\")` from an empty macro is a real empty title)");
    // The per-sub-verb net: `replace` is `save`'s, not `store`'s.
    check(rcOf(piet::dispatch("estimates store m3, replace", session)) == "198",
          "W3b: `estimates store m3, replace` is r(198) — the net moved INSIDE the sub-verbs, so "
          "save's option is not silently accepted by store");
    // `estimates dir` is where a stored title becomes observable.
    std::string dirText;
    for (const auto& b : piet::dispatch("estimates dir", session))
      if (b.is_object() && b.value("type", std::string()) == "display")
        dirText += b.value("text", std::string());
    check(dirText.find("\"A title\"") != std::string::npos,
          "W3b: `estimates dir` prints the stored title — got " + dirText);
  }

  // ══ 2. estimates save -> use -> predict (BUG-470) ════════════════════════
  // oracle pins_oracle_g.log:283-298 — every link rc 0, and after the
  // restore e(cmd)==regress / e(depvar)==price.
  //
  // THE TRAP, and why the second regression is here: `predict` answers rc 0 in
  // any session that has run a regression, off the still-live model. A leg
  // that saved, used and predicted WITHOUT changing the model in between would
  // pass against a `use` that does nothing at all.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    (void)piet::dispatch("quietly regress price mpg weight", session);
    const std::string sterPath = (fs::path(workdir) / "w3b_est").u8string();
    check(rcOf(piet::dispatch("estimates save \"" + sterPath + "\", replace", session)) == "0",
          "W3b: `estimates save F, replace` is rc 0 (Stata B4REC|estimates_save|replace|0)");
    check(fs::exists(sterPath + ".ster"),
          "W3b: ...and the file is written with Stata's own `.ster` suffix, which the command "
          "appends when the name carries none (oracle: `file b4g_est.ster saved`)");
    check(rcOf(piet::dispatch("estimates save \"" + sterPath + "\"", session)) == "602",
          "W3b: ...a second save WITHOUT replace is the file-exists refusal r(602)");

    // A DIFFERENT model, so a no-op `use` cannot pass the legs below.
    (void)piet::dispatch("quietly regress mpg weight", session);
    check(session.lastModel().has_value() && session.lastModel()->depvar == "mpg",
          "W3b seed: the second regression really did replace e() (depvar is now mpg)");

    check(rcOf(piet::dispatch("estimates use \"" + sterPath + "\"", session)) == "0",
          "W3b: `estimates use F` is rc 0 (Stata B4REC|estimates_use|<none>|0)");
    check(session.lastModel().has_value() && session.lastModel()->depvar == "price",
          "W3b: ...and the RESTORE puts the first model back — e(depvar) is price, not the "
          "still-live mpg (oracle B4VAL|ecmd|regress|edepvar|price). This is the assertion an "
          "rc-only round trip cannot make.");
    const std::string* eDep = session.eval.eStrings.find("depvar");
    check(eDep != nullptr && *eDep == "price",
          "W3b: ...published into the e-macro store too, so `e(depvar)` and `ereturn list` "
          "agree with LastModel");
    auto predBlocks = piet::dispatch("predict double pxb, xb", session);
    check(rcOf(predBlocks) == "0",
          "W3b: `predict double pxb, xb` after the restore is rc 0 (oracle "
          "B4REC|predict|xb_after_estimates_use|0) — predict re-fits from the RESTORED "
          "LastModel, which is why the save carries it and not only the display block");
    // Stata's own N for that predict on auto is 74 — every row, no missings.
    auto sumBlocks = piet::dispatch("summarize pxb", session);
    double pxbN = -1.0;
    for (const auto& b : sumBlocks)
      if (b.is_object() && b.value("type", std::string()) == "summarize")
        pxbN = b.at("stats").at("pxb").value("N", -1.0);
    check(pxbN == 74.0,
          "W3b: ...over Stata's N 74 (oracle B4VAL|N|74) — a prediction restricted to the wrong "
          "sample would still be rc 0");

    // The FORMAT refusal is a different thing from the command gap, and stays.
    const std::string junk = (fs::path(workdir) / "notpiet.ster").u8string();
    { std::ofstream os(cecon::to_native_path(junk), std::ios::binary); os << "<stata binary>"; }
    auto badBlocks = piet::dispatch("estimates use \"" + junk + "\"", session);
    check(!firstError(badBlocks).empty() &&
              firstError(badBlocks).value("message", std::string())
                      .find("not a PIET estimates file") != std::string::npos,
          "W3b: a file PIET did not write is refused BY FORMAT, naming the limit (Stata's own "
          "binary .ster), not by pretending the command does not exist");
    check(rcOf(piet::dispatch("estimates use \"" + (fs::path(workdir) / "nope").u8string() +
                                  "\"", session)) == "601",
          "W3b: a missing file is Stata's r(601)");
  }

  // ══ 3. tabulate VAR, generate(stub) — THE MISSING RULE ═══════════════════
  // oracle pins_oracle_g.log:158-181 on auto: rs1..rs5, and
  //   summarize rs1, meanonly -> r(N) 69  (NOT 74)
  //   r(mean) = +1.dae6076b981dbX-006 = 2/69
  // auto.hbin has 5 missing rep78 rows, so 74 - 5 = 69.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto tg = piet::dispatch("tabulate rep78, generate(rs)", session);
    check(rcOf(tg) == "0", "W3b: `tabulate rep78, generate(rs)` is rc 0 (Stata rc 0; PIET r(198))");

    auto describeOf = [&](const std::string& v) -> nlohmann::ordered_json {
      for (const auto& b : piet::dispatch("describe " + v, session)) {
        if (b.is_object() && b.value("type", std::string()) == "describe" &&
            b.contains("variables") && !b["variables"].empty()) {
          return b["variables"][0];
        }
      }
      return nlohmann::ordered_json::object();
    };
    for (int k = 1; k <= 5; ++k) {
      check(!describeOf("rs" + std::to_string(k)).empty(),
            "W3b: rs" + std::to_string(k) + " exists (5 levels of rep78 -> 5 indicators)");
    }
    check(describeOf("rs6").empty(), "W3b: ...and rs6 does NOT — the level list is the table's");

    auto sumBlocks = piet::dispatch("summarize rs1", session);
    double n = -1.0, mean = -1.0;
    for (const auto& b : sumBlocks)
      if (b.is_object() && b.value("type", std::string()) == "summarize") {
        n = b.at("stats").at("rs1").value("N", -1.0);
        mean = b.at("stats").at("rs1").value("mean", -1.0);
      }
    // THE SEEDED-RED SEED, stated as a number. A 0-where-missing implementation
    // — the one every count test and every "did the variables appear" check
    // passes — answers N 74 and mean 2/74. Both are asserted against.
    check(n == 69.0,
          "W3b MISSING RULE: `summarize rs1` is Stata's r(N) 69, NOT 74 — the dummies are "
          "MISSING where rep78 is missing, not 0 (oracle B4VAL|rs1_N|69). A 0-fill answers 74 "
          "here and moves every regression that uses one of these columns; got " +
              std::to_string(n));
    check(std::fabs(mean - 2.0 / 69.0) <= 1e-15 * std::fabs(2.0 / 69.0),
          "W3b MISSING RULE: ...and r(mean) is 2/69 (oracle %21x +1.dae6076b981dbX-006), not "
          "the 2/74 = " + std::to_string(2.0 / 74.0) + " a 0-fill gives; got " +
              std::to_string(mean));
    // The STORED label is `rep78==` + the level under %11.4f (five blanks before
    // 1.0000): the byte-exact `local l : variable label` read-backs of
    // w2as_summary_stats_family.log:3039 (`z==     3.0000`, an int) and :3995
    // (`fw==     1.0000`, a byte). pins_shapes.log:39, which this pin used to
    // read as `rep78== 1.0000`, is `describe`'s rendered table, and that table
    // collapses a run of blanks inside a label; it is not the label's bytes.
    check(describeOf("rs1").value("label", std::string()) == "rep78==     1.0000",
          "W3b: the variable LABEL is Stata's stored spelling `rep78==     1.0000` — the level "
          "under %11.4f (w2as_summary_stats_family.log:3039, :3995); got '" +
              describeOf("rs1").value("label", std::string()) + "'");
    check(describeOf("rs1").value("dtype", std::string()) == "byte",
          "W3b: ...stored as byte, as Stata stores it");
    // Stata's own guard, and it fires before ANY column is written.
    check(rcOf(piet::dispatch("tabulate rep78, generate(rs)", session)) == "110",
          "W3b: a second run collides at r(110) `variable rs1 already defined`");
    check(rcOf(piet::dispatch("tabulate rep78 foreign, generate(xx)", session)) == "198",
          "W3b: generate() on a TWO-WAY tabulate is refused by name — a cross-tabulation has no "
          "single level list to make indicators from");
  }

  // ══ 3b. tabulate display options: sort / cell / col / row / nofreq ═══════
  // oracle tests/eval/golden/aer_b4/b5_importcase_pins.log:99-243 — all five
  // are rc 0 (they were r(198) before this delta). `sort` is the one with a
  // cheap, IN-SCOPE, OBSERVABLE effect (row order); `cell`/`col`/`row`/
  // `nofreq` change only the RENDERED table, which this backend does not
  // compute (no renderer here — see kVocabTabulate's own comment) — so those
  // four are asserted rc-0-only, matching exactly what the oracle itself
  // measures for them.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);

    auto tableRowsOf = [](const nlohmann::ordered_json& blocks) -> std::vector<std::string> {
      std::vector<std::string> out;
      for (const auto& b : blocks) {
        if (b.is_object() && b.value("type", std::string()) == "tabulate" && b.contains("table") &&
            b["table"].is_object() && b["table"].contains("rows")) {
          for (const auto& r : b["table"]["rows"])
            if (r.is_string()) out.push_back(r.get<std::string>());
        }
      }
      return out;
    };

    // Unsorted control: Stata's default is ascending category value.
    auto plainBlocks = piet::dispatch("tabulate rep78", session);
    check(rcOf(plainBlocks) == "0", "B5-D-TAB-SORT: plain `tabulate rep78` is rc 0 (control)");
    std::vector<std::string> plainRows = tableRowsOf(plainBlocks);
    const std::vector<std::string> expectAscending = {"1.0", "2.0", "3.0", "4.0", "5.0"};
    check(plainRows == expectAscending,
          "B5-D-TAB-SORT control: unsorted `tabulate rep78` rows are ascending category value");

    auto sortBlocks = piet::dispatch("tabulate rep78, sort", session);
    check(rcOf(sortBlocks) == "0",
          "B5-D-TAB-SORT: `tabulate rep78, sort` is rc 0 (Stata rc 0; PIET was r(198))");
    // Measured freq 30/18/11/8/2 at levels 3/4/5/2/1 (b5_importcase_pins.log:
    // 103-117) -- descending frequency, NOT the ascending-value control above.
    const std::vector<std::string> expectByFreq = {"3.0", "4.0", "5.0", "2.0", "1.0"};
    check(tableRowsOf(sortBlocks) == expectByFreq,
          "B5-D-TAB-SORT: `, sort` reorders rows by DESCENDING frequency (30/18/11/8/2), not "
          "ascending category value — this is the row-order effect the oracle's rc-only cell "
          "cannot see but the JSON `table.rows` this file builds can");
    // `sort` must not change WHAT is counted, only the order it's presented in.
    for (const auto& r : plainRows)
      check(std::find(expectByFreq.begin(), expectByFreq.end(), r) != expectByFreq.end(),
            "B5-D-TAB-SORT: sort is a reorder, not a re-grouping — every unsorted row survives");

    check(rcOf(piet::dispatch("tabulate rep78 foreign, cell", session)) == "0",
          "B5-D-TAB-DISPLAY: `tabulate rep78 foreign, cell` is rc 0 (Stata rc 0; PIET was "
          "r(198)) -- percentage-line rendering is UNMEASURED and not claimed here");
    check(rcOf(piet::dispatch("tabulate rep78 foreign, col", session)) == "0",
          "B5-D-TAB-DISPLAY: `tabulate rep78 foreign, col` is rc 0 -- same UNMEASURED-rendering "
          "note as `cell`");
    check(rcOf(piet::dispatch("tabulate rep78 foreign, row", session)) == "0",
          "B5-D-TAB-DISPLAY: `tabulate rep78 foreign, row` is rc 0 -- same UNMEASURED-rendering "
          "note as `cell`");
    check(rcOf(piet::dispatch("tabulate rep78 foreign, nofreq", session)) == "0",
          "B5-D-TAB-DISPLAY: `tabulate rep78 foreign, nofreq` is rc 0 -- suppressing the "
          "frequency line is also UNMEASURED rendering, not implemented here");
    // A real unknown spelling still refuses — the four validated no-ops above
    // did not widen the net into a blanket accept.
    check(rcOf(piet::dispatch("tabulate rep78 foreign, bogusdisplayopt", session)) == "198",
          "B5-D-TAB-DISPLAY: an unrecognized tabulate option is still r(198) after this delta");
  }

  // ══ 4. tabstat ..., by() nototal save ════════════════════════════════════
  // oracle pins_oracle_g.log:189-220 on auto:
  //   r(Stat1)[3,2] / r(Stat2)[3,2], rows Mean/SD/N, cols the varlist
  //   r(Stat1)[1,1] = 6072.423     (%21x +1.7b86c4ec4ec4fX+00c)
  //   r(Stat1)[3,1] = 52           (%21x +1.a000000000000X+005)
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto ts = piet::dispatch("tabstat price mpg, by(foreign) stat(mean sd n) nototal save",
                             session);
    check(rcOf(ts) == "0",
          "W3b: `tabstat price mpg, by(foreign) stat(mean sd n) nototal save` is rc 0 (Stata "
          "B4REC|tabstat|by_nototal_save|0; PIET r(198) on nototal)");
    const nlohmann::ordered_json* tsBlock = nullptr;
    for (const auto& b : ts)
      if (b.is_object() && b.value("type", std::string()) == "estimation") tsBlock = &b;
    check(tsBlock != nullptr && !tsBlock->contains("total"),
          "W3b: `nototal` really removes the overall row — accepting it and printing the total "
          "anyway is the accepted-and-ignored class");
    check(tsBlock != nullptr && tsBlock->contains("groups") &&
              (*tsBlock)["groups"].size() == 2,
          "W3b: ...and the two by-groups are CARRIED (the engine has always emitted them; "
          "nothing on this side read the key)");
    const piet::eval::EvalMatrix* s1 = session.eval.findClassMatrix('r', "Stat1");
    const piet::eval::EvalMatrix* s2 = session.eval.findClassMatrix('r', "Stat2");
    check(s1 != nullptr && s2 != nullptr, "W3b: `save` posts r(Stat1) and r(Stat2)");
    check(s1 != nullptr && s1->data.size() == 3 && s1->data[0].size() == 2,
          "W3b: r(Stat1) is [3,2] — stats down, varlist across (oracle B4VAL|s1_rows|3|s1_cols|2)");
    check(s1 != nullptr && s1->rownames == std::vector<std::string>{"Mean", "SD", "N"},
          "W3b: ...with Stata's own row stubs Mean/SD/N in the order asked for");
    check(s1 != nullptr && s1->colnames == std::vector<std::string>{"price", "mpg"},
          "W3b: ...and the columns are the VARLIST in the order written");
    // Bit-exact: a group mean over the fixture's doubles is not an estimate.
    check(s1 != nullptr && std::fabs(s1->data[0][0] - 6072.423076923077) <=
                               1e-12 * 6072.423076923077,
          "W3b: r(Stat1)[1,1] is Stata's Domestic mean price 6072.423 (oracle %21x "
          "+1.7b86c4ec4ec4fX+00c)");
    check(s1 != nullptr && s1->data[2][0] == 52.0,
          "W3b: r(Stat1)[3,1] is the group N 52 (oracle %21x +1.a000000000000X+005) — the "
          "GROUP's N, not the dataset's 74");
    check(s2 != nullptr && s2->data[2][0] == 22.0,
          "W3b: r(Stat2)[3,1] is the Foreign group's N 22");
    const std::string* nm1 = session.eval.rStrings.find("name1");
    check(nm1 != nullptr && *nm1 == "0",
          "W3b: r(name1) is the group's name. On THIS fixture `foreign` carries no value label, "
          "so it is the code `0`; Stata's own log reads `Domestic` because auto.dta ships the "
          "label — the labelled path is exercised by the leg below; got '" +
              (nm1 ? *nm1 : std::string("<unset>")) + "'");
    // The labelled path, on a label this session defines itself.
    (void)piet::dispatch("label define orig 0 \"Domestic\" 1 \"Foreign\"", session);
    (void)piet::dispatch("label values foreign orig", session);
    (void)piet::dispatch("tabstat price mpg, by(foreign) stat(mean sd n) nototal save", session);
    const std::string* nm1b = session.eval.rStrings.find("name1");
    check(nm1b != nullptr && *nm1b == "Domestic",
          "W3b: ...and with the value label attached r(name1) is `Domestic`, matching the "
          "oracle's B4VAL|name1|Domestic exactly");
  }

  // ══ 5. matrix NAME = r(MATRIX) — the wiring gap `save` walked into ═══════
  // handlers_m3.cpp:1978 has always known how to copy a posted r-class matrix,
  // and the macro layer ate the reference before it could: `matrix T =
  // r(table)` (sktest's own matrix) died as "r(table) is not set".
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    (void)piet::dispatch("tabstat price mpg, by(foreign) stat(mean sd n) nototal save", session);
    check(rcOf(piet::dispatch("matrix S1 = r(Stat1)", session)) == "0",
          "W3b: `matrix S1 = r(Stat1)` is rc 0 — an r-class matrix reference now survives the "
          "macro layer on a MATRIX verb");
    const piet::eval::EvalMatrix* copied = session.eval.findMatrix("S1");
    check(copied != nullptr && copied->data.size() == 3 && copied->data[2][0] == 52.0,
          "W3b: ...and the copy carries the values, not a name");
    // The exemption reaches the matrix verbs and the matrix OPERAND spellings
    // (r(NAME)[i,j], rowsof/colsof, el(); BUG-629), so a bare matrix in scalar
    // context is still refused. BUG-629 flipped the message from the untrue "is not
    // set" to r(109) "type mismatch" (design decision, no oracle arm).
    auto disp = piet::dispatch("display r(Stat1)", session);
    check(!firstError(disp).empty() && rcOf(disp) == "109",
          "W3b: `display r(Stat1)` keeps a refusal, at r(109) -- the exemption reaches the "
          "matrix verbs and matrix operands only");
  }

  // ══ 6. expand N, generate() ══════════════════════════════════════════════
  // oracle pins_oracle_g.log:227-233: _N 74 -> 148, dupflag[1] == 0.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto ex = piet::dispatch("expand 2, generate(dupflag)", session);
    check(rcOf(ex) == "0",
          "W3b: `expand 2, generate(dupflag)` is rc 0 (Stata B4REC|expand|generate|0)");
    auto sumBlocks = piet::dispatch("summarize dupflag", session);
    double n = -1.0, mean = -1.0;
    for (const auto& b : sumBlocks)
      if (b.is_object() && b.value("type", std::string()) == "summarize") {
        n = b.at("stats").at("dupflag").value("N", -1.0);
        mean = b.at("stats").at("dupflag").value("mean", -1.0);
      }
    check(n == 148.0, "W3b: ...over Stata's _N 148 (oracle B4VAL|rows|148)");
    check(mean == 0.5,
          "W3b: ...and the marker is 0 for the 74 originals and 1 for the 74 copies — mean "
          "exactly .5 (oracle `tabulate dupflag`: 74 Original / 74 Duplicated). A marker that "
          "is 1 everywhere, or 0 everywhere, is also rc 0.");
    auto listBlocks = piet::dispatch("list dupflag in 1/1", session);
    std::string first;
    for (const auto& b : listBlocks)
      if (b.is_object() && b.value("type", std::string()) == "browse") {
        const auto& rows = b.value("rows", nlohmann::ordered_json::array());
        if (!rows.empty() && !rows[0].empty()) first = rows[0][0].dump();
      }
    check(first.find('0') != std::string::npos,
          "W3b: ...and observation 1 is an ORIGINAL (oracle B4VAL|dup1|0) — the copies are "
          "appended, so the 0s come first; got " + first);
  }

  // ══ 7. rename _all, lower / upper ════════════════════════════════════════
  // oracle pins_oracle_g.log:101-119: rc 0, and a case collision is r(110)
  // "variable x2 already defined".
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto noop = piet::dispatch("rename _all, lower", session);
    check(rcOf(noop) == "0",
          "W3b: `rename _all, lower` is rc 0 on an already-lowercase dataset (Stata "
          "B4REC|rename|_all_lower|0)");
    std::string noopText;
    for (const auto& b : noop)
      if (b.is_object() && b.value("type", std::string()) == "display")
        noopText += b.value("text", std::string());
    check(noopText.find("all newnames==oldnames") != std::string::npos,
          "W3b: ...with Stata's own note when nothing moves — got " + noopText);
    check(rcOf(piet::dispatch("rename _all, upper", session)) == "0",
          "W3b: `rename _all, upper` is rc 0");
    // The names really moved. `PRICE` must exist and `price` must not.
    auto dsBlocks = piet::dispatch("describe PRICE", session);
    check(firstError(dsBlocks).empty(),
          "W3b: ...and PRICE exists afterwards — an accepted-and-ignored case option is rc 0 "
          "too");
    // Asserted at the DATA plane, not through `describe`: this build's
    // `describe` resolves a name case-insensitively, so `describe price`
    // answers rc 0 for the column now called PRICE and would pass whether or
    // not anything moved. `gen` goes to the engine's own column table, which
    // holds the real spelling.
    check(!firstError(piet::dispatch("gen zlow = price", session)).empty(),
          "W3b: ...while the old lowercase name is gone from the DATA — `gen zlow = price` no "
          "longer resolves (an accepted-and-ignored case option passes a describe-based check)");
    check(firstError(piet::dispatch("gen zup = PRICE", session)).empty(),
          "W3b: ...and the uppercase one does");
    check(rcOf(piet::dispatch("rename *, lower", session)) == "0",
          "W3b: the `*` spelling of the same form works (it is not the PATTERN form the "
          "wildcard reject is about)");
    // The collision, on the group form.
    (void)piet::dispatch("gen X = 1", session);
    (void)piet::dispatch("gen x2 = 2", session);
    (void)piet::dispatch("rename X x9", session);
    (void)piet::dispatch("rename x9 X", session);
    (void)piet::dispatch("gen x = 3", session);
    // Two names of the list mapping onto one new name is rename.ado's r(198)
    // "new name specified repeatedly" (aer_w2 w2_data_io.log:846-848 ren7
    // `rename GH gh, lower`); r(110) is a new name held by a variable OUTSIDE
    // the list (ren3 `rename gh, upper` over GH, :836-838; the 1:1 B4REC
    // collision, pins_oracle_g.log:113-119).
    check(rcOf(piet::dispatch("rename _all, lower", session)) == "198",
          "W3b: `X` and `x` both mapping to `x` is rename's r(198) `new name specified "
          "repeatedly` (w2_data_io.log ren7) — and it fires BEFORE any column moves");
    check(!firstError(piet::dispatch("describe X", session)).empty() == false,
          "W3b: ...so `X` is still there, un-recased");
  }

  // ══ 8. tsset t, quarterly ════════════════════════════════════════════════
  // oracle pins_oracle_g.log:257-273: rc 0, r(tsfmt) "%tq", and `describe
  // t` shows the format ON THE VARIABLE.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 8", session);
    (void)piet::dispatch("gen t = _n", session);
    check(rcOf(piet::dispatch("tsset t, quarterly", session)) == "0",
          "W3b: `tsset t, quarterly` is rc 0 (Stata B4REC|tsset|quarterly|0; PIET r(198))");
    const std::string* fmt = session.eval.rStrings.find("tsfmt");
    check(fmt != nullptr && *fmt == "%tq",
          "W3b: ...r(tsfmt) is %tq, not the no-period default %tg (oracle B4VAL|tsfmt|%tq)");
    std::string varFmt;
    for (const auto& b : piet::dispatch("describe t", session))
      if (b.is_object() && b.value("type", std::string()) == "describe" && b.contains("variables") &&
          !b["variables"].empty())
        varFmt = b["variables"][0].value("format", std::string());
    check(varFmt == "%tq",
          "W3b: ...and the period formats the COLUMN too — `describe t` reads %tq (oracle's own "
          "describe block). An r()-only implementation passes every r() test and leaves the "
          "variable displaying as a plain count; got " + varFmt);
    check(rcOf(piet::dispatch("tsset t, monthly quarterly", session)) != "0",
          "W3b: two periods at once is refused rather than silently picking one");
  }

  // ══ 9b. merge ..., update [replace] / generate() ═════════════════════════
  // The PIET side of the engine rider that landed on main 3a5d80e2. Oracle
  // pins_oracle_i.log — a fixture built WITHOUT a second overlapping
  // variable, because batch H's `tag` column put every matched row in conflict
  // and hid the code assignment:
  //   master id 1..6, x = 10 . 30 . 50 60
  //   using  id 1..5,7, x = 99 20 30 40 55 70
  //   merge 1:1 id using ..., update ->
  //     x = 10 20 30 40 50 60 70,  codes 5 4 3 4 5 1 2  (N 7)
  //   ...update replace -> x[1] = 99, x[5] = 55, code distribution UNCHANGED
  // The last clause is the discriminator: `replace` moves VALUES, not codes, so
  // a leg that only counted _merge would pass for an implementation that
  // ignored it.
  {
    piet::SessionState session(workdir);
    const std::string masterP = (fs::path(workdir) / "b4i_master.hbin").u8string();
    const std::string usingP = (fs::path(workdir) / "b4i_using.hbin").u8string();
    for (int side = 0; side < 2; ++side) {
      (void)piet::dispatch("clear", session);
      (void)piet::dispatch("set obs 6", session);
      (void)piet::dispatch("gen id = _n", session);
      if (side == 1) (void)piet::dispatch("replace id = 7 in 6", session);
      (void)piet::dispatch("gen double x = .", session);
      if (side == 0) {
        (void)piet::dispatch("replace x = 10 in 1", session);
        (void)piet::dispatch("replace x = 30 in 3", session);
        (void)piet::dispatch("replace x = 50 in 5", session);
        (void)piet::dispatch("replace x = 60 in 6", session);
      } else {
        (void)piet::dispatch("replace x = 99 in 1", session);
        (void)piet::dispatch("replace x = 20 in 2", session);
        (void)piet::dispatch("replace x = 30 in 3", session);
        (void)piet::dispatch("replace x = 40 in 4", session);
        (void)piet::dispatch("replace x = 55 in 5", session);
        (void)piet::dispatch("replace x = 70 in 6", session);
      }
      (void)piet::dispatch("save \"" + (side == 0 ? masterP : usingP) + "\", replace", session);
    }
    auto countIf = [&](const std::string& cond) -> double {
      auto blocks = piet::dispatch("count if " + cond, session);
      for (const auto& b : blocks)
        if (b.is_object() && b.value("type", std::string()) == "display")
          return std::atof(b.value("text", std::string("-1")).c_str());
      return -1.0;
    };
    auto cellOf = [&](const std::string& var, int obs) -> double {
      auto blocks = piet::dispatch("display " + var + "[" + std::to_string(obs) + "]", session);
      for (const auto& b : blocks)
        if (b.is_object() && b.value("type", std::string()) == "display")
          return std::atof(b.value("text", std::string("nan")).c_str());
      return std::numeric_limits<double>::quiet_NaN();
    };

    (void)piet::dispatch("use \"" + masterP + "\", clear", session);
    check(rcOf(piet::dispatch("merge 1:1 id using \"" + usingP + "\", update", session)) == "0",
          "W3b: `merge 1:1 id using F, update` is rc 0 (Stata B4REC|merge|update_clean|0; PIET "
          "had `update` in the UNIMPLEMENTED set)");
    check(countIf("_merge==1") == 1.0 && countIf("_merge==2") == 1.0 &&
              countIf("_merge==3") == 1.0 && countIf("_merge==4") == 2.0 &&
              countIf("_merge==5") == 2.0,
          "W3b: ...and the five marker codes are Stata's 1/1/1/2/2 — codes 4 (missing updated) "
          "and 5 (nonmissing conflict) are what `update` ADDS, and they are assigned per ROW "
          "over all overlapping variables");
    check(cellOf("x", 2) == 20.0 && cellOf("x", 4) == 40.0,
          "W3b: ...a code-4 row takes the using's value (x[2]=20, x[4]=40; oracle %21x "
          "+1.4000000000000X+004 / +1.4000000000000X+005)");
    check(cellOf("x", 1) == 10.0 && cellOf("x", 5) == 50.0,
          "W3b: ...and a code-5 CONFLICT keeps the MASTER's (x[1]=10 not 99, x[5]=50 not 55). "
          "This is the half `update replace` inverts, and an rc-only leg cannot see it.");

    (void)piet::dispatch("use \"" + masterP + "\", clear", session);
    check(rcOf(piet::dispatch("merge 1:1 id using \"" + usingP + "\", update replace",
                              session)) == "0",
          "W3b: `, update replace` is rc 0 (Stata B4REC|merge|update_replace_clean|0)");
    check(cellOf("x", 1) == 99.0 && cellOf("x", 5) == 55.0,
          "W3b: ...and NOW the conflict rows take the using's value (x[1]=99, x[5]=55; oracle "
          "%21x +1.8c00000000000X+006 / +1.b800000000000X+005)");
    check(countIf("_merge==4") == 2.0 && countIf("_merge==5") == 2.0,
          "W3b: ...while the CODE distribution is unchanged — `replace` moves values, not "
          "codes (oracle: the same 4/5 counts under both spellings)");

    (void)piet::dispatch("use \"" + masterP + "\", clear", session);
    check(rcOf(piet::dispatch("merge 1:1 id using \"" + usingP + "\", generate(mm)",
                              session)) == "0",
          "W3b: `merge ..., generate(mm)` is rc 0 (38 corpus uses)");
    check(countIf("mm==3") == 5.0,
          "W3b: ...the marker lives under the NAME asked for (5 matched rows; oracle "
          "B4VAL|mm3|5)");
    check(!firstError(piet::dispatch("count if _merge==1", session)).empty(),
          "W3b: ...and `_merge` does NOT also exist — generate() MOVES the marker, it does not "
          "add a second one (Stata answers r(111) here; oracle "
          "B4REC|merge|generate_suppresses__merge|111)");

    (void)piet::dispatch("use \"" + masterP + "\", clear", session);
    check(!firstError(piet::dispatch("merge 1:1 id using \"" + usingP + "\", replace", session))
               .empty(),
          "W3b: `replace` WITHOUT `update` is refused — on its own it modifies nothing, and an "
          "ignored `replace` would keep every master value at rc 0");
    check(!firstError(piet::dispatch(
                          "merge 1:1 id using \"" + usingP + "\", generate(mm) nogenerate",
                          session))
               .empty(),
          "W3b: ...and generate() with nogenerate is refused as contradictory");
  }

  // ══ 9. duplicates drop VARLIST, force ════════════════════════════════════
  // oracle pins_oracle_g.log:250-256: rc 0, _N 74 -> 2.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    check(rcOf(piet::dispatch("duplicates drop foreign, force", session)) == "0",
          "W3b: `duplicates drop foreign, force` is rc 0 (Stata B4REC|duplicates_drop|force|0; "
          "PIET r(198))");
    auto cnt = piet::dispatch("count", session);
    double n = -1.0;
    for (const auto& b : cnt)
      if (b.is_object() && b.value("type", std::string()) == "display")
        n = std::atof(b.value("text", std::string("-1")).c_str());
    check(n == 2.0,
          "W3b: ...and _N is Stata's 2 (oracle B4VAL|rows|2) — `force` is an ACKNOWLEDGEMENT, "
          "so accepting it must not change which rows survive; got " + std::to_string(n));
  }

  // ══ 10. `margins, dydx(x) post` (item 4, User ruling 2026-09-09) ═════════
  // Cells `B4REC|margins|dydx_post|0|`, `B4VAL|ecmd|margins|b_mpg|
  // -1.8c19072612ae4X+005` (tests/eval/golden/aer_b4/pins_oracle_r.log)
  // plus the in-repo receipt tests/validate/glm_golden.do:20,29,34
  // (`margins, dydx(*) post` then a `_b[]` read; glm_golden.log:60,77 pairs
  // them). `post` REPLACES e() -- e(cmd)=="margins" and `_b[x]` becomes the
  // margin, not the stale model coefficient.
  //
  // NOTE ON THE FIXTURE: the oracle's own scenario fits `regress` first.
  // Deliberately NOT reproduced here -- `margins, dydx(x)` after a plain
  // OLS/feols model is a PRE-EXISTING, UNRELATED honest reject
  // (estimation_m4c.cpp's `kLinearCmds` set, "marginal effects (dydx) after
  // a linear model are not supported"), and it must stay that way: cobra's
  // own `margins` AME path (apps/cmds/cmds_postest.cpp:929,
  // `opt(a, "--model", "logit")`) has NO plain-OLS branch and DEFAULTS TO
  // LOGIT when no --model is sent -- widening kLinearCmds to admit feols/
  // regress would silently compute a logistic AME for a continuous
  // outcome, a wrong-number regression strictly worse than the reject it
  // would replace. Reproducing the oracle's exact `regress`+`dydx`+`post`
  // cell needs a genuine linear-AME code path (dydx of an OLS coefficient
  // IS the coefficient, computable directly from the already-fitted model
  // without an engine call at all) -- that is new construction beyond this
  // item's `post`-wiring scope; left OPEN, not silently routed through the
  // wrong model family. `logit` below already has a working, oracle-
  // matching dydx path (freeze/margins.json's own `logit female x1 x2` +
  // `margins, dydx(x1)` shape) and is what exercises `post` itself.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    (void)piet::dispatch("quietly logit foreign mpg weight", session);
    std::string plainTxt;
    for (const auto& b : piet::dispatch("display _b[mpg]", session))
      if (b.is_object() && b.value("type", std::string()) == "display")
        plainTxt = b.value("text", std::string());
    // Regression guard: NO `post` leaves e()/the model untouched (this is
    // freeze/margins.json's own shape and must stay byte-identical).
    check(rcOf(piet::dispatch("margins, dydx(mpg)", session)) == "0",
          "B4/item4 regression guard: `margins, dydx(mpg)` (no post) still runs");
    check(session.eval.eStrings.get("cmd", "") == "logit",
          "B4/item4 regression guard: ...and e(cmd) is UNCHANGED (\"logit\") without `post`");
    // The assertion.
    auto postBlocks = piet::dispatch("margins, dydx(mpg) post", session);
    check(rcOf(postBlocks) == "0",
          "B4/item4: `margins, dydx(mpg) post` is rc 0 (was r(198): `post` was `unsupported_"
          "option`)");
    check(session.eval.eStrings.get("cmd", "") == "margins",
          "B4/item4: ...and e(cmd) is now \"margins\" -- got \"" +
              session.eval.eStrings.get("cmd", "") + "\"");
    const auto* margEst = firstBlockOfTypeD1(postBlocks, "estimation");
    check(margEst != nullptr, "B4/item4: `margins, dydx(mpg), post` returns an estimation block");
    auto marginB = margEst != nullptr ? coefFieldD1(*margEst, "mpg", "b") : std::nullopt;
    check(marginB.has_value(), "B4/item4: ...whose coeftable has an `mpg` row with `b`");
    std::string postTxt;
    for (const auto& b : piet::dispatch("display _b[mpg]", session))
      if (b.is_object() && b.value("type", std::string()) == "display")
        postTxt = b.value("text", std::string());
    double postVal = std::atof(postTxt.c_str());
    check(marginB.has_value() && std::abs(postVal - *marginB) < 1e-6,
          "B4/item4: ...and `_b[mpg]` (the macro-layer read, macros.cpp's resolveBRef) agrees "
          "with the block's own coeftable `b` (the two data paths "
          "[[piet-shadow-evaluator-dual-layer]] must not disagree)");
    check(!plainTxt.empty() && postTxt != plainTxt,
          "B4/item4 value discriminator: `_b[mpg]` after `post` is the MARGIN (a small AME), "
          "not the stale logit coefficient (plain was " + plainTxt + ", post is " + postTxt +
              ")");
  }

  // ══ 11. `nlcom (r: _b[a]/_b[b]), post` (item 5, User ruling 2026-09-09) ══
  // Cells `B4REC|nlcom|post|0|`, `B4VAL|ecmd|nlcom|b_r|
  // -1.c59331b6f3b44X+004`. Also exercises defect (a): the parenthesised
  // `(name: expr)` form is broken INDEPENDENTLY of `post` (estimation_
  // m4c.cpp's runNlcom label regex `^(\w+)\s*:\s*(.+)$` cannot match a
  // leading `(`, since `(` is not `\w`).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    (void)piet::dispatch("quietly regress price mpg weight", session);
    // Control: the UNPARENTHESISED labelled form already worked -- proves
    // the paren-strip below is additive, not a rewrite of working grammar.
    check(rcOf(piet::dispatch("nlcom r: _b[mpg]/_b[weight]", session)) == "0",
          "B4/item5 control: `nlcom r: _b[mpg]/_b[weight]` (no parens) still runs");
    // Defect (a), no post: the parenthesised form alone.
    auto noPostBlocks = piet::dispatch("nlcom (r: _b[mpg]/_b[weight])", session);
    check(rcOf(noPostBlocks) == "0",
          "B4/item5 defect(a): `nlcom (r: _b[mpg]/_b[weight])` is rc 0 -- PIET was rc 1, "
          "\"bare identifier 'r'\" (the whole parenthesised string used to ship as the "
          "expression body)");
    const auto* nc = firstBlockOfTypeD1(noPostBlocks, "estimation");
    std::string ncName = (nc != nullptr && nc->contains("coeftable") &&
                          (*nc)["coeftable"].is_array() && !(*nc)["coeftable"].empty())
                             ? (*nc)["coeftable"][0].value("name", std::string())
                             : std::string();
    check(ncName == "r",
          "B4/item5 defect(a): ...and the coeftable row is named `r` (the label was actually "
          "parsed, not folded into the expression body) -- got \"" + ncName + "\"");
    // Defect (b): `post`.
    auto postBlocks = piet::dispatch("nlcom (r: _b[mpg]/_b[weight]), post", session);
    check(rcOf(postBlocks) == "0",
          "B4/item5: `nlcom (r: _b[mpg]/_b[weight]), post` is rc 0 (Stata B4REC|nlcom|post|0|; "
          "PIET was r(198))");
    check(session.eval.eStrings.get("cmd", "") == "nlcom",
          "B4/item5: ...and e(cmd) is now \"nlcom\" -- got \"" +
              session.eval.eStrings.get("cmd", "") + "\"");
    const auto* ncEst = firstBlockOfTypeD1(postBlocks, "estimation");
    auto rB = ncEst != nullptr ? coefFieldD1(*ncEst, "r", "b") : std::nullopt;
    check(rB.has_value(), "B4/item5: ...whose coeftable has an `r` row with `b`");
    std::string rTxt;
    for (const auto& b : piet::dispatch("display _b[r]", session))
      if (b.is_object() && b.value("type", std::string()) == "display")
        rTxt = b.value("text", std::string());
    double rVal = std::atof(rTxt.c_str());
    check(rB.has_value() && std::abs(rVal - *rB) < 1e-6,
          "B4/item5: ...and `_b[r]` agrees with the block's own coeftable `b`");
    // Over-rejection guard: nlcom is BOTH r-class (always) and, under
    // `post`, ALSO e-class -- dispatcher.cpp's captureRFromBlock (Branch 2)
    // still runs on the returned block regardless of postOpt.
    // StataBE 19.5 (checked 2026-10-22, auto: `nlcom (r: _b[mpg]/_b[weight]), post`
    // then `return list`): r(level) 95, r(N) 74, r(table) r(b) r(V); no
    // r(estimate) -- that one is lincom's.
    check(session.eval.rValues.find("N") != nullptr &&
              session.eval.rValues.find("estimate") == nullptr,
          "B4/item5: after `post` nlcom's r-class half is still posted (r(N)) and carries no "
          "r(estimate)");
  }

  // ══ 12. `kdensity v, generate(kx ky) nodraw` (item 6, 2026-09-09) ════════
  // Cells `B4REC|kdensity|generate_nodraw|0|`, `B4VAL|kx_N|50|kx_mean|
  // +1.2bf4000000001X+00d` (= 9598.500000000002 decimal) — the ONLY in-repo
  // Stata capture of kdensity's grid size under generate(); NOT a general
  // claim about Stata's default n (graph_m4.cpp's own comment says so —
  // do not read this leg as validating min(N,50) beyond this one fixture).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto blocks = piet::dispatch("kdensity price, generate(kx ky) nodraw", session);
    check(rcOf(blocks) == "0",
          "B4/item6: `kdensity price, generate(kx ky) nodraw` is rc 0 (Stata "
          "B4REC|kdensity|generate_nodraw|0|; PIET was r(198))");
    check(session.hasData() && session.datastore()->has("kx") && session.datastore()->has("ky"),
          "B4/item6: ...and both `kx`/`ky` columns exist");
    (void)piet::dispatch("summarize kx, meanonly", session);
    const double* kxN = session.eval.rValues.find("N");
    const double* kxMean = session.eval.rValues.find("mean");
    check(kxN != nullptr && *kxN == 50.0,
          "B4/item6: r(N) for kx is 50 (= min(74, 50) on this 74-obs fixture; oracle "
          "B4VAL|kx_N|50) -- got " + (kxN != nullptr ? std::to_string(*kxN) : std::string("MISSING")));
    check(kxMean != nullptr && std::abs(*kxMean - 9598.500000000002) < 1e-6,
          "B4/item6 value discriminator: mean(kx) matches the Stata oracle 9598.500000000002 "
          "(B4VAL|kx_mean|+1.2bf4000000001X+00d) -- an arbitrary 50-number fill would pass the "
          "N==50 check above and fail this one -- got " +
              (kxMean != nullptr ? std::to_string(*kxMean) : std::string("MISSING")));
    // Missing rule: rows beyond the grid are MISSING, never 0 (the same
    // lesson W3b's tabulate,generate() established).
    std::string missTxt;
    for (const auto& b : piet::dispatch("count if missing(kx)", session))
      if (b.is_object() && b.value("type", std::string()) == "display")
        missTxt = b.value("text", std::string());
    check(missTxt == "24",
          "B4/item6 missing rule: rows 51..74 (74-50=24 on this fixture) are MISSING for kx, "
          "not zero-filled -- got `" + missTxt + "`");
  }
  // Separate session: bare `kdensity y` (no generate()) draws kdensity.ado's
  // default grid, n(50) -- the 512-point grid this leg used to pin was not
  // Stata's (gfx-histogram-kdensity-family: w2aq_histogram_family hk_kd_bare_r
  // r(n) = +1.9000000000000X+005 = 50; its serset holds those 50 rows and one
  // missing row).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto bareBlocks = piet::dispatch("kdensity price, nodraw", session);
    const auto* bareGraph = firstBlockOfTypeD1(bareBlocks, "graph");
    check(bareGraph != nullptr && bareGraph->value("n_grid", -1) == 50,
          "B4/item6: bare `kdensity price, nodraw` (no generate()) draws the n(50) default grid");
  }

  fs::remove_all(workdir, ec);
}
