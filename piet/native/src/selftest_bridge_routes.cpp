// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: bridge routes.
#include "selftest_support.h"
#include "selftest_cases.h"


// --- 4. (M4b) bridge routes --------------------------------------------------
//
// Drives every routes::* adapter directly against a real SessionState built
// with a real cobra-backed dataset (tests/validate/auto.hbin, the same
// public fixture the rest of the corpus uses) — the headless proof that
// "api.js's request() path reaches every route" the M4b gate asks for,
// covering the C++ side of that path (JS-side request() plumbing is proven
// separately below via the Node shim, and end-to-end via the real webview
// per the User checklist).
void testBridgeRoutesM4b() {
  std::printf("[selftest] --- M4b bridge routes ---\n");

  // WP-W5: portable temp dir + std::filesystem mkdir — no /tmp, no POSIX shell
  // (mkdir -p / single-quote quoting) so this runs on Windows unchanged.
  std::string workdir =
      (std::filesystem::temp_directory_path() / "piet_native_selftest_m4b")
          .u8string();
  std::error_code workdirEc;
  std::filesystem::create_directories(workdir, workdirEc);

  // BUG-1131: current_cmd is the running statement cut at 120 bytes. A cut
  // inside a UTF-8 character (a long Korean line) left text a strict JSON dump
  // throws on, and the app dumps the snapshot on a bare ticker thread, where a
  // throw is std::terminate. The cut now stops before that character. No data
  // is needed, so this runs ahead of the fixture check below.
  {
    piet::SessionState session(workdir);
    session.progressUpdate(1, std::string(119, 'a') + "\xEA\xB0\x80 = 1", 0);
    bool threw = false;
    std::string cmd;
    try {
      const auto snap = session.progressSnapshot();
      (void)snap.dump();
      cmd = snap.value("current_cmd", std::string());
    } catch (const std::exception&) {
      threw = true;
    }
    check(!threw && cmd == std::string(119, 'a'),
          "BUG-1131: progress current_cmd is cut before a UTF-8 character, not inside it (got " +
              std::to_string(cmd.size()) + " bytes" + (threw ? ", dump threw" : "") + ")");
  }

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
  bool haveFixture = std::ifstream(cecon::to_native_path(fixture)).good();
  if (!haveFixture) {
    skip("M4b bridge routes (needs tests/validate/auto.hbin)", "fixture not found at " + fixture);
    return;
  }

  piet::SessionState session(workdir);

  // version: no data needed, always answers.
  {
    auto r = piet::routes::version(session, "{}");
    check(r.ok, "version route returns ok=true");
    auto j = nlohmann::json::parse(r.json);
    check(j.contains("piet_version") && j.contains("cobra_version") && j.contains("contract_a") &&
              j.contains("contract_b"),
          "version route returns the full 4-tuple {piet_version,cobra_version,contract_a,contract_b}");
  }

  // varlist/browse/tabulate/describe/summarize before any data is loaded:
  // must report the no-data condition, never crash.
  {
    auto r = piet::routes::varlist(session, "{}");
    check(r.ok, "varlist route (no data) still returns ok=true (empty varlist, not an error)");
    auto j = nlohmann::json::parse(r.json);
    check(j.value("n_rows", -1) == 0, "varlist route (no data) reports n_rows=0");
  }
  {
    auto r = piet::routes::browse(session, "{}");
    check(r.ok, "browse route (no data) returns ok=true (200+error block, never a bare reject)");
    auto j = nlohmann::json::parse(r.json);
    check(j.value("code", std::string()) == "no_data_loaded",
          "browse route (no data) response is a no_data_loaded CommandErrorResult");
  }

  // open: load the real fixture through the bridge route (exercises
  // handlers::runUse via the routes::open adapter).
  {
    nlohmann::json body{{"filepath", fixture}};
    auto r = piet::routes::open(session, body.dump());
    check(r.ok, "open route returns ok=true for a valid .hbin path");
    auto j = nlohmann::json::parse(r.json);
    check(j.value("action", std::string()) == "open", "open route response has action:\"open\"");
    check(j.contains("varlist_after") && j["varlist_after"].is_array() &&
              !j["varlist_after"].empty(),
          "open route response carries a non-empty varlist_after");
    check(j.value("n_rows_after", 0) > 0, "open route response carries n_rows_after > 0");
  }

  check(session.hasData(), "session.hasData() true after routes::open");

  // varlist (now with data).
  {
    auto r = piet::routes::varlist(session, "{}");
    auto j = nlohmann::json::parse(r.json);
    check(j.value("n_cols", 0) > 0, "varlist route (with data) reports n_cols > 0");
  }

  // browse (now with data) — offset/limit threading.
  {
    nlohmann::json body{{"offset", 0}, {"limit", 5}};
    auto r = piet::routes::browse(session, body.dump());
    check(r.ok, "browse route (with data) returns ok=true");
    auto j = nlohmann::json::parse(r.json);
    check(j.contains("rows") && j["rows"].is_array() && j["rows"].size() == 5,
          "browse route honours limit=5 (returns exactly 5 rows)");
  }

  // command — routes through the real dispatch(); prove a summarize round
  // trip returns a real SummarizeResult block.
  {
    nlohmann::json body{{"cmd", "summarize price"}};
    auto r = piet::routes::command(session, body.dump());
    check(r.ok, "command route returns ok=true");
    auto j = nlohmann::json::parse(r.json);
    bool sawSummarize = false;
    for (const auto& b : j.value("blocks", nlohmann::json::array())) {
      if (b.value("type", std::string()) == "summarize") sawSummarize = true;
    }
    check(sawSummarize, "command route ('summarize price') response contains a summarize block");
  }

  // tabulate/describe/summarize structured routes.
  {
    nlohmann::json body{{"varlist", nlohmann::json::array({"foreign"})}};
    auto r = piet::routes::tabulate(session, body.dump());
    check(r.ok, "tabulate route returns ok=true");
  }
  {
    auto r = piet::routes::describe(session, "{}");
    check(r.ok, "describe route (no varlist filter) returns ok=true");
  }
  {
    nlohmann::json body{{"varlist", nlohmann::json::array({"price", "mpg"})}, {"detail", false}};
    auto r = piet::routes::summarize(session, body.dump());
    check(r.ok, "summarize route returns ok=true");
  }

  // rename / label.variable / label.define / label.values / sort / recode —
  // each mutates session state; run in a sequence that stays valid
  // throughout (rename price->price2, then rename back so later blocks in
  // this test can keep using "price").
  {
    nlohmann::json body{{"varname", "price"}, {"newname", "price_tmp"}};
    auto r = piet::routes::rename(session, body.dump());
    check(r.ok, "rename route returns ok=true");
    auto j = nlohmann::json::parse(r.json);
    check(j.value("action", std::string()) == "rename", "rename route response has action:\"rename\"");
  }
  {
    nlohmann::json body{{"varname", "price_tmp"}, {"newname", "price"}};
    auto r = piet::routes::rename(session, body.dump());
    check(r.ok, "rename route (rename back) returns ok=true");
  }
  {
    nlohmann::json body{{"varname", "price"}, {"label", "Vehicle price (USD)"}};
    auto r = piet::routes::labelVariable(session, body.dump());
    check(r.ok, "label.variable route returns ok=true");
  }
  {
    // Label VALUES here are arbitrary synthetic strings (they only need to
    // round-trip through session.labelDefs) — not tied to any dataset.
    nlohmann::json body{
        {"lblname", "origin_lbl"},
        {"mapping", nlohmann::json::object({{"0", "GroupZero"}, {"1", "GroupOne"}})}};
    auto r = piet::routes::labelDefine(session, body.dump());
    check(r.ok, "label.define route returns ok=true");
    check(session.labelDefs.count("origin_lbl") == 1 &&
              session.labelDefs.at("origin_lbl").at("0") == "GroupZero",
          "label.define route writes session.labelDefs directly (editor.py parity, no re-attach)");
  }
  {
    nlohmann::json body{{"varname", "foreign"}, {"lblname", "origin_lbl"}};
    auto r = piet::routes::labelValues(session, body.dump());
    check(r.ok, "label.values route returns ok=true");
  }
  {
    // Regression (BUG-025's duplicates-list sibling): `duplicates list`
    // browses a cobra-filtered TEMP dataset, and that call passed an empty
    // value-label map — value-labeled f64 columns silently rendered raw
    // codes (rc=0, wrong display) while plain list/browse rendered label
    // text. foreign carries origin_lbl (attached just above) and every one
    // of auto's 74 rows duplicates on foreign alone, so the filtered browse
    // must render label TEXT and carry the value_labels column-meta sidecar,
    // same as the oracle's duplicates.py.
    nlohmann::json body{{"cmd", "duplicates list foreign"}};
    auto r = piet::routes::command(session, body.dump());
    check(r.ok, "command route ('duplicates list foreign') returns ok=true");
    auto j = nlohmann::json::parse(r.json);
    bool sawBrowse = false, vlSidecar = false, labelText = false;
    for (const auto& b : j.value("blocks", nlohmann::json::array())) {
      if (b.value("type", std::string()) != "browse") continue;
      sawBrowse = true;
      for (const auto& cm : b.value("columns", nlohmann::json::array())) {
        if (cm.value("name", std::string()) == "foreign" && cm.contains("value_labels")) {
          vlSidecar = true;
        }
      }
      const auto& rows = b.value("rows", nlohmann::json::array());
      if (!rows.empty() && rows[0].is_array() && !rows[0].empty() && rows[0][0].is_string()) {
        const std::string v = rows[0][0].get<std::string>();
        labelText = (v == "GroupZero" || v == "GroupOne");
      }
    }
    check(sawBrowse, "duplicates list returns a browse block (filtered temp-dataset path)");
    check(vlSidecar,
          "duplicates list: filtered browse carries the value_labels column-meta sidecar");
    check(labelText,
          "duplicates list: value-labeled f64 column renders label TEXT, not raw codes");
  }
  {
    // ascending-only sort -> delegates to handlers::runSort.
    nlohmann::json body{{"varlist", nlohmann::json::array({"mpg"})}};
    auto r = piet::routes::sortRoute(session, body.dump());
    check(r.ok, "sort route (ascending-only) returns ok=true");
    auto j = nlohmann::json::parse(r.json);
    check(j.value("action", std::string()) == "sort", "sort route response has action:\"sort\"");
  }
  {
    // descending -> delegates to handlers::runGsort, action normalised back
    // to "sort" (see routes.cpp's sortRoute doc comment).
    nlohmann::json body{{"varlist", nlohmann::json::array({"mpg"})},
                        {"ascending", nlohmann::json::array({false})}};
    auto r = piet::routes::sortRoute(session, body.dump());
    check(r.ok, "sort route (descending, gsort-delegated) returns ok=true");
    auto j = nlohmann::json::parse(r.json);
    check(j.value("action", std::string()) == "sort",
          "sort route (descending) response action normalised to \"sort\" (not \"gsort\")");
  }
  {
    // L4 O(1)-cards guard: a reorder (sort/gsort/order) CARRIES the missing-count
    // cache instead of rescanning the whole frame (session_state.cpp
    // seedNMissForPureAdd branch 5 — the full-frame "card tax" this removes). The
    // carried count must equal the truth: rep78 has exactly 5 missing rows in
    // `auto`, and a row permutation cannot change that. This catches a stale or
    // zeroed seed regressing the fast path (which would silently misreport
    // n_missing on the post-sort card).
    auto rep78Miss = [](const nlohmann::json& jv) -> int64_t {
      for (const auto& v : jv.value("varlist_after", nlohmann::json::array()))
        if (v.value("name", std::string()) == "rep78")
          return v.value("n_missing", static_cast<int64_t>(-1));
      return -1;
    };
    nlohmann::json sb{{"varlist", nlohmann::json::array({"mpg"})}};
    auto rs = piet::routes::sortRoute(session, sb.dump());
    auto js = nlohmann::json::parse(rs.json);
    check(rep78Miss(js) == 5,
          "L4 reorder card: rep78 n_missing==5 survives `sort mpg` (seeded count == truth, "
          "not rescanned-away)");
  }
  {
    // Card-tax REGRESSION GATE, companion to the L4 guard just above: a
    // pinned VALUE (rep78 n_missing==5) is correct whether it came from a
    // cheap seed-carry or an expensive full rescan — both give the same
    // answer, which is exactly how the L4 optimization stayed dead while
    // that guard kept passing (the ordering bug: reseedShadowFrom's
    // ds->varlist() call froze a real-scan result into Datastore's memoized
    // cache BEFORE seedNMissForPureAdd ever populated the seed it was
    // supposed to consult). Datastore::fullScanCount() (datastore.h)
    // instruments the MECHANISM instead of the value: it counts every
    // nMissing() call that fell through the nMissSeed_ lookup into a real
    // O(rows) scan. mutationResult (handlers_m3.cpp) builds a BRAND NEW
    // Datastore per mutation (invalidateDatastore()+datastore()), so a
    // freshly-refetched ds's counter starts at 0 and reflects only what THIS
    // sort's own varlist()-cache build did. nMissCache_ already covers every
    // column by this point (populated by the rename/label/sort calls above),
    // so with the ordering fix in place the seed covers every column and the
    // counter must stay exactly 0; with the ordering bug back, every column
    // falls through to a real scan before the seed exists and the counter
    // reads the live column count instead.
    // NMISS-CARRY (2026-08-20): this whole block's DISCRIMINATOR is
    // fullScanCount() — it proves the seed mechanism fired by counting the scans
    // that did NOT happen. The header-carried missing count makes that number 0
    // whether or not the seed fires, so with the carry live every assertion here
    // would keep passing while measuring nothing. Relaxing them to match would be
    // trading a real detector for a green tick
    // ([[gate-fix-can-trade-blindness]]); instead the carry is switched OFF for
    // the duration of this block, so the gate keeps testing exactly what it was
    // built to test. Restored below.
    ::setenv("PIET_NO_NMISS_CARRY", "1", 1);
    nlohmann::json sb2{{"varlist", nlohmann::json::array({"mpg"})}};
    auto rs2 = piet::routes::sortRoute(session, sb2.dump());
    check(rs2.ok, "card-tax gate setup: `sort mpg` returns ok=true");
    // Re-fetch after the mutation — never read a pre-mutation ds pointer (see
    // the BUG-037 comment elsewhere in this file on why that's a stale read).
    piet::Datastore* ds = session.datastore();
    check(ds != nullptr, "card-tax gate setup: session.datastore() non-null after `sort`");
    if (ds != nullptr) {
      check(ds->fullScanCount() == 0,
            "card-tax gate: `sort` consumes the L4 carry-forward seed with ZERO real "
            "nMissing() rescans (fullScanCount()==0) — nonzero means the seed reached "
            "Datastore::varlist()'s memoized build too late to be seen");
    }
  }
  {
    // MAXPERF gate (fix/sort-validate-lazy): runSort's varlist NAME-only
    // validation (handlers_m3.cpp) must consult the resident shadow card
    // (session.shadowVarlistIfCurrent()) instead of unconditionally forcing
    // session.datastore() open the current hbin — that eager re-open purely
    // to answer "does column X exist" was the measured ~700ms-1700ms/sort
    // "datastore+has" cost (phase-split instrumentation of runSort; see the
    // fix's own doc comment there). Datastore::constructCount() (datastore.h)
    // instruments the MECHANISM (every Datastore ctor call, process-wide),
    // the same style as fullScanCount() just above instruments the L4 seed.
    //
    // Setup: prime with one `sort` so the shadow card becomes valid AND
    // tagged for the CURRENT hbin — mutationResult's non-resident fallback
    // (reached below because sortRoute is called directly, off dispatch()'s
    // enterDispatch(), same "stay off the resident-frame short-circuit"
    // reasoning as the card-tax gate just above) unconditionally calls
    // session.reseedShadowFrom(), which tags the card for exactly that
    // rebuild. That SAME rebuild also leaves session's cached Datastore*
    // warm as an incidental side effect (the very next session.datastore()
    // call returns it uninvalidated) — which would mask the regression this
    // gate exists to catch, since it is not what the real production
    // precondition looks like. In a genuine resident-chain sort,
    // mutationResult takes the tryResidentMutationCard FAST path instead and
    // never calls session.datastore() at all, so the cache stays cold from
    // load()'s own unconditional reset. invalidateDatastore() reproduces
    // that exact cold-cache-but-valid-shadow-card precondition directly,
    // without depending on a frames/serve-capable cobra binary being
    // reachable from this test binary.
    nlohmann::json primeBody{{"varlist", nlohmann::json::array({"mpg"})}};
    auto primeResult = piet::routes::sortRoute(session, primeBody.dump());
    check(primeResult.ok, "construct-count gate setup: priming `sort mpg` returns ok=true");
    session.invalidateDatastore();

    piet::Datastore::resetConstructCount();
    nlohmann::json measuredBody{{"varlist", nlohmann::json::array({"mpg"})}};
    auto measuredResult = piet::routes::sortRoute(session, measuredBody.dump());
    check(measuredResult.ok, "construct-count gate: measured `sort mpg` returns ok=true");
    check(piet::Datastore::constructCount() == 1,
          "MAXPERF sort-validate-lazy gate: a `sort mpg` whose shadow card is ALREADY valid "
          "for the current hbin (but whose cached Datastore* is cold, matching a real "
          "resident-chain sort's actual precondition) constructs exactly ONE Datastore end to "
          "end — mutationResult's own unavoidable card rebuild here (this test stays off "
          "dispatch()'s resident-frame short-circuit, so that ONE rebuild cannot itself be "
          "eliminated). A count of 2 means runSort's own varlist-name validation loop is "
          "STILL eagerly re-opening the hbin instead of consulting "
          "session.shadowVarlistIfCurrent() first — reintroducing the datastore+has cost this "
          "fix targets.");
  }
  {
    // Card-tax gate sibling: `gsort` (descending is its native `-var`
    // spelling) delegates to handlers_data_verbs.cpp's runGsort — a SEPARATE
    // mutationResult copy from handlers_m3.cpp's runSort just gated above.
    // Reused via sortRoute (not the "command" route) to stay off dispatch()'s
    // resident-frame short-circuit, same as the setup above.
    nlohmann::json sb3{{"varlist", nlohmann::json::array({"mpg"})},
                        {"ascending", nlohmann::json::array({false})}};
    auto rs3 = piet::routes::sortRoute(session, sb3.dump());
    check(rs3.ok, "card-tax gate setup: `gsort -mpg` returns ok=true");
    piet::Datastore* ds = session.datastore();  // BUG-037: re-fetch, never reuse.
    check(ds != nullptr, "card-tax gate setup: session.datastore() non-null after `gsort`");
    if (ds != nullptr) {
      check(ds->fullScanCount() == 0,
            "card-tax gate: `gsort` (handlers_data_verbs.cpp's mutationResult, via runGsort) "
            "consumes the L4 carry-forward seed with ZERO real nMissing() rescans — same "
            "ordering fix, the copy `sort` above does NOT exercise");
    }
  }
  {
    // Same copy, the OTHER call site: `order` permutes COLUMNS rather than
    // rows, via runOrder. No bridge route exists for it, so call the handler
    // directly — still dispatch()-free, same resident-plane reasoning.
    auto orderResult = piet::handlers::runOrder({"mpg"}, {}, session);
    check(orderResult.value("type", std::string()) == "data_mutation",
          "card-tax gate setup: `order mpg` returns a data_mutation result");
    piet::Datastore* ds = session.datastore();  // BUG-037: re-fetch, never reuse.
    check(ds != nullptr, "card-tax gate setup: session.datastore() non-null after `order`");
    if (ds != nullptr) {
      check(ds->fullScanCount() == 0,
            "card-tax gate: `order` (handlers_data_verbs.cpp's mutationResult, via runOrder) "
            "consumes the L4 carry-forward seed with ZERO real nMissing() rescans — same copy "
            "as `gsort` above, different call site");
    }
  }
  {
    // Sibling to the three gates above, but for seedNMissForPureAdd branch
    // (2) (session_state.cpp: action=="gen" && newVars.empty() &&
    // !rewrittenVars.empty()) instead of their shared branch (5). runReplace
    // (handlers.cpp) passes `varname` as mutationResult's 5th (rewrittenVars)
    // argument; branch 2 erases exactly that column from the carried seed
    // (its value just changed, so its old count is untrustworthy) and seeds
    // every other column — ONE real rescan, not zero. Prime with another
    // `order` first so nMissCache_ is warm for every column beforehand.
    auto primeResult = piet::handlers::runOrder({"mpg"}, {}, session);
    check(primeResult.value("type", std::string()) == "data_mutation",
          "replace card-tax gate setup: priming `order mpg` returns a data_mutation result");
    // Direct handler call, off dispatch()/routes::command: dispatch() would
    // let tryResidentMutationCard fold a resident delta into the shadow card
    // before Datastore is ever rebuilt, holding fullScanCount() steady
    // regardless of which slot rewrittenVars landed in — a tautology.
    auto replaceResult = piet::handlers::runReplace("mpg = mpg + 0", std::nullopt, session);
    check(replaceResult.value("type", std::string()) == "data_mutation",
          "card-tax gate setup: `replace mpg = mpg + 0` returns a data_mutation result");
    piet::Datastore* ds = session.datastore();  // BUG-037: re-fetch, never reuse.
    check(ds != nullptr, "card-tax gate setup: session.datastore() non-null after `replace`");
    if (ds != nullptr) {
      check(ds->fullScanCount() == 1,
            "card-tax gate: `replace` (handlers.cpp's runReplace, via the shared "
            "mutationResult) reaches seedNMissForPureAdd branch (2), carrying every "
            "column except the one just rewritten (fullScanCount()==1: mpg's own "
            "forced fresh scan) — a count equal to the full column count means "
            "rewrittenVars arrived empty (`mpg` misrouted into droppedVars, "
            "mutation_result.h's 3rd slot, instead of its 5th) and branch 2 never "
            "fired");
    }
    // NMISS-CARRY: restore. Every assertion above measured the seed mechanism
    // with the header carry off, which is the only way that counter still
    // discriminates.
    ::unsetenv("PIET_NO_NMISS_CARRY");
  }
  {
    nlohmann::json body{{"varname", "rep78"}, {"rules", "1/2=1 3/5=2"}, {"gen", "rep78_recoded"}};
    auto r = piet::routes::recode(session, body.dump());
    check(r.ok, "recode route returns ok=true");
    auto j = nlohmann::json::parse(r.json);
    bool hasNewVar = false;
    for (const auto& v : j.value("varlist_after", nlohmann::json::array())) {
      if (v.value("name", std::string()) == "rep78_recoded") hasNewVar = true;
    }
    check(hasNewVar, "recode route with gen() creates the new variable");
  }

  // cell — f64 write.
  {
    nlohmann::json body{{"varname", "mpg"}, {"row", 0}, {"value", 99.0}};
    auto r = piet::routes::cell(session, body.dump());
    check(r.ok, "cell route (f64 write) returns ok=true");
    auto j = nlohmann::json::parse(r.json);
    check(j.value("action", std::string()) == "cell_write",
          "cell route response has action:\"cell_write\"");
    // Verify the write actually landed: browse row 0's mpg column.
    nlohmann::json browseBody{{"offset", 0}, {"limit", 1}, {"vars", nlohmann::json::array({"mpg"})}};
    auto br = piet::routes::browse(session, browseBody.dump());
    auto bj = nlohmann::json::parse(br.json);
    double written = bj["rows"][0][0].get<double>();
    check(std::abs(written - 99.0) < 1e-9, "cell route write is observable via a subsequent browse (mpg[0]=99)");
  }
  {
    // factor-column write -> documented piet_gap, not a silent wrong write.
    // `encode make, gen(...)` (M4a) converts the SOURCE string column
    // itself to a genuine dtype=="factor" hbin column (see handlers_
    // data_verbs.cpp's runEncode: the string var's levels are dictionary-
    // encoded in place) — `make` after encode is exactly such a column,
    // reused here rather than `make_f` (which stays f64 + a value-label
    // attachment, a different case entirely — see BUG-025).
    nlohmann::json encodeBody{{"cmd", "encode make, gen(make_f)"}};
    auto encodeResult = piet::routes::command(session, encodeBody.dump());
    auto ej = nlohmann::json::parse(encodeResult.json);
    bool encodeOk = true;
    for (const auto& b : ej.value("blocks", nlohmann::json::array())) {
      if (b.value("type", std::string()) == "error") encodeOk = false;
    }
    if (!encodeOk) {
      skip("cell route (factor write)", "encode make->make_f failed, cannot construct a factor column");
    } else {
      // The written value is an arbitrary synthetic string — the route must
      // reject the factor write regardless of the value's content.
      nlohmann::json body{{"varname", "make"}, {"row", 0}, {"value", "Sample Level"}};
      auto r = piet::routes::cell(session, body.dump());
      check(r.ok, "cell route (factor write) still returns ok=true (200+error, never a bare reject)");
      auto j = nlohmann::json::parse(r.json);
      check(j.value("reason", std::string()) == "piet_gap",
            "cell route (factor write) is an explicit piet_gap, not a silent no-op or wrong write");
    }
  }

  // history — every command() call above should have logged something IF
  // dispatch() itself calls session.addHistory() (M2 behavior); route just
  // needs to not crash and return the right shape regardless of count.
  {
    auto r = piet::routes::history(session, "{}");
    check(r.ok, "history route returns ok=true");
    auto j = nlohmann::json::parse(r.json);
    check(j.contains("history") && j["history"].is_array(), "history route response has a history array");
  }

  // do_string — the async-ROUTE'S UNDERLYING SYNCHRONOUS PATH (routes::
  // doString, used directly by the headless run_block gate / --run-block;
  // main.cpp's live UI instead uses the async bindAsyncRoute wrapper around
  // doexec::executeDoSource on a worker thread — see the Node shim test
  // below for that contract, and doexec.cpp's own corpus coverage for the
  // execution semantics itself, already gated by debug_all_replay.py).
  {
    nlohmann::json body{{"source", "display 1+1\n"}};
    auto r = piet::routes::doString(session, body.dump());
    check(r.ok, "do_string route returns ok=true");
    auto j = nlohmann::json::parse(r.json);
    check(j.contains("blocks") && j["blocks"].is_array() && !j["blocks"].empty(),
          "do_string route response has a non-empty blocks array");
  }

  // run_progress / run_cancel — idle-state shape (no run currently owns
  // progressStart; do_string above already called progressFinish()).
  {
    auto r = piet::routes::runProgress(session, "{}");
    check(r.ok, "run_progress route returns ok=true");
    auto j = nlohmann::json::parse(r.json);
    for (const char* field : {"running", "current_line", "total_lines", "current_cmd", "elapsed_ms",
                               "blocks_ready", "do_path", "cancel_requested", "stopped_reason"}) {
      check(j.contains(field), std::string("run_progress route response has field '") + field + "'");
    }
    check(j.value("running", true) == false, "run_progress route reports running=false after do_string completed");
  }
  {
    auto r = piet::routes::runCancel(session, "{}");
    check(r.ok, "run_cancel route returns ok=true");
    auto j = nlohmann::json::parse(r.json);
    check(j.value("cancelled", true) == false,
          "run_cancel route reports cancelled=false when no run is in flight");
  }

  // Real cancel-in-flight proof: a longer-running do_string on a background
  // thread, cancelled mid-run via run_cancel from THIS (main) thread —
  // exercising the exact concurrent-access pattern main.cpp's live UI uses
  // (one thread runs doexec, another thread polls/cancels through the
  // mutex-guarded SessionState::progress_).
  {
    std::string longSource;
    for (int i = 0; i < 50; ++i) longSource += "display " + std::to_string(i) + "\n";
    std::thread runner([&]() {
      nlohmann::json body{{"source", longSource}};
      piet::routes::doString(session, body.dump());
    });
    // Give the runner a moment to start, then cancel.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto cancelResult = piet::routes::runCancel(session, "{}");
    runner.join();
    // A cancel that lands while the run is still going (a loaded machine) leaves
    // progress_.cancelRequested set after the run stops: only progressStart clears it.
    // The C4-L3 gates below call executeDoSource with progressOwner=false, which
    // checks that flag before every item, so they stopped at their first one (six
    // FAILs whenever this race went that way). An empty run clears it.
    session.progressStart(0);
    session.progressFinish("selftest");
    auto cj = nlohmann::json::parse(cancelResult.json);
    // cancelled may be false if the 50-line run finished before the 5ms
    // sleep elapsed (legitimately fast on this hardware) — only assert the
    // route DIDN'T crash and returned the right shape; the cancel
    // MECHANISM itself (progressCancelRequested() checked between items) is
    // already gated by doexec.cpp's own corpus coverage.
    check(cancelResult.ok, "run_cancel route (racing a live do_string) returns ok=true without crashing");
    (void)cj;
  }

  // -------------------------------------------------------------------------
  // MAXPERF gates (fix/verb-overhead-sweep): extend the fix/sort-validate-
  // lazy mechanism (session.shadowVarlistIfCurrent(), Datastore::
  // constructCount() instrumentation — both in datastore.h/session_state.h,
  // both already proven by the sort construct-count gate above) from runSort
  // to drop/keep/gsort/order: handlers_m3.cpp (drop/keep) and
  // handlers_data_verbs.cpp (gsort/order) each now consult the resident
  // shadow card before falling back to session.datastore() purely to
  // validate column NAMES.
  //
  // Every leg below reproduces the SAME cold-entry precondition the sort
  // construct-count gate above uses: prime with the verb once, then an
  // explicit session.invalidateDatastore() before the MEASURED call. This is
  // deliberately the CHAIN shape (the entry is cold because something
  // upstream just repointed the session onto a new file) and NOT a single-
  // call-fresh-after-`use` shape — a fresh-after-`use` call starts warm
  // either way (session.datastore() already built by `use`) and would read
  // the same construct count on fixed and unfixed code alike: a check that
  // cannot fail is not a gate.
  //
  // All four measured calls stay off dispatch() — direct piet::handlers::
  // calls, or routes::sortRoute for gsort (routes.cpp:401-434 calls
  // handlers::runSort/runGsort directly, no dispatch() in between; the same
  // narrow bridge the gsort/order card-tax gates above already use) — so
  // pendingDeltaValid_ is always false and tryResidentMutationCard misses on
  // its very first guard (session_state.cpp: `if (!pendingDeltaValid_)
  // return std::nullopt;`) regardless of the verb's own card_stable signal.
  // That means mutationResult's non-resident fallback (session.
  // invalidateDatastore() + session.datastore(), ONE unavoidable rebuild of
  // the just-written file) fires in every leg below, on BOTH fixed and
  // unfixed code — which is exactly why each assertion is ==1, not ==0:
  //   unfixed: invalidateDatastore() forces the entry cold -> the verb's own
  //     validation loop calls session.datastore() (construct #1) ->
  //     session.load() resets datastore_ again -> mutationResult's rebuild
  //     constructs #2. Total 2.
  //   fixed: the entry consults session.shadowVarlistIfCurrent() instead (0
  //     constructs) -> mutationResult's rebuild still constructs #1 (the
  //     unavoidable one). Total 1.
  // So each leg catches exactly one thing — a regression of THIS fix's
  // entry-side check back to eagerly reopening the hbin — and catches
  // nothing else. None of them can catch (or reduce) the unavoidable rebuild
  // itself: for order and drop specifically that rebuild is foldable in
  // principle (a permutation changes no column's name/type/n_missing/
  // n_levels; a drop only needs entries removed, not a full reopen), but
  // that fold logic belongs in mutation_result.cpp — out of scope for this
  // file and not attempted here.
  {
    // drop: destructive, so priming and measuring must target DIFFERENT
    // columns (the primed one is gone by the time the measured call runs).
    // Re-`use` the fixture first (BUG catch: an earlier draft of this leg
    // inherited whatever the PREVIOUS leg's mutations left the frame in —
    // the `keep` leg below strips every column but its own three scratch
    // ones, and authoring order alone made that invisible here; re-loading
    // makes every leg independent of what runs before it, so a future leg
    // inserted between these four can't silently break the ones after it).
    // Two scratch columns purpose-built for this gate, created via a direct
    // piet::handlers::runGen call (bypasses dispatch(), so the write is
    // always a real on-disk `eval --gen` — never a virtual resident head
    // this test would then fail to read back).
    nlohmann::json openBody{{"filepath", fixture}};
    auto openResult = piet::routes::open(session, openBody.dump());
    check(openResult.ok, "drop construct-count gate setup: re-`use` fixture returns ok=true");

    auto genA = piet::handlers::runGen("__gate_drop_a = 0", std::nullopt, session);
    check(genA.value("type", std::string()) == "data_mutation",
          "drop construct-count gate setup: gen __gate_drop_a returns a data_mutation result");
    auto genB = piet::handlers::runGen("__gate_drop_b = 0", std::nullopt, session);
    check(genB.value("type", std::string()) == "data_mutation",
          "drop construct-count gate setup: gen __gate_drop_b returns a data_mutation result");

    auto primeResult = piet::handlers::runDrop({"__gate_drop_a"}, session);
    check(primeResult.value("type", std::string()) == "data_mutation",
          "drop construct-count gate setup: priming `drop __gate_drop_a` returns a data_mutation "
          "result");
    session.invalidateDatastore();

    piet::Datastore::resetConstructCount();
    auto measuredResult = piet::handlers::runDrop({"__gate_drop_b"}, session);
    check(measuredResult.value("type", std::string()) == "data_mutation",
          "drop construct-count gate: measured `drop __gate_drop_b` returns a data_mutation "
          "result");
    check(piet::Datastore::constructCount() == 1,
          "MAXPERF verb-overhead-sweep gate: `drop`'s entry validation (handlers_m3.cpp) "
          "constructs ZERO Datastores when the shadow card is already valid for the current "
          "hbin — the ONE construction that survives is mutationResult's own unavoidable "
          "non-resident rebuild (drop unconditionally marks the engine's Frame structure_dirty_, "
          "src/data/frame.cpp Frame::drop(), so that rebuild can never be skipped here or in "
          "production). A count of 2 means runDrop's own varlist-name validation loop is STILL "
          "eagerly re-opening the hbin instead of consulting session.shadowVarlistIfCurrent() "
          "first — reintroducing the cost this fix targets.");
  }
  {
    // keep: same destructive-verb shape as drop, but keep's own varlist IS
    // the surviving set — priming keep{a,b,c} drops every OTHER column,
    // including all of auto.hbin's own AND anything an earlier leg added
    // (e.g. drop's two scratch columns, if they ever survived that leg).
    // Re-`use` the fixture first — this leg's own destructiveness is
    // exactly why the gsort/order legs below it re-`use` too, rather than
    // trust that keep left `mpg` alone. Three scratch columns so the
    // measured call (keep{a,b}) still has a real column (c) to drop,
    // exercising the actual drop-something path rather than a degenerate
    // empty-toDrop no-op.
    {
      nlohmann::json openBody{{"filepath", fixture}};
      auto openResult = piet::routes::open(session, openBody.dump());
      check(openResult.ok, "keep construct-count gate setup: re-`use` fixture returns ok=true");
    }
    for (const char* nm : {"__gate_keep_a", "__gate_keep_b", "__gate_keep_c"}) {
      auto genResult = piet::handlers::runGen(std::string(nm) + " = 0", std::nullopt, session);
      check(genResult.value("type", std::string()) == "data_mutation",
            std::string("keep construct-count gate setup: gen ") + nm +
                " returns a data_mutation result");
    }

    auto primeResult =
        piet::handlers::runKeep({"__gate_keep_a", "__gate_keep_b", "__gate_keep_c"}, session);
    check(primeResult.value("type", std::string()) == "data_mutation",
          "keep construct-count gate setup: priming `keep __gate_keep_a __gate_keep_b "
          "__gate_keep_c` returns a data_mutation result");
    session.invalidateDatastore();

    piet::Datastore::resetConstructCount();
    auto measuredResult = piet::handlers::runKeep({"__gate_keep_a", "__gate_keep_b"}, session);
    check(measuredResult.value("type", std::string()) == "data_mutation",
          "keep construct-count gate: measured `keep __gate_keep_a __gate_keep_b` (drops "
          "__gate_keep_c) returns a data_mutation result");
    check(piet::Datastore::constructCount() == 1,
          "MAXPERF verb-overhead-sweep gate: `keep`'s entry validation (handlers_m3.cpp) — both "
          "its full-column enumeration (building toDrop) and its has-check — constructs ZERO "
          "Datastores when the shadow card is already valid; the ONE surviving construction is "
          "mutationResult's own unavoidable non-resident rebuild (keep is cobra's `eval --drop` "
          "under the hood, same engine-side structure_dirty_ as plain drop). A count of 2 means "
          "runKeep regressed back to eagerly re-opening the hbin.");
  }
  {
    // gsort: non-destructive (row reorder only) — priming/measuring safely
    // reuse the same variable, same convention as the gsort card-tax gate
    // above. Re-`use` first regardless: the `keep` leg immediately above
    // this one strips every column but its own three scratch ones, so
    // `mpg` (referenced below) would not exist without this — same
    // independence-from-authoring-order reasoning as the drop/keep legs.
    nlohmann::json openBody{{"filepath", fixture}};
    auto openResult = piet::routes::open(session, openBody.dump());
    check(openResult.ok, "gsort construct-count gate setup: re-`use` fixture returns ok=true");

    nlohmann::json primeBody{{"varlist", nlohmann::json::array({"mpg"})},
                             {"ascending", nlohmann::json::array({false})}};
    auto primeResult = piet::routes::sortRoute(session, primeBody.dump());
    check(primeResult.ok, "gsort construct-count gate setup: priming `gsort -mpg` returns ok=true");
    session.invalidateDatastore();

    piet::Datastore::resetConstructCount();
    nlohmann::json measuredBody{{"varlist", nlohmann::json::array({"mpg"})},
                                {"ascending", nlohmann::json::array({false})}};
    auto measuredResult = piet::routes::sortRoute(session, measuredBody.dump());
    check(measuredResult.ok, "gsort construct-count gate: measured `gsort -mpg` returns ok=true");
    check(piet::Datastore::constructCount() == 1,
          "MAXPERF verb-overhead-sweep gate: `gsort`'s entry validation (handlers_data_verbs."
          "cpp's runGsort) constructs ZERO Datastores when the shadow card is already valid; "
          "the ONE surviving construction is mutationResult's own rebuild, forced here because "
          "this leg calls off dispatch() (pendingDeltaValid_ is never true regardless of "
          "gsort's own card_stable=true signal) — in real dispatch()-driven usage with a live "
          "resident worker that rebuild is instead SKIPPED entirely (gsort/sort's Frame-level "
          "take_rows never marks structure_dirty_), which is why gsort/sort measure cheapest of "
          "all six verbs in production once the entry-side cost this fix removes is gone. A "
          "count of 2 here means runGsort regressed back to eagerly re-opening the hbin.");
  }
  {
    // order: non-destructive (column reorder only) — same reuse-the-same-
    // variable convention, direct handler call (no bridge route exists for
    // order), same as the order card-tax gate above. Re-`use` first, same
    // reasoning as the gsort leg immediately above (this leg also needs
    // `mpg`, which the `keep` leg two blocks back would otherwise have
    // removed) — every leg below re-establishes its own frame rather than
    // trusting what the previous one left behind.
    nlohmann::json openBody{{"filepath", fixture}};
    auto openResult = piet::routes::open(session, openBody.dump());
    check(openResult.ok, "order construct-count gate setup: re-`use` fixture returns ok=true");

    auto primeResult = piet::handlers::runOrder({"mpg"}, {}, session);
    check(primeResult.value("type", std::string()) == "data_mutation",
          "order construct-count gate setup: priming `order mpg` returns a data_mutation result");
    session.invalidateDatastore();

    piet::Datastore::resetConstructCount();
    auto measuredResult = piet::handlers::runOrder({"mpg"}, {}, session);
    check(measuredResult.value("type", std::string()) == "data_mutation",
          "order construct-count gate: measured `order mpg` returns a data_mutation result");
    check(piet::Datastore::constructCount() == 1,
          "MAXPERF verb-overhead-sweep gate: `order`'s entry validation (handlers_data_verbs."
          "cpp) constructs ZERO Datastores when the shadow card is already valid; the ONE "
          "surviving construction is mutationResult's own unavoidable non-resident rebuild — "
          "order's own Frame-level mutation (src/data/frame.cpp Frame::order(), rvalue "
          "overload) unconditionally marks structure_dirty_=true (\"a column permutation can't "
          "be patched onto the source\"), so card_stable reports false on EVERY call and this "
          "rebuild can never be skipped via the resident fast path, in production or here. A "
          "count of 2 means runOrder's own varlist/anchor validation regressed back to eagerly "
          "re-opening the hbin — the ~1.02s/call this campaign measured before this fix.");
  }
  {
    // S4 sibling legs: `rename` and `encode`. Same entry-side mechanism and the
    // same ==1 arithmetic as the four legs above (both verbs run STATELESS — no
    // ResidentEligible opt-in — so mutationResult's non-resident rebuild is the
    // ONE construction that survives on fixed code, and an unfixed entry makes
    // it 2).
    //
    // WHAT THESE LEGS DO NOT CLAIM. Neither verb's measured per-call cost is
    // caused by the open these legs protect, and saying otherwise here would be
    // the stale-comment failure this campaign keeps having to correct:
    //   • rename (~0.19 s/call): dominated by the ENGINE writing the whole
    //     164 MB out to change one header string — Frame::rename sets
    //     structure_dirty_, which disqualifies write_hbin's header-patch path
    //     (src/data/frame.cpp). Not fixable from piet/native at all.
    //   • encode (~0.24 s/call): dominated by mutationResult's rebuild
    //     rescanning all 192 columns for n_missing, because "encode" is absent
    //     from seedNMissForPureAdd's kPureAdd set (session_state.cpp) despite
    //     being a textbook pure add. A one-token fix in a file this stint does
    //     not own.
    // Both are filed for their owners. These legs gate only the piece that was
    // in scope and is now fixed, which is exactly what a mechanism gate should
    // assert and no more.
    nlohmann::json openBody{{"filepath", fixture}};
    auto openResult = piet::routes::open(session, openBody.dump());
    check(openResult.ok, "rename/encode construct-count gate setup: re-`use` fixture ok=true");

    auto primeRename = piet::handlers::runRename("mpg", "__gate_mpg1", session);
    check(primeRename.value("type", std::string()) == "data_mutation",
          "rename construct-count gate setup: priming `rename mpg __gate_mpg1` returns a "
          "data_mutation result");
    session.invalidateDatastore();

    piet::Datastore::resetConstructCount();
    auto measuredRename = piet::handlers::runRename("__gate_mpg1", "__gate_mpg2", session);
    check(measuredRename.value("type", std::string()) == "data_mutation",
          "rename construct-count gate: measured `rename __gate_mpg1 __gate_mpg2` returns a "
          "data_mutation result");
    check(piet::Datastore::constructCount() == 1,
          "MAXPERF S4 gate: `rename`'s entry validation (handlers_data_verbs.cpp runRename) "
          "constructs ZERO Datastores when the shadow card is valid — it asks only whether one "
          "name exists and another does not. The ONE surviving construction is mutationResult's "
          "rebuild, which rename cannot avoid (Frame::rename sets structure_dirty_, so "
          "card_stable is false on every call). A count of 2 means the entry went back to "
          "opening the hbin to read a name list");

    // encode needs a genuine string column, and it must still be there: the
    // rename leg above renamed mpg, not make. Priming and measuring target
    // DIFFERENT new columns since encode refuses an existing target.
    auto primeEncode =
        piet::handlers::runEncode("make", "__gate_enc1", std::nullopt, session, std::nullopt);
    check(primeEncode.value("type", std::string()) == "data_mutation",
          "encode construct-count gate setup: priming `encode make, gen(__gate_enc1)` returns a "
          "data_mutation result — if this fails, `make` is no longer a string column in the "
          "fixture and the leg below is not measuring what it claims");
    session.invalidateDatastore();

    piet::Datastore::resetConstructCount();
    auto measuredEncode =
        piet::handlers::runEncode("make", "__gate_enc2", std::nullopt, session, std::nullopt);
    check(measuredEncode.value("type", std::string()) == "data_mutation",
          "encode construct-count gate: measured `encode make, gen(__gate_enc2)` returns a "
          "data_mutation result");
    check(piet::Datastore::constructCount() == 1,
          "MAXPERF S4 gate: `encode`'s entry validation (handlers_data_verbs.cpp runEncode) "
          "constructs ZERO Datastores when the shadow card is valid — name, DTYPE and name are "
          "all card-answerable, the dtype half being S4a's treatment applied at a second site. "
          "The ONE surviving construction is mutationResult's rebuild. A count of 2 means the "
          "entry regressed to opening the hbin");
  }

  // -------------------------------------------------------------------------
  // MAXPERF FOLD gates (fix/verb-overhead-sweep S1b) — the OTHER half of the
  // four legs above. Those cover each verb's ENTRY-side name lookup and are
  // explicit that the post-mutation rebuild "can never be skipped"; that is no
  // longer true for order/drop/keep. Their cards are now produced by an
  // incremental FOLD of the resident shadow card (session_state.cpp
  // applyPendingCardPlan): a permutation MOVES card entries and a drop REMOVES
  // them, and neither can change any surviving column's name, type, n_missing
  // or n_levels, so nothing has to be re-read from the frame.
  //
  // These three legs differ structurally from the four above in one way: they
  // MUST run through dispatch(). The fold consumes the pending §26 frame delta,
  // and only a dispatch-active session installs the runCobra intercept that
  // produces one (cobra_runner.h pushActiveSession, driven by
  // SessionState::enterDispatch) — off dispatch every verb runs one-shot,
  // pendingDeltaValid_ is never true, and there is simply nothing to fold.
  //
  // WHY THESE ARE MECHANISM GATES, not value pins: SessionState::cardFoldCount()
  // counts cards produced by a fold rather than a rebuild. Disable the fold in
  // ANY way — drop the capturePendingCardPlan() call from runViaCobraSession,
  // let one applyPendingCardPlan guard decline wrongly, change what the engine
  // reports for these verbs — and the counter reads 0 while every
  // varlist_after in these legs stays perfectly, identically correct: the
  // rebuild computes the same card, just by re-materializing the virtual head
  // and re-scanning every surviving column (~1.02 s/order, ~0.80 s/drop on the
  // 108K×192 corpus this campaign measured). So the fold-count checks are the
  // ONLY thing here that can catch a silently dead optimization, and the
  // permutation/removal checks beside them are the only thing that can catch a
  // fold that fires but folds WRONG. Both classes are needed; neither
  // substitutes for the other. Datastore::constructCount()==0 states the same
  // claim from the other side: with the fold live nothing re-opens the
  // (virtual) head at all, so even ONE construction means the rebuild ran.
  auto mutationBlock = [](const nlohmann::ordered_json& blocks) -> nlohmann::ordered_json {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "data_mutation") return b;
    }
    return nlohmann::ordered_json::object();
  };
  auto cardNames = [](const nlohmann::ordered_json& blk) {
    std::vector<std::string> names;
    for (const auto& v : blk.value("varlist_after", nlohmann::ordered_json::array())) {
      names.push_back(v.value("name", std::string()));
    }
    return names;
  };
  auto joinNames = [](const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
      if (i) s += ",";
      s += v[i];
    }
    return s;
  };
  auto cardNMissing = [](const nlohmann::ordered_json& blk, const std::string& name) -> int64_t {
    for (const auto& v : blk.value("varlist_after", nlohmann::ordered_json::array())) {
      if (v.value("name", std::string()) == name) return v.value("n_missing", int64_t{-1});
    }
    return -1;
  };
  {
    // order — a pure column PERMUTATION.
    nlohmann::json openBody{{"filepath", fixture}};
    auto openResult = piet::routes::open(session, openBody.dump());
    check(openResult.ok, "order fold gate setup: re-`use` fixture returns ok=true");

    // Priming call: this is what frame-`use`s the head into the worker (the
    // resync inside runViaCobraSession) and so SEEDS the shadow card the
    // measured call folds. Without it the measured call would resync inside
    // itself — correct, but its entry-side lookup would find no current card
    // and open a Datastore, which is the OTHER fix's cost, not this one's.
    auto primeBlk = mutationBlock(piet::dispatch("order mpg", session));
    auto before = cardNames(primeBlk);
    check(before.size() > 2 && before.front() == "mpg",
          "order fold gate setup: priming `order mpg` through dispatch() returns a card with mpg "
          "first");
    check(session.residentFrameLive(),
          "order fold gate PRECONDITION: the section 26 resident plane is live after a dispatch-driven "
          "`order` (SessionState::residentFrameLive()). FALSE here means this environment has no "
          "frames-capable `cobra serve` at all — there is then no frame delta for ANY of the "
          "three fold gates below to consume, so read their failures as an ENVIRONMENT result, "
          "not as a regression of the fold");

    session.invalidateDatastore();
    piet::Datastore::resetConstructCount();
    piet::SessionState::resetCardFoldCount();
    auto blk = mutationBlock(piet::dispatch("order rep78", session));
    auto after = cardNames(blk);

    check(piet::SessionState::cardFoldCount() == 1,
          "MAXPERF S1b fold gate: `order rep78` produces its card by ONE incremental fold of the "
          "shadow card (SessionState::cardFoldCount()==1), never by mutationResult's rebuild. A "
          "count of 0 means the fold is dead — the card below is still correct, which is exactly "
          "why this counter and not the card is the gate");
    check(piet::Datastore::constructCount() == 0,
          "MAXPERF S1b fold gate: a folded `order` constructs ZERO Datastores end to end — it "
          "never materializes the virtual head, never re-mmaps it, never rescans a column. Any "
          "count means the fold declined and the full rebuild ran");
    std::vector<std::string> expected{"rep78"};
    for (const auto& n : before) {
      if (n != "rep78") expected.push_back(n);
    }
    check(after == expected,
          "MAXPERF S1b fold gate: the folded `order rep78` card is the EXACT permutation cobra's "
          "Frame::order_names would produce (rep78 first, every other column in its previous "
          "relative order) — got [" + joinNames(after) + "], expected [" + joinNames(expected) +
              "]");
    check(cardNMissing(blk, "rep78") == 5,
          "MAXPERF S1b fold gate: the folded `order` card carries rep78's n_missing==5 through "
          "unchanged — a permutation cannot change a missing count, and the fold must move the "
          "metadata, not drop or recompute it");
  }
  {
    // drop — a pure column REMOVAL. Scratch columns are created with a DIRECT
    // handlers::runGen (off dispatch, so the write is a real on-disk `eval
    // --gen`), same reason the construct-count legs above give; the priming
    // and measured drops then go through dispatch() because only there does a
    // foldable delta exist. Priming and measuring target DIFFERENT columns —
    // the primed one is gone by the time the measured call runs.
    nlohmann::json openBody{{"filepath", fixture}};
    auto openResult = piet::routes::open(session, openBody.dump());
    check(openResult.ok, "drop fold gate setup: re-`use` fixture returns ok=true");
    for (const char* nm : {"__fold_drop_a", "__fold_drop_b"}) {
      auto genResult = piet::handlers::runGen(std::string(nm) + " = 0", std::nullopt, session);
      check(genResult.value("type", std::string()) == "data_mutation",
            std::string("drop fold gate setup: gen ") + nm + " returns a data_mutation result");
    }
    auto primeBlk = mutationBlock(piet::dispatch("drop __fold_drop_b", session));
    auto before = cardNames(primeBlk);
    check(std::find(before.begin(), before.end(), "__fold_drop_a") != before.end(),
          "drop fold gate setup: priming `drop __fold_drop_b` leaves __fold_drop_a in the card");

    session.invalidateDatastore();
    piet::Datastore::resetConstructCount();
    piet::SessionState::resetCardFoldCount();
    auto blk = mutationBlock(piet::dispatch("drop __fold_drop_a", session));
    auto after = cardNames(blk);

    check(piet::SessionState::cardFoldCount() == 1,
          "MAXPERF S1b fold gate: `drop __fold_drop_a` produces its card by ONE incremental fold "
          "(cardFoldCount()==1) — the engine's own dropped-count is the checksum that lets the "
          "argv-derived removal list be trusted. A count of 0 means the fold is dead");
    check(piet::Datastore::constructCount() == 0,
          "MAXPERF S1b fold gate: a folded `drop` constructs ZERO Datastores — no head "
          "materialization, no re-mmap, no rescan of the 191 surviving columns");
    std::vector<std::string> expected;
    for (const auto& n : before) {
      if (n != "__fold_drop_a") expected.push_back(n);
    }
    check(after == expected,
          "MAXPERF S1b fold gate: the folded `drop` card is the previous card MINUS exactly the "
          "dropped column, survivors in their previous relative order — got [" + joinNames(after) +
              "], expected [" + joinNames(expected) + "]");
    check(cardNMissing(blk, "rep78") == 5,
          "MAXPERF S1b fold gate: the folded `drop` card carries rep78's n_missing==5 through "
          "unchanged — dropping a DIFFERENT column cannot change it");
  }
  {
    // keep — the same engine call as drop (cobra `eval --drop` on the
    // complement, computed PIET-side by runKeep), so this leg proves the fold
    // survives keep's complement path, not just drop's direct one.
    nlohmann::json openBody{{"filepath", fixture}};
    auto openResult = piet::routes::open(session, openBody.dump());
    check(openResult.ok, "keep fold gate setup: re-`use` fixture returns ok=true");
    for (const char* nm : {"__fold_keep_a", "__fold_keep_b"}) {
      auto genResult = piet::handlers::runGen(std::string(nm) + " = 0", std::nullopt, session);
      check(genResult.value("type", std::string()) == "data_mutation",
            std::string("keep fold gate setup: gen ") + nm + " returns a data_mutation result");
    }
    auto primeBlk =
        mutationBlock(piet::dispatch("keep mpg rep78 __fold_keep_a __fold_keep_b", session));
    auto before = cardNames(primeBlk);
    check(before.size() == 4,
          "keep fold gate setup: priming `keep mpg rep78 __fold_keep_a __fold_keep_b` leaves "
          "exactly those four columns — got [" + joinNames(before) + "]");

    session.invalidateDatastore();
    piet::Datastore::resetConstructCount();
    piet::SessionState::resetCardFoldCount();
    auto blk = mutationBlock(piet::dispatch("keep mpg rep78", session));
    auto after = cardNames(blk);

    check(piet::SessionState::cardFoldCount() == 1,
          "MAXPERF S1b fold gate: `keep mpg rep78` produces its card by ONE incremental fold "
          "(cardFoldCount()==1) — keep reaches the fold through the SAME `eval --drop` argv shape "
          "as drop, via the complement runKeep computes. A count of 0 means the fold is dead");
    check(piet::Datastore::constructCount() == 0,
          "MAXPERF S1b fold gate: a folded `keep` constructs ZERO Datastores");
    std::vector<std::string> expected;
    for (const auto& n : before) {
      if (n == "mpg" || n == "rep78") expected.push_back(n);
    }
    check(after == expected,
          "MAXPERF S1b fold gate: the folded `keep` card holds exactly the kept columns in their "
          "previous relative order (keep is a drop of the complement, so it must NOT reorder to "
          "match the keep list) — got [" + joinNames(after) + "], expected [" +
              joinNames(expected) + "]");
    check(cardNMissing(blk, "rep78") == 5,
          "MAXPERF S1b fold gate: the folded `keep` card carries rep78's n_missing==5 through "
          "unchanged");
  }

  // -------------------------------------------------------------------------
  // FIX-C4COUNTER-SAMPLERSPAN / BUG-142 gate — the diagnostic byte walks
  // (liveNamedBytesPeakSample / liveNamedBytesEndSnapshot) are neutral for
  // EVERY emitted counter even when the walk itself materialises a DIRTY
  // virtual head (a real frameSnapshot — the C4-L1B §5(b) repro shape:
  // disk `use` → resident numeric mutation → nothing after consumes the
  // datastore, so the window is still open when the end-of-run walk runs).
  // Pre-fix, that walk half-recorded its own closure: the emit block captures
  // snapshot_count/datastore_constructs/touched_bytes BEFORE the walk and
  // snapshot_bytes AFTER it, so the books read snapshot_bytes=+frame with
  // snapshot_count=0, and the C4 trace line fired attributed to the LAST
  // dispatched verb (stale c4TraceVerb_) — L1B misread it as a foreach-body
  // closure erased by a sampler rollback. The fix routes both walks through
  // liveNamedBytesNeutral (session_state.h), whose save/restore set is every
  // emitted counter and whose span suppresses the trace line, so trace and
  // snapshot_count stay the same series. The counter checks below are the
  // regression signal for BOTH halves: the count half restored-but-bytes-kept
  // desync AND the missing C4-L1A keys in the original restore set.
  {
    nlohmann::json openBody{{"filepath", fixture}};
    auto openResult = piet::routes::open(session, openBody.dump());
    check(openResult.ok, "BUG-142 neutral-walk gate setup: re-`use` fixture returns ok=true");

    auto blk = mutationBlock(piet::dispatch("replace mpg = mpg + 1", session));
    check(blk.value("type", std::string()) == "data_mutation",
          "BUG-142 neutral-walk gate setup: `replace mpg = mpg + 1` through dispatch() returns a "
          "data_mutation result");

    // PP-L2B / K1b: read the dirty state DIRECTLY. The old residentLiveCount
    // delta stopped detecting this once the use-stash cache could service the
    // open via a hit/PUT-sync (the mutation then continues the chain with no
    // new frame adoption, so the count never moves while the head IS dirty).
    if (!session.residentHeadDirty()) {
      // No frames-capable worker — the mutation ran one-shot on disk, the head
      // is real+clean, and the dirty-closure path this gate exists to pin is
      // unreachable. The neutrality half alone would pass trivially.
      skip("BUG-142 neutral-walk gate (dirty-head closure path)",
           "frames unavailable — mutation did not go resident");
    } else {
      // The head is now a DIRTY virtual frame. Record every counter the
      // PIET_EMIT_COUNTERS block emits, then drive all three walk shapes.
      struct CounterShot {
        int64_t constructs, touched, snapCount, snapBytes, setcol, useDisk, useVirtual, dtaConv,
            useStash;
        static CounterShot take() {
          return {piet::Datastore::constructCount(),      piet::counters::touchedBytes(),
                  piet::counters::snapshotCount(),        piet::counters::snapshotBytes(),
                  piet::counters::setcolBytes(),          piet::counters::useDiskLoads(),
                  piet::counters::useVirtualHits(),       piet::counters::dtaConverts(),
                  piet::counters::useStashHits()};
        }
        bool operator==(const CounterShot& o) const {
          return constructs == o.constructs && touched == o.touched && snapCount == o.snapCount &&
                 snapBytes == o.snapBytes && setcol == o.setcol && useDisk == o.useDisk &&
                 useVirtual == o.useVirtual && dtaConv == o.dtaConv && useStash == o.useStash;
        }
      };
      const CounterShot c0 = CounterShot::take();

      // 1) Peak sample over the DIRTY head: skips the frame (never
      //    materialises mid-run) — strictly smaller than the end walk below.
      const int64_t vPeakDirty = session.liveNamedBytesPeakSample();
      check(CounterShot::take() == c0,
            "BUG-142 neutral-walk gate: a peak sample over a dirty head leaves every emitted "
            "counter (constructs/touched/snapshot_count/snapshot_bytes/setcol/use_disk_loads/"
            "use_virtual_hits/dta_converts/use_stash_hits) bit-identical");

      // 2) End-snapshot walk: materialises the dirty head (a REAL
      //    frameSnapshot fires inside the walk) — and must STILL leave every
      //    counter untouched. Pre-fix this is exactly where snapshot_bytes
      //    leaked +frame while snapshot_count read 0.
      const int64_t vEnd = session.liveNamedBytesEndSnapshot();
      check(CounterShot::take() == c0,
            "BUG-142 neutral-walk gate: the end-snapshot walk over a dirty head — whose own "
            "closure REALLY snapshots the frame to disk — leaves every emitted counter "
            "bit-identical (pre-fix signature: snapshot_bytes/touched_bytes leak the walk's "
            "frame bytes while the restored snapshot_count reads 0, and a [c4-trace] line "
            "fires attributed to the last dispatched verb)");
      check(vEnd > vPeakDirty,
            "BUG-142 neutral-walk gate: the end walk includes the materialised resident frame, "
            "the dirty-head peak sample skipped it — vEnd > vPeakDirty is the proof this leg "
            "actually exercised the dirty-closure path rather than a clean no-op walk");

      // 3) Peak sample AFTER the end walk materialised the head: the window is
      //    closed now, so the frame is included and the value equals the end
      //    walk's — and the walk stays neutral on the clean path too.
      const int64_t vPeakClean = session.liveNamedBytesPeakSample();
      check(CounterShot::take() == c0,
            "BUG-142 neutral-walk gate: a peak sample over the now-clean head stays "
            "counter-neutral (clean-path construct + cache-state restore)");
      check(vPeakClean == vEnd,
            "BUG-142 neutral-walk gate: post-materialisation peak sample equals the end walk "
            "(same state, same walk — the two diagnostic modes agree once the head is clean)");
    }
  }

  // -------------------------------------------------------------------------
  // FIX-PIET-COUNTERTAX-PEAKSAMPLE gate — the diagnostic byte walk reads column
  // METADATA ONLY. It used to iterate Datastore::varlist(), i.e. it built the
  // VarInfo card for every column, and buildVarInfo() computes n_missing by
  // scanning the column end to end — faulting the whole mmap payload in. Under
  // the peak sample that ran once per top-level do-file item, which measured
  // (instruments/alloc_churn_scoping.md §1, interleaved on/off A/B ×2) as
  // PIET_EMIT_COUNTERS=1 costing a2_mini +11.6% wall / +45% PIET wall / +98k
  // page faults for +0.79s CPU — i.e. a measurement harness taxing every
  // counters-on orbit figure the campaign quotes. The value is report-only and
  // ratified (② denominator), so the fix had to keep the NUMBER and drop only
  // the card build.
  //
  // Three arms, because the mechanism counter alone can pass for the wrong
  // reason: (a) MECHANISM — fullScanCount()==0 across a walk; (b) CONTROL —
  // building the card on the same live Datastore DOES scan every column, so
  // the zero above is this walk's own property and not a dead/unwired counter;
  // (c) VALUE — the walk's total is identical with the card cache cold and
  // warm, and its frame component equals the old varlist-based arithmetic to
  // the byte (measured against a no-data walk, which skips the frame block).
  // Guard: the peak sample skips the frame entirely over a DIRTY virtual head,
  // which would make (a) trivially true — so the leg asserts a clean head
  // first and skips honestly when it cannot get one.
  {
    nlohmann::json openBody{{"filepath", fixture}};
    auto openResult = piet::routes::open(session, openBody.dump());
    check(openResult.ok, "countertax gate setup: re-`use` fixture returns ok=true");

    if (session.residentHeadDirty()) {
      skip("FIX-PIET-COUNTERTAX-PEAKSAMPLE gate",
           "head is a dirty virtual frame after `use` — the peak sample skips the frame block "
           "entirely, so the no-payload-read property cannot be observed here");
    } else {
      // Cold cache is the production precondition: a mid-run sample usually
      // finds datastore_ null and builds a throwaway Datastore (that is what
      // liveNamedBytesNeutral's wasNull/reset dance restores). Fetch the
      // pointer ourselves first so the instance — and its per-instance
      // fullScanCount_ — survives the walk to be read.
      session.invalidateDatastore();
      piet::Datastore* ds = session.datastore();
      check(ds != nullptr, "countertax gate setup: session.datastore() non-null after `use`");
      if (ds != nullptr) {
        const int64_t nCols = static_cast<int64_t>(ds->names().size());
        check(nCols > 0, "countertax gate setup: fixture has columns to walk");

        ds->resetFullScanCount();
        const int64_t vCold = session.liveNamedBytesPeakSample();
        check(ds->fullScanCount() == 0,
              "FIX-PIET-COUNTERTAX-PEAKSAMPLE MECHANISM: a peak sample over a clean head "
              "performs ZERO full-column n_missing scans — pre-fix it built the VarInfo card "
              "for every column and paid one O(nrows) payload-faulting scan each, per "
              "top-level do-file item");

        // (b) CONTROL — the counter is live on this very instance.
        (void)ds->varlist();
        check(ds->fullScanCount() == nCols,
              "FIX-PIET-COUNTERTAX-PEAKSAMPLE CONTROL: building the VarInfo card on the SAME "
              "live Datastore scans every column (fullScanCount()==n_cols) — proof the zero "
              "above is the walk's own property, not an unwired counter");

        // (c) VALUE — cache state must not move the reported number.
        const int64_t vWarm = session.liveNamedBytesPeakSample();
        check(vWarm == vCold,
              "FIX-PIET-COUNTERTAX-PEAKSAMPLE VALUE: the walk reports the same total with the "
              "VarInfo card cache cold and warm (the ② denominator is unchanged by the fix's "
              "cache-state footprint)");

        // (c′) VALUE, exact — recompute the resident-frame component with the
        // OLD varlist-based arithmetic and pin it against the difference
        // between the loaded walk and a no-data walk (datastore()==nullptr
        // skips the frame block, so that difference IS the frame component).
        int64_t refFrame = 0;
        const int64_t rows = ds->nRows();
        for (const piet::VarInfo& vi : ds->varlist()) {
          if (vi.dtype == "factor") {
            refFrame += 4 * rows;
            for (const std::string& lvl : vi.levels) refFrame += static_cast<int64_t>(lvl.size());
          } else {
            refFrame += 8 * rows;
          }
        }
        check(refFrame > 0, "countertax gate setup: reference frame bytes are non-zero");

        piet::dispatch("clear", session);
        const int64_t vNoData = session.liveNamedBytesPeakSample();
        check(vCold - vNoData == refFrame,
              "FIX-PIET-COUNTERTAX-PEAKSAMPLE VALUE (exact): the metadata-only walk's frame "
              "component equals the varlist-based arithmetic it replaced, to the byte "
              "(f64 8B·rows, factor 4B·rows + its level dictionary once)");
      }
    }
  }

  // -------------------------------------------------------------------------
  // C4-L2A gates (FLOOR C4 D1 card plane, C4-RESIDENT-READ-SCOPE-brief §4).
  // Three consumers stop opening a Datastore — and, on a dirty virtual head,
  // stop forcing the EMH frame snapshot that open performs — when the resident
  // card is CURRENT: bare `count` (_N off residentRowsIfCurrent), `confirm`
  // (has/dtype off residentReadCard) and `save`'s entry check (hasData()).
  // Two gate classes per consumer, and BOTH are needed (the S1b legs' law):
  //   · MECHANISM — Datastore::constructCount()==0 / snapshotCount unchanged.
  //     The ANSWER is identical either way (that is the design), so no value
  //     pin can see this fast path die; only the counters can.
  //   · PARITY + ESCAPE — PIET_NO_RESIDENT_READ=1 (per-call getenv, so
  //     setenv around a single dispatch works) must (a) produce byte-identical
  //     blocks/r(N) — the A/B correctness arm — and (b) actually construct/
  //     snapshot again — proving the escape reaches the old path, the
  //     mutant-kill for a dropped env check AND the floor_gate RED arm's
  //     in-process twin (escape arm = old-count reproduction).
  {
    // Env hygiene (FIX-SELFTEST-ENVHARDEN-MECHANISM): an ambient
    // PIET_NO_RESIDENT_READ/_SETCOL leaking in from outside this process
    // flips the "bare" (card fast-path) calls below into the escape path
    // before this leg's own setenv/unsetenv pairs ever run, corrupting the
    // MECHANISM counters and the dirty-window state they depend on. Force
    // the family clean at leg entry; the leg's own toggles below are
    // unaffected.
    unsetenv("PIET_NO_RESIDENT_READ");
    unsetenv("PIET_NO_RESIDENT_SETCOL");

    nlohmann::json openBody{{"filepath", fixture}};
    auto openResult = piet::routes::open(session, openBody.dump());
    check(openResult.ok, "C4-L2A gate setup: re-`use` fixture returns ok=true");

    auto blk = mutationBlock(piet::dispatch("replace mpg = mpg + 1", session));
    check(blk.value("type", std::string()) == "data_mutation",
          "C4-L2A gate setup: `replace mpg = mpg + 1` through dispatch() returns a "
          "data_mutation result");
    // PP-L2B / K1b: direct dirty-state read (see the BUG-142 leg's comment —
    // the residentLiveCount delta no longer detects a chain-continuing verb).
    const bool wentResident = session.residentHeadDirty();

    // --- bare `count`: card plane -------------------------------------------
    piet::Datastore::resetConstructCount();
    const int64_t snapBeforeCount = piet::counters::snapshotCount();
    // dispatch() always leads with the command-echo block; scan past it.
    auto findBlock = [](const nlohmann::ordered_json& blocks, const std::string& type,
                        const std::string& text) {
      for (const auto& b : blocks) {
        if (b.is_object() && b.value("type", std::string()) == type &&
            b.value("text", std::string()) == text) {
          return true;
        }
      }
      return false;
    };
    auto countBlocks = piet::dispatch("count", session);
    check(findBlock(countBlocks, "display", "74"),
          "C4-L2A count gate: bare `count` answers 74 (auto.hbin's row count) as a display "
          "block off the resident card");
    {
      const double* rN = session.eval.rValues.find("N");
      check(rN != nullptr && *rN == 74.0,
            "C4-L2A count gate: the card-plane bare `count` still sets r(N)=74 (r-class "
            "surface identical to the datastore path)");
    }
    check(piet::Datastore::constructCount() == 0,
          "C4-L2A count MECHANISM gate: bare `count` answers _N with ZERO Datastore "
          "constructions (SessionState::residentRowsIfCurrent). A count of 1 means the card "
          "fast path is dead — the answer above stays correct either way, which is exactly "
          "why this counter and not the text is the gate");
    if (wentResident) {
      check(piet::counters::snapshotCount() == snapBeforeCount,
            "C4-L2A count MECHANISM gate: bare `count` over a DIRTY virtual head does NOT "
            "close the window — snapshot_count unchanged (pre-D1, the entry datastore() "
            "forced an EMH frame snapshot here; brief §1 count row)");
    } else {
      skip("C4-L2A count gate (dirty-window half)",
           "frames unavailable — mutation did not go resident, no window to leave open");
    }

    // --- confirm: has/dtype card-read + error-text parity -------------------
    piet::Datastore::resetConstructCount();
    auto confirmOk = piet::dispatch("confirm variable mpg", session);
    check(confirmOk.size() == 1 &&
              confirmOk[0].value("type", std::string()) == "echo",
          "C4-L2A confirm gate: `confirm variable mpg` succeeds silently (echo block only, no "
          "error) off the card");
    auto confirmNumFactor = piet::dispatch("confirm numeric variable make", session);
    auto confirmMissing = piet::dispatch("confirm variable zzz_c4_nope", session);
    auto confirmNewClash = piet::dispatch("confirm new variable zzz_ok mpg", session);
    check(piet::Datastore::constructCount() == 0,
          "C4-L2A confirm MECHANISM gate: all four confirm shapes (exists / numeric-vs-factor "
          "/ not-found / new-variable clash) answer with ZERO Datastore constructions — name, "
          "dtype AND the not-found verdict come off the CURRENT card (a current card is "
          "authoritative for absence too; that is the honest-currency rule, "
          "shadowVarlistIfCurrent's doc)");
    check(confirmNumFactor.size() >= 2 && confirmMissing.size() >= 2 &&
              confirmNewClash.size() >= 2,
          "C4-L2A confirm gate: the three failing confirm shapes each return an error block "
          "beyond the command echo");

    // --- escape arm: byte parity + the old path really runs -----------------
    setenv("PIET_NO_RESIDENT_READ", "1", 1);
    piet::Datastore::resetConstructCount();
    const int64_t snapBeforeEscape = piet::counters::snapshotCount();
    auto countBlocksEsc = piet::dispatch("count", session);
    const double rNEsc = [&] {
      const double* p = session.eval.rValues.find("N");
      return p ? *p : -1.0;
    }();
    auto confirmNumFactorEsc = piet::dispatch("confirm numeric variable make", session);
    auto confirmMissingEsc = piet::dispatch("confirm variable zzz_c4_nope", session);
    auto confirmNewClashEsc = piet::dispatch("confirm new variable zzz_ok mpg", session);
    unsetenv("PIET_NO_RESIDENT_READ");
    check(countBlocksEsc.dump() == countBlocks.dump() && rNEsc == 74.0,
          "C4-L2A escape PARITY gate: PIET_NO_RESIDENT_READ=1 bare `count` returns the "
          "byte-identical display block and the same r(N) — the two planes agree on the "
          "answer, they differ only in what they touch");
    check(confirmNumFactorEsc.dump() == confirmNumFactor.dump() &&
              confirmMissingEsc.dump() == confirmMissing.dump() &&
              confirmNewClashEsc.dump() == confirmNewClash.dump(),
          "C4-L2A escape PARITY gate: every confirm error block — including the dtype-naming "
          "`confirm numeric variable make` text — is byte-identical between the card plane and the "
          "escape arm's datastore plane (the closed f64/factor dtype vocabulary is shared, "
          "datastore.cpp:68)");
    check(piet::Datastore::constructCount() >= 1,
          "C4-L2A escape MECHANISM gate: the escape arm REALLY runs the old path — at least "
          "one Datastore construction under PIET_NO_RESIDENT_READ=1. A zero here means the "
          "env check was dropped/cached and the floor_gate RED arm (escape = old-count "
          "reproduction) no longer proves corpus reach");
    if (wentResident) {
      check(piet::counters::snapshotCount() == snapBeforeEscape + 1,
            "C4-L2A escape MECHANISM gate: the escape-arm `count` CLOSES the dirty window the "
            "card plane had left open (snapshot_count +1) — the in-process twin of the "
            "floor_gate RED arm's old-count reproduction");
    }

    // --- save: entry card-read (L2A) + C4-L2C direct-to-destination write ----
    // The save's entry check opens ZERO datastores (L2A hasData()), and on a
    // dirty virtual head the real-file save now serializes STRAIGHT into its
    // destination temp (C4-L2C / D3, trySaveHeadDirect) — no scratch
    // materialization, so the snapshot count is UNCHANGED and the head is left
    // virtual. The escape arm reproduces the pre-C4 materialize-then-copy path
    // (+1 snapshot) and must land byte-identical bytes: the A/B save-parity +
    // mutant-kill arm (brief §4 "save 이중쓰기 절반", §6 gate plan).
    {
      auto blk2 = mutationBlock(piet::dispatch("replace mpg = mpg + 1", session));
      check(blk2.value("type", std::string()) == "data_mutation",
            "C4-L2C save gate setup: second `replace mpg = mpg + 1` returns a data_mutation "
            "result (fresh dirty window)");
      const bool wentResident2 = session.residentHeadDirty();
      const std::optional<std::string> savedCwd = session.cwd;
      session.cwd = workdir;

      // Direct path (resident plane): one frameSnapshot straight into dest temp.
      piet::Datastore::resetConstructCount();
      const int64_t snapBeforeSave = piet::counters::snapshotCount();
      auto saveBlocks = piet::dispatch("save c4_l2c_direct.hbin, replace", session);
      check(findBlock(saveBlocks, "echo", "file c4_l2c_direct.hbin saved"),
            "C4-L2C save gate: `save` still echoes success");
      check(piet::Datastore::constructCount() == 0,
            "C4-L2A save MECHANISM gate: `save`'s entry check constructs ZERO Datastores — "
            "hasData() answers \"is a dataset loaded\" without the redundant datastore() open");
      if (wentResident2) {
        check(piet::counters::snapshotCount() == snapBeforeSave,
              "C4-L2C save MECHANISM gate: the dirty-head real-file save snapshots ZERO scratch "
              "frames — the frame is written DIRECTLY into the destination temp "
              "(trySaveHeadDirect), not materialized to the scratch head first, and the head is "
              "left virtual. A +1 here means the direct path is dead and every save pays the old "
              "materialize-then-copy double write again");
      }
      std::error_code exDirectEc;
      check(std::filesystem::exists(workdir + "/c4_l2c_direct.hbin", exDirectEc),
            "C4-L2C save gate: the direct-write saved file exists at the resolved cwd destination");

      // Escape arm (PIET_NO_RESIDENT_READ=1): the SAME still-dirty head goes
      // through the pre-C4 path — it materializes (snapshot +1) then copies. The
      // bytes must match the direct-write file exactly (A/B save parity).
      setenv("PIET_NO_RESIDENT_READ", "1", 1);
      const int64_t snapBeforeEscSave = piet::counters::snapshotCount();
      auto saveEscBlocks = piet::dispatch("save c4_l2c_escape.hbin, replace", session);
      unsetenv("PIET_NO_RESIDENT_READ");
      check(findBlock(saveEscBlocks, "echo", "file c4_l2c_escape.hbin saved"),
            "C4-L2C save escape gate: the escape-arm `save` echoes success");
      if (wentResident2) {
        check(piet::counters::snapshotCount() == snapBeforeEscSave + 1,
              "C4-L2C save escape MECHANISM gate: the escape arm REALLY runs the pre-C4 path — "
              "the dirty head is materialized once (snapshot +1). A zero means the env check was "
              "dropped and the direct path's win is unverifiable");
      }
      auto readFileBytes = [](const std::string& p) {
        std::ifstream f(cecon::to_native_path(p), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
      };
      const std::string directBytes = readFileBytes(workdir + "/c4_l2c_direct.hbin");
      const std::string escapeBytes = readFileBytes(workdir + "/c4_l2c_escape.hbin");
      session.cwd = savedCwd;
      check(!directBytes.empty() && directBytes == escapeBytes,
            "C4-L2C save PARITY gate: the direct-to-destination write and the escape arm's "
            "materialize-then-copy produce a BYTE-IDENTICAL saved file — both serialize the same "
            "resident frame via frameSnapshot, so the deliverable is unchanged (no golden re-pin)");
    }
  }

  // --- C4-L3 / D2a: lazy if/while condition binder ---------------------------
  // The shared expression-ns binder (loops.cpp evalBoolExpr) used to call
  // session.datastore() UNCONDITIONALLY — on a dirty virtual head, a full EMH
  // frame snapshot for a condition that reads no data (the corpus-dominant
  // window-closer after D1, c4_window_attribution §3). Lazy binding reads _N
  // and column membership off the CURRENT card and pays the datastore only
  // for a real row-1 value reference. Gates: pure-scalar no-close MECHANISM,
  // _N-from-card, value-read honest flush, escape mutant-kill + branch parity.
  {
    nlohmann::json openBody{{"filepath", fixture}};
    auto openResult = piet::routes::open(session, openBody.dump());
    check(openResult.ok, "C4-L3 gate setup: re-`use` fixture returns ok=true");
    auto blk = mutationBlock(piet::dispatch("replace mpg = mpg + 1", session));
    check(blk.value("type", std::string()) == "data_mutation",
          "C4-L3 gate setup: `replace mpg = mpg + 1` returns a data_mutation result");
    if (!session.residentHeadDirty()) {
      skip("C4-L3 lazy-binder gates",
           "frames unavailable — mutation did not go resident, no dirty window to probe");
    } else {
      auto runBlockSrc = [&](const std::string& src) {
        return piet::doexec::executeDoSource(src, session, piet::dispatch);
      };
      // 1) MECHANISM — pure-scalar condition over a DIRTY head: no close.
      piet::Datastore::resetConstructCount();
      const int64_t snapPS = piet::counters::snapshotCount();
      (void)runBlockSrc("if 1 {\n  local c4l3ps = 7\n}\n");
      check(session.eval.locals.get("c4l3ps", "") == "7",
            "C4-L3 MECHANISM gate setup: the `if 1` branch executed (its body set the marker "
            "local)");
      check(piet::Datastore::constructCount() == 0 && piet::counters::snapshotCount() == snapPS,
            "C4-L3 MECHANISM gate: a pure-scalar `if` over a DIRTY virtual head constructs ZERO "
            "Datastores and closes NO window (snapshot_count unchanged) — pre-D2 this very line "
            "paid a whole-frame EMH snapshot to read nothing");
      check(session.residentHeadDirty(),
            "C4-L3 MECHANISM gate: the dirty window is STILL OPEN after the pure-scalar `if` — "
            "the closure moved nowhere, it vanished");
      // 2) _N answered from the card — still no datastore, still no close.
      piet::Datastore::resetConstructCount();
      const int64_t snapN = piet::counters::snapshotCount();
      (void)runBlockSrc("if _N == 74 {\n  local c4l3n = 1\n}\n");
      check(session.eval.locals.get("c4l3n", "") == "1",
            "C4-L3 _N gate: `if _N == 74` took the true branch — the CARD answered auto.hbin's "
            "row count correctly (residentRowsIfCurrent, the D1 rows fact)");
      check(piet::Datastore::constructCount() == 0 && piet::counters::snapshotCount() == snapN &&
                session.residentHeadDirty(),
            "C4-L3 _N MECHANISM gate: the _N reference bound off the card — zero constructs, no "
            "window close, window still open");
      // 3) A genuine column reference: the honest flush (D2a's accepted class).
      piet::Datastore::resetConstructCount();
      const int64_t snapV = piet::counters::snapshotCount();
      (void)runBlockSrc("if mpg > 0 {\n  local c4l3v = 1\n}\n");
      check(session.eval.locals.get("c4l3v", "") == "1",
            "C4-L3 value gate: `if mpg > 0` read the real row-1 value and took the true branch");
      check(piet::Datastore::constructCount() >= 1 &&
                piet::counters::snapshotCount() == snapV + 1,
            "C4-L3 value MECHANISM gate: a genuine column reference pays the datastore exactly "
            "like the eager arm — the window closes ONCE for a real read (honest flush), never "
            "silently answered from a stale plane");
      // 4) Escape mutant-kill: re-dirty, then the escape arm must CLOSE on the
      //    same pure-scalar condition (pre-D2 reproduction, floor RED arm twin).
      (void)mutationBlock(piet::dispatch("replace mpg = mpg + 1", session));
      if (session.residentHeadDirty()) {
        setenv("PIET_NO_RESIDENT_READ", "1", 1);
        const int64_t snapEsc = piet::counters::snapshotCount();
        (void)runBlockSrc("if 1 {\n  local c4l3e = 3\n}\n");
        unsetenv("PIET_NO_RESIDENT_READ");
        check(session.eval.locals.get("c4l3e", "") == "3",
              "C4-L3 escape PARITY gate: the escape arm's `if 1` takes the same branch (the two "
              "arms differ in what they touch, never in the answer)");
        check(piet::counters::snapshotCount() == snapEsc + 1,
              "C4-L3 escape MECHANISM gate: under PIET_NO_RESIDENT_READ=1 the SAME pure-scalar "
              "`if` REALLY runs the pre-D2 eager path — the dirty head materializes (snapshot "
              "+1). A zero here means the env check was dropped and the lazy win is "
              "unverifiable (mutant-kill)");
      }
    }
  }

  // -------------------------------------------------------------------------
  // MAXPERF S4a gate (fix/verb-overhead-sweep) — strmutate::factorColumns, the
  // names-and-dtypes predicate that was re-opening the whole dataset.
  //
  // WHAT IS BEING GATED. factorColumns (strmutate.cpp) answers "which columns
  // are factors", and it was answering it with session.datastore() — which on a
  // resident chain first calls ensureMaterializedHead(), snapshotting the whole
  // virtual 164 MB frame to disk, and then mmaps and header-parses it. (It does
  // NOT hash: the FNV provenance hash this campaign's earlier notes blamed is
  // engine-side, apps/cmds/common.cpp:41, and unreachable from here. See
  // factorColumns' own comment for the full correction.) Five verbs funnel
  // through it — gen and replace via
  // needsStringEval (dispatcher.cpp:1910/1969), and drop if / keep if /
  // count if via needsStringCount (dispatcher.cpp:2533/2553/2884) — so it taxed
  // even the verbs whose OWN entry validation S1 had already made lazy. The fix
  // is the established one: consult session.shadowVarlistIfCurrent(), which
  // carries dtype, and fall back to the datastore only when the card cannot be
  // trusted.
  //
  // WHY THIS LEG IS DIRECT AND NOT DRIVEN THROUGH A VERB. Every verb that
  // reaches factorColumns also, on at least one of its branches, legitimately
  // opens the datastore afterwards (runCount does so unconditionally,
  // handlers_m3.cpp:772), and session.datastore() memoizes — so an unfixed
  // factorColumns and a fixed one would both read a total of ONE construct
  // through those verbs. A check that cannot fail is not a gate (the same
  // reasoning the S1 legs above give for using the chain shape rather than a
  // fresh-after-`use` shape). Calling the predicate directly is the only shape
  // in which the counter can distinguish the two, so that is what this does.
  //
  // Precondition is the sort-validate-lazy gate's exactly: prime so the shadow
  // card is valid and tagged for the CURRENT hbin (sortRoute's non-resident
  // mutationResult calls reseedShadowFrom, which tags it for that rebuild),
  // then invalidateDatastore() to reproduce the cold-cache-but-valid-card state
  // a real resident chain leaves behind.
  {
    nlohmann::json openBody{{"filepath", fixture}};
    auto openResult = piet::routes::open(session, openBody.dump());
    check(openResult.ok, "S4a factorColumns gate setup: re-`use` fixture returns ok=true");
    nlohmann::json primeBody{{"varlist", nlohmann::json::array({"mpg"})}};
    auto primeResult = piet::routes::sortRoute(session, primeBody.dump());
    check(primeResult.ok, "S4a factorColumns gate setup: priming `sort mpg` returns ok=true");
    session.invalidateDatastore();

    piet::Datastore::resetConstructCount();
    // `mpg > 20` names no factor, uses no string fn and holds no literal — so
    // needsStringCount cannot short-circuit (strmutate.cpp checks usesStringFn
    // and hasStringLiteral first) and MUST consult the complete dtype map to
    // answer. This is the measured call.
    const bool numericNeedsString =
        piet::strmutate::needsStringCount(session, std::optional<std::string>("mpg > 20"));
    const int64_t numericConstructs = piet::Datastore::constructCount();
    check(!numericNeedsString,
          "MAXPERF S4a gate: `mpg > 20` does NOT need the row-wise string path — mpg is f64. A "
          "true here would reroute every numeric filter through strmutate::filterRowsIf");
    check(numericConstructs == 0,
          "MAXPERF S4a mechanism gate: answering the factor-column question for `mpg > 20` "
          "constructs ZERO Datastores — strmutate::factorColumns reads name+dtype off "
          "session.shadowVarlistIfCurrent(). A count of 1 is the regression signal and the ONLY "
          "thing that catches it: the ANSWER is identical either way, so no value pin anywhere "
          "can see this fast path die. Disable the card branch in factorColumns and this leg goes "
          "red while every card, count and filter in the suite stays green — which is exactly the "
          "silent-death class this campaign exists to close");

    piet::Datastore::resetConstructCount();
    // The complement, and the reason the leg above cannot stand alone: a fast
    // path that simply returned an EMPTY set would pass it (nothing referenced
    // => false => 0 constructs) while silently routing every genuine string
    // condition to the all-f64 engine (BUG-032). `make` is auto.hbin's string
    // column, and this expression carries no literal and no string fn, so the
    // true answer can ONLY come from the dtype map having actually been read.
    const bool factorNeedsString =
        piet::strmutate::needsStringCount(session, std::optional<std::string>("make > 0"));
    const int64_t factorConstructs = piet::Datastore::constructCount();
    check(factorNeedsString,
          "MAXPERF S4a gate: `make > 0` DOES need the row-wise string path — make is a factor. "
          "A false here means factorColumns returned a set that is missing real factors, sending "
          "a string condition to cobra's all-f64 eval (BUG-032). This is the leg that makes the "
          "zero-construct assertions above mean something rather than being satisfiable by "
          "returning nothing at all");
    check(factorConstructs == 0,
          "MAXPERF S4a mechanism gate: the FACTOR-finding answer is also free (zero Datastore "
          "constructions) — the card supplies dtype for the positive case, not just the negative");

    // Ground both answers in the real file rather than in each other. Done
    // AFTER the measured calls on purpose: opening the datastore first would
    // warm session's cache and make the counters above unfalsifiable.
    session.invalidateDatastore();
    piet::Datastore* truthDs = session.datastore();
    check(truthDs != nullptr && truthDs->has("make") && truthDs->has("mpg") &&
              truthDs->dtype("make") == "factor" && truthDs->dtype("mpg") == "f64",
          "MAXPERF S4a equivalence gate: the shadow card's dtypes match the hbin's own — make is "
          "`factor` and mpg is `f64` per Datastore::dtype(). If a future fixture changed either "
          "column's type, the two legs above would be asserting against a stale assumption; this "
          "check makes that show up here instead of silently weakening them");
  }

  // -------------------------------------------------------------------------
  // MAXPERF S2 gates (fix/verb-overhead-sweep) — the ROW FILTER (`keep if` /
  // `drop if`) on the resident plane.
  //
  // WHAT IS BEING GATED. `drop if`/`keep if` was the single largest verb cost
  // in this campaign's attribution: 22 calls / 7.9 s / 359 ms mean in one
  // dependent-variable pass, and both layers paid roughly half each. The engine
  // half was routing: --keepif used to disqualify EVERY call from the §26
  // resident plane, so each filter re-read the whole 164 MB frame off disk and
  // wrote a whole new --out (527 ms/call, against 58-96 ms for the same filter
  // run on the frame already in the worker's RAM). The PIET half was the card:
  // with no delta to fold, mutationResult re-opened the result and rescanned
  // all 192 columns for n_missing (596 ms/call). (This sentence used to say the
  // re-open also "re-hashed it for provenance". It does not — PIET's Datastore
  // constructor never hashes; the FNV whole-file hash is engine-side and
  // estimator-gated, apps/cmds/common.cpp:41/274/288. Corrected under S4a; the
  // 596 ms and the conclusion it supports are unaffected, since the cost was
  // always the 192-column rescan and its whole-file page-fault-in.)
  //
  // WHY THE ROW FILTER IS NOT AN S1b FOLD, AND WHY THAT IS THE POINT. A row
  // filter CHANGES n_missing and n_levels per column — card_stable:false is the
  // engine telling the truth about that. The S1b fold above is licensed by a
  // permutation/removal provably not touching any surviving column's metadata;
  // that licence does not extend here, and extending it would ship stale
  // missing-counts and factor levels into the variable panel. So this path
  // consumes the engine's RECOMPUTED vars_delta instead, and assumes invariance
  // of nothing. The equivalence leg below is what holds that claim to account:
  // a value pin on the card cannot see a fold that quietly starts folding
  // wrong, because the pinned card would go on agreeing with itself.
  //
  // Shared between the two legs: the `keep if` survivor count, so the `drop if`
  // leg can assert the exact complement relation without either leg hard-coding
  // a fixture-specific row count.
  int64_t s2KeepRows = -1;
  int64_t s2BaseRows = -1;
  {
    nlohmann::json openBody{{"filepath", fixture}};
    auto openResult = piet::routes::open(session, openBody.dump());
    check(openResult.ok, "keepif S2 gate setup: re-`use` fixture returns ok=true");

    // Priming resident mutation — seeds the shadow card and leaves the worker
    // holding this head, exactly as the order/drop/keep legs above do. Without
    // it the measured call would resync inside itself and the card it folds
    // would not exist yet.
    auto primeBlk = mutationBlock(piet::dispatch("gen __s2_prime = 1", session));
    check(primeBlk.value("type", std::string()) == "data_mutation",
          "keepif S2 gate setup: priming `gen __s2_prime = 1` through dispatch() returns a card");
    check(session.residentFrameLive(),
          "keepif S2 gate PRECONDITION: the section 26 resident plane is live "
          "(SessionState::residentFrameLive()). FALSE here means this environment has no "
          "frames-capable `cobra serve`, so there is no resident frame to filter and no delta to "
          "consume — read the failures below as an ENVIRONMENT result, not as a regression");
    const int64_t rowsBefore = primeBlk.value("n_rows_after", int64_t{-1});

    session.invalidateDatastore();
    piet::Datastore::resetConstructCount();
    piet::SessionState::resetCardFoldCount();
    // Driven through dispatch() with NOTHING entered here: the resident opt-in
    // must come from runKeepIf's own `SessionState::ResidentEligible`
    // (handlers_m3.cpp). An earlier draft of this leg entered that guard
    // itself, which made every assertion below pass while production still
    // routed `keep if` stateless — the gate proved the card path and silently
    // assumed the wiring. Delete either opt-in line in handlers_m3.cpp and this
    // leg goes red, which is the only thing that makes it a ROUTING gate rather
    // than a card gate wearing one's clothes.
    nlohmann::ordered_json blk = mutationBlock(piet::dispatch("keep if mpg > 20", session));
    check(blk.value("type", std::string()) == "data_mutation",
          "MAXPERF S2 gate: a resident `keep if mpg > 20` returns a data_mutation card. Failure "
          "means either S2's routing regressed, or this engine predates --keepif on a resident "
          "frame (contract_A section 26's stale carve-out text) and the frame run was rejected");

    const int64_t foldCount = piet::SessionState::cardFoldCount();
    const int64_t constructs = piet::Datastore::constructCount();
    check(foldCount == 1,
          "MAXPERF S2 mechanism gate: `keep if` produces its card from the engine's recomputed "
          "vars_delta (SessionState::cardFoldCount()==1), never from mutationResult's rebuild. A "
          "count of 0 is the whole reason this is a mechanism gate — reachable THREE ways, and "
          "every one of them leaves the cards below perfectly correct while the filter silently "
          "costs ~1.1 s/call again: drop runKeepIf's ResidentEligible opt-in (handlers_m3.cpp), "
          "put `!hasKeepif` back into goResident, or let residentCardDeltaComplete decline");
    check(constructs == 0,
          "MAXPERF S2 mechanism gate: a resident `keep if` constructs ZERO Datastores end to end. "
          "This was ==1 while S2 shipped: the survivor was the dispatcher's own entry-side open "
          "(strmutate::needsStringCount -> factorColumns -> session.datastore()), which needed "
          "only column NAMES and DTYPES and was then out of S2's scope. S4a gave factorColumns "
          "the same shadow-card treatment (strmutate.cpp), so that open is gone and nothing on "
          "this path re-reads the file. TWO distinct regressions now land here: a count of 1 "
          "means EITHER factorColumns went back to re-opening the hbin (S4a — the dedicated leg "
          "above will have gone red too, which is how the two are told apart) OR mutationResult "
          "fell back and re-opened the FILTERED result to rebuild the card, rescanning every "
          "column for n_missing");

    // PLANE EQUIVALENCE. The card just produced on the resident plane must be
    // the card the stateless rebuild would have produced — field for field, not
    // just name-for-name. This is deliberately an INDEPENDENT recomputation:
    // session.datastore() re-opens the head and mmaps it fresh, and
    // nothing seeds its n_missing (SessionState::seedNMissForPureAdd carries
    // nothing for a row-changing action — see its own branch list), so every
    // count below is scanned off the real filtered bytes. It is also exactly
    // what mutation_result.cpp's fallthrough does, so this compares the two
    // planes and not two spellings of the same one. If a future engine change
    // made vars_delta partial and the guard wrongly accepted it, THIS is the
    // leg that goes red — the mechanism counters above would stay green.
    auto residentCard = blk.value("varlist_after", nlohmann::ordered_json::array());
    const int64_t residentRows = blk.value("n_rows_after", int64_t{-1});
    session.invalidateDatastore();
    piet::Datastore* rebuiltDs = session.datastore();
    nlohmann::ordered_json rebuiltCard = nlohmann::ordered_json::array();
    if (rebuiltDs != nullptr) {
      for (const auto& v : session.varlistWithMeta(rebuiltDs)) rebuiltCard.push_back(v.toJson());
    }
    check(rebuiltDs != nullptr && !residentCard.empty(),
          "keepif S2 equivalence gate setup: both planes produced a non-empty card to compare");
    check(residentCard == rebuiltCard,
          "MAXPERF S2 equivalence gate: the resident `keep if` card equals the datastore-rebuilt "
          "card FIELD FOR FIELD (name/dtype/format/label/n_levels/n_missing, in column order). A "
          "mismatch on n_missing or n_levels is the campaign's named trap arriving: metadata "
          "carried across a row filter instead of taken from the engine's recomputation");
    check(rebuiltDs != nullptr && residentRows == rebuiltDs->nRows(),
          "MAXPERF S2 equivalence gate: n_rows_after equals the filtered frame's real row count. "
          "Taken from delta.rows, never inferred — keep_if sets structure_dirty_ unconditionally, "
          "so card_stable is false even for a filter that drops no row at all");
    check(rowsBefore < 0 || (rebuiltDs != nullptr && rebuiltDs->nRows() < rowsBefore),
          "keepif S2 gate sanity: `keep if mpg > 20` actually filtered rows away (" +
              std::to_string(rebuiltDs != nullptr ? rebuiltDs->nRows() : -1) + " of " +
              std::to_string(rowsBefore) + ") — a no-op filter would make the equivalence check "
              "above pass trivially");
    s2KeepRows = residentRows;
    s2BaseRows = rowsBefore;
  }
  {
    // `drop if` — the SECOND opt-in line, and the verb actually measured: all
    // seven of the largest filter calls in the attribution pass were
    // `drop if missing(...)`. It reaches the same routing and the same card
    // branch as `keep if`, but through runDropIf's own ResidentEligible and
    // through applyIfAndInFilter's complement=true predicate, so the leg above
    // does NOT cover it — delete runDropIf's opt-in and every assertion up
    // there still passes. The card path itself is shared and already gated, so
    // this leg asserts the mechanism plus a row-count check that the complement
    // really was taken (a filter routed correctly but complemented wrongly
    // would keep the wrong 38 rows with identical counters).
    nlohmann::json openBody{{"filepath", fixture}};
    auto openResult = piet::routes::open(session, openBody.dump());
    check(openResult.ok, "dropif S2 gate setup: re-`use` fixture returns ok=true");
    auto primeBlk = mutationBlock(piet::dispatch("gen __s2_prime2 = 1", session));
    check(primeBlk.value("type", std::string()) == "data_mutation",
          "dropif S2 gate setup: priming `gen __s2_prime2 = 1` through dispatch() returns a card");
    const int64_t rowsBefore = primeBlk.value("n_rows_after", int64_t{-1});

    session.invalidateDatastore();
    piet::Datastore::resetConstructCount();
    piet::SessionState::resetCardFoldCount();
    auto blk = mutationBlock(piet::dispatch("drop if mpg > 20", session));
    check(blk.value("type", std::string()) == "data_mutation",
          "MAXPERF S2 gate: a resident `drop if mpg > 20` returns a data_mutation card");
    check(piet::SessionState::cardFoldCount() == 1,
          "MAXPERF S2 mechanism gate: `drop if` reaches the resident plane through runDropIf's OWN "
          "ResidentEligible opt-in (handlers_m3.cpp) and folds its card from the engine's "
          "vars_delta. A count of 0 with the `keep if` leg above still green means exactly one "
          "thing: runDropIf's opt-in line specifically is gone");
    check(piet::Datastore::constructCount() == 0,
          "MAXPERF S2 mechanism gate: a resident `drop if` constructs ZERO Datastores — neither "
          "the card (S2) nor the dispatcher's entry-side factor-column lookup (S4a, which removed "
          "the ONE construction this leg used to allow). See the `keep if` leg for how a count of "
          "1 tells the two regressions apart");
    const int64_t dropRows = blk.value("n_rows_after", int64_t{-1});
    check(s2KeepRows > 0 && s2BaseRows > 0 && rowsBefore == s2BaseRows &&
              dropRows == s2BaseRows - s2KeepRows && dropRows > 0,
          "MAXPERF S2 gate: `drop if mpg > 20` keeps the exact COMPLEMENT of what `keep if mpg > "
          "20` keeps — kept(" + std::to_string(dropRows) + ") + kept_by_keep_if(" +
              std::to_string(s2KeepRows) + ") must equal the unfiltered " +
              std::to_string(s2BaseRows) + ". Stated as a relation between the two legs rather "
              "than a hard-coded count, so it stays honest on any fixture. This is the one "
              "assertion that would catch complement=true being lost on the resident path — no "
              "counter above can see a filter that routes perfectly and keeps the wrong rows");
  }
  {
    // THE FALLBACK GATE. residentCardDeltaComplete is the load-bearing safety
    // check of the branch above, and it guards a field the engine does NOT
    // contract: vars_delta's completeness for a row filter is incidental
    // (Frame::keep_if allocates a fresh vector per column, so pointer-identity
    // marks every column changed). A lazy/CoW row selection — the same class of
    // rewrite this codebase already ships elsewhere — would preserve untouched
    // pointers and make the field PARTIAL while rows still change.
    //
    // Leaning on an uncontracted field is only defensible if the fallback is
    // itself gated. Without this leg, that future change would make the guard
    // quietly stop firing, PIET would silently return to the slow path, and
    // nobody would notice for months — the "optimization died in silence" class
    // this campaign exists to close, arriving through the back door. So the
    // guard's truth table is asserted directly here rather than left to
    // whichever shapes a live engine happens to emit today.
    auto mkVar = [](const char* n) {
      piet::CobraFrameVar fv;
      fv.name = n;
      fv.type = "f64";
      fv.nMissing = 0;
      return fv;
    };
    std::vector<piet::VarInfo> card;
    for (const char* n : {"a", "b", "c"}) {
      piet::VarInfo v;
      v.name = n;
      v.dtype = "f64";
      card.push_back(v);
    }

    piet::CobraFrameMeta complete;
    complete.valid = true;
    complete.rows = 40;
    complete.cols = 3;
    complete.changed = 3;
    complete.card_stable = false;  // a row filter: the card IS invalid, honestly reported
    complete.varsDelta = {mkVar("a"), mkVar("b"), mkVar("c")};
    check(piet::SessionState::residentCardDeltaComplete(complete, card),
          "MAXPERF S2 fallback gate: a delta naming every surviving column (3 of 3, no add, no "
          "drop) is accepted — the shape today's engine emits for `keep if`");

    piet::CobraFrameMeta partial = complete;
    partial.varsDelta.pop_back();  // 2 of 3 — the lazy/CoW future, arriving silently
    check(!piet::SessionState::residentCardDeltaComplete(partial, card),
          "MAXPERF S2 fallback gate: a PARTIAL delta (2 of 3 columns) is REFUSED, so the card is "
          "rebuilt from the datastore instead of half-filled. This is the leg that matters: the "
          "engine does not contract completeness, and a partial card would ship stale n_missing "
          "for the columns the delta omitted with nothing to flag them");

    piet::CobraFrameMeta collapseShaped = complete;
    collapseShaped.cols = 2;
    collapseShaped.dropped = 1;
    collapseShaped.varsDelta = {mkVar("a"), mkVar("b")};
    check(!piet::SessionState::residentCardDeltaComplete(collapseShaped, card),
          "MAXPERF S2 fallback gate: a delta whose count matches the POST-mutation column count "
          "but which dropped a column is REFUSED — `collapse` returns vars_delta=2 with "
          "dropped=13 on the SHIPPED engine, so 'vars_delta is the varlist' is already wrong "
          "today, not merely after some future change");

    piet::CobraFrameMeta stable = complete;
    stable.card_stable = true;
    check(!piet::SessionState::residentCardDeltaComplete(stable, card),
          "MAXPERF S2 fallback gate: card_stable=true is REFUSED here — a still-valid card "
          "belongs to the S1b fold branch, and rebuilding it from a delta that may legitimately "
          "be empty would blank it");

    piet::CobraFrameMeta renamed = complete;
    renamed.varsDelta[2] = mkVar("zzz");
    check(!piet::SessionState::residentCardDeltaComplete(renamed, card),
          "MAXPERF S2 fallback gate: the right COUNT with the wrong NAME SET is REFUSED — the "
          "guard proves set equality, not arithmetic, which is what licenses taking column order "
          "from the pre-filter card");

    piet::CobraFrameMeta duplicated = complete;
    duplicated.varsDelta[2] = mkVar("a");
    check(!piet::SessionState::residentCardDeltaComplete(duplicated, card),
          "MAXPERF S2 fallback gate: a delta naming one column twice is REFUSED — it would leave "
          "a real column uncovered while the count still added up");
  }

  // -------------------------------------------------------------------------
  // MAXPERF §31 gate (fix/verb-overhead-sweep S3) — post-estimation BY HANDLE,
  // without the re-fit.
  //
  // WHAT IS BEING GATED. lincom/nlcom/test used to rebuild the whole model
  // specification on every call (estimation_m4.cpp's lastModelBaseArgv) and the
  // engine answered by fitting that model AGAIN — measured at 54 lincom calls /
  // 22.61 s in one dependent-variable pass, 17 % of it. contract_A §31 lets the
  // engine retain the fit and PIET address it by an opaque handle, so only the
  // FIRST post-estimation call per model fits.
  //
  // WHY THIS CANNOT BE A VALUE PIN. A resurrected re-fit returns the SAME
  // numbers, just slowly — every coefficient assertion below would stay green
  // with the optimization completely dead. That is the exact defect class this
  // campaign exists to close, so the load-bearing checks here are the two
  // counters:
  //   • est.fits — the ENGINE's own count of post-estimation FITS (as opposed
  //     to reuses of a stored model). One mint followed by N handle calls must
  //     report the same value on all N+1 responses.
  //   • CobraSession::postestHitCount() — PIET's count of calls actually
  //     answered through a stored model.
  // Disable the handle path in ANY way — drop the estHandle field, let
  // runPostEstimationM4's step (1) decline, stop capturing the mint in step
  // (3) — and postestHitCount() reads 0 while est.fits climbs 1,2,3,4, and
  // every value assertion below still passes.
  //
  // THE POISONED-HANDLE LEG IS A CONTROL, not decoration. "fits does not climb"
  // is only meaningful if fits CAN climb; a counter frozen at a constant (the
  // engine failing to increment it) would satisfy the flat assertion
  // vacuously. Forcing one fallback and requiring fits to move proves the
  // assertion has teeth. It doubles as the only headless proof that the exit-3
  // fallback produces the identical answer — the path a PIET pinned to a
  // pre-§31 engine takes on EVERY call (Art 7: the shipped PIET pins the
  // ARCHIVED engine, and the engine lands before PIET at merge).
  //
  // ENVIRONMENT vs REGRESSION. If the engine here has no §31 at all, nothing
  // mints and there is nothing to gate — that reads as an environment result,
  // not a failure, exactly like the fold gates' residentFrameLive() check. Note
  // this discriminates correctly: minting happens on the engine side whether or
  // not PIET consumes it, so a PIET-side regression still mints, still trips
  // the counters, and still goes RED.
  {
    nlohmann::json openBody{{"filepath", fixture}};
    auto openResult = piet::routes::open(session, openBody.dump());
    check(openResult.ok, "postest gate setup: re-`use` fixture returns ok=true");

    auto estRow0 = [](const nlohmann::ordered_json& blocks) -> nlohmann::ordered_json {
      for (const auto& b : blocks) {
        if (!b.is_object() || b.value("type", std::string()) != "estimation") continue;
        auto ct = b.value("coeftable", nlohmann::ordered_json::array());
        if (!ct.empty()) return ct[0];
      }
      return nlohmann::ordered_json::object();
    };

    (void)piet::dispatch("regress price mpg weight", session);

    piet::CobraSession::resetPostestHitCount();

    // Call 1 MINTS. §31 does not mint on feols itself, so this one still fits.
    auto row1 = estRow0(piet::dispatch("lincom mpg + weight", session));
    check(!row1.empty(), "postest gate setup: first `lincom` returns a coeftable row");

    piet::CobraSession* cs = piet::activeCobraSession();
    const bool haveStore = (cs != nullptr) && cs->lastEst().valid;
    if (!haveStore) {
      skip("MAXPERF S31 postest gate (needs a S31-capable cobra serve)",
           "the first `lincom` minted no est handle — this engine predates the session "
           "estimation store, so there is nothing for PIET to spend. ENVIRONMENT result, not a "
           "regression: minting is engine-side and happens whether or not PIET consumes it, so a "
           "PIET-side regression would still mint and would still trip the counters below");
    } else {
      const int64_t fits0 = cs->lastEst().fits;
      const double b1 = row1.value("b", 0.0);
      const double se1 = row1.value("se", 0.0);
      check(fits0 >= 0,
            "MAXPERF S31 postest gate: the minting response carries est.fits (the engine's own "
            "post-estimation FIT count) — without it there is no mechanism signal to assert on");

      // Three more identical combinations on the SAME model. Each must be
      // answered from the stored fit.
      bool valuesIdentical = true;
      for (int i = 0; i < 3; ++i) {
        auto rowN = estRow0(piet::dispatch("lincom mpg + weight", session));
        if (rowN.empty() || rowN.value("b", 0.0) != b1 || rowN.value("se", 0.0) != se1) {
          valuesIdentical = false;
        }
      }

      check(cs->lastEst().fits == fits0,
            "MAXPERF S31 postest gate: est.fits does NOT climb across 3 further `lincom` calls on "
            "one model — the engine reused the stored fit every time. A climbing count means the "
            "re-fit is back and this optimization is dead, which the value checks beside it "
            "CANNOT detect: a resurrected re-fit returns the same numbers, just ~419 ms slower "
            "each");
      check(piet::CobraSession::postestHitCount() == 3,
            "MAXPERF S31 postest gate: PIET answered all 3 through the S31 handle path "
            "(postestHitCount()==3), not by re-sending the full model specification. A count of 0 "
            "with a valid mint above means estimation_m4.cpp's runPostEstimationM4 is not "
            "spending the handle the engine already minted");
      check(valuesIdentical,
            "MAXPERF S31 postest gate: every handle-answered `lincom` is value-identical to the "
            "minting call's b/se — S31 guarantees byte-identical stdout because both entry forms "
            "share one arithmetic tail, so any difference means the handle resolved to the WRONG "
            "model");

      // CONTROL + fallback proof. A deliberately unresolvable handle: §31's
      // nonce is per-PROCESS and its counter process-monotonic, so this string
      // cannot name a live model. Forging a known-BAD handle is not the
      // "construct a handle" §31 forbids — the point is precisely that it does
      // not resolve.
      {
        piet::LastModel poisoned = *session.lastModel();
        poisoned.estHandle = "est:0000000000000000:999999999";
        session.setLastModel(std::move(poisoned));

        piet::CobraSession::resetPostestHitCount();
        auto rowP = estRow0(piet::dispatch("lincom mpg + weight", session));

        check(!rowP.empty() && rowP.value("b", 0.0) == b1 && rowP.value("se", 0.0) == se1,
              "MAXPERF S31 postest gate (fallback proof): an unresolvable handle falls back to the "
              "full model specification and returns the IDENTICAL answer — no error reaches the "
              "user. This is the path every PIET pinned to a pre-S31 engine takes on every call");
        check(cs->lastEst().fits > fits0,
              "MAXPERF S31 postest gate (CONTROL): the forced fallback DID re-fit, so est.fits "
              "climbed. Without this leg the flat-fits assertion above could pass vacuously on an "
              "engine whose counter never increments at all");
        check(piet::CobraSession::postestHitCount() == 0,
              "MAXPERF S31 postest gate (fallback proof): the poisoned call was NOT counted as a "
              "handle hit — a miss must fall through to the specification, never be papered over");
        check(session.lastModel().has_value() && !session.lastModel()->estHandle.empty() &&
                  session.lastModel()->estHandle != "est:0000000000000000:999999999",
              "MAXPERF S31 postest gate: the dead handle was DROPPED and the re-fit's fresh mint "
              "retained in its place — otherwise every subsequent call pays a failed round trip "
              "before falling back");
      }
    }
  }

  std::error_code rmEc;
  std::filesystem::remove_all(workdir, rmEc);  // WP-W5: portable rm -rf
}


// --- 5. (M4b) async-route JS shim contract -----------------------------
//
// Proves window.pietNative.request('do_string', ...) still resolves as a
// normal Promise from the JS side even though the C++ side resolves it
// LATER, off the UI thread's bind() callback (bindAsyncRoute's contract) —
// the exact shape main.cpp's do/do_string routes use. Runs the REAL shim
// source text (piet::internal::pietNativeRequestShimJs(), same string
// Shell::installBridge() injects) under Node with a mock pietNativeInvoke
// that defers its resolution via setTimeout (simulating a worker thread
// resolving later), proving the Promise doesn't resolve/reject prematurely
// and correctly carries the deferred payload once it does.
void testAsyncRouteShimContract() {
  std::printf("[selftest] --- M4b async-route JS shim contract ---\n");
  if (!nodeAvailable()) {
    skip("async-route JS shim contract", "node not found on PATH");
    return;
  }

  std::string script = R"NODE(
    class FakeEventTarget {
      constructor() { this._listeners = {}; }
      addEventListener(type, fn) { (this._listeners[type] = this._listeners[type] || []).push(fn); }
      dispatchEvent(ev) { for (const fn of (this._listeners[ev.type] || [])) fn(ev); return true; }
    }
    class FakeCustomEvent {
      constructor(type, opts) { this.type = type; this.detail = opts.detail; }
    }
    global.EventTarget = FakeEventTarget;
    global.CustomEvent = FakeCustomEvent;
    global.window = global;

    // Simulates the C++ side's async-route dispatch: pietNativeInvoke does
    // NOT resolve immediately (mirrors bindAsyncRoute — the handler returns
    // without calling resolve(); a worker thread calls resolveInvoke()
    // later). Here that "later" is a setTimeout, standing in for the real
    // worker thread's eventual shell->resolveInvoke(seq, ok, json) call.
    window.pietNativeInvoke = function(path, bodyJson) {
      return new Promise(function(resolve) {
        setTimeout(function() {
          // Resolve with an already-PARSED object, mirroring the real webview
          // transport (webview.h resolve() eval-splices the JSON as a JS
          // expression) — NOT a JSON string. See the fidelity note on the
          // other pietNativeInvoke mock above.
          resolve({ blocks: [{ type: 'display', text: 'deferred-ok' }], path: path,
                    echoedBody: JSON.parse(bodyJson) });
        }, 20);
      });
    };
)NODE";
  script += piet::internal::pietEventsShimJs();
  script += "\n";
  script += piet::internal::pietNativeRequestShimJs();
  script += R"NODE(
    let failures = 0;
    function assert(cond, label) {
      if (cond) { console.log('NODE_PASS ' + label); }
      else { console.log('NODE_FAIL ' + label); failures++; }
    }

    const t0 = Date.now();
    window.pietNative.request('do_string', { source: 'display 1\n' }).then(function(result) {
      const elapsed = Date.now() - t0;
      assert(elapsed >= 15, 'promise did not resolve before the deferred resolution fired (elapsed=' + elapsed + 'ms)');
      assert(result.blocks && result.blocks.length === 1, 'deferred result carries the blocks array');
      assert(result.blocks[0].text === 'deferred-ok', 'deferred result block content roundtrips');
      assert(result.path === 'do_string', 'deferred result carries the routing key');
      assert(result.echoedBody && result.echoedBody.source === 'display 1\n',
             'deferred result carries the original request body');
      console.log(failures === 0 ? 'NODE_ALL_PASS' : ('NODE_SOME_FAIL:' + failures));
      process.exit(failures === 0 ? 0 : 1);
    }).catch(function(e) {
      console.log('NODE_FAIL promise rejected: ' + e);
      process.exit(1);
    });
)NODE";

  // WP-W5, same as the two shim legs above: temp_directory_path() is portable
  // ($TMPDIR/%TEMP%). A hardcoded /tmp resolves to a nonexistent C:\tmp here and
  // node fails with MODULE_NOT_FOUND before a single assertion runs.
  std::string scriptPath =
      (std::filesystem::temp_directory_path() / "piet_native_selftest_async_shim.js").u8string();
  {
    std::ofstream f(cecon::to_native_path(scriptPath));
    f << script;
  }
  auto [exitCode, output] = runNode(scriptPath);
  cecon::native_unlink(scriptPath);

  std::istringstream lines(output);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind("NODE_PASS ", 0) == 0 || line.rfind("NODE_FAIL ", 0) == 0) {
      std::printf("[selftest]        %s\n", line.c_str());
    }
  }
  check(exitCode == 0, "async-route JS shim (real source, deferred resolution) — all assertions pass");
  if (exitCode != 0) {
    std::printf("[selftest]        node output:\n%s\n", output.c_str());
  }
}



// §38 getcols — the engine hands back a column SUBSET of the resident frame.
//
// BUG-1372 and BUG-1419, on an inline dataset (the route arms above need the
// auto.hbin fixture, which a fresh worktree does not have).
//   * substr() counts bytes, as Stata's does, so a displayed piece can end inside
//     a character; the console route sends it as U+FFFD, as the do paths do,
//     where a plain dump() threw the whole response away.
//   * an `in` past the data is Stata's own refusal on the row-wise count too
//     (StataBE 19.5: `count if s == "ab" in 5/6` and `assert s == "ab" in 5/6` on
//     3 rows, "observation numbers out of range" r(198)); it said "This `in`
//     row-range form is not supported".
void testSplitByteRouteAndRowRange() {
  std::printf("[selftest] --- BUG-1372 console route / BUG-1419 row-wise in ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_b1372").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  {
    nlohmann::json body{{"cmd", "display substr(\"h\xC3\xA9llo\",3,2)"}};
    auto r = piet::routes::command(session, body.dump());
    check(r.ok, "BUG-1372: command route answers a display that splits a UTF-8 sequence");
    std::string shown;
    if (r.ok) {
      for (const auto& b : nlohmann::json::parse(r.json).value("blocks", nlohmann::json::array()))
        if (b.value("type", std::string()) == "display") shown = b.value("text", std::string());
    }
    check(shown == "\xEF\xBF\xBDl",
          "BUG-1372: the split byte of substr(\"h\u00e9llo\",3,2) arrives as U+FFFD, then l (got '" +
              shown + "')");
  }

  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 3", session);
  (void)piet::dispatch("generate str5 s = \"ab\"", session);
  (void)piet::dispatch("replace s = \"cd\" in 2", session);
  {
    auto inside = piet::strmutate::countRowsIf(session, "s == \"ab\"", std::string("1/3"));
    check(inside.ok && inside.n == 2, "BUG-1419: `count if s == \"ab\" in 1/3` counts 2 (got " +
                                          std::to_string(inside.n) + ")");
    auto outside = piet::strmutate::countRowsIf(session, "s == \"ab\"", std::string("5/6"));
    check(!outside.ok && outside.inRangeError &&
              outside.errMsg == "observation numbers out of range",
          "BUG-1419: `count if s == \"ab\" in 5/6` on 3 rows refuses with Stata's \"observation "
          "numbers out of range\" (got '" + outside.errMsg + "')");
    auto zero = piet::strmutate::countRowsIf(session, "s == \"ab\"", std::string("0"));
    check(!zero.ok && zero.errMsg == "'0' invalid observation number",
          "BUG-1419: `in 0` on the row-wise count is \"'0' invalid observation number\" (got '" +
              zero.errMsg + "')");
  }
  fs::remove_all(workdir, ec);
}


// This is the entrance-side half of the campaign's largest write: PIET's row-wise
// evaluator used to make the engine flush the WHOLE frame to disk just to read the
// two or three columns an expression names (154 of 350 head materializations on the
// reference slice, ~24.8 GB). The leg drives the real serve worker and checks the
// three things a wrong implementation would get wrong:
//   (1) the projection carries the REQUESTED columns and no others,
//   (2) their VALUES survive (a projection that shares payloads must not shift a row),
//   (3) a missing name fails the WHOLE request — a partial file is the worse outcome,
//       because the caller would evaluate against a column set it never asked for.
void testGetcolsProjection() {
  std::printf("[selftest] --- getcols (contract_A 1.25.0 section 38) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_getcols").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 6", session);
  (void)piet::dispatch("gen double a = _n", session);
  (void)piet::dispatch("gen double b = _n * 10", session);
  (void)piet::dispatch("gen double c = _n * 100", session);

  // The worker is spawned lazily by the dispatches above (a resident data verb is
  // what makes one), so ask for the LIVE session rather than forcing a spawn from
  // the test — that keeps this leg honest about environments where serve never
  // came up at all.
  piet::CobraSession* cs = session.cobraSessionIfLive();
  if (cs == nullptr || cs->framesState() == piet::CobraSession::Frames::No) {
    check(true, "getcols PRECONDITION: this environment has no frames-capable `cobra serve` — "
                "leg skipped, read as an ENVIRONMENT result, not a regression");
    return;
  }
  const std::string head = session.currentHbin().value_or(std::string());
  check(!head.empty(), "getcols setup: the session has a head to make resident");
  (void)cs->frameUse(head, nullptr);

  const std::string out = workdir + "/mini.hbin";
  auto r = cs->frameGetcols({"a", "c"}, out);
  check(r.ok, "getcols: a two-column projection succeeds");
  if (r.ok) {
    cecon::Dataset ds = cecon::Dataset::open(out);
    check(ds.n_rows() == 6, "getcols: the projection keeps the frame's row count (6)");
    check(ds.has("a") && ds.has("c"), "getcols: the projection carries BOTH requested columns");
    check(!ds.has("b"), "getcols: the projection carries ONLY the requested columns (no `b`)");
    if (ds.has("a") && ds.has("c")) {
      const double* av = ds.col("a").f64();
      const double* cv = ds.col("c").f64();
      bool ok = true;
      for (int i = 0; i < 6 && ok; ++i)
        ok = (av[i] == static_cast<double>(i + 1)) && (cv[i] == static_cast<double>((i + 1) * 100));
      check(ok, "getcols: values survive the projection (a = 1..6, c = 100..600)");
    }
  }

  const std::string bad = workdir + "/bad.hbin";
  auto rb = cs->frameGetcols({"a", "nosuchcolumn"}, bad);
  check(!rb.ok, "getcols: ONE missing name fails the whole request");
  check(!cecon::native_exists(bad),
        "getcols: a failed request leaves NO file — a partial projection would be evaluated "
        "against silently");
}
