// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: data commands.
#include "selftest_support.h"
#include "selftest_cases.h"


void testBS002BysortParentheticalSort() {
  std::printf("[selftest] --- BS-002 `bysort V (sortvar):` parenthetical strip ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs002").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);

  auto blocks = piet::dispatch("bysort state (year): gen bs002g1 = _N", session);
  bool sawError = false;
  bool sawMutation = false;
  for (const auto& b : blocks) {
    if (!b.is_object()) continue;
    if (b.value("type", std::string()) == "error") sawError = true;
    if (b.value("type", std::string()) == "data_mutation") sawMutation = true;
  }
  check(!sawError,
        "BS-002: `bysort state (year): gen ...` no longer errors "
        "(the parenthetical no longer corrupts the by-group list)");
  check(sawMutation, "BS-002: `bysort state (year): gen ...` produced a data_mutation block");
  check(session.hasData() && session.datastore()->has("bs002g1"),
        "BS-002: the new column was actually created");

  // Stronger check: `(year)` must be SORT-ONLY, not folded into the
  // group-by key. fatalities is a state-year panel (~1 row per (state,year)
  // pair) — if BS-002's fix accidentally left `year` in the group-by list
  // too, per-row _N would differ from grouping by `state` alone.
  (void)piet::dispatch("bysort state: gen bs002g2 = _N", session);
  auto cntBlocks = piet::dispatch("count if bs002g1 != bs002g2", session);
  std::string diffText;
  for (const auto& b : cntBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "display") diffText = b.value("text", std::string());
  }
  check(diffText == "0",
        "BS-002: grouping by `state (year)` matches grouping by `state` alone "
        "(year is sort-only, not a second group-by key)");

  fs::remove_all(workdir, ec);
}


// Sibling of testAerResultsBundle above, and here for the same reason: all ten
// floors.json rows of AER bundle C are `no_fixture` (the floor VALUE has to be
// captured on the post-merge binary), so this function, pangram S27, three
// freeze_extra.do stanzas and internal script probe_gtools.do carry the
// bundle until a --counters batch captures it.
//
// THE SHARPEST ASSERTION AVAILABLE FOR THIS BUNDLE IS EQUIVALENCE, so it leads.
// Every row here is an ALIAS: `gcollapse` is not a new answer, it is `collapse`
// under the name a gtools user already types. That makes the property testable
// in a way an ordinary feature is not -- run the alias and the native command
// in two FRESH sessions over the same data and the blocks that come back must
// be IDENTICAL. A per-command "does it work" check passes on a shim that
// dropped an option, reordered a by(), or forwarded to a neighbouring handler.
// The identity does not.
//
// WHAT THE COMPARISON IGNORES, AND WHY EXACTLY TWO THINGS. (1) The command
// ECHO, filtered on the text's own leading '.' -- the same rule renderText uses
// in testAerResultsBundle above, and for the same reason: some printers reuse
// the "echo" block TYPE for real output, so a type filter would throw away the
// thing under test. (2) The alias SPELLING itself, rewritten to the native
// spelling before the compare, because a block that records which command
// produced it would otherwise differ on the one byte an alias is entitled to
// differ on. Everything else must match byte for byte.
//
// EVERY EQUIVALENCE LEG ALSO ASSERTS ITS SIGNATURE IS NON-EMPTY. Two commands
// that both did nothing at all compare equal, and an equality check that can be
// satisfied by silence is the empty-golden shape -- it certifies the handler
// that stopped working. `gisid` is silent on success, so its equivalence is
// taken on the REFUSING form (a duplicated key), where there is output to
// compare; its success form is asserted separately as "no error block".
//
// Deliberately NOT asserted anywhere below: the number of blocks. Every
// dispatched line also emits an echo, so a block count measures the echo (an
// earlier bundle in this campaign learned that). Every check here reads block
// CONTENT.
//
// THE OPTION LEGS ARE THE ONES THAT CATCH THE DEFECT THIS CAMPAIGN SHIPPED. A
// gtools option is one of three kinds and each has its own correct outcome, so
// each has its own leg. `fast` describes only SPEED and is DROPPED -- asserted
// as "the result is identical with and without it", because an option that was
// accepted as an unknown prefix and then ignored also returns rc 0, which is
// how `ivreg, ro` once ran WITHOUT robust standard errors and reported no
// problem. `nq()` is a RENAME of `nquantiles()` -- asserted by VALUE against
// summarize's own p25, because an option dropped on the floor falls back on the
// engine default and still returns rc 0. `merge` would change the ANSWER and is
// REFUSED -- asserted on the code AND on the rc, because eval::coarseRc answers
// 199 only for code == "unsupported_command" or a reason spelled "r(NNN)", so a
// refusal built with the default engine_error code arrives as rc 1 and a
// do-file branching on 199 silently takes the other branch.
void testAerGtoolsBundle() {
  std::printf("[selftest] --- AER-SCOPE1 bundle C: gtools/ftools aliases == their native "
              "commands; dropped/renamed/refused options ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_aergtools").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto exec = [](const std::string& src, piet::SessionState& session) {
    return piet::doexec::executeDoSource(src, session, piet::dispatch);
  };
  // Returns a NULL json when there is no error block rather than a bare
  // `value()` on nothing, which throws and would abort the whole run instead of
  // failing one check (the BUG-129 trap the graph legs earlier in this file
  // record). ordered_json rather than json because eval::coarseRc takes that.
  auto firstError = [](const nlohmann::ordered_json& blocks) -> nlohmann::ordered_json {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return b;
    return nlohmann::ordered_json();
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
  // The comparable part of a block list: see this function's header for the
  // two exclusions and why there are exactly two.
  auto signature = [](const nlohmann::ordered_json& blocks, const std::string& alias,
                      const std::string& native) {
    std::string out;
    for (const auto& b : blocks) {
      if (!b.is_object()) continue;
      const std::string t = b.value("text", std::string());
      if (!t.empty() && t[0] == '.') continue;
      out += b.dump();
      out += "\n";
    }
    if (!alias.empty()) {
      size_t at = 0;
      while ((at = out.find(alias, at)) != std::string::npos) {
        out.replace(at, alias.size(), native);
        at += native.size();
      }
    }
    return out;
  };
  const std::string kLoad = "sysuse fatalities, clear\n";
  // The setup runs in its OWN call in every leg below, so the signatures being
  // compared hold the TARGET line's blocks and not `sysuse`'s.
  auto runFresh = [&](const std::string& setup, const std::string& line) {
    piet::SessionState session(workdir);
    if (!setup.empty()) (void)exec(setup, session);
    return exec(line, session);
  };

  // --- four of the five equivalence legs. alias == native, block for block.
  // The fifth is `gisid`, and it is separate below rather than in this table
  // because it is SILENT on success: a table-driven identity over two empty
  // block lists would pass on two handlers that had both stopped working.
  {
    // {alias name, native name, alias line, native line}. A vector of vectors
    // rather than std::array because <array> is not among this file's includes
    // and this lane may not add one.
    //
    // The native NAME is written out rather than derived by stripping the
    // leading g/f, and that is not verbosity: it happens to work for these four
    // and it is wrong for the rest of the bundle -- `gquantiles`'s native
    // command is `xtile`, `gstats`'s is two commands, and a derived name would
    // have silently rewritten the wrong token in the signature the moment
    // either was added to this table.
    const std::vector<std::vector<std::string>> kPairs = {
        {"gcollapse", "collapse", "gcollapse (mean) fatal beertax, by(state)\n",
         "collapse (mean) fatal beertax, by(state)\n"},
        {"gegen", "egen", "gegen gm = mean(fatal), by(state)\n",
         "egen gm = mean(fatal), by(state)\n"},
        {"gduplicates", "duplicates", "gduplicates report state\n", "duplicates report state\n"},
        {"glevelsof", "levelsof", "glevelsof state, local(lv)\n", "levelsof state, local(lv)\n"},
    };
    for (const auto& p : kPairs) {
      const std::string alias = p[0];
      const std::string native = p[1];
      const auto aRun = runFresh(kLoad, p[2]);
      const auto bRun = runFresh(kLoad, p[3]);
      const std::string aSig = signature(aRun.blocks, alias, native);
      const std::string bSig = signature(bRun.blocks, std::string(), std::string());
      // Non-empty is NOT enough on its own: an error block is output too, so
      // two identical FAILURES satisfy both the emptiness guard and the
      // identity below. That is the same unfalsifiable shape the emptiness
      // guard exists to close, arriving from the other side -- so the four
      // legs in this table (all of which are supposed to SUCCEED) assert the
      // absence of an error on both sides first. `gisid` is exactly the leg
      // that cannot make this assertion, which is why it is not in this table.
      //
      // errText, not `firstError(...).value("message", ...)` inline: check()'s
      // message argument is built EAGERLY, on the passing path too, and
      // nlohmann's value() on a NULL json throws type_error.306 rather than
      // returning the default -- which would abort the run on every GREEN pass.
      auto errText = [](const nlohmann::ordered_json& e) {
        return e.is_object() ? e.value("message", std::string()) : std::string("<none>");
      };
      check(firstError(aRun.blocks).is_null(),
            "GTOOLS EQUIV: `" + alias + "` SUCCEEDS on this data -- the identity below is "
            "satisfied by two identical failures, so the failure case is ruled out here. Got `" +
                errText(firstError(aRun.blocks)) + "`");
      check(firstError(bRun.blocks).is_null(),
            "GTOOLS EQUIV: ...and so does the native `" + native +
                "`, so the pair being compared is two commands that both reached an answer. Got `" +
                errText(firstError(bRun.blocks)) + "`");
      check(!aSig.empty(), "GTOOLS EQUIV: `" + alias +
                               "` returns SOMETHING to compare -- an equality check between two "
                               "empty block lists certifies a handler that did nothing");
      check(aSig == bSig,
            "GTOOLS EQUIV: `" + alias + "` returns exactly what `" + native +
                "` returns on the same data. Alias name and command echo aside, every block must "
                "match -- a shim that dropped an option or reordered by() differs HERE while "
                "still answering rc 0. Got alias `" + aSig + "` vs native `" + bSig + "`");
    }
  }

  // --- gisid: silent on success, so equivalence is taken where there IS
  // output. `state` alone repeats across years; `state year` does not.
  {
    auto okAlias = runFresh(kLoad, "gisid state year\n");
    auto okNative = runFresh(kLoad, "isid state year\n");
    check(firstError(okAlias.blocks).is_null(),
          "GTOOLS EQUIV/gisid: `gisid state year` SUCCEEDS -- state-year is the key of this "
          "dataset");
    check(firstError(okNative.blocks).is_null(),
          "GTOOLS EQUIV/gisid: ...and so does the native `isid state year`, so the pair below "
          "compares two commands that are both reaching a real check");
    const std::string aSig = signature(runFresh(kLoad, "gisid state\n").blocks, "gisid", "isid");
    const std::string bSig =
        signature(runFresh(kLoad, "isid state\n").blocks, std::string(), std::string());
    check(!aSig.empty(),
          "GTOOLS EQUIV/gisid: the DUPLICATED-key form produces output -- this is the form the "
          "equivalence is taken on precisely because the success form produces none");
    check(aSig == bSig, "GTOOLS EQUIV/gisid: `gisid state` refuses exactly as `isid state` does. "
                        "Got alias `" + aSig + "` vs native `" + bSig + "`");
  }

  // --- the by-PREFIX form, which the by() option form does not cover: a shim
  // that stripped the prefix along with the name passes every leg above.
  // Synthetic rather than `sysuse`, because two named by-groups with known
  // means make a wrong grouping visible as a wrong NUMBER, not as an error.
  {
    const std::string kSetup = "clear\nset obs 12\ngen double yy = _n\ngen double region = 1\n"
                               "replace region = 2 in 7/12\n";
    const std::string aSig =
        signature(runFresh(kSetup, "bysort region: gegen m = mean(yy)\n").blocks, "gegen", "egen");
    const std::string bSig = signature(runFresh(kSetup, "bysort region: egen m = mean(yy)\n").blocks,
                                       std::string(), std::string());
    check(!aSig.empty(), "GTOOLS EQUIV/bysort: the prefixed form returns something to compare");
    check(aSig == bSig, "GTOOLS EQUIV/bysort: `bysort region: gegen` == `bysort region: egen`. Got "
                        "alias `" + aSig + "` vs native `" + bSig + "`");
    // ...and the grouping is the RIGHT one. Identity above proves the two
    // spellings agree; it cannot prove they are both correct, and two
    // handlers that both ignored the prefix would also agree.
    piet::SessionState session(workdir);
    (void)exec(kSetup, session);
    auto r = exec("bysort region: gegen m = mean(yy)\n"
                  "display \"AER_G1=\" m[1]\n"
                  "display \"AER_G2=\" m[12]\n",
                  session);
    check(std::fabs(displayValue(r.blocks, "AER_G1=") - 3.5) < 1e-12,
          "GTOOLS EQUIV/bysort: the first by-group's mean is 3.5 (rows 1-6 carry 1..6), so the "
          "prefix actually GROUPED -- a prefix consumed and ignored answers 6.5 here");
    check(std::fabs(displayValue(r.blocks, "AER_G2=") - 9.5) < 1e-12,
          "GTOOLS EQUIV/bysort: ...and the second group's is 9.5, so the two groups are distinct "
          "rather than one group repeated");
  }

  // --- a DROPPED option changes nothing. `fast` is speed-only. ---
  {
    const std::string withFast =
        signature(runFresh(kLoad, "gcollapse (mean) fatal beertax, by(state) fast\n").blocks,
                  std::string(), std::string());
    const std::string without =
        signature(runFresh(kLoad, "gcollapse (mean) fatal beertax, by(state)\n").blocks,
                  std::string(), std::string());
    check(!withFast.empty(),
          "GTOOLS DROP/fast: `gcollapse ..., fast` is ACCEPTED and produces a result -- a refusal "
          "here would be the opposite defect from the one this leg is looking for");
    check(withFast == without,
          "GTOOLS DROP/fast: ...and the result is identical to the same line WITHOUT `fast`. That "
          "option describes speed and nothing else, so dropping it is correct and changing the "
          "answer with it is not. Got `" + withFast + "` vs `" + without + "`");
  }

  // --- a REFUSED option refuses, under the code that maps to r(199), and
  // leaves the dataset alone.
  {
    piet::SessionState session(workdir);
    (void)exec(kLoad, session);
    const double before = displayValue(exec("display \"AER_N=\" _N\n", session).blocks, "AER_N=");
    auto r = exec("gcollapse (mean) fatal, by(state) merge\n", session);
    auto err = firstError(r.blocks);
    check(!err.is_null(),
          "GTOOLS REFUSE/merge: `gcollapse ..., merge` is REFUSED. It writes the aggregate back "
          "onto the source rows instead of replacing them, which is a different ANSWER -- an "
          "option in that class must never be dropped quietly");
    // The code is read into a local BEFORE the check: check()'s message is
    // built eagerly, and nlohmann's value() on the NULL json firstError()
    // returns when there is no error block throws type_error.306 -- which
    // would abort the whole run in place of failing this one check. That is
    // the BUG-129 trap firstError() itself was written to avoid, and an
    // inline value() in the message re-opens it on the failing path.
    const std::string code = err.is_object() ? err.value("code", std::string()) : std::string();
    check(code == "unsupported_command",
          "GTOOLS REFUSE/merge: ...under the `unsupported_command` code, got `" + code + "`");
    const std::string reason = err.is_null() ? std::string() : err.value("reason", std::string());
    check(reason.rfind("refused:", 0) == 0,
          "GTOOLS REFUSE/merge: ...with a reason beginning `refused:`, which is what separates an "
          "answer-changing option from an unimplemented one in this bundle's vocabulary. Got `" +
              reason + "`");
    check(!err.is_null() && piet::eval::coarseRc(err) == "199",
          "GTOOLS REFUSE/merge: ...and it maps to r(199). Asserted separately from the code "
          "because coarseRc answers 199 only for `unsupported_command` or a reason spelled "
          "`r(NNN)`; a refusal built with the default engine_error code arrives as rc 1 and every "
          "do-file branching on 199 takes the wrong branch. Got r(" +
              (err.is_null() ? std::string("<none>") : piet::eval::coarseRc(err)) + ")");
    const double after = displayValue(exec("display \"AER_N=\" _N\n", session).blocks, "AER_N=");
    check(before > 0 && std::fabs(after - before) < 1e-12,
          "GTOOLS REFUSE/merge: ...and it collapsed NOTHING. A refusal that had already aggregated "
          "before it reached the option is the silent half of this class and the error block alone "
          "cannot tell the two apart -- got " + std::to_string(before) + " obs before, " +
              std::to_string(after) + " after");
  }

  // --- gquantiles _pctile fills r(), and the VALUE is the assertion. ---
  {
    piet::SessionState session(workdir);
    (void)exec(kLoad, session);
    // The _pctile form must fill r() and write NO column, so the width is read
    // on both sides of it. A shim that routed _pctile onto the xtile form
    // satisfies every value check below and is caught only here.
    auto varCount = [&](piet::SessionState& s) {
      const nlohmann::ordered_json blocks = piet::dispatch("describe", s);
      for (const auto& b : blocks)
        if (b.is_object() && b.value("type", std::string()) == "describe" &&
            b.contains("variables"))
          return static_cast<int>(b["variables"].size());
      return -1;
    };
    const int widthBefore = varCount(session);
    // r(r1) is READ BEFORE `summarize, detail` runs: summarize refills r() and
    // would overwrite the store this leg is about, so the ordering here is
    // load-bearing rather than stylistic.
    auto r = exec("gquantiles fatal, _pctile nq(4)\n"
                  "display \"AER_R1=\" r(r1)\n"
                  "summarize fatal, detail\n"
                  "display \"AER_P25=\" r(p25)\n",
                  session);
    const double r1 = displayValue(r.blocks, "AER_R1=");
    const double p25 = displayValue(r.blocks, "AER_P25=");
    check(!std::isnan(r1),
          "GTOOLS/_pctile: `gquantiles fatal, _pctile nq(4)` FILLS r(r1) -- an r() store left "
          "empty reads as nothing here while the command still returns rc 0");
    check(!std::isnan(p25) && std::fabs(r1 - p25) < 1e-9,
          "GTOOLS/_pctile: ...and r(r1) IS the 25th percentile `summarize, detail` reports. This "
          "is where `nq(4)` is proved to be a RENAME of `nquantiles(4)` rather than a dropped "
          "token: a dropped option falls back on this handler's own kPctileDefaultNq of 2 and "
          "returns rc 0 while r(r1) becomes the MEDIAN. (BUG-370 runs the other way from the way "
          "it is usually misremembered -- Stata's xtile default is 2 and the ENGINE's is 4, which "
          "is why the shim emits nquantiles() explicitly instead of forwarding as typed.) Got r1=" +
              std::to_string(r1) +
              " p25=" + std::to_string(p25));
    const int widthAfter = varCount(session);
    check(widthBefore > 0 && widthAfter == widthBefore,
          "GTOOLS/_pctile: ...and it wrote NO column. The `_pctile` form fills r() and nothing "
          "else, so a shim that forwarded it to the xtile form leaves one variable more here "
          "while every value check above still passes -- got " + std::to_string(widthBefore) +
              " variables before, " + std::to_string(widthAfter) + " after");
  }

  // --- gstats is a MULTIPLEX, so each arm is checked against ITS OWN native
  // target: winsor against winsor2, by value on both tails.
  {
    const std::string kTails = "summarize fatal_w\n"
                               "display \"AER_MIN=\" r(min)\n"
                               "display \"AER_MAX=\" r(max)\n";
    piet::SessionState sa(workdir);
    (void)exec(kLoad, sa);
    auto a = exec("gstats winsor fatal, cuts(5 95)\n" + kTails, sa);
    piet::SessionState sb(workdir);
    (void)exec(kLoad, sb);
    auto b = exec("winsor2 fatal, cuts(5 95)\n" + kTails, sb);
    const double aMin = displayValue(a.blocks, "AER_MIN=");
    const double aMax = displayValue(a.blocks, "AER_MAX=");
    const double bMin = displayValue(b.blocks, "AER_MIN=");
    const double bMax = displayValue(b.blocks, "AER_MAX=");
    check(!std::isnan(aMin) && !std::isnan(aMax),
          "GTOOLS/gstats winsor: `gstats winsor fatal, cuts(5 95)` writes fatal_w -- the column "
          "has to exist before either tail can be compared");
    check(std::fabs(aMin - bMin) < 1e-9,
          "GTOOLS/gstats winsor: the LOWER tail equals `winsor2 fatal, cuts(5 95)`'s. Both tails "
          "are read because a cuts() that arrived half-parsed clamps one end and leaves the other "
          "alone. Got " + std::to_string(aMin) + " vs " + std::to_string(bMin));
    check(std::fabs(aMax - bMax) < 1e-9,
          "GTOOLS/gstats winsor: ...and the UPPER tail equals it too. Got " + std::to_string(aMax) +
              " vs " + std::to_string(bMax));
  }

  // --- the other gstats arm, and a subcommand this multiplex does not have. ---
  {
    piet::SessionState session(workdir);
    (void)exec(kLoad, session);
    auto sum = exec("gstats sum fatal\ndisplay \"AER_SUMN=\" r(N)\n", session);
    check(firstError(sum.blocks).is_null(),
          "GTOOLS/gstats sum: `gstats sum fatal` reaches summarize rather than the multiplex's "
          "reject arm -- the SUBCOMMAND has to travel with the name");
    check(std::fabs(displayValue(sum.blocks, "AER_SUMN=") - 336.0) < 1e-12,
          "GTOOLS/gstats sum: ...and it fills r(N) with this dataset's 336 observations, so the "
          "arm actually SUMMARIZED rather than merely being accepted");
    auto tab = exec("gstats tab state\n", session);
    auto err = firstError(tab.blocks);
    check(!err.is_null(),
          "GTOOLS/gstats tab: an arm this multiplex does not implement is REFUSED as the whole "
          "command. Accepting the name and ignoring the subcommand is the shape that runs "
          "`gstats tab` as a bare summarize and reports no problem");
    check(!err.is_null() && piet::eval::coarseRc(err) == "199",
          "GTOOLS/gstats tab: ...at r(199), got r(" +
              (err.is_null() ? std::string("<none>") : piet::eval::coarseRc(err)) + ")");
  }

  // --- the community-name gate reaches this bundle too. Pattern read from
  // testExtcompatGate below; what is added here is the ISOLATION. `set
  // extcompat off` moves persisted settings, and a gate that writes the
  // operator's real settings file is the BUG-346 class -- so the store is
  // pointed at this function's own workdir for the duration and the previous
  // value is RESTORED rather than unset, because the harness itself pins
  // PIET_SETTINGS_FILE on some runs and clobbering it would break every case
  // after this one.
  //
  // POSIX-ONLY, and the skip is named rather than left to be discovered: the
  // isolation above needs setenv/unsetenv, so on Windows this leg does not
  // run. What is lost there is the bundle-C-specific half only -- testExtcompat
  // Gate below still exercises `set extcompat off` on every platform, so the
  // gate MECHANISM stays covered and it is this bundle's membership in it that
  // goes unchecked.
#ifndef _WIN32
  {
    const char* prevSettings = std::getenv("PIET_SETTINGS_FILE");
    const std::string savedSettings = prevSettings ? prevSettings : "";
    const std::string ownSettings = (fs::path(workdir) / "settings.json").u8string();
    setenv("PIET_SETTINGS_FILE", ownSettings.c_str(), 1);

    piet::SessionState session(workdir);
    (void)exec(kLoad, session);
    (void)piet::dispatch("set extcompat off", session);
    auto blocks = piet::dispatch("gcollapse (mean) fatal, by(state)", session);
    auto err = firstError(blocks);
    check(!err.is_null() && err.value("code", std::string()) == "unsupported_command" &&
              err.value("token", std::string()) == "gcollapse",
          "GTOOLS/extcompat OFF: a gtools name is gated exactly as `reghdfe` is -- these ten rows "
          "are community spellings and the opt-out has to reach them, or the setting promises an "
          "isolation it does not deliver");
    check(!err.is_null() && piet::eval::coarseRc(err) == "199",
          "GTOOLS/extcompat OFF: ...and the gate maps to r(199)");

    // The env var is restored FIRST, then the cache invalidated, then the
    // process default forced -- in that order. Invalidating AFTER the set
    // would re-resolve the flag from whatever the restored store says and
    // silently undo the restore, which is the same "cache invalidates the
    // test's own proxy" shape a cache introduction cost this project once.
    if (!savedSettings.empty()) {
      setenv("PIET_SETTINGS_FILE", savedSettings.c_str(), 1);
    } else {
      unsetenv("PIET_SETTINGS_FILE");
    }
    piet::settings::invalidateExtcompatCache();
    piet::settings::setExtcompatEnabled(true);
    // The restore is verified by DISPATCHING the name again, not by reading
    // the flag back: the flag was written by the line directly above, so
    // reading it proves the setter works and nothing about the gate. This
    // check has to be able to fail, and the cases after this one inherit
    // whatever it leaves behind.
    auto again = piet::dispatch("gcollapse (mean) fatal, by(state)", session);
    auto againErr = firstError(again);
    check(againErr.is_null() || againErr.value("code", std::string()) != "unsupported_command",
          "GTOOLS/extcompat: ...and once the default is restored the name ANSWERS again, so this "
          "function hands the following cases the state they were written against rather than a "
          "session with the community names still switched off");
  }
#endif

  fs::remove_all(workdir, ec);
}


// ── AER-SCOPE1 bundle I (UTIL, 2026-09-04) ───────────────────────────────────
//
// Twelve data-tidying verbs over thirteen registry rows (`unique` and `gunique`
// share one handler). Their failure class is NOT a return code. Every wrong
// implementation this bundle can plausibly ship answers rc 0 and leaves a frame
// that lists correctly: a `unique` that dropped missing instead of counting it,
// a `distinct` that kept it, a `mdesc` that reported the NON-missing count, a
// `missings tag` that wrote an any-missing indicator where Stata writes a
// rowmiss COUNT, a `carryforward` that leaked the last value across a group
// boundary, a `rangestat` that returned the by-group aggregate instead of the
// window, a `tsspell` whose sequence never resets, a `renvarlab` that renamed
// half a varlist before hitting a collision. So no leg below stops at a return
// code: each reads a VALUE back out of the frame, and the value is chosen so
// that the wrong implementation named in its own comment gets it wrong.
//
// FIVE OF THE PLAN'S SEVENTEEN NAMES DO NOT APPEAR HERE AND MUST NOT BE ADDED.
// `dropmiss` and `renvars` have no package under their own name, so the
// campaign's attribution rule gives them no registry row; `strdist`, `matchit`
// and `reclink` are record linkage, out of scope. All five are UNREGISTERED,
// which means they answer a GENERIC r(199) rather than a named refusal -- a
// test asserting a specific reason string for any of them would be asserting
// something the tree does not claim.
//
// THREE HOUSE RULES ARE FOLLOWED LITERALLY, ALL THREE BECAUSE THEY COST TIME:
//   - EVERY session seeds `cwd` as well as `eval.cwd`. resolveUserPath reads
//     SessionState::cwd (handlers.cpp:2040); bundle E seeded only eval.cwd and
//     sixteen checks failed for that one reason. `filelist` is file I/O.
//   - Numeric comparisons report through `fullPrec` (%.17g), never
//     std::to_string, which prints six decimals and renders a genuine
//     eighth-digit difference as two identical strings.
//   - Nothing here asserts an absolute PATH, and missing is `.`, never 0.
//
// WHY THE FIXTURES ARE BUILT WITH `set obs` + `gen` + `replace ... in #/#`
// RATHER THAN `input`: every count below is hand-arithmetic over a frame this
// file states in a comment, and the `in`-range replace is the oldest, most
// heavily frozen way to punch a specific row (BUG-077/D9-in). A fixture built
// out of the bundle-F verb under test in the next function over would make a
// bundle-I failure readable as a bundle-F failure.

void testAerUtilCounts() {
  std::printf("[selftest] --- AER-SCOPE1 bundle I: unique/gunique/distinct/mdesc/missings, "
              "hand-computed off an 8-row frame ---\n");
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_aerutilcounts").u8string();
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
  // NULL rather than a throw when there is no error block -- the BUG-129 trap:
  // check()'s message argument is built EAGERLY, on the passing path too.
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
  auto renderText = [](const auto& blocks) {
    std::string out;
    for (const auto& b : blocks) {
      if (!b.is_object()) continue;
      const std::string t = b.value("text", std::string());
      if (t.empty()) continue;
      out += t;
      out += "\n";
    }
    return out;
  };
  auto displays = [](const auto& blocks) {
    std::vector<std::string> v;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "display")
        v.push_back(b.value("text", std::string()));
    return v;
  };
  // The RAW remainder after `prefix`, so a missing cell can be compared as the
  // string "." -- the house rule that missing is never 0. `displayValue` below
  // parses the same line as a number for the non-missing cases.
  auto displayText = [&](const auto& blocks, const std::string& prefix) -> std::string {
    for (const auto& s : displays(blocks))
      if (s.rfind(prefix, 0) == 0) return s.substr(prefix.size());
    return std::string("<no such display line>");
  };
  auto rOf = [](piet::SessionState& s, const char* key) -> double {
    const double* p = s.eval.rValues.find(key);
    return p ? *p : std::nan("");
  };
  // r() is REPLACED wholesale by the next r-class command (macros.cpp's
  // captureRFromBlock), so every read happens immediately after the command
  // that published it -- never two commands later.
  auto runR = [&](piet::SessionState& s, const std::string& cmd, const char* key) -> double {
    (void)piet::dispatch(cmd, s);
    return rOf(s, key);
  };
  auto nRows = [&](piet::SessionState& s) { return runR(s, "count", "N"); };
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

  // FIXTURE U -- eight rows. Everything asserted in this function is
  // arithmetic over exactly this table, worked out by hand:
  //
  //     row :  1  2  3  4  5  6  7  8
  //     ug  :  1  1  1  2  2  2  3  3
  //     uv  :  5  5  .  7  8  9  4  5
  //     uw  :  1  2  .  4  .  6  7  8
  //
  // uv's missing sits in group 1; uw's two missing (rows 3 and 5) OVERLAP uv's
  // on row 3 and not on row 5. That overlap is deliberate: it makes the number
  // of ROWS carrying a missing (2) differ from the sum of the per-variable
  // counts (3), which is the one shape that separates `missings report`'s
  // observation line from a report that added its own table up.
  static const char* kFixU =
      "clear\n"
      "set obs 8\n"
      "gen ug = 1\n"
      "replace ug = 2 in 4/6\n"
      "replace ug = 3 in 7/8\n"
      "gen uv = 5\n"
      "replace uv = . in 3\n"
      "replace uv = 7 in 4\n"
      "replace uv = 8 in 5\n"
      "replace uv = 9 in 6\n"
      "replace uv = 4 in 7\n"
      "gen uw = _n\n"
      "replace uw = . in 3\n"
      "replace uw = . in 5\n";

  // ── (i) `unique` counts MISSING as one of the values. ──────────────────────
  // uv holds {5, ., 7, 8, 9, 4} over eight records. 6 and 8 are the two numbers
  // this verb prints, and pinning both is what separates them: a handler that
  // published the record count as r(unique) matches one line and not the other.
  // 5 -- the missing-excluded answer, which is `distinct`'s rule and is asserted
  // as such in leg (iv) -- is the wrong answer this leg exists to catch.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixU, s);
    auto r = exec("unique uv\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/counts (i): `unique uv` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const double u = rOf(s, "unique");
    check(u == 6.0,
          "AER-UTIL/counts (i): r(unique) == 6 -- {5, ., 7, 8, 9, 4}. MISSING IS A VALUE here; "
          "excluding it gives 5, which is `distinct`'s rule and a silently different answer. Got " +
              fullPrec(u));
    const double n = rOf(s, "N");
    check(n == 8.0,
          "AER-UTIL/counts (i): ...and r(N) == 8, the record count, which is the SECOND line of "
          "this verb's output. A handler that published one number twice satisfies whichever of "
          "these two checks it happened to match. Got " + fullPrec(n));
    const std::string txt = renderText(r.blocks);
    check(has(txt, "Number of unique values of uv is"),
          "AER-UTIL/counts (i): ...and it renders the unique-values line by name. Got `" + txt +
              "`");
    check(has(txt, "Number of records is"),
          "AER-UTIL/counts (i): ...and the records line under it -- two lines, not one. Got `" +
              txt + "`");
  }

  // ── (ii) a multi-variable `unique` is a TUPLE count. ───────────────────────
  // (ug,uv) over the eight rows is {(1,5),(1,.),(2,7),(2,8),(2,9),(3,4),(3,5)}
  // = 7. Every plausible non-tuple implementation lands somewhere else: first
  // variable only = 3, last variable only = 6, per-variable SUM = 3+6 = 9.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixU, s);
    auto r = exec("unique ug uv\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/counts (ii): `unique ug uv` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const double u = rOf(s, "unique");
    check(u == 7.0,
          "AER-UTIL/counts (ii): r(unique) == 7 over the (ug,uv) TUPLES. 3 = ug alone, 6 = uv "
          "alone, 9 = the two per-variable counts added; all three are plausible numbers a "
          "non-tuple handler prints without complaint. Got " + fullPrec(u));
  }

  // ── (iii) `by() + gen()` writes the group's FIRST observation only. ────────
  //
  // BUG-422 (2026-09-06): this leg used to assert the opposite — a fill across
  // every observation of the group — on the strength of a comment in the handler
  // that said unique.ado does that. Bundle I's own P5 wave pin measured StataBE
  // 19.5 with unique 1.2.4 and settled it: `by `by': gen `gen' = `count' if
  // _n==1`, so five of the eight rows are MISSING and `summarize` reads N=3.
  // Group unique-uv counts: g1 {5,.} = 2, g2 {7,8,9} = 3, g3 {4,5} = 2, so nu is
  // 2 . . 3 . . 2 . — the three assertions below are still three different wrong
  // answers: N==3 fails a whole-group fill (which reads 8), sum==7 fails a fill
  // that wrote the group SIZE (3 + 3 + 2 = 8 here), and the per-row reads fail a
  // write-back that computed in group order and landed positionally.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixU, s);
    auto r = exec("unique uv, by(ug) gen(nu)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/counts (iii): `unique uv, by(ug) gen(nu)` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const double nn = runR(s, "summarize nu", "N");
    check(nn == 3.0,
          "AER-UTIL/counts (iii): nu is non-missing in exactly THREE rows -- one per group, the "
          "group's first observation (unique.ado's `if _n==1`). A whole-group fill reads 8. "
          "Got N " + fullPrec(nn));
    const double sm = runR(s, "summarize nu", "sum");
    check(sm == 7.0,
          "AER-UTIL/counts (iii): ...and nu sums to 7 (2 + 3 + 2, one row per group). A fill that "
          "wrote the group SIZE instead of its unique-value count sums to 8 and passes the count "
          "above. Got " + fullPrec(sm));
    check(displayText(exec("display \"U3A=\" nu[1]\n", s).blocks, "U3A=") == "2",
          "AER-UTIL/counts (iii): ...nu[1] == 2 (group 1: {5, .}, missing counted). Got `" +
              displayText(exec("display \"U3A=\" nu[1]\n", s).blocks, "U3A=") + "`");
    check(displayText(exec("display \"U3B=\" nu[4]\n", s).blocks, "U3B=") == "3",
          "AER-UTIL/counts (iii): ...nu[4] == 3 (group 2's FIRST row: {7, 8, 9}). Got `" +
              displayText(exec("display \"U3B=\" nu[4]\n", s).blocks, "U3B=") + "`");
    check(displayText(exec("display \"U3D=\" nu[5]\n", s).blocks, "U3D=") == ".",
          "AER-UTIL/counts (iii): ...and nu[5] is MISSING -- the second row of group 2 is where a "
          "whole-group fill and Stata's `if _n==1` disagree, so this is the discriminating read. "
          "Got `" + displayText(exec("display \"U3D=\" nu[5]\n", s).blocks, "U3D=") + "`");
    check(displayText(exec("display \"U3C=\" nu[7]\n", s).blocks, "U3C=") == "2",
          "AER-UTIL/counts (iii): ...and nu[7] == 2 (group 3's first row: {4, 5}) -- the per-row "
          "reads are what a positional write-back after an internal group sort gets wrong while "
          "the sum and the count both still agree. Got `" +
              displayText(exec("display \"U3C=\" nu[7]\n", s).blocks, "U3C=") + "`");
  }

  // ── (iv) `distinct` EXCLUDES missing, and `missing` puts it back. ──────────
  // This is the leg that makes leg (i) mean something: over the SAME column the
  // two verbs are contracted to disagree, 5 against 6, and a handler that
  // routed one to the other passes every other check in this function.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixU, s);
    auto r = exec("distinct uv\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/counts (iv): `distinct uv` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const double nd = rOf(s, "ndistinct");
    check(nd == 5.0,
          "AER-UTIL/counts (iv): r(ndistinct) == 5 -- {5, 7, 8, 9, 4}, missing EXCLUDED. 6 is "
          "`unique`'s answer over the same column (leg (i)), so this pair is the whole "
          "distinct-vs-unique contract. Got " + fullPrec(nd));
    const double nt = rOf(s, "N");
    check(nt == 7.0,
          "AER-UTIL/counts (iv): ...and r(N) == 7, the row's TOTAL, which is also missing-free. 8 "
          "here means the total counted rows and the distinct count did not. Got " +
              fullPrec(nt));

    auto r2 = exec("distinct uv, missing\n", s);
    check(firstError(r2.blocks).is_null(),
          "AER-UTIL/counts (iv/missing): `distinct uv, missing` runs clean. Got `" +
              errText(firstError(r2.blocks)) + "`");
    check(rOf(s, "ndistinct") == 6.0,
          "AER-UTIL/counts (iv/missing): ...and `missing` puts it back: 6, agreeing with `unique` "
          "at last. An option that was consumed and ignored leaves 5 here. Got " +
              fullPrec(rOf(s, "ndistinct")));
    check(rOf(s, "N") == 8.0,
          "AER-UTIL/counts (iv/missing): ...over a total of 8, not 7 -- the option has to move "
          "BOTH numbers. Got " + fullPrec(rOf(s, "N")));
  }

  // ── (v) `joint` is a tuple count, and it is not the last per-variable row. ─
  // With `missing` no row is dropped, so this is the same 7-of-8 arithmetic as
  // leg (ii) reached by a different verb. Without `joint` the published row is
  // uv's (5 of 7); with `joint` but no `missing` it is 6 of 7. Three separate
  // numbers, so this check cannot be satisfied by the wrong one.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixU, s);
    auto r = exec("distinct ug uv, joint missing\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/counts (v): `distinct ug uv, joint missing` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(rOf(s, "ndistinct") == 7.0,
          "AER-UTIL/counts (v): r(ndistinct) == 7 over the (ug,uv) tuples. 5 = uv's own row (a "
          "`joint` that was parsed and dropped), 6 = joint with the missing row excluded. Got " +
              fullPrec(rOf(s, "ndistinct")));
    check(rOf(s, "N") == 8.0,
          "AER-UTIL/counts (v): ...over all 8 rows, because `missing` keeps row 3. Got " +
              fullPrec(rOf(s, "N")));
  }

  // ── (vi) `mdesc` publishes the MISSING count, not its complement. ──────────
  // uv is 1 missing of 8 = 12.5%, and 12.5 is exact in binary so it can be
  // compared with ==. The two wrong answers are 7 (the non-missing count, which
  // renders a table of exactly the same shape) and 14.2857... (a percent taken
  // over the non-missing denominator).
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixU, s);
    auto r = exec("mdesc uv\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/counts (vi): `mdesc uv` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(rOf(s, "miss") == 1.0,
          "AER-UTIL/counts (vi): r(miss) == 1 -- the MISSING count. 7, the complement, renders an "
          "identically shaped table and is the reading this leg exists to fail. Got " +
              fullPrec(rOf(s, "miss")));
    check(rOf(s, "total") == 8.0,
          "AER-UTIL/counts (vi): ...r(total) == 8, all rows. Got " + fullPrec(rOf(s, "total")));
    check(rOf(s, "percent") == 12.5,
          "AER-UTIL/counts (vi): ...and r(percent) == 12.5, which is 1/8 as a PERCENT over the "
          "full denominator. 0.125 is the fraction, 14.285714285714286 is the percent over the "
          "non-missing denominator; 12.5 is exact in binary, so == is safe here. Got " +
              fullPrec(rOf(s, "percent")));
  }

  // ── (vii) `missings report` lists only variables that HAVE missing. ────────
  // Rows carrying a missing: 3 and 5 -- two. Per-variable counts: uv 1, uw 2 --
  // three. The fixture's overlap on row 3 is what makes those two numbers
  // differ, so "2 observations" cannot be produced by adding the table up. And
  // ug, which has none, must not appear at all: a report that tabulated every
  // variable is the other half of this row's contract.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixU, s);
    auto r = exec("missings report\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/counts (vii): `missings report` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const std::string txt = renderText(r.blocks);
    check(has(txt, "2 observations with missing values"),
          "AER-UTIL/counts (vii): the observation line reads 2 -- rows 3 and 5. 3 is the sum of "
          "the per-variable counts (uv 1 + uw 2), which double-counts row 3, and it is the number "
          "a report that added its own table up prints. Got `" + txt + "`");
    check(has(txt, "uv") && has(txt, "uw"),
          "AER-UTIL/counts (vii): ...and both variables that HAVE missing appear in the table. "
          "Got `" + txt + "`");
    check(!has(txt, "ug"),
          "AER-UTIL/counts (vii): ...and ug, which has none, does NOT -- this row reports "
          "missingness, it does not enumerate the frame. Got `" + txt + "`");
  }

  // ── (viii) `missings tag` writes a rowmiss COUNT, not an indicator. ────────
  // rowmiss over (ug,uv,uw) is 0 0 2 0 1 0 0 0. sum 3 and max 2 are two
  // independent statements: an any-missing INDICATOR sums to 2 and maxes at 1,
  // so it fails both, and a rowmiss over only the first variable sums to 0.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixU, s);
    auto r = exec("missings tag, generate(mtag)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/counts (viii): `missings tag, generate(mtag)` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const double sm = runR(s, "summarize mtag", "sum");
    check(sm == 3.0,
          "AER-UTIL/counts (viii): mtag sums to 3 -- one missing on row 5 and TWO on row 3. An "
          "any-missing indicator sums to 2 over the same frame and is a column that lists "
          "correctly. Got " + fullPrec(sm));
    const double mx = runR(s, "summarize mtag", "max");
    check(mx == 2.0,
          "AER-UTIL/counts (viii): ...and its maximum is 2, which no indicator can reach. Got " +
              fullPrec(mx));
    check(displayText(exec("display \"U8A=\" mtag[3]\n", s).blocks, "U8A=") == "2",
          "AER-UTIL/counts (viii): ...mtag[3] == 2 (uv and uw both missing there). Got `" +
              displayText(exec("display \"U8A=\" mtag[3]\n", s).blocks, "U8A=") + "`");
    check(displayText(exec("display \"U8B=\" mtag[1]\n", s).blocks, "U8B=") == "0",
          "AER-UTIL/counts (viii): ...and mtag[1] == 0 -- ZERO, not missing. A tag column that "
          "left the clean rows empty has the same sum and the same max. Got `" +
              displayText(exec("display \"U8B=\" mtag[1]\n", s).blocks, "U8B=") + "`");
  }

  // FIXTURE M -- three rows, for the two DROP sub-verbs. Built so that
  // "missing in EVERY observation" and "missing in SOME observation" pick out
  // different variables, and "all variables missing" and "any variable missing"
  // pick out different rows:
  //
  //     row : 1  2  3
  //     ma  : 1  2  .
  //     mb  : .  2  .
  //     mc  : .  .  .
  static const char* kFixM =
      "clear\n"
      "set obs 3\n"
      "gen ma = _n\n"
      "replace ma = . in 3\n"
      "gen mb = ma\n"
      "replace mb = . in 1\n"
      "gen mc = ma\n"
      "replace mc = . in 1/3\n";

  // ── (ix) `missings dropvars` drops the ALL-missing variables only. ─────────
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixM, s);
    // The fixture is a changed dataset: missings wants `force` then (matches
    // missings' behaviour, identified from its ado)
    // (w2d_uncovered.log missings_dropvars_ctl r(4), _force rc 0).
    auto r = exec("missings dropvars, force\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/counts (ix): `missings dropvars` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const std::vector<std::string> vn = varNames(s);
    check(!hasName(vn, "mc"),
          "AER-UTIL/counts (ix): mc -- missing in every observation -- is GONE. A handler that "
          "dropped nothing leaves all three and says nothing about it. Got `" + joinNames(vn) +
              "`");
    check(hasName(vn, "ma") && hasName(vn, "mb"),
          "AER-UTIL/counts (ix): ...and ma and mb, which are missing in SOME observations, "
          "SURVIVE. An any-missing reading drops the whole frame here, which is silent data loss "
          "under rc 0. Got `" + joinNames(vn) + "`");
    check(nRows(s) == 3.0,
          "AER-UTIL/counts (ix): ...and no OBSERVATION was dropped -- dropvars is not dropobs. "
          "Got " + fullPrec(nRows(s)));
  }

  // ── (x) `missings dropobs` drops rows where EVERY variable is missing. ─────
  // Row 3 only. Row 1 has ma == 1 and must survive; an any-missing reading
  // keeps row 2 alone and pins _N == 1, which is a frame that still lists.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixM, s);
    auto r = exec("missings dropobs, force\n", s);  // missings requires force here, as (ix)
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/counts (x): `missings dropobs` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(nRows(s) == 2.0,
          "AER-UTIL/counts (x): TWO rows survive -- only row 3 is missing in every variable. An "
          "any-missing reading leaves 1 (row 2 alone) and an all-of-nothing reading leaves 3. Got " +
              fullPrec(nRows(s)));
    const std::vector<std::string> vn = varNames(s);
    check(hasName(vn, "mc"),
          "AER-UTIL/counts (x): ...and mc is still THERE -- dropobs drops observations, not "
          "variables. Got `" + joinNames(vn) + "`");
  }

  fs::remove_all(workdir, ec);
}



void testAerUtilFillers() {
  std::printf("[selftest] --- AER-SCOPE1 bundle I: carryforward / rangestat / tsspell "
              "(group resets, real windows, original row order) ---\n");
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_aerutilfill").u8string();
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
  auto displays = [](const auto& blocks) {
    std::vector<std::string> v;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "display")
        v.push_back(b.value("text", std::string()));
    return v;
  };
  auto displayText = [&](const auto& blocks, const std::string& prefix) -> std::string {
    for (const auto& s : displays(blocks))
      if (s.rfind(prefix, 0) == 0) return s.substr(prefix.size());
    return std::string("<no such display line>");
  };
  auto rOf = [](piet::SessionState& s, const char* key) -> double {
    const double* p = s.eval.rValues.find(key);
    return p ? *p : std::nan("");
  };
  auto runR = [&](piet::SessionState& s, const std::string& cmd, const char* key) -> double {
    (void)piet::dispatch(cmd, s);
    return rOf(s, key);
  };
  auto countIf = [&](piet::SessionState& s, const std::string& expr) {
    return runR(s, "count if " + expr, "N");
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

  // FIXTURE C -- six rows, DELIBERATELY NOT SORTED BY THE GROUP VARIABLE:
  //
  //     row : 1  2  3  4  5  6
  //     cg  : 1  2  1  2  1  2
  //     cy  : 3  .  .  7  .  .
  //     cs  : aa "" "" "" "" ""
  //
  // The interleave is the whole point. Group 1's rows are 1,3,5 and group 2's
  // are 2,4,6, so a handler that sorts internally and writes back positionally
  // lands its answers on the wrong rows while still producing a column with the
  // right SUM -- which is why the per-row reads below exist and the sum alone
  // would not have been enough.
  static const char* kFixC =
      "clear\n"
      "set obs 6\n"
      "gen cg = 1\n"
      "replace cg = 2 in 2\n"
      "replace cg = 2 in 4\n"
      "replace cg = 2 in 6\n"
      "gen cy = 3\n"
      "replace cy = . in 2\n"
      "replace cy = . in 3\n"
      "replace cy = 7 in 4\n"
      "replace cy = . in 5\n"
      "replace cy = . in 6\n"
      "gen str5 cs = \"aa\"\n"
      "replace cs = \"\" in 2/6\n";

  // ── (i) `carryforward, by()` resets at the group and keeps ROW ORDER. ──────
  // Correct answer, in original row order: 3 . 3 7 3 7.
  //   * a fill with no grouping at all gives 3 3 3 7 7 7 -- no missing, sum 30;
  //   * a fill that sorted by cg and wrote back positionally gives 3 3 3 . 7 7
  //     -- FIVE non-missing and sum 23, identical to the correct answer on both
  //     summary statistics, and wrong on rows 2 and 4;
  //   * a back-fill gives 3 7 3 7 3 7.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixC, s);
    auto r = exec("carryforward cy, gen(cf) by(cg)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/fill (i): `carryforward cy, gen(cf) by(cg)` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(displayText(exec("display \"F1A=\" cf[1]\n", s).blocks, "F1A=") == "3",
          "AER-UTIL/fill (i): cf[1] == 3 -- group 1's own first value. Got `" +
              displayText(exec("display \"F1A=\" cf[1]\n", s).blocks, "F1A=") + "`");
    check(displayText(exec("display \"F1B=\" cf[2]\n", s).blocks, "F1B=") == ".",
          "AER-UTIL/fill (i): cf[2] is MISSING. Row 2 is group 2's first observation and it is "
          "missing, so there is nothing to carry: this is the boundary a leaking fill crosses to "
          "put group 1's 3 there, and the leak reads as a perfectly ordinary value. Got `" +
              displayText(exec("display \"F1B=\" cf[2]\n", s).blocks, "F1B=") + "`");
    check(displayText(exec("display \"F1C=\" cf[3]\n", s).blocks, "F1C=") == "3",
          "AER-UTIL/fill (i): cf[3] == 3 -- carried from row 1, SKIPPING row 2, which belongs to "
          "the other group. Got `" +
              displayText(exec("display \"F1C=\" cf[3]\n", s).blocks, "F1C=") + "`");
    check(displayText(exec("display \"F1D=\" cf[4]\n", s).blocks, "F1D=") == "7",
          "AER-UTIL/fill (i): cf[4] == 7 -- row 4's own value, untouched. A sorted-and-not-restored "
          "handler puts MISSING here, which is the failure this per-row read exists for: its "
          "column has the same N and the same sum as the correct one. Got `" +
              displayText(exec("display \"F1D=\" cf[4]\n", s).blocks, "F1D=") + "`");
    check(displayText(exec("display \"F1E=\" cf[5]\n", s).blocks, "F1E=") == "3",
          "AER-UTIL/fill (i): cf[5] == 3 -- group 1 again, three rows after its source. Got `" +
              displayText(exec("display \"F1E=\" cf[5]\n", s).blocks, "F1E=") + "`");
    check(displayText(exec("display \"F1F=\" cf[6]\n", s).blocks, "F1F=") == "7",
          "AER-UTIL/fill (i): cf[6] == 7 -- group 2's carry. Got `" +
              displayText(exec("display \"F1F=\" cf[6]\n", s).blocks, "F1F=") + "`");
    const double nn = runR(s, "summarize cf", "N");
    check(nn == 5.0,
          "AER-UTIL/fill (i): ...five non-missing in all. An ungrouped fill has six. Got " +
              fullPrec(nn));
    const double sm = runR(s, "summarize cf", "sum");
    check(sm == 23.0,
          "AER-UTIL/fill (i): ...summing to 23 (3+3+7+3+7). An ungrouped fill sums to 30. Got " +
              fullPrec(sm));
    check(displayText(exec("display \"F1G=\" cy[2]\n", s).blocks, "F1G=") == ".",
          "AER-UTIL/fill (i): ...and the SOURCE column is untouched -- gen() writes a new column, "
          "it does not fill in place. Got `" +
              displayText(exec("display \"F1G=\" cy[2]\n", s).blocks, "F1G=") + "`");
  }

  // ── (ii) a string/factor column propagates too. ────────────────────────────
  // cs is "aa" on row 1 and empty everywhere else. Group 1 (rows 1,3,5) fills
  // to "aa"; group 2 (rows 2,4,6) never sees a non-empty value and stays empty.
  // A handler that only knows the f64 plane writes an all-empty column here and
  // is invisible in every numeric check above.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixC, s);
    auto r = exec("carryforward cs, gen(cfs) by(cg)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/fill (ii): `carryforward cs, gen(cfs) by(cg)` runs clean over a STRING "
          "column. Got `" + errText(firstError(r.blocks)) + "`");
    const double n3 = countIf(s, "cfs == \"aa\"");
    check(n3 == 3.0,
          "AER-UTIL/fill (ii): three rows carry \"aa\" -- group 1's rows 1, 3 and 5. 1 means the "
          "factor plane was never propagated; 6 means the group reset was skipped on this plane "
          "even though it holds on the numeric one. Got " + fullPrec(n3));
  }

  // ── (iii) `bysort cg:` and an explicit `by(cg)` are the same command. ──────
  // The prefix SORTS the frame (dispatcher.cpp, the bundle-I by-prefix branch),
  // so row order is not comparable across the two forms and the assertions here
  // are deliberately order-free: what must agree is which rows got which value.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixC, s);
    auto r = exec("bysort cg: carryforward cy, gen(cf)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/fill (iii): `bysort cg: carryforward cy, gen(cf)` runs clean -- the prefix "
          "reaches the handler with by() injected, the way egen's does. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(countIf(s, "cg == 1 & cf == 3") == 3.0,
          "AER-UTIL/fill (iii): all three of group 1's rows carry 3. Got " +
              fullPrec(countIf(s, "cg == 1 & cf == 3")));
    check(countIf(s, "cg == 2 & cf == 7") == 2.0,
          "AER-UTIL/fill (iii): ...two of group 2's carry 7. Got " +
              fullPrec(countIf(s, "cg == 2 & cf == 7")));
    check(countIf(s, "cg == 2 & missing(cf)") == 1.0,
          "AER-UTIL/fill (iii): ...and exactly one of group 2's is still missing -- its earliest "
          "row, which had nothing before it. If the prefix had NOT reset at the boundary this is "
          "0 and every other count above still agrees. Got " +
              fullPrec(countIf(s, "cg == 2 & missing(cf)")));
    check(runR(s, "summarize cf", "sum") == 23.0,
          "AER-UTIL/fill (iii): ...and the total matches the explicit-by() form's 23. Got " +
              fullPrec(runR(s, "summarize cf", "sum")));
  }

  // ── (iv) plain `by cg:` SORTS as well, and answers. ───────────────────────
  // Real Stata refuses a plain `by:` over unsorted data at r(5). Replicating
  // that needs a sortedness oracle PIET does not expose to the dispatcher (the
  // `sorted_by` claim lives in the .hbin header), so the landed branch sorts for
  // both spellings -- and for these two order-dependent verbs that is the safe
  // direction, because running them unsorted produces a different, entirely
  // plausible answer. So the assertion here is the ANSWER, not a refusal: a
  // test demanding r(5) would be asserting behaviour the tree deliberately does
  // not have.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixC, s);
    auto r = exec("by cg: carryforward cy, gen(cf)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/fill (iv): plain `by cg: carryforward cy, gen(cf)` over UNSORTED data runs "
          "clean rather than refusing. Got `" + errText(firstError(r.blocks)) + "`");
    check(countIf(s, "cg == 1 & cf == 3") == 3.0 && countIf(s, "cg == 2 & cf == 7") == 2.0 &&
              countIf(s, "cg == 2 & missing(cf)") == 1.0,
          "AER-UTIL/fill (iv): ...and it produces the SORTED-AND-CORRECT answer, identical to the "
          "bysort form's. Reaching the handler without sorting would carry values across group "
          "boundaries in row order and answer rc 0. Got g1==3 " +
              fullPrec(countIf(s, "cg == 1 & cf == 3")) + " g2==7 " +
              fullPrec(countIf(s, "cg == 2 & cf == 7")) + " g2 missing " +
              fullPrec(countIf(s, "cg == 2 & missing(cf)")));
  }

  // FIXTURE W -- six rows, two panels, years INTERLEAVED so the answer column
  // has to be put back in the frame's own order:
  //
  //     row   : 1     2     3     4     5     6
  //     rid   : 1     2     1     1     2     1
  //     ryear : 2002  2001  2000  2003  2000  2001
  //     rv    : 3     6     1     4     5     2
  static const char* kFixW =
      "clear\n"
      "set obs 6\n"
      "gen rid = 1\n"
      "replace rid = 2 in 2\n"
      "replace rid = 2 in 5\n"
      "gen ryear = 2002\n"
      "replace ryear = 2001 in 2\n"
      "replace ryear = 2000 in 3\n"
      "replace ryear = 2003 in 4\n"
      "replace ryear = 2000 in 5\n"
      "replace ryear = 2001 in 6\n"
      "gen rv = 3\n"
      "replace rv = 6 in 2\n"
      "replace rv = 1 in 3\n"
      "replace rv = 4 in 4\n"
      "replace rv = 5 in 5\n"
      "replace rv = 2 in 6\n";

  // ── (v) `rangestat` computes over a WINDOW, in original row order. ────────
  // interval(ryear -1 1) is the closed window [year-1, year+1] within rid, the
  // row itself included. Hand-computed, per original row:
  //     row 1 (1,2002): 2001+2002+2003 = 2+3+4 = 9
  //     row 2 (2,2001): 2000+2001      = 5+6   = 11
  //     row 3 (1,2000): 2000+2001      = 1+2   = 3
  //     row 4 (1,2003): 2002+2003      = 3+4   = 7
  //     row 5 (2,2000): 2000+2001      = 5+6   = 11
  //     row 6 (1,2001): 2000+2001+2002 = 1+2+3 = 6
  // The by-GROUP sum -- the aggregate a window-blind handler produces -- is 10
  // for every rid-1 row and 11 for every rid-2 row, so it differs on four of the
  // six rows and totals 62 against 47. And a handler that computed the right
  // window in sorted order and wrote back positionally puts 3 on row 1.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixW, s);
    auto r = exec("rangestat (sum) rv, interval(ryear -1 1) by(rid)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/fill (v): `rangestat (sum) rv, interval(ryear -1 1) by(rid)` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(hasName(varNames(s), "rv_sum"),
          "AER-UTIL/fill (v): ...creating `rv_sum` off the default <var>_<stat> naming. Got `" +
              joinNames(varNames(s)) + "`");
    check(displayText(exec("display \"F5A=\" rv_sum[1]\n", s).blocks, "F5A=") == "9",
          "AER-UTIL/fill (v): rv_sum[1] == 9 (2+3+4). The by-group aggregate is 10 here and the "
          "sorted-order-write-back is 3 -- three different plausible columns, and this is the "
          "row that separates all three. Got `" +
              displayText(exec("display \"F5A=\" rv_sum[1]\n", s).blocks, "F5A=") + "`");
    check(displayText(exec("display \"F5B=\" rv_sum[3]\n", s).blocks, "F5B=") == "3",
          "AER-UTIL/fill (v): rv_sum[3] == 3 (1+2) -- an END row of its panel's year range, where "
          "the window is truncated and the group aggregate (10) is not. Got `" +
              displayText(exec("display \"F5B=\" rv_sum[3]\n", s).blocks, "F5B=") + "`");
    check(displayText(exec("display \"F5C=\" rv_sum[4]\n", s).blocks, "F5C=") == "7",
          "AER-UTIL/fill (v): rv_sum[4] == 7 (3+4) -- the other end. Got `" +
              displayText(exec("display \"F5C=\" rv_sum[4]\n", s).blocks, "F5C=") + "`");
    check(displayText(exec("display \"F5D=\" rv_sum[6]\n", s).blocks, "F5D=") == "6",
          "AER-UTIL/fill (v): rv_sum[6] == 6 (1+2+3) -- a full three-year window that is still "
          "not the panel total. Got `" +
              displayText(exec("display \"F5D=\" rv_sum[6]\n", s).blocks, "F5D=") + "`");
    check(displayText(exec("display \"F5E=\" rv_sum[2]\n", s).blocks, "F5E=") == "11",
          "AER-UTIL/fill (v): rv_sum[2] == 11 -- panel 2, whose two rows are inside one window, so "
          "here the window and the group DO agree. Got `" +
              displayText(exec("display \"F5E=\" rv_sum[2]\n", s).blocks, "F5E=") + "`");
    const double sm = runR(s, "summarize rv_sum", "sum");
    check(sm == 47.0,
          "AER-UTIL/fill (v): ...and the column totals 47. A by-group aggregate totals 62; a "
          "window that forgot to include the row itself totals 23. Got " + fullPrec(sm));
    check(runR(s, "summarize rv_sum", "N") == 6.0,
          "AER-UTIL/fill (v): ...over six non-missing rows -- every row gets a value, because the "
          "window always contains at least the row itself. Got N " +
              fullPrec(runR(s, "summarize rv_sum", "N")));
  }

  // ── (vi) `(mean)` over the same windows, and the answer is not an integer. ─
  // row 3 (1,2000): mean of {1,2} = 1.5; row 4 (1,2003): mean of {3,4} = 3.5.
  // Both are exactly representable, and both differ from panel 1's group mean
  // (2.5) -- so this leg fails a group aggregate on the same rows the sum leg
  // does, through a completely different arithmetic path.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixW, s);
    auto r = exec("rangestat (mean) rv, interval(ryear -1 1) by(rid)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/fill (vi): `rangestat (mean) rv, interval(ryear -1 1) by(rid)` runs clean. "
          "Got `" + errText(firstError(r.blocks)) + "`");
    const std::string m3 = displayText(exec("display \"F6A=\" rv_mean[3]\n", s).blocks, "F6A=");
    check(m3 == "1.5",
          "AER-UTIL/fill (vi): rv_mean[3] == 1.5 -- the mean of {1,2}, not of panel 1 (2.5) and "
          "not the sum. Got `" + m3 + "`");
    const std::string m4 = displayText(exec("display \"F6B=\" rv_mean[4]\n", s).blocks, "F6B=");
    check(m4 == "3.5",
          "AER-UTIL/fill (vi): rv_mean[4] == 3.5 -- the mean of {3,4}. Got `" + m4 + "`");
  }

  // ── (vii) `tsspell` walks runs of EQUAL VALUE and resets the sequence. ────
  // tt 1..6, tx = 1 1 0 0 1 2. Four spells: [1,2] [3,4] [5] [6].
  //     spell : 1 1 2 2 3 4   (sum 13, max 4)
  //     seq   : 1 2 1 2 1 1   (sum 8)
  //     end   : 0 1 0 1 1 1   (sum 4)
  // A sequence that never resets runs 1..6 and sums to 21; a spell counter that
  // never increments sums to 6; an end flag set only on the last row sums to 1.
  static const char* kFixT =
      "clear\n"
      "set obs 6\n"
      "gen tt = _n\n"
      "gen tx = 1\n"
      "replace tx = 0 in 3/4\n"
      "replace tx = 2 in 6\n"
      "tsset tt\n";
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixT, s);
    auto r = exec("tsspell tx, spell(tsp) seq(tsq) end(tnd)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/fill (vii): `tsspell tx, spell(tsp) seq(tsq) end(tnd)` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(runR(s, "summarize tsp", "max") == 4.0,
          "AER-UTIL/fill (vii): FOUR spells -- [1,1] [0,0] [1] [2]. A spell counter that never "
          "increments maxes at 1 and one that started a spell per ROW maxes at 6. Got " +
              fullPrec(runR(s, "summarize tsp", "max")));
    check(runR(s, "summarize tsp", "sum") == 13.0,
          "AER-UTIL/fill (vii): ...and the spell ids sum to 13 (1+1+2+2+3+4), which is the half "
          "that says WHERE the boundaries fell and not just how many there were. Got " +
              fullPrec(runR(s, "summarize tsp", "sum")));
    check(runR(s, "summarize tsq", "sum") == 8.0,
          "AER-UTIL/fill (vii): the within-spell sequence sums to 8 (1+2+1+2+1+1). A sequence "
          "that never reset runs 1..6 and sums to 21 -- a strictly increasing column that lists "
          "perfectly. Got " + fullPrec(runR(s, "summarize tsq", "sum")));
    check(runR(s, "summarize tnd", "sum") == 4.0,
          "AER-UTIL/fill (vii): the end flag fires once per spell -- four. An end flag set only "
          "on the frame's last row sums to 1. Got " +
              fullPrec(runR(s, "summarize tnd", "sum")));
    check(displayText(exec("display \"F7A=\" tsp[5]\n", s).blocks, "F7A=") == "3",
          "AER-UTIL/fill (vii): ...and row 5 is in spell 3 -- the run of 1s that comes AFTER the "
          "run of 0s is a new spell, not a resumption of spell 1. Got `" +
              displayText(exec("display \"F7A=\" tsp[5]\n", s).blocks, "F7A=") + "`");
    check(displayText(exec("display \"F7B=\" tnd[2]\n", s).blocks, "F7B=") == "1",
          "AER-UTIL/fill (vii): ...and row 2 is an END, because row 3 leaves the spell. Got `" +
              displayText(exec("display \"F7B=\" tnd[2]\n", s).blocks, "F7B=") + "`");
  }

  // ── (viii) the bare form writes _spell / _seq / _end. ─────────────────────
  // Read off `describe` rather than through an expression, because the default
  // names begin with an underscore. What this catches is a handler that only
  // ever honours the three renaming options and writes nothing without them.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixT, s);
    auto r = exec("tsspell tx\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/fill (viii): the bare `tsspell tx` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const std::vector<std::string> vn = varNames(s);
    check(hasName(vn, "_spell") && hasName(vn, "_seq") && hasName(vn, "_end"),
          "AER-UTIL/fill (viii): ...and writes all THREE default columns. One of three is the "
          "shape a half-landed handler ships, and the two missing ones are only noticed by the "
          "next command that reads them. Got `" + joinNames(vn) + "`");
  }

  // ── (ix) a TIME GAP breaks a run of equal values. ─────────────────────────
  // tt = 1, 2, 4 with tx constant at 1. Two spells, because 3 is absent:
  //     spell 1 1 2 / seq 1 2 1 / end 0 1 1.
  // A run-length walk over ROW order sees one spell of three (seq 1 2 3, sum 6)
  // and is indistinguishable from the correct answer on any check that does not
  // put a hole in the time index.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec("clear\n"
               "set obs 3\n"
               "gen tt = _n\n"
               "replace tt = 4 in 3\n"
               "gen tx = 1\n"
               "tsset tt\n",
               s);
    auto r = exec("tsspell tx, spell(gsp) seq(gsq) end(gnd)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/fill (ix): tsspell over a gapped time index runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(displayText(exec("display \"F9A=\" gsp[3]\n", s).blocks, "F9A=") == "2",
          "AER-UTIL/fill (ix): the observation at t=4 opens a NEW spell even though tx never "
          "changed -- the missing t=3 breaks the run. A row-order walk puts 1 here. Got `" +
              displayText(exec("display \"F9A=\" gsp[3]\n", s).blocks, "F9A=") + "`");
    check(runR(s, "summarize gsq", "sum") == 4.0,
          "AER-UTIL/fill (ix): ...so the sequence is 1 2 1 and sums to 4. A gap-blind walk runs "
          "1 2 3 and sums to 6. Got " + fullPrec(runR(s, "summarize gsq", "sum")));
    check(runR(s, "summarize gnd", "sum") == 2.0,
          "AER-UTIL/fill (ix): ...and two spells means two end flags. Got " +
              fullPrec(runR(s, "summarize gnd", "sum")));
  }

  // ── (x) `cond()` spells a CONDITION, not a run of equal values. ───────────
  // Over the same tx = 1 1 0 0 1 2, `cond(tx == 1)` gives spells [1,2] and [5]:
  // within-spell sequence 1 on two rows and 2 on ONE. The varname form over the
  // same column gives 1 on four rows and 2 on two, so a cond() that was quietly
  // read as a varname separates here. The rows outside every spell are counted
  // by neither check, so this holds whether they are set to 0 or left missing.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kFixT, s);
    auto r = exec("tsspell, cond(tx == 1) spell(csp) seq(csq) end(cnd)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/fill (x): `tsspell, cond(tx == 1) ...` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(countIf(s, "csq == 1") == 2.0,
          "AER-UTIL/fill (x): TWO spells begin (rows 1 and 5). The varname form over the same "
          "column begins four. Got " + fullPrec(countIf(s, "csq == 1")));
    check(countIf(s, "csq == 2") == 1.0,
          "AER-UTIL/fill (x): ...and only ONE row is second in its spell (row 2). The varname "
          "form has two. Got " + fullPrec(countIf(s, "csq == 2")));
  }

  fs::remove_all(workdir, ec);
}



void testAerUtilRefusals() {
  std::printf("[selftest] --- AER-SCOPE1 bundle I: marksample / filelist / the option and "
              "by()-without-gen() refusals ---\n");
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_aerutilref").u8string();
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
  auto errField = [](const nlohmann::ordered_json& e, const std::string& f) {
    return e.is_object() ? e.value(f, std::string()) : std::string("<none>");
  };
  auto fullPrec = [](double v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return std::string(buf);
  };
  auto has = [](const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
  };
  auto spit = [](const std::string& p, const std::string& bytes) {
    std::ofstream f(cecon::to_native_path(p), std::ios::binary | std::ios::trunc);
    f << bytes;
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
  auto rOf = [](piet::SessionState& s, const char* key) -> double {
    const double* p = s.eval.rValues.find(key);
    return p ? *p : std::nan("");
  };
  auto runR = [&](piet::SessionState& s, const std::string& cmd, const char* key) -> double {
    (void)piet::dispatch(cmd, s);
    return rOf(s, key);
  };
  auto nRows = [&](piet::SessionState& s) { return runR(s, "count", "N"); };
  auto countIf = [&](piet::SessionState& s, const std::string& expr) {
    return runR(s, "count if " + expr, "N");
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

  // NOTE ON WHAT IS *NOT* IN HERE. The plan's scope table carries three
  // record-linkage names (strdist / matchit / reclink) and two more whose
  // authorship could not be confirmed (dropmiss / renvars). None of the five is
  // in the registry, so each answers a GENERIC r(199) that names no cause. A leg
  // asserting a specific refusal reason for them would be asserting a promise
  // the tree does not make, and it would keep passing after somebody registered
  // them with a different cause. They are absent on purpose.

  // ── (i) `marksample` at the top level refuses. ────────────────────────────
  // The local it reads (`varlist') is set by `syntax', and `syntax' only runs
  // inside a program. Marking every observation instead hands the caller a
  // touse that silently includes the rows they filtered out -- the whole point
  // of the marker being wrong in the direction nobody checks.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec("clear\nset obs 3\ngen q = _n\n", s);
    auto r = exec("marksample touse\n", s);
    const nlohmann::ordered_json e = firstError(r.blocks);
    check(!e.is_null(),
          "AER-UTIL/refuse (i): `marksample touse` at the TOP LEVEL is refused, not honoured");
    check(has(errText(e), "not allowed outside a program"),
          "AER-UTIL/refuse (i): ...and the sentence says why -- `not allowed outside a program`. "
          "A bare `invalid syntax` here reads as a spelling complaint about a command the user "
          "spelled correctly. Got `" + errText(e) + "`");
  }

  // ── (ii) inside a program it marks the NON-MISSING rows of `varlist'. ─────
  // Five rows; mv is missing on row 2 and mz on row 4, disjoint, so the marker
  // selects three. The two wrong answers are 5 (a marker that ignored the
  // varlist and marked everything) and 4 (one that read only the first
  // variable) -- both of which produce a byte column of 0s and 1s that lists
  // perfectly and quietly changes every estimate downstream of it.
  {
    piet::SessionState s(workdir);
    seed(s);
    auto r = exec("clear\n"
                  "set obs 5\n"
                  "gen mv = _n\n"
                  "replace mv = . in 2\n"
                  "gen mz = _n\n"
                  "replace mz = . in 4\n"
                  "program define aer_i_ms\n"
                  "    syntax varlist(min=1)\n"
                  "    marksample touse\n"
                  "    quietly count if `touse'\n"
                  "    display \"AER_I_TOUSE=\" r(N)\n"
                  "end\n"
                  "aer_i_ms mv mz\n",
                  s);
    check(!r.stoppedAtReason.has_value(),
          "AER-UTIL/refuse (ii): the program defining and calling marksample runs to its end. "
          "stop=" + r.stoppedAtReason.value_or("<none>"));
    const double n = displayValue(r.blocks, "AER_I_TOUSE=");
    check(n == 3.0,
          "AER-UTIL/refuse (ii): the touse marker selects THREE rows -- those non-missing in BOTH "
          "of `varlist'. 5 means the varlist was ignored and everything marked; 4 means only the "
          "first variable was read. Got " + fullPrec(n));
  }

  // ── (iii) `novarlist` marks everything, and that is the point of it. ──────
  {
    piet::SessionState s(workdir);
    seed(s);
    auto r = exec("clear\n"
                  "set obs 5\n"
                  "gen mv = _n\n"
                  "replace mv = . in 2\n"
                  "gen mz = _n\n"
                  "replace mz = . in 4\n"
                  "program define aer_i_msn\n"
                  "    syntax varlist(min=1)\n"
                  "    marksample touse, novarlist\n"
                  "    quietly count if `touse'\n"
                  "    display \"AER_I_NOVAR=\" r(N)\n"
                  "end\n"
                  "aer_i_msn mv mz\n",
                  s);
    check(!r.stoppedAtReason.has_value(),
          "AER-UTIL/refuse (iii): the novarlist program runs to its end. stop=" +
              r.stoppedAtReason.value_or("<none>"));
    const double n = displayValue(r.blocks, "AER_I_NOVAR=");
    check(n == 5.0,
          "AER-UTIL/refuse (iii): `novarlist` marks all FIVE rows -- it suppresses the "
          "missing-value screen entirely. 3 here is leg (ii)'s answer, which means the option was "
          "consumed and ignored, and an ignored `novarlist` drops exactly the observations the "
          "caller asked to keep. Got " + fullPrec(n));
  }

  // ── (iv) `filelist` REPLACES the frame with one row per file. ─────────────
  // A directory this leg stages itself, three files of three DIFFERENT sizes so
  // the byte total is a real discriminator: a handler that reported 0, or the
  // same size on every row, agrees with the row count and not with the sum.
  {
    const fs::path fdir = fs::path(workdir) / "fl";
    fs::create_directories(fdir, ec);
    spit((fdir / "a.txt").u8string(), "abc");        // 3 bytes
    spit((fdir / "bb.txt").u8string(), "de");        // 2 bytes
    spit((fdir / "ccc.dat").u8string(), "fghij");    // 5 bytes

    piet::SessionState s(workdir);
    seed(s);
    (void)exec("clear\nset obs 7\ngen zz = _n\n", s);
    auto r = exec("filelist, directory(\"" + fdir.u8string() + "\") norecursive\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/refuse (iv): `filelist, directory(...) norecursive` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(nRows(s) == 3.0,
          "AER-UTIL/refuse (iv): THREE rows, one per file. Got " + fullPrec(nRows(s)));
    check(!hasName(varNames(s), "zz"),
          "AER-UTIL/refuse (iv): ...and the seven-row frame that was in memory is GONE: this verb "
          "replaces the dataset, it does not append to it. A handler that appended would leave zz "
          "here and pin 10 rows. Got `" + joinNames(varNames(s)) + "`");
    const std::vector<std::string> vn = varNames(s);
    check(hasName(vn, "dirname") && hasName(vn, "filename") && hasName(vn, "fsize"),
          "AER-UTIL/refuse (iv): ...with the three contracted columns. Got `" + joinNames(vn) +
              "`");
    check(countIf(s, "filename == \"a.txt\"") == 1.0 &&
              countIf(s, "filename == \"bb.txt\"") == 1.0 &&
              countIf(s, "filename == \"ccc.dat\"") == 1.0,
          "AER-UTIL/refuse (iv): ...and filename holds the LEAF, once each. A filename column "
          "carrying the full path satisfies the row count and fails every join a caller would "
          "make on it. Got a.txt " + fullPrec(countIf(s, "filename == \"a.txt\"")) + " bb.txt " +
              fullPrec(countIf(s, "filename == \"bb.txt\"")) + " ccc.dat " +
              fullPrec(countIf(s, "filename == \"ccc.dat\"")));
    const double bytes = runR(s, "summarize fsize", "sum");
    check(bytes == 10.0,
          "AER-UTIL/refuse (iv): ...and fsize totals 10 (3+2+5). 0 is a column that was created "
          "and never filled and 9 is one that repeated a single stat() three times; both list as "
          "an ordinary numeric column. Got " + fullPrec(bytes));
  }

  // ── (v) `pattern()` is a glob, and it actually filters. ───────────────────
  {
    const fs::path fdir = fs::path(workdir) / "fl";
    piet::SessionState s(workdir);
    seed(s);
    auto r = exec("filelist, directory(\"" + fdir.u8string() +
                      "\") pattern(\"*.txt\") norecursive\n",
                  s);
    check(firstError(r.blocks).is_null(),
          "AER-UTIL/refuse (v): `filelist, ... pattern(\"*.txt\")` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(nRows(s) == 2.0,
          "AER-UTIL/refuse (v): TWO rows -- ccc.dat is excluded. 3 means the option was consumed "
          "and ignored, which is a filter that silently does not filter. Got " +
              fullPrec(nRows(s)));
    check(runR(s, "summarize fsize", "sum") == 5.0,
          "AER-UTIL/refuse (v): ...and the surviving bytes are 3+2, not 3+2+5. Got " +
              fullPrec(runR(s, "summarize fsize", "sum")));
  }

  // ── (vi) `unique ..., by()` with no `gen()` refuses BY NAME. ──────────────
  // by() without gen() has nowhere to put its per-group answer, and Stata's
  // `unique` prints nothing in that form. Printing the ungrouped count instead
  // would answer a question the user did not ask, under rc 0.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec("clear\nset obs 4\ngen ug = 1\nreplace ug = 2 in 3/4\ngen uv = _n\n", s);
    auto r = exec("unique uv, by(ug)\n", s);
    const nlohmann::ordered_json e = firstError(r.blocks);
    check(!e.is_null(),
          "AER-UTIL/refuse (vi): `unique uv, by(ug)` without gen() is refused rather than "
          "silently answered as the ungrouped form");
    check(errField(e, "reason") == "refused:unique_by_without_gen",
          "AER-UTIL/refuse (vi): ...naming its OWN cause, so a caller can tell this apart from a "
          "misspelled option. Got `" + errField(e, "reason") + "`");
  }

  // ── (vii) the two options this arm does not implement are refused BY NAME. ─
  // `unique, detail` and `gunique, replace` change the ANSWER, not its speed --
  // detail adds a per-group listing and replace overwrites the source column.
  // Consuming either and carrying on is the accept-note trap this campaign
  // banned outside the ENV bundle.
  {
    struct Case { const char* cmd; const char* opt; };
    const std::vector<Case> kCases = {
        {"unique uv, detail", "detail"},
        {"gunique uv, replace", "replace"},
    };
    for (const auto& c : kCases) {
      piet::SessionState s(workdir);
      seed(s);
      (void)exec("clear\nset obs 4\ngen uv = _n\n", s);
      auto r = exec(std::string(c.cmd) + "\n", s);
      const nlohmann::ordered_json e = firstError(r.blocks);
      check(!e.is_null(),
            std::string("AER-UTIL/refuse (vii): `") + c.cmd +
                "` is refused rather than quietly accepted");
      check(has(errText(e), c.opt),
            std::string("AER-UTIL/refuse (vii): ...and the sentence NAMES the option (`") + c.opt +
                "`), which is what tells the user which half of their line to drop. Got `" +
                errText(e) + "`");
    }
  }

  fs::remove_all(workdir, ec);
}




// ── AER scope 2, bundle K1: tolower / move / renvars, plus the varlist glob ──
//
// Every expected value below is a MEASUREMENT against StataNow 19.5 BE with
// tolower.ado (Nick Cox) and renvars.ado (dm88_1, Cox/Weesie) installed, taken
// on 2026-09-07 BEFORE any of this was written. That order is the point: the
// plan for this bundle said `move` was `order v1, before(v2)` and that `renvars`
// could be an alias for `renvarlab`, and the captures falsified both.
//
// THREE LEGS ARE DISCRIMINATORS -- they are the checks a plausible wrong
// implementation fails, and they are why this function is not just a smoke test:
//
//   (a) `move a c` on `a b c` is `b c a`, not `b a c`. A before()-always
//       implementation gets the first direction right and this one wrong, and
//       nothing about the wrong answer looks wrong.
//   (b) bare `tolower` on `AA bb CC` answers rc 110 AND LEAVES `aa bb CC`.
//       tolower is a while-loop around `rename`, so it renames until one fails.
//       An implementation that validated the list first (which is what the
//       sibling `renvars` correctly does, and what reads as more careful) leaves
//       `AA bb CC` untouched -- a different answer at the same rc.
//   (c) `renvars share_sigvar*, postsub("_" "")` is a NO-OP at rc 0, because
//       postsub substitutes at the END only. That exact line is in the corpus.
//       An implementation that read postsub as "substitute everywhere" renames
//       the columns to `sharesigvara` and answers rc 0 -- a silent wrong answer
//       with no diagnostic on any surface.
void testAerK1() {
  std::printf("[selftest] --- AER scope-2 bundle K1: tolower / move / renvars + varlist glob ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_aerk1").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // Runs a do-source in a FRESH session and reports the trailing `ds` line plus
  // the rc of the last `capture`d statement. Every leg is independent: these
  // commands mutate the variable plane, and a shared session would make leg N+1
  // depend on leg N's failure mode.
  auto run = [&](const std::string& src) {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;
    return piet::doexec::executeDoSource(src, s, piet::dispatch);
  };
  // The LAST display block, which every fixture below makes a `ds`.
  auto lastDisplay = [](const nlohmann::ordered_json& blocks) {
    std::string out;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display")
        out = b.value("text", std::string());
    }
    return out;
  };
  auto namesAfter = [&](const std::string& body) {
    return lastDisplay(run(body + "\nds\n").blocks);
  };
  // rc via a local, because `_rc` is not resolvable inside a gen expression.
  auto rcOfSrc = [&](const std::string& body) {
    return lastDisplay(run(body + "\nlocal rc = _rc\ndisplay \"`rc'\"\n").blocks);
  };

  const std::string kThree = "clear\nset obs 2\ngen a = 1\ngen b = 2\ngen c = 3\n";

  // ---- tolower -----------------------------------------------------------
  check(namesAfter("clear\nset obs 2\ngen AAA = 1\ngen BbB = 2\ngen ccc = 3\ntolower AAA BbB") ==
            "aaa bbb ccc",
        "K1 tolower: named variables are lowercased and the one NOT named is left alone "
        "(Stata: aaa bbb ccc)");
  {
    auto r = run("clear\nset obs 2\ngen AAA = 1\nlabel variable AAA \"Label for AAA\"\n"
                 "label define lb2 1 \"one\"\nlabel values AAA lb2\ntolower AAA\n"
                 "local L : variable label aaa\ndisplay \"`L'\"\n");
    check(lastDisplay(r.blocks) == "Label for AAA",
          "K1 tolower: the VARIABLE LABEL survives, because this routes through runRename and "
          "rename does not touch the label plane (measured)");
  }
  {
    auto r = run("clear\nset obs 2\ngen AAA = 1\nlabel define lb2 1 \"one\"\n"
                 "label values AAA lb2\ntolower AAA\n"
                 "local V : value label aaa\ndisplay \"`V'\"\n");
    check(lastDisplay(r.blocks) == "lb2",
          "K1 tolower: the VALUE LABEL survives too (measured `lb2`)");
  }
  check(namesAfter("clear\nset obs 2\ngen QQ = 1\ngen Rr = 2\ntolower") == "qq rr",
        "K1 tolower: bare `tolower` is _all");
  // (b) the non-atomic discriminator.
  check(rcOfSrc("clear\nset obs 2\ngen AA = 1\ngen bb = 2\ngen CC = 3\ncapture tolower") == "110",
        "K1 tolower: bare `tolower` over `AA bb CC` STOPS at r(110) -- `rename bb bb` is a "
        "collision, not a skip");
  check(namesAfter("clear\nset obs 2\ngen AA = 1\ngen bb = 2\ngen CC = 3\ncapture tolower") ==
            "aa bb CC",
        "K1 tolower: ...and it leaves `aa bb CC` behind -- the first rename LANDED and the third "
        "was never reached. A validate-first implementation leaves `AA bb CC`, which is a "
        "different answer at the same rc");
  check(rcOfSrc("clear\nset obs 2\ngen abc = 1\ncapture tolower abc") == "110",
        "K1 tolower: a name that is ALREADY lowercase is r(110), not a no-op (Stata issues the "
        "self-rename and gets the collision)");
  check(namesAfter("clear\nset obs 2\ngen AL1 = 1\ngen MID = 2\ngen KONEV_L = 3\ngen ZZ = 4\n"
                   "tolower AL1- KONEV_L") == "al1 mid konev_l ZZ",
        "K1 tolower: the corpus's range form, hyphen followed by a SPACE (`tolower AL1- KONEV_L`)");

  // ---- move --------------------------------------------------------------
  check(namesAfter(kThree + "move c a") == "c a b",
        "K1 move: `move c a` puts c in a's position -> c a b");
  // (a) the direction discriminator.
  check(namesAfter(kThree + "move a c") == "b c a",
        "K1 move: `move a c` is `b c a`, NOT `b a c`. This is the leg that fails if move is "
        "implemented as `order v1, before(v2)` in both directions");
  check(namesAfter(kThree + "move a a") == "a b c",
        "K1 move: moving a variable onto itself is rc 0 and changes nothing");
  check(rcOfSrc(kThree + "capture move zz a") == "111",
        "K1 move: an absent source is r(111) `variable zz not found`");
  check(rcOfSrc(kThree + "capture move a b c") == "103",
        "K1 move: three names is r(103) `too many variables specified`");
  check(rcOfSrc(kThree + "capture move a") == "102",
        "K1 move: one name is r(102) `too few variables specified`");

  // ---- renvars -----------------------------------------------------------
  check(namesAfter("clear\nset obs 2\ngen date = 1\ngen mschool = 2\ngen zz = 3\n"
                   "renvars date-mschool, prefix(bl_)") == "bl_date bl_mschool zz",
        "K1 renvars: prefix() over a hyphen range (a real corpus line)");
  check(namesAfter("clear\nset obs 2\ngen state = 1\ngen county = 2\n"
                   "renvars state county, postfix(\"10\")") == "state10 county10",
        "K1 renvars: postfix() with a QUOTED argument -- the quotes come off, or the target name "
        "is `state\"10\"` and illegal");
  check(namesAfter("clear\nset obs 2\ngen s_kfr = 1\ngen s_kir = 2\n"
                   "renvars s_kfr s_kir, suff(_)") == "s_kfr_ s_kir_",
        "K1 renvars: `suff()` is renvars' own synonym for postfix(), and the corpus spells it "
        "that way");
  check(namesAfter("clear\nset obs 2\ngen state10 = 1\ngen county10 = 2\n"
                   "renvars state10 county10, postsub(10)") == "state county",
        "K1 renvars: one-argument postsub() DROPS the trailing text (replacement is empty)");
  // (c) the silent-wrong-answer discriminator.
  check(namesAfter("clear\nset obs 2\ngen share_sigvar_a = 1\ngen share_sigvar_b = 2\n"
                   "renvars share_sigvar*, postsub(\"_\" \"\")") ==
            "share_sigvar_a share_sigvar_b",
        "K1 renvars: postsub() matches at the END ONLY, so this real corpus line is a NO-OP at "
        "rc 0. Reading postsub as substitute-everywhere renames them to `sharesigvara` and says "
        "nothing");
  check(namesAfter("clear\nset obs 2\ngen n_state = 1\ngen n_county = 2\ngen keepme = 3\n"
                   "renvars n_state n_county, presub(n_)") == "state county keepme",
        "K1 renvars: presub() matches at the START only");
  check(namesAfter("clear\nset obs 2\ngen a_cen_2010 = 1\ngen b_cen_2010 = 2\n"
                   "renvars *2010, subst(\"_cen_\" \"\")") == "a2010 b2010",
        "K1 renvars: subst() replaces ANYWHERE -- and the varlist is a LEADING wildcard, which "
        "the expander could not do before this bundle");
  check(namesAfter("clear\nset obs 2\ngen j6qe_a = 1\ngen j6qe_b = 2\nrenvars j6qe*, upper") ==
            "J6QE_A J6QE_B",
        "K1 renvars: upper");
  check(namesAfter("clear\nset obs 2\ngen v2 = 1\ngen v3 = 2\ngen v4 = 3\n"
                   "renvars v2-v4 \\ id age income") == "id age income",
        "K1 renvars: the backslash one-to-one MAP form, which renvarlab has no syntax for at all "
        "-- this is why the alias the plan proposed could not have worked");
  check(rcOfSrc("clear\nset obs 2\ngen v2 = 1\ngen v3 = 2\ncapture renvars v2-v3 \\ onlyone") ==
            "198",
        "K1 renvars: a map form whose two lists differ in length is r(198)");
  check(rcOfSrc("clear\nset obs 2\ngen aa = 1\ncapture renvars aa, prefix(x_) upper") == "198",
        "K1 renvars: TWO transformation options is r(198) -- the ado counts them and demands "
        "exactly one");
  check(rcOfSrc("clear\nset obs 2\ngen a = 1\ngen bl_a = 2\ncapture renvars a, prefix(bl_)") ==
            "110",
        "K1 renvars: a collision is r(110)");
  check(namesAfter("clear\nset obs 2\ngen a = 1\ngen bl_a = 2\ncapture renvars a, prefix(bl_)") ==
            "a bl_a",
        "K1 renvars: ...and NOTHING was renamed -- renvars runs `confirm new var` over the whole "
        "plan first, so it is atomic where tolower above is not");
  check(namesAfter("clear\nset obs 2\ngen AA = 1\nrenvars AA, lower test") == "AA",
        "K1 renvars: `test` prints the plan and renames nothing");
  check(namesAfter("clear\nset obs 2\ngen AA = 1\ngen BB = 2\nrenvars, lower") == "aa bb",
        "K1 renvars: no varlist at all is _all");
  {
    auto r = run("clear\nset obs 2\ngen AAA = 1\nlabel variable AAA \"Label for AAA\"\n"
                 "renvars AAA, lower\nlocal L : variable label aaa\ndisplay \"`L'\"\n");
    check(lastDisplay(r.blocks) == "Label for AAA",
          "K1 renvars: the variable label survives here too");
  }

  // ---- the varlist glob, which K1b widened from prefix-only --------------
  // Measured with `unab` on auto.dta: t* -> trunk turn, *e -> make price,
  // tr*k -> trunk, ?rice -> price, zz* -> r(111).
  {
    const std::string fixture =
        "clear\nset obs 2\ngen price = 1\ngen mpg = 2\ngen trunk = 3\ngen turn = 4\n";
    auto seen = [&](const std::string& pat) {
      auto r = run(fixture + "local s = \"\"\nforeach v of varlist " + pat +
                   " {\nlocal s = \"`s' `v'\"\n}\ndisplay \"`s'\"\n");
      std::string t = lastDisplay(r.blocks);
      while (!t.empty() && t.front() == ' ') t.erase(t.begin());
      return t;
    };
    check(seen("t*") == "trunk turn", "K1 glob: a TRAILING wildcard (the only shape that worked "
                                      "before this bundle)");
    check(seen("*e") == "price", "K1 glob: a LEADING wildcard (only price ends in e in this fixture)");
    check(seen("tr*k") == "trunk", "K1 glob: a wildcard in the MIDDLE");
    check(seen("?rice") == "price", "K1 glob: `?` matches exactly ONE character");
    check(rcOfSrc(fixture + "capture unab z : zz*") == "111",
          "K1 glob: a pattern matching nothing is r(111), which is what Stata answers -- not this "
          "expander's old private message at rc 1");
  }
}


// BETA campaign Phase B (B-data cluster): sort real dispatch (BS-001),
// destring/tostring (BS-003/D4), joinby (BS-004/D5), format (BS-005/D6).

void testBS001SortRealDispatch() {
  std::printf("[selftest] --- BS-001 `sort` real dispatch (was silent no-op) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs001").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("keep in 1/30", session);
  // Force a KNOWN non-ascending order first via the already-correct
  // `gsort`, so the subsequent `sort` has unambiguous work to do —
  // isolates this test from any assumption about fatalities' own natural
  // row order.
  (void)piet::dispatch("gsort -year", session);

  piet::Datastore* dsDesc = session.datastore();
  bool haveFixture = dsDesc != nullptr && dsDesc->nRows() > 1 && dsDesc->has("year");
  check(haveFixture, "BS-001 setup: fixture loaded with >1 row and a `year` column");
  if (!haveFixture) {
    fs::remove_all(workdir, ec);
    return;
  }

  bool wasNonIncreasing = true;
  {
    const double* years = dsDesc->col("year").f64();
    for (int64_t i = 1; i < dsDesc->nRows(); ++i) {
      if (years[i] > years[i - 1]) {
        wasNonIncreasing = false;
        break;
      }
    }
  }
  check(wasNonIncreasing, "BS-001 setup: `gsort -year` produced a non-increasing `year` column");

  auto blocks = piet::dispatch("sort year", session);
  bool sawError = false;
  for (const auto& b : blocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") sawError = true;
  }
  check(!sawError, "BS-001: `sort year` does not error");

  piet::Datastore* ds = session.datastore();
  bool nowNonDecreasing = (ds != nullptr && ds->has("year"));
  if (nowNonDecreasing) {
    const double* years = ds->col("year").f64();
    for (int64_t i = 1; i < ds->nRows(); ++i) {
      if (years[i] < years[i - 1]) {
        nowNonDecreasing = false;
        break;
      }
    }
  }
  check(nowNonDecreasing,
        "BS-001: `sort year` actually reorders rows ascending — previously a silent no-op "
        "(it shelled out to `eval --sort`, a flag cobra's `eval` verb never recognized; cobra's "
        "own `gsort` verb is the real thing)");

  fs::remove_all(workdir, ec);
}


void testC1BS024HettestVarlist() {
  std::printf("[selftest] --- C1/BS-024 estat hettest explicit varlist = auxiliary test "
              "regressors ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c1_bs024").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen high = (beertax > .5)", session);
  (void)piet::dispatch("regress income beertax", session);
  auto blocks = piet::dispatch("estat hettest spirits unemp", session);

  const nlohmann::ordered_json* est = nullptr;
  for (const auto& b : blocks) {
    if (b.is_object() && b.value("type", std::string()) == "estimation") est = &b;
  }
  check(est != nullptr, "C1/BS-024 setup: `estat hettest spirits unemp` returns an estimation "
        "block");
  if (est != nullptr) {
    // Oracle (real Stata): chi2(2) = 14.82 for `estat hettest spirits
    // unemp` after `regress income beertax`. Previously this arm silently
    // dropped "spirits" (misread as a replacement y for the base
    // regression) and only tested against "unemp" alone -- df=1,
    // chi2~=5.97, a completely different, silently-wrong number.
    check(est->contains("df"), "C1/BS-024: the block has a df field");
    if (est->contains("df")) {
      double df = (*est)["df"].get<double>();
      check(std::abs(df - 2.0) < 0.01,
            "C1/BS-024: df==2 (both spirits AND unemp used as auxiliary regressors), got " +
                std::to_string(df));
    }
    check(est->contains("chi2") && (*est)["chi2"].is_number(), "C1/BS-024: the block has a "
          "numeric chi2");
    if (est->contains("chi2") && (*est)["chi2"].is_number()) {
      double chi2 = (*est)["chi2"].get<double>();
      check(std::abs(chi2 - 14.82) < 0.1,
            "C1/BS-024: chi2 ~= 14.82 (oracle), got " + std::to_string(chi2));
    }
  }

  // Regression guard: the lastModel FALLBACK path (no explicit varlist)
  // still uses lastModel's y/x exactly as before (untouched by this fix).
  auto blocksPlain = piet::dispatch("estat hettest", session);
  bool sawPlainError = false;
  for (const auto& b : blocksPlain) {
    if (b.is_object() && b.value("type", std::string()) == "error") sawPlainError = true;
  }
  check(!sawPlainError, "C1/BS-024 regression guard: `estat hettest` (no varlist, fallback to "
        "lastModel) still succeeds");

  fs::remove_all(workdir, ec);
}


// ---------------------------------------------------------------------------
// BETA campaign Phase C batch C2 (data-verb integrity): BS-030 tsset/xtset
// delta(), BS-031 silent-if/in-drop sweep, BS-032 egen group()/tag()+by
// reject, corr `obs` field composing with C1/BS-028.

void testC2BS030TssetDelta() {
  std::printf("[selftest] --- C2/BS-030 tsset delta() forward + honest reject ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c2_bs030").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen high = (beertax > .5)", session);

  // Reject case (live-verified against StataBE this stint — the shipped
  // oracle log's own BS-030 stanza covers exactly this scenario): t=_n has
  // a minimum gap of 1; declaring delta(2) is narrower than the data
  // actually supports -> real Stata errors "time values with period less
  // than delta() found" and the ts context is left UNSET.
  (void)piet::dispatch("gen t=_n", session);
  auto blocksReject = piet::dispatch("tsset t, delta(2)", session);
  bool sawRejectError = false;
  std::string rejectMsg;
  for (const auto& b : blocksReject) {
    if (b.is_object() && b.value("type", std::string()) == "error") {
      sawRejectError = true;
      rejectMsg = b.value("message", std::string());
    }
  }
  check(sawRejectError, "C2/BS-030: `tsset t, delta(2)` on gap-1 data is an honest reject");
  check(rejectMsg.find("time values with period less than delta") != std::string::npos,
        "C2/BS-030: reject message matches the oracle text, got: " + rejectMsg);
  check(!session.timevar.has_value(),
        "C2/BS-030: the ts context stays UNSET after the rejected tsset (matches the oracle's "
        "subsequent `gen lag1=L.income` -> \"time variable not set\")");

  // B4-W3b SUPERSEDES BS-030's reject here. The period options were rejected
  // because nothing on this side represented them; they are now implemented,
  // and Stata's own answer is rc 0 with the format landing on r(tsfmt) AND on
  // the column (pins_oracle_g.log:257-273, `tsset t, quarterly` ->
  // r(tsfmt) "%tq" + `describe t` %tq). Keeping the old assertion would be
  // pinning a divergence: this leg now asserts what StataBE does, on the same
  // spelling the old one used.
  auto blocksUnitopt = piet::dispatch("tsset t, daily", session);
  bool sawUnitoptError = false;
  for (const auto& b : blocksUnitopt) {
    if (b.is_object() && b.value("type", std::string()) == "error") sawUnitoptError = true;
  }
  check(!sawUnitoptError,
        "C2/BS-030 -> B4-W3b: `tsset t, daily` is now rc 0 (Stata's own answer), not the "
        "unitoption reject BS-030 shipped");
  {
    const std::string* dailyFmt = session.eval.rStrings.find("tsfmt");
    check(dailyFmt != nullptr && *dailyFmt == "%td",
          "C2/BS-030 -> B4-W3b: ...and `daily` names %td in r(tsfmt) — an accepted-and-ignored "
          "period would be rc 0 with the %tg default still standing");
  }
  // Restore the state the legs below expect: they re-tsset t2 anyway, but the
  // format the line above put on `t` must not travel into them as a surprise.
  (void)piet::dispatch("tsset t", session);

  // Success + forward case (live-verified against StataBE this stint): t2
  // is spaced by EXACTLY delta(2), so tsset succeeds, and delta must reach
  // the engine for L. to resolve at all. Before this fix, delta was
  // silently dropped -> the engine defaulted to delta=1 -> `regress income
  // L.spirits` errored "[no_obs] not enough observations: n <= k" (no row's
  // t2-1 matched another row's t2, since all t2 values are even).
  (void)piet::dispatch("gen t2=_n*2", session);
  auto blocksOk = piet::dispatch("tsset t2, delta(2)", session);
  bool sawOkError = false;
  for (const auto& b : blocksOk) {
    if (b.is_object() && b.value("type", std::string()) == "error") sawOkError = true;
  }
  check(!sawOkError, "C2/BS-030: `tsset t2, delta(2)` on gap-2 data succeeds");

  auto blocksEst = piet::dispatch("regress income L.spirits", session);
  const nlohmann::ordered_json* est = nullptr;
  for (const auto& b : blocksEst) {
    if (b.is_object() && b.value("type", std::string()) == "estimation") est = &b;
  }
  check(est != nullptr,
        "C2/BS-030: `regress income L.spirits` under delta(2) succeeds -- delta reached the "
        "engine via --tsdelta (previously errored \"not enough observations\")");
  if (est != nullptr) {
    double nObs = est->value("n_obs", 0.0);
    check(std::abs(nObs - 335.0) < 0.5,
          "C2/BS-030: n_obs == 335 (one row lost to the lag, oracle-pinned), got " +
              std::to_string(nObs));
  }

  fs::remove_all(workdir, ec);
}


void testXtsetPhysicalSortAndN() {
  std::printf("[selftest] --- xtset physically sorts panel/time before downstream _n ---\n");
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_xtset_sort").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("set obs 4", session);
  (void)piet::dispatch("gen panel=(_n==1)+(_n==3)+1", session);  // 2,1,2,1
  (void)piet::dispatch("gen time=(_n<=2)+1", session);           // 2,2,1,1
  (void)piet::dispatch("gen before=_n", session);

  auto xtset = piet::dispatch("xtset panel time", session);
  bool sawError = false;
  bool sawMutation = false;
  for (const auto& b : xtset) {
    if (!b.is_object()) continue;
    sawError = sawError || b.value("type", std::string()) == "error";
    sawMutation = sawMutation || b.value("type", std::string()) == "data_mutation";
  }
  check(!sawError, "xtset sort: unsorted unique panel/time data succeeds");
  check(!sawMutation,
        "xtset sort: physical reorder preserves xtset's established echo-only output blocks");

  (void)piet::dispatch("gen after=_n", session);
  piet::Datastore* ds = session.datastore();
  check(ds != nullptr && ds->has("panel") && ds->has("time") && ds->has("before") &&
            ds->has("after"),
        "xtset sort: sorted fixture columns remain available");
  if (ds != nullptr && ds->has("panel") && ds->has("time") && ds->has("before") &&
      ds->has("after")) {
    const double* panel = ds->col("panel").f64();
    const double* time = ds->col("time").f64();
    const double* before = ds->col("before").f64();
    const double* after = ds->col("after").f64();
    check(panel[0] == 1.0 && panel[1] == 1.0 && panel[2] == 2.0 && panel[3] == 2.0 &&
              time[0] == 1.0 && time[1] == 2.0 && time[2] == 1.0 && time[3] == 2.0,
          "xtset sort: live rows are physically ordered by panel then time");
    check(before[0] == 4.0 && before[1] == 2.0 && before[2] == 3.0 && before[3] == 1.0,
          "xtset sort: pre-sort row identity follows the physical reorder (4,2,3,1)");
    check(after[0] == 1.0 && after[1] == 2.0 && after[2] == 3.0 && after[3] == 4.0,
          "xtset sort: downstream `gen after=_n` uses the sorted row order");
  }

  fs::remove_all(workdir, ec);
}


void testC2BS032EgenGroupTagByReject() {
  std::printf("[selftest] --- C2/BS-032 egen group()/tag() + by-context reject ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c2_bs032").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen high = (beertax > .5)", session);

  // bysort-injected by() form.
  auto blocksBysort = piet::dispatch("bysort state: egen g1=group(year)", session);
  bool sawBysortError = false;
  std::string bysortMsg;
  for (const auto& b : blocksBysort) {
    if (b.is_object() && b.value("type", std::string()) == "error") {
      sawBysortError = true;
      bysortMsg = b.value("message", std::string());
    }
  }
  check(sawBysortError,
        "C2/BS-032: `bysort state: egen g1=group(year)` is an honest reject (real Stata: "
        "\"group() may not be combined with by\", r(190)) -- previously ran silently");
  check(bysortMsg.find("group()") != std::string::npos && bysortMsg.find("by") != std::string::npos,
        "C2/BS-032: reject message names group() and by, got: " + bysortMsg);

  // Explicit by() option form.
  auto blocksExplicit = piet::dispatch("egen g2=tag(year), by(state)", session);
  bool sawExplicitError = false;
  for (const auto& b : blocksExplicit) {
    if (b.is_object() && b.value("type", std::string()) == "error") sawExplicitError = true;
  }
  check(sawExplicitError,
        "C2/BS-032: `egen g2=tag(year), by(state)` (explicit by()) is ALSO an honest reject");

  // Regression guard: every OTHER egen function still legitimately combines
  // with by() -- this fix must not overreach.
  auto blocksOther = piet::dispatch("bysort state: egen g3=mean(income)", session);
  bool sawOtherError = false;
  for (const auto& b : blocksOther) {
    if (b.is_object() && b.value("type", std::string()) == "error") sawOtherError = true;
  }
  check(!sawOtherError,
        "C2/BS-032 regression guard: `bysort state: egen g3=mean(income)` (non group/tag "
        "function) still succeeds -- the by-reject is scoped to group()/tag() only");

  fs::remove_all(workdir, ec);
}


void testC4BS044ClearScoping() {
  std::printf("[selftest] --- C4/BS-044 `clear <subcmd>` scoping ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c4_bs044").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // (a) `clear results` — oracle-pinned: dataset (count=336) AND scalars
  // survive; this batch additionally clears e()/r() state (lastModel).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("scalar sca1_bs044 = 42", session);
    (void)piet::dispatch("regress income beertax", session);
    check(session.lastModel().has_value(),
          "C4/BS-044 setup: `regress` populates lastModel before `clear results`");

    (void)piet::dispatch("clear results", session);
    check(session.hasData(), "C4/BS-044: `clear results` KEEPS the loaded dataset");
    auto countBlocks = piet::dispatch("count", session);
    bool countErrored = false;
    for (const auto& b : countBlocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") countErrored = true;
    }
    check(!countErrored,
          "C4/BS-044: `count` after `clear results` succeeds (dataset was NOT wiped, the "
          "pre-fix bug)");
    check(session.eval.scalars.get("sca1_bs044", "") == "42",
          "C4/BS-044: `clear results` does NOT touch user scalars");
    check(!session.lastModel().has_value(),
          "C4/BS-044: `clear results` DOES clear the e()-state (lastModel)");
  }

  // (b) `clear matrix` — shares the results branch (r()/e() cleared) PLUS
  // drops user matrices + the eststo store. Measured on StataBE
  // (results and matrix share a branch; only matrix also drops all matrices
  // and the estimates store) — a deliberate
  // deviation from this batch's task-brief shorthand, which grouped
  // "matrix" with the no-backing-store subcommands; PIET already has a
  // real matrix store (eval.matrices) and est store (session.estimates),
  // so implementing the real behavior costs no new infrastructure. See
  // this task's return report.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("scalar sca2_bs044 = 7", session);
    (void)piet::dispatch("regress income beertax", session);
    (void)piet::dispatch("matrix mb_bs044 = e(b)", session);
    (void)piet::dispatch("eststo m1_bs044", session);
    check(!session.eval.matrices.empty(), "C4/BS-044 setup: `matrix mb = e(b)` populates a matrix");
    check(!session.estimatesNames().empty(), "C4/BS-044 setup: `eststo` populates the est store");

    (void)piet::dispatch("clear matrix", session);
    check(session.hasData(), "C4/BS-044: `clear matrix` KEEPS the loaded dataset");
    check(session.eval.scalars.get("sca2_bs044", "") == "7",
          "C4/BS-044: `clear matrix` does NOT touch user scalars");
    check(session.eval.matrices.empty(), "C4/BS-044: `clear matrix` drops user matrices");
    check(session.estimatesNames().empty(), "C4/BS-044: `clear matrix` drops the est store");
    check(!session.lastModel().has_value(),
          "C4/BS-044: `clear matrix` also clears e()-state (shares the results branch)");
  }

  // (c) `clear programs` — real backing store (session.programs). Built
  // directly (not via `program define ... end` through dispatch(): that
  // multi-line block form is captured pre-tokenizer by doexec's own
  // program::captureProgams over a WHOLE do-file's lines, not a single
  // dispatch() call — see dispatcher.cpp's own doc comment on this) since
  // this leg only needs a populated store, not real program-execution
  // semantics.
  {
    piet::SessionState session(workdir);
    piet::ProgramDef pd;
    pd.name = "pfoo_bs044";
    pd.body = {"di 1"};
    session.programs["pfoo_bs044"] = pd;
    check(!session.programs.empty(), "C4/BS-044 setup: session.programs is populated");
    (void)piet::dispatch("clear programs", session);
    check(session.programs.empty(), "C4/BS-044: `clear programs` drops captured programs");
  }

  // (d) no-backing-store subcommands (mata/ado/rngstream/frames/collect) —
  // honest scoped no-ops: must NOT wipe the dataset (the pre-fix bug every
  // subcommand shared).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    for (const std::string& sub : {"mata", "ado", "rngstream", "frames", "collect"}) {
      auto blocks = piet::dispatch("clear " + sub, session);
      bool sawError = false;
      for (const auto& b : blocks) {
        if (b.is_object() && b.value("type", std::string()) == "error") sawError = true;
      }
      check(!sawError, "C4/BS-044: `clear " + sub + "` is not an error");
      check(session.hasData(), "C4/BS-044: `clear " + sub + "` does NOT wipe the dataset");
    }
  }

  // (e) bare `clear` still wipes the dataset (that IS real Stata) but
  // leaves scalars/matrices untouched (Stata-verified: bare `clear`
  // drops the data and value labels ONLY).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("scalar sca3_bs044 = 1", session);
    (void)piet::dispatch("clear", session);
    check(!session.hasData(), "C4/BS-044: bare `clear` still wipes the dataset");
    check(session.eval.scalars.get("sca3_bs044", "") == "1",
          "C4/BS-044: bare `clear` does NOT touch scalars (Stata-verified)");
  }

  // (f) `clear all` wipes dataset + scalars + programs.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("scalar sca4_bs044 = 1", session);
    piet::ProgramDef pd;
    pd.name = "pbar_bs044";
    pd.body = {"di 1"};
    session.programs["pbar_bs044"] = pd;
    (void)piet::dispatch("clear all", session);
    check(!session.hasData(), "C4/BS-044: `clear all` wipes the dataset");
    check(session.eval.scalars.get("sca4_bs044", "") == "",
          "C4/BS-044: `clear all` wipes user scalars too");
    check(session.programs.empty(), "C4/BS-044: `clear all` wipes captured programs");
  }

  // (g) unrecognized subcommand — hard reject (rc=198), not a silent
  // dataset wipe or no-op.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("capture clear bogus_subcmd_c4", session);
    check(session.eval.locals.get("_rc", "") == "198",
          "C4/BS-044: `clear <unrecognized>` is a hard reject (rc=198), not a dataset wipe");
    check(session.hasData(), "C4/BS-044: the rejected `clear bogus_subcmd_c4` did NOT wipe data");
  }

  fs::remove_all(workdir, ec);
}


// BETA campaign Phase G (BS-090): `in a/b` row-range expansion beyond
// list/browse/gen/replace — summarize/sum, count, ttest, corr/correlate/
// pwcorr, regress/reg/r/feols/reghdfe, tabulate/tab/tab1/tab2, duplicates
// (report/tag/list real, drop honest-rejects — same disclosed boundary as
// its pre-existing `if` reject), egen, drop, keep, collapse. Every leg
// below cross-checks the qualifier-restricted call against an
// INDEPENDENTLY reduced copy (`keep in a/b` on a fresh session, then the
// plain unqualified command) rather than a hardcoded oracle number — this
// is self-verifying (two independent code paths must agree) and exercises
// `keep`'s own new `in` support as a side effect of every leg. All legs use
// the bundled `fatalities` panel, whose rows are state-blocked in CSV
// appearance order (7 consecutive rows per state, live-verified this
// stint) — `in 1/N` for N a multiple of 7 always selects a whole number of
// complete states.
void testBS090InRangeExpansion() {
  std::printf("[selftest] --- BS-090 `in` row-range expansion (BETA campaign Phase G) ---\n");
  namespace fs = std::filesystem;
  std::string base = (fs::temp_directory_path() / "piet_native_selftest_bs090").u8string();
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base, ec);
  std::string wdA = base + "/a";
  std::string wdB = base + "/b";
  fs::create_directories(wdA, ec);
  fs::create_directories(wdB, ec);
  piet::SessionState sessA(wdA);  // runs the `in`-qualified command directly
  piet::SessionState sessB(wdB);  // runs `keep in a/b` then the plain command

  auto freshA = [&]() { (void)piet::dispatch("sysuse fatalities, clear", sessA); };
  auto freshBReduced = [&](const std::string& range) {
    (void)piet::dispatch("sysuse fatalities, clear", sessB);
    (void)piet::dispatch("keep in " + range, sessB);
  };
  auto findBlockOfType = [](const nlohmann::ordered_json& blocks,
                            const std::string& type) -> nlohmann::ordered_json {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == type) return b;
    }
    return nlohmann::ordered_json();
  };
  auto hasError = [](const nlohmann::ordered_json& blocks) {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") return true;
    }
    return false;
  };

  // (a) summarize ... in a/b
  freshA();
  freshBReduced("1/5");
  {
    auto blocksA = piet::dispatch("summarize beertax in 1/5", sessA);
    auto blocksB = piet::dispatch("summarize beertax", sessB);
    check(!hasError(blocksA), "BS-090(a): `summarize beertax in 1/5` does not error");
    auto sumA = findBlockOfType(blocksA, "summarize");
    auto sumB = findBlockOfType(blocksB, "summarize");
    bool ok = !sumA.is_null() && !sumB.is_null() && sumA.contains("stats") &&
              sumA["stats"].contains("beertax") && sumB.contains("stats") &&
              sumB["stats"].contains("beertax");
    check(ok, "BS-090(a): both `summarize beertax in 1/5` and the keep-in-1/5-reduced copy "
              "return beertax stats");
    if (ok) {
      const auto& sa = sumA["stats"]["beertax"];
      const auto& sb = sumB["stats"]["beertax"];
      check(sa.value("N", -1.0) == 5.0, "BS-090(a): `in 1/5` restricts to exactly 5 observations");
      check(sa.value("N", -1.0) == sb.value("N", -2.0),
            "BS-090(a): N matches the independently keep-in-1/5-reduced copy");
      check(std::abs(sa.value("mean", 0.0) - sb.value("mean", 1.0)) < 1e-9,
            "BS-090(a): mean matches the independently keep-in-1/5-reduced copy");
    }
  }

  // (b) count ... in a/b (value-independent: `in 1/5` is always exactly 5
  // rows regardless of the composition mechanism).
  freshA();
  {
    auto blocksA = piet::dispatch("count in 1/5", sessA);
    check(!hasError(blocksA), "BS-090(b): `count in 1/5` does not error");
    auto dispBlock = findBlockOfType(blocksA, "display");
    check(!dispBlock.is_null() && dispBlock.value("text", std::string()) == "5",
          "BS-090(b): `count in 1/5` == 5");
  }

  // (c) drop/keep in a/b: `keep in 1/5` -> 5 rows left; `drop in 1/5` on a
  // FRESH copy -> 336-5=331 rows left (drop's `in` is the COMPLEMENT of the
  // range, matching runDropIf's pre-existing complement-of-if idiom).
  freshA();
  {
    auto blocksKeep = piet::dispatch("keep in 1/5", sessA);
    auto mutKeep = findBlockOfType(blocksKeep, "data_mutation");
    check(!mutKeep.is_null() && mutKeep.value("n_rows_after", -1) == 5,
          "BS-090(c): `keep in 1/5` leaves exactly 5 rows");
  }
  freshA();
  {
    auto blocksDrop = piet::dispatch("drop in 1/5", sessA);
    auto mutDrop = findBlockOfType(blocksDrop, "data_mutation");
    check(!mutDrop.is_null() && mutDrop.value("n_rows_after", -1) == 331,
          "BS-090(c): `drop in 1/5` leaves 336-5=331 rows (complement of the range)");
  }

  // (d) egen ... in a/b: computed ONLY over the in-sample, missing outside
  // it (same contract egen's pre-existing `if` support already has).
  freshA();
  {
    auto blocksEgen = piet::dispatch("egen gm5 = mean(beertax) in 1/5", sessA);
    check(!hasError(blocksEgen), "BS-090(d): `egen gm5 = mean(beertax) in 1/5` does not error");
    auto sumBlocks = piet::dispatch("summarize gm5", sessA);
    auto sumGm = findBlockOfType(sumBlocks, "summarize");
    bool ok = !sumGm.is_null() && sumGm.contains("stats") && sumGm["stats"].contains("gm5");
    check(ok && sumGm["stats"]["gm5"].value("N", -1.0) == 5.0,
          "BS-090(d): `egen gm5 = mean(beertax) in 1/5` has exactly 5 non-missing rows (the "
          "other 331 are missing, matching the if-merge-back contract)");
    if (ok) {
      double gmMean = sumGm["stats"]["gm5"].value("mean", 0.0);
      // Every in-sample row carries the SAME broadcast group value, so
      // gm5's own mean == mean(beertax) over the in-sample — cross-check
      // against the keep-in-1/5-reduced copy's plain summarize.
      freshBReduced("1/5");
      auto plainBlocks = piet::dispatch("summarize beertax", sessB);
      auto sumB = findBlockOfType(plainBlocks, "summarize");
      if (sumB.contains("stats") && sumB["stats"].contains("beertax")) {
        double refMean = sumB["stats"]["beertax"].value("mean", 1.0);
        check(std::abs(gmMean - refMean) < 1e-6,
              "BS-090(d): egen's in-sample mean matches the independently keep-in-1/5-reduced "
              "copy's plain summarize mean");
      }
    }
  }

  // (e) corr ... in a/b
  freshA();
  freshBReduced("1/10");
  {
    auto blocksA = piet::dispatch("corr beertax income in 1/10", sessA);
    auto blocksB = piet::dispatch("corr beertax income", sessB);
    check(!hasError(blocksA), "BS-090(e): `corr beertax income in 1/10` does not error");
    auto estA = findBlockOfType(blocksA, "estimation");
    auto estB = findBlockOfType(blocksB, "estimation");
    check(!estA.is_null() && !estB.is_null() && estA.value("n_obs", -1) == 10 &&
              estA.value("n_obs", -1) == estB.value("n_obs", -2),
          "BS-090(e): n_obs==10, matching the independently keep-in-1/10-reduced copy");
  }

  // (f) ttest ... in a/b
  freshA();
  freshBReduced("1/10");
  {
    auto blocksA = piet::dispatch("ttest beertax in 1/10", sessA);
    auto blocksB = piet::dispatch("ttest beertax", sessB);
    check(!hasError(blocksA), "BS-090(f): `ttest beertax in 1/10` does not error");
    auto estA = findBlockOfType(blocksA, "estimation");
    auto estB = findBlockOfType(blocksB, "estimation");
    check(!estA.is_null() && !estB.is_null() && estA.value("n_obs", -1) == 10 &&
              estA.value("n_obs", -1) == estB.value("n_obs", -2),
          "BS-090(f): n_obs==10, matching the independently keep-in-1/10-reduced copy");
  }

  // (g) regress/feols ... in a/b
  freshA();
  freshBReduced("1/20");
  {
    auto blocksA = piet::dispatch("regress income beertax in 1/20", sessA);
    auto blocksB = piet::dispatch("regress income beertax", sessB);
    check(!hasError(blocksA), "BS-090(g): `regress income beertax in 1/20` does not error");
    auto estA = findBlockOfType(blocksA, "estimation");
    auto estB = findBlockOfType(blocksB, "estimation");
    check(!estA.is_null() && !estB.is_null() && estA.value("n_obs", -1) == 20 &&
              estA.value("n_obs", -1) == estB.value("n_obs", -2),
          "BS-090(g): n_obs==20, matching the independently keep-in-1/20-reduced copy");
    if (!estA.is_null() && !estB.is_null()) {
      double bA = -1.0, bB = -2.0;
      bool foundA = false, foundB = false;
      for (const auto& row : estA.value("coeftable", nlohmann::ordered_json::array())) {
        if (row.value("name", std::string()) == "beertax") { bA = row.value("b", -1.0); foundA = true; }
      }
      for (const auto& row : estB.value("coeftable", nlohmann::ordered_json::array())) {
        if (row.value("name", std::string()) == "beertax") { bB = row.value("b", -2.0); foundB = true; }
      }
      check(foundA && foundB && std::abs(bA - bB) < 1e-6,
            "BS-090(g): the `beertax` coeftable row's `b` matches the independently "
            "keep-in-1/20-reduced copy");
    }
  }

  // (h) tabulate ... in a/b (2 full states' worth of the state-blocked
  // fixture — 14 rows, 2 distinct `state` values).
  freshA();
  freshBReduced("1/14");
  {
    auto blocksA = piet::dispatch("tabulate state in 1/14", sessA);
    auto blocksB = piet::dispatch("tabulate state", sessB);
    check(!hasError(blocksA), "BS-090(h): `tabulate state in 1/14` does not error");
    auto tabA = findBlockOfType(blocksA, "tabulate");
    auto tabB = findBlockOfType(blocksB, "tabulate");
    check(!tabA.is_null() && !tabB.is_null() && tabA == tabB,
          "BS-090(h): `tabulate state in 1/14` byte-matches the independently "
          "keep-in-1/14-reduced copy's plain `tabulate state`");
  }

  // (i) duplicates report ... in a/b (same 14-row, 2-state slice: `year`
  // has each of 7 distinct values appearing exactly twice -> 7 duplicate
  // groups).
  freshA();
  freshBReduced("1/14");
  {
    auto blocksA = piet::dispatch("duplicates report year in 1/14", sessA);
    auto blocksB = piet::dispatch("duplicates report year", sessB);
    check(!hasError(blocksA), "BS-090(i): `duplicates report year in 1/14` does not error");
    auto dA = findBlockOfType(blocksA, "display");
    auto dB = findBlockOfType(blocksB, "display");
    check(!dA.is_null() && !dB.is_null() &&
              dA.value("text", std::string()) == dB.value("text", std::string()),
          "BS-090(i): `duplicates report year in 1/14` byte-matches the independently "
          "keep-in-1/14-reduced copy's plain `duplicates report year`");
  }

  // (j) duplicates drop ... in a/b — honest reject, same disclosed boundary
  // as its pre-existing `if` reject (dedup-then-splice-back needs more than
  // a simple filter). `force' is spelled: without it duplicates refuses a
  // varlist drop first (r(198); w2ah_data_verbs.log
  // ah_dup_drop_noforce), so the `in' refusal is only reachable with it.
  freshA();
  {
    auto blocksA = piet::dispatch("duplicates drop year in 1/14, force", sessA);
    auto errA = findBlockOfType(blocksA, "error");
    check(!errA.is_null(), "BS-090(j): `duplicates drop year in 1/14, force` still honestly rejects "
                           "(same disclosed boundary as its pre-existing `if` reject)");
    check(errA.value("message", std::string()).find("with `in`") != std::string::npos,
          "BS-090(j): reject message names the `in` qualifier specifically — got: " +
              errA.value("message", std::string()));
  }

  // (k) collapse ... in a/b, by(state) — 2-state slice collapses to
  // exactly 2 rows.
  freshA();
  freshBReduced("1/14");
  {
    auto blocksA = piet::dispatch("collapse (mean) beertax in 1/14, by(state)", sessA);
    auto blocksB = piet::dispatch("collapse (mean) beertax, by(state)", sessB);
    check(!hasError(blocksA), "BS-090(k): `collapse (mean) beertax in 1/14, by(state)` does not "
                              "error");
    auto mutA = findBlockOfType(blocksA, "data_mutation");
    auto mutB = findBlockOfType(blocksB, "data_mutation");
    check(!mutA.is_null() && mutA.value("n_rows_after", -1) == 2 &&
              mutA.value("n_rows_after", -1) == mutB.value("n_rows_after", -2),
          "BS-090(k): collapsing the in-1/14 sample by state gives exactly 2 rows, matching the "
          "independently keep-in-1/14-reduced copy");
  }

  fs::remove_all(base, ec);
}


// BETA campaign Phase G (BS-092): `by:`/`bysort: summarize` real per-group
// dispatch (dispatcher.cpp's by/bysort interception block) — the ONE OTHER
// subcommand besides gen/replace/egen with a genuine PIET-side by-group
// implementation this phase. Cross-checks each group's own stats against a
// bare `summarize X if G==val` for that SAME group value (the task brief's
// own suggested verification), on a small, deterministic 2-group fixture
// (the bundled fatalities panel's first 14 rows — exactly 2 states' worth,
// 7 years each, per the CSV's own state-blocked appearance order).
void testBS092BysortSummarize() {
  std::printf("[selftest] --- BS-092 `by:`/`bysort: summarize` grouped dispatch (BETA campaign "
              "Phase G) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs092").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  // Exactly 2 states' worth (BS-090(c)/(k) already cover `keep in`'s own
  // correctness — used here purely as fixture setup for a small, fast,
  // deterministic 2-group dataset).
  (void)piet::dispatch("keep in 1/14", session);

  piet::Datastore* ds = session.datastore();
  bool haveFixture = ds != nullptr && ds->nRows() == 14 && ds->has("state") && ds->has("beertax");
  check(haveFixture, "BS-092 setup: 14-row, 2-state fixture loaded");
  if (!haveFixture) {
    fs::remove_all(workdir, ec);
    return;
  }

  // The two distinct `state` levels actually present, read directly off the
  // factor column (rather than assumed) so this doesn't hardcode which
  // postal codes the bundled CSV happens to use.
  std::string state1, state2;
  {
    const auto& levels = ds->varinfo("state").levels;
    const int32_t* codes = ds->col("state").codes();
    check(levels.size() >= 2, "BS-092 setup: at least 2 distinct `state` levels present");
    if (levels.size() >= 2 && codes[0] >= 0 && codes[13] >= 0) {
      state1 = levels[static_cast<size_t>(codes[0])];
      state2 = levels[static_cast<size_t>(codes[13])];
      check(state1 != state2, "BS-092 setup: row 1 and row 14 are DIFFERENT states");
    }
  }

  auto blocks = piet::dispatch("bysort state: summarize beertax", session);
  bool sawError = false;
  for (const auto& b : blocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") sawError = true;
  }
  check(!sawError, "BS-092: `bysort state: summarize beertax` does not error (was an honest "
                   "reject before this phase)");

  // Collect the (display header, summarize block) pairs in emission order.
  std::vector<std::pair<std::string, nlohmann::ordered_json>> groups;
  {
    std::string pendingHeader;
    for (const auto& b : blocks) {
      if (!b.is_object()) continue;
      if (b.value("type", std::string()) == "display") {
        pendingHeader = b.value("text", std::string());
      } else if (b.value("type", std::string()) == "summarize") {
        groups.emplace_back(pendingHeader, b);
      }
    }
  }
  check(groups.size() == 2,
        "BS-092: exactly 2 group blocks emitted (one per distinct `state` value) — got " +
            std::to_string(groups.size()));
  if (groups.size() == 2 && !state1.empty() && !state2.empty()) {
    check(groups[0].first == "-> state = " + state1,
          "BS-092: first group's header is `-> state = " + state1 + "` — got `" +
              groups[0].first + "`");
    check(groups[1].first == "-> state = " + state2,
          "BS-092: second group's header is `-> state = " + state2 + "` — got `" +
              groups[1].first + "`");

    for (size_t gi = 0; gi < 2; ++gi) {
      const std::string& sval = (gi == 0) ? state1 : state2;
      const auto& grpBlock = groups[gi].second;
      bool haveStats = grpBlock.contains("stats") && grpBlock["stats"].contains("beertax");
      check(haveStats,
            "BS-092: group " + std::to_string(gi) + "'s summarize block has beertax stats");
      if (!haveStats) continue;
      const auto& grpStats = grpBlock["stats"]["beertax"];

      // Cross-check against a bare `summarize beertax if state=="VAL"` for
      // this SAME group value.
      auto refBlocks = piet::dispatch("summarize beertax if state==\"" + sval + "\"", session);
      nlohmann::ordered_json refSum;
      for (const auto& b : refBlocks) {
        if (b.is_object() && b.value("type", std::string()) == "summarize") refSum = b;
      }
      bool refOk = refSum.is_object() && refSum.contains("stats") &&
                   refSum["stats"].contains("beertax");
      check(refOk, "BS-092: `summarize beertax if state==\"" + sval + "\"` cross-check succeeds");
      if (!refOk) continue;
      const auto& refStats = refSum["stats"]["beertax"];
      check(grpStats.value("N", -1.0) == refStats.value("N", -2.0),
            "BS-092: group " + std::to_string(gi) + "'s N matches `summarize ... if state==\"" +
                sval + "\"` (N=" + std::to_string(grpStats.value("N", -1.0)) + ")");
      check(std::abs(grpStats.value("mean", 0.0) - refStats.value("mean", 1.0)) < 1e-9,
            "BS-092: group " + std::to_string(gi) + "'s mean matches the if-restricted "
            "cross-check");
      check(std::abs(grpStats.value("sd", 0.0) - refStats.value("sd", 1.0)) < 1e-9,
            "BS-092: group " + std::to_string(gi) + "'s sd matches the if-restricted cross-check");
    }
  }

  // `count` graduated out of the BS-107 bucket (FU-163): under by it runs per group.
  {
    auto rejBlocks = piet::dispatch("bysort state: count", session);
    bool sawReject = false;
    for (const auto& b : rejBlocks)
      if (b.is_object() && b.value("type", std::string()) == "error") sawReject = true;
    check(!sawReject, "BS-092: `bysort: count` runs per group (FU-163, graduated out of BS-107)");
  }

  fs::remove_all(workdir, ec);
}


// BUG-105 (옵션망): `merge`/`reshape`/`order` used to silently ignore any
// option token their opts-loop didn't explicitly read (e.g. a real Stata
// `merge` option this build doesn't implement, like `force`) — same class as
// the `save` suboption fix (@c638da1). Each verb now runs its known-option
// set through eval::rejectUnknownOptions (handlers_data_verbs.cpp), naming
// the first unrecognized key instead of swallowing it.
void testBUG105OptNetDataVerbs() {
  std::printf("[selftest] --- BUG-105 옵션망: merge/reshape/order residual-option honest-reject ---\n");
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_bug105_optnet").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto has = [](const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
  };
  auto firstError = [](const nlohmann::ordered_json& blocks) -> std::string {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") {
        return b.value("message", std::string());
      }
    }
    return std::string();
  };

  // --- merge: keepusing()/nogenerate/keep()/assert() known; a real Stata
  // merge option this build doesn't implement (`force`) is now a DISTINCT
  // named reject from a genuine typo (BUG-105 secondary ⑤) — was a plain
  // "not allowed" reject (still true for the typo case), not a silent no-op.
  {
    const std::string usingPath = workdir + "/merge_using.hbin";
    {
      piet::SessionState setup(workdir);
      (void)piet::dispatch("clear", setup);
      (void)piet::dispatch("set obs 2", setup);
      (void)piet::dispatch("gen double gid = _n", setup);
      (void)piet::dispatch("gen double uval = _n * 10", setup);
      (void)piet::dispatch("save \"" + usingPath + "\", replace", setup);
    }
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 2", session);
    (void)piet::dispatch("gen double gid = _n", session);
    (void)piet::dispatch("gen double mval = _n", session);
    // `force` is served since the built-in _merge plan landed (dc_mf_force rc 0),
    // and `nolabel` since merge carries value labels (StataBE uni-oracle/vm2.do;
    // T7 testMergeValueLabels): merge.ado has no option left that this build
    // names as unimplemented, so the arm checks that nolabel is served.
    std::string rejectMsg =
        firstError(piet::dispatch("merge 1:1 gid using \"" + usingPath + "\", nolabel", session));
    check(rejectMsg.empty(),
          "BUG-105 secondary ⑤: merge's `nolabel` is served (no reject). Got `" + rejectMsg + "`");

    piet::SessionState sessionTypo(workdir);
    (void)piet::dispatch("clear", sessionTypo);
    (void)piet::dispatch("set obs 2", sessionTypo);
    (void)piet::dispatch("gen double gid = _n", sessionTypo);
    (void)piet::dispatch("gen double mval = _n", sessionTypo);
    std::string typoMsg = firstError(piet::dispatch(
        "merge 1:1 gid using \"" + usingPath + "\", keepusign", sessionTypo));
    check(has(typoMsg, "option `keepusign` not allowed") &&
              !has(typoMsg, "valid Stata option"),
          "BUG-105 secondary ⑤: merge's `keepusign` (a typo, not a real Stata option) keeps "
          "the plain \"not allowed\" wording — distinct from the `force` message above");

    piet::SessionState session2(workdir);
    (void)piet::dispatch("clear", session2);
    (void)piet::dispatch("set obs 2", session2);
    (void)piet::dispatch("gen double gid = _n", session2);
    (void)piet::dispatch("gen double mval = _n", session2);
    std::string okMsg = firstError(
        piet::dispatch("merge 1:1 gid using \"" + usingPath + "\", nogenerate", session2));
    check(okMsg.empty(), "BUG-105 merge: known option `nogenerate` still works (no regression)");
  }

  // --- reshape: i()/j() known; a residual option token must be a named
  // reject.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 2", session);
    (void)piet::dispatch("gen double id = _n", session);
    (void)piet::dispatch("gen double inc1980 = _n * 10", session);
    (void)piet::dispatch("gen double inc1981 = _n * 20", session);
    std::string rejectMsg =
        firstError(piet::dispatch("reshape long inc, i(id) j(year) badopt", session));
    check(has(rejectMsg, "option `badopt` not allowed"),
          "BUG-105 reshape: unrecognized residual option is a named reject (was silent no-op)");

    piet::SessionState session2(workdir);
    (void)piet::dispatch("clear", session2);
    (void)piet::dispatch("set obs 2", session2);
    (void)piet::dispatch("gen double id = _n", session2);
    (void)piet::dispatch("gen double inc1980 = _n * 10", session2);
    (void)piet::dispatch("gen double inc1981 = _n * 20", session2);
    std::string okMsg = firstError(piet::dispatch("reshape long inc, i(id) j(year)", session2));
    check(okMsg.empty(), "BUG-105 reshape: known options i()/j() still work (no regression)");
  }

  // --- order: first/last/before()/after() known; a residual option token
  // must be a named reject.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 2", session);
    (void)piet::dispatch("gen double a = _n", session);
    (void)piet::dispatch("gen double b = _n * 2", session);
    std::string rejectMsg = firstError(piet::dispatch("order b, first badopt", session));
    check(has(rejectMsg, "option `badopt` not allowed"),
          "BUG-105 order: unrecognized residual option is a named reject (was silent no-op)");

    piet::SessionState session2(workdir);
    (void)piet::dispatch("clear", session2);
    (void)piet::dispatch("set obs 2", session2);
    (void)piet::dispatch("gen double a = _n", session2);
    (void)piet::dispatch("gen double b = _n * 2", session2);
    std::string okMsg = firstError(piet::dispatch("order b, first", session2));
    check(okMsg.empty(), "BUG-105 order: known option `first` still works (no regression)");
  }

  // --- B5-D3/D4 (BUG-475): `label define` redefining an EXISTING label
  // without add/modify/replace is r(110) and leaves the original mapping
  // intact; `, replace` moves from unimplemented to known and performs the
  // wholesale redefine. Measured B4REC|label_define|<redefine_no_replace>|110|
  // and B4REC|label_define|replace|0| (tests/eval/golden/aer_b4/
  // pins_oracle_r.log:52-66).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 1", session);
    (void)piet::dispatch("label define b5lbl 1 \"one\" 2 \"two\"", session);
    auto redefBlocks = piet::dispatch("label define b5lbl 1 \"ONE\" 3 \"three\"", session);
    std::string redefMsg = firstError(redefBlocks);
    check(has(redefMsg, "already defined"),
          "B5-D3 (BUG-475): redefining an existing label without add/modify/replace is an "
          "honest \"already defined\" reject");
    check(session.labelDefs.count("b5lbl") != 0 && session.labelDefs["b5lbl"].count("1") != 0 &&
              session.labelDefs["b5lbl"]["1"] == "one" &&
              session.labelDefs["b5lbl"].count("2") != 0 &&
              session.labelDefs["b5lbl"]["2"] == "two" && session.labelDefs["b5lbl"].count("3") == 0,
          "B5-D3 (BUG-475): the refusal left the ORIGINAL mapping intact (value survival, not "
          "just a rejected rc)");

    auto replaceBlocks =
        piet::dispatch("label define b5lbl 1 \"UNO\" 3 \"three\", replace", session);
    check(firstError(replaceBlocks).empty(),
          "B5-D4: `label define ..., replace` on an existing label is accepted (rc 0)");
    check(session.labelDefs.count("b5lbl") != 0 && session.labelDefs["b5lbl"].count("2") == 0 &&
              session.labelDefs["b5lbl"].count("1") != 0 &&
              session.labelDefs["b5lbl"]["1"] == "UNO" &&
              session.labelDefs["b5lbl"].count("3") != 0 &&
              session.labelDefs["b5lbl"]["3"] == "three",
          "B5-D4: `, replace` DISCARDS the old mapping wholesale (code 2 is gone) rather than "
          "merging like `, modify` would");
  }

  // --- B5-D14 (BUG-454): `restore` used to call NO reject net and read no
  // options at all, so `restore, preserve` was rc 0 and the option was
  // silently dropped (same "opts vanish silently" class this function is
  // about). Now: an EMPTY stack -> r(622) with `, preserve`; with a preserve
  // on the stack, `restore, preserve` restores AND re-arms it, so a SECOND
  // `restore` also succeeds. Measured B5REC|restore|preserve_nopreserve|622|,
  // B5REC|restore|preserve_after_preserve|0|, B5VAL|zz_exists_after|0|
  // (tests/eval/golden/aer_b4/b5_misc_pins.log:90-103).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 2", session);
    (void)piet::dispatch("gen double a = _n", session);

    (void)piet::dispatch("capture restore, preserve", session);
    check(session.eval.locals.get("_rc", "") == "622",
          "B5-D14 (BUG-454): `restore, preserve` with an EMPTY stack is r(622)");

    (void)piet::dispatch("preserve", session);
    check(session.preserveStack.size() == 1, "B5-D14: `preserve` armed the stack (depth 1)");
    (void)piet::dispatch("drop a", session);
    auto rearmBlocks = piet::dispatch("restore, preserve", session);
    check(firstError(rearmBlocks).empty(),
          "B5-D14: `restore, preserve` on an ARMED stack is accepted (rc 0)");
    check(session.preserveStack.size() == 1,
          "B5-D14: ...and RE-ARMS the stack (depth back to 1, not 0)");
    piet::Datastore* dsRe = session.datastore();
    check(dsRe != nullptr && dsRe->has("a"),
          "B5-D14: the restore genuinely brought column `a` back");
    auto genBlocks = piet::dispatch("gen zz = 1", session);
    check(firstError(genBlocks).empty(),
          "B5-D14: the restored frame is fully usable afterward (`gen zz` succeeds)");

    auto secondBlocks = piet::dispatch("restore", session);
    check(firstError(secondBlocks).empty(),
          "B5-D14: the re-armed preserve lets a SECOND `restore` succeed from the same "
          "snapshot (this is the bug BUG-454 named: the old code consumed it on the first "
          "restore)");
  }

  // --- B5-D-USE-REPLACE: `use FILE, replace` (92 corpus uses, this lane's
  // highest-weight item) was r(198) -- `replace` was not in kVocabUse at all.
  // The DISCRIMINATING half, and the whole reason this check does not stop at
  // rc 0: `, replace` DISCARDS unsaved changes exactly as `, clear` does, so a
  // variable `gen`erated AFTER the prior `use` must NOT survive a reload with
  // `, replace` -- a build that silently no-oped `use ..., replace` (kept the
  // in-memory frame, `junk` and all) would still pass an rc-only check.
  // Measured B5REC|use|replace|0| + B5REC|use|replace_over_modified|0| +
  // B5VAL|junk_survives_rc|111| (tests/eval/golden/aer_b4/b5_io_pins.log:
  // 37-61).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 2", session);
    (void)piet::dispatch("gen double a = _n", session);
    const std::string useReplacePath = workdir + "/b5_use_replace.dta";
    (void)piet::dispatch("save \"" + useReplacePath + "\", replace", session);

    auto replaceBlocks = piet::dispatch("use \"" + useReplacePath + "\", replace", session);
    check(firstError(replaceBlocks).empty(),
          "B5-D-USE-REPLACE: `use FILE, replace` on a freshly-saved file is accepted (rc 0), "
          "not r(198)");

    (void)piet::dispatch("gen junk = 1", session);
    piet::Datastore* dsWithJunk = session.datastore();
    check(dsWithJunk != nullptr && dsWithJunk->has("junk"),
          "B5-D-USE-REPLACE setup: `junk` really was created in the live frame");

    auto reloadBlocks = piet::dispatch("use \"" + useReplacePath + "\", replace", session);
    check(firstError(reloadBlocks).empty(),
          "B5-D-USE-REPLACE: `use FILE, replace` OVER a modified frame is still rc 0");
    (void)piet::dispatch("capture confirm variable junk", session);
    check(session.eval.locals.get("_rc", "") == "111",
          "B5-D-USE-REPLACE: ...and `junk` is GONE afterward -- `confirm variable junk` is "
          "r(111), matching B5VAL|junk_survives_rc|111 -- `, replace` discards, it does not "
          "merge or no-op");
  }

  fs::remove_all(workdir, ec);
}


// BUG-272 (closes BUG-200): the engine's sort-state plane, exercised through the
// wired cobra binary end to end — the product-shaped path PIET's own save/load
// plane rides. The one invariant under test: a carried "Sorted by:" claim is never
// FALSE. Positive legs: a sorted source .dta's claim survives import-dta (into the
// .hbin header's root "sorted_by" key) and a full .dta round trip; an ascending
// numeric gsort MINTS the claim; a descending gsort does not. Adversarial leg: a
// claim-breaking value write on the key column (setcol) must leave NO claim in the
// .hbin header and an all-zero <sortlist> after re-export — the stale-claim lie the
// sidecar-only design was rejected over (BUG-272's scope entry).
//
// ENGINE ARM: run with COBRA_BIN=<this tree>/build/cobra. A stale archive engine
// fails the header legs honestly (the key never appears) rather than skipping —
// [[게이트 엔진-팔 선언법]]: a silent pass against the wrong arm is the trap.
void testBUG272SortStatePlane() {
  std::printf("[selftest] --- BUG-272 sort-state plane (.dta <sortlist> round trip) ---\n");
  namespace fs = std::filesystem;

  // The sorted fixture (real Stata save of auto: srtlist = foreign), resolved off
  // __FILE__ exactly like testBUG202LevelCiFamily's fixturePath.
  std::string dta = std::string(selftestSourceFile());
  {
    auto pos = dta.rfind("/piet/native/src/selftest.cpp");
    dta = (pos == std::string::npos)
              ? std::string("tests/validate/dta_fixtures/auto_118.dta")
              : dta.substr(0, pos) + "/tests/validate/dta_fixtures/auto_118.dta";
    if (!std::ifstream(cecon::to_native_path(dta)).good()) {
      const char* vd = std::getenv("COBRA_VALIDATE_DIR");
      dta = std::string(vd ? std::string(vd) : selftestValidateDir()) +
            "/dta_fixtures/auto_118.dta";
    }
  }
  if (!std::ifstream(cecon::to_native_path(dta)).good()) {
    skip("BUG-272 (all legs)", "sorted .dta fixture not found at " + dta);
    return;
  }

  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug272").u8string();
  std::error_code ec;
  fs::remove_all(cecon::to_native_path(workdir), ec);
  fs::create_directories(cecon::to_native_path(workdir), ec);

  // Read an .hbin header's root "sorted_by" key. {value, key_present} — the
  // distinction matters: the byte discipline is ABSENT (not empty) for no-claim.
  auto hbinClaim = [](const std::string& path) -> std::pair<std::string, bool> {
    std::ifstream f(cecon::to_native_path(path), std::ios::binary);
    if (!f) return {"<unreadable>", false};
    char magic[8];
    uint64_t hlen = 0;
    f.read(magic, 8);
    f.read(reinterpret_cast<char*>(&hlen), 8);
    std::string h(static_cast<size_t>(hlen), '\0');
    f.read(&h[0], static_cast<std::streamsize>(hlen));
    if (!f) return {"<truncated>", false};
    auto j = nlohmann::json::parse(h, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return {"<malformed>", false};
    if (!j.contains("sorted_by")) return {std::string(), false};
    return {j["sorted_by"].is_string() ? j["sorted_by"].get<std::string>() : "<nonstring>", true};
  };
  auto run = [&](const std::vector<std::string>& args) {
    return piet::runCobra(args).exit_code == 0;
  };

  const std::string autoH = workdir + "/auto.hbin", rtD = workdir + "/rt.dta",
                    rtH = workdir + "/rt.hbin", csv = workdir + "/newkey.csv",
                    brokenH = workdir + "/broken.hbin", brokenD = workdir + "/broken.dta",
                    broken2H = workdir + "/broken2.hbin", sortH = workdir + "/sorted.hbin",
                    descH = workdir + "/desc.hbin";

  // Leg 1: import carries the source claim into the .hbin header.
  check(run({"import-dta", dta, autoH}), "BUG-272: import-dta of the sorted fixture succeeds");
  check(hbinClaim(autoH) == std::make_pair(std::string("foreign"), true),
        "BUG-272: source <sortlist> reaches the .hbin header (sorted_by=foreign) — a miss "
        "here with a passing import means a stale engine arm (declare COBRA_BIN=<tree>/build/cobra)");

  // Leg 2: the full .dta round trip preserves it (what real Stata `describe` reads).
  check(run({"export-dta", autoH, rtD}) && run({"import-dta", rtD, rtH}) &&
            hbinClaim(rtH) == std::make_pair(std::string("foreign"), true),
        "BUG-272: claim survives export-dta -> import-dta (the BUG-200 round trip)");

  // Leg 3 (adversarial): overwrite the KEY column's values, then look for the lie.
  {
    std::ofstream cf(cecon::to_native_path(csv), std::ios::binary);
    for (int i = 0; i < 74; ++i) cf << (i % 2) << "\n";  // alternating: NOT sorted
  }
  check(run({"setcol", "--data", autoH, "--var", "foreign", "--values", csv, "--dtype", "f64",
             "--out", brokenH}),
        "BUG-272: claim-breaking setcol succeeds");
  check(!hbinClaim(brokenH).second,
        "BUG-272: value write on the key column leaves NO sorted_by key (a stale claim "
        "would be a FALSE 'Sorted by:' in real Stata — the lie this plane exists to prevent)");
  check(run({"export-dta", brokenH, brokenD}) && run({"import-dta", brokenD, broken2H}) &&
            !hbinClaim(broken2H).second,
        "BUG-272: re-exported broken file carries an all-zero <sortlist> (blank, not stale)");

  // Leg 4: gsort mints ascending-numeric claims and refuses descending ones.
  check(run({"gsort", "--data", brokenH, "--vars", "price", "--out", sortH}) &&
            hbinClaim(sortH) == std::make_pair(std::string("price"), true),
        "BUG-272: ascending numeric gsort mints the claim");
  check(run({"gsort", "--data", brokenH, "--vars", "-price", "--out", descH}) &&
            !hbinClaim(descH).second,
        "BUG-272: descending gsort mints nothing (<sortlist> cannot express direction)");

  fs::remove_all(cecon::to_native_path(workdir), ec);
}
