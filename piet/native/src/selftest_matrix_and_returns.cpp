// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: matrix and returns.
#include "selftest_support.h"
#include "selftest_cases.h"


// Sibling of testAerAbbrevBundle above, and here for the same reason: all four
// floors.json rows of AER bundle B are `no_fixture` (two because there is no
// dataset quantity to floor, two because the timing corpus is absent from this
// tree), so this function, pangram S26 and internal capture
// probe_results.do carry the bundle until a --counters batch captures it.
//
// THE HEADLINE PROPERTY IS ONE LINE OF BEHAVIOUR AND IT IS INVISIBLE TO EVERY
// OTHER GATE. At the TOP LEVEL `return scalar`/`return local` now set r()
// IMMEDIATELY. Before this bundle every form staged, and a staged store still
// prints a perfectly healthy `return list` while `display r(q)` reads nothing --
// so each of the first two legs reads the value TWICE, once through the listing
// and once through an expression, and it is the SECOND reading that fails on a
// half-landed store. A run-it-once probe passes either way.
//
// The in-program half is the mirror, and it is asserted NEGATIVELY on purpose:
// inside a program `return scalar` must STILL stage (Stata's behaviour, and the
// half the top-level change must not have broken). The in-body read is wrapped
// in `capture noisily` and the check is "the value is not visible YET" rather
// than "the display prints missing", because an engine that refuses an unset
// r() reference and one that prints a dot both satisfy the property under test;
// only printing 7 is the regression. The leg cannot pass vacuously -- the
// after-the-call reading pins 7 positively, so a program that never ran fails.
//
// svmat and mkmat are asserted as a ROUND TRIP rather than one at a time. Each
// alone can only be checked against numbers this function itself wrote, which a
// handler that ignored its input would also produce; composed, the columns have
// to come back.
//
// Deliberately NOT asserted anywhere below: the number of blocks. Every
// dispatched line also emits an echo block, so a block count measures the echo
// (an earlier bundle in this campaign learned that). Every check here reads
// block CONTENT.
void testAerResultsBundle() {
  std::printf("[selftest] --- AER-SCOPE1 bundle B: return list/scalar/local/clear / svmat / "
              "mkmat / matlist ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_aerresults").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto displays = [](const auto& blocks) {
    std::vector<std::string> v;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "display")
        v.push_back(b.value("text", std::string()));
    return v;
  };
  // Guarded rather than returning a bare json: `value()` on a null throws, so
  // an absent block would abort the run instead of failing one check (the trap
  // the BUG-129 graph legs earlier in this file record).
  auto firstOfType = [](const auto& blocks, const std::string& type) -> nlohmann::json {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == type) return b;
    return nlohmann::json::object();
  };
  // The printed output with the COMMAND ECHO removed. Filtered on the text's
  // own leading '.' rather than on the block type, because this bundle's
  // printers reuse runEreturnList's "echo"-typed block (handlers.h says so in
  // as many words) -- a type filter would throw away the output itself. The
  // ereturn-shaped listings open with a newline, so they survive the filter.
  auto renderText = [](const auto& blocks) {
    std::string out;
    for (const auto& b : blocks) {
      if (!b.is_object()) continue;
      const std::string t = b.value("text", std::string());
      if (t.empty() || t[0] == '.') continue;
      out += t;
      out += "\n";
    }
    return out;
  };
  // The one LINE carrying `needle`, so a check on r(q)'s value cannot be
  // satisfied by a different row of the same listing.
  auto lineWith = [](const std::string& text, const std::string& needle) {
    const size_t at = text.find(needle);
    if (at == std::string::npos) return std::string();
    size_t b = text.rfind('\n', at);
    b = (b == std::string::npos) ? size_t(0) : b + 1;
    const size_t e = text.find('\n', at);
    return text.substr(b, (e == std::string::npos ? text.size() : e) - b);
  };
  auto numberAfter = [](const std::string& line, const std::string& sep) -> double {
    const size_t at = line.find(sep);
    if (at == std::string::npos) return std::nan("");
    try {
      return std::stod(line.substr(at + sep.size()));
    } catch (...) {
      return std::nan("");
    }
  };
  // `display "TAG=" expr` prints "TAG=<value>" with no separating space (the
  // shape BUG-167's leg above pins). Parsed as a NUMBER rather than compared as
  // a string so a formatting choice ("5" against "5.0000") cannot fail a check
  // about arithmetic.
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
  auto sawDisplayExactly = [&](const auto& blocks, const std::string& want) {
    for (const auto& s : displays(blocks))
      if (s == want) return true;
    return false;
  };
  auto varCount = [](piet::SessionState& session) {
    const nlohmann::ordered_json blocks = piet::dispatch("describe", session);
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "describe" && b.contains("variables"))
        return static_cast<int>(b["variables"].size());
    return -1;
  };
  auto exec = [](const std::string& src, piet::SessionState& session) {
    return piet::doexec::executeDoSource(src, session, piet::dispatch);
  };
  auto sawCode = [](const auto& blocks, const std::string& code) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error" &&
          b.value("code", std::string()) == code)
        return true;
    return false;
  };

  // --- TOP-LEVEL `return scalar` / `return local` are REFUSED (BUG-423). ---
  //
  // This leg used to assert the opposite: that a top-level `return scalar` set
  // r() immediately. That came from an unmeasured comment in program.cpp, and
  // bundle B's own wave pin measured StataBE 19.5 answering **r(151)**, "non
  // r-class program may not set r()". The 2026-09-06 ruling is to match Stata
  // (accepting what Stata refuses is the one direction that silently produces a
  // do-file which cannot run there), so the leg now pins the refusal -- and,
  // just as importantly, pins that r() is NOT quietly set behind it.
  {
    piet::SessionState session(workdir);
    // Separate executeDoSource calls, not one source: every scan here reads
    // every block, and the ECHO of `display r(q)` contains the literal "r(q)",
    // so a one-call read could pass on the echo of the line under test.
    const auto refused = exec("return scalar q = 5\n", session);
    // The sentence lives in the error block's `message`, not in rendered text —
    // renderText reads `text`, which an error block does not carry.
    const auto msgOf = [](const auto& blocks) {
      for (const auto& b : blocks)
        if (b.is_object() && b.value("type", std::string()) == "error")
          return b.value("message", std::string());
      return std::string();
    };
    const std::string rtext = msgOf(refused.blocks);
    check(rtext.find("non r-class program may not set r()") != std::string::npos,
          "RESULTS/return scalar: a TOP-LEVEL `return scalar q = 5` is REFUSED with Stata's own "
          "sentence (r(151)) -- got `" + rtext + "`");
    const std::string listing = renderText(exec("return list\n", session).blocks);
    check(lineWith(listing, "r(q)").empty(),
          "RESULTS/return scalar: ...and r(q) is NOT set behind the refusal -- an error that still "
          "wrote the value would be the worst of both. Got `" + listing + "`");
  }
  {
    piet::SessionState session(workdir);
    const auto msgOf = [](const auto& blocks) {
      for (const auto& b : blocks)
        if (b.is_object() && b.value("type", std::string()) == "error")
          return b.value("message", std::string());
      return std::string();
    };
    const std::string rtext = msgOf(exec("return local w abc\n", session).blocks);
    check(rtext.find("non r-class program may not set r()") != std::string::npos,
          "RESULTS/return local: the macro twin is refused the same way -- the two stores went "
          "immediate together and they are refused together. Got `" + rtext + "`");
    const std::string listing = renderText(exec("return list\n", session).blocks);
    check(lineWith(listing, "r(w)").empty(),
          "RESULTS/return local: ...and r(w) is unset behind it. Got `" + listing + "`");
  }

  // --- inside a program the SAME statements must still stage. ---
  {
    piet::SessionState session(workdir);
    auto r = exec("program define aer_b_stage, rclass\n"
                  "    return scalar z = 7\n"
                  "    capture noisily display \"AER_INPROG=\" r(z)\n"
                  "end\n"
                  "aer_b_stage\n"
                  "display \"AER_AFTER=\" r(z)\n",
                  session);
    check(!r.stoppedAtReason.has_value(),
          "RESULTS/in-program: the rclass program runs to its end, stop=" +
              r.stoppedAtReason.value_or("<none>"));
    check(!sawDisplayExactly(r.blocks, "AER_INPROG=7"),
          "RESULTS/in-program: `return scalar z = 7` INSIDE a program still STAGES -- the value "
          "must NOT be readable before the program returns. This is Stata in both places, and it "
          "is the half the top-level change must not have taken with it");
    check(sawDisplayExactly(r.blocks, "AER_AFTER=7"),
          "RESULTS/in-program: ...and it IS readable once the program returns -- without this "
          "positive reading the negative check above passes on a program that never ran at all");
  }

  // --- return clear empties BOTH stores. ---
  {
    piet::SessionState session(workdir);
    (void)exec("return scalar q = 5\n", session);
    (void)exec("return local w abc\n", session);
    (void)exec("return clear\n", session);
    const std::string listing = renderText(exec("return list\n", session).blocks);
    check(listing.find("r(q)") == std::string::npos,
          "RESULTS/return clear: the SCALAR store is empty afterwards -- got `" + listing + "`");
    check(listing.find("r(w)") == std::string::npos,
          "RESULTS/return clear: ...and the MACRO store with it. Two stores, and a clear that "
          "reached only one is the half-landing shape again -- got `" + listing + "`");
  }

  // --- svmat with NO dataset: this is how Stata builds a dataset FROM a
  // matrix, so a no-data refusal here is the defect rather than the guard.
  {
    piet::SessionState session(workdir);
    auto mat = exec("matrix A = (1,2\\3,4\\5,6)\n", session);
    // Read FIRST, and it is not defensive padding. MEASURED on the shipped
    // binary: `matrix A = (1,2\3,4\5,6)` with nothing in memory answers
    // no_data_loaded and STOPS the do-file, because the dispatcher's
    // kNoDataCommands set carries `svmat` and `matlist` but not `matrix`. Every
    // check under this one would then read as "svmat did nothing" while the real
    // cause is one line upstream, so the cause is asserted where it happens.
    check(!sawCode(mat.blocks, "no_data_loaded"),
          "RESULTS/svmat no-data: the `matrix A = ...` that FEEDS svmat is itself reachable with "
          "nothing in memory -- svmat sitting in the no-data allowlist buys nothing while the only "
          "way to build its input does not");
    auto r = exec("svmat A\n"
                  "display \"AER_N=\" _N\n"
                  "display \"AER_A13=\" A1[3]\n"
                  "display \"AER_A22=\" A2[2]\n",
                  session);
    check(std::fabs(displayValue(r.blocks, "AER_N=") - 3.0) < 1e-12,
          "RESULTS/svmat no-data: `svmat A` over a 3x2 matrix with no dataset sets _N to 3");
    check(std::fabs(displayValue(r.blocks, "AER_A13=") - 5.0) < 1e-12,
          "RESULTS/svmat no-data: ...and A1[3] is the matrix's own (3,1) cell, 5");
    check(std::fabs(displayValue(r.blocks, "AER_A22=") - 4.0) < 1e-12,
          "RESULTS/svmat no-data: ...and A2[2] is 4 -- the SECOND column, so a route that wrote "
          "column 1 twice moves this rather than passing quietly");
  }

  // --- names(col) reads the matrix's COLUMN names. ---
  {
    piet::SessionState session(workdir);
    auto r = exec("sysuse fatalities, clear\n"
                  "matrix B = (1,2\\3,4)\n"
                  "matrix colnames B = alpha beta\n"
                  "svmat B, names(col)\n"
                  "display \"AER_ALPHA=\" alpha[2]\n"
                  "display \"AER_BETA=\" beta[1]\n",
                  session);
    check(std::fabs(displayValue(r.blocks, "AER_ALPHA=") - 3.0) < 1e-12,
          "RESULTS/svmat names(col): the variables are named from the matrix COLUMN names, so "
          "alpha[2] reads 3. A silently-ignored option makes B1/B2 instead and this reads nothing "
          "-- which is why the check is on the NAMED column, not on the values");
    check(std::fabs(displayValue(r.blocks, "AER_BETA=") - 2.0) < 1e-12,
          "RESULTS/svmat names(col): ...and beta[1] reads 2, the second column's first cell");
  }

  // --- a name collision refuses, and refuses having created NOTHING. ---
  {
    piet::SessionState session(workdir);
    (void)exec("clear\nset obs 3\ngen double A1 = 99\nmatrix A = (1,2\\3,4\\5,6)\n", session);
    const int before = varCount(session);
    // svmat in its OWN call: the setup's `gen` emits a data_mutation of its own
    // and the counts below would then be reading the setup.
    auto r = exec("svmat A\n", session);
    check(!firstOfType(r.blocks, "error").empty(),
          "RESULTS/svmat collision: `svmat A` is REFUSED when the target name A1 is already taken");
    const int after = varCount(session);
    check(before > 0 && after == before,
          "RESULTS/svmat collision: ...and it created NOTHING. A refusal that had already made A2 "
          "before reaching A1 is the silent half of this class and an error block alone cannot "
          "tell the two apart -- got " + std::to_string(before) + " variables before, " +
              std::to_string(after) + " after");
    check(std::fabs(displayValue(exec("display \"AER_KEPT=\" A1[1]\n", session).blocks,
                                 "AER_KEPT=") -
                    99.0) < 1e-12,
          "RESULTS/svmat collision: ...and the EXISTING A1 still holds 99, not the matrix's 1");
  }

  // --- mkmat is svmat's inverse, and the pair is asserted as a ROUND TRIP. ---
  {
    piet::SessionState session(workdir);
    (void)exec("clear\nset obs 3\ngen double xx = _n\ngen double yy = _n * 2\n"
               "mkmat xx yy, matrix(RT)\n",
               session);
    auto r = exec("svmat RT\n"
                  "display \"AER_RT11=\" RT1[1]\n"
                  "display \"AER_RT13=\" RT1[3]\n"
                  "display \"AER_RT23=\" RT2[3]\n",
                  session);
    check(std::fabs(displayValue(r.blocks, "AER_RT11=") - 1.0) < 1e-12,
          "RESULTS/mkmat round trip: xx -> matrix -> RT1 returns xx[1] == 1");
    check(std::fabs(displayValue(r.blocks, "AER_RT13=") - 3.0) < 1e-12,
          "RESULTS/mkmat round trip: ...and xx[3] == 3, so the column arrived whole rather than "
          "as its first cell repeated");
    check(std::fabs(displayValue(r.blocks, "AER_RT23=") - 6.0) < 1e-12,
          "RESULTS/mkmat round trip: ...and yy[3] == 6 came back as the SECOND column -- the "
          "reading a transposed or single-column matrix fails and two separate one-command "
          "checks would not");
  }

  // --- nomissing drops LISTWISE, which is a different answer from truncating. ---
  {
    piet::SessionState session(workdir);
    auto r = exec("clear\nset obs 3\ngen double xx = _n\ngen double yy = _n * 2\n"
                  "replace yy = . in 2\n"
                  "mkmat xx yy, matrix(NM) nomissing\n"
                  "display \"AER_NMROWS=\" rowsof(NM)\n"
                  "display \"AER_NM21=\" NM[2,1]\n",
                  session);
    check(std::fabs(displayValue(r.blocks, "AER_NMROWS=") - 2.0) < 1e-12,
          "RESULTS/mkmat nomissing: the observation whose yy is missing is dropped -- 3 in, 2 out");
    check(std::fabs(displayValue(r.blocks, "AER_NM21=") - 3.0) < 1e-12,
          "RESULTS/mkmat nomissing: ...and it is the MIDDLE observation that went, so matrix row "
          "2 carries observation 3 (xx == 3). A handler that dropped the LAST row instead would "
          "answer 2 rows here too and read 2 for this cell");
  }

  // --- a string variable has no cell in a matrix, and the refusal says which. ---
  {
    piet::SessionState session(workdir);
    (void)exec("clear\nset obs 3\ngen double xx = _n\ngen str6 sname = \"ab\"\n", session);
    auto r = exec("mkmat xx sname, matrix(SM)\n", session);
    auto err = firstOfType(r.blocks, "error");
    check(!err.empty(),
          "RESULTS/mkmat string: a string variable in the varlist is REFUSED -- accepting it "
          "would write a column of missing and call it data");
    const std::string smsg = err.value("message", std::string());
    check(smsg.find("sname") != std::string::npos,
          "RESULTS/mkmat string: ...and the message NAMES the variable, which is the difference "
          "between a fixable do-file and a re-run with prints in it -- got `" + smsg + "`");
    // The NAME alone cannot certify this leg. `mkmat: variable sname not found`
    // carries "sname" too, and that is what this leg reads if the setup's
    // `gen str6` ever stops making a column -- a refusal for the wrong reason,
    // certifying a string path that was never walked. The wording is the
    // discriminator, and it is load-bearing twice over: the handler states
    // r(109) through `reason` because coarseRc has no message rule for this
    // phrase, so a rewrite that dropped the phrase would also have to keep the
    // explicit rc.
    check(smsg.find("string variable") != std::string::npos,
          "RESULTS/mkmat string: ...and it is refused AS A STRING rather than as a missing "
          "variable -- `variable sname not found` names sname too and would satisfy the check "
          "above while proving the string path was never reached. Got `" + smsg + "`");
  }

  // --- matlist renders; `matrix list` already prints numbers. ---
  {
    piet::SessionState session(workdir);
    (void)exec("sysuse fatalities, clear\n"
               "matrix C = (1,2\\3,4)\n"
               "matrix rownames C = rowa rowb\n"
               "matrix colnames C = cola colb\n",
               session);
    const std::string plain = renderText(exec("matlist C\n", session).blocks);
    for (const char* stub : {"rowa", "rowb", "cola", "colb"}) {
      check(plain.find(stub) != std::string::npos,
            std::string("RESULTS/matlist: the rendering carries the stub `") + stub +
                "` -- BOTH row names and BOTH column names, because a printer that emitted only "
                "the numbers is what `matrix list` already does. Got `" + plain + "`");
    }
    const std::string fmt = renderText(exec("matlist C, format(%9.3f)\n", session).blocks);
    check(fmt != plain,
          "RESULTS/matlist format(): format(%9.3f) CHANGES the rendering. The command echo is "
          "filtered out of both strings, so this compares OUTPUT and not command text -- an "
          "option parsed and then dropped leaves these two identical");
    check(fmt.find("1.000") != std::string::npos,
          "RESULTS/matlist format(): ...to three decimals -- got `" + fmt + "`");
  }

  // --- the honest-rejection half, one line per command of the bundle. The
  // campaign's wiring checklist owes one per bundle, and an unknown option is
  // the sharpest one available here: a command missing from its option
  // vocabulary ACCEPTS the token and then ignores it, which is a silent wrong
  // answer wearing a green gate. The code is asserted as well as the block,
  // because eval::coarseRc answers r(198) for `unsupported_option` and turns a
  // refusal carrying the default engine_error code into rc 1 instead.
  {
    const std::vector<std::pair<std::string, std::string>> kBogus = {
        {"return list, floob", "`return list` takes `all` and nothing else"},
        {"svmat A, floob", "svmat takes a type and names(), and floob is neither"},
        {"mkmat xx yy, matrix(BM) floob", "floob is not one of mkmat's own options"},
        {"matlist C, floob", "matlist takes format() and its siblings"},
    };
    for (const auto& bg : kBogus) {
      piet::SessionState session(workdir);
      (void)exec("clear\nset obs 3\ngen double xx = _n\ngen double yy = _n * 2\n"
                 "matrix A = (1,2\\3,4\\5,6)\nmatrix C = (1,2\\3,4)\n",
                 session);
      auto r = exec(bg.first + "\n", session);
      auto err = firstOfType(r.blocks, "error");
      check(!err.empty(), "RESULTS REFUSE: `" + bg.first + "` is refused -- " + bg.second);
      check(err.value("code", std::string()) == "unsupported_option",
            "RESULTS REFUSE: ...under the `unsupported_option` code, the one eval::coarseRc maps "
            "to r(198) -- got `" + err.value("code", std::string()) + "`");
    }
  }

  fs::remove_all(workdir, ec);
}


// ── AER-SCOPE1 bundle H prerequisite: the e()/r() MATRIX LAYER (2026-09-05) ──
//
// Measured before writing a line of H2: the matrix layer was narrower than the
// plan assumed, and in a way no gate could see. e-matrices were effectively
// WRITE-ONLY — `matlist e(...)` and `matrix X = e(...)` both hard-coded their
// regex to `[bV]`, `ereturn list` read neither session.eval.eScalars nor
// eMatrices (it rendered the regression coeftable and a fixed nine-scalar map),
// and no syntax anywhere could read an ELEMENT out of a posted class matrix
// because `\b([A-Za-z_]\w*)\s*\[` stops at the ')' in `e(Ev)[1,1]`. r-class
// matrices did not exist at all. So pca could have posted seven matrices and
// eight macros, sktest could have posted r(table), every gate would have stayed
// green, and none of it would have been readable — the
// publish-what-nobody-can-read failure that the bundle-H errata exists to stop.
//
// This bundle pins the layer itself, seeded directly into the stores rather
// than through pca/sktest, so it fails for ONE reason (the plumbing) and keeps
// failing if a later refactor narrows it again.
void testMatrixClassLayer() {
  std::printf("[selftest] --- AER-SCOPE1 bundle H prereq: e()/r() matrix layer ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_matrixlayer").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto echoText = [](const auto& blocks) {
    std::string s;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "echo")
        s += b.value("text", std::string());
    return s;
  };
  auto displayText = [](const auto& blocks) {
    std::string s;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "display")
        s += b.value("text", std::string());
    return s;
  };
  auto anyError = [](const auto& blocks) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return true;
    return false;
  };
  // A 2x3 with a distinct value per cell, so an index mix-up (row/col swap,
  // off-by-one) shows up as a WRONG NUMBER rather than as a pass.
  auto mk23 = []() {
    piet::eval::EvalMatrix m;
    m.data = {{11.0, 12.0, 13.0}, {21.0, 22.0, 23.0}};
    m.rownames = {"r1", "r2"};
    m.colnames = {"c1", "c2", "c3"};
    return m;
  };

  // (a) e(<name>) beyond b/V — the pca shape.
  {
    piet::SessionState session(workdir);
    session.eval.eMatrices.emplace_back("Ev", mk23());

    auto ml = piet::dispatch("matlist e(Ev)", session);
    check(!anyError(ml), "H-prereq: `matlist e(Ev)` is not an error (was rc 111: regex was [bV])");
    const std::string mlText = echoText(ml) + displayText(ml);
    check(mlText.find("11") != std::string::npos && mlText.find("23") != std::string::npos,
          "H-prereq: `matlist e(Ev)` renders the posted cells");

    auto md = piet::dispatch("matrix EVCOPY = e(Ev)", session);
    check(!anyError(md), "H-prereq: `matrix EVCOPY = e(Ev)` is accepted");
    const piet::eval::EvalMatrix* copied = session.eval.findMatrix("EVCOPY");
    check(copied != nullptr, "H-prereq: `matrix X = e(Ev)` lands in the user matrix store");
    if (copied != nullptr) {
      check(copied->data.size() == 2 && copied->data[0].size() == 3,
            "H-prereq: the copy keeps 2 x 3");
      check(copied->data[1][2] == 23.0, "H-prereq: the copy keeps the VALUES (not a fresh zero)");
      check(copied->colnames.size() == 3 && copied->colnames[2] == "c3",
            "H-prereq: the copy keeps the stripe");
    }
  }

  // (b) element access + colsof/rowsof through the class wrapper. This is the
  // path that did not exist in ANY form, so each assertion is a live number.
  {
    piet::SessionState session(workdir);
    session.eval.eMatrices.emplace_back("Ev", mk23());
    session.eval.rMatrices.emplace_back("table", mk23());

    (void)piet::dispatch("scalar h_e12 = e(Ev)[1,2]", session);
    check(session.eval.scalars.get("h_e12", "") == "12",
          "H-prereq: `e(Ev)[1,2]` reads 12 (row-major, 1-based) — got '" +
              session.eval.scalars.get("h_e12", "<unset>") + "'");
    (void)piet::dispatch("scalar h_e23 = e(Ev)[2,3]", session);
    check(session.eval.scalars.get("h_e23", "") == "23",
          "H-prereq: `e(Ev)[2,3]` reads 23 — got '" +
              session.eval.scalars.get("h_e23", "<unset>") + "'");
    (void)piet::dispatch("scalar h_r21 = r(table)[2,1]", session);
    check(session.eval.scalars.get("h_r21", "") == "21",
          "H-prereq: `r(table)[2,1]` reads 21 — got '" +
              session.eval.scalars.get("h_r21", "<unset>") + "'");
    (void)piet::dispatch("scalar h_cols = colsof(e(Ev))", session);
    check(session.eval.scalars.get("h_cols", "") == "3",
          "H-prereq: `colsof(e(Ev))` == 3 — got '" +
              session.eval.scalars.get("h_cols", "<unset>") + "'");
    (void)piet::dispatch("scalar h_rows = rowsof(r(table))", session);
    check(session.eval.scalars.get("h_rows", "") == "2",
          "H-prereq: `rowsof(r(table))` == 2 — got '" +
              session.eval.scalars.get("h_rows", "<unset>") + "'");
  }

  // (c) `return list` grows a matrices: section — but ONLY when one was posted.
  // The conditional is the point: every frozen return-list fixture in the tree
  // was captured without that section, so an unconditional header would have
  // forced a whole-tree re-capture to say "no matrices" — the re-pin that hides
  // real drift.
  {
    piet::SessionState session(workdir);
    // The listing is a "display" block, as output is (BUG-1505); the "echo" is
    // only the command line.
    const std::string bare = displayText(piet::dispatch("return list", session));
    check(bare.find("matrices:") == std::string::npos,
          "H-prereq: `return list` with no r-matrix prints NO matrices: section "
          "(existing fixtures stay byte-identical)");

    session.eval.rMatrices.emplace_back("table", mk23());
    const std::string withMat = displayText(piet::dispatch("return list", session));
    check(withMat.find("matrices:") != std::string::npos,
          "H-prereq: `return list` prints matrices: once an r-matrix exists");
    check(withMat.find("r(table)") != std::string::npos && withMat.find("2 x 3") != std::string::npos,
          "H-prereq: `return list` names r(table) with its dims");

    // `return clear` outside a program leaves r() alone (w2o_rnames.log:528 N34);
    // inside one it empties r(), matrices included.
    (void)piet::dispatch("return clear", session);
    check(!session.eval.rMatrices.empty(),
          "H-prereq: top-level `return clear` leaves r() -- r-matrices included -- alone (N34)");
  }

  // (d) `ereturn list` reports what a NON-regression estimator posted. Before
  // this it was driven entirely by the coeftable + a fixed scalar map, so a pca
  // would have printed nothing of its own.
  {
    piet::SessionState session(workdir);
    // `fatalities` is the ONLY bundled dataset (sysuse auto is not available in
    // this build) -- the same fixture the bundle B legs above use.
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("regress income beertax", session);  // gives us a LastModel
    session.eval.eScalars.set("rho", 0.908259483427898);
    session.eval.eStrings.set("Ctype", "correlation");
    session.eval.eMatrices.emplace_back("Ev", mk23());

    auto elBlocks = piet::dispatch("ereturn list", session);
    const std::string el = displayText(elBlocks);  // a display block (BUG-1505)
    const std::string elWhy = el.empty() ? (" -- ereturn list produced no display; blocks=" +
                                            nlohmann::json(elBlocks).dump().substr(0, 240))
                                         : (" -- got:" + el);
    check(el.find("e(rho)") != std::string::npos,
          "H-prereq: `ereturn list` prints a handler-posted e-scalar" + elWhy);
    check(el.find("e(Ctype)") != std::string::npos && el.find("correlation") != std::string::npos,
          "H-prereq: `ereturn list` prints a handler-posted e-macro" + elWhy);
    check(el.find("e(Ev)") != std::string::npos && el.find("2 x 3") != std::string::npos,
          "H-prereq: `ereturn list` prints a handler-posted e-matrix with dims" + elWhy);
    check(el.find("e(b)") != std::string::npos,
          "H-prereq: `ereturn list` still prints the coeftable-derived e(b)" + elWhy);
  }

  // (e) the negative arms. A name nobody posted must still be a clean refusal,
  // not a silent empty matrix — the generalised regex must not turn "not found"
  // into "found nothing".
  {
    piet::SessionState session(workdir);
    session.eval.eMatrices.emplace_back("Ev", mk23());
    check(anyError(piet::dispatch("matlist e(nosuch)", session)),
          "H-prereq: `matlist e(nosuch)` still errors");
    check(anyError(piet::dispatch("matlist r(table)", session)),
          "H-prereq: `matlist r(table)` errors when no r-matrix was posted");
    // Stata reads an absent r(NAME) in a matrix definition as the missing
    // value: `matrix T = r(nosuch)` is rc 0 with a 1x1 `.` (w2f_residuals.log
    // :1675 e_matrix_nosuch; w2n_followups.log:938 E05), never an empty matrix.
    check(!anyError(piet::dispatch("matrix Z = r(nosuch)", session)),
          "H-prereq: `matrix Z = r(nosuch)` is rc 0 (w2f_residuals.log:1675)");
    {
      const auto* z = session.eval.findMatrix("Z");
      check(z != nullptr && z->data.size() == 1 && z->data[0].size() == 1 &&
                (std::isnan(z->data[0][0]) || z->data[0][0] >= piet::eval::kStataMissingSentinel),
            "H-prereq: `matrix Z = r(nosuch)` defines a 1x1 missing matrix, not an empty one");
    }
    // findMatrix has ALWAYS searched eMatrices as well as the user store, so a
    // posted e-matrix is reachable bare -- pre-existing behaviour, asserted here
    // so that a future narrowing of findMatrix is a deliberate change and not a
    // silent one. rMatrices is deliberately NOT in that chain: r() is replaced
    // wholesale by the next r-class command, and letting `matlist table` answer
    // with r(table) would make a user matrix named `table` unaddressable.
    check(session.eval.findMatrix("Ev") != nullptr,
          "H-prereq: findMatrix still reaches e-matrices (unchanged)");
    check(session.eval.findMatrix("table") == nullptr,
          "H-prereq: findMatrix does NOT reach r-matrices (bare `table` stays the user store)");
  }

  fs::remove_all(workdir, ec);
}


void testC3BS039EreturnList() {
  std::printf("[selftest] --- C3/BS-039 `ereturn list` display-only e() dump ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c3_bs039").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen high = (beertax > .5)", session);
  (void)piet::dispatch("regress income beertax", session);

  auto blocks = piet::dispatch("ereturn list", session);
  bool sawError = false;
  std::string text;
  for (const auto& b : blocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") sawError = true;
    // A "display" block since BUG-1505 (it was "echo", the command-line type).
    if (b.is_object() && b.value("type", std::string()) == "display" && b.contains("text") &&
        b.value("text", std::string()).find("scalars:") != std::string::npos) {
      text = b.value("text", std::string());
    }
  }
  check(!sawError, "C3/BS-039: `ereturn list` after a real regression no longer honest-rejects "
        "as an unsupported command");
  check(!text.empty(), "C3/BS-039: `ereturn list` returns a display block with the e() dump");

  // Oracle-pinned scalars PIET actually tracks (n_obs=336, df_m=1, df_r=334,
  // F~=62.68, r2~=0.1580, r2_a~=0.1555 -- ll/ll_0/rank are NOT tracked and
  // must be honestly ABSENT).
  check(text.find("e(N)") != std::string::npos && text.find("336") != std::string::npos,
        "C3/BS-039: e(N)=336 present");
  check(text.find("e(df_m)") != std::string::npos, "C3/BS-039: e(df_m) present");
  check(text.find("e(df_r)") != std::string::npos, "C3/BS-039: e(df_r) present");
  check(text.find("e(F)") != std::string::npos, "C3/BS-039: e(F) present");
  check(text.find("e(r2)") != std::string::npos && text.find("e(r2_a)") != std::string::npos,
        "C3/BS-039: e(r2) and e(r2_a) both present");
  // e(ll)/e(ll_0) are posted now (r150 WS-G, the ordered e() name lists):
  // regress posts them ([R] regress, Stored results; StataBE 19.5 e(scalars)
  // `N sum_w df_m df_r F r2 rmse mss rss r2_a ll ll_0 rank`), as the Gaussian
  // log likelihoods -N/2 (1 + ln 2pi + ln(SS/N)) of the fit (SS = rss) and of
  // the constant-only fit (SS = mss + rss) -- StataBE regress y x1 x2 on the
  // w2s_multiway_class fixture: e(ll) -429.7924086070367 from rss
  // 1249.565276772959, N 180. Checked here against that closed form from the
  // fit's own N/rss/mss. e(rank) is still not dumped by `ereturn list`.
  check(text.find("e(ll)") != std::string::npos && text.find("e(ll_0)") != std::string::npos &&
            text.find("e(rank)") == std::string::npos,
        "C3/BS-039: e(ll)/e(ll_0) are posted after regress, e(rank) is not dumped");
  {
    const double* ll = session.eval.eScalars.find("ll");
    const double* ll0 = session.eval.eScalars.find("ll_0");
    const auto& lmr = session.lastModel();
    bool ok = ll != nullptr && ll0 != nullptr && lmr.has_value() && lmr->result.has_value() &&
              lmr->result->is_object() && lmr->result->contains("rss") &&
              lmr->result->contains("mss") && lmr->result->contains("n_obs");
    if (ok) {
      const double n = (*lmr->result)["n_obs"].get<double>();
      const double rss = (*lmr->result)["rss"].get<double>();
      const double mss = (*lmr->result)["mss"].get<double>();
      const double c = 1.0 + std::log(2.0 * 3.14159265358979323846);
      const double wantLl = -0.5 * n * (c + std::log(rss / n));
      const double wantLl0 = -0.5 * n * (c + std::log((mss + rss) / n));
      ok = n == 336.0 && std::fabs(*ll - wantLl) <= 1e-12 * std::fabs(wantLl) &&
           std::fabs(*ll0 - wantLl0) <= 1e-12 * std::fabs(wantLl0) && *ll > *ll0;
    }
    check(ok, "C3/BS-039: e(ll)/e(ll_0) equal -N/2 (1 + ln 2pi + ln(SS/N)) for SS = rss and "
              "mss + rss (N 336)");
  }

  // macros: section.
  check(text.find("macros:") != std::string::npos, "C3/BS-039: macros: section present");
  check(text.find("e(depvar)") != std::string::npos && text.find("\"income\"") != std::string::npos,
        "C3/BS-039: e(depvar)=\"income\" present");
  check(text.find("e(cmd)") != std::string::npos, "C3/BS-039: e(cmd) present");

  // matrices: section (dims only, derived from the coeftable's own length).
  check(text.find("matrices:") != std::string::npos, "C3/BS-039: matrices: section present");
  check(text.find("e(b)") != std::string::npos && text.find("1 x 2") != std::string::npos,
        "C3/BS-039: e(b) dims 1 x 2 (2 coefficients: beertax + intercept)");
  check(text.find("e(V)") != std::string::npos && text.find("2 x 2") != std::string::npos,
        "C3/BS-039: e(V) dims 2 x 2");

  // Regression guard: `ereturn list` with nothing estimated is an honest
  // reject, not an empty/misleading dump.
  {
    std::string freshWorkdir = workdir + "_fresh";
    std::error_code ec2;
    fs::create_directories(freshWorkdir, ec2);
    piet::SessionState freshSession(freshWorkdir);
    auto blocksNone = piet::dispatch("ereturn list", freshSession);
    bool sawNoModelError = false;
    for (const auto& b : blocksNone) {
      if (b.is_object() && b.value("type", std::string()) == "error") sawNoModelError = true;
    }
    check(sawNoModelError,
          "C3/BS-039 regression guard: `ereturn list` with no estimation in memory is an "
          "honest reject");
    fs::remove_all(freshWorkdir, ec2);
  }

  fs::remove_all(workdir, ec);
}


void testG_BUG373DeclaredReturnsLock() {
  std::printf("[selftest] --- BUG-373 lock: r-class returns measured against a REAL StataBE "
              "capture (inspect / levelsof / dfuller / wntestq) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug373").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  // The values below are not mine. They were captured from StataBE 19.5 running
  // the same commands on the same fixture (internal notes stata-capture-20260831), which
  // is the whole point: a capture gate compares what a command PRINTS, so a
  // command that computes correctly and publishes nothing stays green forever.
  // The sweep found 97 such names across 26 commands; these are the four wired.
  (void)piet::dispatch("sysuse fatalities, clear", session);

  auto rval = [&session](const char* nm) {
    auto blocks = piet::dispatch(std::string("display r(") + nm + ")", session);
    const auto* d = firstBlockOfTypeD1(blocks, "display");
    return d == nullptr ? std::string("<none>") : d->value("text", std::string());
  };

  // inspect — a command written the day before the sweep, computing every one of
  // these and dropping them on the floor.
  {
    auto blocks = piet::dispatch("inspect beertax", session);
    check(firstBlockOfTypeD1(blocks, "error") == nullptr, "BUG-373: `inspect` runs");
    check(rval("N") == "336" && rval("N_neg") == "0" && rval("N_0") == "0" &&
              rval("N_pos") == "336" && rval("N_posint") == "0",
          "BUG-373: inspect publishes r(N)=336 r(N_neg)=0 r(N_0)=0 r(N_pos)=336 r(N_posint)=0 "
          "— every value matching the live StataBE capture");
  }
  // levelsof — r(levels) was wired long ago; the COUNTS a loop actually needs
  // were not.
  {
    (void)piet::dispatch("levelsof beertax", session);
    check(rval("r") == "302" && rval("N") == "336",
          "BUG-373: levelsof publishes r(r)=302 distinct levels and r(N)=336 observations "
          "(both exact against the Stata capture)");
  }
  // dfuller — the numbers were already right to every digit Stata prints; only
  // the publication was missing.
  {
    (void)piet::dispatch("keep if state == \"al\"", session);
    (void)piet::dispatch("tsset year", session);
    auto blocks = piet::dispatch("dfuller fatal", session);
    check(firstBlockOfTypeD1(blocks, "error") == nullptr, "BUG-373: `dfuller` runs");
    check(rval("N") == "6" && rval("lags") == "0",
          "BUG-373: dfuller publishes r(N)=6 and r(lags)=0");
    // Stata publishes the critical values under BOTH spellings; a consumer that
    // reads r(cv5) must not find an absence where r(cv_5) exists.
    check(rval("cv_5") == "-3" && rval("cv5") == "-3",
          "BUG-373: dfuller publishes the critical values under BOTH of Stata's spellings "
          "(r(cv_5) and r(cv5))");
    const std::string zt = rval("Zt");
    check(zt.rfind("-1.602", 0) == 0,
          "BUG-373: dfuller r(Zt) is the statistic itself (-1.6020263, Stata: -1.60202628564971) "
          "— got " + zt);
  }
  {
    auto blocks = piet::dispatch("wntestq fatal", session);
    check(firstBlockOfTypeD1(blocks, "error") == nullptr, "BUG-373: `wntestq` runs");
    check(rval("df") == "1" && rval("stat") != "<none>" && rval("p") != "<none>",
          "BUG-373: wntestq publishes r(df)/r(stat)/r(p) — Stata calls the Q statistic r(stat)");
  }

  // ---- the second wave: the remaining commands the sweep named -------------
  // Every expected value below is from the SAME live StataBE capture.
  {
    std::string wd2 = (fs::temp_directory_path() / "piet_native_selftest_bug373b").u8string();
    std::error_code ec2;
    fs::remove_all(wd2, ec2);
    fs::create_directories(wd2, ec2);
    piet::SessionState s2(wd2);
    auto rv = [&s2](const char* nm) {
      auto b = piet::dispatch(std::string("display r(") + nm + ")", s2);
      const auto* d = firstBlockOfTypeD1(b, "display");
      return d == nullptr ? std::string("<none>") : d->value("text", std::string());
    };
    (void)piet::dispatch("sysuse fatalities, clear", s2);

    // tsset/xtset were the largest single block the sweep found (10 + 14 names)
    // and published nothing at all. tmin/tmax come from the data, not a guess.
    (void)piet::dispatch("gen t = year", s2);
    (void)piet::dispatch("keep if state == \"al\"", s2);
    (void)piet::dispatch("tsset year", s2);
    check(rv("tdelta") == "1" && rv("tmin") == "1982" && rv("tmax") == "1988" &&
              rv("gaps") == "0",
          "BUG-373: tsset publishes r(tdelta)=1 r(tmin)=1982 r(tmax)=1988 r(gaps)=0 "
          "(all four exact against the StataBE capture)");
    {
      auto b = piet::dispatch("local tv `r(timevar)'", s2);
      (void)b;
      check(s2.eval.locals.get("tv", "") == "year",
            "BUG-373: tsset publishes r(timevar) as a STRING r-class result");
    }

    (void)piet::dispatch("sysuse fatalities, clear", s2);
    (void)piet::dispatch("egen long id = group(state)", s2);
    (void)piet::dispatch("xtset id year", s2);
    check(rv("imin") == "1" && rv("imax") == "48" && rv("tmin") == "1982" &&
              rv("tmax") == "1988",
          "BUG-373: xtset publishes the PANEL edges too — r(imin)=1 r(imax)=48 alongside the "
          "time edges");

    // ttest: the confidence bounds and BOTH one-sided p's, which is what makes a
    // directional test readable from a do-file.
    (void)piet::dispatch("sysuse fatalities, clear", s2);
    (void)piet::dispatch("ttest beertax == .5", s2);
    check(rv("lb_1").rfind("0.4619", 0) == 0 || rv("lb_1").rfind(".4619", 0) == 0,
          "BUG-373: ttest publishes r(lb_1) = .4619773 (Stata: .46197733145176356) — got " +
              rv("lb_1"));
    check(rv("ub_1").rfind(".5645", 0) == 0 || rv("ub_1").rfind("0.5645", 0) == 0,
          "BUG-373: ttest publishes r(ub_1) = .5645346 (Stata: .5645346363734519)");
    check(rv("p_l").rfind(".6942", 0) == 0 && rv("p_u").rfind(".3057", 0) == 0,
          "BUG-373: ttest publishes BOTH one-sided p's — r(p_l) .6942831 and r(p_u) .3057169, "
          "which sum to 1 by construction");

    // The r(N) family. For a RESHAPING verb this is the count BEFORE it ran:
    // collapse leaves one row and still reports 336.
    (void)piet::dispatch("sysuse fatalities, clear", s2);
    (void)piet::dispatch("collapse beertax", s2);
    check(rv("N") == "336",
          "BUG-373: collapse publishes r(N)=336 — the rows it PROCESSED, not the one row it "
          "left behind");
    (void)piet::dispatch("sysuse fatalities, clear", s2);
    (void)piet::dispatch("contract beertax", s2);
    check(rv("N") == "336", "BUG-373: contract publishes r(N)=336");

    // tab1/tab2 emit a different block shape than tabulate and so fell through
    // the existing r-fill entirely.
    (void)piet::dispatch("sysuse fatalities, clear", s2);
    (void)piet::dispatch("tab1 beertax", s2);
    check(rv("N") == "336" && rv("r") == "302",
          "BUG-373: tab1 publishes r(N)=336 and r(r)=302 — its multi-table shape used to miss "
          "the single-table r-fill");

    // winsor2's cut values: exactly the p1/p99 it clipped at, computed BEFORE the
    // mutation because afterwards the column has been clipped to them.
    (void)piet::dispatch("sysuse fatalities, clear", s2);
    (void)piet::dispatch("winsor2 beertax", s2);
    check(rv("r1").rfind(".0476", 0) == 0 && rv("r2").rfind("2.4152", 0) == 0,
          "BUG-373: winsor2 publishes r(r1)=.0476695 r(r2)=2.4152541 — the cut values, matching "
          "the StataBE capture to every digit it printed");

    // estat vif: one name/value pair per regressor, 1-based.
    (void)piet::dispatch("sysuse fatalities, clear", s2);
    (void)piet::dispatch("regress fatal beertax unemp", s2);
    (void)piet::dispatch("estat vif", s2);
    {
      (void)piet::dispatch("local n1 `r(name_1)'", s2);
      check(s2.eval.locals.get("n1", "") == "beertax" && rv("vif_1") != "<none>",
            "BUG-373: `estat vif` publishes r(name_K)/r(vif_K) per regressor");
    }

    // datasignature's whole point is the signature, and it was printed without
    // being published.
    (void)piet::dispatch("sysuse fatalities, clear", s2);
    (void)piet::dispatch("datasignature", s2);
    {
      (void)piet::dispatch("local sg `r(datasignature)'", s2);
      check(s2.eval.locals.get("sg", "").find(':') != std::string::npos,
            "BUG-373: datasignature publishes r(datasignature) — a do-file can compare it now");
    }
    fs::remove_all(wd2, ec2);
  }

  fs::remove_all(workdir, ec);
}
