// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: label metadata.
#include "selftest_support.h"
#include "selftest_cases.h"



void testAerUtilRenameLabel() {
  std::printf("[selftest] --- AER-SCOPE1 bundle I: renvarlab / labmask / splitvallabels ---\n");
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_aerutilrenlab").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto exec = [](const std::string& src, piet::SessionState& session) {
    return piet::doexec::executeDoSource(src, session, piet::dispatch);
  };
  auto seed = [&](piet::SessionState& s) {
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (handlers.cpp:2040).
  };
  auto firstError = [](const nlohmann::ordered_json& blocks) -> nlohmann::ordered_json {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return b;
    return nlohmann::ordered_json();
  };
  auto errText = [](const nlohmann::ordered_json& e) {
    return e.is_object() ? e.value("message", std::string()) : std::string("<none>");
  };
  auto fullPrec = [](double v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return std::string(buf);
  };
  auto has = [](const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
  };
  auto rOf = [](piet::SessionState& s, const char* key) -> double {
    const double* p = s.eval.rValues.find(key);
    return p ? *p : std::nan("");
  };
  auto runR = [&](piet::SessionState& s, const std::string& cmd, const char* key) -> double {
    (void)piet::dispatch(cmd, s);
    return rOf(s, key);
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
  auto joinNames = [](const std::vector<std::string>& v) {
    std::string s;
    for (const auto& n : v) { if (!s.empty()) s += " "; s += n; }
    return s.empty() ? std::string("<none>") : s;
  };
  // The attached value label, read straight off the session rather than out of
  // a rendered `label list`: this is the store every downstream consumer reads,
  // and a labmask that printed a correct listing without attaching anything is
  // exactly the wrong implementation worth catching.
  auto labelOf = [](piet::SessionState& s, const std::string& var, int code) -> std::string {
    auto it = s.valueLabels.find(var);
    if (it == s.valueLabels.end()) return std::string("<no label attached>");
    auto jt = it->second.find(code);
    return jt == it->second.end() ? std::string("<no such code>") : jt->second;
  };

  // FIXTURE R -- four rows, two numeric columns with DIFFERENT sums so a
  // rename can be told from a copy of the neighbour:
  //     ra : 1  2  3  4      (sum 10)
  //     rb : 10 2  3  4      (sum 19)
  static const char* kFixR =
      "clear\n"
      "set obs 4\n"
      "gen ra = _n\n"
      "gen rb = _n\n"
      "replace rb = 10 in 1\n";

  // ── (i) `renvarlab, prefix()` renames and carries the DATA. ────────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixR, s);
    auto r = exec("renvarlab ra rb, prefix(z_)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/renlab (i): `renvarlab ra rb, prefix(z_)` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const std::vector<std::string> vn = varNames(s);
    check(hasName(vn, "z_ra") && hasName(vn, "z_rb"),
          "AER-UTIL/renlab (i): both targets exist under the prefixed names. Got `" +
              joinNames(vn) + "`");
    check(!hasName(vn, "ra") && !hasName(vn, "rb"),
          "AER-UTIL/renlab (i): ...and the ORIGINALS are gone. A `rename` that cloned instead of "
          "renaming leaves four columns, doubles the frame's width on every call, and answers "
          "rc 0. Got `" + joinNames(vn) + "`");
    const double s1 = runR(s, "summarize z_rb", "sum");
    check(s1 == 19.0,
          "AER-UTIL/renlab (i): ...and z_rb carries rb's data (10+2+3+4). 10 would mean it "
          "carries ra's, which is what a rename loop that walked the varlist and the target list "
          "out of step produces. Got " + fullPrec(s1));
    const double n1 = runR(s, "summarize z_ra", "N");
    check(n1 == 4.0,
          "AER-UTIL/renlab (i): ...and z_ra is a real column of four values, not a freshly "
          "created empty one. Got N " + fullPrec(n1));
  }

  // ── (ii) `upper` is the case transform, applied to the NAME. ───────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixR, s);
    auto r = exec("renvarlab ra, upper\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/renlab (ii): `renvarlab ra, upper` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const std::vector<std::string> vn = varNames(s);
    check(hasName(vn, "RA") && !hasName(vn, "ra"),
          "AER-UTIL/renlab (ii): ra became RA. Got `" + joinNames(vn) + "`");
    check(hasName(vn, "rb"),
          "AER-UTIL/renlab (ii): ...and rb, which was not in the varlist, was NOT touched -- a "
          "transform that defaulted to _all when a varlist WAS given renames the whole frame. "
          "Got `" + joinNames(vn) + "`");
    check(runR(s, "summarize RA", "sum") == 10.0,
          "AER-UTIL/renlab (ii): ...and RA still holds 1+2+3+4. Got " +
              fullPrec(runR(s, "summarize RA", "sum")));
  }

  // ── (iii) a colliding target aborts the WHOLE call, before anything moves. ─
  // p1 -> p1_x is free; p2 -> p2_x is taken. The plan's contract is that every
  // target is validated before the first `rename` runs, and this leg is the only
  // thing that tells that apart from a sequential loop: the sequential loop
  // renames p1 successfully, then fails, and leaves a frame with p1_x and p2 in
  // it -- half-applied, rc non-zero, and no way for a caller to undo it. Worse
  // still is a loop that validates nothing: p2 overwrites p2_x and 99*4 = 396
  // becomes 19.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec("clear\n"
               "set obs 4\n"
               "gen p1 = _n\n"
               "gen p2 = _n\n"
               "replace p2 = 10 in 1\n"
               "gen p2_x = 99\n",
               s);
    auto r = exec("renvarlab p1 p2, suffix(_x)\n", s);
    const nlohmann::ordered_json e = firstError(r.blocks);
    check(!e.is_null(),
          "AER-UTIL/renlab (iii): `renvarlab p1 p2, suffix(_x)` REFUSES -- p2_x already exists");
    const std::vector<std::string> vn = varNames(s);
    check(hasName(vn, "p1") && hasName(vn, "p2"),
          "AER-UTIL/renlab (iii): ...and NOTHING moved: both source names are still there. A "
          "sequential loop renames p1 first and leaves the frame half-transformed under a "
          "non-zero rc, which no caller can unwind. Got `" + joinNames(vn) + "`");
    check(!hasName(vn, "p1_x"),
          "AER-UTIL/renlab (iii): ...so p1_x does not exist. This is the check the error code "
          "alone cannot make. Got `" + joinNames(vn) + "`");
    check(runR(s, "summarize p2_x", "sum") == 396.0,
          "AER-UTIL/renlab (iii): ...and p2_x still holds its own data (99*4). 19 here means p2 "
          "overwrote it -- silent data loss, and the reason targets are validated up front. Got " +
              fullPrec(runR(s, "summarize p2_x", "sum")));
  }

  // FIXTURE L -- four rows, a code column and the text that names each code:
  //     lcode : 1      1      2      2
  //     ltext : alpha  alpha  beta   beta
  static const char* kFixL =
      "clear\n"
      "set obs 4\n"
      "gen lcode = 1\n"
      "replace lcode = 2 in 3/4\n"
      "gen str6 ltext = \"alpha\"\n"
      "replace ltext = \"beta\" in 3/4\n";

  // ── (iv) `labmask` attaches the code->text map it synthesised. ─────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixL, s);
    auto r = exec("labmask lcode, values(ltext)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/renlab (iv): `labmask lcode, values(ltext)` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(labelOf(s, "lcode", 1) == "alpha",
          "AER-UTIL/renlab (iv): code 1 is labelled `alpha` on lcode itself. Read off "
          "session.valueLabels, which is the store list/browse/coeftable all read -- a labmask "
          "that rendered a correct `label list` without attaching anything passes any check made "
          "against its own output. Got `" + labelOf(s, "lcode", 1) + "`");
    check(labelOf(s, "lcode", 2) == "beta",
          "AER-UTIL/renlab (iv): ...and code 2 is `beta`, so the map is keyed on the CODE and not "
          "on the row index (which would put `alpha` on 2 by way of row 2). Got `" +
              labelOf(s, "lcode", 2) + "`");
    check(s.varLblname.count("lcode") == 1 && s.varLblname.at("lcode") == "lcode",
          "AER-UTIL/renlab (iv): ...under a label set named after the variable, which is the "
          "default when lblname() is absent. Got `" +
              (s.varLblname.count("lcode") ? s.varLblname.at("lcode") : std::string("<none>")) +
              "`");
    const std::string listing = [&] {
      std::string out;
      for (const auto& b : piet::dispatch("label list lcode", s))
        if (b.is_object()) { out += b.value("text", std::string()); out += "\n"; }
      return out;
    }();
    check(has(listing, "alpha") && has(listing, "beta"),
          "AER-UTIL/renlab (iv): ...and `label list lcode` renders both entries -- the second, "
          "independent reader of the same store. Got `" + listing + "`");
  }

  // ── (v) `lblname()` names the set, without moving the attachment. ──────────
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixL, s);
    auto r = exec("labmask lcode, values(ltext) lblname(mymask)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/renlab (v): `labmask lcode, values(ltext) lblname(mymask)` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(s.varLblname.count("lcode") == 1 && s.varLblname.at("lcode") == "mymask",
          "AER-UTIL/renlab (v): the set is named `mymask`. An option consumed and ignored leaves "
          "`lcode` here and is invisible in every rendering. Got `" +
              (s.varLblname.count("lcode") ? s.varLblname.at("lcode") : std::string("<none>")) +
              "`");
    check(labelOf(s, "lcode", 2) == "beta",
          "AER-UTIL/renlab (v): ...and the map is still ATTACHED to lcode under that name -- "
          "defining a set and forgetting to attach it is the half-landing shape. Got `" +
              labelOf(s, "lcode", 2) + "`");
  }

  // ── (vi) a code that carries TWO texts is refused, not resolved. ───────────
  // lcode 1 now names both `alpha` (row 1) and `gamma` (row 2). Picking the
  // first or the last seen produces a label that is wrong for half the rows and
  // says nothing about it -- the exact silent-wrong-answer this row exists to
  // avoid, and every downstream table would carry it.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixL, s);
    (void)exec("replace ltext = \"gamma\" in 2\n", s);
    auto r = exec("labmask lcode, values(ltext)\n", s);
    check(!firstError(r.blocks).is_null(),
          "AER-UTIL/renlab (vi): a code mapping to two different texts is REFUSED. Last-wins "
          "would label half of code 1's rows `gamma` under rc 0");
    check(s.valueLabels.count("lcode") == 0,
          "AER-UTIL/renlab (vi): ...and nothing was attached on the way out. A refusal that had "
          "already written half a label set leaves the frame in the state the refusal claims to "
          "have prevented");
  }

  // ── (vii) a non-integer masked variable is an error. ───────────────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixL, s);
    (void)exec("gen lfrac = 1.5\n", s);
    auto r = exec("labmask lfrac, values(ltext)\n", s);
    check(!firstError(r.blocks).is_null(),
          "AER-UTIL/renlab (vii): a NON-INTEGER variable cannot carry a value label and is "
          "refused. Truncating 1.5 to 1 silently relabels the column");
  }

  // ── (viii) `splitvallabels` breaks on WORD boundaries, not at length(). ────
  // "one two three" under length(8): "one two" is 7 and adding " three" makes
  // 13, so the break is after `two` -- ["one two", "three"]. "four five" is 9,
  // so it breaks into ["four", "five"]. A character split at 8 gives
  // ["one two ", "three"] and ["four fiv", "e"]; no split at all leaves the two
  // originals. All three renderings are a plausible-looking r(relabel).
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec("clear\n"
               "set obs 2\n"
               "gen sv = 1\n"
               "replace sv = 2 in 2\n"
               "label define svl 1 \"one two three\" 2 \"four five\"\n"
               "label values sv svl\n",
               s);
    auto r = exec("splitvallabels sv, length(8)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/renlab (viii): `splitvallabels sv, length(8)` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const std::string rel = s.eval.rStrings.get("relabel", "<unset>");
    check(rel != "<unset>" && !rel.empty(),
          "AER-UTIL/renlab (viii): r(relabel) is published at all -- this command's ENTIRE answer "
          "is that macro, so an empty one is the command not having run. Got `" + rel + "`");
    check(has(rel, "1 \"one two\" \"three\""),
          "AER-UTIL/renlab (viii): ...code 1 splits at the WORD boundary: \"one two\" then "
          "\"three\". A character split at 8 yields \"one two \" (trailing space) and no split at "
          "all yields \"one two three\". Got `" + rel + "`");
    check(has(rel, "2 \"four\" \"five\""),
          "AER-UTIL/renlab (viii): ...and code 2, whose text is 9 characters, splits into two "
          "pieces rather than staying whole -- the length() bound is applied per PIECE. Got `" +
              rel + "`");
  }

  fs::remove_all(workdir, ec);
}


// P1-W2-0-LABELBOOK-BUG102: `labelbook` is a direct-read report over
// session.labelDefs/valueLabels/varLblname — no engine change, the data
// already arrives via `use`'s --out-meta sidecar (session_state.h:1054-
// 1067). Checks label-set name + code:text definition + attached-variable
// presence, and the label-less fallback text (mirrors runLabelList/
// runLabelDir's existing "(no value labels defined)" idiom).
void testLabelbook() {
  std::printf("[selftest] --- labelbook (P1-W2-0) ---\n");

#ifdef _WIN32
  std::error_code lbec;
  std::string workdir =
      (std::filesystem::temp_directory_path() / "piet_native_selftest_labelbook").u8string();
  std::filesystem::create_directories(workdir, lbec);
#else
  std::string workdir = "/tmp/piet_native_selftest_labelbook";
  (void)std::system(("mkdir -p '" + workdir + "'").c_str());
#endif

  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("gen state_num = mod(_n,3)+1", session);
    (void)piet::dispatch("label define statelbl 1 \"CA\" 2 \"NY\" 3 \"TX\"", session);
    (void)piet::dispatch("label values state_num statelbl", session);
    auto blocks = piet::dispatch("labelbook", session);
    std::string text;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display")
        text += b.value("text", std::string());
    }
    check(text.find("statelbl") != std::string::npos,
          "labelbook: reports the defined label set name");
    check(text.find("CA") != std::string::npos && text.find("NY") != std::string::npos &&
              text.find("TX") != std::string::npos,
          "labelbook: reports the code:text definition");
    check(text.find("state_num") != std::string::npos,
          "labelbook: reports the attached variable");
  }

  {
    // Label-less fallback: a fresh session with no value labels defined.
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    auto blocks = piet::dispatch("labelbook", session);
    std::string text;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display")
        text += b.value("text", std::string());
    }
    check(text.find("no value labels defined") != std::string::npos,
          "labelbook: label-less fallback message");
  }

#ifdef _WIN32
  std::error_code lrec;
  std::filesystem::remove_all(workdir, lrec);
#else
  (void)std::system(("rm -rf '" + workdir + "'").c_str());
#endif
}


// --- BUG-355: tabulate must apply the value labels the session already has ---
// The discriminating fact is not "labels are missing" but that ONE session's
// label store was being read by one consumer and ignored by another: after
// `label values g3 glab`, `regress fatal i.g3` rendered mid.g3/high.g3 while
// `tabulate g3` rendered 1.0/2.0/3.0. Stata renders a MIX — labelled codes as
// text, unlabelled codes as numbers — so the leg pins the mix, not "all text".
void testBUG355TabulateValueLabels() {
  std::printf("[selftest] --- BUG-355: tabulate applies value labels (mixed) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug355").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto rowsOf = [](const nlohmann::ordered_json& blocks) {
    std::vector<std::string> out;
    for (const auto& b : blocks) {
      if (!b.is_object() || b.value("type", std::string()) != "tabulate") continue;
      if (!b.contains("table") || !b["table"].contains("rows")) continue;
      for (const auto& r : b["table"]["rows"]) out.push_back(r.get<std::string>());
    }
    return out;
  };
  auto valuesKeys = [](const nlohmann::ordered_json& blocks) {
    std::vector<std::string> out;
    for (const auto& b : blocks) {
      if (!b.is_object() || b.value("type", std::string()) != "tabulate") continue;
      if (!b.contains("table") || !b["table"].contains("values")) continue;
      for (auto it = b["table"]["values"].begin(); it != b["table"]["values"].end(); ++it)
        out.push_back(it.key());
    }
    return out;
  };
  auto has = [](const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
  };

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen byte g5 = 1 + mod(_n, 5)", session);
  (void)piet::dispatch("label define plab 3 \"three\" 4 \"four\"", session);
  (void)piet::dispatch("label values g5 plab", session);

  auto rows = rowsOf(piet::dispatch("tabulate g5", session));
  if (rows.empty()) {
    skip("BUG-355: tabulate value labels", "tabulate produced no table in this environment");
  } else {
    check(has(rows, "three") && has(rows, "four"),
          "BUG-355: LABELLED codes render as their label");
    check(has(rows, "1.0") && has(rows, "5.0"),
          "BUG-355: UNLABELLED codes stay numeric — Stata's mix, not all-or-nothing");
    // The values map must be re-keyed in step, or a consumer joining rows to
    // counts silently loses every relabelled cell.
    auto keys = valuesKeys(piet::dispatch("tabulate g5", session));
    check(has(keys, "three") && has(keys, "1.0"),
          "BUG-355: table.values is re-keyed in step with table.rows");

    // An UNLABELLED variable must be untouched.
    auto plain = rowsOf(piet::dispatch("tabulate year if year < 1985", session));
    check(has(plain, "1982.0"),
          "BUG-355: a variable with no value label is rendered exactly as before");
    // And the r-class carriage added by BUG-354 must survive the rewrite.
    std::string rn;
    for (const auto& b : piet::dispatch("display r(N)", session))
      if (b.is_object() && b.value("type", std::string()) == "display")
        rn = b.value("text", std::string());
    check(rn.find("144") != std::string::npos,
          "BUG-355: r(N) still answers after the relabel (144) — got " + rn);
  }
  fs::remove_all(workdir, ec);
}


// --- BUG-351: a row filter must not delete value-label definitions ----------
// Stata keeps the label set when observations are dropped (StataBE 19.5,
// 2026-08-18: `encode make`, `keep if foreign == 1` deletes 52 observations and
// label 74 "Volvo 260" is STILL defined). PIET's string-predicate filter used to
// rewrite the frame as CSV and re-convert it, which re-factorizes and so erases
// every level no surviving row uses — `state` measured 48 levels -> 16.
//
// The leg also A/Bs the fallback: PIET_NO_RESIDENT_KEEPIF forces the old
// whole-frame path, which must still produce the same ROWS and VALUES. Without
// that arm the fallback would rot silently, and it is the path every failure in
// the fast one lands on.
void testBUG351KeepIfLevels() {
  std::printf("[selftest] --- BUG-351: keep if must not drop value-label definitions ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug351").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto levelsOf = [](const nlohmann::ordered_json& blocks, const std::string& var) -> int {
    for (const auto& b : blocks) {
      if (!b.is_object() || b.value("type", std::string()) != "data_mutation") continue;
      if (!b.contains("varlist_after")) continue;
      for (const auto& v : b["varlist_after"])
        if (v.value("name", std::string()) == var && v.contains("n_levels"))
          return v["n_levels"].get<int>();
    }
    return -1;
  };
  auto rowsAfter = [](const nlohmann::ordered_json& blocks) -> long long {
    long long last = -1;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "data_mutation" &&
          b.contains("n_rows_after"))
        last = b["n_rows_after"].get<long long>();
    return last;
  };
  auto runArm = [&](bool forceOldPath) {
    if (forceOldPath) {
#ifndef _WIN32
      setenv("PIET_NO_RESIDENT_KEEPIF", "1", 1);
#endif
    } else {
#ifndef _WIN32
      unsetenv("PIET_NO_RESIDENT_KEEPIF");
#endif
    }
    piet::SessionState s(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", s);
    (void)piet::dispatch("gen str8 grp = \"aa\"", s);
    (void)piet::dispatch("replace grp = \"bb\" if beertax > .5", s);
    auto blocks = piet::dispatch("keep if grp == \"bb\"", s);
    return std::make_pair(levelsOf(blocks, "state"), rowsAfter(blocks));
  };

  auto fast = runArm(false);
  auto slow = runArm(true);
#ifndef _WIN32
  unsetenv("PIET_NO_RESIDENT_KEEPIF");
#endif

  if (fast.first < 0) {
    skip("BUG-351: keep-if level retention",
         "no data_mutation varlist — no frames-capable `cobra serve` in this environment");
  } else {
    check(fast.first == 48,
          "BUG-351: the filter KEEPS every value-label definition (state stays 48 levels, as "
          "Stata does) — got " + std::to_string(fast.first));
    check(slow.first > 0 && slow.first < fast.first,
          "BUG-351 precondition: the OLD whole-frame path really did drop them (" +
              std::to_string(slow.first) + " < " + std::to_string(fast.first) +
              ") — if these ever match, this leg is no longer testing anything");
    check(fast.second == slow.second && fast.second > 0,
          "BUG-351: both paths keep the SAME rows (" + std::to_string(fast.second) +
              ") — the fast path is an optimization, not a different answer");
  }

  fs::remove_all(workdir, ec);
}
