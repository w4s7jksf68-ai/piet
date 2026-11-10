// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// --ui-smoke journey driver script and the seeded-injection probe (injected JS).
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

std::string smokeDriverJs(const std::string& dataPath, const std::string& doFixturePath,
                          const std::string& saveTargetPath, bool occludeStep8) {
  // nlohmann dump() of a string produces a double-quoted, escaped literal —
  // valid as a JS string literal (JSON is a JS subset here).
  std::string dataLit = nlohmann::json(dataPath).dump();
  std::string doFixtureLit = nlohmann::json(doFixturePath).dump();
  std::string saveTargetLit = nlohmann::json(saveTargetPath).dump();
  std::string js = R"JS(
    (function () {
      var DATA_PATH = )JS";
  js += dataLit;
  js += R"JS(;
      var DO_FIXTURE_PATH = )JS";
  js += doFixtureLit;
  js += R"JS(;
      var SAVE_TARGET_PATH = )JS";
  js += saveTargetLit;
  js += R"JS(;
      var OCCLUDE_STEP8 = )JS";
  js += occludeStep8 ? "true" : "false";
  js += R"JS(;
      var STEP_TIMEOUT_MS = 15000;
      var t0 = Date.now();
      // BUG-197 hardening: the boot census below (report(true, false,
      // 'boot_prereq_dom_census', ...)) and the journey's own top-level
      // .catch() can both try to send a final report for the SAME run (the
      // census throws to unwind the chain after already reporting) — without
      // this guard the second, generic 'journey' report would overwrite the
      // C++ side's smoke.finalPass with a redundant duplicate. Only the
      // FIRST final report counts; every later one is a silent no-op.
      var finalReported = false;

      function report(isFinal, pass, step, detail) {
        if (isFinal) {
          if (finalReported) return;
          finalReported = true;
        }
        var payload = { final: isFinal, pass: pass, step: step,
                        detail: detail || '', elapsed_ms: Date.now() - t0 };
        try {
          // The invoke leg delivers to C++ even if the resolve leg is broken
          // (see the C++ comment above runUiSmoke) — swallow the response.
          window.pietNative.request('smoke.report', payload).catch(function () {});
        } catch (e) { /* bridge fully absent — the C++ watchdog fires */ }
      }

      // --- BUG-197: boot-time injected-content census -----------------------
      // Runs before step 1, on the theory that a gate whose FIRST recorded
      // step already inspects its own execution environment can tell a future
      // reader which regime judged the run (see the mode line in the FINAL
      // report below). Two checks, both boot-time DOM snapshots — this is
      // deliberately NOT a live MutationObserver: the goal is "did anything
      // land in this document before we started", not "watch it forever",
      // matching the wave brief's "enumerate injected content at boot".
      //
      // 1. Script census: piet/frontend/index.html ships exactly two inline
      //    <script> elements (the pre-paint mode/theme FOUC guard, and the
      //    type="module" boot script) and ships them with NO `src` attribute
      //    — everything else the app loads (js/app.js etc.) arrives via
      //    `import`, which does not create a `<script>` DOM node. The C++
      //    side's own shims (installBridge, this very driver) are injected as
      //    WKUserScripts, which run in the page's JS context WITHOUT ever
      //    becoming document.scripts entries — so this census cannot see (or
      //    false-positive on) its own harness. Any script that IS a DOM node
      //    beyond those two, or that carries a `src`, is content this driver
      //    did not ship.
      // 2. Grammarly marker census: the internal bug ledger BUG-197 /
      //    piet/README.md's "Known issues" section confirm Grammarly Desktop
      //    as the one injector this fleet has actually caught mutating this
      //    DOM. Its confirmed markers (data-gramm*, the desktop-integration
      //    host element, its shadow-root host attribute) are checked
      //    explicitly as a named tripwire, separate from the generic script
      //    census, because they land on EXISTING elements (textarea/input)
      //    rather than as new <script> nodes — the two checks catch different
      //    injection shapes and neither subsumes the other.
      var EXPECTED_SHIPPED_SCRIPT_COUNT = 2;
      var GRAMMARLY_MARKER_SELECTORS = [
        '[data-gramm]', '[data-gramm_editor]', '[data-enable-grammarly]',
        '#grammarly-desktop-integration', '[data-grammarly-shadow-root]'
      ];
      function domCensus() {
        var findings = [];
        var scripts = document.querySelectorAll('script');
        for (var i = 0; i < scripts.length; i++) {
          if (scripts[i].src) {
            findings.push('foreign <script src="' + scripts[i].src + '">');
          }
        }
        if (scripts.length > EXPECTED_SHIPPED_SCRIPT_COUNT) {
          findings.push('script element count ' + scripts.length + ' > shipped ' +
                        EXPECTED_SHIPPED_SCRIPT_COUNT);
        }
        GRAMMARLY_MARKER_SELECTORS.forEach(function (sel) {
          try {
            if (document.querySelector(sel)) findings.push('grammarly marker present: ' + sel);
          } catch (e) { /* invalid selector on an old WebKit — ignore that one check */ }
        });
        return findings;
      }

      function waitFor(desc, pred, timeoutMs) {
        return new Promise(function (resolve, reject) {
          var deadline = Date.now() + timeoutMs;
          (function poll() {
            var v = null;
            try { v = pred(); } catch (e) { /* keep polling */ }
            if (v) return resolve(v);
            if (Date.now() > deadline) return reject(new Error('timeout waiting for ' + desc));
            setTimeout(poll, 100);
          })();
        });
      }

      // s5's negative assertion ("no do_string executed"): poll a settle
      // window watching for a NEW console block rather than a single
      // sleep-then-check — a routing regression that auto-runs the pasted
      // block would append one almost immediately, so this fails fast
      // instead of only at the deadline.
      function waitNoNewBlock(desc, baselineCount, windowMs) {
        return new Promise(function (resolve, reject) {
          var deadline = Date.now() + windowMs;
          (function poll() {
            var n = document.querySelectorAll('#console-body .block').length;
            if (n > baselineCount) {
              return reject(new Error('unexpected new console block appeared while waiting for ' +
                                       desc + ' (count ' + baselineCount + ' -> ' + n + ')'));
            }
            if (Date.now() > deadline) return resolve();
            setTimeout(poll, 100);
          })();
        });
      }

      function submit(cmd) {
        var input = document.getElementById('cmd-input');
        var btn = document.getElementById('btn-run');
        if (!input || !btn) throw new Error('cmd-input / btn-run not in DOM');
        input.value = cmd;
        btn.click();
      }

      function domReady() {
        return new Promise(function (resolve) {
          if (document.readyState !== 'loading') return resolve();
          document.addEventListener('DOMContentLoaded', function () { resolve(); });
        });
      }

      // Console overlays: #browser-overlay is the sole exception that uses
      // the `hidden` attribute instead of `.open` (grid.js owns it — see
      // index.html's comment on #browser-overlay); the other three
      // (doedit/graphics/about) use `.open` via app.js's OVERLAYS registry.
      var OVERLAY_IDS = ['browser-overlay', 'doedit-overlay', 'graphics-overlay', 'about-overlay'];
      function overlayVisible(id) {
        var el = document.getElementById(id);
        if (!el) return false;
        return id === 'browser-overlay' ? !el.hidden : el.classList.contains('open');
      }
      function countVisibleOverlays() {
        return OVERLAY_IDS.filter(overlayVisible).length;
      }

      domReady().then(function () {
        // 0. boot-time injected-content census (BUG-197). Runs BEFORE step 1
        // on purpose — a page that already has foreign content on it should
        // never get to claim a bridge_echo pass, since nothing downstream can
        // be trusted once this fails. Reports its own final verdict directly
        // (report()'s finalReported guard makes the generic top-level .catch()
        // below a no-op for this case) so the FAIL is attributed to
        // 'boot_prereq_dom_census', not the generic 'journey' bucket.
        var findings = domCensus();
        if (findings.length) {
          var detail = 'PANGRAM_PREREQ: injected content detected at boot — ' +
                       findings.join('; ');
          report(true, false, 'boot_prereq_dom_census', detail);
          throw new Error(detail);
        }
        report(false, true, 'boot_prereq_dom_census');
      }).then(function () {
        // 1. bridge alive: direct echo round-trip through the real shim.
        return window.pietNative.request('echo', { probe: 'ui-smoke', n: 42 }).then(function (r) {
          if (!r || !r.echoed || r.echoed.n !== 42) {
            throw new Error('echo payload mismatch: ' + JSON.stringify(r));
          }
          report(false, true, 'bridge_echo');
        });
      }).then(function () {
        // 2. kernel dot green — the SPA's own boot + ping succeeded.
        return waitFor('kernel dot green (#kernel-dot .dot-ok)', function () {
          var dot = document.getElementById('kernel-dot');
          return dot && dot.classList.contains('dot-ok');
        }, STEP_TIMEOUT_MS).then(function () {
          var banner = document.getElementById('mock-banner');
          if (banner && !banner.hidden) {
            throw new Error('mock banner visible — SPA fell back to mock mode');
          }
          report(false, true, 'kernel_dot_green');
        });
      }).then(function () {
        // 3. open dataset via the real console `use` path; varlist populates.
        submit('use "' + DATA_PATH + '"');
        return waitFor('varlist rows (#var-list-wrap .var-row)', function () {
          return document.querySelectorAll('#var-list-wrap .var-row').length > 0;
        }, STEP_TIMEOUT_MS).then(function () {
          var n = document.querySelectorAll('#var-list-wrap .var-row').length;
          report(false, true, 'varlist_populated', n + ' variable rows');
        }).then(function () {
          // 3b. STATUSBAR HONESTY (P8 "Honest Cockpit", U1 gate-blindness
          // close): a painted-but-empty statusbar that never actually named
          // the loaded file was invisible to this gate before (varlist's
          // source_file / open's filepath were wired nowhere). Only the
          // FILENAME (DATA_PATH's basename) need appear as a substring — the
          // bar may render the full resolved path.
          var fname = DATA_PATH.split('/').pop();
          return waitFor('#statusbar text contains loaded filename (' + fname + ')', function () {
            var bar = document.getElementById('statusbar');
            return !!(bar && bar.textContent && bar.textContent.indexOf(fname) !== -1);
          }, STEP_TIMEOUT_MS).then(function () {
            var bar = document.getElementById('statusbar');
            report(false, true, 'statusbar_honest', 'statusbar: ' + JSON.stringify(bar.textContent.slice(0, 160)));
          });
        });
      }).then(function () {
        // 4. run one estimation; a rendered results block must appear.
        submit('regress fatal beertax unemp');
        return waitFor('rendered estimation block (.block-estimation .coeftable)', function () {
          var el = document.querySelector('#console-body .block-estimation .coeftable');
          return (el && el.textContent && el.textContent.length > 0) ? el : null;
        }, STEP_TIMEOUT_MS).then(function (el) {
          var txt = el.textContent;
          if (txt.indexOf('beertax') === -1) {
            throw new Error('coeftable does not mention regressor beertax: ' + txt.slice(0, 120));
          }
          var errs = document.querySelectorAll('#console-body .block-error');
          if (errs.length > 0) {
            throw new Error('console has ' + errs.length + ' error block(s): ' +
                            (errs[errs.length - 1].textContent || '').slice(0, 200));
          }
          report(false, true, 'estimation_rendered', txt.slice(0, 120));
        });
      }).then(function () {
        // 5. PASTE-ROUTING (design decision 4): a multi-line paste on
        // #cmd-input, with piet-paste-autorun OFF (the default), must load
        // into the do-editor and must NOT auto-run.
        localStorage.setItem('piet-paste-autorun', '0');
        // Deterministic UI state (gate self-isolation): WKWebView persists
        // localStorage by bundle id ACROSS runs, so a prior run that engaged
        // split-view — or died before step 12's Reset cleared it — leaves THIS
        // run in split-view, where openDoEditor() docks the editor as a column
        // instead of adding `.open`, breaking the editor-first step below.
        // Force single-view + drop the persisted key so the journey is
        // independent of any leaked prefs.
        (function () {
          var st = document.getElementById('split-toggle');
          if (st && st.getAttribute('aria-pressed') === 'true') st.click();
          try { localStorage.removeItem('piet-split'); } catch (e) {}
        })();
        var input = document.getElementById('cmd-input');
        if (!input) throw new Error('#cmd-input not in DOM');
        var pasteLines = 'summarize fatal\nsummarize beertax';
        var baselineBlocks = document.querySelectorAll('#console-body .block').length;

        // Feasibility probe: WKWebView's ClipboardEvent/DataTransfer
        // constructors are exercised first; if they don't round-trip (some
        // WebKit builds restrict DataTransfer construction/setData outside
        // a real user gesture), fall back to a duck-typed plain Event
        // carrying a `clipboardData.getData()` shim — onInputPaste() only
        // calls e.clipboardData?.getData('text') and e.preventDefault(),
        // both satisfied structurally by the fallback, and addEventListener
        // dispatches on event TYPE regardless of the concrete constructor.
        var pasteEvent;
        var usedRealClipboardEvent = true;
        var ceFeasibilityDetail;
        try {
          var dt = new DataTransfer();
          dt.setData('text/plain', pasteLines);
          pasteEvent = new ClipboardEvent('paste', { clipboardData: dt, cancelable: true, bubbles: true });
          if (!pasteEvent.clipboardData || pasteEvent.clipboardData.getData('text') !== pasteLines) {
            throw new Error('constructed ClipboardEvent.clipboardData did not round-trip setData/getData');
          }
          ceFeasibilityDetail = 'real ClipboardEvent + DataTransfer constructible and round-tripped in this WKWebView';
        } catch (ceErr) {
          usedRealClipboardEvent = false;
          ceFeasibilityDetail = 'ClipboardEvent/DataTransfer path failed (' + String(ceErr.message || ceErr) +
                                ') — used a duck-typed plain Event with a clipboardData.getData() shim instead';
          pasteEvent = new Event('paste', { cancelable: true, bubbles: true });
          pasteEvent.clipboardData = { getData: function () { return pasteLines; } };
        }
        report(false, true, 'paste_routing_ce_feasibility', ceFeasibilityDetail);

        input.value = 'text-that-must-be-cleared-by-paste-handler';
        input.dispatchEvent(pasteEvent);

        return waitFor('do-editor opens after multi-line paste (#doedit-overlay.open)', function () {
          return document.getElementById('doedit-overlay')?.classList.contains('open');
        }, STEP_TIMEOUT_MS).then(function () {
          var ta = document.getElementById('doedit-text');
          if (!ta || ta.value.indexOf('summarize fatal') === -1 || ta.value.indexOf('summarize beertax') === -1) {
            throw new Error('#doedit-text does not contain the pasted lines verbatim: ' +
                            JSON.stringify(ta && ta.value));
          }
          if (input.value !== '') {
            throw new Error('#cmd-input was not cleared by the paste handler: ' + JSON.stringify(input.value));
          }
          return waitNoNewBlock('paste-routing settle window', baselineBlocks, 1200);
        }).then(function () {
          document.getElementById('doedit-close')?.click();
          report(false, true, 'paste_routing',
                 (usedRealClipboardEvent ? '(real ClipboardEvent) ' : '(fallback event) ') +
                 'routed to do-editor, no auto-run');
        });
      }).then(function () {
        // 6. EDITOR-FIRST RUN: window.pietNative.request('file.read', ...)
        // backs the Run.do editor-first hop (design decision 3); onRunDo()
        // itself is unreachable headlessly (native Finder picker), so this
        // drives file.read directly, then reproduces onRunDo()'s
        // externally-observable effect (fill #doedit-text, foreground the
        // editor via the SAME [data-action="doedit"] entry point
        // openDoEditor() is bound to), then runs it.
        return window.pietNative.request('file.read', { path: DO_FIXTURE_PATH }).then(function (r) {
          if (!r || typeof r.source !== 'string' || r.source.indexOf('describe') === -1) {
            throw new Error('file.read source missing/unexpected: ' + JSON.stringify(r));
          }
          if (!r.resolved_path || r.resolved_path.indexOf('ui_smoke_fixture.do') === -1) {
            throw new Error('file.read resolved_path unexpected: ' + JSON.stringify(r));
          }
          report(false, true, 'file_read_bridge', r.resolved_path);

          var ta = document.getElementById('doedit-text');
          if (!ta) throw new Error('#doedit-text not in DOM');
          ta.value = r.source;
          var doeditAnchor = document.querySelector('[data-action="doedit"]');
          if (!doeditAnchor) throw new Error('[data-action="doedit"] not in DOM');
          doeditAnchor.click();

          return waitFor('doedit overlay open after editor-first load', function () {
            return document.getElementById('doedit-overlay')?.classList.contains('open');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          var runBtn = document.getElementById('doedit-run');
          if (!runBtn) throw new Error('#doedit-run not in DOM');
          runBtn.click();
          // Wait for the TERMINAL state directly (not just "any new block") —
          // runDoEditor() synchronously appends an immediate 'do (editor)'
          // echo block BEFORE the async client.runDoString(source) call
          // resolves, so a naive "block count grew" predicate would resolve
          // on that echo and race ahead of the actual describe result.
          return waitFor('describe result or error block renders after doedit-run', function () {
            return document.querySelector('#console-body .block-table, #console-body .block-error');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          var errs = document.querySelectorAll('#console-body .block-error');
          if (errs.length > 0) {
            throw new Error('console has error block(s) after editor-first run: ' +
                            (errs[errs.length - 1].textContent || '').slice(0, 200));
          }
          var tableEl = document.querySelector('#console-body .block-table');
          if (!tableEl) {
            throw new Error('no .block-table (describe) rendered after the do-editor run — ' +
                            'honest run semantics violated');
          }
          report(false, true, 'editor_first_run', 'file.read -> editor -> doedit-run -> describe block rendered');
        });
      }).then(function () {
        // 6b. OPENING A DO-FILE MOVES THE WORKING DIRECTORY TO ITS FOLDER, through
        // the real product path: a file_dropped push -> routeDroppedFile ->
        // onOpenDoFile. The folder name is Hangul. It must NOT move with the
        // setting off, after a `cd`, or for a typed `do FILE`.
        var pwd = function () {
          return window.pietNative.request('command', { cmd: 'display c(pwd)' }).then(function (r) {
            var b = ((r && r.blocks) || []).find(function (x) { return x && typeof x.text === 'string' && x.type !== 'echo'; });
            if (!b) throw new Error('display c(pwd) printed nothing: ' + JSON.stringify(r));
            return b.text.trim();
          });
        };
        var folder = 'do 폴더', before;
        var endsInFolder = function (p) { return String(p).replace(/[\\/]+$/, '').slice(-folder.length) === folder; };
        var moveNotes = function () {
          return (document.getElementById('console-body')?.textContent || '').split('Working directory is now').length - 1;
        };
        // One drop, finished when the editor holds the file again. The wait first:
        // the previous step releases the busy lock just after its work completes.
        var dropDoFile = function () {
          document.getElementById('doedit-text').value = '';
          return new Promise(function (resolve) { setTimeout(resolve, 500); }).then(function () {
            return window.pietNative.request('smoke.drop_do_file', {});
          }).then(function () {
            return waitFor('dropped do-file loaded into the editor', function () {
              return document.getElementById('doedit-text').value.indexOf('ui smoke sibling') >= 0;
            }, STEP_TIMEOUT_MS);
          });
        };
        return pwd().then(function (p) {
          before = p;
          if (endsInFolder(before)) throw new Error('fixture error: already in the do-file folder');
          return window.pietNative.request('settings.set', { follow_do_file_folder: false });
        }).then(dropDoFile).then(pwd).then(function (p) {
          if (p !== before || moveNotes() !== 0) throw new Error('the setting is off, but opening moved to ' + p);
          return window.pietNative.request('settings.set', { follow_do_file_folder: true });
        }).then(dropDoFile).then(function () {
          if (moveNotes() !== 1 || (document.getElementById('console-body')?.textContent || '').indexOf(folder) < 0) {
            throw new Error('the console did not name the new working directory');
          }
        }).then(pwd).then(function (p) {
          if (!endsInFolder(p)) throw new Error('c(pwd) did not follow the do-file: ' + p);
          // A relative path now resolves beside the do-file, the reported problem.
          return window.pietNative.request('file.read', { path: 'smoke_sibling.do' });
        }).then(function (r) {
          if (!r || r.source.indexOf('ui smoke sibling') < 0) throw new Error('relative path missed the sibling');
          // Opening it again from its own folder moves nothing and says nothing.
          return window.pietNative.request('file.read', { path: r.resolved_path, follow_cwd: true }).then(function (again) {
            if (again.cwd_changed) throw new Error('reopened in place but reported a move: ' + again.cwd_changed);
            return r.resolved_path;
          });
        }).then(function (siblingPath) {
          // A directory the user chose outranks the do-file's folder.
          return window.pietNative.request('command', { cmd: 'cd "' + before + '"' }).then(dropDoFile).then(pwd).then(function (p) {
            if (p !== before || moveNotes() !== 1) throw new Error('opening overrode the user\'s cd, now at ' + p);
            document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
            return window.pietNative.request('command', { cmd: 'do "' + siblingPath + '"' });
          });
        }).then(pwd).then(function (p) {
          if (p !== before) throw new Error('a typed do FILE moved the working directory to ' + p);
          report(false, true, 'do_file_folder_cwd', 'setting off stays; on follows and relative path resolves; a cd stands; typed do FILE stays');
        });
      }).then(function () {
        // 7. OVERLAY EXCLUSIVITY (design decision 5 / A2-05): opening one
        // console-overlay via its toolbar anchor must close any other.
        var graphicsAnchor = document.querySelector('[data-action="graphics"]');
        var doeditAnchor = document.querySelector('[data-action="doedit"]');
        if (!graphicsAnchor || !doeditAnchor) throw new Error('toolbar overlay anchors not in DOM');

        graphicsAnchor.click();
        return waitFor('graphics overlay open', function () {
          return overlayVisible('graphics-overlay');
        }, STEP_TIMEOUT_MS).then(function () {
          var n = countVisibleOverlays();
          if (n !== 1) throw new Error('expected exactly 1 overlay open after graphics click, got ' + n);
          doeditAnchor.click();
          return waitFor('doedit overlay open (graphics must close)', function () {
            return overlayVisible('doedit-overlay');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          if (overlayVisible('graphics-overlay')) {
            throw new Error('graphics-overlay still visible after opening doedit-overlay — exclusivity violated');
          }
          var n = countVisibleOverlays();
          if (n !== 1) throw new Error('expected exactly 1 overlay open after doedit click, got ' + n);
          document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
          return waitFor('all overlays closed after Escape', function () {
            return countVisibleOverlays() === 0;
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          report(false, true, 'overlay_exclusivity', 'graphics -> doedit -> Escape; at most 1 open at each point');
        });
      }).then(function () {
        // 8. TRANSITION SANITY: opening an overlay must reach a REAL
        // rendered end-state, not just a class flip with no visible effect.
        // Polled — no fixed sleep-then-check.
        //
        // Occlusion hardening (2026-07-09): WKWebView suspends the CSS
        // transition/animation clock while the window is occluded (covered by
        // another app, locked screen) — JS timers keep firing but a 200ms
        // opacity transition never advances, so asserting the animated
        // computed style produced a false RED on a covered window with the
        // timeout message below (observed 2026-07-09; immediate re-run
        // GREEN). Forcing the window frontmost can't close that hole
        // (unattended/locked-screen runs stay occluded), so the step instead
        // runs under the frontend's OWN reduced-motion configuration:
        // piet.css §1 zeroes --dur-fast/--dur-med for
        // prefers-reduced-motion, and the same inline override makes the
        // .open class flip update computed style DISCRETELY — independent of
        // the animation clock. The assertion is unchanged (getComputedStyle
        // opacity '1' + visibility 'visible'), so a class flip whose CSS
        // stops producing a visible end-state still FAILS; only the
        // animation-clock dependency is gone. Vars are restored afterwards.
        var motionVars = document.documentElement.style;
        motionVars.setProperty('--dur-fast', '0ms');
        motionVars.setProperty('--dur-med', '0ms');
        var occludePre = OCCLUDE_STEP8
            ? waitFor('window occluded before transition_sanity (document.hidden; cover the ' +
                      'window — PIET_UI_SMOKE_OCCLUDE_STEP8 is set)', function () {
                return document.hidden === true;
              }, 30000)
            : Promise.resolve();
        return occludePre.then(function () {
          var graphicsAnchor = document.querySelector('[data-action="graphics"]');
          if (!graphicsAnchor) throw new Error('[data-action="graphics"] not in DOM');
          graphicsAnchor.click();
          return waitFor('graphics-overlay opacity settles to 1 + visibility visible', function () {
            var el = document.getElementById('graphics-overlay');
            if (!el) return null;
            var cs = getComputedStyle(el);
            return (cs.opacity === '1' && cs.visibility === 'visible') ? el : null;
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          report(false, true, 'transition_sanity',
                 'graphics-overlay reached opacity:1 + visibility:visible ' +
                 '(reduced-motion vars, visibilityState=' + document.visibilityState + ')');
          // UX-047 removed #graphics-close; Escape goes through the top-level
          // overlay-registry handler, which exists on both frontend versions
          // (same idiom as the overlay-exclusivity step above).
          document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
          return waitFor('graphics-overlay closed before the next step', function () {
            return !document.getElementById('graphics-overlay')?.classList.contains('open');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          motionVars.removeProperty('--dur-fast');
          motionVars.removeProperty('--dur-med');
        });
      }).then(function () {
        // 9. BUSY-LOCK (A1-01/A1-02 regression): setBusy(true) runs
        // synchronously inside runCommand() before its first await, so the
        // lock must already be visible the instant btn.click() returns —
        // asserted here with NO await/setTimeout in between.
        var input = document.getElementById('cmd-input');
        var btn = document.getElementById('btn-run');
        var openLink = document.querySelector('[data-action="open"]');
        var varWrap = document.getElementById('var-list-wrap');
        if (!input || !btn || !openLink || !varWrap) throw new Error('busy-lock targets not in DOM');

        input.value = 'describe';
        btn.click();

        if (!openLink.classList.contains('busy') || openLink.getAttribute('aria-disabled') !== 'true') {
          throw new Error('[data-action="open"] did not carry the busy lock synchronously after btn-run click');
        }
        if (!varWrap.classList.contains('busy')) {
          throw new Error('#var-list-wrap did not carry the busy lock synchronously after btn-run click');
        }

        return waitFor('busy lock releases after the run settles', function () {
          return !openLink.classList.contains('busy') && !varWrap.classList.contains('busy');
        }, STEP_TIMEOUT_MS).then(function () {
          if (openLink.getAttribute('aria-disabled') !== 'false') {
            throw new Error('[data-action="open"] aria-disabled not restored to false after busy release');
          }
          report(false, true, 'busy_lock', 'busy classes set synchronously on click, released after settle');
        });
      }).then(function () {
        // 10. TOOLBAR GROUP ORDER (2.2.0 item A, USER-corrected swap): the view
        // tabs sit LEFT of the structural divider, the actions group RIGHT.
        var views = document.querySelector('.toolbar-views');
        var divider = document.querySelector('.toolbar-group-divider');
        var actions = document.querySelector('.toolbar-actions');
        if (!views || !divider || !actions) throw new Error('toolbar groups (.toolbar-views/.toolbar-group-divider/.toolbar-actions) not all in DOM');
        var FOLLOWING = Node.DOCUMENT_POSITION_FOLLOWING;
        var viewsBeforeDivider = (views.compareDocumentPosition(divider) & FOLLOWING) !== 0;
        var dividerBeforeActions = (divider.compareDocumentPosition(actions) & FOLLOWING) !== 0;
        if (!viewsBeforeDivider || !dividerBeforeActions) {
          throw new Error('toolbar order wrong: expected views | divider | actions (item A swap)');
        }
        var tabs = Array.prototype.map.call(document.querySelectorAll('.toolbar-views .view-tab'),
          function (t) { return t.getAttribute('data-view'); }).join(',');
        if (tabs !== 'results,doedit,graphics') {
          throw new Error('view-tab internal order changed: ' + tabs);
        }
        report(false, true, 'toolbar_group_order', 'views | divider | actions; tabs ' + tabs);
      }).then(function () {
        // 11. RUN-SELECTION (2.2.0 item B): a drag-selected range in the
        // do-editor, run via the Stata Do-button binding (⇧⌘D), executes ONLY
        // that range (same do_string path Run .do uses).
        document.querySelector('[data-action="doedit"]').click();
        return waitFor('doedit open for run-selection', function () {
          return document.getElementById('doedit-overlay')?.classList.contains('open');
        }, STEP_TIMEOUT_MS).then(function () {
          var ta = document.getElementById('doedit-text');
          ta.value = 'display "RUNSEL_A"\ndisplay "RUNSEL_B"\ndisplay "RUNSEL_C"';
          var start = ta.value.indexOf('display "RUNSEL_B"');
          var end = start + 'display "RUNSEL_B"'.length;
          ta.focus();
          ta.setSelectionRange(start, end);
          var baseline = document.querySelectorAll('#console-body .block').length;
          ta.dispatchEvent(new KeyboardEvent('keydown',
            { key: 'd', metaKey: true, ctrlKey: true, shiftKey: true, bubbles: true, cancelable: true }));
          return waitFor('run-selection produced a new console block', function () {
            return document.querySelectorAll('#console-body .block').length > baseline;
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          // Let any trailing display block land, then assert isolation.
          return waitFor('selected line RUNSEL_B rendered', function () {
            return (document.getElementById('console-body').textContent || '').indexOf('RUNSEL_B') !== -1;
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          var consoleTxt = document.getElementById('console-body').textContent || '';
          if (consoleTxt.indexOf('RUNSEL_A') !== -1 || consoleTxt.indexOf('RUNSEL_C') !== -1) {
            throw new Error('run-selection ran unselected lines (RUNSEL_A/RUNSEL_C leaked into console)');
          }
          report(false, true, 'run_selection', 'selected middle line ran alone (RUNSEL_B only)');
        });
      }).then(function () {
        // 12. SPLIT + RESET (2.2.0 item C): split -> dock a surface column ->
        // one-click Reset restores the default single-view layout.
        var splitToggle = document.getElementById('split-toggle');
        var resetBtn = document.getElementById('reset-layout');
        var pane = document.getElementById('pane-console');
        if (!splitToggle || !resetBtn || !pane) throw new Error('split controls not in DOM');
        splitToggle.click();
        return waitFor('split mode engaged', function () {
          return pane.classList.contains('split') &&
                 splitToggle.getAttribute('aria-pressed') === 'true' && !resetBtn.hidden;
        }, STEP_TIMEOUT_MS).then(function () {
          document.querySelector('[data-action="doedit"]').click();
          return waitFor('do-editor docked as a side-by-side column', function () {
            var ov = document.getElementById('doedit-overlay');
            return ov && ov.classList.contains('docked') && ov.classList.contains('open');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          resetBtn.click();
          return waitFor('Reset layout restored single-view', function () {
            var ov = document.getElementById('doedit-overlay');
            return !pane.classList.contains('split') && resetBtn.hidden &&
                   ov && !ov.classList.contains('docked') && !ov.classList.contains('open');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          if (localStorage.getItem('piet-split') !== null) {
            throw new Error('Reset layout did not clear the persisted piet-split key');
          }
          report(false, true, 'split_reset', 'split -> dock editor column -> Reset -> single-view + persistence cleared');
        });
      }).then(function () {
        // 13. SETTINGS open -> change -> apply (2.2.0 item D): the panel loads
        // detected system info; a display change (font size) applies live; an
        // engine change persists via settings.set (isolated store).
        document.querySelector('[data-action="settings"]').click();
        return waitFor('settings overlay open + populated', function () {
          var ov = document.getElementById('settings-overlay');
          var sysinfo = document.getElementById('settings-sysinfo');
          var cores = document.getElementById('set-cores');
          return ov && ov.classList.contains('open') &&
                 sysinfo && sysinfo.textContent.indexOf('cores') !== -1 &&
                 cores && cores.options.length > 1;
        }, STEP_TIMEOUT_MS).then(function () {
          var fsSel = document.getElementById('set-fontsize');
          fsSel.value = 'large';
          fsSel.dispatchEvent(new Event('change', { bubbles: true }));
          if (document.documentElement.dataset.fontsize !== 'large') {
            throw new Error('font-size change did not apply live');
          }
          var mem = document.getElementById('set-mem-budget');
          mem.value = String(Number(mem.min) + 128);
          document.getElementById('settings-apply').click();
          return waitFor('settings apply confirmed (settings.set round-trip)', function () {
            var st = document.getElementById('settings-status');
            return st && st.textContent.indexOf('Saved') !== -1;
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          // cleanup: restore medium font, close the panel.
          var fsSel = document.getElementById('set-fontsize');
          fsSel.value = 'medium';
          fsSel.dispatchEvent(new Event('change', { bubbles: true }));
          document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
          report(false, true, 'settings_apply', 'open -> font-size live -> memory-budget Apply -> Saved');
        });
      }).then(function () {
        // 14. PNG-EXPORT PAINT CHECK (P8 "Honest Cockpit", U2 regression gate;
        // BUG-084 was "PNG download is all-black — flatten CSS before
        // rasterizing"). This harness does not know the frontend's private
        // "⬇ PNG"-button DOM wiring or exact svgToPng() call shape (out of
        // this file's owned scope — piet/frontend/** belongs to a separate
        // agent), so a real canvas/Image decode of the exported download was
        // impractical to drive reliably here. Using the SANCTIONED fallback
        // instead: render a graph and assert its SVG marks carry INLINE
        // paint (a fill/stroke ATTRIBUTE or inline `style`), not paint that
        // lives ONLY in an external CSS class — exactly the condition the
        // BUG-084 fix restored. A canvas rasterizer given a serialized SVG
        // never resolves an external stylesheet class (no cascade context
        // outside the live DOM), so a mark painted only via a CSS class
        // rasterizes as black/transparent; inline paint survives
        // serialization and rasterizes correctly. This is a proxy for "the
        // exported PNG is not all-black", not a pixel decode.
        var svgBaseline = document.querySelectorAll('#console-body svg').length;
        submit('scatter fatal beertax');
        return waitFor('a new graph SVG renders in #console-body', function () {
          var svgs = document.querySelectorAll('#console-body svg');
          return svgs.length > svgBaseline ? svgs[svgs.length - 1] : null;
        }, STEP_TIMEOUT_MS).then(function (svg) {
          var marks = svg.querySelectorAll('circle, rect, path, line, polyline, polygon');
          if (marks.length === 0) {
            throw new Error('graph SVG rendered with no mark elements ' +
                            '(circle/rect/path/line/polyline/polygon)');
          }
          var NEUTRAL = { '': 1, 'none': 1, 'transparent': 1, 'black': 1,
                          '#000': 1, '#000000': 1, 'rgb(0,0,0)': 1, 'rgba(0,0,0,0)': 1 };
          function paints(v) {
            if (!v) return false;
            return !NEUTRAL[v.replace(/\s+/g, '').toLowerCase()];
          }
          // svgToPng() (app.js) flattens paint by reading getComputedStyle on
          // the LIVE marks and inlining it onto the export clone. The live SVG
          // paints via CSS classes (.g-point/.g-line/…) by design, so checking
          // the live node for INLINE paint is the WRONG artifact (absent on
          // purpose). The correct proxy for "the exported PNG is not all-black"
          // is that the marks RESOLVE (via the cascade) to real, non-black
          // paint — exactly the value svgToPng captures and inlines.
          var hasResolvedPaint = Array.prototype.some.call(marks, function (el) {
            var cs = getComputedStyle(el);
            return paints(cs.fill) || paints(cs.stroke);
          });
          if (!hasResolvedPaint) {
            throw new Error('graph SVG marks resolve to no visible fill/stroke paint (getComputedStyle) — ' +
                            'svgToPng would flatten black and the exported PNG would be all-black (BUG-084)');
          }
          report(false, true, 'png_export_paint_check',
                 marks.length + ' mark element(s) resolve to real paint via getComputedStyle (the value ' +
                 'svgToPng inlines onto its export clone — proxy for a non-black PNG rasterization; a real ' +
                 'canvas getImageData decode was impractical from this headless harness)');
        });
      }).then(function () {
        // 16. REQUEST-DIALOG JOURNEY (D9, BETA Phase F — BUG-087 native
        // _request()): a do-string containing `_request(macname)` must pause
        // the run, push a REAL request_dialog modal (#request-overlay), take
        // a typed answer through the REAL bridge (POST /api/request/answer),
        // and resume — $macname must resolve to the typed value afterward.
        // Driven through the do-editor's Run .do (client.runDoString ->
        // the ASYNC "do_string" bridge route, the background do-executor
        // thread) — NOT the single-line #cmd-input/#btn-run path, which
        // goes through the SYNCHRONOUS "command" route and is refused
        // outright by the native side on purpose (blocking the UI thread
        // there would deadlock the whole app; see session_state.h's
        // isCurrentThreadADoWorker() doc comment).
        document.querySelector('[data-action="doedit"]').click();
        return waitFor('doedit open for _request() journey', function () {
          return document.getElementById('doedit-overlay')?.classList.contains('open');
        }, STEP_TIMEOUT_MS).then(function () {
          var ta = document.getElementById('doedit-text');
          if (!ta) throw new Error('#doedit-text not in DOM');
          ta.value = 'display "before request"\n' +
                     'display as text "Type a value: " _request(uismokereq)\n' +
                     'display "after: $uismokereq"';
          var runBtn = document.getElementById('doedit-run');
          if (!runBtn) throw new Error('#doedit-run not in DOM');
          runBtn.click();
          return waitFor('request-dialog overlay opens (#request-overlay.open)', function () {
            return document.getElementById('request-overlay')?.classList.contains('open');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          var macroEl = document.getElementById('request-macro');
          var promptEl = document.getElementById('request-prompt');
          if (!macroEl || macroEl.textContent.indexOf('uismokereq') === -1) {
            throw new Error('#request-macro does not name the pending macro: ' +
                            JSON.stringify(macroEl && macroEl.textContent));
          }
          if (!promptEl || promptEl.textContent.indexOf('Type a value:') === -1) {
            throw new Error('#request-prompt does not carry the preceding display text: ' +
                            JSON.stringify(promptEl && promptEl.textContent));
          }
          var input = document.getElementById('request-input');
          if (!input) throw new Error('#request-input not in DOM');
          input.value = 'ui-smoke-answer';
          var form = document.getElementById('request-form');
          if (!form) throw new Error('#request-form not in DOM');
          if (form.requestSubmit) { form.requestSubmit(); }
          else { document.getElementById('request-submit').click(); }
          return waitFor('request-dialog overlay closes after submit', function () {
            return !document.getElementById('request-overlay')?.classList.contains('open');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          return waitFor('do-run resumed and rendered the post-_request() display line', function () {
            var txt = document.getElementById('console-body').textContent || '';
            return txt.indexOf('after: ui-smoke-answer') !== -1;
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          report(false, true, 'request_dialog_journey',
                 '_request() paused the run, real modal answered via /api/request/answer, ' +
                 '$uismokereq resolved to the typed value, execution resumed');
        });
      }).then(function () {
        // 17. RESIZER GEOMETRY (P8.1 T6, BUG-094): the Variables/History
        // pane-resizer grab zones (panes.js's #pane-resizer-left/-right) must
        // span ONLY the content row — never the toolbar or statusbar rows.
        // This regressed once as CSS Grid row '1 / 6' (the full grid height),
        // which put the invisible col-resize strip on TOP of toolbar controls
        // near the divider and silently swallowed their clicks; the fix
        // scoped it to gridRow '3' (the content row alongside Results/
        // Variables/History). Real CSS Grid layout is required to observe
        // this at all (jsdom has none — the DOM test runner can only assert
        // the element exists, not where it sits), so it can ONLY be gated
        // here, against the packaged app's real WKWebView layout engine.
        var toolbarEl = document.getElementById('toolbar');
        var statusbarEl = document.getElementById('statusbar');
        if (!toolbarEl || !statusbarEl) throw new Error('#toolbar/#statusbar not in DOM');
        var toolbarRect = toolbarEl.getBoundingClientRect();
        var statusbarRect = statusbarEl.getBoundingClientRect();
        var GEOM_TOL = 1;   // px tolerance for sub-pixel layout rounding
        ['pane-resizer-left', 'pane-resizer-right'].forEach(function (id) {
          var el = document.getElementById(id);
          if (!el) throw new Error('#' + id + ' not in DOM');
          var r = el.getBoundingClientRect();
          if (r.top < toolbarRect.bottom - GEOM_TOL) {
            throw new Error('#' + id + ' (top=' + r.top + ') vertically intersects #toolbar (bottom=' +
                            toolbarRect.bottom + ')');
          }
          if (r.bottom > statusbarRect.top + GEOM_TOL) {
            throw new Error('#' + id + ' (bottom=' + r.bottom + ') vertically intersects #statusbar (top=' +
                            statusbarRect.top + ')');
          }
        });
        report(false, true, 'resizer_geometry',
               'pane-resizer-left/right grab zones stay within the content row — no vertical ' +
               'overlap with #toolbar or #statusbar (BUG-094)');
      }).then(function () {
        // 18. SETTINGS TOGGLE (P8.1 T6, BUG-095): the [data-action="settings"]
        // toolbar link opens #settings-overlay; #settings-close (the
        // reintroduced visible ✕ — see step 13's BUG-095 comment) closes it;
        // and a SECOND click on the SAME link toggles it closed again
        // (app.js's settingsLink handler branches on _isOverlayOpen rather
        // than unconditionally opening — the exact BUG-095 fix under test).
        // Ends with the overlay closed so it doesn't linger open for
        // whatever runs after this journey.
        var settingsLink = document.querySelector('[data-action="settings"]');
        var settingsOv = document.getElementById('settings-overlay');
        var settingsCloseBtn = document.getElementById('settings-close');
        if (!settingsLink || !settingsOv || !settingsCloseBtn) {
          throw new Error('settings toggle elements ([data-action="settings"]/#settings-overlay/#settings-close) not in DOM');
        }
        settingsLink.click();
        return waitFor('settings overlay opens on first link click', function () {
          return settingsOv.classList.contains('open');
        }, STEP_TIMEOUT_MS).then(function () {
          settingsCloseBtn.click();
          return waitFor('#settings-close closes the overlay', function () {
            return !settingsOv.classList.contains('open');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          settingsLink.click();
          return waitFor('settings overlay re-opens on a second link click', function () {
            return settingsOv.classList.contains('open');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          settingsLink.click();   // same link, clicked again — toggle closed
          return waitFor('the SAME link click toggles the overlay closed (BUG-095 toggle semantics)', function () {
            return !settingsOv.classList.contains('open');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          report(false, true, 'settings_toggle',
                 'link opens -> #settings-close closes -> link re-opens -> same link toggles closed; ' +
                 'restored to closed');
        });
      }).then(function () {
        // 19. FILE SAVE ROUNDTRIP (P8.1 W2, N1): write a file through the REAL
        // file.save route with an EXPLICIT path (the headless / default-export
        // branch — no NSSavePanel), then read it back via file.read and assert
        // the bytes roundtrip. Proves the bridge genuinely wrote to disk, not a
        // painted {ok:true}. The target is a .do path so file.read (which only
        // serves .do) can verify it; kind:"log" is advisory on the explicit-
        // path branch and does not reshape the written bytes.
        var SAVE_MARKER = 'ui-smoke file.save roundtrip OK\n';
        return window.pietNative.request('file.save', {
          suggested_name: 'ui_smoke_save_roundtrip.do',
          kind: 'log',
          text: SAVE_MARKER,
          path: SAVE_TARGET_PATH
        }).then(function (r) {
          if (!r || r.ok !== true || r.path !== SAVE_TARGET_PATH) {
            throw new Error('file.save did not return {ok:true, path:<target>}: ' + JSON.stringify(r));
          }
          return window.pietNative.request('file.read', { path: SAVE_TARGET_PATH });
        }).then(function (rd) {
          if (!rd || rd.source !== SAVE_MARKER) {
            throw new Error('file.read did not return the exact bytes file.save wrote: ' + JSON.stringify(rd));
          }
          report(false, true, 'file_save_roundtrip',
                 'file.save(explicit path, text) -> file.read: bytes roundtrip, real disk write (N1)');
        });
      }).then(function () {
        // 20. WINDOW TITLE ROUTE (P8.1 W2, N2): POST /api/window/title sets the
        // native NSWindow title verbatim. The title is an AppKit property, not
        // in the DOM, so it is not observable from JS — per the contract this
        // asserts ONLY the route's {ok:true} response shape.
        return window.pietNative.request('window.title', { text: 'PIET — ui-smoke roundtrip' })
          .then(function (r) {
            if (!r || r.ok !== true) {
              throw new Error('window.title did not return {ok:true}: ' + JSON.stringify(r));
            }
            report(false, true, 'window_title_route',
                   'POST /api/window/title -> {ok:true} (native title set verbatim; not JS-observable) (N2)');
          });
      }).then(function () {
        report(true, true, 'journey',
               'boot -> bridge -> kernel dot green -> use -> varlist -> statusbar honest -> regress -> ' +
               'rendered block -> paste routing -> editor-first run -> overlay exclusivity -> ' +
               'transition sanity -> busy lock -> toolbar order -> run-selection -> split+reset -> ' +
               'settings apply -> png export paint check -> request dialog journey -> resizer geometry -> ' +
               'settings toggle -> file save roundtrip -> window title route');
      }).catch(function (e) {
        var detail = String((e && e.message) || e);
        try {
          var errs = document.querySelectorAll('#console-body .block-error');
          if (errs.length) {
            detail += ' | last console error block: ' +
                      (errs[errs.length - 1].textContent || '').slice(0, 300);
          }
        } catch (ignored) {}
        report(true, false, 'journey', detail);
      });
    })();
  )JS";
  return js;
}

// --- BUG-197 falsification hook: PIET_UI_SMOKE_SEED_INJECTION ---------------
// A hermetic stand-in for "a real extension/injector got to this page
// first", so the DONE-WHEN #2 seeded-RED certificate needs no actual
// Grammarly install. Registered via initJs BEFORE smokeDriverJs (user
// scripts run in registration order — same fact main.cpp's installBridge
// comment relies on), so this listener attaches to DOMContentLoaded first
// and its synchronous appendChild()s land before smokeDriverJs's domReady()
// .then() microtask runs (that .then() cannot fire until the DOMContentLoaded
// dispatch — during which both listeners run synchronously in registration
// order — has fully returned). No network I/O: the fake script node carries
// no `src`, only a marker attribute, so this cannot hang on a DNS lookup.
std::string seedInjectionProbeJs() {
  return R"JS(
    (function () {
      function seed() {
        var s = document.createElement('script');
        s.setAttribute('data-fake-injected-probe', 'PIET_UI_SMOKE_SEED_INJECTION');
        s.textContent = '/* seeded by PIET_UI_SMOKE_SEED_INJECTION — not a real threat */';
        document.documentElement.appendChild(s);
        var marker = document.createElement('div');
        marker.id = 'grammarly-desktop-integration';
        marker.setAttribute('data-gramm', 'true');
        marker.style.display = 'none';
        (document.body || document.documentElement).appendChild(marker);
      }
      if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', seed);
      } else {
        seed();
      }
    })();
  )JS";
}

}  // namespace app_entry_detail
