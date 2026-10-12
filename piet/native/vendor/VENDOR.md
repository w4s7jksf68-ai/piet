# Vendored dependencies — piet/native/vendor/

Per the M0 directive (Doctrine gate ③, lightest — no heavy dependencies):
webview single-header + nlohmann/json single-header, pinned and vendored,
nothing else. Both are dev-time fetches from upstream GitHub; the shipped
product stays offline-first (no network calls at runtime).

## webview/webview

- **Vendored file:** `vendor/webview/webview.h` (+ `vendor/webview/LICENSE`, MIT)
- **Pinned tag:** `0.10.0`
- **Commit sha:** `ea2d72a0f6084c2e61f3e715cbcca1585dd09add`
- **Source:** https://raw.githubusercontent.com/webview/webview/0.10.0/webview.h
- **sha256 (webview.h):** `2dff29233cd0b7bd77e6530cf8c778da6167e0d397c2ac8c524f0108716c4fd1`
- **Why 0.10.0, not the latest (0.12.0):** upstream restructured the repo
  starting at 0.11.0 into a multi-file library under `core/include/webview/`
  (the top-level `core/include/webview.h` in 0.11.0+ is an explicit
  `@deprecated` 15-line shim that just `#include`s the split headers — see
  its own doc comment). `0.10.0` is the last release that is a genuine
  single self-contained header (2651 lines: `webview.h` + a small
  `webview.cc` implementation folded into the same file, selected via the
  `WEBVIEW_HEADER` include guard) — this matches the M0 directive's explicit
  ask ("vendor the webview/webview single-header library"). Re-evaluate this
  pin if a later milestone needs a 0.11+-only feature; nothing in M0's scope
  requires it.
- **Platform backend selection (automatic, in the header):** `__APPLE__` →
  `WEBVIEW_COCOA` (WKWebView), `__unix__` → `WEBVIEW_GTK` (WebKitGTK),
  `_WIN32` → `WEBVIEW_EDGE` (WebView2). See `webview.h` lines ~191-197.
- **License:** MIT (Copyright (c) 2017 Serge Zaitsev, 2022 Steffen André Langnes).

## nlohmann/json

- **Vendored file:** `vendor/nlohmann/json.hpp` (+ `vendor/nlohmann/LICENSE.MIT`, MIT)
- **Pinned release:** `v3.12.0`
- **Source:** https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp
  (the release's own pre-built single-header amalgamation asset, not
  hand-amalgamated by us)
- **sha256 (json.hpp):** `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`
- **License:** MIT.

## Microsoft WebView2 SDK header (Windows / WEBVIEW_EDGE only)

Vendored for the **Windows port** (Phase D / WP-W1). The vendored
`webview/webview.h`'s `WEBVIEW_EDGE` backend does a bare
`#include "WebView2.h"` for the `ICoreWebView2*` COM interface *type*
declarations — even with `WEBVIEW_MSWEBVIEW2_BUILTIN_IMPL=1` (which only
removes the need for the `WebView2Loader.dll` import library; the loader is
resolved at runtime via `LoadLibrary`/`GetProcAddress`, it does NOT inline the
COM interface types). So the SDK header is a **build-time compiler input**;
nothing from it is linked into or shipped with the binary.

- **Vendored file:** `vendor/webview2/WebView2.h` (+ `vendor/webview2/LICENSE.txt`)
- **Pinned NuGet package:** `Microsoft.Web.WebView2` version `1.0.2957.106`
- **Source:** https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2957.106
  → `build/native/include/WebView2.h` (the `.nupkg` is a plain zip; extracted
  verbatim, unmodified). `WebView2EnvironmentOptions.h` is NOT needed — our
  `webview.h` creates the environment with a `nullptr` options argument, and
  `WebView2.h` is self-contained (does not include the options header).
- **sha256 (WebView2.h):** `cbcdc6a5135b7b09cc82cafac983505aa36fb61c1af12a1020b7605cf1da3829`
- **License:** **BSD-3-Clause** (`© Microsoft Corporation`; the SDK's own
  `LICENSE.txt`, vendored beside the header). BSD-3 permits source
  redistribution provided the copyright notice + conditions are retained —
  which the co-located `LICENSE.txt` does. This is why vendoring (not
  build-time fetch) is license-clean, consistent with the MIT single-headers
  above.
- **Why this version:** matches the pin in `docker/Dockerfile.piet-windows`
  (Porting Op's cross-build recipe). The patched backend requires a WebView2
  SDK **≥ 1.0.1774.30**: besides `ICoreWebView2_3`
  (`SetVirtualHostNameToFolderMapping`, patch #5 below) it must declare
  `ICoreWebView2WebMessageReceivedEventArgs2` and `ICoreWebView2File` for the
  Windows file-drop `AdditionalObjects` carrier (patch #8). 1.0.2957.106
  declares all three — verified in this vendored copy:
  `IID_ICoreWebView2File` at `WebView2.h:44814`,
  `IID_ICoreWebView2WebMessageReceivedEventArgs2` at `WebView2.h:59392`,
  `ICoreWebView2ObjectCollectionView` at `WebView2.h:50256`.
- **Used only when** `WEBVIEW_EDGE` is selected (`_WIN32`); the macOS/Linux
  builds never see this header. `piet/native/CMakeLists.txt`'s `WIN32` branch
  adds `vendor/webview2` to the include path.
- **Case shim (ours):** `vendor/webview2/EventToken.h` — a one-line
  `#include <eventtoken.h>` bridge. WebView2.h itself does
  `#include "EventToken.h"` (CamelCase), which fails in a MinGW-w64
  cross-compile on a case-sensitive filesystem because the toolchain ships
  the header lowercase. The quoted include searches WebView2.h's own
  directory first, so the shim resolves it without modifying the SDK header
  (kept verbatim, sha above). This file is OURS, not Microsoft's — do not
  count it against the SDK provenance.

## webview.h — vendored PIET patch registry

`vendor/webview/webview.h` is upstream 0.10.0 with the local patches below.
**Every patch must be re-applied and re-verified on any webview vendor bump**
(see the Update procedure). The patch bodies carry inline `── PIET patch ──`
banners; this table is the index.

| # | Platform | Backend | What | Where (marker) | Origin |
|---|----------|---------|------|----------------|--------|
| 1 | macOS | `WEBVIEW_COCOA` | `file://` SPA module loading: KVC prefs `allowFileAccessFromFileURLs` + `allowUniversalAccessFromFileURLs` so `<script type="module">` sibling imports/fetch resolve under the opaque file:// origin | `PIET patch (M5 GA — file:// SPA module loading)` (~L1169) | @a9141d1 |
| 2 | macOS | `WEBVIEW_COCOA` | JS-dialog panels: WKUIDelegate handlers backing `window.prompt`/`alert`/`confirm` with `NSAlert` (WKWebView renders none by default) | `PIET patch (2.0.1 — JS dialog support)` (~L987) | @c99ee07 |
| 3 | macOS | `WEBVIEW_COCOA` | `show_open_dialog(exts)`: native `NSOpenPanel` file picker backing the `dialog.open` bridge route | `PIET (2.0.1): show a native file-open panel` (~L854) | @c99ee07 |
| 4 | Windows | `WEBVIEW_EDGE` | `show_open_dialog(exts)`: Win32 `IFileOpenDialog` counterpart of patch #3 (same signature, so `Shell::openFileDialog` is platform-agnostic) | `PIET patch (WP-W3 — Win32 file-open dialog)` in `win32_edge_engine` | WP-W3 |
| 5 | Windows | `WEBVIEW_EDGE` | `set_virtual_host_mapping(host, folder)`: `ICoreWebView2_3::SetVirtualHostNameToFolderMapping` so the frontend loads over an `https://` virtual-host origin instead of `file://` (Chromium rejects file:// ES-module imports — same class as patch #1 on WebKit). **2026-07-08 (WS-N2):** now returns `bool` (was `void`) and `fprintf(stderr, ...)`s which step failed (missing `ICoreWebView2_3` vs. a failing `SetVirtualHostNameToFolderMapping` HRESULT) instead of silently swallowing either — `Shell::setVirtualHostMapping` (shell.cpp) treats a `false` return as FATAL (`exit(1)`); the original silent-`void` version is what shipped the first Windows canary's dead-app-painting-a-DNS-error failure | `PIET patch (WP-W4 — virtual host mapping)` / `PIET patch (WS-N2 — loud failure, 2026-07-08)` in `win32_edge_engine` | WP-W4 / WS-N2 |

| 6 | macOS | `WEBVIEW_COCOA` | `show_save_dialog(suggested_name, ext)`: native `NSSavePanel` backing the `file.save` bridge route's no-`path` branch | `PIET (2.4.0, N1): show a native file-SAVE panel` (~L892) | 2.4.0 / N1 |
| 7 | Windows | `WEBVIEW_EDGE` | `show_save_dialog(suggested_name, ext)`: Win32 `IFileSaveDialog` counterpart of patch #6 (same signature, so `Shell::saveFileDialog` is platform-agnostic) | `PIET patch (2.4.0, N1 — Win32 file-save dialog)` in `win32_edge_engine` | 2.4.0 / N1 |
| 8 | Windows | `WEBVIEW_EDGE` | WebView2 `AdditionalObjects` carrier for drag/drop: a reserved page message carrying one DOM `File` is read through `ICoreWebView2WebMessageReceivedEventArgs2`, projected to `ICoreWebView2File`, converted from an owned UTF-16 path to UTF-8, and delivered to `Shell` without entering the ordinary JSON-RPC parser. All COM/task allocations are released on the acquisition path. Also hardens upstream's `Invoke` to check `TryGetWebMessageAsString`'s HRESULT before dereferencing. | `PIET patch (Windows file-drop AdditionalObjects carrier)` in `webview2_com_handler` + `win32_edge_engine::set_file_message_handler` | Windows parity port |

**Verification note (WP-W5, no code):** the macOS JS-dialog patch (#2) has **no
EDGE counterpart** and needs none — WebView2/Chromium renders
`window.alert`/`confirm`/`prompt` natively, unlike WKWebView. Left intentionally
un-patched on the Windows backend.

The `#include <shobjidl.h>` added to the `WEBVIEW_EDGE` include block (for
`IFileOpenDialog`/`IShellItem`) is part of patch #4's footprint.

Patch #8 also raises the **runtime** feature floor (distinct from the SDK
floor above): the native side feature-tests
`ICoreWebView2WebMessageReceivedEventArgs2` with `QueryInterface` and the page
shim is expected to feature-test
`chrome.webview.postMessageWithAdditionalObjects`, so an older Evergreen
Runtime stays safe but simply cannot produce a dropped path. This is why
`piet/packaging/piet_setup.iss` now runs Microsoft's bootstrapper
unconditionally instead of skipping it on any non-empty EdgeUpdate registry
entry — "a runtime is present" is not the same as "a runtime new enough to
drop files into".

Patch #8's `file_message_cb_t` handler is **optional**: `win32_edge_engine`
starts with no handler installed, in which case nothing is ever diverted and
the backend behaves exactly as it did before. The host side wires it up via
`set_file_message_handler` (piet/native/src/, Shell/bridge layer).

## Update procedure

1. Pick the new pinned tag/commit from the upstream repo (check whether
   webview has re-consolidated to single-header before assuming `webview.h`
   is still fetchable the same way — see the note above).
2. `curl -sL <raw-url> -o vendor/<lib>/<file>`, re-run `shasum -a 256`, update
   this file's sha + tag/commit.
3. Rebuild (`cmake --build piet/native/build`) and re-run `--selftest` before
   committing the bump.
