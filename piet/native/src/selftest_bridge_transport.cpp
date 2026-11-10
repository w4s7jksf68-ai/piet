// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: bridge transport.
#include "selftest_support.h"
#include "selftest_cases.h"


// --- 1. Bridge echo -------------------------------------------------------

void testBridgeEcho() {
  std::printf("[selftest] --- bridge echo ---\n");

  auto result = piet::routes::echo(R"({"msg":"hello","n":42})");
  check(result.ok, "echo() returns ok=true for valid JSON body");

  try {
    auto parsed = nlohmann::json::parse(result.json);
    check(parsed.contains("echoed"), "echo() response has top-level 'echoed' key");
    check(parsed["echoed"]["msg"] == "hello", "echo() roundtrips string field verbatim");
    check(parsed["echoed"]["n"] == 42, "echo() roundtrips numeric field verbatim");
  } catch (const nlohmann::json::exception& e) {
    check(false, std::string("echo() response is valid JSON (") + e.what() + ")");
  }

  auto badResult = piet::routes::echo("{not json");
  check(!badResult.ok, "echo() returns ok=false for malformed JSON body");

  // REAL JS ENGINE CHECK: take the exact shim source text shell.cpp injects
  // (piet::internal::pietEventsShimJs() + pietNativeRequestShimJs() — the
  // very same strings, not a paraphrase) and execute it under Node with a
  // mocked `window`, `EventTarget`, `CustomEvent`, and a fake
  // `pietNativeInvoke` that calls back into a JSON echo (mirroring what the
  // real C++ bind() handler does — resolving with a PARSED value, see the mock
  // note below). This proves: (a) the shim is syntactically valid JS, (b)
  // window.pietNative.request() correctly wraps pietNativeInvoke into a Promise
  // and passes the already-parsed transport result straight through WITHOUT
  // re-parsing (the webview bind delivers a parsed JS value, not a JSON string;
  // re-parsing would throw "Unexpected identifier object"), (c) window.pietEvents
  // dispatches a 'piet-event' CustomEvent with the parsed detail when
  // __pietDispatchEvent is invoked (the exact mechanism Shell::postEvent()
  // drives). What this does NOT prove: WKWebView's real init-timing/CSP/file://
  // origin behaviour — that gap is covered by the packaged-app smoke (kernel
  // dot must go green), not this selftest.
  if (!nodeAvailable()) {
    skip("JS shim execution under Node", "node not found on PATH");
  } else {
    std::string script = R"NODE(
      // --- minimal browser-like shims -----------------------------------
      class FakeEventTarget {
        constructor() { this._listeners = {}; }
        addEventListener(type, fn) {
          (this._listeners[type] = this._listeners[type] || []).push(fn);
        }
        dispatchEvent(ev) {
          for (const fn of (this._listeners[ev.type] || [])) fn(ev);
          return true;
        }
      }
      class FakeCustomEvent {
        constructor(type, opts) { this.type = type; this.detail = opts.detail; }
      }
      global.EventTarget = FakeEventTarget;
      global.CustomEvent = FakeCustomEvent;
      global.window = global;

      // Fake native bridge: echoes {mocked:true, path, body} back, exactly
      // like piet::routes::echo() wraps its input under an "echoed" key —
      // shaped differently on purpose so we can assert the SHIM correctly
      // threads path/body through, not just that echo() itself works (that
      // is covered separately by the direct C++ call in testBridgeEcho()).
      //
      // CRITICAL FIDELITY: resolve with an already-PARSED object, NOT a JSON
      // string. The real webview transport delivers the route result to JS by
      // EVAL-splicing the JSON text as a JS expression (vendored webview.h
      // resolve(): eval("...resolve(" + result + ")")), so the bound function
      // resolves with a parsed JS value. A mock that returned JSON.stringify(...)
      // here would let a double-JSON.parse bug in the shim pass this test while
      // throwing "Unexpected identifier object" against the actual WKWebView —
      // which is precisely the regression that once shipped and broke the whole
      // file:// webview UI. Keep this returning a bare object.
      window.pietNativeInvoke = function(path, bodyJson) {
        return Promise.resolve({ mocked: true, path: path, body: JSON.parse(bodyJson) });
      };

      // --- the REAL shim source, unmodified -----------------------------
)NODE";
    script += piet::internal::pietEventsShimJs();
    script += "\n";
    script += piet::internal::pietNativeRequestShimJs();
    script += R"NODE(
      // --- assertions -----------------------------------------------------
      let failures = 0;
      function assert(cond, label) {
        if (cond) { console.log('NODE_PASS ' + label); }
        else { console.log('NODE_FAIL ' + label); failures++; }
      }

      assert(typeof window.pietNative === 'object', 'window.pietNative installed');
      assert(typeof window.pietNative.request === 'function', 'window.pietNative.request is a function');
      assert(typeof window.pietEvents === 'object', 'window.pietEvents installed');
      assert(typeof window.__pietDispatchEvent === 'function', '__pietDispatchEvent installed');

      let receivedEvent = null;
      window.pietEvents.addEventListener('piet-event', function(ev) { receivedEvent = ev.detail; });
      window.__pietDispatchEvent(JSON.stringify({ running: true, current_line: 3 }));
      assert(receivedEvent !== null, 'piet-event listener fired synchronously');
      assert(receivedEvent && receivedEvent.running === true, 'dispatched event detail.running roundtrips');
      assert(receivedEvent && receivedEvent.current_line === 3, 'dispatched event detail.current_line roundtrips');

      window.pietNative.request('echo', { msg: 'hi', n: 7 }).then(function(result) {
        assert(result.mocked === true, 'pietNative.request resolves the mocked promise');
        assert(result.path === 'echo', 'pietNative.request threads path through to pietNativeInvoke');
        assert(result.body && result.body.msg === 'hi', 'pietNative.request JSON-encodes body correctly');
        assert(result.body && result.body.n === 7, 'pietNative.request preserves numeric fields in body');
        console.log(failures === 0 ? 'NODE_ALL_PASS' : ('NODE_SOME_FAIL:' + failures));
        process.exit(failures === 0 ? 0 : 1);
      }).catch(function(e) {
        console.log('NODE_FAIL promise rejected: ' + e);
        process.exit(1);
      });
)NODE";

    // WP-W5: temp_directory_path() is portable ($TMPDIR/%TEMP%) — no hardcoded
    // /tmp so this path resolves on Windows too.
    std::string scriptPath =
        (std::filesystem::temp_directory_path() / "piet_native_selftest_shim.js")
            .u8string();
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
    check(exitCode == 0, "JS shim (real source, executed under Node) — all assertions pass");
    if (exitCode != 0) {
      std::printf("[selftest]        node output:\n%s\n", output.c_str());
    }
  }
}


// --- 1b. Windows file-drop bridge seam ------------------------------------
//
// Runs on EVERY platform, deliberately: the file_dropped payload is a shared
// Boundary-B contract, so the macOS build must prove it emits the same bytes
// the Windows AdditionalObjects producer does. The Node leg additionally
// executes the ACTUAL injected shim source (bridge_internal.cpp's
// pietWindowsFileDropShimJs), not a paraphrase — same discipline as
// testBridgeEcho above.
void testWindowsFileDropBridgeSeam() {
  std::printf("[selftest] --- Windows file-drop bridge seam ---\n");

  const std::string path = u8R"(C:\Users\홍 길동\자료\"quoted".dta)";
  const std::string eventJson = piet::internal::fileDroppedEventJson(path);
  try {
    nlohmann::ordered_json event = nlohmann::ordered_json::parse(eventJson);
    check(event.size() == 2, "file_dropped event has exactly kind + path");
    check(event.value("kind", std::string()) == "file_dropped",
          "file_dropped event kind is exact");
    check(event.value("path", std::string()) == path,
          "file_dropped event roundtrips UTF-8, spaces, backslashes, and quotes");
    check(eventJson.rfind(R"({"kind":"file_dropped","path":)", 0) == 0,
          "file_dropped event preserves documented Boundary-B key order");
  } catch (const nlohmann::json::exception& e) {
    check(false, std::string("file_dropped event is valid JSON (") + e.what() + ")");
  }

  if (!nodeAvailable()) {
    skip("Windows file-drop document-start shim", "node not found on PATH");
    return;
  }

  std::string script = R"NODE(
    let failures = 0;
    function assert(cond, label) {
      if (cond) { console.log('NODE_PASS ' + label); }
      else { console.log('NODE_FAIL ' + label); failures++; }
    }
    let dropHandler = null;
    let captureFlag = null;
    const calls = [];
    global.window = {
      addEventListener: function(type, fn, capture) {
        if (type === 'drop') {
          dropHandler = fn;
          captureFlag = capture;
        }
      },
      chrome: {
        webview: {
          postMessageWithAdditionalObjects: function(tag, objects) {
            calls.push({ tag: tag, objects: objects });
          }
        }
      }
    };
)NODE";
  script += piet::internal::pietWindowsFileDropShimJs();
  script += R"NODE(
    assert(typeof dropHandler === 'function', 'actual shim installs one drop listener');
    assert(captureFlag === true, 'drop listener is capture-phase');
    const file = { name: '자료.dta' };
    dropHandler({ dataTransfer: { files: [file] } });
    assert(calls.length === 1, 'one dropped File produces exactly one native message');
    assert(calls[0] && calls[0].tag === 'piet:file-drop:v1', 'reserved carrier tag is exact');
    assert(calls[0] && calls[0].objects.length === 1 && calls[0].objects[0] === file,
           'carrier contains the original File as its sole AdditionalObject');

    dropHandler({ dataTransfer: { files: [] } });
    assert(calls.length === 1, 'empty DataTransfer produces no native message');
    delete window.chrome.webview.postMessageWithAdditionalObjects;
    dropHandler({ dataTransfer: { files: [file] } });
    assert(calls.length === 1, 'runtime without AdditionalObjects fails closed');

    console.log(failures === 0 ? 'NODE_ALL_PASS' : ('NODE_SOME_FAIL:' + failures));
    process.exit(failures === 0 ? 0 : 1);
)NODE";

  const std::string scriptPath =
      (std::filesystem::temp_directory_path() / "piet_native_selftest_file_drop.js")
          .u8string();
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
  check(exitCode == 0,
        "Windows file-drop shim (actual injected source, executed under Node) passes");
  if (exitCode != 0) {
    std::printf("[selftest]        node output:\n%s\n", output.c_str());
  }
}


// --- 2. Progress push channel ---------------------------------------------

void testProgressStream() {
  std::printf("[selftest] --- progress stream ---\n");

  std::vector<nlohmann::json> ticks;
  piet::runSyntheticProgress([&ticks](const std::string& tickJson) {
    ticks.push_back(nlohmann::json::parse(tickJson));
  });

  check(ticks.size() == 6, "full run emits 6 ticks (5 steps + 1 final)");

  bool schemaOk = true;
  for (const auto& t : ticks) {
    for (const char* field : {"running", "current_line", "total_lines", "current_cmd",
                               "elapsed_ms", "blocks_ready", "do_path", "cancel_requested",
                               "stopped_reason"}) {
      if (!t.contains(field)) {
        schemaOk = false;
        std::printf("[selftest]        missing field '%s' in tick: %s\n", field, t.dump().c_str());
      }
    }
  }
  check(schemaOk, "every tick has all 9 B1 schema fields (frozen contract_B v1.11.0)");

  check(ticks.front()["running"] == true, "first tick has running=true");
  check(ticks.front()["current_line"] == 1, "first tick current_line=1");
  check(ticks.back()["running"] == false, "final tick has running=false");
  check(ticks.back()["stopped_reason"] == "completed", "final tick stopped_reason='completed'");
  check(ticks.back()["current_line"] == 5, "final tick current_line=5 (=total_lines)");
  check(ticks.back()["blocks_ready"] == 5, "final tick blocks_ready=5");

  bool monotonic = true;
  for (size_t i = 1; i < ticks.size(); ++i) {
    if (ticks[i]["elapsed_ms"].get<long long>() < ticks[i - 1]["elapsed_ms"].get<long long>()) {
      monotonic = false;
    }
  }
  check(monotonic, "elapsed_ms is non-decreasing across ticks");

  // Cancel path: start on a background thread, flip the cancel flag after
  // the first tick observed, confirm we get a stopped_reason="cancelled"
  // tick and the stream stops short of all 5 lines.
  std::atomic<bool> cancelFlag{false};
  std::vector<nlohmann::json> cancelTicks;
  std::thread runner([&]() {
    piet::runSyntheticProgress(
        [&](const std::string& tickJson) {
          auto j = nlohmann::json::parse(tickJson);
          cancelTicks.push_back(j);
          if (cancelTicks.size() == 1) {
            cancelFlag.store(true);  // request cancel right after the first tick
          }
        },
        &cancelFlag);
  });
  runner.join();

  check(!cancelTicks.empty(), "cancel-path run produced at least one tick");
  if (!cancelTicks.empty()) {
    check(cancelTicks.back()["stopped_reason"] == "cancelled",
          "cancel-path final tick stopped_reason='cancelled'");
    check(cancelTicks.back()["cancel_requested"] == true,
          "cancel-path final tick cancel_requested=true");
    check(cancelTicks.back()["running"] == false, "cancel-path final tick running=false");
    check(cancelTicks.size() < 6, "cancel-path stream stopped short of the full 6-tick run");
  }

  // BUG-1068: a cancel that arrives while a run is in flight but is not seen by
  // it (it lands after the last poll) must not carry over to the next block run
  // on the session. It did, and that block stopped at its first line with "run
  // cancelled" -- the C4-L3 lazy-binder block failed six checks that way whenever
  // the M4b leg's racing run_cancel arm landed inside its run (a loaded machine).
  {
    piet::SessionState session(
        (std::filesystem::temp_directory_path() / "piet_native_selftest_bug1068").u8string());
    session.progressStart(1);
    check(session.progressRequestCancel(),
          "BUG-1068 setup: the cancel lands on a run in flight");
    session.progressFinish("completed");
    auto r = piet::doexec::executeDoSource("if 1 {\n  local b1068 = 7\n}\n", session,
                                           piet::dispatch);
    check(!r.stoppedAtLine.has_value() && session.eval.locals.get("b1068", "") == "7",
          "BUG-1068: a block run after the cancelled run's end executes (the stale cancel "
          "does not stop it at line 1)");
    check(session.progressSnapshot().value("cancel_requested", false) == true,
          "BUG-1068: the finished run's record still reports that a cancel was asked");
  }
}


// --- 3. cobra roundtrip -----------------------------------------------------

void testCobraRoundtrip() {
  std::printf("[selftest] --- cobra version --json roundtrip ---\n");

  std::printf("[selftest]        resolved cobra bin: %s\n", piet::resolveCobraBin().c_str());

  auto result = piet::routes::cobraVersion("{}");
  check(result.ok, "cobra.version route returns ok=true");

  if (result.ok) {
    try {
      auto parsed = nlohmann::json::parse(result.json);
      check(parsed.contains("cobra_version"), "cobra version JSON has 'cobra_version' field");
      check(parsed.contains("contract_version"), "cobra version JSON has 'contract_version' field");
      if (parsed.contains("cobra_version")) {
        std::printf("[selftest]        cobra_version = %s\n",
                     parsed["cobra_version"].get<std::string>().c_str());
      }
    } catch (const nlohmann::json::exception& e) {
      check(false, std::string("cobra version response is valid JSON (") + e.what() + ")");
    }
  } else {
    std::printf("[selftest]        cobra.version FAILED: %s\n", result.json.c_str());
  }
}


// --- 4. M1 evaluator smoke ---------------------------------------------------
// Compact native-only checks over the M1 evaluator port (tokenizer/gettoken/
// errors). NOT the parity gate — that is the differential harness in
// tests/eval_diff/ (run_diff.py, gate = 100% vs the Python 1.16.0 oracle).
// This leg just keeps `--selftest` covering M1 code paths headlessly.

void testEvaluatorSmoke() {
  std::printf("[selftest] --- M1 evaluator smoke ---\n");

  using piet::eval::tokenize;
  using piet::eval::TokenizerError;

  // tokenize: full qualifier + option surface
  try {
    auto pc = tokenize("regress y x if x>0, robust vce(cluster id)");
    check(pc.cmd == "regress", "tokenize: cmd extracted");
    check(pc.varlist.size() == 2 && pc.varlist[0] == "y" && pc.varlist[1] == "x",
          "tokenize: varlist extracted");
    check(pc.ifExpr && *pc.ifExpr == "x>0", "tokenize: if-expression extracted");
    check(pc.opts.size() == 2 && pc.opts[0].first == "robust" && pc.opts[0].second.isBool &&
              pc.opts[1].first == "vce" && pc.opts[1].second.strValue == "cluster id",
          "tokenize: options parsed (flag + valued)");
  } catch (const TokenizerError&) {
    check(false, "tokenize: valid command must not throw");
  }

  // tokenize: macro ref rejected with the frozen error shape
  try {
    tokenize("display `x'");
    check(false, "tokenize: macro ref must throw TokenizerError");
  } catch (const TokenizerError& e) {
    check(e.block.at("code") == "unsupported_macro", "tokenize: macro ref -> unsupported_macro");
    check(e.block.at("token") == "`x'", "tokenize: macro ref token captured");
  }

  // tokenize: estimation weights rejected (Doctrine ① guard). `list` carries
  // WeightPolicy::None (unaffected by the P3 weights wave — `regress` used to
  // be this probe's command, but it moved to WeightPolicy::AllWeights and
  // [aw=] is now legal there; see testBS096QualifierPolicyTable for that
  // newly-legal case).
  try {
    tokenize("list y x [aw=w]");
    check(false, "tokenize: weight must throw TokenizerError");
  } catch (const TokenizerError& e) {
    check(e.block.at("code") == "unsupported_option", "tokenize: weight -> unsupported_option");
  }

  // scanOneToken: compound-quote awareness, bind semantics
  {
    auto [tok, rest] = piet::eval::scanOneToken("`\"foo bar\"' tail", /*bind=*/false);
    check(tok == "`\"foo bar\"'" && rest == "tail",
          "scanOneToken: compound token preserved without bind");
    auto [tok2, rest2] = piet::eval::scanOneToken("`\"foo bar\"' tail", /*bind=*/true);
    check(tok2 == "foo bar" && rest2 == "tail", "scanOneToken: bind strips compound quotes");
  }

  // gettoken: peel + aliasing (SRC read fully before binding)
  {
    std::vector<std::pair<std::string, std::string>> locals{{"src", "a b c"}};
    piet::eval::LocalsStore store;
    store.get = [&locals](const std::string& n) -> std::string {
      for (auto& [k, v] : locals) if (k == n) return v;
      return "";
    };
    store.set = [&locals](const std::string& n, const std::string& v) {
      for (auto& [k, ev] : locals) if (k == n) { ev = v; return; }
      locals.emplace_back(n, v);
    };
    auto blocks = piet::eval::runGettoken("gettoken src rest : src", store);
    check(blocks.empty(), "gettoken: silent on success");
    // BUG-1499: the rest keeps its leading blank (StataBE 19.5: `alpha beta`
    // -> ` beta`).
    check(store.get("src") == "a" && store.get("rest") == " b c",
          "gettoken: aliased SRC==TOK reads old value first");
    auto errBlocks = piet::eval::runGettoken("gettoken oops", store);
    check(errBlocks.size() == 1 && errBlocks[0].at("code") == "syntax_error",
          "gettoken: malformed line -> syntax_error block");
  }

  // errors: unknown code coerced to engine_error (frozen contract shape)
  {
    auto b = piet::eval::errorBlock("not_a_code", "msg");
    check(b.at("code") == "engine_error", "errorBlock: unknown code coerced to engine_error");
    check(b.at("type") == "error", "errorBlock: type field frozen");
  }

  // pyeval: Python-semantics expression engine (M1 exprfns commit)
  {
    using piet::eval::PyValue;
    piet::eval::PyNamespace ns = piet::eval::mathNamespace(/*withInfNan=*/false);
    piet::eval::addStataFns(ns);

    auto r1 = piet::eval::evalPyExpr("7/2", ns);
    check(r1 && r1->isFloat() && r1->floatVal() == 3.5, "pyeval: true division 7/2 == 3.5");

    auto r2 = piet::eval::evalPyExpr("-2**2", ns);
    check(r2 && r2->isInt() && r2->intVal() == -4, "pyeval: -2**2 == -4 (Python precedence)");

    auto r3 = piet::eval::evalPyExpr("1 < 2 < 3", ns);
    check(r3 && r3->isBool() && r3->boolVal(), "pyeval: chained comparison 1<2<3");

    auto r4 = piet::eval::evalPyExpr("0 or 5", ns);
    check(r4 && r4->isInt() && r4->intVal() == 5, "pyeval: `or` returns the deciding operand");

    auto r5 = piet::eval::evalPyExpr("1/0", ns);
    check(!r5, "pyeval: ZeroDivisionError -> failure (None)");

    auto r6 = piet::eval::evalPyExpr("inlist(2,1,2,3)", ns);
    check(r6 && r6->isBool() && r6->boolVal(), "pyeval: STATA_FNS injected (inlist)");

    auto r7 = piet::eval::evalPyExpr("lower(strtrim(\"  NO  \"))", ns);
    check(r7 && r7->isStr() && r7->strVal() == "no", "pyeval: nested string fns");

    check(piet::eval::pythonFloatRepr(1e16) == "1e+16" &&
              piet::eval::pythonFloatRepr(1e15) == "1000000000000000.0" &&
              piet::eval::pythonFloatRepr(0.1) == "0.1",
          "pythonFloatRepr: str(float) fixed/scientific thresholds");

    auto rMod = piet::eval::fnMod({PyValue::integer(-7), PyValue::integer(3)});
    check(rMod.isFloat() && rMod.floatVal() == 2.0, "exprfns: mod(-7,3) == 2 (sign of b)");
  }

  // macro engine (M1 macros commit)
  {
    piet::eval::EvalSession s;

    // D1-① DRIFT GUARD (WS-D1). tryCaptureDefinition now recognises
    // `local NAME rest` / `global NAME rest` with a hand-written single-pass
    // scan rather than reLocalDef()/reGlobalDef(), because the profile put
    // those two regex matches at ~2 us of the ~3.3 us every dispatched line
    // was paying. A hand-written twin of a regex is only safe while it cannot
    // drift from it, so this re-runs BOTH over a corpus of forms — matched,
    // unmatched, and the awkward boundaries (no space after the keyword, tab
    // and vertical-tab runs, embedded newlines, non-identifier names, case
    // variants, `:`/`,`/`)` immediately after the name) — and fails with the
    // first disagreement rather than a bare false.
    {
      std::string disagreement;
      check(piet::eval::selftestMacroDefScanMatchesRegex(&disagreement),
            "macros: local/global single-pass scan == reLocalDef/reGlobalDef" +
                (disagreement.empty() ? std::string() : " [" + disagreement + "]"));
    }

    // EXPRLAYER DRIFT GUARDS (P2-PERF). Five more hand-written twins of
    // regexes the expression-layer profile named, held to their originals the
    // same way. The first four are equivalence gates; the fifth is the
    // macro-expansion SEMANTICS gate the arc's parity-hazard list names first
    // (undefined macro, `=expr', nesting, `"..."' quoting, r()/e()/s(), and
    // the identifier-ending-in-r/e decoys the tightened pre-filters created).
    {
      std::string d;
      check(piet::eval::selftestSafeArithScanMatchesRegex(&d),
            "macros: isSafeArith/Ext scans == reSafeArith/reSafeArithExt" +
                (d.empty() ? std::string() : " [" + d + "]"));
    }
    {
      std::string d;
      check(piet::eval::selftestBarePrefilterIsStrict(&d),
            "macros: bare r()/e() pre-filters are strict for reRRef/reERef/reRRefDelim" +
                (d.empty() ? std::string() : " [" + d + "]"));
    }
    {
      std::string d;
      check(piet::eval::selftestScalarScansMatchRegex(&d),
            "macros: scalar() / bare-name / scalar-drop / scalar-def scans == their regexes" +
                (d.empty() ? std::string() : " [" + d + "]"));
    }
    {
      std::string d;
      check(piet::eval::selftestLocalRefScanMatchesRegex(&d),
            "macros: `NAME' reference scan == reLocalRef" +
                (d.empty() ? std::string() : " [" + d + "]"));
    }
    {
      std::string d;
      check(piet::loops::selftestBoolIdentScanMatchesRegex(&d),
            "loops: evalBoolExpr referenced-ident scan == [A-Za-z_]\\w* iterator" +
                (d.empty() ? std::string() : " [" + d + "]"));
    }
    {
      std::string d;
      check(piet::eval::selftestExpansionSemantics(&d),
            "macros: expansion semantics unchanged (S32-class corpus)" +
                (d.empty() ? std::string() : " [" + d + "]"));
    }

    check(piet::eval::tryCaptureDefinition("local a mpg", s) &&
              s.locals.get("a") == "mpg",
          "macros: local definition captured");
    check(piet::eval::tryCaptureDefinition("local b `a' weight", s) &&
              s.locals.get("b") == "mpg weight",
          "macros: append/nested RHS expanded at definition");
    check(piet::eval::tryCaptureDefinition("local n = 2+3", s) && s.locals.get("n") == "5",
          "macros: `= expr` arithmetic evaluated");
    check(piet::eval::tryCaptureDefinition("local w : word 2 of x y z", s) &&
              s.locals.get("w") == "y",
          "macros: extended macro fn (word N of)");

    auto exp = piet::eval::expandMacros("regress price `b'", s);
    check(!exp.isError && exp.line == "regress price mpg weight",
          "macros: expand_macros resolves refs");

    // BUG-294: undefined -> empty at rc 0 (Stata 19.5 IC, measured). This
    // check used to assert the engine_error that stopped the do-file.
    auto err = piet::eval::expandMacros("display `undef'", s);
    check(!err.isError && err.line == "display ",
          "macros: BUG-294 undefined local expands to empty at rc 0");
    auto errG = piet::eval::expandMacros("display [$UNDEFGLOBAL]", s);
    check(!errG.isError && errG.line == "display []",
          "macros: BUG-294 undefined global expands to empty at rc 0");

    piet::eval::tryCaptureDefinition("local loopy `loopy'", s);
    // self-referential: `local loopy `loopy'` stores "" (undefined ref at
    // definition time expands empty? no — single pass fails, stores
    // literal); expanding a TRUE self-reference must terminate cleanly.
    s.locals.set("selfref", "`selfref'");
    auto circ = piet::eval::expandMacros("display `selfref'", s);
    check(circ.isError &&
              circ.error.at("message").get<std::string>().find("did not converge") !=
                  std::string::npos,
          "macros: circular macro terminates with clean non-convergence error");

    auto arith = piet::eval::evalArith("2^8", nullptr);
    check(arith && *arith == "256", "macros: _eval_arith handles ^ as exponentiation");

    auto mex = piet::eval::evalMacroExpr("2+3*4", s);
    check(mex && *mex == "14", "macros: eval_macro_expr (`=expr' form)");

    piet::eval::EvalSession s2;
    nlohmann::ordered_json block{
        {"type", "summarize"},
        {"stats", {{"price", {{"N", 74}, {"mean", 6165.25}, {"sd", 2949.5}}}}}};
    piet::eval::captureRFromBlock(block, s2);
    check(s2.rValues.find("mean") != nullptr && *s2.rValues.find("mean") == 6165.25 &&
              s2.rValues.find("Var") != nullptr,
          "macros: capture_r_from_block populates r() incl. Var=sd^2");

    // BUG-344 is covered END TO END by testBug344SummarizeSum() below, through
    // the user-visible `display r(sum)` path. It is deliberately NOT tested here
    // by feeding captureRFromBlock a hand-built block: this mapping has read
    // `sum`/`sum_w` all along, so such a leg would have passed for the entire
    // time the bug was live. The producer is the half that was missing.

    // `= <string literal>` must store the CONTENTS, not the quotes. Stata's
    // `=` form evaluates its RHS and a bare literal evaluates to its text.
    // Regression pin: c() string values arrive pre-quoted from
    // substituteCRefs, so keeping the quotes made every macro-built path
    // expand to <cwd>/"<value>"/file — invisible to `display` (adjacent
    // quoted segments concatenate) and fatal to `use`.
    piet::eval::EvalSession s3;
    piet::eval::tryCaptureDefinition("local q = \"C:/some/dir\"", s3);
    check(s3.locals.get("q", "<unset>") == "C:/some/dir",
          "macros: `local x = \"str\"` stores the string WITHOUT its quotes");
    piet::eval::tryCaptureDefinition("global gq = \"C:/some/dir\"", s3);
    check(s3.globals.get("gq", "<unset>") == "C:/some/dir",
          "macros: `global x = \"str\"` stores the string WITHOUT its quotes or the `=`");
    piet::eval::tryCaptureDefinition("global gn = 2+3", s3);
    check(s3.globals.get("gn", "<unset>") == "5",
          "macros: `global x = expr` EVALUATES (globals had no `=` branch at all)");
    // Guard the conservative half: a non-literal expression keeps its quotes
    // because stripOuterDquotes refuses anything with an interior quote.
    piet::eval::tryCaptureDefinition("local cat = \"a\" + \"b\"", s3);
    check(s3.locals.get("cat", "<unset>").find('"') != std::string::npos ||
              s3.locals.get("cat", "<unset>") == "ab",
          "macros: a multi-literal `= ` RHS is not silently half-stripped");
  }
}
