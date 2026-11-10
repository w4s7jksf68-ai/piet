// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Bridge route table of the native app window and the background do/do_string runner.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <signal.h>   // kill(pid, 0) — BUG-133 orphan-scratch liveness probe
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "base64.h"
#include "piet/bundle_paths.h"
#include "piet/counters.h"
#include "piet/strmutate.h"
#include "piet/doexec.h"
#include "piet/dispatcher.h"
#include "piet/eval/tokenizer.h"
#include "piet/parallel.h"
#include "piet/progress.h"
#include "piet/routes.h"
#include "piet/session_state.h"
#include "piet/settings.h"
#include "piet/shell.h"
#include "dispatch/registry.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)
#include "vendor/nlohmann/json.hpp"
#include "app_entry_internal.h"

namespace app_entry_detail {

namespace {

// Background do/do_string runner: spawns an OWNED worker thread (registered
// with AppState, joined at shutdown — never detached) that executes the
// do-source, pushes RunProgress ticks over pietEvents as it goes, and
// resolves the bridge promise with the final result once done. `shell` is
// captured by pointer (not reference) into the thread lambda specifically
// so the thread-lifetime discipline is visible at the call site: the
// pointer is only ever dereferenced from a thread AppState::joinAll() joins
// before the Shell is torn down, so it never dangles — see the file header
// and AppState's doc comment for the full argument.
void runDoInBackground(piet::Shell* shell, AppState* app, const std::string& seq,
                        const std::string& source, std::vector<std::string> args,
                        std::optional<std::string> filepath) {
  app->reapFinished();
  auto runFinished = std::make_shared<std::atomic<bool>>(false);  // BUG-391
  std::thread worker([shell, app, seq, source, args = std::move(args),
                       filepath = std::move(filepath), runFinished]() {
    // BUG-087/D9 (BETA Phase F): mark this exact thread as the one
    // SessionState::requestUserInput() (a blocking wait) may safely run on
    // — see session_state.h's isCurrentThreadADoWorker() doc comment for
    // why blocking any OTHER thread (the UI thread in particular) would
    // deadlock the app. Set once, at the top of this thread's one-shot
    // body; no corresponding reset needed since the thread terminates when
    // this lambda returns.
    piet::setCurrentThreadIsDoWorker(true);

    // Progress-push side channel: poll the session's own progress snapshot
    // on a short interval WHILE the run's main work happens synchronously
    // below on this SAME worker thread would be wrong (executeDoSource
    // blocks until done) — instead, spawn a second, even shorter-lived
    // "ticker" thread that reads progressSnapshot() (mutex-guarded, safe
    // concurrent read) and postEvent()s it every 100ms until a stop flag
    // (set by this outer thread right before returning) tells it to stop
    // and push one FINAL snapshot.
    std::atomic<bool> runDone{false};
    // The snapshot carries the running statement's text, which can hold a
    // byte that is not UTF-8; a strict dump would throw on this bare thread
    // and std::terminate the app, so it goes out as U+FFFD (BUG-1131).
    std::thread ticker([shell, app, &runDone]() {
      while (!runDone.load()) {
        shell->postEvent(app->session.progressSnapshot().dump(
            -1, ' ', false, nlohmann::ordered_json::error_handler_t::replace));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });

    // N6 (P8.1-W2, contract_B 2.4.0): live block streaming. This sink is
    // called SYNCHRONOUSLY from executeDo*/executeDoSource on THIS worker
    // thread (the only writer of streamedSeq — no lock needed on the counter),
    // once per block as it is appended, and pushes a `{kind:"block", seq,
    // block}` event over the SAME pietEvents conduit the RunProgress ticker
    // uses. postEvent marshals onto the UI thread (dispatch_async — thread-
    // safe, so it interleaves fine with the concurrent ticker postEvent()s and
    // with any request_dialog push a mid-run `_request()` pause emits). seq is
    // monotone from 0; the final `streamed` below == this count.
    int streamedSeq = 0;
    auto onBlock = [shell, &streamedSeq](const nlohmann::ordered_json& block) {
      nlohmann::ordered_json ev;
      ev["kind"] = "block";
      ev["seq"] = streamedSeq++;
      ev["block"] = block;
      shell->postEvent(ev.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace));
    };

    // F6c tail-only return: move blocks[streamedCount:] — the UN-streamed
    // remainder (the terminal cancel block, or the whole array for a no-onBlock
    // caller) — into the response. The already-streamed prefix is NOT re-sent;
    // this eliminates F6's full-array copy+dump spike (the old code copied the
    // ENTIRE stored array into the JSON, dumped it, shipped it over the bridge,
    // and the frontend then discarded blocks[:streamed]). std::move out of the
    // (dead-after-this) ExecuteResult, so no deep copy.
    auto tailBlocks = [](piet::doexec::ExecuteResult& r) {
      nlohmann::ordered_json tail = nlohmann::ordered_json::array();
      int start = r.streamedCount < 0 ? 0 : r.streamedCount;
      for (int i = start; i < static_cast<int>(r.blocks.size()); ++i) {
        tail.push_back(std::move(r.blocks[static_cast<size_t>(i)]));
      }
      return tail;
    };

    nlohmann::ordered_json out;
    try {
      if (filepath.has_value()) {
        piet::doexec::ExecuteFileResult fr =
            piet::doexec::executeDo(*filepath, app->session, piet::dispatch, args, onBlock);
        out["blocks"] = tailBlocks(fr.result);
        if (fr.result.stoppedAtLine.has_value()) out["stopped_at_line"] = *fr.result.stoppedAtLine;
        if (fr.result.stoppedAtReason.has_value())
          out["stopped_at_reason"] = *fr.result.stoppedAtReason;
        out["resolved_path"] = fr.resolvedPath;
      } else {
        piet::doexec::ExecuteResult r = piet::doexec::executeDoSource(
            source, app->session, piet::dispatch, args, /*progressOwner=*/true, std::nullopt, onBlock);
        out["blocks"] = tailBlocks(r);
        if (r.stoppedAtLine.has_value()) out["stopped_at_line"] = *r.stoppedAtLine;
        if (r.stoppedAtReason.has_value()) out["stopped_at_reason"] = *r.stoppedAtReason;
      }
      // F6c dedup contract (contract_B 2.5.0): `streamed` = count of
      // {kind:"block"} pushes this run. `blocks` above is now ONLY the
      // un-streamed tail, so the frontend appends ALL of it (total = the
      // `streamed` live blocks + these blocks — no slice). The cancel block,
      // never streamed, rides in via that tail. (Pre-2.5.0 the response echoed
      // the FULL array and the frontend sliced blocks[streamed:] — the frontend
      // MUST drop that slice; see CONTRACT.md v2.5.0.)
      out["streamed"] = streamedSeq;
    } catch (const std::exception& e) {
      // A C++-level exception mid-run (not a Stata-level error block, which
      // executeDoSource already reports as a normal block) — surface it the
      // same way Shell::installBridge()'s handler_exception path does, so
      // the frontend's error-shape mapping (M0 carry-forward #2, api.js
      // seam) sees ONE consistent shape regardless of where the failure
      // originated.
      out = nlohmann::ordered_json::object();
      out["blocks"] = nlohmann::ordered_json::array();
      nlohmann::ordered_json errBlock;
      errBlock["type"] = "error";
      errBlock["code"] = "engine_error";
      errBlock["message"] = std::string("run failed: ") + e.what();
      out["blocks"].push_back(errBlock);
    }

    runDone.store(true);
    ticker.join();
    // Final snapshot push AFTER progressFinish() has landed (executeDo*
    // calls progressFinish() internally before returning), so the last
    // pietEvents tick the frontend sees has running:false + the real
    // stopped_reason — never a stale "still running" snapshot.
    shell->postEvent(app->session.progressSnapshot().dump(
        -1, ' ', false, nlohmann::ordered_json::error_handler_t::replace));

    // Resolve the ORIGINAL bind() promise with the final {blocks,...}
    // payload. webview's resolve() is documented safe from any thread (it
    // internally dispatches onto the UI thread's run loop, same as
    // postEvent — see shell.h's threading note); Shell exposes this via
    // postEvent for events, but the request/response promise itself is
    // resolved through the SAME impl_->view.resolve() shell.cpp's bind()
    // callback uses — routed here via a dedicated Shell::resolveInvoke()
    // (see shell.h/.cpp) so do/do_string can return asynchronously instead
    // of blocking the UI thread for a potentially long run.
    // BUG-1372: a string function works on bytes, as Stata's do, so a block
    // may carry a split UTF-8 sequence; it is sent as U+FFFD (what a terminal
    // shows for Stata's own log byte) instead of failing the run.
    shell->resolveInvoke(seq, true,
                         out.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace));
    // BUG-391: LAST statement — nothing above may run after this store, because
    // reapFinished() joins (and thereby lets AppState/Shell teardown proceed)
    // the moment it sees the flag.
    runFinished->store(true, std::memory_order_release);
  });
  app->registerThread(std::move(worker), runFinished);
}

}  // namespace

void bindAllRoutes(piet::Shell& shell, AppState& app) {
  shell.bindRoute("echo", piet::routes::echo);
  shell.bindRoute("cobra.version", piet::routes::cobraVersion);

  shell.bindRoute("command", [&app](const std::string& body) {
    return piet::routes::command(app.session, body);
  });
  // do / do_string: ASYNC routes — see Shell::bindAsyncRoute's doc comment.
  // The handler below returns immediately (BridgeResult is ignored by the
  // async dispatch path); the real response comes later via
  // shell->resolveInvoke() from the background worker thread.
  shell.bindAsyncRoute(
      "do", [&app](piet::Shell* shell, const std::string& seq, const std::string& body) {
        nlohmann::json req;
        try {
          req = nlohmann::json::parse(body);
        } catch (const nlohmann::json::exception&) {
          shell->resolveInvoke(seq, false, R"({"code":"bad_json","message":"malformed request body"})");
          return;
        }
        std::string filepath = req.value("filepath", std::string());
        std::vector<std::string> args;
        if (req.contains("args") && req["args"].is_array()) {
          for (const auto& a : req["args"]) args.push_back(a.get<std::string>());
        }
        runDoInBackground(shell, &app, seq, std::string(), args, filepath);
      });
  shell.bindAsyncRoute(
      "do_string", [&app](piet::Shell* shell, const std::string& seq, const std::string& body) {
        nlohmann::json req;
        try {
          req = nlohmann::json::parse(body);
        } catch (const nlohmann::json::exception&) {
          shell->resolveInvoke(seq, false, R"({"code":"bad_json","message":"malformed request body"})");
          return;
        }
        std::string source = req.value("source", std::string());
        std::vector<std::string> args;
        if (req.contains("args") && req["args"].is_array()) {
          for (const auto& a : req["args"]) args.push_back(a.get<std::string>());
        }
        runDoInBackground(shell, &app, seq, source, args, std::nullopt);
      });

  shell.bindRoute("run_progress", [&app](const std::string& body) {
    return piet::routes::runProgress(app.session, body);
  });
  shell.bindRoute("run_cancel", [&app](const std::string& body) {
    return piet::routes::runCancel(app.session, body);
  });
  // request.answer (D9, BETA Phase F — BUG-087 _request(), new contract_B
  // additive route): POST /api/request/answer -> routing key "request.answer"
  // (api.js's pathToRoutingKey: strip "/api/", "/" -> "."; matches the SAME
  // convention "dialog.open"/"label.variable" already use). Answers a
  // pending `request_dialog` push and wakes the do-executor worker thread
  // blocked in SessionState::requestUserInput(). The actual work (map write
  // + notify_all) is sub-millisecond — this uses the async-resolve pattern
  // (bindAsyncRoute + shell->resolveInvoke(), called inline before this
  // handler returns) to reuse the SAME infra do/do_string's async routes
  // already established, per this feature's own implementation brief;
  // behaviorally it is indistinguishable from a synchronous bindRoute.
  shell.bindAsyncRoute(
      "request.answer",
      [&app](piet::Shell* shell, const std::string& seq, const std::string& body) {
        nlohmann::json req;
        try {
          req = nlohmann::json::parse(body);
        } catch (const nlohmann::json::exception&) {
          shell->resolveInvoke(seq, false,
                                R"({"code":"bad_json","message":"malformed request body"})");
          return;
        }
        std::string requestId = req.value("requestId", std::string());
        std::string value = req.value("value", std::string());
        app.session.resolveUserInput(requestId, value);
        shell->resolveInvoke(seq, true, R"({"ok":true})");
      });
  shell.bindRoute("open", [&app](const std::string& body) { return piet::routes::open(app.session, body); });
  // dialog.open (2.0.1): native Finder file picker (NSOpenPanel). body:
  // {"exts":"dta,csv,hbin"} (optional filter). Returns {"path":"<abs>"} or
  // {"cancelled":true}. Backs the frontend "Open data" / "Run .do" buttons,
  // which previously fell back to a window.prompt that WKWebView doesn't
  // implement (dead no-op). Runs the panel on the UI thread (synchronous
  // route), then the frontend calls the existing `open`/`do` route with the
  // returned path.
  shell.bindRoute("dialog.open", [&shell](const std::string& body) -> piet::BridgeResult {
    std::string exts;
    try {
      auto j = nlohmann::json::parse(body);
      if (j.is_object() && j.contains("exts") && j["exts"].is_string())
        exts = j["exts"].get<std::string>();
    } catch (const nlohmann::json::exception&) {
      // no/invalid body — treat as "no extension filter"
    }
    std::string path = shell.openFileDialog(exts);
    nlohmann::ordered_json out;
    if (path.empty())
      out["cancelled"] = true;
    else
      out["path"] = path;
    return piet::BridgeResult::success(out.dump());
  });
  // file.read (WS-F1 scope addendum, native-paths stabilization wave):
  // read-only .do source fetch backing the frontend's editor-first Run.do
  // flow. body: {"path":"<user-typed path>"}. Resolves via the SAME
  // piet::resolveUserPath() every other user-path-taking handler now uses
  // (item 2 of this wave) so a relative path follows the seeded
  // session.cwd exactly like `use`/`do` would. App-shell facility, not a
  // Boundary-B kernel endpoint — same classification family as
  // dialog.open/legal.notices (it has no directory-listing or write affordance,
  // and only reads session.cwd unless the caller asks for {follow_cwd:true}, the
  // app's "opened this do-file" gesture; the reply then names the new directory
  // in cwd_changed if it moved). The resolve+validate+read logic lives in piet::readDoFileForBridge() (just
  // above bindAllRoutes) so selftest.cpp can exercise it directly; this
  // lambda is thin JSON-in/JSON-out wiring, same shape as dialog.open.
  shell.bindRoute("file.read", [&app](const std::string& body) -> piet::BridgeResult {
    auto badRequest = [](const std::string& message) {
      nlohmann::ordered_json err;
      err["code"] = "bad_request";
      err["message"] = message;
      return piet::BridgeResult::failure(err.dump());
    };

    std::string rawPath;
    bool followCwd = false;
    try {
      auto j = nlohmann::json::parse(body);
      if (!j.is_object() || !j.contains("path") || !j["path"].is_string()) {
        return badRequest("file.read: body must be {\"path\": string}");
      }
      rawPath = j["path"].get<std::string>();
      followCwd = j.value("follow_cwd", false);
    } catch (const nlohmann::json::exception& e) {
      return badRequest(std::string("file.read: invalid JSON body: ") + e.what());
    }

    piet::FileReadResult r = piet::readDoFileForBridge(app.session, rawPath);
    if (!r.ok) return badRequest(r.errorMessage);

    nlohmann::ordered_json out;
    out["source"] = r.source;
    out["resolved_path"] = r.resolvedPath;
    if (followCwd) {
      const std::string moved = piet::followDoFileDirectory(app.session, r.resolvedPath);
      if (!moved.empty()) out["cwd_changed"] = moved;
    }
    // A do-file saved in Latin-1 still opens: its non-UTF-8 bytes show as
    // U+FFFD instead of failing the route (BUG-1131).
    return piet::BridgeResult::success(
        out.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace));
  });
  shell.bindRoute("varlist", [&app](const std::string& body) {
    return piet::routes::varlist(app.session, body);
  });
  shell.bindRoute("browse", [&app](const std::string& body) {
    return piet::routes::browse(app.session, body);
  });
  shell.bindRoute("history", [&app](const std::string& body) {
    return piet::routes::history(app.session, body);
  });
  shell.bindRoute("version", [&app](const std::string& body) {
    return piet::routes::version(app.session, body);
  });
  shell.bindRoute("rename", [&app](const std::string& body) {
    return piet::routes::rename(app.session, body);
  });
  shell.bindRoute("label.variable", [&app](const std::string& body) {
    return piet::routes::labelVariable(app.session, body);
  });
  shell.bindRoute("label.define", [&app](const std::string& body) {
    return piet::routes::labelDefine(app.session, body);
  });
  shell.bindRoute("label.values", [&app](const std::string& body) {
    return piet::routes::labelValues(app.session, body);
  });
  shell.bindRoute("sort", [&app](const std::string& body) {
    return piet::routes::sortRoute(app.session, body);
  });
  shell.bindRoute("recode", [&app](const std::string& body) {
    return piet::routes::recode(app.session, body);
  });
  shell.bindRoute("cell", [&app](const std::string& body) { return piet::routes::cell(app.session, body); });
  shell.bindRoute("tabulate", [&app](const std::string& body) {
    return piet::routes::tabulate(app.session, body);
  });
  shell.bindRoute("describe", [&app](const std::string& body) {
    return piet::routes::describe(app.session, body);
  });
  shell.bindRoute("summarize", [&app](const std::string& body) {
    return piet::routes::summarize(app.session, body);
  });
  // var_detail (2.4.0, N7): single-column stats for the Variables-panel
  // detail popover. GET /api/var_detail?name=X -> the JS shim reshapes the
  // query param into a {name} body (same convention as browse).
  shell.bindRoute("var_detail", [&app](const std::string& body) {
    return piet::routes::varDetail(app.session, body);
  });

  // file.save (2.4.0, N1): write an export to disk. body:
  // {suggested_name, kind, text?|content_b64?, path?}. An explicit `path`
  // writes there directly with NO dialog (the Settings default-export-folder
  // flow AND headless gates); no `path` pops a native NSSavePanel (the
  // interactive "Export…" flow). Response {ok:true, path} on success,
  // {ok:false, cancelled:true} if the panel was dismissed. A FAILED write is
  // an honest {type:"error"} (routes::fileSaveToPath), NEVER {ok:true}
  // (COMMS §4.8). Captures &shell for the save panel, same as dialog.open.
  shell.bindRoute("file.save", [&shell](const std::string& body) -> piet::BridgeResult {
    nlohmann::json req;
    try {
      req = nlohmann::json::parse(body);
    } catch (const nlohmann::json::exception& e) {
      nlohmann::ordered_json err;
      err["code"] = "bad_json";
      err["message"] = e.what();
      return piet::BridgeResult::failure(err.dump());
    }
    std::string suggested = req.value("suggested_name", std::string());
    std::string kind = req.value("kind", std::string());
    std::optional<std::string> text, contentB64;
    if (req.contains("text") && req["text"].is_string()) text = req["text"].get<std::string>();
    if (req.contains("content_b64") && req["content_b64"].is_string())
      contentB64 = req["content_b64"].get<std::string>();
    std::string path;
    if (req.contains("path") && req["path"].is_string()) path = req["path"].get<std::string>();
    if (path.empty()) {
      // No explicit destination -> native save panel on the UI thread (this
      // bind() callback runs on it; runModal spins a nested loop, like
      // dialog.open's openFileDialog). `kind` doubles as the extension filter.
      path = shell.saveFileDialog(suggested, kind);
      if (path.empty()) {
        nlohmann::ordered_json out;
        out["ok"] = false;
        out["cancelled"] = true;
        return piet::BridgeResult::success(out.dump());
      }
    }
    return piet::routes::fileSaveToPath(path, kind, text, contentB64);
  });

  // window.title (2.4.0, N2): POST /api/window/title {text} -> set the native
  // window title verbatim (frontend composes "PIET — <file>"). This bind()
  // callback runs on the UI thread, so setTitle (which calls the AppKit
  // set_title: the A16 site uses) is called directly on the main thread.
  shell.bindRoute("window.title", [&shell](const std::string& body) -> piet::BridgeResult {
    std::string text;
    try {
      auto j = nlohmann::json::parse(body);
      if (j.is_object() && j.contains("text") && j["text"].is_string())
        text = j["text"].get<std::string>();
    } catch (const nlohmann::json::exception& e) {
      nlohmann::ordered_json err;
      err["code"] = "bad_json";
      err["message"] = e.what();
      return piet::BridgeResult::failure(err.dump());
    }
    shell.setTitle(text);
    return piet::BridgeResult::success(R"({"ok":true})");
  });

  // legal.notices (WI-5): app-shell service route (no session), backing the
  // frontend's "About / Legal" acknowledgements panel — same classification
  // family as dialog.open (shell facility, not a Boundary-B kernel endpoint).
  shell.bindRoute("legal.notices", piet::routes::legalNotices);

  // settings.get / settings.set (2.2.0 item D): app-shell config routes (no
  // session) — same app-shell classification as legal.notices/dialog.open,
  // contract_B unchanged. Back the Settings panel's engine/scratch group.
  shell.bindRoute("settings.get", piet::routes::settingsGet);
  shell.bindRoute("settings.set", piet::routes::settingsSet);

  // M0 demo route, kept for the bridge self-test / --demo page.
  shell.bindRoute("progress.start", [&shell](const std::string&) -> piet::BridgeResult {
    std::thread([&shell]() {
      piet::runSyntheticProgress([&shell](const std::string& tickJson) { shell.postEvent(tickJson); });
    }).detach();
    return piet::BridgeResult::success(R"({"started":true})");
  });
}

}  // namespace app_entry_detail
