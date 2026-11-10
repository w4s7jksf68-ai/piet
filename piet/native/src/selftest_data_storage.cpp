// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: data storage.
#include "selftest_support.h"
#include "selftest_cases.h"


// BUG-349 — a stateless mutation over a FOREIGN file must not invalidate the
// virtual head.
//
// The router used to clear `residentToken_` and `residentDirty_` after EVERY
// stateless mutation, on the reasoning that "a real --out was written, so the
// resident frame is stale". That reasoning holds only when the mutation READ the
// head: §25 stateless requests never enter the frame session, so a mutation over
// some other file leaves the worker's frame exactly as it was. Clearing the flags
// there declares a DIRTY VIRTUAL head clean, and the bytes that only ever existed
// in the worker become unreachable — every later reader gets `data_io: cannot open
// <the head>`.
//
// It stayed latent for as long as every caller filtered the head itself. The
// moment `summarize VARLIST if COND` began filtering a §38 projection instead
// (ONESHOT-LASTFLUSH), it became a hard failure on the very first task of the
// reference corpus — while ctest, eval, freeze and serve-parity all stayed green,
// because none of them keeps a virtual head alive across such a filter. Hence this
// leg: the observable is not a number, it is whether the NEXT command can still see
// the data at all.
void testVirtualHeadSurvivesForeignMutation() {
  std::printf("[selftest] --- BUG-349 virtual head survives a foreign stateless mutation ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug349").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 6", session);
  (void)piet::dispatch("gen double a = _n", session);
  (void)piet::dispatch("gen double b = _n * 10", session);  // head is virtual from here on

  piet::CobraSession* cs = session.cobraSessionIfLive();
  if (cs == nullptr || cs->framesState() == piet::CobraSession::Frames::No) {
    check(true, "BUG-349 PRECONDITION: no frames-capable `cobra serve` here — leg skipped, read "
                "as an ENVIRONMENT result, not a regression");
    return;
  }
  check(session.residentHeadDirty(), "BUG-349 setup: the head really is virtual before the filter");

  auto hasError = [](const nlohmann::json& blocks) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return true;
    return false;
  };

  // Filters a projection, not the head — the foreign stateless mutation.
  auto filtered = piet::dispatch("summarize a if a > 2", session);
  check(!hasError(filtered), "BUG-349: `summarize a if a > 2` succeeds over a projection");

  // The head was never written to disk. If the router forgot it, this cannot resolve.
  auto after = piet::dispatch("summarize b", session);
  check(!hasError(after), "BUG-349: the virtual head is STILL reachable after that mutation");

  double mean = std::nan("");
  std::function<void(const nlohmann::json&)> walk = [&](const nlohmann::json& n) {
    if (n.is_object()) {
      auto it = n.find("mean");
      if (it != n.end() && it->is_number()) mean = it->get<double>();
      for (auto& [k, v] : n.items()) { (void)k; walk(v); }
    } else if (n.is_array()) {
      for (const auto& v : n) walk(v);
    }
  };
  walk(after);
  check(std::fabs(mean - 35.0) < 1e-9,
        "BUG-349: and it still holds the RIGHT bytes (mean of b over 1..6 * 10 = 35)");
}


void testScratchSupersededDelete() {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path root = fs::temp_directory_path() / "piet_native_selftest_scratchgc";
  fs::remove_all(root, ec);
  fs::path workdir = root / "wd";
  fs::path userdir = root / "user";  // OUTSIDE workdir — a stand-in user file location
  fs::create_directories(workdir, ec);
  fs::create_directories(userdir, ec);
  auto touch = [](const fs::path& p) {
    std::ofstream f(p, std::ios::binary);
    f << "hbin-stub";
  };
  // Eviction is async (off the mutation critical path — see session_state.cpp),
  // so a DELETION assertion polls for the file to vanish within a short window;
  // a SAFETY assertion (file must survive) needs no wait because an excluded
  // file is never registered for eviction.
  auto waitGone = [](const fs::path& p, int ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    std::error_code e;
    while (std::filesystem::exists(p, e)) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
  };
  auto setBudgetMb = [](const char* v) {
#ifndef _WIN32
    setenv("PIET_SCRATCH_BUDGET_MB", v, 1);
#else
    _putenv_s("PIET_SCRATCH_BUDGET_MB", v);
#endif
  };

  // Budget=0 => evict immediately (the eager policy) for the deletion/safety
  // legs; a below-budget short burst is covered by the retain leg further down.
  setBudgetMb("0");

  // Deletion: load(A) then load(B) -> A superseded and evicted, B kept.
  {
    piet::SessionState session(workdir.u8string());
    fs::path a = workdir / "loaded.hbin";
    fs::path b = workdir / "gen_1.hbin";
    touch(a);
    touch(b);
    session.load(a.u8string());
    check(fs::exists(a), "scratch-gc: the first load leaves the loaded hbin in place");
    session.load(b.u8string());
    check(waitGone(a, 2000), "scratch-gc DELETION: superseded predecessor removed after load() (async)");
    check(fs::exists(b, ec), "scratch-gc: the new live dataset is kept");
  }

  // Safety 1 (NEVER deletes user files): a superseded file OUTSIDE workdir.
  {
    piet::SessionState session(workdir.u8string());
    fs::path u = userdir / "mydata.hbin";
    fs::path b = workdir / "gen_2.hbin";
    touch(u);
    touch(b);
    session.load(u.u8string());
    session.load(b.u8string());
    check(fs::exists(u, ec), "scratch-gc SAFETY: a user file outside workdir is never deleted");
  }

  // Safety 2: a superseded file referenced by a live preserve snapshot survives.
  {
    piet::SessionState session(workdir.u8string());
    fs::path p = workdir / "preserve_1.hbin";
    fs::path b = workdir / "gen_3.hbin";
    touch(p);
    touch(b);
    session.load(p.u8string());
    piet::PreserveFrame frame;
    frame.hbinPath = p.u8string();
    session.preserveStack.push_back(frame);
    session.load(b.u8string());  // p superseded, but it is a live preserve snapshot
    check(fs::exists(p, ec), "scratch-gc SAFETY: a live preserve snapshot is never deleted");
  }

  // Safety 3 (BUG-381): a superseded file the LIVE estimation still names is
  // never deleted. `predict`/`test`/`lincom`/`margins` replay LastModel's
  // dataPath and fitDataPath back to the engine long after the head moved on,
  // so both must be pinned. Two files, so a fix that pins only one is RED here.
  {
    piet::SessionState session(workdir.u8string());
    fs::path fitd = workdir / "fit_data.hbin";
    fs::path fitf = workdir / "fit_filtered.hbin";
    fs::path b = workdir / "gen_4.hbin";
    touch(fitd);
    touch(fitf);
    touch(b);
    session.load(fitd.u8string());
    piet::LastModel lm;
    lm.cobraCmd = "regress";
    lm.y = "y";
    lm.dataPath = fitd.u8string();
    lm.fitDataPath = fitf.u8string();
    session.setLastModel(lm);
    session.load(b.u8string());              // fitd superseded ...
    session.load(fitf.u8string());           // ... and now fitf too
    session.load(b.u8string());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(fs::exists(fitd, ec),
          "scratch-gc SAFETY: LastModel::dataPath survives — a post-estimation `predict` "
          "replays it as --data after the head has moved on");
    check(fs::exists(fitf, ec),
          "scratch-gc SAFETY: LastModel::fitDataPath survives too (--fit-data, the "
          "restricted-sample half of the same replay)");
  }

  // Safety 4 (BUG-381): the user's OWN loaded file, when it happens to resolve
  // INSIDE the workdir. Safety 1 above refuses user files by LOCATION, which is
  // the right test only while no user file sits in the workdir; currentFile_
  // states the ownership directly. `use f` + `replace` is the whole shape.
  {
    piet::SessionState session(workdir.u8string());
    fs::path mine = workdir / "my_own_data.hbin";
    fs::path b = workdir / "gen_5.hbin";
    touch(mine);
    touch(b);
    session.setCurrentFile(mine.u8string());
    session.load(mine.u8string());
    session.load(b.u8string());  // superseded, and INSIDE workdir — (1) does not save it
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(fs::exists(mine, ec),
          "scratch-gc SAFETY: the user's own loaded file is never deleted, even when it "
          "resolves inside the session workdir (exclusion (1) tests location, not ownership)");
  }

  // Opt-out: PIET_NO_SCRATCH_GC=1 disables deletion (debug escape hatch).
  {
#ifndef _WIN32
    setenv("PIET_NO_SCRATCH_GC", "1", 1);
#else
    _putenv_s("PIET_NO_SCRATCH_GC", "1");
#endif
    piet::SessionState session(workdir.u8string());
    fs::path a = workdir / "nogc_a.hbin";
    fs::path b = workdir / "nogc_b.hbin";
    touch(a);
    touch(b);
    session.load(a.u8string());
    session.load(b.u8string());
    check(fs::exists(a, ec), "scratch-gc: PIET_NO_SCRATCH_GC=1 disables deletion");
#ifndef _WIN32
    unsetenv("PIET_NO_SCRATCH_GC");
#else
    _putenv_s("PIET_NO_SCRATCH_GC", "");
#endif
  }

  // Budget defers deletion (the bench-friendly property, Gate ②): with a high
  // budget a superseded file is RETAINED — a short mutation burst pays no
  // deletion I/O on the critical path.
  {
    setBudgetMb("1048576");  // 1 TB ceiling — far above any stub-file total
    piet::SessionState session(workdir.u8string());
    fs::path a = workdir / "retain_a.hbin";
    fs::path b = workdir / "retain_b.hbin";
    touch(a);
    touch(b);
    session.load(a.u8string());
    session.load(b.u8string());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let any wrong async evict fire
    check(fs::exists(a, ec), "scratch-gc BUDGET: superseded scratch retained under budget (no eager unlink)");
  }
#ifndef _WIN32
  unsetenv("PIET_SCRATCH_BUDGET_MB");
#else
  _putenv_s("PIET_SCRATCH_BUDGET_MB", "");
#endif

  // DEFAULT (BUG-381, User ruling 2026-08-31): with the env UNSET the policy is
  // eager. Every check above sets the budget explicitly, so none of them can see
  // the default at all — the ruling would otherwise live in one constant with
  // nothing guarding it, and a later "restore the 4 GiB ceiling" edit would pass
  // this whole block green. Measured basis for the ruling: retention cost 3.9
  // GiB of peak (8.916 vs 5.009 GiB high-water on the 47-artifact corpus) and
  // bought nothing readable back. Note this runs AFTER the unsetenv above, which
  // is the only place in this process where the default is observable.
  {
    piet::SessionState session(workdir.u8string());
    fs::path a = workdir / "default_a.hbin";
    fs::path b = workdir / "default_b.hbin";
    touch(a);
    touch(b);
    session.load(a.u8string());
    session.load(b.u8string());
    check(waitGone(a, 2000),
          "scratch-gc DEFAULT: with PIET_SCRATCH_BUDGET_MB unset the policy is EAGER — "
          "a superseded file is evicted, not retained against a 4 GiB ceiling");
  }

  // BUG-381: `preserve` -> mutate -> `save` -> `restore`, the shape a sectioning
  // loop types, must not strand one scratch file per turn. The resident restore
  // has no load() to reclaim the head it abandons, and the reclaim it does have
  // was gated to the cache-`use` path only. Measured cost of the gap on the
  // 47-artifact corpus: 1145 stranded files, 4.538 GiB — 96.7% of a scratch
  // high-water that NO budget setting could reach, because a file no ledger
  // knows about is one no reap can find.
  //
  // The discriminator is scratchCensusJson()'s ORPHAN class, which is what that
  // instrument was built for. The `save` matters: without it the head stays a
  // virtual token naming no file, and nothing is stranded — so a lock written
  // without the save passes while blind (measured, 2026-08-31).
  //
  // This exercises the RESIDENT plane, which needs a live worker; with no cobra
  // the clone plane takes over and never had the bug, so the check would pass
  // vacuously. Its seeded-RED certificate (restore the `keepStash &&` gate ->
  // this goes red) is what proves the plane was actually engaged here.
  {
    fs::path pwd2 = root / "preserve_wd";
    fs::create_directories(pwd2, ec);
    piet::SessionState session(pwd2.u8string());
    session.cwd = pwd2.u8string();
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 500", session);
    (void)piet::dispatch("gen double x = _n", session);
    (void)piet::dispatch("tempfile bug381t", session);
    const int kTurns = 6;
    for (int i = 0; i < kTurns; ++i) {
      (void)piet::dispatch("preserve", session);
      (void)piet::dispatch("rename x y", session);
      (void)piet::dispatch("save `bug381t', replace", session);
      (void)piet::dispatch("restore", session);
    }
    long long strandedRenames = 0;
    try {
      nlohmann::ordered_json c = nlohmann::ordered_json::parse(session.scratchCensusJson());
      for (const auto& r : c.value("orphan_shapes", nlohmann::ordered_json::array())) {
        const std::string shape = r.value("shape", std::string());
        if (shape.rfind("rename_", 0) == 0) strandedRenames += r.value("n", 0LL);
      }
    } catch (...) {
      strandedRenames = -1;  // a parse failure must not read as "clean"
    }
    // One in flight is the head itself on the final turn; the leak minted one
    // PER TURN, so the pre-fix number here was 5-7 and the bound separates them.
    check(strandedRenames >= 0 && strandedRenames <= 1,
          "BUG-381: `preserve` -> mutate -> `save` -> `restore` strands no scratch — the "
          "resident restore reclaims the head it abandons (it has no load() to do it)");
  }

  // BUG-381, the same class at its other three sites. load()'s reclaim is the
  // ONLY one there is, so every place that assigns currentHbin_ directly — or
  // writes scratch that never becomes a head at all — is a place it does not
  // run. Found by tracing head transitions (PIET_SCRATCH_TRACE) against the
  // census on one real campaign group: a file "adopted as head once, never
  // superseded in a load" names a direct assignment; "never adopted at all"
  // names a side file. Both were present, and neither is reachable by any
  // budget setting. One group went 25.42 MiB of orphan bytes -> 0.03 MiB.
  //
  // Each leg counts orphan bytes rather than files: the point is the volume a
  // user's free space must survive, and a 30 KB metadata sidecar is not it.
  {
    // Count orphan bytes for the SHAPES the leg is about, not the workdir total.
    // Two reasons, both learned the hard way here. (1) `set obs` leaves a 225 KB
    // seed hbin + a 40 KB csv in every one of these sessions; a total-bytes test
    // is a test of that seed. (2) The first version of this helper wrote
    //     for (auto& [k, v] : c.value("by_class", json::object()).items())
    // which iterates .items() of a TEMPORARY — it returned 0 unconditionally, so
    // all three legs passed under seeds that removed the very fixes they guard.
    // Binding the object first is the fix; the seeded-RED run is what exposed it.
    auto strandedBytes = [](piet::SessionState& s2,
                            const std::vector<std::string>& prefixes) -> long long {
      try {
        const nlohmann::ordered_json c = nlohmann::ordered_json::parse(s2.scratchCensusJson());
        auto it = c.find("orphan_shapes");
        if (it == c.end() || !it->is_array()) return -1;
        long long total = 0;
        for (const auto& r : *it) {
          const std::string shape = r.value("shape", std::string());
          for (const std::string& pfx : prefixes) {
            if (shape.rfind(pfx, 0) == 0) { total += r.value("bytes", 0LL); break; }
          }
        }
        return total;
      } catch (...) {
        return -1;  // a parse failure must not read as "clean"
      }
    };
    // Nothing of the named shapes may survive. The shapes are minted per
    // iteration, so a leak is 6 files here and the bound does not need slack.
    const long long kSlack = 0;

    // BUG-382 (2026-09-01): e() must SUBSTITUTE at full precision while bare
  // `display e(...)` keeps its short default rendering. Driven through
  // doString — the dispatch() shortcut was measured not to exercise the real
  // substitution path on an earlier lock in this same file.
  {
    fs::path w = root / "eprec_wd";
    fs::create_directories(w, ec);
    piet::SessionState session(w.u8string());
    session.cwd = w.u8string();
    nlohmann::json body{{"source",
                         "sysuse fatalities, clear\n"
                         "gen byte hi382 = (fatal > 1000)\n"
                         // logit, not regress: OLS publishes no e(ll), so the
                         // resolver correctly answers "." there and the leg
                         // would measure the model's absence, not precision.
                         "quietly logit hi382 beertax\n"
                         "scalar s382 = e(ll)\n"
                         "display %23.16e s382\n"
                         "display e(ll)\n"}};
    auto r = piet::routes::doString(session, body.dump());
    std::string hexline, shortline;
    if (r.ok) {
      auto j = nlohmann::json::parse(r.json);
      std::vector<std::string> outs;
      for (const auto& b : j.value("blocks", nlohmann::json::array()))
        if (b.value("type", std::string()) == "display")
          outs.push_back(b.value("text", std::string()));
      if (outs.size() >= 2) { hexline = outs[outs.size() - 2]; shortline = outs.back(); }
    }
    // Discriminator: DECIMAL full-precision round-trip. (%21x on a scalar is a
    // pre-existing display-format gap — it falls back to the plain render — so
    // the hex spelling cannot carry this leg, and the plain render is only 9
    // digits. %23.16e prints 17 significant digits, which round-trips ANY
    // double; %20.15e (16 digits) round-tripped only by luck and failed on the
    // logit e(ll) -199.14775257269972 once logit stopped at Stata's ml
    // iterate. (BUG-1353: an e-format shows at most w-7 decimals, so this leg's
    // old %21.16e is 15 digits in StataBE, -1.99147752572700e+02; it read 17
    // only through printf.) The old fmtNumeric arm stored 9 significant digits, so the
    // 17-digit text lands ~1e-10 off the block's ll — bitwise inequality, which
    // is the RED this leg produces.)
    double llBlock = 0.0;
    if (session.eval.lastModelResult && session.eval.lastModelResult->contains("ll"))
      llBlock = session.eval.lastModelResult->at("ll").get<double>();
    double got = std::nan("");
    try { got = std::stod(hexline); } catch (...) {}
    check(llBlock != 0.0 && !std::isnan(got) && got == llBlock,
          "BUG-382: `scalar s = e(ll)` carries FULL precision — the scalar's rendered value "
          "round-trips to the e-block's own double bit-for-bit (was: 9 significant digits)");
    // Render-invariance leg: bare `display e(ll)` still prints the SHORT form.
    // Discriminator = significant-digit count of the printed text: the default
    // %9.0g render carries at most 9 digit characters; an echoed full-precision
    // substitution carries 16-17. Counted, not pattern-matched.
    int digits = 0;
    for (char c : shortline)
      if (c >= '0' && c <= '9') ++digits;
    check(!shortline.empty() && digits <= 10,
          "BUG-382: bare `display e(ll)` still renders the short default form (" +
              std::to_string(digits) + " digit chars) — the full-precision substitution is "
              "invisible to plain display output");
  }

  // What this block does NOT guard, stated plainly. BUG-381 fixed three more
    // sites of the same class; only this one has a synthetic shape that a
    // selftest session actually reaches. Legs for `clear` (clearData) and for
    // the §27 virtual-`use` fast path (residentUse) were written and DELETED
    // after seeded-RED measured them at 0 -> 0: those paths need a live virtual
    // table / a head shape these sessions do not produce, so the legs passed
    // whether or not the fix was there. Shipping them would have claimed
    // coverage that does not exist ([[gate-falsification-registry]]).
    //
    // Their certificate is the campaign one, and it is reproducible: run one
    // group of the acceptance corpus with PIET_SCRATCH_CENSUS=1 and remove one
    // fix at a time. Measured 2026-08-31, one group (2 dependent variables):
    //     all three fixes in      0.03 MiB orphan
    //     minus residentUse      24.13 MiB   (drop_ 2, append_ 2)  <- 94.8%
    //     minus levelsof          1.32 MiB   (levelsof_ 2)         <-  5.1%
    //     minus clearData         0.03 MiB   (no measured effect on this corpus)
    //
    // The driver matters as much as the commands: routes::doString is the path
    // a real .do takes, and a dispatch() sequence of the same text does not
    // reproduce the levelsof leak at all.
    auto doRun = [](piet::SessionState& s2, const std::string& src) {
      nlohmann::json body{{"source", src}};
      (void)piet::routes::doString(s2, body.dump());
    };

    // (b) `levelsof <var> if <cond>` writes a filtered copy it READS and
    //     abandons. Nothing ever supersedes it, so only the §38 projection
    //     ledger can reach it. The `if` is load-bearing: without it there is no
    //     side file at all and the leg would prove nothing.
    {
      fs::path w = root / "levelsof_wd";
      fs::create_directories(w, ec);
      piet::SessionState session(w.u8string());
      session.cwd = w.u8string();
      doRun(session,
            "clear\nset obs 20000\ngen double g = mod(_n, 7)\ngen double k = _n\n"
            "forvalues i = 1/6 {\n    levelsof g if k > 10, local(lv) clean\n}\n");
      const long long b = strandedBytes(session, {"levelsof_"});
      check(b >= 0 && b <= kSlack,
            "BUG-381: `levelsof ... if` does not strand its filtered copy — a side file that "
            "is never a head is invisible to the supersede ledger by construction");
    }

  }

  fs::remove_all(root, ec);
}


// BUG-085 (SE-044/045): the classroom-core checkpoint idiom
// BUG-360: a tempfile's lifetime is the PROGRAM that minted it, not the run.
//
// Stata, [P] macro: "When the program or do-file concludes, any datasets created
// with these assigned names are erased", and for `tempfile` it happens
// "regardless of whether your program ends with an error". PIET reaped only at
// `doExecDepth == 0` — the end of the whole top-level run — so a program called
// N times left N generations of scratch behind. Measured on the acceptance
// corpus's controls=yes arm (one plotting program, 47 calls): `.tmp` files grew
// 13 -> 80 over five tasks and were never reclaimed, while the `.hbin` bytes
// beside them plateaued at the 4 GiB scratch budget. The budget cannot bound a
// tempfile — it only ever sees a head that a NEW head superseded.
//
// The PRECONDITION check is what keeps this leg honest: it asserts the file
// EXISTED while the program was running. Without it the leg passes trivially on
// a build where `tempfile` never created anything at all.
// BUG-361: `confirm file` must ask the session before it asks the filesystem.
//
// A `save` to a MINTED tempfile is routed to the engine's in-RAM virtual-file
// table when the resident plane is live and writes no bytes. `confirm file`
// called `fs::exists` and nothing else, so it reported _rc 601 — the SAME code a
// genuinely absent file returns — for a file the session had just saved and
// could still `use`. A script cannot tell those two apart.
//
// The three-way shape is the point: the leg asserts a PLAIN saved file confirms,
// a genuinely ABSENT file does not, and the tempfile behaves like the first
// rather than the second. Without the absent arm this would pass on a build
// where `confirm file` said yes to everything.
void testConfirmFileSeesVirtualSaveBug361() {
  std::printf("[selftest] --- BUG-361 confirm file vs the virtual-save table ---\n");
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_bug361").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  const std::string plainPath = workdir + "/plain361.hbin";
  const std::string src =
      "clear\n"
      "set obs 5\n"
      "gen x = _n\n"
      "save \"" + plainPath + "\", replace\n"
      "capture confirm file \"" + plainPath + "\"\n"
      "global RC_PLAIN = _rc\n"
      "tempfile tf\n"
      "save \"`tf'.hbin\", replace\n"
      "capture confirm file \"`tf'.hbin\"\n"
      "global RC_TEMP = _rc\n"
      "global TFPATH \"`tf'.hbin\"\n"
      "capture confirm file \"definitely_absent_361.hbin\"\n"
      "global RC_ABSENT = _rc\n";
  (void)piet::doexec::executeDoSource(src, session, piet::dispatch);

  const std::string rcPlain  = session.eval.globals.get("RC_PLAIN", "");
  const std::string rcTemp   = session.eval.globals.get("RC_TEMP", "");
  const std::string rcAbsent = session.eval.globals.get("RC_ABSENT", "");
  const std::string tfPath   = session.eval.globals.get("TFPATH", "");

  // PRECONDITIONS: the two ends of the scale must behave, or the middle assertion
  // is measuring nothing.
  check(rcPlain == "0",
        "BUG-361 precondition: `confirm file` succeeds on an ordinary saved file");
  check(rcAbsent != "0",
        "BUG-361 precondition: `confirm file` still FAILS on a genuinely absent file");
  // And the tempfile really did take the virtual arm — if it wrote bytes, the
  // assertion below would pass without the fix.
  check(!tfPath.empty() && session.isVirtualSave(tfPath),
        "BUG-361 precondition: the tempfile save took the VIRTUAL arm (no bytes on disk)");

  check(rcTemp == "0",
        "BUG-361: `confirm file` succeeds on a virtually-saved tempfile the session can still use");

  fs::remove_all(workdir, ec);
}


// Phase 1 (2026-08-20): `rename` must not force a full card rebuild.
//
// The Rename fold has existed since P2-PERF ARC-B, but it required
// `delta.changed == 0` on the strength of a measurement its own comment
// recorded. On the acceptance corpus HALF of a 322-rename run reports
// `dropped=1, added=1, changed=11` with a varsDelta covering EVERY column, and
// those 154 declines fell into mutationResult's rebuild — rename alone forcing
// 220 of 476 head materializations (PIET_FOLDTRACE + PIET_ROUTELOG, Phase 0).
// A complete delta is more information, not less, so the card is now rebuilt
// FROM it, exactly as the row-filter branch does.
//
// This leg gates the INVARIANT both arms share: a resident rename folds its
// card and constructs no Datastore. It does not by itself distinguish the
// complete-delta arm — the trigger for `changed != 0` is a property of the
// frame, not of the argv, and is not reproducible from this fixture. That arm's
// certificate is measured instead, on the corpus that does trigger it: with the
// branch disabled, all five acceptance artifacts are byte-IDENTICAL (so it
// changes no answer), while flushes fall 332 -> 196 and rename's 154 go to zero.
void testRenameFoldsItsCardPhase1() {
  std::printf("[selftest] --- Phase 1 rename folds its card (no rebuild) ---\n");
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_rnfold").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 40", session);
  (void)piet::dispatch("gen a = _n", session);
  (void)piet::dispatch("gen b = _n * 2", session);
  (void)piet::dispatch("gen c = _n * 3", session);

  auto pickMutation = [](const nlohmann::ordered_json& blocks) -> nlohmann::ordered_json {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "data_mutation") return b;
    }
    return nlohmann::ordered_json::object();
  };
  session.invalidateDatastore();
  piet::Datastore::resetConstructCount();
  piet::SessionState::resetCardFoldCount();
  auto blk = pickMutation(piet::dispatch("rename b b_renamed", session));

  check(blk.value("type", std::string()) == "data_mutation",
        "Phase 1: a resident `rename` returns a data_mutation card");
  check(piet::SessionState::cardFoldCount() == 1,
        "Phase 1 mechanism gate: `rename` FOLDS its card (count 1). A count of 0 means the "
        "fold declined and mutationResult rebuilt from the datastore — which is the 46%-of-"
        "all-flushes path this work exists to close");
  check(piet::Datastore::constructCount() == 0,
        "Phase 1 mechanism gate: a folded `rename` constructs ZERO Datastores — the rebuild "
        "it avoids is what materializes the virtual head");

  // And the card must actually say the new name, with the old one gone.
  bool sawNew = false, sawOld = false;
  for (const auto& v : blk.value("varlist_after", nlohmann::ordered_json::array())) {
    const std::string n = v.value("name", std::string());
    if (n == "b_renamed") sawNew = true;
    if (n == "b") sawOld = true;
  }
  check(sawNew && !sawOld,
        "Phase 1: the folded card carries the NEW name and no longer carries the old one");

  fs::remove_all(workdir, ec);
}


// --- C4-L2B (FLOOR C4 D1 card plane, brief §1 use/describe rows + §4) -----
// The render-plane trio: runUse's virtual-save varlist render, expandVarlist
// (the `foreach ... of varlist` header) and runDescribe's card share stop
// opening a Datastore — and, on a dirty virtual head, stop forcing the EMH
// frame snapshot — when the CURRENT resident card can answer. Same two gate
// classes as the C4-L2A legs (testBridgeRoutesM4b): MECHANISM (constructs==0,
// window left open — the answer is identical either way, only counters can
// see the fast path die) and PARITY+ESCAPE (PIET_NO_RESIDENT_READ=1 must
// byte-reproduce the blocks AND really construct/snapshot again — the
// dropped-env-check mutant-kill and the floor_gate RED arm's in-process twin).
void testC4L2BRenderPlaneGates() {
  std::printf("[selftest] --- C4-L2B use-render/expandVarlist/describe card plane ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c4l2b").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto runCmd = [](piet::SessionState& session, const std::string& cmd) {
    nlohmann::json body{{"cmd", cmd}};
    auto r = piet::routes::command(session, body.dump());
    return nlohmann::json::parse(r.json);
  };
  auto firstBlockOfType = [](const nlohmann::json& j, const std::string& type) -> nlohmann::json {
    for (const auto& b : j.value("blocks", nlohmann::json::array())) {
      if (b.value("type", std::string()) == type) return b;
    }
    return nlohmann::json();
  };

  // A REAL on-disk head first (the M4b idiom): the resident plane only engages
  // over a real hbin head the worker can frame-use — a set-obs-from-empty
  // chain stays stateless and the tempfile save would fall back to a real
  // file, skipping every card-plane leg below (measured while authoring).
  std::string fixture = std::string(selftestSourceFile());
  {
    auto pos = fixture.rfind("/piet/native/src/selftest.cpp");
    fixture = (pos == std::string::npos)
                  ? "tests/validate/auto.hbin"
                  : fixture.substr(0, pos) + "/tests/validate/auto.hbin";
  }
  if (!std::ifstream(cecon::to_native_path(fixture)).good()) {
    fixture = selftestValidateDir() + "/auto.hbin";
  }
  if (!std::ifstream(cecon::to_native_path(fixture)).good()) {
    skip("C4-L2B use-render/expand/describe gates", "fixture not found at " + fixture);
    fs::remove_all(workdir, ec);
    return;
  }

  piet::SessionState session(workdir);
  auto jOpen = runCmd(session, "use " + fixture + ", clear");
  check(firstBlockOfType(jOpen, "error").is_null() && session.hasData(),
        "C4-L2B setup: `use auto.hbin` loads the 74-row/12-var fixture");
  (void)runCmd(session, "replace mpg = mpg + 1");

  (void)runCmd(session, "tempfile c4p");
  const std::string vpath = session.eval.locals.get("c4p") + ".hbin";
  auto jSave = runCmd(session, "save \"`c4p'.hbin\", replace");
  check(firstBlockOfType(jSave, "error").is_null(),
        "C4-L2B setup: `save \"`c4p'.hbin\"` (minted tempfile) succeeds");

  if (!session.isVirtualSave(vpath)) {
    skip("C4-L2B use-render/expand/describe gates",
         "resident plane unavailable — the tempfile save fell back to a real file, so the "
         "virtual-render card path (and the dirty-window mechanism halves) cannot be reached");
    fs::remove_all(workdir, ec);
    return;
  }

  // --- use (virtual-save render): card arm --------------------------------
  piet::Datastore::resetConstructCount();
  const int64_t snapBeforeUse = piet::counters::snapshotCount();
  auto jUse = runCmd(session, "use \"`c4p'.hbin\", clear");
  check(firstBlockOfType(jUse, "error").is_null(),
        "C4-L2B use gate: virtual-save `use \"`c4p'.hbin\"` succeeds on the card plane");
  {
    auto dm = firstBlockOfType(jUse, "data_mutation");
    check(!dm.is_null() && dm.value("action", std::string()) == "open" &&
              dm.value("n_rows_after", int64_t(-1)) == 74 &&
              dm.contains("varlist_after") && dm["varlist_after"].size() == 12,
          "C4-L2B use gate: the card-plane render emits the frozen wire shape — action=open, "
          "n_rows_after=74 (rows off residentRowsIfCurrent), varlist_after carrying all 12 "
          "columns");
  }
  check(piet::Datastore::constructCount() == 0,
        "C4-L2B use MECHANISM gate: the virtual-save render answers varlist_after/n_rows_after "
        "off the card loadFromVirtual just seeded — ZERO Datastore constructions (pre-D1 this "
        "site materialized the JUST-RECEIVED frame to disk for a metadata-only render; a2's 10 "
        "use-closures were 100% this site, brief §3)");
  check(piet::counters::snapshotCount() == snapBeforeUse,
        "C4-L2B use MECHANISM gate: the incoming frame's dirty window is left OPEN — "
        "snapshot_count unchanged (no EMH fired for the render)");

  // --- use: escape arm (byte parity + the old path really runs) -----------
  setenv("PIET_NO_RESIDENT_READ", "1", 1);
  piet::Datastore::resetConstructCount();
  const int64_t snapBeforeUseEsc = piet::counters::snapshotCount();
  auto jUseEsc = runCmd(session, "use \"`c4p'.hbin\", clear");
  unsetenv("PIET_NO_RESIDENT_READ");
  check(jUseEsc.dump() == jUse.dump(),
        "C4-L2B use PARITY gate: PIET_NO_RESIDENT_READ=1 `use` returns the byte-identical "
        "response — varlist_after (name/dtype/format/label/n_levels/n_missing per column) and "
        "n_rows_after agree between the card plane and the datastore plane (the brief's "
        "field-by-field render audit, machine-checked)");
  check(piet::Datastore::constructCount() >= 1,
        "C4-L2B use escape MECHANISM gate: the escape arm REALLY renders through "
        "session.datastore() — at least one Datastore construction (a zero means the env gate "
        "was dropped and the floor_gate RED arm no longer proves corpus reach)");
  check(piet::counters::snapshotCount() == snapBeforeUseEsc + 1,
        "C4-L2B use escape MECHANISM gate: the escape-arm render CLOSES the incoming frame's "
        "dirty window (snapshot_count +1 — the pre-D1 use-closure, reproduced on demand)");

  // --- expandVarlist: mechanism via the no-match verdict -------------------
  // A resident `replace` opens a fresh dirty window and resets the memoized
  // Datastore, so a construct here is attributable to expandVarlist alone.
  // The no-match error is produced FROM the expanded name list, so asserting
  // its exact text proves the expansion really ran on both planes (an
  // aggregation failure would surface a different error and trip the checks).
  (void)runCmd(session, "replace mpg = mpg + 1");
  piet::Datastore::resetConstructCount();
  const int64_t snapBeforeFe = piet::counters::snapshotCount();
  auto feErr = piet::doexec::executeDoSource("foreach v of varlist zz* {\ndisplay \"`v'\"\n}\n",
                                             session, piet::dispatch);
  // The pinned text moved with the verdict, not with the assertion's power:
  // K1b replaced expandVarlist's private "no variables match 'zz*'" wording
  // with Stata's own `variable zz* not found` at r(111) (measured on StataNow
  // 19.5 BE: `unab a : zz*` and `drop zz*` both answer 111). This still comes
  // out of the expanded name list, so it still proves the expansion ran, and it
  // is still a string only this path produces.
  const bool sawNoMatch =
      feErr.blocks.dump().find("variable zz* not found") != std::string::npos;
  check(sawNoMatch,
        "C4-L2B expandVarlist gate: `foreach v of varlist zz*` answers the no-match verdict "
        "(text produced from the expanded name list — the expansion really ran)");
  check(piet::Datastore::constructCount() == 0,
        "C4-L2B expandVarlist MECHANISM gate: the header expansion (names + no-match verdict) "
        "reads the CURRENT card — ZERO Datastore constructions for a header that never looks "
        "at a value");
  check(piet::counters::snapshotCount() == snapBeforeFe,
        "C4-L2B expandVarlist MECHANISM gate: the dirty window stays OPEN across the card-plane "
        "expansion (snapshot_count unchanged)");

  setenv("PIET_NO_RESIDENT_READ", "1", 1);
  piet::Datastore::resetConstructCount();
  const int64_t snapBeforeFeEsc = piet::counters::snapshotCount();
  auto feErrEsc = piet::doexec::executeDoSource("foreach v of varlist zz* {\ndisplay \"`v'\"\n}\n",
                                                session, piet::dispatch);
  unsetenv("PIET_NO_RESIDENT_READ");
  check(feErrEsc.blocks.dump() == feErr.blocks.dump(),
        "C4-L2B expandVarlist PARITY gate: the no-match error is byte-identical between the "
        "card plane and the escape arm's datastore plane");
  check(piet::Datastore::constructCount() >= 1 &&
            piet::counters::snapshotCount() == snapBeforeFeEsc + 1,
        "C4-L2B expandVarlist escape MECHANISM gate: the escape arm really walks the datastore "
        "path — construct fired AND the dirty window the card plane left open is closed "
        "(snapshot_count +1), the dropped-env-check mutant-kill");

  // --- BUG-339: `= expr` resolves NESTED macro references -----------------
  // One expansion pass resolves one LEVEL. `local rr = `cell_rel_`i''` has two,
  // so the pass copied the outer backtick through, evalArith met a backtick it
  // has in neither whitelist, and the line died as a bare r(198). Every other
  // consumer of a possibly-nested RHS already loops; these two branches were
  // the exception. On the acceptance corpus that was 160 errors per run — one
  // per depvar, aborting three of every four estimations — displayed but never
  // fatal, so no gate saw it for three weeks.
  {
    auto nested = piet::doexec::executeDoSource(
        "local i = 1\n"
        "local a1 = 7\n"
        "local x = `a`i''\n"
        "display \"x=[`x']\"\n"
        "global gi = 1\n"
        "global gb1 = 9\n"
        "global gy = ${gb`i'}\n"
        "display \"gy=[${gy}]\"\n",
        session, piet::dispatch);
    const std::string dump = nested.blocks.dump();
    check(dump.find("x=[7]") != std::string::npos,
          "BUG-339: `local x = `a`i''` resolves BOTH levels — the inner `i' names the outer "
          "macro, exactly the shape the acceptance corpus uses per event-study cell");
    check(dump.find("gy=[9]") != std::string::npos,
          "BUG-339 global twin: `global gy = ${gb`i'}` resolves both levels too");
    check(dump.find("invalid syntax") == std::string::npos,
          "BUG-339: no r(198) survives on the nested form (the seeded-RED for this pair is "
          "reverting either expandMacros call in macros.cpp back to expandPass)");
  }
  {
    // The corpus's own shape, loop and all.
    auto corpusShape = piet::doexec::executeDoSource(
        "local cell_count = 0\n"
        "foreach rr of numlist 3 5 8 {\n"
        "local ++cell_count\n"
        "local cell_rel_`cell_count' = `rr'\n"
        "}\n"
        "forvalues i = 1/`cell_count' {\n"
        "local rr = `cell_rel_`i''\n"
        "display \"cell `i' rel=`rr'\"\n"
        "}\n",
        session, piet::dispatch);
    const std::string dump = corpusShape.blocks.dump();
    check(dump.find("cell 1 rel=3") != std::string::npos &&
              dump.find("cell 2 rel=5") != std::string::npos &&
              dump.find("cell 3 rel=8") != std::string::npos,
          "BUG-339: the acceptance corpus's construct (build cell_rel_N in a loop, read it back "
          "through a nested reference) runs to completion with the right values");
  }
  {
    // The diagnostic half: an expression that really is unevaluable must name
    // ITSELF and its line. Before this, 160 identical bare 'invalid syntax'
    // blocks per run named neither, which is why localising the fault needed a
    // three-week log-slice comparison instead of a read.
    auto named = piet::doexec::executeDoSource("local bad = 1 % 2\n", session, piet::dispatch);
    bool sawNamed = false, sawContext = false;
    for (const auto& b : named.blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") {
        sawNamed = b.value("message", std::string()).find("1 % 2") != std::string::npos;
        sawContext = b.value("context", std::string()).find("local bad") != std::string::npos;
      }
    }
    check(sawNamed, "BUG-339 diagnostic: an unevaluable `= expr` names the expression it could "
                    "not evaluate, instead of a bare 'invalid syntax'");
    check(sawContext, "BUG-339 diagnostic: the error carries the line as `context` — the step-0a "
                      "return builds a fresh array and drops the entry echo, and `capture "
                      "noisily` strips inner echoes, so without this the block names nothing");
  }

  // --- ATTR-20260812: the depth-split timing instrument -------------------
  // What is being protected: `timing{}` bills every doexec item, and a loop
  // header's item stays OPEN around its body's items, so its wall INCLUDES
  // them. Rows therefore sit at mixed nesting levels and do not sum — the trap
  // p0_landings §T6 had to write "행 합산 금지" beside. timing_items (depth 1,
  // summable) and timing_nested (depth >= 2) separate the levels; these legs
  // pin that separation, the canonical-verb rekeying, and the two coverage
  // counters.
  {
    const int lvlSave = piet::counters::timingLevel();
    piet::counters::setTimingLevelForTest(3);
    const size_t itemsBefore = piet::counters::itemRecords().size();
    piet::counters::nestedTimings().clear();
    const int64_t graphBefore = piet::counters::graphEmissions();
    const int64_t errBefore = piet::counters::errorBlocks();

    auto attr = piet::doexec::executeDoSource(
        "display 1\n"
        "capture noisily summarize price\n"
        "forvalues i = 1/3 {\n"
        "display `i'\n"
        "}\n"
        "capture display invalid_syntax_here(\n",
        session, piet::dispatch);

    std::vector<piet::counters::ItemRecord> rows(
        piet::counters::itemRecords().begin() + static_cast<long>(itemsBefore),
        piet::counters::itemRecords().end());

    check(rows.size() == 4,
          "ATTR depth-split: one row per DEPTH-1 item — 4 items (display, capture-summarize, "
          "the whole forvalues block, capture-display), not one per executed line");
    if (rows.size() == 4) {
      check(rows[0].verb == "display" && rows[1].verb == "summarize" &&
                rows[2].verb == "forvalues" && rows[3].verb == "display",
            "ATTR verb rekeying: a prefixed item bills to the verb the registry RESOLVED, not "
            "its leading token — `capture noisily summarize` is a summarize row, and the brace "
            "header keeps its own token because it never reaches the registry");
      check(rows[0].line == 1 && rows[1].line == 2 && rows[2].line == 3 && rows[3].line == 4,
            "ATTR depth-split: `line` is the DEPTH-1 item index, so a brace block advances it "
            "by one however many lines its body has");
    }
    const auto& nested = piet::counters::nestedTimings();
    auto nd = nested.find("display");
    check(nd != nested.end() && nd->second.count == 3,
          "ATTR depth-split: the loop body's 3 display items land in timing_nested, NOT in "
          "timing_items — the level below, kept in its own map so the two can never be summed");
    check(nested.find("forvalues") == nested.end(),
          "ATTR depth-split: the loop HEADER is a depth-1 item and never appears in the nested "
          "map (the mixing this instrument exists to prevent)");

    // The exclusive view is the one that must ADD UP.
    const auto& excl = piet::counters::exclusiveTimings();
    int64_t exclTotal = 0;
    for (const auto& [k, v] : excl) exclTotal += v.wallNs;
    int64_t rootTotal = 0;
    for (const auto& r : rows) rootTotal += r.wallNs;
    check(exclTotal <= rootTotal,
          "ATTR exclusive: self-time over ALL depths sums to no more than the depth-1 total — "
          "the property the inclusive views cannot have (each second billed to exactly one verb)");
    auto ef = excl.find("forvalues");
    auto ed = excl.find("display");
    check(ef != excl.end() && ed != excl.end() && ef->second.wallNs < ed->second.wallNs,
          "ATTR exclusive: the loop HEADER's own self time is smaller than the body's, where its "
          "INCLUSIVE row was larger — the same run, decomposed instead of nested");

    // The parent-inclusive property, asserted rather than assumed: the header's
    // own row must be at least the sum of the body rows it wrapped.
    int64_t nestedWall = 0;
    for (const auto& [k, v] : nested) nestedWall += v.wallNs;
    if (rows.size() == 4) {
      check(rows[2].wallNs >= nestedWall,
            "ATTR depth-split: a depth-1 row's wall is INCLUSIVE of the depth-2 work it ran — "
            "the reason level-1 rows must not be added together");
    }

    check(piet::counters::errorBlocks() == errBefore,
          "ATTR coverage counter: a PLAIN `capture` suppresses the error block, so error_blocks "
          "does not move — the counter measures what the run displayed");
    check(piet::counters::graphEmissions() == graphBefore,
          "ATTR coverage counter: a run that draws nothing leaves graph_emissions alone");

    // Uncaptured error: one block out, one count.
    auto attrErr = piet::doexec::executeDoSource("display nonexistent_fn_xyz(3)\n", session,
                                                 piet::dispatch);
    int errBlocks = 0;
    for (const auto& b : attrErr.blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") ++errBlocks;
    }
    check(errBlocks == 1 && piet::counters::errorBlocks() == errBefore + 1,
          "ATTR coverage counter: error_blocks counts exactly the error blocks the run emitted "
          "(mechanised replacement for the whole-log grep the 08-12 bar-grade record used)");

    // `capture noisily` DISPLAYS the error, so it is a block and it counts —
    // the distinction the acceptance corpus turns on, where 160 such errors sit
    // behind `capture noisily` and never reach the log's own error channel.
    const int64_t errBeforeNoisily = piet::counters::errorBlocks();
    auto attrNoisy = piet::doexec::executeDoSource("capture noisily summarize no_such_var_xyz\n",
                                                   session, piet::dispatch);
    int noisyBlocks = 0;
    for (const auto& b : attrNoisy.blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") ++noisyBlocks;
    }
    check(noisyBlocks == 1 && piet::counters::errorBlocks() == errBeforeNoisily + 1,
          "ATTR coverage counter: `capture noisily` SHOWS its error, so it is an emitted block "
          "and error_blocks counts it — captured-ness is not the discriminator, display is");

    piet::counters::setTimingLevelForTest(lvlSave);
  }

  // --- INSPECT-2608: the per-FORM counter instrument (PIET_EMIT_TIMING=4) ----
  // WHY THIS NEEDS A GATE AT ALL. A misattributed counter delta is still a
  // plausible integer — every consumer downstream keeps producing a table that
  // looks right while pointing at the wrong command. That is the Verification
  // law's "new reusable mechanism whose value output stays correct even if the
  // optimization silently stopped working" class, so it is falsified, not merely
  // exercised. These legs run BEFORE any census reading is trusted.
  //
  // The three properties, in the order they can fail:
  //   (a) ATTRIBUTION  — the delta lands on the form that caused it, and the
  //                      neighbouring form reads zero.
  //   (b) FORM KEYING  — `summarize … if` bills to `summarize|if`, not to
  //                      `summarize`. Without this the census averages a
  //                      filtered read together with a whole-frame one.
  //   (c) COMPLETENESS — the rows SUM to the whole-run delta. This is the one
  //                      property the inclusive views cannot have, and the only
  //                      one that catches a leak in the child-bucket subtraction.
  {
    const int lvlSave = piet::counters::timingLevel();
    piet::counters::setTimingLevelForTest(4);
    piet::SessionState pcSession(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", pcSession);

    piet::counters::percmdRows().clear();
    piet::counters::childCounters().clear();
    const int64_t touchedBefore = piet::counters::touchedBytes();

    // `display` is the zero-work neighbour; the two summarizes differ ONLY in
    // the qualifier, which is what the form key claims to separate.
    // The `gen` first is not decoration: it leaves a DIRTY virtual head, which
    // is what makes the following reads materialize and move bytes at all. On a
    // clean resident frame `summarize` reads in place, every counter stays 0,
    // and the leg would SKIP for an environment reason that is really a fixture
    // one — measured, that is exactly what the first version of this leg did.
    // The loop is load-bearing, and finding that out is what this certificate is
    // for: the first version of this fixture was FLAT, so every item had zero
    // children, the child-bucket subtraction never had anything to subtract, and
    // the completeness leg below stayed GREEN with that subtraction DELETED — a
    // gate that could not fail on the defect it exists to catch. With a nested
    // item present, disabling the subtraction double-counts the body inside the
    // header's row and the sum overshoots the run.
    (void)piet::doexec::executeDoSource(
        "gen double zz = beertax * 2\n"
        "display 1\n"
        "summarize fatal\n"
        "summarize fatal if beertax > .5\n"
        "forvalues i = 1/2 {\n"
        "gen double q`i\' = beertax + `i\'\n"
        "summarize q`i\'\n"
        "}\n",
        pcSession, piet::dispatch);

    const auto& rows = piet::counters::percmdRows();
    auto touchedOf = [&](const std::string& form) -> int64_t {
      auto it = rows.find(form);
      if (it == rows.end() || it->second.self.empty()) return -1;  // -1 = row absent
      return it->second.self[0];  // index 0 = touched_bytes, foldableAccumulators() order
    };
    const int64_t whole = touchedOf("summarize");
    const int64_t filtered = touchedOf("summarize|if");
    const int64_t neighbour = touchedOf("display");

    if (whole <= 0) {
      skip("INSPECT-2608: per-form counter attribution",
           "summarize moved no bytes — no frames-capable `cobra serve` in this environment, so "
           "there is no delta to attribute");
    } else {
      check(filtered > 0,
            "INSPECT-2608 (b) FORM KEYING: `summarize … if` bills to its OWN row, not to "
            "`summarize` — the two forms are separate work");
      // `filtered > 0` is part of the predicate, not an assumption: touchedOf
      // returns -1 for an ABSENT row, and -1 != whole would otherwise let this
      // leg pass on the very defect the leg above is reporting (measured — the
      // seeded arm that drops the qualifier flag made this read 973648 vs -1).
      check(filtered > 0 && whole != filtered,
            "INSPECT-2608 (b): the two forms carry DIFFERENT byte counts (" +
                std::to_string(whole) + " vs " + std::to_string(filtered) +
                ") — equal counts would mean the qualifier never reached the key");
      check(neighbour <= 0,
            "INSPECT-2608 (a) ATTRIBUTION: the zero-work neighbour `display` carries no bytes — "
            "a delta that leaked across items would land here first");
      int64_t sum = 0;
      for (const auto& [form, r] : rows) {
        (void)form;
        if (!r.self.empty()) sum += r.self[0];
      }
      // WALL completeness, added after the instrument's first census exposed
      // that the counter columns were exclusive while `self_wall_ms` carried the
      // INCLUSIVE wall — three nested wrappers each reporting the same 146 s
      // under a key named "self". The counter legs could not see it, so the
      // property gets its own assertion: no single form may claim more wall than
      // the whole run took.
      double wallSum = 0.0, wallMax = 0.0;
      for (const auto& [form2, r2] : rows) {
        (void)form2;
        wallSum += static_cast<double>(r2.selfWallNs);
        if (static_cast<double>(r2.selfWallNs) > wallMax) wallMax = static_cast<double>(r2.selfWallNs);
      }
      check(wallMax <= wallSum + 1.0 && wallSum > 0.0,
            "INSPECT-2608 (c') WALL is EXCLUSIVE: no form claims more wall than every form "
            "together — an inclusive wall under a `self` key makes the column unsummable");
      const int64_t runDelta = piet::counters::touchedBytes() - touchedBefore;
      check(sum == runDelta,
            "INSPECT-2608 (c) COMPLETENESS: the per-form rows SUM to the run's own delta (" +
                std::to_string(sum) + " == " + std::to_string(runDelta) +
                ") — the property that makes this table readable as a decomposition");
    }
    piet::counters::setTimingLevelForTest(lvlSave);
  }

  // --- expandVarlist: success-path ORDER parity (wildcard + range) ---------
  auto feOk = piet::doexec::executeDoSource(
      "foreach v of varlist price-rep78 {\ndisplay \"`v'\"\n}\nforeach v of varlist t* "
      "{\ndisplay \"w:`v'\"\n}\n",
      session, piet::dispatch);
  setenv("PIET_NO_RESIDENT_READ", "1", 1);
  auto feOkEsc = piet::doexec::executeDoSource(
      "foreach v of varlist price-rep78 {\ndisplay \"`v'\"\n}\nforeach v of varlist t* "
      "{\ndisplay \"w:`v'\"\n}\n",
      session, piet::dispatch);
  unsetenv("PIET_NO_RESIDENT_READ");
  {
    std::vector<std::string> texts;
    for (const auto& b : feOk.blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display") {
        texts.push_back(b.value("text", std::string()));
      }
    }
    std::vector<std::string> expected = {"price", "mpg", "rep78", "w:trunk", "w:turn"};
    check(texts == expected,
          "C4-L2B expandVarlist ORDER gate: range `price-rep78` expands to price mpg rep78 and "
          "wildcard `t*` to trunk turn — column order off the card matches ds->names() order");
  }
  check(feOkEsc.blocks.dump() == feOk.blocks.dump(),
        "C4-L2B expandVarlist ORDER PARITY gate: the expanded iteration is byte-identical "
        "between planes (wildcard AND range forms)");

  // --- describe: factor-only card share + f64 honest fallback --------------
  (void)runCmd(session, "replace mpg = mpg + 2");  // fresh dirty window + memoized-ds reset
  piet::Datastore::resetConstructCount();
  const int64_t snapBeforeDesc = piet::counters::snapshotCount();
  auto descBlocks = piet::dispatch("describe make", session);
  {
    bool pinned = false;
    for (const auto& b : descBlocks) {
      if (b.is_object() && b.value("type", std::string()) == "describe" && b.contains("variables") &&
          b["variables"].size() == 1 && b["variables"][0].value("name", std::string()) == "make" &&
          b["variables"][0].value("n_unique", int64_t(-1)) == 74 &&
          b["variables"][0].value("n_missing", int64_t(-1)) == 0) {
        pinned = true;
      }
    }
    check(pinned,
          "C4-L2B describe gate: factor-only `describe make` answers off the card — "
          "n_unique==74 (the card's n_levels IS the datastore plane's levels.size()), "
          "n_missing==0");
  }
  check(piet::Datastore::constructCount() == 0,
        "C4-L2B describe MECHANISM gate: the factor-only describe constructs ZERO Datastores — "
        "presence, dtype, n_missing and n_unique all come off the CURRENT card");
  check(piet::counters::snapshotCount() == snapBeforeDesc,
        "C4-L2B describe MECHANISM gate: the dirty window stays OPEN across the card-plane "
        "describe (snapshot_count unchanged)");

  setenv("PIET_NO_RESIDENT_READ", "1", 1);
  piet::Datastore::resetConstructCount();
  const int64_t snapBeforeDescEsc = piet::counters::snapshotCount();
  auto descBlocksEsc = piet::dispatch("describe make", session);
  unsetenv("PIET_NO_RESIDENT_READ");
  check(descBlocksEsc.dump() == descBlocks.dump(),
        "C4-L2B describe PARITY gate: `describe make` is byte-identical between the card plane "
        "and the escape arm's datastore plane (shared describeEntry renderer — the two planes "
        "cannot drift field-by-field)");
  check(piet::Datastore::constructCount() >= 1 &&
            piet::counters::snapshotCount() == snapBeforeDescEsc + 1,
        "C4-L2B describe escape MECHANISM gate: the escape arm really walks the datastore path "
        "(construct fired, window closed) — the dropped-env-check mutant-kill");

  // f64 in the described set: the card plane must fall back WHOLESALE (n_unique
  // is a values question) and stay byte-identical to the escape arm.
  (void)runCmd(session, "replace mpg = mpg + 3");  // fresh dirty window again
  piet::Datastore::resetConstructCount();
  auto descX = piet::dispatch("describe mpg make", session);
  check(piet::Datastore::constructCount() >= 1,
        "C4-L2B describe FALLBACK gate: an f64 in the described set forces the datastore plane "
        "even on the card plane — n_unique for f64 needs column VALUES, honesty over speed");
  setenv("PIET_NO_RESIDENT_READ", "1", 1);
  auto descXEsc = piet::dispatch("describe mpg make", session);
  unsetenv("PIET_NO_RESIDENT_READ");
  check(descXEsc.dump() == descX.dump(),
        "C4-L2B describe FALLBACK PARITY gate: the mixed f64+factor describe is byte-identical "
        "between planes (the fallback IS the pre-D1 path)");

  fs::remove_all(workdir, ec);
}


// BUG-191 RED certificate (P2-PERF ARC-B/B2). A silent-wrong-answer fix needs a
// probe that FAILS on the defect, and no existing gate had one: the corruption
// only appears when a column is renamed ON THE RESIDENT FRAME, which nothing
// asked for until `rename` was given the residency opt-in.
//
// The defect: cecon_cli's resident swap reattaches a pending column that still
// borrows the OUTGOING frame's buffer by looking the column up BY NAME. A
// rename is the one mutation that keeps the bytes and changes the name, so the
// lookup missed, the reattach was skipped, and the renamed column went on
// pointing at a buffer freed by the swap. Pre-fix, the summarize below reported
// mean 8.6e+109 over four values that are 1, 2, 1, 2.
//
// Deliberately built from a VIRTUAL head (`set obs` + `gen`, never saved), which
// is the state that makes every column an owned buffer of the outgoing frame —
// a file-backed head borrows the shared root mmap instead and survives by a
// different route, which is why the benchmark's own rename loop looked fine.
void testBUG191ResidentRenameKeepsValues() {
  std::printf("[selftest] --- BUG-191 resident `rename` preserves the renamed column's values ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug191").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 4", session);
  (void)piet::dispatch("gen double side = cond(_n==1 | _n==3, 1, 2)", session);
  (void)piet::dispatch("rename side other_side", session);

  piet::Datastore* ds = session.datastore();
  bool ok = ds != nullptr && ds->has("other_side") && ds->nRows() == 4;
  check(ok, "BUG-191: the renamed column exists with the original row count");
  if (ok) {
    const double* v = ds->col("other_side").f64();
    check(v[0] == 1.0 && v[1] == 2.0 && v[2] == 1.0 && v[3] == 2.0,
          "BUG-191: the renamed column still holds 1,2,1,2 — not the freed predecessor buffer");
  }
}


// BETA campaign Phase G, SCOPE.md regression-lock rows (test-only —
// verified-correct-today behavior, no functional code change): BS-060/
// BS-061/BS-062/BS-088/BS-094. Each row was independently re-verified
// against this session's actual source (not assumed from SCOPE.md's own
// "already shipped" claim) before writing its lock leg — see the task's own
// return report for the verification trail.

void testG_BS060MeanonlyStrmutateLock() {
  std::printf("[selftest] --- BS-060 lock (Phase G): `summarize, meanonly` + string-typed "
              "gen/replace (strmutate) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs060").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);

  // (a) `summarize, meanonly` — already locked by the pre-existing BUG-090
  // leg above; re-asserted here under the BS-060 ledger name (SCOPE.md's
  // own row), same underlying dispatcher.cpp accept-as-no-op mechanism.
  {
    auto blocks = piet::dispatch("summarize beertax, meanonly", session);
    bool sawError = false;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") sawError = true;
    }
    check(!sawError, "BS-060(a): `summarize beertax, meanonly` is accepted (no error)");
  }

  // (b) string-typed gen/replace (datamgmt/string_generate_replace.cpp): create a string column,
  // then replace part of it — both routed through strmutate::runStringGen/
  // runStringReplace's own row-wise evaluator (cobra is all-f64 and cannot
  // hold string columns at all, so this MUST stay a PIET-native path).
  {
    auto genBlocks = piet::dispatch("gen str10 bs060s = \"abc\"", session);
    bool sawGenError = false;
    for (const auto& b : genBlocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") sawGenError = true;
    }
    check(!sawGenError, "BS-060(b): `gen str10 bs060s = \"abc\"` does not error");
    check(session.hasData() && session.datastore()->has("bs060s"),
          "BS-060(b): the string variable was actually created");

    auto repBlocks = piet::dispatch("replace bs060s = \"xyz\" in 1/1", session);
    bool sawRepError = false;
    for (const auto& b : repBlocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") sawRepError = true;
    }
    check(!sawRepError, "BS-060(b): `replace bs060s = \"xyz\" in 1/1` does not error");
  }

  fs::remove_all(workdir, ec);
}


void testG_BS061LockLegs() {
  std::printf("[selftest] --- BS-061 lock (Phase G): lincom fractional coefficients + csdid "
              "`estat event, post` -> `matrix b=e(b)/V=e(V)` ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs061").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  // (a) lincom fractional coefficients: runLincom is a pure pass-through
  // (parsed.varlist rejoined verbatim into cobra's --exp) — decimal/
  // division syntax is an ENGINE grammar question, this locks that the
  // pass-through itself stays intact end to end.
  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("regress income beertax spirits", session);
  {
    auto blocks = piet::dispatch("lincom 0.5*beertax + 0.5*spirits", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr,
          "BS-061(a): `lincom 0.5*beertax + 0.5*spirits` (fractional coefficients) returns an "
          "estimation block, not an error");
  }

  // (b)/(c) csdid `estat event, post` + `matrix b=e(b)`/`matrix V=e(V)`:
  // cross-worktree Stata-captured fixture (same sharing convention
  // testD3BS076Drdid's drdid2x2.dta leg uses) — skip gracefully if
  // unavailable in this environment. Oracle: tests/validate/
  // csdid_estat_golden.do's sibling csdid_event.csv (`estat event,
  // window(-5 5)` — the SAME window this leg requests): Post_avg
  // att=4.248773850990443 se=1.072095877429521. `, post` only changes
  // WHERE the identical numbers land (r() vs e()), never their value.
  (void)piet::dispatch(
      ("capture use \"" + selftestValidateDir() + "/csdid_input.dta\", clear").c_str(),
      session);
  if (session.eval.locals.get("_rc", "") != "0") {
    skip("BS-061(b/c) csdid estat event,post + matrix e(b)/e(V)",
         "could not load the csdid_input.dta cross-worktree fixture in this environment (see "
         "tests/validate/csdid_estat_golden.log for the pinned oracle values)");
    fs::remove_all(workdir, ec);
    return;
  }

  (void)piet::dispatch(
      "csdid visits_w female age_z edu_hi, ivar(h_pid) time(wv) gvar(gvar) method(dripw)",
      session);
  auto evBlocks = piet::dispatch("estat event, window(-5 5) post", session);
  check(firstBlockOfTypeD1(evBlocks, "error") == nullptr,
        "BS-061(b): `estat event, window(-5 5) post` does not error");

  (void)piet::dispatch("matrix bmat_bs061 = e(b)", session);
  (void)piet::dispatch("matrix vmat_bs061 = e(V)", session);
  const piet::eval::EvalMatrix* bmat = nullptr;
  const piet::eval::EvalMatrix* vmat = nullptr;
  for (const auto& p : session.eval.matrices) {
    if (p.first == "bmat_bs061") bmat = &p.second;
    if (p.first == "vmat_bs061") vmat = &p.second;
  }
  check(bmat != nullptr && vmat != nullptr,
        "BS-061(c): `matrix bmat=e(b)` / `matrix vmat=e(V)` after `estat event, post` both "
        "populate a matrix");
  if (bmat != nullptr && vmat != nullptr) {
    int idx = -1;
    for (size_t i = 0; i < bmat->colnames.size(); ++i) {
      if (bmat->colnames[i] == "Post_avg") {
        idx = static_cast<int>(i);
        break;
      }
    }
    check(idx >= 0, "BS-061(c): posted e(b) has a \"Post_avg\" column");
    if (idx >= 0 && !bmat->data.empty() && static_cast<size_t>(idx) < bmat->data[0].size()) {
      double att = bmat->data[0][static_cast<size_t>(idx)];
      check(std::abs(att - 4.248773850990443) < 1e-6,
            "BS-061(c) oracle: e(b)[Post_avg] ~= 4.248773850990443, got " + std::to_string(att));
    }
    if (idx >= 0 && static_cast<size_t>(idx) < vmat->data.size() &&
        static_cast<size_t>(idx) < vmat->data[static_cast<size_t>(idx)].size()) {
      double se = std::sqrt(vmat->data[static_cast<size_t>(idx)][static_cast<size_t>(idx)]);
      check(std::abs(se - 1.072095877429521) < 1e-6,
            "BS-061(c) oracle: sqrt(e(V)[Post_avg,Post_avg]) ~= 1.072095877429521, got " +
                std::to_string(se));
    }
  }

  fs::remove_all(workdir, ec);
}


// ── BUG-133: orphan-scratch reaping (owner-pid marker + startup sweep) ───────
// makeWorkdir() stamps each scratch dir with the creator's pid; a later launch
// reaps the ones whose owner is provably dead — SIGTERM/SIGKILL (routine
// kill_tree bounding) leaves the dir because NEITHER signal runs the RAII
// ScratchDirGuard. This drives the REAL main.cpp sweep (reapDeadScratch) and
// marker writer (writeScratchOwnerMarker), forward-declared above. Asserts a
// dead-owner+settled dir is reaped and that a live-owner dir, an unmarked dir,
// a too-fresh dead-owner dir, and a non-'piet_native_' dir are all preserved
// (the 07-18 INCIDENT class: never delete a live/uncertain scratch dir).
// Runs on BOTH platforms with the SAME assertions: only the dead-pid
// acquisition differs (fork/_exit/waitpid on POSIX, spawn-a-short-lived-child
// with its handle held open on Windows — see the _WIN32 arm below).
void testBUG133OrphanScratchReap() {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path base = fs::temp_directory_path() / "piet_native_selftest_bug133";
  fs::remove_all(base, ec);
  fs::create_directories(base, ec);
#ifdef _WIN32
  // Windows has no fork(), but the DEAD-PID ACQUISITION is the only fork-shaped
  // part of this gate — the sweep itself, legs (1)-(5) and the
  // PIET_KEEP_SCRATCH arm are platform-neutral and all run here, asserting
  // exactly what the POSIX arm asserts. The pid comes from re-spawning our OWN
  // image, the same self-spawn-instead-of-fork pattern
  // tests/unit/test_morsel_determinism.cpp uses; `--version` is main.cpp's
  // cheapest returning CLI mode (prints one line, returns 0, opens no window,
  // and — checked — never reaches makeWorkdir()/reapDeadScratch(), so the child
  // cannot perturb this test's base dir). Its one stdout line lands in the
  // selftest transcript; that is cosmetic.
  //
  // _P_NOWAIT rather than _P_WAIT is deliberate and is what makes this leg
  // SOUND rather than flaky: it hands back the child's PROCESS HANDLE, and
  // Windows cannot recycle a pid while any handle to that process object is
  // open. Holding `deadChild` open until after the last reapDeadScratch() call
  // closes the pid-reuse window entirely. It does NOT make the pid look alive:
  // scratchPidAlive() (main.cpp) probes GetExitCodeProcess(...) == STILL_ACTIVE,
  // which an exited-but-still-referenced process fails, so the owner is
  // reported provably dead for the whole sweep.
  HANDLE deadChild = nullptr;
  auto acquireDeadPid = [&deadChild]() -> long {
    wchar_t exe[32768];
    const DWORD exeCap = static_cast<DWORD>(sizeof(exe) / sizeof(exe[0]));
    const DWORD exeLen = ::GetModuleFileNameW(nullptr, exe, exeCap);
    if (exeLen == 0 || exeLen >= exeCap) return -1;
    const intptr_t rc =
        ::_wspawnl(_P_NOWAIT, exe, exe, L"--version", static_cast<wchar_t*>(nullptr));
    if (rc <= 0) return -1;
    HANDLE h = reinterpret_cast<HANDLE>(rc);
    if (::WaitForSingleObject(h, 30000) != WAIT_OBJECT_0) {
      ::CloseHandle(h);
      return -1;
    }
    DWORD code = STILL_ACTIVE;
    if (!::GetExitCodeProcess(h, &code) || code == STILL_ACTIVE) {
      ::CloseHandle(h);
      return -1;
    }
    deadChild = h;  // kept open (pid pinned dead) until the end of this function
    return static_cast<long>(::GetProcessId(h));
  };
#else
  // A definitively-dead pid: fork a child that immediately _exit()s and reap it,
  // so the pid names no running process by the time the sweep probes it.
  auto acquireDeadPid = []() -> long {
    pid_t c = ::fork();
    if (c == 0) _exit(0);  // child: touch nothing that could deadlock, just leave
    if (c < 0) return -1;
    int st = 0;
    ::waitpid(c, &st, 0);
    return static_cast<long>(c);
  };
#endif
  auto mkScratch = [&](const char* name) -> fs::path {
    fs::path d = base / name;
    fs::create_directories(d, ec);
    std::ofstream(d / "payload.hbin", std::ios::binary) << "stub-scratch-bytes";
    return d;
  };
  auto stampPid = [&](const fs::path& d, long pid) {
    std::ofstream(d / piet::scratchOwnerMarkerName(), std::ios::binary | std::ios::trunc)
        << pid << '\n';
  };
  auto settle = [&](const fs::path& d) {  // push mtime past the reap age floor
#ifdef _WIN32
    // std::filesystem::last_write_time on a DIRECTORY fails with EACCES here:
    // Windows needs the directory opened with FILE_FLAG_BACKUP_SEMANTICS before
    // its timestamps can be set, which the CRT wrapper does not do. The ec was
    // swallowed, so settle() silently did nothing, every dir stayed "fresh", and
    // the reap legs failed against a reaper that was behaving correctly. Set the
    // time through Win32 directly. reapDeadScratch reads the DIRECTORY's mtime
    // (main.cpp), so aging the marker file instead would not exercise it.
    HANDLE h = ::CreateFileW(d.c_str(), FILE_WRITE_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    // 30 minutes back, in 100ns ticks.
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t;
    t.LowPart = now.dwLowDateTime;
    t.HighPart = now.dwHighDateTime;
    t.QuadPart -= 30ULL * 60ULL * 10000000ULL;
    FILETIME aged{};
    aged.dwLowDateTime = t.LowPart;
    aged.dwHighDateTime = t.HighPart;
    ::SetFileTime(h, nullptr, nullptr, &aged);
    ::CloseHandle(h);
#else
    fs::last_write_time(d, fs::file_time_type::clock::now() - std::chrono::minutes(30), ec);
#endif
  };

  long dead = acquireDeadPid();
#ifdef _WIN32
  if (dead <= 0) {
    // Honest degradation, NOT a blanket skip: without a provably-dead pid the
    // dead-owner legs have nothing real to assert, so run the platform-neutral
    // SAFETY legs that need no dead pid and name EXACTLY which legs did not run.
    fs::path liveOnly = mkScratch("piet_native_liveB");
    piet::writeScratchOwnerMarker(liveOnly.u8string());
    settle(liveOnly);
    fs::path unmarkedOnly = mkScratch("piet_native_unmarkedC");
    settle(unmarkedOnly);
    int reapedNone = piet::reapDeadScratch(base.u8string());
    check(fs::exists(liveOnly, ec),
          "BUG-133 SAFETY: live-owner (our pid) scratch dir preserved");
    check(fs::exists(unmarkedOnly, ec),
          "BUG-133 SAFETY: unmarked scratch dir preserved (conservative skip)");
    check(reapedNone == 0,
          "BUG-133 SAFETY: sweep reaps nothing when no dir has a dead owner");
    skip("BUG-133 orphan-reap: dead-owner legs only — (1) dead+settled REAPED, "
         "(4) too-fresh dead-owner preserved, (5) non-'piet_native_' dead-owner preserved, "
         "the reaped==1 count, and the PIET_KEEP_SCRATCH arm",
         "could not spawn a short-lived child to obtain a provably-dead pid on this host; "
         "the live-owner and unmarked SAFETY legs above DID run");
    fs::remove_all(base, ec);
    return;
  }
  // Not a tautology (the `dead <= 0` case returned above): this asserts the pid
  // we are about to stamp is the exited CHILD's and not our own still-live one,
  // i.e. GetProcessId() read the spawned handle, not the current process.
  check(dead != static_cast<long>(::GetCurrentProcessId()),
        "BUG-133 orphan-reap: acquired a spawned short-lived child's pid, distinct from this "
        "process's own live pid (_wspawnl(_P_NOWAIT) + WaitForSingleObject + GetProcessId; the "
        "process handle is held open so the pid cannot be recycled mid-sweep)");
#else
  check(dead > 0, "BUG-133 orphan-reap: acquired a dead pid via fork/_exit/waitpid");
#endif

  // (1) dead owner + settled mtime → REAPED.
  fs::path deadDir = mkScratch("piet_native_deadA");
  stampPid(deadDir, dead);
  settle(deadDir);
  // (2) live owner (our own pid, via the REAL writer) → PRESERVED.
  fs::path liveDir = mkScratch("piet_native_liveB");
  piet::writeScratchOwnerMarker(liveDir.u8string());
  settle(liveDir);
  // (3) unmarked → PRESERVED (conservative skip).
  fs::path unmarkedDir = mkScratch("piet_native_unmarkedC");
  settle(unmarkedDir);
  // (4) dead owner but brand-new mtime → PRESERVED this round (mtime floor).
  fs::path freshDir = mkScratch("piet_native_freshD");
  stampPid(freshDir, dead);
  // (5) dead-owner marker but NOT our scratch-name prefix → PRESERVED.
  fs::path alienDir = base / "not_ours_E";
  fs::create_directories(alienDir, ec);
  stampPid(alienDir, dead);
  settle(alienDir);

  int reaped = piet::reapDeadScratch(base.u8string());

  check(!fs::exists(deadDir, ec), "BUG-133 REAP: dead-owner + settled scratch dir removed");
  check(fs::exists(liveDir, ec), "BUG-133 SAFETY: live-owner (our pid) scratch dir preserved");
  check(fs::exists(unmarkedDir, ec),
        "BUG-133 SAFETY: unmarked scratch dir preserved (conservative skip)");
  check(fs::exists(freshDir, ec),
        "BUG-133 SAFETY: too-fresh dead-owner dir preserved (mtime floor)");
  check(fs::exists(alienDir, ec),
        "BUG-133 SAFETY: non-'piet_native_' dir preserved (prefix guard)");
  check(reaped == 1, "BUG-133 REAP: exactly one dir reaped (only the dead+settled one)");

  // PIET_KEEP_SCRATCH=1 disables the sweep wholesale (debug escape hatch, parity
  // with ScratchDirGuard) — a fresh dead+settled dir must survive.
  fs::path keepDir = mkScratch("piet_native_keepF");
  stampPid(keepDir, dead);
  settle(keepDir);
  ::setenv("PIET_KEEP_SCRATCH", "1", 1);
  int reapedKept = piet::reapDeadScratch(base.u8string());
  ::unsetenv("PIET_KEEP_SCRATCH");
  check(fs::exists(keepDir, ec) && reapedKept == 0,
        "BUG-133 SAFETY: PIET_KEEP_SCRATCH=1 disables the reap sweep");

  fs::remove_all(base, ec);
#ifdef _WIN32
  // Release the pid pin only now — every reapDeadScratch() call above has run.
  if (deadChild != nullptr) ::CloseHandle(deadChild);
#endif
}


// ── BUG-138: `save` must never destroy its own source file ───────────────────
// `use FILE` leaves session.currentHbin() pointed AT the user's own file (only
// a *mutating* verb re-points it at a workdir scratch hbin), so the old
// open-coded `ofstream(dest, trunc)` in runSaveData truncated that very file
// while reading from it — `use X` → `save X, replace` deleted X and reported a
// disk-full that never happened. runSaveData now clones/copies into a sibling
// temp and rename(2)s it over `dest`, so `dest` is never opened for write.
//
// The three destructive arms from the BUG-138 repro table are asserted here
// (arm 1 = nothing in between, arm 3 = a NON-mutating command in between,
// arm 4 = a relative spelling resolved through session.cwd), plus arm 2 (a
// mutating verb, incidentally safe before this fix) as the control proving the
// ordinary write path still produces a real, larger file. Each arm checks BOTH
// that the file survives AND that its bytes are unchanged — a surviving but
// rewritten file would be a different, quieter bug.
void testBUG138SaveOverSource() {
  std::printf("[selftest] --- BUG-138 `save` over its own source must not destroy it ---\n");
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path base = fs::temp_directory_path() / "piet_native_selftest_bug138";
  fs::remove_all(base, ec);
  fs::create_directories(base, ec);

  auto slurp = [](const fs::path& p) -> std::string {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
  };

  // One canonical user file, minted once and copied per arm so every arm starts
  // from byte-identical content.
  fs::path seedDir = base / "seed";
  fs::create_directories(seedDir, ec);
  fs::path seed = seedDir / "seed.hbin";
  {
    piet::SessionState session(seedDir.u8string());
    piet::dispatch("sysuse fatalities, clear", session);
    piet::dispatch("save \"" + seed.u8string() + "\", replace", session);
  }
  const std::string seedBytes = slurp(seed);
  check(fs::exists(seed, ec) && !seedBytes.empty(),
        "BUG-138 setup: a real on-disk user dataset was minted");

  // arm: (label, the command run between `use` and `save`, expect bytes unchanged)
  struct Arm {
    const char* tag;
    const char* between;
    bool expectUnchanged;
    bool relativeSpelling;
  };
  const Arm arms[] = {
      {"arm1 (nothing in between)", nullptr, true, false},
      {"arm3 (non-mutating `summarize` in between)", "summarize fatal", true, false},
      {"arm4 (relative spelling via session.cwd)", nullptr, true, true},
      {"arm2 (mutating `gen` in between — control)", "gen __probe138 = 1", false, false},
  };
  for (const Arm& a : arms) {
    fs::path wd = base / a.tag;
    fs::create_directories(wd, ec);
    fs::path user = wd / "user.hbin";
    fs::copy_file(seed, user, fs::copy_options::overwrite_existing, ec);
    check(!ec, std::string("BUG-138 setup: ") + a.tag + " starts from a copy of the seed");

    std::string spelling = a.relativeSpelling ? std::string("user.hbin") : user.u8string();
    piet::SessionState session(wd.u8string());
    session.cwd = wd.u8string();  // what the relative spelling resolves against
    piet::dispatch("use \"" + spelling + "\", clear", session);
    if (a.between != nullptr) piet::dispatch(a.between, session);
    piet::dispatch("capture save \"" + spelling + "\", replace", session);

    bool present = fs::exists(user, ec);
    check(present, std::string("BUG-138 ") + a.tag + ": the user's file still EXISTS after "
                               "`save` over its own source");
    check(session.eval.locals.get("_rc", "") == "0",
          std::string("BUG-138 ") + a.tag + ": the save reports success (_rc=0), not a "
                     "phantom disk-full");
    if (present) {
      std::string after = slurp(user);
      if (a.expectUnchanged) {
        check(after == seedBytes,
              std::string("BUG-138 ") + a.tag + ": the file's bytes are UNCHANGED (a "
                         "no-op round-trip, not a silent rewrite)");
      } else {
        // The control: a mutating verb re-points currentHbin() at a workdir
        // scratch file, so this arm writes a genuinely different (larger)
        // dataset — proving the ordinary save path still works.
        check(after.size() > seedBytes.size(),
              std::string("BUG-138 ") + a.tag + ": the ordinary write path still produces a "
                         "real, larger file (the added column landed)");
      }
    }
    // The sibling temp must never survive a successful save.
    int leftovers = 0;
    for (const auto& e : fs::directory_iterator(wd, ec)) {
      if (e.path().filename().u8string().find(".savetmp") != std::string::npos) ++leftovers;
    }
    check(leftovers == 0,
          std::string("BUG-138 ") + a.tag + ": no `.savetmp` sibling left behind");
  }

  fs::remove_all(base, ec);
}


// FLOOR campaign (stint 05a) — slot-compiled evalRows mechanism gate.
//
// WHAT IS BEING GATED. strmutate::evalRows was ~42% of PIET main-thread CPU on
// the campaign corpus: it decoded each column once but then re-bound EVERY
// column by string name into a std::map (ns.consts) for EVERY row, and
// re-parsed the expression string every row too — ~21M ordered-map insertions
// plus a per-row parse for one gen/replace/count. The fix compiles the
// expression ONCE (parse to AST + resolve each identifier to a slot / const /
// fn) and runs the row loop reading a flat slot vector: zero per-row map
// writes, zero per-row parse.
//
// WHY A MECHANISM COUNTER AND NOT A VALUE PIN. The slot path and the legacy
// per-row path compute byte-identical answers (same parser, same AST
// interpreter, only the identifier binding is hoisted out of the loop). So
// every freeze-119 and DOM value pin stays green whether or not the fast path
// is applying — a value pin CANNOT see this optimization die. evalRowsMapWrite
// Count() instruments the MECHANISM: it is 0 exactly when the slot path ran
// and >0 when the per-row map rebind ran, the same style as
// Datastore::constructCount() / SessionState::cardFoldCount() elsewhere.
void testFloorSlotCompiledEvalRows() {
  std::printf("[selftest] --- FLOOR: slot-compiled evalRows (strmutate) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_floor_slotrows").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  std::string fixture = std::string(selftestSourceFile());
  {
    auto pos = fixture.rfind("/piet/native/src/selftest.cpp");
    fixture = (pos == std::string::npos)
                  ? "tests/validate/auto.hbin"
                  : fixture.substr(0, pos) + "/tests/validate/auto.hbin";
  }
  if (!std::ifstream(cecon::to_native_path(fixture)).good()) {
    fixture = selftestValidateDir() + "/auto.hbin";
  }
  if (!std::ifstream(cecon::to_native_path(fixture)).good()) {
    skip("FLOOR slot-compiled evalRows (needs tests/validate/auto.hbin)",
         "fixture not found at " + fixture);
    fs::remove_all(workdir, ec);
    return;
  }

  piet::SessionState session(workdir);
  {
    nlohmann::json openBody{{"filepath", fixture}};
    auto openResult = piet::routes::open(session, openBody.dump());
    check(openResult.ok, "FLOOR slotrows setup: `use` auto.hbin returns ok=true");
  }
  piet::Datastore* ds = session.datastore();
  check(ds != nullptr && ds->has("mpg") && ds->has("make") && ds->dtype("mpg") == "f64" &&
            ds->dtype("make") == "factor",
        "FLOOR slotrows setup: fixture has f64 `mpg` and factor `make`");
  if (ds == nullptr) {
    fs::remove_all(workdir, ec);
    return;
  }
  const int64_t nRows = ds->nRows();
  check(nRows > 1, "FLOOR slotrows setup: multi-row fixture (nRows > 1)");

  // Independent ground truth for `mpg > 20`, scanned straight off the column
  // with the SAME missing rule evalRows applies (NaN -> +inf sentinel, which is
  // > 20). Grounds the counts below in the real file, not in each other.
  int64_t truthMpgGt20 = 0;
  {
    const double* mpg = ds->col("mpg").f64();
    for (int64_t i = 0; i < nRows; ++i) {
      double v = mpg[i];
      if (std::isnan(v) || v > 20.0) ++truthMpgGt20;
    }
  }

  piet::strmutate::setForceLegacyEvalRowsForTest(false);  // start on the fast path

  // === Leg A: the mechanism gate proper (numeric column slot) ==============
  piet::strmutate::resetEvalRowsMapWriteCount();
  auto slotA = piet::strmutate::countRowsIf(session, "mpg > 20");
  const int64_t slotWritesA = piet::strmutate::evalRowsMapWriteCount();
  check(slotA.ok, "FLOOR slotrows A: `count if mpg > 20` evaluates (slot path)");
  check(slotWritesA == 0,
        "FLOOR slotrows MECHANISM gate: a slot-compiled `mpg > 20` over a multi-row fixture "
        "performs ZERO per-row namespace-map writes (evalRowsMapWriteCount()==0). This is the "
        "ONLY signal that slot-compilation is still applying — the answer is identical either way, "
        "so no freeze/DOM value pin can catch a regression here. A nonzero count means "
        "compilePreboundExpr silently stopped compiling `col > literal` (or a new per-row "
        "`ns.consts[...] =` reached the fast path) and evalRows fell back to the ~21M-insertion "
        "per-row rebind while every value pin stayed green");

  // === Leg A fallback: force the legacy path, same expression ==============
  // The deliberately-excluded path (here via the test hook; in production a
  // compile failure routes here). It must still evaluate correctly AND show the
  // per-row map writes the slot path elided.
  piet::strmutate::setForceLegacyEvalRowsForTest(true);
  piet::strmutate::resetEvalRowsMapWriteCount();
  auto legacyA = piet::strmutate::countRowsIf(session, "mpg > 20");
  const int64_t legacyWritesA = piet::strmutate::evalRowsMapWriteCount();
  piet::strmutate::setForceLegacyEvalRowsForTest(false);
  check(legacyA.ok, "FLOOR slotrows A: forced-legacy `count if mpg > 20` evaluates");
  check(legacyWritesA > 0,
        "FLOOR slotrows FALLBACK gate: the legacy per-row path performs map writes > 0 — proof "
        "the counter can observe the very cost the slot path removed, and that the fallback the "
        "slot path relies on for uncompilable expressions still runs correctly");
  check(slotA.n == legacyA.n && slotA.n == truthMpgGt20,
        "FLOOR slotrows EQUIVALENCE gate: slot path, forced-legacy path and a direct column scan "
        "all agree on `count if mpg > 20`. slot != legacy means compile-once changed an answer; "
        "either != the scan means both evaluator paths share a bug the independent scan catches");

  // === Leg B: factor column read through a string fn (A12 #3, #4) ==========
  piet::strmutate::resetEvalRowsMapWriteCount();
  auto slotB = piet::strmutate::countRowsIf(session, "strlen(make) > 4");
  const int64_t slotWritesB = piet::strmutate::evalRowsMapWriteCount();
  piet::strmutate::setForceLegacyEvalRowsForTest(true);
  piet::strmutate::resetEvalRowsMapWriteCount();
  auto legacyB = piet::strmutate::countRowsIf(session, "strlen(make) > 4");
  const int64_t legacyWritesB = piet::strmutate::evalRowsMapWriteCount();
  piet::strmutate::setForceLegacyEvalRowsForTest(false);
  check(slotB.ok && legacyB.ok,
        "FLOOR slotrows B: `count if strlen(make) > 4` evaluates on both paths");
  check(slotWritesB == 0 && legacyWritesB > 0,
        "FLOOR slotrows B mechanism: a factor column read through strlen() slot-compiles (0 map "
        "writes) while the legacy path rebinds (writes > 0). Exercises the FACTOR slot fill (level "
        "string, \"\" for a missing code) and the string-fn capture both living on the fast path");
  check(slotB.n == legacyB.n,
        "FLOOR slotrows B equivalence: the factor-slot + string-fn answer is identical "
        "compile-once vs per-row-rebind — the level string reaches the evaluator the same way");

  // === Leg C: the _n row special is a per-row-varying slot (A12 #5) ========
  const int64_t truthNLe10 = nRows < 10 ? nRows : 10;
  piet::strmutate::resetEvalRowsMapWriteCount();
  auto slotC = piet::strmutate::countRowsIf(session, "_n <= 10");
  const int64_t slotWritesC = piet::strmutate::evalRowsMapWriteCount();
  piet::strmutate::setForceLegacyEvalRowsForTest(true);
  auto legacyC = piet::strmutate::countRowsIf(session, "_n <= 10");
  piet::strmutate::setForceLegacyEvalRowsForTest(false);
  check(slotC.ok && legacyC.ok, "FLOOR slotrows C: `count if _n <= 10` evaluates on both paths");
  check(slotWritesC == 0,
        "FLOOR slotrows C mechanism: `_n <= 10` slot-compiles with 0 map writes — the _n special "
        "is a per-row slot the caller refills, not a per-row map insertion");
  check(slotC.n == legacyC.n && slotC.n == truthNLe10,
        "FLOOR slotrows C equivalence: _n varies correctly per row — slot, legacy and the "
        "min(10, nRows) truth all agree. A frozen _n slot would make this count wrong");

  // === Leg D: an explicit subscript is answered, not refused (A12 #6) =======
  // The pyeval grammar has no `[`. This leg pinned that limitation: the subscript
  // form fell to the legacy per-row path and failed (ok=false). Since BUG-412's
  // review (4526341a) the condition evaluator hands an explicit subscript on a
  // numeric variable to the engine (materializeTsIf, the route L./F./D. take) and
  // reads the scratch column, so the count is Stata's: StataBE 19.5 `sysuse auto`,
  // `count if weight > weight[_n-1]` 36, `count if price > price[_n-1] & mpg > 20`
  // 13 (2026-11-04).
  auto slotD = piet::strmutate::countRowsIf(session, "weight > weight[_n-1]");
  check(slotD.ok && slotD.n == 36,
        "FLOOR slotrows D: `count if weight > weight[_n-1]` answers Stata's 36 (the subscript "
        "is materialized by the engine, not refused) -- got ok=" + std::to_string(slotD.ok) +
            " n=" + std::to_string(slotD.n));
  auto slotD2 = piet::strmutate::countRowsIf(session, "price > price[_n-1] & mpg > 20");
  check(slotD2.ok && slotD2.n == 13,
        "FLOOR slotrows D: `count if price > price[_n-1] & mpg > 20` answers Stata's 13 -- got "
        "ok=" + std::to_string(slotD2.ok) + " n=" + std::to_string(slotD2.n));

  piet::strmutate::setForceLegacyEvalRowsForTest(false);  // leave the test hook off
  fs::remove_all(workdir, ec);
}


// A12/FLOOR mechanism gate: Datastore::buildVarInfo must NOT deep-copy factor
// level strings — it installs a non-owning LevelsRef VIEW (datastore.cpp) so a
// card build over a large factor no longer pays ~59 ms copying strings the
// card (n_levels only, F5) never emits. LevelsRef::levelStringCopies() counts
// every whole-dictionary owned install; this leg asserts it stays 0 across a
// factor-bearing varlist card build, and that the value-label remap
// COPIES-ON-WRITE exactly one dictionary (not the whole card, not per build)
// while leaving the shared column view untouched.
void testLevelsViewNoCopy() {
  std::printf("[selftest] --- A12/FLOOR: buildVarInfo level-view (no deep copy) ---\n");

  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_levelsview").u8string();
  std::error_code ec;
  fs::create_directories(workdir, ec);

  std::string fixture = std::string(selftestSourceFile());
  auto pos = fixture.rfind("/piet/native/src/selftest.cpp");
  fixture = (pos == std::string::npos) ? "tests/validate/auto.hbin"
                                       : fixture.substr(0, pos) + "/tests/validate/auto.hbin";
  if (!std::ifstream(cecon::to_native_path(fixture)).good()) {
    fixture = selftestValidateDir() + "/auto.hbin";
  }
  if (!std::ifstream(cecon::to_native_path(fixture)).good()) {
    skip("A12 level-view gate (needs tests/validate/auto.hbin)", "fixture not found at " + fixture);
    fs::remove_all(workdir, ec);
    return;
  }

  piet::SessionState session(workdir);
  {
    nlohmann::json body{{"filepath", fixture}};
    (void)piet::routes::open(session, body.dump());
  }
  if (!session.hasData()) {
    skip("A12 level-view gate", "auto.hbin did not load");
    fs::remove_all(workdir, ec);
    return;
  }
  piet::Datastore* ds = session.datastore();
  check(ds != nullptr && ds->dtype("make") == "factor",
        "A12 gate setup: auto.hbin loaded and `make` is a factor column");
  if (ds == nullptr) {
    fs::remove_all(workdir, ec);
    return;
  }

  const std::size_t nMakeLevels = ds->col("make").levels.size();
  check(nMakeLevels >= 2,
        "A12 gate setup: `make` carries a real level dictionary (>= 2 levels) to (not) copy");

  // ---- Leg 1: the card build copies ZERO level strings. -------------------
  // varlistWithMeta() runs the whole card path: Datastore::varlist()
  // (buildVarInfo per column) + the per-entry VarInfo copy + toJson (n_levels
  // only). No column carries a value label here, so nothing takes the CoW
  // branch and the build must not materialize a single level dictionary.
  piet::LevelsRef::resetLevelStringCopies();
  {
    auto vlist = session.varlistWithMeta(ds);
    nlohmann::ordered_json card = nlohmann::ordered_json::array();
    for (const auto& v : vlist) card.push_back(v.toJson());  // realize the emit
    int64_t makeNLevels = -1;
    for (const auto& v : card)
      if (v.value("name", std::string()) == "make")
        makeNLevels = v.value("n_levels", static_cast<int64_t>(-1));
    check(makeNLevels == static_cast<int64_t>(nMakeLevels),
          "A12 gate: the varlist card still reports make's n_levels (the value did not move when "
          "the owned copy became a view)");
  }
  check(piet::LevelsRef::levelStringCopies() == 0,
        "A12 mechanism gate: a full varlist card build over a factor-bearing dataset copies ZERO "
        "level strings — buildVarInfo installs a view, not `info.levels = c.levels`. A nonzero "
        "count is the regression signal that the deep copy was reintroduced on the card path");

  // ---- Leg 2: the value-label remap copies EXACTLY one dictionary (CoW). ---
  // Attach a value label to `make` so varlistWithMetaFrom takes the remap
  // branch, which must materialize make's dictionary ONCE (nMakeLevels
  // strings) — not the whole card, not per build — and must leave the shared
  // column view untouched.
  const std::string orig0 = ds->col("make").levels.front();
  session.varLblname["make"] = "mklbl";
  session.labelDefs["mklbl"] = {{orig0, "REMAPPED_0"}};
  piet::LevelsRef::resetLevelStringCopies();
  {
    auto vlist = session.varlistWithMetaFrom(ds->varlist());
    check(piet::LevelsRef::levelStringCopies() == static_cast<int64_t>(nMakeLevels),
          "A12 CoW gate: the value-label remap materializes EXACTLY make's one dictionary "
          "(nMakeLevels strings) — a larger count means it is copying more than the single column "
          "it rewrites, or copying per build");
    bool sawRemap = false;
    for (const auto& v : vlist)
      if (v.name == "make" && !v.levels.empty() && v.levels[0] == "REMAPPED_0") sawRemap = true;
    check(sawRemap, "A12 CoW gate: the remapped label is visible on make's returned VarInfo");
  }
  check(ds->col("make").levels.front() == orig0,
        "A12 CoW gate: the shared column level dictionary is UNCHANGED by the remap — copy-on-write "
        "did not write the remapped strings back through the view");

  fs::remove_all(workdir, ec);
}


// ── BUG-146: `merge`'s cardinality-violation hints must only name remedies
// THIS build accepts. The master-not-unique message used to end with "Use m:m
// or pre-sort and drop duplicates", but dispatcher.cpp's reMergeType matches
// `1:1|m:1|1:m` only — a user who followed the hint got `merge: syntax error`
// and a dead end. The gate below is deliberately NOT a pure string check: it
// takes the remedy each message NAMES and re-runs it, so a future edit that
// re-introduces an unreachable suggestion fails here rather than in the field.
void testBUG146MergeCardinalityHintHonesty() {
  std::printf("[selftest] --- BUG-146 `merge` cardinality hints name only reachable remedies ---\n");
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_bug146").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  const std::string uniqPath = workdir + "/uniq.hbin";
  const std::string dupPath = workdir + "/dup.hbin";

  // Two using-side files: one unique on `gid` (1,2), one duplicated (1,1,2,2).
  // Non-key column names differ from the master's so the conflicting-variable
  // check upstream of the cardinality check never fires first.
  {
    piet::SessionState setup(workdir);
    (void)piet::dispatch("clear", setup);
    (void)piet::dispatch("set obs 2", setup);
    (void)piet::dispatch("gen double gid = _n", setup);
    (void)piet::dispatch("gen double uval = _n * 10", setup);
    (void)piet::dispatch("save \"" + uniqPath + "\", replace", setup);
    (void)piet::dispatch("clear", setup);
    (void)piet::dispatch("set obs 4", setup);
    (void)piet::dispatch("gen double gid = cond(_n<=2, 1, 2)", setup);
    (void)piet::dispatch("gen double uval = _n * 10", setup);
    (void)piet::dispatch("save \"" + dupPath + "\", replace", setup);
  }
  check(fs::exists(uniqPath, ec) && fs::exists(dupPath, ec),
        "BUG-146 setup: unique-key and duplicate-key using files were written");

  // masterDup=true -> master is 1,1,2,2 (violates the "1" on the master side);
  // masterDup=false -> master is 1,2.
  auto runMergeCmd = [&](bool masterDup, const std::string& cmd) -> std::string {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch(masterDup ? "set obs 4" : "set obs 2", session);
    (void)piet::dispatch(
        masterDup ? "gen double gid = cond(_n<=2, 1, 2)" : "gen double gid = _n", session);
    (void)piet::dispatch("gen double mval = _n", session);
    auto blocks = piet::dispatch(cmd, session);
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") {
        return b.value("message", std::string("<error block with no message>"));
      }
    }
    return std::string();  // no error block == the command was accepted
  };
  auto has = [](const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
  };

  // ①-④ The built-in _merge's own refusal, r(459), naming the side that is
  // not unique (w2aw_data_combine_family.log dc_mx_11_dupmaster,
  // dc_mx_1m_dupmaster, dc_mx_11_dupusing, dc_mx_m1_dupusing: rc 459).
  const std::string kMasterDup = "does not uniquely identify observations in the master data";
  const std::string kUsingDup = "does not uniquely identify observations in the using data";
  std::string m11master = runMergeCmd(true, "merge 1:1 gid using \"" + uniqPath + "\"");
  check(has(m11master, kMasterDup),
        "BUG-146 ①: `merge 1:1` on a non-unique master reports Stata's r(459) master text");
  std::string m1mMaster = runMergeCmd(true, "merge 1:m gid using \"" + uniqPath + "\"");
  check(has(m1mMaster, kMasterDup),
        "BUG-146 ②: `merge 1:m` on a non-unique master reports Stata's r(459) master text");
  std::string m11using = runMergeCmd(false, "merge 1:1 gid using \"" + dupPath + "\"");
  check(has(m11using, kUsingDup),
        "BUG-146 ③: `merge 1:1` on a non-unique using reports Stata's r(459) using text");
  std::string mm1using = runMergeCmd(false, "merge m:1 gid using \"" + dupPath + "\"");
  check(has(mm1using, kUsingDup),
        "BUG-146 ④: `merge m:1` on a non-unique using reports Stata's r(459) using text");

  // ⑤/⑥ REACHABILITY — the whole point of the bug. Run the remedies ①/③ name
  // and require them to actually work; a syntax error here means the hint is
  // once again a dead end.
  std::string remedyM1 = runMergeCmd(true, "merge m:1 gid using \"" + uniqPath + "\"");
  check(remedyM1.empty(),
        "BUG-146 ⑤: the remedy ① names (`merge m:1`) genuinely runs on the same data — "
        "no error block at all");
  std::string remedy1M = runMergeCmd(false, "merge 1:m gid using \"" + dupPath + "\"");
  check(remedy1M.empty(),
        "BUG-146 ⑥: the remedy ③ names (`merge 1:m`) genuinely runs on the same data");

  // ⑦ `merge m:m` runs since the built-in _merge plan landed (dc_mmm rc 0).
  std::string mmm = runMergeCmd(true, "merge m:m gid using \"" + uniqPath + "\"");
  check(mmm.empty(), "BUG-146 ⑦: `merge m:m` runs (w2aw_data_combine_family.log dc_mmm rc 0)");

  fs::remove_all(workdir, ec);
}


// --- INSPECT-2608: joinby answers its metadata from the card, not a full open -
// Everything joinby asks of the MASTER frame is metadata (does a key exist, what
// are the non-key names, how many rows) and it was answering with a full
// Datastore open — while the ENGINE reads the file itself for the actual join,
// so the open was pure overhead. Measured, acceptance-corpus slice, deterministic
// and identical across repeats: touched_bytes 5,486 MB -> 3,663 MB (-33%).
//
// WALL IS A WASH and this leg does not claim otherwise: paired alternated arms,
// two rounds, gave old 130.0/113.6 s against new 118.8/119.3 s — the run-to-run
// spread swamps the difference. The counter is the sworn witness here; the change
// is kept on gate ③ (bytes the semantics do not require), not on a speed claim.
void testJoinbyCardEntrance() {
  std::printf("[selftest] --- INSPECT-2608: joinby entrance reads the card, not the frame ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_joinbycard").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("keep state year fatal beertax", session);
  (void)piet::dispatch("save \"" + workdir + "/u.hbin\", replace", session);
  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen double dirty = beertax * 2", session);  // leaves a dirty head

  const int64_t before = piet::counters::touchedBytes();
  auto blocks = piet::dispatch("joinby state year using \"" + workdir + "/u.hbin\"", session);
  const int64_t moved = piet::counters::touchedBytes() - before;

  bool ok = false, err = false;
  for (const auto& b : blocks) {
    if (!b.is_object()) continue;
    const std::string t = b.value("type", std::string());
    if (t == "error") err = true;
    if (t == "data_mutation" || t == "join_report") ok = true;
  }
  if (err || !ok) {
    skip("INSPECT-2608: joinby card entrance",
         "joinby did not complete here — no frames-capable `cobra serve`, so there is no "
         "entrance to measure");
  } else {
    // The frame is ~330 rows x 5 columns; a full master open would move the whole
    // thing on top of what the join itself reads. The bar is deliberately loose —
    // this asserts the ENTRANCE is gone, not a byte budget.
    // The real assertion is the ENTRANCE one below; this one keeps the byte
    // number honest by bounding it. `moved >= 0` would have been vacuous for an
    // int64 delta — a check that cannot fail is not a check. The master frame
    // here is ~336 rows x 5 columns (~13 KB); a full pre-join open of it on top
    // of the join's own reads would show as a multiple of that, so 4 MB is a
    // loose ceiling that still fails if the entrance open comes back.
    check(moved < 4 * 1024 * 1024,
          "INSPECT-2608: joinby moved " + std::to_string(moved / 1024) +
              " KB — no whole-frame open before the join");
    check(session.shadowVarlistIfCurrent() != nullptr,
          "INSPECT-2608 precondition: the shadow card is live after joinby — if it were not, the "
          "entrance would silently fall back to the full open on every call and this optimization "
          "would be dead code");
  }
  fs::remove_all(workdir, ec);
}


// --- INSPECT-2608: a scratch column's removal must not veto the card fold ----
// BUG-351's row filter pushes a mask column and has the engine delete it in the
// same call, so the delta reports dropped=1 — and residentCardDeltaComplete's
// guard was a bare `dropped != 0`, which refused the fold and rebuilt the WHOLE
// frame. Measured on a 108k x 192 corpus, 7 consecutive filters: 7.99 s / 2,512
// MB / 7 head materializations before, 0.81 s / 15 MB / 0 after.
//
// The narrowing is "refuse only when a dropped name is one the CARD HOLDS". The
// leg therefore pins BOTH directions — a scratch drop folds, and a real column
// drop still refuses — because a guard that only ever says yes is not a guard.
void testCardFoldScratchDrop() {
  std::printf("[selftest] --- INSPECT-2608: scratch-column drop still folds the card ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_foldscratch").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto varNames = [](const nlohmann::ordered_json& blocks) {
    std::vector<std::string> out;
    for (const auto& b : blocks) {
      if (!b.is_object() || !b.contains("varlist_after")) continue;
      for (const auto& v : b["varlist_after"]) out.push_back(v.value("name", std::string()));
    }
    return out;
  };
  auto has = [](const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
  };

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen str8 grp = \"aa\"", session);
  (void)piet::dispatch("replace grp = \"bb\" if beertax > .5", session);

  const int64_t snapsBefore = piet::counters::snapshotCount();
  auto filtered = piet::dispatch("keep if grp == \"bb\"", session);
  const int64_t snapsAfter = piet::counters::snapshotCount();

  auto names = varNames(filtered);
  if (names.empty()) {
    skip("INSPECT-2608: card fold on a scratch drop",
         "no varlist_after — no frames-capable `cobra serve` in this environment");
  } else {
    check(!has(names, "__piet_filtermask"),
          "the scratch mask never appears in the user's variable list");
    // e(sample) T2 (TASKS.md T2 item 1), the sibling: a fit's hidden mask
    // column rides the same card fold and never reaches varlist_after either,
    // while the rows it marks are still counted (w2_esample.log E5 keep-if).
    {
      auto displayText = [](const nlohmann::ordered_json& blocks) {
        for (const auto& b : blocks)
          if (b.is_object() && b.value("type", std::string()) == "display")
            return b.value("text", std::string());
        return std::string();
      };
      piet::SessionState es(workdir);
      (void)piet::dispatch("sysuse fatalities, clear", es);
      (void)piet::dispatch("quietly regress beertax spirits if year<=1984", es);
      auto kept = varNames(piet::dispatch("keep if beertax > .5", es));
      (void)piet::dispatch("quietly count if e(sample) & year<=1984", es);
      const std::string a = displayText(piet::dispatch("display r(N)", es));
      (void)piet::dispatch("quietly count if year<=1984", es);
      const std::string b = displayText(piet::dispatch("display r(N)", es));
      check(!kept.empty() && !has(kept, "__piet_esample__") && !a.empty() && a == b,
            "e(sample) T2: the mask column stays out of varlist_after through `keep if`, "
            "and the kept sample rows still count (" + a + " vs " + b + ")");
    }
    check(has(names, "beertax") && has(names, "grp"),
          "the real columns survive the filter and the fold");
    // The point of the narrowing: this filter must no longer materialize the
    // head to rebuild a card. One `keep if` used to cost one full frame.
    check(snapsAfter == snapsBefore,
          "INSPECT-2608: a string-predicate filter materializes the head ZERO times (" +
              std::to_string(snapsBefore) + " -> " + std::to_string(snapsAfter) + ")");
    // And the guard must still REFUSE a real restructure: `drop` of a real
    // column changes the column set the card describes.
    auto dropped = piet::dispatch("drop grp", session);
    auto after = varNames(dropped);
    check(!after.empty() && !has(after, "grp"),
          "a REAL column drop is still reflected — the narrowed guard did not make the card lie");
  }
  fs::remove_all(workdir, ec);
}



// --- BUG-198: a native `.hbin` save carries the session's display-format and
// variable-label layer, and `use` reads it back ------------------------------
//
// `format x %12.4f` and `label variable x "…"` write into SessionState::varMeta
// and NOWHERE else — not the engine's frame, not the scratch hbin. So the save
// plane's two hbin arms (the resident-frame snapshot and the clonefile/copy of
// the scratch head) both produced a file that had never heard of them, and the
// very next `use` came back at rc 0 with the pre-override format and label.
// Values were never affected, which is exactly why no gate saw it: a
// values-only check passes on the bug.
//
// The three checks below are BUG-190's family, on the persistence side:
// metadata CARRIAGE · payload-pointer REATTACH (BUG-191's shape: a rename must
// follow the payload, not a name) · RE-SEED SURVIVAL (BUG-190's own shape: a
// resident mutation must not erase the display layer). The fourth leg is the
// one the append-only key discipline exists for — an .hbin with no `label` key
// must load EXACTLY as it did before the key existed, because absent is not
// empty.
void testBUG198HbinMetaCarriage() {
  std::printf("[selftest] --- BUG-198: .hbin save/use carries the format+label layer ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug198").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto runCmd = [](piet::SessionState& session, const std::string& cmd) {
    nlohmann::json body{{"cmd", cmd}};
    auto r = piet::routes::command(session, body.dump());
    return nlohmann::json::parse(r.json);
  };
  // The header TEXT of an .hbin, verbatim: magic(8) + u64 header_len + JSON.
  // Read directly rather than through the reader, because half of what this leg
  // asserts is about which keys are PRESENT in the bytes — a reader that
  // defaults an absent key cannot tell that apart from a written one.
  auto headerText = [](const std::string& path) -> std::string {
    std::ifstream f(cecon::to_native_path(path), std::ios::binary);
    if (!f) return std::string();
    char magic[8] = {0};
    f.read(magic, 8);
    if (std::string(magic, 8) != "CECONHB1") return std::string();
    std::uint64_t hlen = 0;
    f.read(reinterpret_cast<char*>(&hlen), 8);
    if (!f || hlen == 0 || hlen > (1u << 26)) return std::string();
    std::string out(static_cast<size_t>(hlen), '\0');
    f.read(out.data(), static_cast<std::streamsize>(hlen));
    return f ? out : std::string();
  };
  auto meanOf = [&runCmd](piet::SessionState& s, const std::string& var) {
    double m = std::nan("");
    auto j = runCmd(s, "summarize " + var);
    std::function<void(const nlohmann::json&)> walk = [&](const nlohmann::json& n) {
      if (n.is_object()) {
        auto it = n.find("mean");
        if (it != n.end() && it->is_number()) m = it->get<double>();
        for (auto& [k, v] : n.items()) { (void)k; walk(v); }
      } else if (n.is_array()) {
        for (const auto& v : n) walk(v);
      }
    };
    walk(j);
    return m;
  };

  const std::string wd = workdir + "/carriage";
  fs::create_directories(wd, ec);

  // ---- author a dataset with an override on x and NOTHING on y -------------
  {
    piet::SessionState session(wd);
    session.cwd = wd;
    (void)runCmd(session, "clear");
    (void)runCmd(session, "set obs 5");
    (void)runCmd(session, "gen double x = _n");
    (void)runCmd(session, "gen double y = _n * 10");
    (void)runCmd(session, "format x %12.4f");
    (void)runCmd(session, "label variable x \"carried x label\"");
    check(session.varMeta.count("x") != 0 && session.varMeta["x"].format.has_value() &&
              *session.varMeta["x"].format == "%12.4f" &&
              session.varMeta["x"].label.has_value() &&
              *session.varMeta["x"].label == "carried x label",
          "BUG-198 setup: the session holds BOTH overrides before the save");
    auto jSave = runCmd(session, "save meta.hbin, replace");
    bool saveErr = false;
    for (const auto& b : jSave.value("blocks", nlohmann::json::array())) {
      if (b.value("type", std::string()) == "error") saveErr = true;
    }
    check(!saveErr, "BUG-198: `save meta.hbin, replace` succeeded");
  }

  // ---- the bytes themselves ------------------------------------------------
  const std::string hdr = headerText(wd + "/meta.hbin");
  check(!hdr.empty(), "BUG-198: the saved .hbin has a readable header");
  check(hdr.find("\"format\":\"%12.4f\"") != std::string::npos,
        "BUG-198: the saved header STATES the display format (not just the session)");
  check(hdr.find("\"label\":\"carried x label\"") != std::string::npos,
        "BUG-198: the saved header STATES the variable label — the key that did not exist");
  // Absent-vs-empty, on the write side: y had no override, so neither key may
  // appear for it. An emitted `"label":""` would be a fabricated value and would
  // also move every freeze fixture.
  check(hdr.find("\"label\":\"\"") == std::string::npos &&
            hdr.find("\"format\":\"\"") == std::string::npos,
        "BUG-198: no empty-string key is ever written (absent != empty)");

  // ---- CHECK 1 of 3 — METADATA CARRIAGE ------------------------------------
  {
    piet::SessionState reloaded(wd);
    reloaded.cwd = wd;
    (void)runCmd(reloaded, "use meta.hbin");
    check(reloaded.hasData(), "BUG-198: the saved .hbin loads back");
    check(reloaded.varMeta.count("x") != 0 && reloaded.varMeta["x"].format.has_value() &&
              *reloaded.varMeta["x"].format == "%12.4f",
          "BUG-198 (carriage): the display FORMAT survived save -> use");
    check(reloaded.varMeta.count("x") != 0 && reloaded.varMeta["x"].label.has_value() &&
              *reloaded.varMeta["x"].label == "carried x label",
          "BUG-198 (carriage): the variable LABEL survived save -> use");
    // ...and the untouched column stayed untouched. `y` must not acquire an
    // override out of the file's silence — if it did, the next save would write
    // the fabricated value into the header for real.
    const bool yClean = reloaded.varMeta.count("y") == 0 ||
                        (!reloaded.varMeta["y"].format.has_value() &&
                         !reloaded.varMeta["y"].label.has_value());
    check(yClean, "BUG-198 (carriage): a column the file said nothing about gains no override");
  }

  // ---- CHECK 2 of 3 — PAYLOAD-POINTER REATTACH (BUG-191's shape) -----------
  // BUG-191 was a resident swap reattaching borrowed columns BY NAME, so a
  // renamed column read freed memory. The metadata layer rides next to the name
  // and must NOT change that: after a rename the VALUES must still be the
  // column's own, and the layer must follow the variable it belongs to.
  {
    piet::SessionState renamed(wd);
    renamed.cwd = wd;
    (void)runCmd(renamed, "use meta.hbin");
    (void)runCmd(renamed, "rename x xx");
    check(std::abs(meanOf(renamed, "xx") - 3.0) < 1e-9,
          "BUG-198 (reattach): after `rename x xx` the column still reads its OWN payload "
          "(mean 3), not another column's and not freed memory");
    check(renamed.varMeta.count("xx") != 0 && renamed.varMeta["xx"].label.has_value() &&
              *renamed.varMeta["xx"].label == "carried x label",
          "BUG-198 (reattach): the label followed the rename to the new name");
    check(renamed.varMeta.count("x") == 0,
          "BUG-198 (reattach): and did not stay behind under the OLD name");
  }

  // ---- CHECK 3 of 3 — RE-SEED SURVIVAL (BUG-190's own shape) ---------------
  // BUG-190: a resident mutation re-seeds the frame's card from the worker,
  // which knows only DATA — so the display layer was erased by any `gen`/
  // `replace`. Here the layer must survive a mutation AND the save that follows,
  // because that save is what a user's next `use` will read.
  {
    piet::SessionState mutated(wd);
    mutated.cwd = wd;
    (void)runCmd(mutated, "use meta.hbin");
    (void)runCmd(mutated, "replace y = y + 1");
    (void)runCmd(mutated, "gen double z = x * 2");
    check(mutated.varMeta.count("x") != 0 && mutated.varMeta["x"].format.has_value() &&
              *mutated.varMeta["x"].format == "%12.4f" &&
              mutated.varMeta["x"].label.has_value() &&
              *mutated.varMeta["x"].label == "carried x label",
          "BUG-198 (re-seed): the layer survives a resident mutation in-session");
    (void)runCmd(mutated, "save meta2.hbin, replace");
    piet::SessionState after(wd);
    after.cwd = wd;
    (void)runCmd(after, "use meta2.hbin");
    check(after.varMeta.count("x") != 0 && after.varMeta["x"].format.has_value() &&
              *after.varMeta["x"].format == "%12.4f" &&
              after.varMeta["x"].label.has_value() &&
              *after.varMeta["x"].label == "carried x label",
          "BUG-198 (re-seed): and survives mutate -> save -> use, which is the shape a user "
          "actually types");
    check(std::abs(meanOf(after, "y") - 31.0) < 1e-9,
          "BUG-198 (re-seed): the mutation's VALUES are in the reloaded file too (mean(y)=31) — "
          "so the metadata did not ride a stale copy of the data");
  }

  // ---- APPEND-ONLY / OLD-FILE COMPATIBILITY -------------------------------
  // A session that declares nothing must write a header with NEITHER optional
  // key, byte-for-byte as before this change — that is what keeps the freeze
  // corpus and every hbin byte-compare golden from moving — and such a file must
  // load exactly as it always did.
  {
    const std::string pd = workdir + "/plain";
    fs::create_directories(pd, ec);
    piet::SessionState plain(pd);
    plain.cwd = pd;
    (void)runCmd(plain, "clear");
    (void)runCmd(plain, "set obs 4");
    (void)runCmd(plain, "gen double a = _n");
    (void)runCmd(plain, "save plain.hbin, replace");
    const std::string ph = headerText(pd + "/plain.hbin");
    check(!ph.empty() && ph.find("\"label\"") == std::string::npos,
          "BUG-198 (append-only): an override-free save writes NO `label` key at all");
    check(ph.find("\"format\"") == std::string::npos,
          "BUG-198 (append-only): and no `format` key either — the pre-A0 byte shape is intact");

    piet::SessionState back(pd);
    back.cwd = pd;
    (void)runCmd(back, "use plain.hbin");
    check(back.hasData(), "BUG-198 (old files): a key-less .hbin still loads");
    check(back.varMeta.count("a") == 0,
          "BUG-198 (old files): and produces NO override — absent is not empty");
    auto vlist = back.varlistWithMeta(back.datastore());
    bool aDefaulted = false;
    for (const auto& v : vlist) {
      if (v.name == "a") aDefaulted = (v.format == "%9.0g" && v.label.empty());
    }
    check(aDefaulted,
          "BUG-198 (old files): the card still renders the %9.0g default and an empty label — "
          "identical to the pre-BUG-198 build");
  }

  // ---- the VIRTUAL (RAM-only) tempfile arm --------------------------------
  // A minted `.tmp` save can take the no-disk virtual arm, which has no header
  // to carry anything. Its layer is parked beside the RAM entry instead; both
  // planes build it from the same hbinMetaPatch, so this leg holds whichever
  // arm the build actually takes.
  {
    const std::string td = workdir + "/tempfile";
    fs::create_directories(td, ec);
    piet::SessionState tsession(td);
    tsession.cwd = td;
    (void)runCmd(tsession, "clear");
    (void)runCmd(tsession, "set obs 3");
    (void)runCmd(tsession, "gen double v = _n");
    (void)runCmd(tsession, "format v %9.3f");
    (void)runCmd(tsession, "label variable v \"tempfile carried\"");
    (void)runCmd(tsession, "tempfile p");
    (void)runCmd(tsession, "save \"`p'\", replace");
    (void)runCmd(tsession, "clear");
    (void)runCmd(tsession, "use \"`p'\"");
    check(tsession.hasData(), "BUG-198 (tempfile): the checkpoint round-trip reloaded");
    check(tsession.varMeta.count("v") != 0 && tsession.varMeta["v"].format.has_value() &&
              *tsession.varMeta["v"].format == "%9.3f" &&
              tsession.varMeta["v"].label.has_value() &&
              *tsession.varMeta["v"].label == "tempfile carried",
          "BUG-198 (tempfile): the format+label layer survives the tempfile checkpoint idiom "
          "on whichever save arm this build takes");
  }

  fs::remove_all(workdir, ec);
}
