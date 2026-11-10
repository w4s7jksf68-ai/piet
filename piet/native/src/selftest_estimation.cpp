// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: estimation.
#include "selftest_support.h"
#include "selftest_cases.h"


// ── 2.2.0 item E: scratch superseded-delete (deletion + safety exclusions) ───
// SessionState::load() reclaims the superseded predecessor hbin — but ONLY a
// session-owned scratch file (inside workdir_, never a user file) that is not
// a live preserve snapshot.
// 2026-09-02 — the two regressions the wave answerbooks found on main once they
// became a merge-gate leg (BUG-395, BUG-396) and the `frame drop` orphan (BUG-389),
// pinned at the exact shapes that reproduced them:
//   395  `keep if` → `merge m:1 … using` — keep_if's gather leaves narrow (f32)
//        columns; merge_frames' morsel-parallel gather widened them from every
//        worker at once (const ensure_wide mutating shared state) → SIGABRT in the
//        engine, surfaced as "engine session restarted". Seeded-RED = revert the
//        F64Reader gather in src/data/frame2.cpp → this check reports the error block.
//   396  `frame F: cmd` swaps the head to F's file; the default head it returns to
//        was nothing's pin, so the eager scratch budget (default 0) unlinked it →
//        the next `preserve` reported "disk full or I/O error". Seeded-RED = drop the
//        ScratchPin in runFramePrefix.
//   389  `frame drop F` forgot the map entry and left the file — an ORPHAN no reap
//        could reach. Seeded-RED = drop the reclaimScratchFile call in runFrameDrop.
void testWaveRegressions20260902() {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path root = fs::temp_directory_path() / "piet_native_selftest_wavereg";
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  const std::string masterCsv = (root / "m.csv").u8string();
  const std::string usingCsv = (root / "u.csv").u8string();
  {
    std::ofstream f(cecon::to_native_path(masterCsv), std::ios::binary | std::ios::trunc);
    f << "id,t,x\n";
    for (int id = 1; id <= 40; ++id)
      for (int t = 1; t <= 6; ++t) f << id << "," << t << "," << (id * 10 + t) * 0.5 << "\n";
  }
  {
    std::ofstream f(cecon::to_native_path(usingCsv), std::ios::binary | std::ios::trunc);
    f << "id,z\n";
    for (int id = 1; id <= 40; ++id) f << id << "," << id * 3 << "\n";
  }
#ifndef _WIN32
  setenv("PIET_SCRATCH_BUDGET_MB", "0", 1);   // the eager policy that exposed 396/389
#else
  _putenv_s("PIET_SCRATCH_BUDGET_MB", "0");
#endif
  auto hasError = [](const nlohmann::json& blocks) {
    if (!blocks.is_array()) return false;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return true;
    return false;
  };
  auto errText = [](const nlohmann::json& blocks) {
    if (blocks.is_array())
      for (const auto& b : blocks)
        if (b.is_object() && b.value("type", std::string()) == "error")
          return b.value("message", std::string());
    return std::string();
  };
  auto countOf = [](const nlohmann::json& blocks) -> long long {
    if (blocks.is_array())
      for (const auto& b : blocks)
        if (b.is_object() && b.value("type", std::string()) == "display") {
          try { return std::stoll(b.value("text", std::string("-1"))); } catch (...) { return -1; }
        }
    return -1;
  };
  auto waitGone = [](const fs::path& p, int ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    std::error_code e;
    while (fs::exists(p, e)) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
  };
  fs::path wd = root / "wd";
  fs::create_directories(wd, ec);
  piet::SessionState s(wd.u8string());
  nlohmann::json mb{{"filepath", masterCsv}};
  auto opened = nlohmann::json::parse(piet::routes::open(s, mb.dump()).json);
  if (opened.value("action", std::string()) != "open") {
    skip("BUG-395/396/389 pins", "this cobra build could not open the csv fixture");
  } else {
    // ---- BUG-395: keep if → merge m:1 (narrow master into the parallel gather)
    (void)piet::dispatch("keep if t<=4", s);
    nlohmann::json mg = piet::dispatch("merge m:1 id using \"" + usingCsv + "\", nogen", s);
    check(!hasError(mg), "BUG-395: `keep if` then `merge m:1 … using` completes (no engine restart) — " + errText(mg));
    nlohmann::json cnt = piet::dispatch("count", s);
    check(countOf(cnt) == 160, "BUG-395: the merged filtered frame has 40 ids x 4 periods = 160 rows (got " +
                                   std::to_string(countOf(cnt)) + ")");
    nlohmann::json zsum = piet::dispatch("count if z==id*3", s);
    check(countOf(zsum) == 160, "BUG-395: every row carries its using value (z==id*3 on all 160)");

    // ---- BUG-396: frame prefix must not strand the default head
    (void)piet::dispatch("frame put id x, into(ff)", s);
    nlohmann::json fc = piet::dispatch("frame ff: count", s);
    check(!hasError(fc) && countOf(fc) == 160, "BUG-396 precondition: `frame ff: count` answers from the frame");
    bool headExists = s.currentHbin().has_value() &&
                      fs::exists(cecon::to_native_path(*s.currentHbin()), ec);
    check(headExists, "BUG-396: after `frame F: cmd` the default frame's head file still exists on disk "
                      "(eager scratch budget must not have unlinked it during the swap)");
    nlohmann::json pv = piet::dispatch("preserve", s);
    check(!hasError(pv), "BUG-396: `preserve` after a frame-prefix command succeeds — " + errText(pv));
    (void)piet::dispatch("restore", s);

    // ---- BUG-389: frame drop releases the frame's file
    std::string ffPath = s.frames.count("ff") ? s.frames.at("ff") : std::string();
    check(!ffPath.empty() && fs::exists(cecon::to_native_path(ffPath), ec),
          "BUG-389 precondition: the frame's hbin exists before `frame drop`");
    (void)piet::dispatch("frame drop ff", s);
    check(!ffPath.empty() && waitGone(cecon::to_native_path(ffPath), 2000),
          "BUG-389: `frame drop` releases the frame's scratch file (eager budget, no pin left) instead of orphaning it");
    check(s.frames.count("ff") == 0, "BUG-389: the dropped frame is gone from the ledger");
    nlohmann::json after = piet::dispatch("count", s);
    check(countOf(after) == 160, "BUG-389: the default frame is intact after the drop (160 rows)");

    // ---- BUG-389 (the mechanism): the orphan sweep at dispatch exit. The LEAK AUDIT's
    // worst shapes — a by: statistic and a filtered regression — minted per-group
    // slices and projections no ledger knew; after each top-level command the exit
    // census must read ORPHAN 0. Seeded-RED = PIET_NO_ORPHAN_SWEEP=1 (the A/B arm).
    std::string orphanShapes;
    auto orphanCount = [&]() -> long long {
      try {
        auto j = nlohmann::json::parse(s.scratchCensusJson());
        orphanShapes.clear();
        if (j.contains("orphan_shapes"))
          for (const auto& o : j["orphan_shapes"]) orphanShapes += o.value("shape", std::string()) + " ";
        return j.contains("by_class") && j["by_class"].contains("ORPHAN")
                   ? j["by_class"]["ORPHAN"].value("n", 0LL) : 0LL;
      } catch (...) { return -1; }
    };
    (void)piet::dispatch("bysort t: summarize x", s);
    (void)piet::dispatch("quietly regress x z if t<=2", s);
    (void)piet::dispatch("egen xbar = mean(x) if t<=3, by(id)", s);
    long long orphans = orphanCount();
    check(orphans == 0, "BUG-389: after `bysort: summarize`, `regress … if` and `egen … if` the exit census reads "
                        "ORPHAN 0 (got " + std::to_string(orphans) + ": " + orphanShapes + ") — every minted slice was swept");
    nlohmann::json cnt2 = piet::dispatch("count", s);
    check(countOf(cnt2) == 160, "BUG-389: the sweep never touched the live head (160 rows still)");
    // A user's own file inside the workdir is NOT the sweep's to take (registry-gated).
    const std::string userSave = (wd / "mine.dta").u8string();
    (void)piet::dispatch("save \"" + userSave + "\", replace", s);
    (void)piet::dispatch("count", s);  // a top-level command => a sweep ran
    check(fs::exists(cecon::to_native_path(userSave), ec),
          "BUG-389: a user `save` into the workdir survives the sweep (only registered mints are candidates)");

    // Every dispatch below goes through a catcher: a thrown exception is reported as
    // a named FAIL instead of terminating the whole selftest (the first cut died in
    // __cxa_rethrow with no line to blame).
    auto safeDispatch = [&](const std::string& cmd, piet::SessionState& sess) -> nlohmann::json {
      try {
        return piet::dispatch(cmd, sess);
      } catch (const std::exception& e) {
        check(false, std::string("BUG-390/392 harness: `") + cmd.substr(0, 60) + "` threw: " + e.what());
      } catch (...) {
        check(false, std::string("BUG-390/392 harness: `") + cmd.substr(0, 60) + "` threw a non-std exception");
      }
      return nlohmann::json::array();
    };
    // ---- BUG-389 (the crash floor): a virtual resident chain keeps its last on-disk
    // snapshot as the floor rollbackToFloor() re-adopts, until the next materialization
    // replaces it. The owner table never listed it, so probe_b4's
    // `gen str1 s = ""` + `replace s = s + lab + "_"` ended with the census at ORPHAN 1
    // (the strmut_ snapshot `gen` took to read rows, left as the floor when its setcol
    // went virtual), and a REGISTERED floor — the setobs_ file under the first virtual
    // gen — was deleted by the dispatch-exit sweep while rollbackToFloor still named it.
    // Seeded-RED = drop the `crash_floor` claim in SessionState::scratchOwners.
    {
      fs::path fwd = root / "floor_wd";
      fs::create_directories(fwd, ec);
      piet::SessionState fsess(fwd.u8string());
      fsess.cwd = fwd.u8string();
      auto classN = [&](const char* cls) -> long long {
        try {
          const nlohmann::json j = nlohmann::json::parse(fsess.scratchCensusJson());
          auto bc = j.find("by_class");
          if (bc == j.end() || !bc->is_object() || !bc->contains(cls)) return 0;
          return (*bc)[cls].value("n", 0LL);
        } catch (...) {
          return -1;  // a parse failure must not read as "clean"
        }
      };
      (void)safeDispatch("clear", fsess);
      (void)safeDispatch("set obs 3", fsess);
      (void)safeDispatch("gen str3 lab = \"ab\"", fsess);
      if (!fsess.residentHeadDirty()) {
        skip("BUG-389 crash floor", "the resident setcol plane is not engaged here (the head is not virtual)");
      } else {
        const long long floorAfterGen = classN("crash_floor");
        (void)safeDispatch("gen str1 s = \"\"", fsess);
        nlohmann::json rep = safeDispatch("replace s = s + lab + \"_\"", fsess);
        const long long orphans = classN("ORPHAN");
        const long long floors = classN("crash_floor");
        check(floorAfterGen == 1,
              "BUG-389: the snapshot under the first virtual gen is the crash floor and the dispatch-exit "
              "sweep spares it (crash_floor n=" + std::to_string(floorAfterGen) + ")");
        check(!hasError(rep) && orphans == 0 && floors == 1,
              "BUG-389: `gen str1 s = \"\"` + `replace s = s + lab + \"_\"` on the resident plane leaves ORPHAN 0 "
              "with the floor owned (ORPHAN n=" + std::to_string(orphans) + ", crash_floor n=" +
                  std::to_string(floors) + ") " + errText(rep));
      }
    }
    // ---- BUG-390: the `use` memos are bounded by bytes (cap 1 MB here => at most the
    // most recent entry survives). Seeded-RED = skip enforceUseCacheCap in the puts.
#ifndef _WIN32
    setenv("PIET_USE_CACHE_MB", "1", 1);
#else
    _putenv_s("PIET_USE_CACHE_MB", "1");
#endif
    const std::string big1 = (root / "big1.csv").u8string();
    const std::string big2 = (root / "big2.csv").u8string();
    for (const std::string* bp : {&big1, &big2}) {
      std::ofstream f(cecon::to_native_path(*bp), std::ios::binary | std::ios::trunc);
      f << "id,v1,v2,v3,v4\n";
      for (int i = 0; i < 60000; ++i) f << i << "," << i * 0.5 << "," << i * 0.25 << "," << (i % 7) << "," << (i % 11) << "\n";
    }
    (void)safeDispatch("use \"" + big1 + "\", clear", s);
    (void)safeDispatch("use \"" + big2 + "\", clear", s);
    check(s.useCacheEntryCount() >= 1 && s.useCacheEntryCount() <= 2,
          "BUG-390: with PIET_USE_CACHE_MB=1 two 60k-row `use`s leave exactly the newest source's memo(s), never zero "
          "(entries=" + std::to_string(s.useCacheEntryCount()) + ", bytes=" + std::to_string(s.useCacheBytes()) + ")");
    check(s.useConvCacheGet(big1, "x") == nullptr,
          "BUG-390: the older source's conversion memo was evicted (LRU by bytes)");
#ifndef _WIN32
    unsetenv("PIET_USE_CACHE_MB");
#else
    _putenv_s("PIET_USE_CACHE_MB", "");
#endif

    // ---- BUG-392: `clear all` forgets estimates, graphs, postfiles, file handles,
    // frames, the preserve stack and the use memos. Seeded-RED = drop the
    // clearAllSessionObjects() call in runClear.
    (void)safeDispatch("use \"" + masterCsv + "\", clear", s);
    (void)safeDispatch("quietly regress x t", s);
    (void)safeDispatch("estimates store m1", s);
    (void)safeDispatch("frame put id x, into(g1)", s);
    (void)safeDispatch("preserve", s);
    const std::string postTarget = (root / "post.dta").u8string();
    (void)safeDispatch("postfile ph a b using \"" + postTarget + "\", replace", s);
    (void)safeDispatch("post ph (1) (2)", s);
    const std::string txt = (root / "h.txt").u8string();
    (void)safeDispatch("file open fh using \"" + txt + "\", write replace", s);
    const bool armed = !s.estimates.empty() && s.frames.count("g1") && !s.preserveStack.empty() &&
                       s.postfiles.count("ph") && s.fileHandles.count("fh") && s.useCacheEntryCount() > 0;
    check(armed, "BUG-392 precondition: estimates, a frame, a preserve, an open postfile, an open file handle "
                 "and a use memo all exist before `clear all`");
    (void)safeDispatch("clear all", s);
    check(s.estimates.empty() && s.namedGraphs.empty() && s.postfiles.empty() && s.fileHandles.empty() &&
              s.frames.empty() && s.frameLinks.empty() && s.useCacheEntryCount() == 0,
          "BUG-392: `clear all` emptied estimates, graphs, postfiles, file handles, frames, links and the use memos");
    // Oracle (StataBE 19.5, 2026-09-02): a preserve SURVIVES `clear all`; `restore` brings the data back.
    check(!s.preserveStack.empty(), "BUG-392: `clear all` keeps the preserve stack (Stata: restore after clear all is rc 0)");
    nlohmann::json rs = safeDispatch("restore", s);
    nlohmann::json backN = safeDispatch("count", s);
    check(!hasError(rs) && countOf(backN) == 240,
          "BUG-392: `restore` after `clear all` brings the preserved 240 rows back (got " + std::to_string(countOf(backN)) + ")");
    (void)safeDispatch("clear all", s);
    // BUG-1079: this check used to assert the file was NOT written, which pinned
    // the defect. StataBE 19.5 (2026-10-28, internal capture
    // t1_clearall_postfile.log PREC|ca_desc|0|3|): `clear all` closes the open
    // postfile and the posted rows are on disk.
    check(fs::exists(cecon::to_native_path(postTarget), ec),
          "BUG-1079: a postfile open at `clear all` is closed and written, as Stata's postutil clear does");
    // BUG-1104: a bare `count` with nothing in memory is Stata's 0 at rc 0, so the
    // emptied session is read off hasData() and that zero (240 would be the restore).
    nlohmann::json nodata = safeDispatch("count", s);
    check(!s.hasData() && !hasError(nodata) && countOf(nodata) == 0,
          "BUG-392: no dataset is loaded after `clear all` (`count` answers 0, got " +
              std::to_string(countOf(nodata)) + ")");
  }
#ifndef _WIN32
  unsetenv("PIET_SCRATCH_BUDGET_MB");
#else
  _putenv_s("PIET_SCRATCH_BUDGET_MB", "");
#endif
  fs::remove_all(root, ec);
}


// ── AER-SCOPE1 bundle H (ESTIM, 2026-09-05) ──────────────────────────────────
//
// The numeric pins below are ORACLE values, captured from live StataBE 19.5 on
// tests/validate/auto.hbin's own source dataset -- not from this build's output.
// That direction matters: bundle H's specs were verified before implementation
// precisely because a golden captured from the same transcription as the code
// pins the transcription error as the answer. Loosening a tolerance here is
// therefore never the fix; it is the failure mode.
//
// Several of these arms exist because a correction had no other detector. The
// spearman guard pair, the sktest complement-form pin and the centile lower
// bound each caught a real defect while this bundle was being written.
void testAerEstimBundle() {
  std::printf("[selftest] --- AER-SCOPE1 bundle H (ESTIM): areg/ivreg2 + spearman/sktest/"
              "signrank/centile + xi + E3 ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_aerestim").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  const std::string autoFix = selftestValidateDir() + "/auto.hbin";
  if (!std::ifstream(cecon::to_native_path(autoFix)).good()) {
    skip("AER bundle H (needs tests/validate/auto.hbin)", "fixture not found at " + autoFix);
    return;
  }

  auto rv = [](piet::SessionState& s, const char* k) -> double {
    const double* p = s.eval.rValues.find(k);
    return p ? *p : std::numeric_limits<double>::quiet_NaN();
  };
  auto close = [](double a, double b, double rtol) {
    if (std::isnan(a) || std::isnan(b)) return false;
    const double d = std::fabs(a - b);
    return d <= rtol * std::max(1.0, std::fabs(b));
  };
  auto anyError = [](const auto& blocks) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return true;
    return false;
  };
  auto firstError = [](const auto& blocks) -> nlohmann::json {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return b;
    return nlohmann::json::object();
  };
  auto displayText = [](const auto& blocks) {
    std::string s;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "display")
        s += b.value("text", std::string());
    return s;
  };

  // (a) spearman -- the auto pin, to full precision.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto out = piet::dispatch("spearman mpg rep78", session);
    check(!anyError(out), "H/spearman: `spearman mpg rep78` runs");
    check(rv(session, "N") == 69.0, "H/spearman: r(N) == 69 (casewise: rep78 has 5 missing)");
    check(close(rv(session, "rho"), 0.3098266801580117, 1e-12),
          "H/spearman: r(rho) == 0.3098266801580117 (StataBE oracle) -- got " +
              std::to_string(rv(session, "rho")));
    check(close(rv(session, "p"), 0.0098023612827953, 1e-9),
          "H/spearman: r(p) == 0.0098023612827953 (StataBE oracle) -- got " +
              std::to_string(rv(session, "p")));
    // p_l and p_u are separate published returns and the whole point of using
    // betaitail rather than 1 - p_l. Pinned independently of p.
    check(close(rv(session, "p_l"), 0.9950988193586023, 1e-9), "H/spearman: r(p_l) oracle");
    check(close(rv(session, "p_u"), 0.0049011806413977, 1e-9), "H/spearman: r(p_u) oracle");
    const std::string t = displayText(out);
    check(t.find("Number of observations =     69") != std::string::npos,
          "H/spearman: display line 1 matches Stata's spacing");
    check(t.find("Spearman's rho = 0.3098") != std::string::npos,
          "H/spearman: display line 2 matches Stata");
    check(t.find("are independent") != std::string::npos, "H/spearman: H0 line present");
  }

  // (b) spearman on a TIE-FREE pair -- the d clamp. Without it, d lands just
  // below zero, sqrt gives NaN, and every downstream quantity including p
  // becomes missing. This arm exists because the rep78 pin above is
  // structurally blind to it: rep78 is heavily tied, so it takes the d > 0
  // branch and passes either way.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto out = piet::dispatch("spearman price weight", session);
    check(!anyError(out), "H/spearman: tie-free pair runs");
    check(!std::isnan(rv(session, "p")),
          "H/spearman: r(p) is NOT missing on a tie-free pair (the d clamp; unclamped this is NaN)");
    check(!std::isnan(rv(session, "rho")), "H/spearman: r(rho) is not missing there either");
  }

  // (c) sktest -- table shape, last-variable r(), and the r(table) matrix that
  // had nowhere to live until this bundle added an r-class matrix store.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto out = piet::dispatch("sktest mpg trunk", session);
    check(!anyError(out), "H/sktest: two-variable form runs");
    check(rv(session, "N") == 74.0, "H/sktest: r(N) is the LAST variable's N");
    check(close(rv(session, "p_skew"), 0.9115468643976649, 1e-9),
          "H/sktest: r(p_skew) is trunk's, not mpg's (StataBE oracle) -- got " +
              std::to_string(rv(session, "p_skew")));
    check(close(rv(session, "chi2"), 4.194021136220797, 1e-9), "H/sktest: r(chi2) oracle");
    const piet::eval::EvalMatrix* tbl = session.eval.findClassMatrix('r', "table");
    check(tbl != nullptr, "H/sktest: r(table) is posted");
    if (tbl != nullptr) {
      check(tbl->data.size() == 2 && tbl->data[0].size() == 5, "H/sktest: r(table) is 2 x 5");
      check(tbl->rownames.size() == 2 && tbl->rownames[0] == "mpg" && tbl->rownames[1] == "trunk",
            "H/sktest: r(table) rownames are the varlist, in order");
      check(tbl->colnames.size() == 5 && tbl->colnames[0] == "N" && tbl->colnames[4] == "p_chi2",
            "H/sktest: r(table) colnames are N p_skew p_kurt chi2 p_chi2");
    }
    const std::string t = displayText(out);
    check(t.find("Skewness and kurtosis tests for normality") != std::string::npos,
          "H/sktest: header matches Stata");
    check(t.find("Prob>chi2") != std::string::npos, "H/sktest: column head matches Stata");
  }

  // (d) sktest's COMPLEMENT form. The literal 2(1 - Phi|Z|) is right to about
  // 1.3e-11 here and 2*Phi(-|Z|) to about 1.1e-14, so only a pin at 1e-9 or
  // tighter can tell them apart -- which is why this arm is separate from the
  // display-level checks above.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    (void)piet::dispatch("sktest price", session);
    check(close(rv(session, "p_skew"), 1.9901828556006e-06, 1e-9),
          "H/sktest: r(p_skew) for price at rtol 1e-9 -- the literal 1-Phi form misses by "
          "1.3e-11 and trips here; got " + std::to_string(rv(session, "p_skew")));
  }

  // (e) sktest with too few observations is NOT a refusal. Measured on live
  // StataBE: n = 7 returns rc 0 with "." for all four statistics and continues.
  // Implementing the manual's Description prose as an error would abort whole
  // commands that Stata completes.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto out = piet::dispatch("sktest mpg if mpg > 40", session);
    check(!anyError(out), "H/sktest: a too-small sample is rc 0, not an error");
    check(std::isnan(rv(session, "chi2")), "H/sktest: ...and r(chi2) is missing rather than wrong");
    check(displayText(out).find(".") != std::string::npos,
          "H/sktest: ...and the row still prints");
  }

  // (f) centile -- all three centiles and both interval ends. lb_1 is the one
  // that caught the F_0 defect: the manual's boundary is F_{-1} = 0, and
  // zeroing F_0 instead leaves the t search right while making g wrong, moving
  // ONLY this bound (3292.8187 against Stata's 3291.232).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto out = piet::dispatch("centile price, centile(5 50 95)", session);
    check(!anyError(out), "H/centile: runs");
    check(rv(session, "N") == 74.0 && rv(session, "n_cent") == 3.0,
          "H/centile: r(N) and r(n_cent)");
    check(close(rv(session, "c_1"), 3727.75, 1e-9), "H/centile: r(c_1) oracle");
    check(close(rv(session, "c_2"), 5006.5, 1e-9), "H/centile: r(c_2) oracle");
    check(close(rv(session, "c_3"), 13498.0, 1e-9), "H/centile: r(c_3) oracle");
    check(close(rv(session, "lb_1"), 3291.231571513434, 1e-9),
          "H/centile: r(lb_1) == 3291.2316 -- the F_{-1}=0 boundary; F_0=0 gives 3292.8187 and "
          "leaves every OTHER bound correct. Got " + std::to_string(rv(session, "lb_1")));
    check(close(rv(session, "ub_1"), 3914.158992888472, 1e-9), "H/centile: r(ub_1) oracle");
    check(close(rv(session, "lb_2"), 4593.566284952724, 1e-9), "H/centile: r(lb_2) oracle");
    check(close(rv(session, "ub_3"), 15865.30130651407, 1e-9), "H/centile: r(ub_3) oracle");
  }

  // (g) centile's domain is the CLOSED interval [0,100] (StataBE: centile(0) and
  // centile(100) run), so q = 0 and q = 100 run (the sample minimum and
  // maximum, both interval ends held there) and only a value outside [0,100] is
  // refused, as r(125) (w2as_summary_stats_family.log:7871-7877, sx_centile_ref_100
  // `c(101)` -> "invalid numlist has elements outside of allowed range", 125). This
  // pin used to assert that 0 and 100 are refused, an rc the manual never stated.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    check(!anyError(piet::dispatch("centile price, centile(0)", session)) &&
              rv(session, "c_1") == 3291.0 && rv(session, "lb_1") == 3291.0 &&
              rv(session, "ub_1") == 3291.0,
          "H/centile: q = 0 runs and answers the sample minimum 3291 with both interval ends "
          "held there (StataBE: the centile() domain is [0,100]). Got c_1 " +
              std::to_string(rv(session, "c_1")));
    check(!anyError(piet::dispatch("centile price, centile(100)", session)) &&
              rv(session, "c_1") == 15906.0 && rv(session, "ub_1") == 15906.0,
          "H/centile: q = 100 runs and answers the sample maximum 15906. Got c_1 " +
              std::to_string(rv(session, "c_1")));
    auto out = piet::dispatch("centile price, centile(101)", session);
    check(anyError(out) && firstError(out).value("reason", std::string()) == "r(125)",
          "H/centile: q = 101 is refused as r(125) (w2as_summary_stats_family.log:7872-7877)");
  }

  // (h) areg. absorb() is REQUIRED -- Stata gives r(198), not a silent plain
  // regression, and the rc has to come from the error CODE (syntax_error) since
  // engine_error lands in the generic bucket and reads back as 1.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto bad = piet::dispatch("areg price mpg", session);
    check(anyError(bad), "H/areg: absorb() missing is an error");
    check(firstError(bad).value("code", std::string()) == "syntax_error",
          "H/areg: ...with the code that maps to r(198)");
    auto good = piet::dispatch("areg price mpg, absorb(rep78)", session);
    check(!anyError(good), "H/areg: with absorb() it estimates");
  }

  // (i) xi. The omitted level is the MINIMUM, the label is `v==k`, and `noomit`
  // prints NOTHING -- all three measured on live StataBE.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto out = piet::dispatch("xi i.foreign", session);
    check(!anyError(out), "H/xi: `xi i.foreign` runs");
    piet::Datastore* ds = session.datastore();
    check(ds != nullptr && ds->has("_Iforeign_1"),
          "H/xi: _Iforeign_1 exists (level 1 retained)");
    check(ds != nullptr && !ds->has("_Iforeign_0"),
          "H/xi: _Iforeign_0 does NOT exist -- the MINIMUM level is the omitted one");
    check(displayText(out).find("naturally coded; _Iforeign_0 omitted") != std::string::npos,
          "H/xi: the mapping line names the omitted column");
  }
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto out = piet::dispatch("xi, noomit i.foreign", session);
    piet::Datastore* ds = session.datastore();
    check(ds != nullptr && ds->has("_Iforeign_0") && ds->has("_Iforeign_1"),
          "H/xi: noomit keeps BOTH levels");
    check(displayText(out).empty(),
          "H/xi: noomit prints NO mapping line (measured on StataBE, not assumed)");
  }
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    (void)piet::dispatch("xi, prefix(_X) i.foreign", session);
    piet::Datastore* ds = session.datastore();
    check(ds != nullptr && ds->has("_Xforeign_1"), "H/xi: prefix() renames the generated columns");
    check(anyError(piet::dispatch("xi, prefix(_TOOLONG) i.foreign", session)),
          "H/xi: prefix() longer than 4 characters is refused");
  }
  // The prefix form: materialize AND rewrite. `_b[_Iforeign_1]` is only
  // answerable if the rewrite actually happened.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto out = piet::dispatch("xi: regress price i.foreign", session);
    check(!anyError(out), "H/xi: `xi: regress` runs");
    (void)piet::dispatch("scalar h_xib = _b[_Iforeign_1]", session);
    check(session.eval.scalars.get("h_xib", "") != "",
          "H/xi: the wrapped command estimated on the GENERATED column "
          "(_b[_Iforeign_1] resolves)");
  }

  // (j) E3. Registered so the NAME resolves, refused with a cause the route
  // drift guard can tell apart from an unrecognised token. Each name's own
  // per-command selftest file (see SELFTESTS.md) now owns its refusal arm (the
  // native task flips it); this leg stays as the cross-name count of eight.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    static const char* kE3[] = {"lasso logit price mpg",
                                "honestdid, pre(1) post(2)"};
    int refused = 0;
    for (const char* line : kE3) {
      auto blocks = piet::dispatch(line, session);
      const nlohmann::json e = firstError(blocks);
      if (e.value("reason", std::string()) == "refused:e3_deferred") ++refused;
    }
    // Nine since 2026-09-09: `oaxaca` left E3 with AER scope-2 B2 and
    // `teffects` with B2-4 (their legs live in testAerB2Oaxaca /
    // testAerB2Nnmatch). Both keep refusing the parts they do not implement,
    // but with a DIFFERENT named cause -- `refused:oaxaca_threefold_default`,
    // `refused:teffects_ra_unimplemented` -- deliberately not `e3_deferred`,
    // so the route-drift guard can tell "not routed" from "routed and declined".
    // Eight since X2-GLM (2026-10-12): `glm` left E3 for the native fitter
    // (estimation_glm.cpp; its legs live in testW2bGlm), and the forms it does
    // not implement refuse with their own named causes (refused:glm_*).
    // Five since 2026-10-21: `swindex`, `acreg` and `suest` left E3
    // (handlers_e3_{swindex,acreg,suest}.cpp; their legs live in testW2E3Swindex /
    // testW2E3Acreg / testW2E3Suest, oracles w2j_e3_swindex.log, w2j_e3_acreg.log,
    // w2j_e3_builtin.log S_*). Four since: `rlasso` left E3
    // (estimation/lasso_estimation.cpp runRlasso; testW2E3Lasso, oracle w2j_e3_rlasso.log).
    // Three since: `ritest` left E3 (estimation/ritest_inference.cpp; testW2E3Ritest,
    // oracle w2j_e3_ritest.log). Two since: `qreg2` left E3 (estimation/qreg2_estimation.cpp;
    // testW2E3Qreg2, oracle w2j_e3_qreg2.log).
    check(refused == 2,
          "H/E3: both refuse with reason `refused:e3_deferred` -- got " +
              std::to_string(refused));
  }

  // (k) The engine-backed four, now that H1 has landed. These same lines used to
  // assert a REFUSAL; the flip from 199 to 0 is the signal that the cobra half
  // exists, and the handlers still ask the engine first so an older cobra says
  // so by name.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto out = piet::dispatch("pca price mpg weight length turn", session);
    check(!anyError(out), "H/pca: runs against the H1 engine");
    // Oracle values, live StataBE on auto.dta.
    const double* cond = session.eval.eScalars.find("cond");
    check(cond != nullptr && close(*cond, 9.12262537900241, 1e-9),
          "H/pca: e(cond) == sqrt(lambda_1/lambda_p) == 9.1226 -- the RATIO is 83.22, and "
          "the plan had it as the ratio");
    const double* tr = session.eval.eScalars.find("trace");
    check(tr != nullptr && *tr == 5.0, "H/pca: e(trace) is the integer-snapped 5 for correlation");
    const double* rho = session.eval.eScalars.find("rho");
    check(rho != nullptr && close(*rho, 1.0, 1e-12), "H/pca: e(rho) == 1 when f == p");
    check(session.eval.findClassMatrix('e', "components") != nullptr,
          "H/pca: e(components) is posted (the manual lists it; the plan omitted it)");
    check(session.eval.findClassMatrix('e', "sds") != nullptr,
          "H/pca: e(sds) exists for a CORRELATION pca");
    check(session.eval.eStrings.get("Ctype", "") == "correlation", "H/pca: e(Ctype) macro");
    const piet::eval::EvalMatrix* Ev = session.eval.findClassMatrix('e', "Ev");
    check(Ev != nullptr && Ev->data.size() == 1 && Ev->data[0].size() == 5,
          "H/pca: e(Ev) is 1 x p");
    if (Ev != nullptr)
      check(close(Ev->data[0][0], 3.7762071, 1e-6), "H/pca: leading eigenvalue oracle");
  }
  {
    // Covariance PCA: e(sds) must be ABSENT, and the trace is snapped to an
    // integer 4.6e-8 away from the raw sum -- 46x outside a 1e-9 golden, which
    // is why a naive sum(C_ii) fails there.
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto out = piet::dispatch("pca price mpg weight length turn, covariance", session);
    check(!anyError(out), "H/pca: covariance form runs");
    check(session.eval.findClassMatrix('e', "sds") == nullptr,
          "H/pca: e(sds) is ABSENT after a covariance pca");
    const double* tr = session.eval.eScalars.find("trace");
    check(tr != nullptr && *tr == 9304104.0,
          "H/pca: e(trace) == 9304104 exactly (raw sum 9304104.43132173, chopped)");
  }
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto out = piet::dispatch("lowess mpg weight, gen(h_lw)", session);
    check(!anyError(out), "H/lowess: runs and generates");
    // lowess is NOT rclass and posts no r(bwidth): w2ae_lpoly_lowess lpl_lw_ctl
    // and lpl_lw_bw read r(bwidth) as missing (this arm used to pin the 0.8 the
    // handler posted, which no Stata run produces).
    check(session.eval.rValues.find("bwidth") == nullptr, "H/lowess: r(bwidth) is not posted");
    (void)piet::dispatch("summarize h_lw", session);
    check(close(rv(session, "mean"), 21.4854228927206, 1e-9),
          "H/lowess: the smoothed mean is the StataBE oracle -- got " +
              std::to_string(rv(session, "mean")));
  }
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    (void)piet::dispatch("lowess mpg weight, bwidth(2) gen(h_lw2) nograph", session);
    // lowess.ado substitutes 0.8 for a bwidth outside (0,1): the smooth is the
    // default one (same oracle mean as the default-bandwidth arm above).
    (void)piet::dispatch("summarize h_lw2", session);
    check(close(rv(session, "mean"), 21.4854228927206, 1e-9),
          "H/lowess: bwidth(2) smooths with the SUBSTITUTED 0.8, not the request");
  }
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    auto out = piet::dispatch("lpoly price mpg, nograph", session);
    check(!anyError(out), "H/lpoly: runs");
    check(close(rv(session, "bwidth"), 0.962518238058985, 1e-5),
          "H/lpoly: r(bwidth) matches the StataBE ROT within the chain's own limit -- got " +
              std::to_string(rv(session, "bwidth")));
    auto out2 = piet::dispatch("lpoly price mpg, nograph kernel(epan2)", session);
    check(!anyError(out2), "H/lpoly: epan2 runs");
    check(close(rv(session, "bwidth"), 2.15249845166361, 1e-5),
          "H/lpoly: epan2 r(bwidth) oracle (its constant was MEASURED, not analytic)");
  }
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);
    check(anyError(piet::dispatch("tsfilter bw c = price", session)),
          "H/tsfilter: a non-hp filter word is an unknown SUBcommand, not an unknown command");
    // tsfilter without tsset is r(111), the same as Stata.
    check(anyError(piet::dispatch("tsfilter hp c = price", session)),
          "H/tsfilter: refuses without a tsset");
  }

  fs::remove_all(workdir, ec);
}


// --- P3 weights wave: registry.cpp's WeightPolicy grew from a 2-value
// (None/FweightOnly) blanket rule into a genuine per-command allow-set
// (AweightFweight on summarize, PweightOnly on the new mean/proportion rows,
// AllWeights on regress/reghdfe/feols) — the engine side (cobra) is being
// taught real weighted estimation in a parallel stint, so PIET's job here is
// to stop REJECTING the newly-legal combinations and forward them, without
// loosening anything that's still genuinely unsupported.
//
// NOTE: cobra itself may not have landed every new flag yet at the time this
// runs (concurrent engine work) — so the dispatch-level legs below only
// assert ROUTING (the tokenizer let the qualifier through and the real
// handler ran, same "not unsupported_command/unsupported_option" bar
// testRegistryRoutes() uses), never a specific numeric result. The
// TOKENIZER-level legs (no cobra invocation at all) are the ones asserting
// the actual policy — parses/doesn't-throw, or throws with the right
// command-aware message.
void testP3WeightsWave() {
  std::printf("[selftest] --- P3 weights wave: registry-driven weight policy expansion ---\n");

  using piet::eval::tokenize;
  using piet::eval::TokenizerError;

  // --- registry sanity: the exact rows this wave was supposed to move,
  // and nothing else — a drift guard mirroring testBS096's byIllegalInStata
  // leg above. ------------------------------------------------------------
  {
    using piet::registry::WeightPolicy;
    auto policyOf = [](const std::string& canon) -> std::optional<WeightPolicy> {
      const auto* spec = piet::registry::findByName(canon);
      return spec ? std::optional<WeightPolicy>(spec->weight) : std::nullopt;
    };
    check(policyOf("regress") == WeightPolicy::AllWeights, "P3: regress row is AllWeights");
    check(policyOf("reghdfe") == WeightPolicy::AllWeights, "P3: reghdfe row is AllWeights");
    // summarize serves [iw=] since the residual sweep (help summarize; w2aj
    // qf_sw_iw_tight), pweight still Stata's r(101) via kStataLegalWeights; mean
    // takes every type (help mean; the engine serves pw and aw, w2aj
    // qf_mn_aw_sp, and refuses fw/iw by name).
    check(policyOf("summarize") == WeightPolicy::AllWeights, "P3: summarize row is AllWeights");
    check(policyOf("mean") == WeightPolicy::AllWeights, "P3: mean row is AllWeights");
    // proportion takes fweights and iweights too since BUG-1541; its aweight is
    // Stata's r(101) via kStataLegalWeights (StataBE 19.5, scratch-laneA2/agents/wml/x1.log).
    check(policyOf("proportion") == WeightPolicy::AllWeights, "P3: proportion row is AllWeights (BUG-1541)");
    // Regression guard: the OLD FweightOnly allow-list members that did NOT
    // move this wave stay exactly where they were.
    check(policyOf("tab2") == WeightPolicy::FweightOnly, "P3: tab2 row still FweightOnly (unchanged)");
    // collapse moved later: it takes aw, fw, iw and pw weights (w2au_collapse_family.log cz_c_w_*).
    check(policyOf("collapse") == WeightPolicy::AllWeights, "P3: collapse row is AllWeights");
    // Rows LATER waves moved, each to the set Stata documents: tabulate/tab1 serve
    // [aw=]/[iw=] since the qualifier wave (f8413c27; help tabulate_oneway,
    // w2aj_qualifiers.log:2041 qf_tb_aw_sp, :2065 qf_tb_iw_sp), with pweight refused
    // as Stata's r(101) by the tokenizer's kStataLegalWeights row; corr/pwcorr serve
    // [aw=]/[fw=] since BUG-1014 (help correlate; w2aj_qualifiers.log:1869 qf_co_aw).
    check(policyOf("tabulate") == WeightPolicy::AllWeights, "P3: tabulate row is AllWeights (qualifier wave)");
    check(policyOf("tab1") == WeightPolicy::AllWeights, "P3: tab1 row is AllWeights (qualifier wave)");
    check(policyOf("corr") == WeightPolicy::AweightFweight, "P3: corr row is AweightFweight (BUG-1014)");
    // A command no wave touched (still None) — spot-check one. (tobit was this
    // check's command until BUG-1544 served its four weight types.)
    check(policyOf("reg3") == WeightPolicy::None, "P3: reg3 row still None (unchanged)");
  }

  // --- tokenizer level: every newly-allowed (command, weight-type) pair
  // parses without throwing, and pc.weightType/weightVar carry the exact
  // spelling the user typed. -----------------------------------------------
  {
    static const std::vector<std::pair<std::string, std::string>> kNewlyLegal = {
        {"regress y x1 [aw=w]", "aw"},       {"regress y x1 [aweight=w]", "aweight"},
        {"regress y x1 [fw=w]", "fw"},       {"regress y x1 [fweight=w]", "fweight"},
        {"regress y x1 [iw=w]", "iw"},       {"regress y x1 [iweight=w]", "iweight"},
        {"regress y x1 [pw=w]", "pw"},       {"regress y x1 [pweight=w]", "pweight"},
        {"reghdfe y x1 [pw=w]", "pw"},       {"summarize wage [aw=w]", "aw"},
        {"summarize wage [aweight=w]", "aweight"}, {"mean wage [pw=w]", "pw"},
        {"mean wage [pweight=w]", "pweight"}, {"proportion state [pw=w]", "pw"},
        {"proportion state [pweight=w]", "pweight"}, {"proportion state [fw=w]", "fw"},
        {"proportion state [iw=w]", "iw"},
    };
    for (const auto& [line, expectType] : kNewlyLegal) {
      try {
        auto pc = tokenize(line);
        check(pc.weightType && *pc.weightType == expectType && pc.weightVar && *pc.weightVar == "w",
              "P3 weight: newly-legal `" + line + "` parses (weightType/weightVar captured)");
      } catch (const TokenizerError& e) {
        check(false, "P3 weight: newly-legal `" + line +
                         "` must NOT throw — got: " + e.block.value("message", std::string()));
      }
    }
  }

  // --- tokenizer level: pairs that are STILL illegal after this wave reject
  // with a command-aware message (a narrower allow-set does not become a
  // blanket one). -----------------------------------------------------------
  {
    // proportion: fw/iw are legal since BUG-1541 (above); its aweight is Stata's
    // own r(101) `aweights not allowed` (StataBE 19.5, scratch-laneA2/agents/wml/
    // x1.log). (mean admits every type since the residual sweep, see the row check.)
    {
      const std::string line = "proportion wage [aw=w]";
      try {
        tokenize(line);
        check(false, "P3 weight: `" + line + "` must still throw (Stata's r(101))");
      } catch (const TokenizerError& e) {
        check(e.block.value("code", std::string()) == "engine_error" &&
                  e.block.value("reason", std::string()) == "r(101)" &&
                  e.block.value("message", std::string()) == "aweights not allowed",
              "P3 weight: `" + line + "` is Stata's r(101) `aweights not allowed` — got: " +
                  e.block.dump());
      }
    }
    // summarize: pw is illegal in Stata itself (r(101) "pweights not allowed";
    // oracle w2_pweight.log:764-768 S1_summarize_pw_refused, aer_b4
    // esttab-beta.log:338). iw is served now (QF-SUM-IW, w2aj_qualifiers.log:826).
    for (const std::string wt : {"pw"}) {
      std::string line = "summarize wage [" + wt + "=w]";
      try {
        tokenize(line);
        check(false, "P3 weight: `" + line + "` must still throw (AweightFweight)");
      } catch (const TokenizerError& e) {
        std::string msg = e.block.value("message", std::string());
        if (wt == "pw") {
          check(e.block.value("code", std::string()) == "engine_error" &&
                    e.block.value("reason", std::string()) == "r(101)" &&
                    msg == "pweights not allowed",
                "P3 weight: `" + line + "` is Stata's r(101) `pweights not allowed` — got: " +
                    e.block.dump());
        } else {
          check(msg.find("summarize") != std::string::npos && msg.find("analytic") != std::string::npos,
                "P3 weight: `" + line + "` names the command AND clarifies analytic-or-frequency — got: " + msg);
        }
      }
    }
  }

  // --- full dispatch: newly-legal weights REACH the real handler (routing
  // only — cobra's own new flags may still be landing concurrently, so this
  // never asserts a specific numeric result, only "not a qualifier-gap
  // reject"). ----------------------------------------------------------------
  {
    namespace fs = std::filesystem;
    std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_p3_weights").u8string();
    std::error_code ec;
    fs::remove_all(workdir, ec);
    fs::create_directories(workdir, ec);

    auto routedOk = [](const nlohmann::ordered_json& blocks) {
      for (const auto& b : blocks) {
        if (!b.is_object() || b.value("type", std::string()) != "error") continue;
        std::string code = b.value("code", std::string());
        if (code == "unsupported_command" || code == "unsupported_option") return false;
      }
      return true;
    };

    {
      piet::SessionState session(workdir);
      (void)piet::dispatch("sysuse fatalities, clear", session);
      auto blocks = piet::dispatch("regress fatal beertax [aw=beertax]", session);
      check(routedOk(blocks), "P3 weight: `regress ... [aw=]` reaches the real handler (not a qualifier reject)");
    }
    {
      piet::SessionState session(workdir);
      (void)piet::dispatch("sysuse fatalities, clear", session);
      auto blocks = piet::dispatch("mean beertax [pw=fatal]", session);
      check(routedOk(blocks), "P3 weight: `mean ... [pw=]` reaches the real handler (not a qualifier reject)");
    }
    {
      piet::SessionState session(workdir);
      (void)piet::dispatch("sysuse fatalities, clear", session);
      auto blocks = piet::dispatch("proportion beertax [pw=fatal]", session);
      check(routedOk(blocks),
            "P3 weight: `proportion ... [pw=]` reaches the real handler (not a qualifier reject)");
    }
    {
      // Bare `mean`/`proportion` (registry drift guard's own probe shape,
      // testRegistryRoutes above) — must land on the handler's own
      // "requires at least one variable" syntax_error, never fall through
      // to unsupported_command.
      piet::SessionState session(workdir);
      (void)piet::dispatch("sysuse fatalities, clear", session);
      auto blocks = piet::dispatch("mean", session);
      bool sawSyntaxError = false;
      for (const auto& b : blocks) {
        if (b.is_object() && b.value("type", std::string()) == "error" &&
            b.value("code", std::string()) == "syntax_error") {
          sawSyntaxError = true;
        }
      }
      check(sawSyntaxError, "P3 weight: bare `mean` (no varlist) is an honest syntax_error, not a crash");
    }

    fs::remove_all(workdir, ec);
  }
}


void testC1BS022LogisticOddsRatio() {
  std::printf("[selftest] --- C1/BS-022 logistic reports odds ratios, not log-odds ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c1_bs022").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen high = (beertax > .5)", session);
  auto blocks = piet::dispatch("logistic high income", session);

  const nlohmann::ordered_json* est = nullptr;
  for (const auto& b : blocks) {
    if (b.is_object() && b.value("type", std::string()) == "estimation") est = &b;
  }
  check(est != nullptr, "C1/BS-022 setup: `logistic high income` returns an estimation block");
  if (est != nullptr) {
    check(est->value("estimator", std::string()) == "logistic",
          "C1/BS-022: the block's estimator is \"logistic\", not \"logit\" (previously identical "
          "to logit's block)");
    // Oracle (real Stata): income row OR = .9995511; _cons row OR = 195.857.
    bool sawIncomeOr = false, sawConsOr = false;
    if (est->contains("coeftable") && (*est)["coeftable"].is_array()) {
      for (const auto& row : (*est)["coeftable"]) {
        std::string name = row.value("name", std::string());
        if (name == "income" && row.contains("b") && row["b"].is_number()) {
          double b = row["b"].get<double>();
          check(std::abs(b - 0.9995511) < 1e-4,
                "C1/BS-022: income's OR ~= .9995511 (oracle), got " + std::to_string(b));
          sawIncomeOr = true;
        }
        if (name == "(intercept)" && row.contains("b") && row["b"].is_number()) {
          double b = row["b"].get<double>();
          check(std::abs(b - 195.857) < 0.5,
                "C1/BS-022: _cons's OR ~= 195.857 (oracle), got " + std::to_string(b));
          sawConsOr = true;
        }
      }
    }
    check(sawIncomeOr, "C1/BS-022: coeftable has an income row with a numeric OR");
    check(sawConsOr, "C1/BS-022: coeftable has an intercept row with a numeric OR");
  }

  // Regression guard: `logit` itself is untouched (log-odds, not OR).
  auto blocksLogit = piet::dispatch("logit high income", session);
  const nlohmann::ordered_json* estLogit = nullptr;
  for (const auto& b : blocksLogit) {
    if (b.is_object() && b.value("type", std::string()) == "estimation") estLogit = &b;
  }
  check(estLogit != nullptr && estLogit->value("estimator", std::string()) == "logit",
        "C1/BS-022 regression guard: `logit` still reports estimator \"logit\" (log-odds)");
  if (estLogit != nullptr && estLogit->contains("coeftable") && (*estLogit)["coeftable"].is_array()) {
    for (const auto& row : (*estLogit)["coeftable"]) {
      if (row.value("name", std::string()) == "income" && row.contains("b") && row["b"].is_number()) {
        check(row["b"].get<double>() < 0,
              "C1/BS-022 regression guard: `logit`'s income b is still the (negative) log-odds "
              "coefficient, not exponentiated");
        check(row.contains("p") && row["p"].is_number(),
              "C1/BS-022: logit coeftable p is backfilled (not left null)");
        check(row.contains("ci_lo") && row["ci_lo"].is_number(),
              "C1/BS-022: logit coeftable ci_lo is backfilled (not left null)");
        check(row.contains("ci_hi") && row["ci_hi"].is_number(),
              "C1/BS-022: logit coeftable ci_hi is backfilled (not left null)");
      }
    }
  }

  // Complete separation is an honest Stata-parity reject, while the
  // income-based control above remains a normal estimation table.
  auto separated = piet::dispatch("capture noisily logit high beertax", session);
  bool sawPerfectReject = false;
  for (const auto& b : separated) {
    if (b.is_object() && b.value("type", std::string()) == "error" &&
        b.value("message", std::string()).find("predicts data perfectly") != std::string::npos) {
      sawPerfectReject = true;
    }
  }
  check(sawPerfectReject,
        "F2: separated logit returns a visible captured error block naming perfect prediction");
  check(session.eval.locals.get("_rc", "") == "2000",
        "F2: capture maps the perfect-prediction reject to Stata rc 2000");

  fs::remove_all(workdir, ec);
}


void testD3BS076Drdid() {
  std::printf("[selftest] --- D3h/BS-076 drdid (classic 2x2 doubly-robust DiD) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d3_bs076").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  // Oracle: tests/validate/drdid2x2.{do,log} (StataBE 19.5) — `use
  // drdid2x2.dta, clear` + `drdid Y x1 x2, ivar(id) time(t)
  // treatment(treat) dripw`. ATT=2.05610834185971 SE=.124005088944043.
  // Cross-worktree fixture path (same sharing convention as csdid's own
  // Stata-captured fixtures) — skip gracefully (not a hard failure) if unavailable in this
  // particular environment.
  (void)piet::dispatch(
      ("capture use \"" + selftestValidateDir() + "/drdid2x2.dta\", clear").c_str(), session);
  if (session.eval.locals.get("_rc", "") != "0") {
    skip("D3h/BS-076 drdid",
         "could not load the drdid2x2.dta cross-worktree fixture in this environment (see "
         "tests/validate/drdid2x2.log for the pinned oracle values)");
    fs::remove_all(workdir, ec);
    return;
  }

  {
    auto blocks = piet::dispatch("drdid Y x1 x2, ivar(id) time(t) treatment(treat) dripw", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr,
          "D3h: `drdid Y x1 x2, ivar(id) time(t) treatment(treat) dripw` returns an estimation "
          "block");
    if (est != nullptr) {
      check(est->value("estimator", std::string()) == "drdid", "D3h: estimator label is \"drdid\"");
      // drdid-ematrix-post: the row is Stata's e(b) name `ATET:r1vs0.<treat>`
      // (matches drdid's behaviour, identified from its ado;
      // w2ag_csdid_drdid.log:1810 ag_drdid), not the bare `treat`.
      auto b = coefFieldD1(*est, "ATET:r1vs0.treat", "b");
      check(b.has_value() && std::abs(*b - 2.05610834185971) < 1e-6,
            "D3h oracle: ATT ~= 2.05610834185971, got " +
                (b.has_value() ? std::to_string(*b) : "MISSING"));
      auto se = coefFieldD1(*est, "ATET:r1vs0.treat", "se");
      check(se.has_value() && std::abs(*se - 0.124005088944043) < 1e-6,
            "D3h oracle: SE ~= .124005088944043, got " +
                (se.has_value() ? std::to_string(*se) : "MISSING"));
    }
  }

  // time() must have exactly 2 distinct values (the "2x2" restriction) —
  // honest reject otherwise (real drdid.ado shares this restriction). `id`
  // has 300 distinct values, cheaply triggering the same reject path with
  // no new fixture needed.
  (void)piet::dispatch("capture drdid Y x1 x2, ivar(id) time(id) treatment(treat) dripw", session);
  check(session.eval.locals.get("_rc", "") != "0",
        "D3/BS-076: drdid honestly rejects when time() doesn't have exactly 2 distinct values");

  // dripw is required explicitly (no oracle for the bare/other-method forms).
  (void)piet::dispatch("capture drdid Y x1 x2, ivar(id) time(t) treatment(treat)", session);
  check(session.eval.locals.get("_rc", "") != "0",
        "D3/BS-076: drdid without the explicit `dripw` option honestly rejects");

  fs::remove_all(workdir, ec);
}


void testG_BUG370ResidualWireLock() {
  std::printf("[selftest] --- BUG-370 lock: TS operators in a STRING-layer `if`, plus "
              "xtile / ipolate / tssmooth ma ---\n");
  namespace fs = std::filesystem;
  auto freshSession = [](const char* tag) {
    std::string wd = (fs::temp_directory_path() / (std::string("piet_native_selftest_") + tag)).u8string();
    std::error_code ec2;
    fs::remove_all(wd, ec2);
    fs::create_directories(wd, ec2);
    return wd;
  };
  auto displayText = [](const nlohmann::ordered_json& blocks) {
    const auto* d = firstBlockOfTypeD1(blocks, "display");
    return d == nullptr ? std::string() : d->value("text", std::string());
  };

  // ---- (a)-(d): the string-layer evaluator ---------------------------------
  {
    std::string workdir = freshSession("bug370a");
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("gen t = _n", session);
    (void)piet::dispatch("tsset t", session);

    // (a) The reported defect. A STRING replace whose `if` names a TS operator:
    // one half of the predicate belongs to the row-wise interpreter, the other to
    // the engine's parser. It used to die "if condition evaluation failed at obs 1".
    // The check is PARITY, not absence-of-error: the rows the string command
    // touched must be exactly the rows the numeric predicate selects.
    {
      auto blocks = piet::dispatch("gen s2 = \"x\"", session);
      check(firstBlockOfTypeD1(blocks, "error") == nullptr, "BUG-370 setup: `gen s2 = \"x\"`");
    }
    {
      auto blocks = piet::dispatch("replace s2 = \"y\" if L.beertax > 0.5", session);
      check(firstBlockOfTypeD1(blocks, "error") == nullptr,
            "BUG-370(a): `replace s2 = \"y\" if L.beertax>0.5` does not error");
    }
    const std::string touched = displayText(piet::dispatch("count if s2==\"y\"", session));
    const std::string expect = displayText(piet::dispatch("count if L.beertax > 0.5", session));
    check(!touched.empty() && touched == expect,
          "BUG-370(a): the string replace touched exactly the operator-selected rows (" +
              touched + " vs " + expect + ")");

    // (b) The temps the fix materialises must never be visible. They go into a
    // scratch frame that is never adopted as the head, so a `__piet_tsif` column
    // reaching a varlist would mean that design broke.
    {
      auto blocks = piet::dispatch("describe", session);
      const auto* de = firstBlockOfTypeD1(blocks, "describe");
      bool leaked = false;
      if (de != nullptr && de->contains("variables")) {
        for (const auto& v : (*de)["variables"])
          if (v.value("name", std::string()).rfind("__piet_tsif", 0) == 0) leaked = true;
      }
      check(de != nullptr && !leaked,
            "BUG-370(b): no `__piet_tsif` scratch column reaches the user's varlist");
    }
    // e(sample) T2 (TASKS.md T2 item 1), the sibling: the estimation-sample mask
    // IS adopted as part of the working frame, and stays out of every listing
    // while `count if e(sample)` reads it (w2_esample.log B1 count == e(N)).
    {
      (void)piet::dispatch("quietly regress beertax spirits", session);
      auto blocks = piet::dispatch("describe", session);
      const auto* de = firstBlockOfTypeD1(blocks, "describe");
      bool leaked = false;
      if (de != nullptr && de->contains("variables")) {
        for (const auto& v : (*de)["variables"])
          if (v.value("name", std::string()).rfind("__piet_", 0) == 0) leaked = true;
      }
      const std::string dsText = nlohmann::ordered_json(piet::dispatch("ds", session)).dump();
      (void)piet::dispatch("quietly count if e(sample)", session);
      const std::string cnt = displayText(piet::dispatch("display r(N)", session));
      const std::string en = displayText(piet::dispatch("display e(N)", session));
      check(de != nullptr && !leaked && dsText.find("__piet_") == std::string::npos &&
                !cnt.empty() && cnt == en,
            "e(sample) T2: after a fit no `__piet_` column reaches describe/ds, and count if "
            "e(sample) answers e(N) (" + cnt + " vs " + en + ")");
    }

    // (d) A MIXED predicate — a string function AND an operator in one condition.
    // Neither layer can evaluate this alone, which is the whole point of the fix.
    {
      auto blocks = piet::dispatch("gen s3 = \"hit\" if strlen(s2)>0 & L.beertax > 0.5", session);
      check(firstBlockOfTypeD1(blocks, "error") == nullptr,
            "BUG-370(d): a mixed `strlen(s)>0 & L.x>0.5` predicate evaluates");
    }
    check(displayText(piet::dispatch("count if s3==\"hit\"", session)) == expect,
          "BUG-370(d): the mixed predicate selects the same rows as its operator half");
  }

  // (c) keep-if with a mixed predicate, against the pre-materialised twin. Two
  // separate sessions so neither can contaminate the other.
  {
    std::string wdA = freshSession("bug370c1");
    piet::SessionState sA(wdA);
    (void)piet::dispatch("sysuse fatalities, clear", sA);
    (void)piet::dispatch("gen t = _n", sA);
    (void)piet::dispatch("tsset t", sA);
    (void)piet::dispatch("keep if breath==\"no\" & L.beertax > 0.5", sA);
    const std::string got = displayText(piet::dispatch("count", sA));

    std::string wdB = freshSession("bug370c2");
    piet::SessionState sB(wdB);
    (void)piet::dispatch("sysuse fatalities, clear", sB);
    (void)piet::dispatch("gen t = _n", sB);
    (void)piet::dispatch("tsset t", sB);
    (void)piet::dispatch("gen lb = L.beertax", sB);
    (void)piet::dispatch("keep if breath==\"no\" & lb > 0.5", sB);
    const std::string want = displayText(piet::dispatch("count", sB));
    check(!got.empty() && got == want,
          "BUG-370(c): `keep if` with a mixed string+operator predicate keeps the same rows as "
          "the pre-materialised twin (" + got + " vs " + want + ")");
  }

  // ---- (e)-(g): the three commands ----------------------------------------
  {
    std::string workdir = freshSession("bug370e");
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 8", session);
    (void)piet::dispatch("gen x = _n", session);
    (void)piet::dispatch("xtile q = x, nq(4)", session);
    (void)piet::dispatch("xtile h = x", session);
    // (e) THE trap. Stata's xtile defaults to nquantiles(2); this engine's flag
    // defaults to 4. If the handler ever stops sending an explicit nq, `h` becomes
    // four groups of two and this check is the only thing that says so.
    check(displayText(piet::dispatch("count if q==1", session)) == "2",
          "BUG-370(e): `xtile q = x, nq(4)` puts 2 of 8 rows in the first quartile");
    check(displayText(piet::dispatch("count if h==2", session)) == "4",
          "BUG-370(e): bare `xtile h = x` uses Stata's DEFAULT of 2 quantiles (4 of 8 rows in "
          "group 2), not the engine flag's default of 4");
  }
  {
    // (f) ipolate must reconstruct the exact linear values it interpolates over.
    std::string workdir = freshSession("bug370f");
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 5", session);
    (void)piet::dispatch("gen x = _n", session);
    (void)piet::dispatch("gen y = x", session);
    (void)piet::dispatch("replace y = . if x==2 | x==4", session);
    auto blocks = piet::dispatch("ipolate y x, gen(yi)", session);
    check(firstBlockOfTypeD1(blocks, "error") == nullptr, "BUG-370(f): `ipolate y x, gen(yi)` runs");
    check(displayText(piet::dispatch("count if yi != x", session)) == "0",
          "BUG-370(f): ipolate recovers the two punched-out values EXACTLY (0 rows differ)");
  }
  {
    // (g) tssmooth ma under a plain `tsset` — the engine used to demand xtset, so
    // this whole case was unreachable. Value-checked, not just error-checked:
    // {1,2,3,4,5} with window(1 1 1) smooths to {1.5,2,3,4,4.5}, mean 3.
    std::string workdir = freshSession("bug370g");
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 5", session);
    (void)piet::dispatch("gen t = _n", session);
    (void)piet::dispatch("gen y = t", session);
    (void)piet::dispatch("tsset t", session);
    auto blocks = piet::dispatch("tssmooth ma m = y, window(1 1 1)", session);
    check(firstBlockOfTypeD1(blocks, "error") == nullptr,
          "BUG-370(g): `tssmooth ma` works after a plain `tsset` (no panel)");
    auto sm = piet::dispatch("summarize m", session);
    const auto* smb = firstBlockOfTypeD1(sm, "summarize");
    double mean = -1;
    if (smb != nullptr && smb->contains("stats") && (*smb)["stats"].contains("m"))
      mean = (*smb)["stats"]["m"].value("mean", -1.0);
    check(std::fabs(mean - 3.0) < 1e-12,
          "BUG-370(g): the smoothed series is {1.5,2,3,4,4.5} — mean 3, edges SHORTENED rather "
          "than missing (got " + std::to_string(mean) + ")");
    // Honest refusals, so a later relaxation cannot silently guess.
    check(firstBlockOfTypeD1(piet::dispatch("tssmooth exponential e = y, window(1 1 1)", session),
                             "error") != nullptr,
          "BUG-370(g): `tssmooth exponential` is refused by name");
    // BUG-378 SUPERSEDES the old check here. This used to assert that
    // `window(#l 0 #f)` is REFUSED, on the belief that the engine must always
    // include the focal observation. The 2026-08-31 StataBE capture measured the
    // opposite: `window(1)` in real Stata is (1/1)*[x(t-1) + 0*x(t)], i.e. centre
    // weight 0 is not only legal, it is the DEFAULT when #c is omitted. The
    // engine can express it now, so the assertion is inverted rather than deleted
    // — a refusal that became a feature has to be re-pinned as a feature.
    {
      auto b = piet::dispatch("tssmooth ma m2 = y, window(1 0 1)", session);
      check(firstBlockOfTypeD1(b, "error") == nullptr,
            "BUG-378: `window(#l 0 #f)` is ACCEPTED — centre weight 0 excludes the focal "
            "observation, which is what real Stata does (was refused before the capture)");
    }
    // `xtile ... if` was refused here while the engine could only cut over every
    // row. BUG-1015 (99968638) derives the cutpoints from the restricted sample, as
    // Stata's xtile does (pctile over the if-sample rows only):
    // pins_oracle_a.log:22-30 (rc 0, non-missing exactly on the if-sample) and
    // w2as_summary_stats_family.log:8441-8482 (sx_xtile_n_if). y = 1..5, `if y>2`,
    // nq(2): the median of {3,4,5} is 4, so {3,4} -> 1 and {5} -> 2. Cutpoints over
    // ALL rows (median 3) would put only y=3 in group 1.
    check(firstBlockOfTypeD1(piet::dispatch("xtile bad = y if y>2, nq(2)", session), "error") ==
                  nullptr &&
              displayText(piet::dispatch("count if bad==1", session)) == "2" &&
              displayText(piet::dispatch("count if bad==2", session)) == "1" &&
              displayText(piet::dispatch("count if missing(bad)", session)) == "2",
          "BUG-370(e)/BUG-1015: `xtile bad = y if y>2, nq(2)` cuts at the RESTRICTED sample's "
          "median: 2 rows in group 1, 1 in group 2, the 2 excluded rows missing");
    // xtile's weights are aweight, fweight and pweight, checked
    // by `mark` (help mark: a negative weight r(402), a non-integer fweight
    // r(401)); [iw=] is Stata's r(101). The qualifier wave (f8413c27) served all of
    // this and the branch integration had dropped [pw=] to an engine-gap refusal.
    {
      auto reasonOf = [&](const std::string& cmd) {
        const auto blocks = piet::dispatch(cmd, session);  // the pointer below points into it
        const auto* e = firstBlockOfTypeD1(blocks, "error");
        return e == nullptr ? std::string("rc0") : e->value("reason", std::string("?"));
      };
      (void)piet::dispatch("gen double xw1 = 1", session);
      const std::string pw = reasonOf("xtile xqpw = y [pw=xw1], nq(2)");
      const std::string pwN = displayText(piet::dispatch("count if xqpw==1", session));
      (void)piet::dispatch("gen double xwneg = 1", session);
      (void)piet::dispatch("replace xwneg = -1 in 1", session);
      const std::string neg = reasonOf("xtile xqneg = y [aw=xwneg] in 2/5, nq(2)");
      (void)piet::dispatch("gen double xwfr = 1.5", session);
      const std::string fr = reasonOf("xtile xqfr = y [fw=xwfr], nq(2)");
      const std::string iw = reasonOf("xtile xqiw = y [iw=xw1], nq(2)");
      check(pw == "rc0" && pwN == "3" && neg == "r(402)" && fr == "r(401)" && iw == "r(101)",
            "BUG-1015 xtile weights: [pw=] runs (3 of 5 rows at or below the median), a "
            "negative weight anywhere is r(402) even outside `in`, a non-integer [fw=] r(401), "
            "[iw=] r(101). Got " + pw + " " + pwN + " / " + neg + " / " + fr + " / " + iw);
    }
  }
}


// --- BUG-112(1) PIET-half: `generate` over an existing variable must be a
// NAMED reject (Stata r(110)), not a silent overwrite -------------------------
// Pre-fix, live-measured: PIET guarded exactly one of the verb's four sinks
// (strmutate::runStringGen); the three numeric ones silently replaced. Every
// leg below therefore asserts BOTH halves — the diagnostic AND that the column
// is untouched — because the defect's signature was a moved value with NO
// message, which a message-only assertion cannot distinguish from a fix that
// rejects and mutates anyway.
void testBUG112PietGenGuard() {
  std::printf("[selftest] --- BUG-112(1) PIET-half: generate exists-guard (was silent REPLACE) ---\n");
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_bug112_genguard").u8string();
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
  // nullopt = the variable was not in summarize's output at all, which is a
  // DIFFERENT failure from "wrong mean" and must not silently compare equal.
  auto meanOf = [](const nlohmann::ordered_json& blocks,
                   const std::string& var) -> std::optional<double> {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "summarize" && b.contains("stats") &&
          b["stats"].contains(var) && b["stats"][var].contains("mean")) {
        return b["stats"][var]["mean"].get<double>();
      }
    }
    return std::nullopt;
  };

  // --- sink 3 (handlers::runGen, the plain numeric route) + the `replace`
  // no-regression bar that decides where the guard may live at all.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 5", session);
    (void)piet::dispatch("gen x = 1", session);
    std::string rejectMsg = firstError(piet::dispatch("gen x = 2", session));
    check(has(rejectMsg, "variable x already defined"),
          "BUG-112(1) gen: existing name is a named reject (was a silent REPLACE)");
    check(meanOf(piet::dispatch("summarize x", session), "x") == std::optional<double>(1.0),
          "BUG-112(1) gen: the rejected gen left the column untouched (mean still 1)");
    // r(110) parity: coarseRc keys off the "already defined" wording, so the
    // spelling above is load-bearing — assert the rc, not just the text.
    (void)piet::dispatch("capture gen x = 2", session);
    std::string rcText;
    for (const auto& b : piet::dispatch("display _rc", session)) {
      if (b.is_object() && b.value("type", std::string()) != "echo") {
        rcText += b.value("text", std::string());
      }
    }
    check(has(rcText, "110"),
          "BUG-112(1) gen: a capture-wrapped reject sets _rc=110 (Stata parity)");
    // `replace` must keep overwriting — it builds the IDENTICAL `cobra eval
    // --gen` call, which is why the guard is at the verb and not the handler.
    check(firstError(piet::dispatch("replace x = 5", session)).empty(),
          "BUG-112(1) replace: still overwrites an existing variable (no regression)");
    check(meanOf(piet::dispatch("summarize x", session), "x") == std::optional<double>(5.0),
          "BUG-112(1) replace: the overwrite actually landed (mean 5)");
    check(firstError(piet::dispatch("gen xnew = 7", session)).empty(),
          "BUG-112(1) gen: a fresh name still creates (the guard is not blanket)");
  }

  // --- sink 2 (egen::runBysortGen with an empty by-list): the standalone
  // `_n`/`_N` route and the `in`-range route both land here, and neither is
  // reachable through sink 3's assertions above.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 5", session);
    (void)piet::dispatch("gen z = _n", session);
    check(has(firstError(piet::dispatch("gen z = _n", session)), "variable z already defined"),
          "BUG-112(1) gen `_n` route: existing name is a named reject");
    check(meanOf(piet::dispatch("summarize z", session), "z") == std::optional<double>(3.0),
          "BUG-112(1) gen `_n` route: the rejected gen left the column untouched (mean still 3)");
    (void)piet::dispatch("gen w = 3 in 1/3", session);
    check(has(firstError(piet::dispatch("gen w = 88 in 1/3", session)),
              "variable w already defined"),
          "BUG-112(1) gen `in`-range route: existing name is a named reject");
    check(meanOf(piet::dispatch("summarize w", session), "w") == std::optional<double>(3.0),
          "BUG-112(1) gen `in`-range route: the rejected gen left the column untouched (mean 3)");
  }

  // --- the `bysort v:` prefixed route (a second dispatch site, its own guard).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 5", session);
    (void)piet::dispatch("gen g = 1", session);
    (void)piet::dispatch("bysort g: gen y = _n", session);
    check(has(firstError(piet::dispatch("bysort g: gen y = 99", session)),
              "variable y already defined"),
          "BUG-112(1) bysort gen: existing name is a named reject (was a silent REPLACE)");
    check(meanOf(piet::dispatch("summarize y", session), "y") == std::optional<double>(3.0),
          "BUG-112(1) bysort gen: the rejected gen left the column untouched (mean still 3)");
    check(firstError(piet::dispatch("bysort g: replace y = 99", session)).empty(),
          "BUG-112(1) bysort replace: still overwrites (no regression)");
  }

  // --- sink 1 (strmutate::runStringGen) already guarded before this fix; it is
  // asserted here so the two sinks cannot drift apart in wording.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 3", session);
    (void)piet::dispatch("gen s = \"a\"", session);
    check(has(firstError(piet::dispatch("gen s = \"b\"", session)), "variable s already defined"),
          "BUG-112(1) gen (string sink): pre-existing guard fires with the identical wording");
  }

  fs::remove_all(workdir, ec);
}


// --- BUG-331: `offset()` on logit/probit, pinned to a real StataBE oracle ------
// Stata implements offset() on both (StataBE 19.5, 2026-08-17, `sysuse auto` +
// `gen double lnw = ln(weight)`); cobra had no --offset flag for either, so PIET
// honestly rejected a model Stata answers. The numbers below are that log, not a
// self-capture — the class BUG-227 exists to prevent.
//
//   logit  foreign mpg, offset(lnw):  mpg .19929673 (.0535906)  _cons -13.18912 (1.225766)  ll -42.027216
//   probit foreign mpg, offset(lnw):  mpg .13389677 (.0304922)  _cons -11.391033 (.6849953) ll -44.118245
//
// The fit needs data-based starting values: beta=0 makes eta_0 = offset, which
// for ln(weight) ~ 8 walks IRLS off (measured pre-fix: 100 iterations, beta ~3e10,
// converged=false). That start is applied ONLY when an offset is present, so the
// no-offset path stays byte-identical — the leg below pins BOTH halves of that.
void testBUG331Offset() {
  std::printf("[selftest] --- BUG-331: logit/probit offset() vs the StataBE oracle ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug331").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto coefOf = [](const nlohmann::ordered_json& blocks, const std::string& name)
      -> std::optional<std::pair<double, double>> {
    for (const auto& b : blocks) {
      if (!b.is_object() || b.value("type", std::string()) != "estimation") continue;
      if (!b.contains("coeftable")) continue;
      for (const auto& row : b["coeftable"]) {
        if (row.value("name", std::string()) == name && row.contains("b") && row.contains("se") &&
            !row["b"].is_null() && !row["se"].is_null()) {
          return std::make_pair(row["b"].get<double>(), row["se"].get<double>());
        }
      }
    }
    return std::nullopt;
  };
  // rtol 1e-6: the oracle log prints 6-7 significant digits, so a tighter bar
  // would be testing the LOG's precision, not ours (TOLERANCE.md's relaxation key
  // = the oracle's own precision).
  auto near = [](double got, double want) {
    return std::fabs(got - want) <= 1e-6 * std::fabs(want);
  };

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);
  // The oracle fixture is `auto`, which PIET does not bundle, so this leg does
  // NOT re-check the auto numbers — that comparison was made once, by hand, at
  // the fix (cobra reproduced every printed digit of both fits; the log and the
  // values are in the BUGLOG entry). What is pinned HERE, on the bundled dataset,
  // are the three properties the defect violated:
  //   (a) the option is accepted at all (it used to be rejected by name),
  //   (b) it CHANGES the fit (a silently-ignored offset is the failure mode),
  //   (c) a zero-valued offset reproduces the no-offset fit exactly.
  (void)piet::dispatch("gen byte hi = (fatal > 1000)", session);
  (void)piet::dispatch("gen double lnp = ln(beertax + 1)", session);
  (void)piet::dispatch("gen double zoff = 0", session);

  auto plain = piet::dispatch("logit hi beertax", session);
  auto withOff = piet::dispatch("logit hi beertax, offset(lnp)", session);
  auto zeroOff = piet::dispatch("logit hi beertax, offset(zoff)", session);

  auto p0 = coefOf(plain, "beertax");
  auto p1 = coefOf(withOff, "beertax");
  auto pz = coefOf(zeroOff, "beertax");
  if (!p0.has_value()) {
    skip("BUG-331: logit offset()", "no estimation block — no frames-capable `cobra serve` here");
  } else {
    check(p1.has_value(), "BUG-331: `logit …, offset(VAR)` is ACCEPTED (was rejected by name)");
    check(p1.has_value() && !near(p1->first, p0->first),
          "BUG-331: the offset CHANGES the fit — a silently-ignored offset is the failure mode");
    // A zero offset must reproduce the ordinary fit. NOT asserted bit-identical:
    // an offset routes through the data-based starting values, so the iteration
    // PATH differs and the two land on the same MLE up to the last bit (measured
    // here: rel-diff 0 on the slope, 1.7e-16 = 1 ulp on the intercept). 1e-12 is
    // eight orders tighter than any tolerance-ladder tier and still catches a real
    // perturbation; claiming bit-identity would be claiming something false.
    auto sameFit = [&](std::pair<double, double> a, std::pair<double, double> b) {
      return std::fabs(a.first - b.first) <= 1e-12 * std::fabs(b.first) &&
             std::fabs(a.second - b.second) <= 1e-12 * std::fabs(b.second);
    };
    check(pz.has_value() && sameFit(*pz, *p0),
          "BUG-331: a ZERO offset reproduces the no-offset fit to 1e-12 — the offset-only "
          "starting-value path must not move the answer");
    auto q1 = coefOf(piet::dispatch("probit hi beertax, offset(lnp)", session), "beertax");
    check(q1.has_value(), "BUG-331: `probit …, offset(VAR)` is accepted too");
  }

  fs::remove_all(workdir, ec);
}


// BUG-202: nbreg/tobit/heckman/xtpois had NO `--level` option at all — their
// coeftable ci_lo/ci_hi was unconditionally null, with no way to get one at
// any confidence level. Cross-checks the raw `cobra` CLI directly (the fix
// lives in apps/cmds/{cmds_mle,cmds_panel}.cpp + options_table.cpp, none of
// which PIET's dispatcher forwards `level()` into yet for these four verbs —
// see the reject sites under estimation/ this same fix leaves stale, filed
// separately) against the same tests/validate fixtures the estimator unit
// tests (test_nbreg/test_tobit/test_heckman/test_xtpois) already pin:
// tobit.hbin, heckman.hbin, xtpois_in.hbin. Two independent proofs per
// estimator: (1) --level 95 populates a finite ci_lo/ci_hi that brackets b
// (never present before this fix — pre-fix this whole verb hard-errored
// "unrecognized option '--level'"), and (2) --level 90 gives a STRICTLY
// narrower interval than --level 95 on the SAME fit — the qreg-precedent
// check (see runQreg's header comment) that rules out a cosmetic no-op that
// merely stops erroring without actually driving the multiplier.
void testBUG202LevelCiFamily() {
  std::printf("[selftest] --- BUG-202 nbreg/tobit/heckman/xtpois --level ---\n");
  namespace fs = std::filesystem;

  auto fixturePath = [](const char* rel) -> std::string {
    std::string f = std::string(selftestSourceFile());
    auto pos = f.rfind("/piet/native/src/selftest.cpp");
    f = (pos == std::string::npos) ? (std::string("tests/validate/") + rel)
                                    : f.substr(0, pos) + "/tests/validate/" + rel;
    if (std::ifstream(cecon::to_native_path(f)).good()) return f;
    return std::string(selftestValidateDir() + "/") + rel;
  };

  auto ciOf = [](const nlohmann::json& raw, const char* name) -> std::pair<double, double> {
    if (!raw.contains("coefs") || !raw["coefs"].is_array())
      return {std::nan(""), std::nan("")};
    for (const auto& c : raw["coefs"]) {
      if (c.value("name", std::string()) == name) {
        double lo = c.contains("ci_lo") && c["ci_lo"].is_number() ? c["ci_lo"].get<double>() : std::nan("");
        double hi = c.contains("ci_hi") && c["ci_hi"].is_number() ? c["ci_hi"].get<double>() : std::nan("");
        return {lo, hi};
      }
    }
    return {std::nan(""), std::nan("")};
  };

  auto checkOneLevel = [&](const std::string& label, const std::vector<std::string>& baseArgv,
                           const char* coefName) {
    std::string fx = baseArgv.empty() ? std::string() : baseArgv[baseArgv.size() - 1];
    if (!std::ifstream(cecon::to_native_path(fx)).good()) {
      skip(label + " (fixture missing)", "not found at " + fx);
      return;
    }
    nlohmann::json r95, r90;
    bool have95 = false, have90 = false;
    try {
      auto a95 = baseArgv; a95.insert(a95.end() - 2, {"--level", "95"});
      r95 = piet::runCobraJson(a95);
      have95 = true;
    } catch (const piet::CobraError& ce) {
      skip(label, "engine has no --level for this verb yet (declare COBRA_BIN=<tree>/build/cobra): " +
                       std::string(ce.what()));
      return;
    }
    try {
      auto a90 = baseArgv; a90.insert(a90.end() - 2, {"--level", "90"});
      r90 = piet::runCobraJson(a90);
      have90 = true;
    } catch (const piet::CobraError&) {
      have90 = false;
    }
    auto [lo95, hi95] = ciOf(r95, coefName);
    check(have95 && std::isfinite(lo95) && std::isfinite(hi95) && lo95 < hi95,
          label + ": --level 95 gives a finite, ordered ci_lo/ci_hi (BUG-202 — was unconditionally null)");
    if (have90) {
      auto [lo90, hi90] = ciOf(r90, coefName);
      check(std::isfinite(lo90) && std::isfinite(hi90) && (hi90 - lo90) < (hi95 - lo95),
            label + ": --level 90 is STRICTLY narrower than --level 95 on the same fit "
                    "(rules out a cosmetic no-op)");
    }
  };

  checkOneLevel("BUG-202 tobit",
                {"tobit", "--y", "visits_w", "--x", "age_z,edu_hi,treated", "--ll", "0",
                 "--data", fixturePath("tobit.hbin")},
                "treated");
  checkOneLevel("BUG-202 heckman",
                {"heckman", "--y", "y", "--x", "x1", "--select", "S=z1,z2",
                 "--data", fixturePath("heckman.hbin")},
                "x1");
  checkOneLevel("BUG-202 xtpois",
                {"xtpois", "--y", "y_int", "--x", "treated,age_z,edu_hi", "--ivar", "h_pid",
                 "--data", fixturePath("xtpois_in.hbin")},
                "treated");
  checkOneLevel("BUG-202 nbreg",
                {"nbreg", "--y", "y_int", "--x", "treated,age_z,edu_hi",
                 "--data", fixturePath("xtpois_in.hbin")},
                "treated");
}


// BUG-274 residual + BUG-263 (WS-U): the PIET-side wiring for what BUG-202
// gave the engine. Three converted level() forwards (nbreg / xtpoisson,fe /
// heckman) with their two kept refusals (xtlogit here; IV/LIML and reg3 are
// pinned by their own earlier legs), and the mediate treatment sub-block's
// new option net (BUG-148's bidirectional triple on a SECOND-SOURCE opts
// vector — BUG-262's class, one layer further in).
//
// Engine gating, same two-shape split as the BUG-203 leg above: PIET's OWN
// refusal text ("does not yet implement for `<verb>`" / "not allowed") on a
// converted verb is a PIET regression and FAILS unconditionally; a cobra
// error envelope means the wired engine predates BUG-202's --level (the Art 7
// archive pin) and the numeric legs SKIP — declare COBRA_BIN=<tree>/build/cobra.
void testBUG274MediateWiringResiduals() {
  std::printf("[selftest] --- BUG-274/BUG-263 level() forwards + mediate sub-block net ---\n");
  using ojson = nlohmann::ordered_json;
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_bug274").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // The bundled sample the D3 oracle batch already keys on, resolved off
  // __FILE__ exactly like testBUG202LevelCiFamily's fixturePath.
  std::string csv = std::string(selftestSourceFile());
  {
    auto pos = csv.rfind("/piet/native/src/selftest.cpp");
    csv = (pos == std::string::npos)
              ? std::string("piet/packaging/sample/fatalities.csv")
              : csv.substr(0, pos) + "/piet/packaging/sample/fatalities.csv";
  }
  if (!std::ifstream(cecon::to_native_path(csv)).good()) {
    skip("BUG-274/BUG-263 (all legs)", "sample csv not found at " + csv);
    return;
  }

  piet::SessionState session(workdir);
  (void)piet::dispatch("import delimited \"" + csv + "\"", session);
  (void)piet::dispatch("gen jailb = (unemp > 7)", session);
  (void)piet::dispatch("egen snum = group(state)", session);
  (void)piet::dispatch("xtset snum year", session);

  auto firstEst = [](const ojson& blocks) -> ojson {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "estimation") return b;
    }
    return ojson(nullptr);
  };
  auto errText = [](const ojson& blocks) -> std::string {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") {
        return b.value("message", std::string());
      }
    }
    return std::string();
  };
  auto ciOf = [](const ojson& est, const char* name) -> std::pair<double, double> {
    if (est.is_null() || !est.contains("coeftable")) return {std::nan(""), std::nan("")};
    for (const auto& r : est["coeftable"]) {
      if (r.value("name", std::string()) == name) {
        double lo = r.contains("ci_lo") && r["ci_lo"].is_number() ? r["ci_lo"].get<double>()
                                                                  : std::nan("");
        double hi = r.contains("ci_hi") && r["ci_hi"].is_number() ? r["ci_hi"].get<double>()
                                                                  : std::nan("");
        return {lo, hi};
      }
    }
    return {std::nan(""), std::nan("")};
  };

  // --- the three conversions: PIET must never answer its own old refusal ---
  struct Conv { const char* label; const char* cmd90; const char* cmd95; const char* verb; };
  const Conv kConvs[] = {
      {"nbreg", "nbreg fatal beertax spirits, level(90)",
       "nbreg fatal beertax spirits, level(95)", "`nbreg`"},
      {"xtpoisson,fe", "xtpoisson fatal beertax spirits, fe level(90)",
       "xtpoisson fatal beertax spirits, fe level(95)", "`xtpoisson`"},
      {"heckman,twostep", "heckman income beertax, select(jailb = spirits unemp) twostep level(90)",
       "heckman income beertax, select(jailb = spirits unemp) twostep level(95)", "`level` not allowed"},
  };
  for (const auto& c : kConvs) {
    auto b90 = piet::dispatch(c.cmd90, session);
    auto b95 = piet::dispatch(c.cmd95, session);
    const std::string e90 = errText(b90);
    check(e90.find("does not yet implement") == std::string::npos &&
              e90.find("not allowed") == std::string::npos,
          std::string("BUG-274 ") + c.label + ": level() is not PIET's own refusal any more "
          "(seeing one here is a PIET-side regression) — got: " + e90);
    ojson e = firstEst(b90);
    ojson e5 = firstEst(b95);
    if (e.is_null() || e5.is_null()) {
      skip(std::string("BUG-274 ") + c.label + " numeric legs",
           "wired engine has no --level for this verb (declare COBRA_BIN=<tree>/build/cobra); "
           "engine error: " + (e90.empty() ? errText(b95) : e90));
      continue;
    }
    auto [lo90, hi90] = ciOf(e, "beertax");
    auto [lo95, hi95] = ciOf(e5, "beertax");
    check(std::isfinite(lo90) && std::isfinite(hi90) && lo90 < hi90,
          std::string("BUG-274 ") + c.label + ": level(90) answers a finite ordered CI");
    check(std::isfinite(lo95) && std::isfinite(hi95) && (hi90 - lo90) < (hi95 - lo95),
          std::string("BUG-274 ") + c.label + ": the 90% interval is STRICTLY narrower than "
          "95% on the same fit (rules out a forwarded-but-cosmetic no-op)");
    if (std::string(c.label) == "nbreg") {
      // The independent oracle, BS-069-style: real StataBE 19.5, marker-judged,
      // same sample and command (`nbreg fatal beertax spirits, level(90)`):
      // r(table) beertax ll = -6.4254890197274e-02, ul = 2.2342713412572e-01,
      // crit = 1.6448536269515 (z). Ours agreed at ~5e-10 abs when captured.
      check(std::abs(lo90 - (-6.4254890197274e-02)) < 1e-6 &&
                std::abs(hi90 - 2.2342713412572e-01) < 1e-6,
            "BUG-274 nbreg oracle: the level(90) CI matches Stata's r(table) ll/ul on the "
            "bundled sample (not merely finite)");
    }
    if (std::string(c.label) == "xtpoisson,fe") {
      // Same capture batch: `xtpoisson fatal beertax spirits, fe level(90)`
      // r(table) beertax ll = -5.4976419440441e-01, ul = -4.3331919034386e-01.
      check(std::abs(lo90 - (-5.4976419440441e-01)) < 1e-6 &&
                std::abs(hi90 - (-4.3331919034386e-01)) < 1e-6,
            "BUG-274 xtpoisson oracle: the level(90) CI matches Stata's r(table) ll/ul on the "
            "bundled sample (not merely finite)");
    }
    if (std::string(c.label) == "heckman,twostep") {
      // The select-equation carriage: pre-fix the outcomes[] remap hard-nulled
      // ci_lo/ci_hi; the engine sends them when --level is forwarded.
      bool selCi = false;
      if (e.contains("outcomes") && e["outcomes"].is_array() && e["outcomes"].size() == 2) {
        const auto& sel = e["outcomes"][1];
        if (sel.contains("coeftable") && !sel["coeftable"].empty()) {
          const auto& r0 = sel["coeftable"][0];
          selCi = r0.contains("ci_lo") && r0["ci_lo"].is_number() &&
                  r0.contains("ci_hi") && r0["ci_hi"].is_number();
        }
      }
      check(selCi, "BUG-274 heckman: select_coefs ci_lo/ci_hi carried through the outcomes[] "
                   "remap when level() was forwarded (pre-fix: hard-coded null)");
      // The default (no level()) shape: PIET carries exactly what the engine
      // sends. On a post-BUG-279 engine (default 95% CI, WS-T's parallel fix)
      // the select CI is populated and the 90% interval above must be STRICTLY
      // narrower; on a pre-BUG-279 engine it is null and must be carried as
      // null, never fabricated. Both arms are asserted, keyed off what the
      // engine actually sent.
      ojson eDef = firstEst(piet::dispatch(
          "heckman income beertax, select(jailb = spirits unemp) twostep", session));
      if (!eDef.is_null() && eDef.contains("outcomes") && eDef["outcomes"].size() == 2) {
        const auto& r0 = eDef["outcomes"][1]["coeftable"][0];
        const bool defHasCi = r0["ci_lo"].is_number() && r0["ci_hi"].is_number();
        if (defHasCi) {
          double dLo = r0["ci_lo"].get<double>(), dHi = r0["ci_hi"].get<double>();
          double sLo = std::nan(""), sHi = std::nan("");
          if (e.contains("outcomes") && e["outcomes"].size() == 2) {
            const auto& s0 = e["outcomes"][1]["coeftable"][0];
            if (s0["ci_lo"].is_number()) sLo = s0["ci_lo"].get<double>();
            if (s0["ci_hi"].is_number()) sHi = s0["ci_hi"].get<double>();
          }
          check(std::isfinite(sLo) && std::isfinite(sHi) && (sHi - sLo) < (dHi - dLo),
                "BUG-274 heckman: the DEFAULT (BUG-279 engine-side 95%) select-equation CI is "
                "strictly wider than the forwarded level(90) one — default and forward agree on "
                "one mechanism");
        } else {
          check(r0["ci_lo"].is_null() && r0["ci_hi"].is_null(),
                "BUG-274 heckman: on a pre-BUG-279 engine the default select-equation CI is "
                "carried as null, not fabricated");
        }
      }
    }
  }

  // --- the kept refusal: xtlogit's premise is still true (re-measured) ---
  {
    const std::string e =
        errText(piet::dispatch("xtlogit jailb beertax spirits, fe level(90)", session));
    check(e.find("does not yet implement") != std::string::npos &&
              e.find("xtlogit") != std::string::npos,
          "BUG-274 kept: `xtlogit ,fe level()` still honest-rejects (the engine's xtlogit CLI "
          "has no --level surface — premise re-verified live) — got: " + e);
  }

  // --- BUG-263: the mediate treatment sub-block net (BUG-148's triple) ---
  {
    const unsigned long lateBefore = piet::eval::optNetLateResolutionCount();
    // 1. abbreviation works: `contin` is a >=2-char unique prefix of the
    //    sub-block vocabulary, resolved by the handler's OWN canonicalization
    //    point (the dispatcher's never sees tOpts).
    auto bAbbrev = piet::dispatch(
        "mediate (income unemp) (spirits) (beertax, contin(0.5 1))", session);
    const std::string eAbbrev = errText(bAbbrev);
    check(eAbbrev.find("only a continuous treatment") == std::string::npos &&
              eAbbrev.find("not allowed") == std::string::npos,
          "BUG-263: `contin(0.5 1)` resolves to continuous() instead of being silently dropped "
          "into the misdiagnosing specification reject — got: " + eAbbrev);
    ojson eM = firstEst(bAbbrev);
    if (eM.is_null()) {
      skip("BUG-263 abbreviation numeric leg",
           "wired engine could not run mediate; the net verdicts above/below still bind");
    } else {
      check(eM.contains("treatment_levels") && eM["treatment_levels"].is_array() &&
                eM["treatment_levels"][0].get<double>() == 0.5,
            "BUG-263: the resolved continuous() levels actually reached the handler's reads "
            "(treatment_levels == [0.5, 1])");
    }
    // 2. typo rejected, naming the KEY — not the specification.
    const std::string eTypo = errText(piet::dispatch(
        "mediate (income unemp) (spirits) (beertax, bogus(1))", session));
    check(eTypo.find("`bogus` not allowed") != std::string::npos,
          "BUG-263: a typo'd sub-block key is REJECTED by name (pre-fix it was silently ignored "
          "and the error blamed the treatment specification) — got: " + eTypo);
    // 3. the real-Stata-but-unimplemented shape keeps its honest message: the
    //    sub-block grammar has exactly ONE option ([CAUSAL] mediate), so the
    //    honesty leg is the numlist arity, not a second vocabulary.
    const std::string eArity = errText(piet::dispatch(
        "mediate (income unemp) (spirits) (beertax, continuous(0 0.5 1))", session));
    check(eArity.find("two-level continuous()") != std::string::npos,
          "BUG-263: a >2-value continuous() numlist (real Stata, unimplemented here) still gets "
          "the honest two-level-only message — got: " + eArity);
    // The BUG-233 guard, scoped to this net: the handler canonicalizes tOpts
    // ITSELF, so rejectUnknownOptions must never have had to resolve a key.
    // Falsification: delete runMediate's canonicalizeOptionKeys call and
    // `contin` arrives raw at the net, the count moves, this names it.
    check(piet::eval::optNetLateResolutionCount() == lateBefore,
          "BUG-263 GUARD: the treatment sub-block net saw only canonical keys (late resolution "
          "means runMediate's own canonicalization point was bypassed); last seen: " +
              piet::eval::optNetLastLateResolution());
  }

  fs::remove_all(workdir, ec);
}



void testBUG291to295WsvResiduals() {
  std::printf("[selftest] --- BUG-291/293/294/295 WS-V residuals (ONESHOT-ZERO WS-N) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_wsn").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);
  session.cwd = workdir;

  auto run = [&](const std::string& line) { return piet::dispatch(line, session); };
  auto dtypeOf = [&](const std::string& var) -> std::string {
    const nlohmann::ordered_json blocks = run("describe " + var);
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "describe") {
        for (const auto& v : b.at("variables")) {
          if (v.value("name", std::string()) == var) return v.value("dtype", std::string());
        }
      }
    }
    return std::string("<none>");
  };
  auto dispOf = [&](const std::string& line) -> std::string {
    const nlohmann::ordered_json blocks = run(line);
    std::string out;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display") {
        out = b.value("text", std::string());
      }
    }
    return out;
  };
  auto rcOf = [&](const std::string& line) -> std::string {
    const nlohmann::ordered_json blocks = run(line);
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") {
        return piet::eval::coarseRc(b);
      }
    }
    return std::string("0");
  };

  // ---- BUG-291: the varname-keyed planes forget a column that leaves --------
  (void)run("clear");
  (void)run("set obs 3");
  (void)run("gen double x = 1.5");
  (void)run("drop x");
  (void)run("gen x = 1");
  check(dtypeOf("x") == "float",
        "BUG-291: `gen double x` -> `drop x` -> `gen x = 1` answers float "
        "(WSN|291a_type_after_drop_recreate|float) — the stale double claim is gone");

  (void)run("clear");
  (void)run("set obs 3");
  (void)run("gen double kx = 1");
  (void)run("gen ky = 2");
  (void)run("keep ky");
  (void)run("gen kx = 3");
  check(dtypeOf("kx") == "float",
        "BUG-291: `keep` erases the planes of the columns it removes too "
        "(WSN|291c_keep_then_recreate|float)");

  // The label/format planes leak by the same mechanism, so they are pruned by
  // the same eraser — a recreated column must not inherit either.
  (void)run("clear");
  (void)run("set obs 3");
  (void)run("gen lx = 1");
  (void)run("format lx %12.4f");
  (void)run("label variable lx \"stale label\"");
  (void)run("drop lx");
  (void)run("gen lx = 2");
  {
    const nlohmann::ordered_json blocks = run("describe lx");
    std::string fmt = "<none>", lbl = "<none>";
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "describe") {
        for (const auto& v : b.at("variables")) {
          if (v.value("name", std::string()) == "lx") {
            fmt = v.value("format", std::string());
            lbl = v.value("label", std::string());
          }
        }
      }
    }
    check(fmt != "%12.4f" && lbl != "stale label",
          "BUG-291: the format and variable-label planes are pruned with the column too "
          "(they leaked across drop/recreate the same way all along)");
  }

  // CONTROL, and the falsified half of the entry: Stata's collapse KEEPS the
  // source type (WSN|291b_collapse_double_source|double), so a double source
  // must still answer double. A prune that fired on collapse's survivors would
  // break this leg — which is why the reconcile is against the LIVE card.
  (void)run("clear");
  (void)run("set obs 4");
  (void)run("gen double cx = 2.5");
  (void)run("gen g = _n>2");
  (void)run("collapse (mean) cx, by(g)");
  check(dtypeOf("cx") == "double",
        "BUG-291 control: collapse of a DOUBLE source still answers double "
        "(WSN|291b_collapse_double_source|double — the entry's float claim is falsified)");

  // ---- BUG-294: an undefined macro expands to empty at rc 0 ----------------
  check(dispOf("display \"[$ZZTOTALLYUNDEFINED]\"") == "[]",
        "BUG-294: an undefined GLOBAL expands to empty (WSN|294_undef_global_render|[])");
  check(dispOf("display \"[`nosuchlocal']\"") == "[]",
        "BUG-294: an undefined LOCAL expands to empty");
  check(rcOf("display \"[$ZZTOTALLYUNDEFINED]\"") == "0",
        "BUG-294: ...at rc 0 (WSN|294_display_undef_global_rc|0) — the run does not stop");

  // ---- BUG-293: the display arm --------------------------------------------
  check(dispOf("display %09.2f (42.5)") == "000042.50",
        "BUG-293(a): the `0` flag zero-fills (WSN|293_display_zeroflag_val|000042.50)");
  check(dispOf("display %td (20000)") == "04oct2014",
        "BUG-293(a): bare %td renders the calendar date (WSN|293_display_td_val|04oct2014)");
  check(dispOf("display %tc (1234567890000)") == "13feb1999 23:31:30",
        "BUG-293(a): bare %tc renders date+time (WSN|293_display_tc_val)");
  check(dispOf("display string(42.5,\"%9.2f\")") == "42.50",
        "BUG-293(c): a quoted argument INSIDE parentheses no longer splits the segment, and "
        "the string result is printed verbatim (WSN|293_display_string_goodfmt_val|42.50)");
  check(rcOf("display 1%2") == "198",
        "BUG-293(b): an unevaluable expression is r(198), not its own source text at rc 0 "
        "(WSN|293_display_mod_rc|198)");
  check(rcOf("display \"abc \" string(\"abc\",\"%9.0g\")") == "109",
        "BUG-293: an error that NAMES its Stata code carries it into _rc "
        "(WSN|293_display_string_badfmt_rc|109 — was a generic 1)");
  // The two boundaries this deliberately does NOT claim, pinned so a later
  // widening is a visible decision: a BARE identifier keeps the legacy echo
  // (the dataset-variable read this layer cannot do — the same shape BUG-246's
  // classifier leaves unclaimed), and a resolved value that is not arithmetic
  // must PRINT, never error.
  // name-binding-display-literals closed the first boundary: a bare name that
  // binds to nothing is StataBE's r(111) `NAME not found`, alone or beside
  // other words (tests/eval/golden/aer_w2/w2av_name_binding.log:157 `[foo not
  // found`, :166 `PREC|bd_dy_unb|111|`). The second boundary stands.
  check(rcOf("display zzqq_no_such_thing") == "111",
        "BUG-293 boundary (moved): a bare undefined identifier is r(111), not its own text");
  check(rcOf("display foo bar") == "111",
        "BUG-293 boundary (moved): multi-token prose of unbound names is r(111), not echoed");

  // ---- BUG-295: an undeclared gen round-trips as float ----------------------
  (void)run("clear");
  (void)run("set obs 2");
  (void)run("gen z = _n");
  (void)run("gen byte b = 1");
  (void)run("gen double d = 1.5");
  check(dtypeOf("z") == "float", "BUG-295: `gen z = _n` is float in-session (BUG-250 fallback)");
  (void)run("save wsn295.dta, replace");
  (void)run("use wsn295.dta, clear");
  check(dtypeOf("z") == "float",
        "BUG-295: ...and float after save+use too (WSN|295_type_after_roundtrip|float) — the "
        "same column no longer answers float before a save and byte after");
  check(dtypeOf("b") == "byte" && dtypeOf("d") == "double",
        "BUG-295 control: an EXPLICIT declaration still wins over the implicit float");
}
