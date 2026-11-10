# App shell maintenance

This directory holds everything that turns the engine into a running program: the
process entry point and its command-line modes, the webview shell and JS bridge,
the bridge route adapters, settings, bundle and dev-tree path discovery, the
`--run-block` adapter, the headless do runner and the `--ui-smoke` gate. Start
with `main.cpp` (which flag runs which function), then the single file below.

| Command or symptom | Owner |
|---|---|
| Which command-line flag does what; `--version`, `--do`, `--selftest` hand-off | `main.cpp` |
| Window does not open, wrong frontend page, blank window, `--demo` page | `frontend_window.cpp` |
| A bridge route name is unknown or wired to the wrong handler | `bridge_route_bindings.cpp` |
| `do` / `do_string` worker thread, streamed blocks, progress ticks, window-close join | `bridge_route_bindings.cpp`, `app_entry_internal.h` (`AppState`) |
| `command`, `open`, `varlist`, `browse`, `cell`, `summarize`, `var_detail`, `file.save` payload, `legal.notices` | `routes.cpp` |
| `settings.get` / `settings.set` merge; settings file location; thread and memory budget in the environment; extcompat flag; component packs | `settings.cpp` |
| Scratch directory choice (`PIET_WORKDIR`, `TMPDIR`), persisted settings applied at startup | `startup_environment.cpp` |
| Seeded working directory, `file.read` rules, opening a do-file moves the folder, orphaned scratch directories | `app_support.cpp` |
| JS shims injected into the page, bridge argument parsing, file-drop payload | `bridge_internal.cpp`, `bridge_internal.h` |
| Native window, menu bar, file dialogs, `bind`/`resolve` of bridge calls, file drag-and-drop | `shell.cpp` |
| Packaged `Resources` folder, executable path, development checkout root | `bundle_paths.cpp` |
| `--do FILE` output JSON, exit code, counters emit | `headless_do_runner.cpp` |
| `--run-block` JSON adapter (single command, sequence, `steps`) | `run_block.cpp` |
| `--ui-smoke` fixtures, watchdog, verdict file / journey steps and injected-content probe | `ui_smoke.cpp` / `ui_smoke_driver.cpp` |
| `progress.start` synthetic demo ticks | `progress.cpp` |
| `file.save` base64 decode, `--demo` page encoding | `base64.h` |

`app_entry_internal.h` declares what the entry-point files share: `AppState` (the
one persistent `SessionState` plus the registry of run threads), `ScratchDirGuard`
and the cross-file declarations. Only `main.cpp`, `bridge_route_bindings.cpp`,
`startup_environment.cpp`, `frontend_window.cpp`, `headless_do_runner.cpp`,
`ui_smoke.cpp` and `ui_smoke_driver.cpp` include it. `bridge_internal.h` is shared
by `shell.cpp` and `bridge_internal.cpp`; the selftests also use it to run the
injected JS and the file-drop serializer directly. `base64.h` is a header-only
helper used by `routes.cpp` and the entry files. The functions of `app_support.cpp`
are redeclared in the selftests, so they stay in `piet_core`, not in the app-only files.

State and lifetimes. `AppState` owns the single `SessionState` every bridge route
closes over; routes must use it and never keep a copy. Each `do`/`do_string` run
starts a worker thread plus a 100 ms progress ticker; the worker sets its completion
flag last, finished threads are joined at the next run, and `joinAll()` must run
after `shell.run()` returns and before the `Shell` is destroyed, because workers call
`postEvent()` and `resolveInvoke()` on it. `ScratchDirGuard` is declared before
`AppState` so it is destroyed after the threads are joined; it removes only this
process's `piet_native_*` directory and honours `PIET_KEEP_SCRATCH`.
`reapDeadScratch()` removes only directories with the owner-pid marker, a provably
dead pid and an old enough mtime. `routes.cpp` keeps one unsynchronised file-scope
counter (`routeTmpCounter`) for scratch file names; keep it off worker threads. The
extcompat switch in `settings.cpp` is a process-wide atomic cache, and every change
to threads or memory budget bumps the engine-settings epoch so the resident engine
worker respawns. `settings.set` validates the whole body (64 KB `ui_state` cap)
before saving. `progress.start` (bound in both `bridge_route_bindings.cpp` and the `--demo` page in
`frontend_window.cpp`) detaches a thread that captures the `Shell` by reference and is
not joined by `joinAll()`.

Compile flags. No file here is on the `-ffp-contract=off` list in
`piet/native/CMakeLists.txt` and none carries `#pragma clang fp contract(off)`;
numeric code lives in other directories. `shell.cpp` is compiled with
`-x objective-c++` on macOS because the vendored webview header drives Cocoa
directly; it is the only file allowed to include that header, and it must stay
without ARC. `shell.cpp`, `app_support.cpp`, `bridge_internal.cpp`, `routes.cpp`,
`bundle_paths.cpp`, `progress.cpp`, `settings.cpp` and `run_block.cpp` are in
`piet_core`; the other `.cpp` files are listed on the `piet_native` target only.

Tests. `selftest_bridge_routes.cpp`, `selftest_bridge_transport.cpp`,
`selftest_settings_and_execution.cpp`, `selftest_startup_and_paths.cpp` and
`selftest_data_storage.cpp` (scratch reaping) cover routes, shims, settings, `file.save`,
`file.read`, seeded cwd and path discovery; the window, `--do` and `--ui-smoke` are not.

Elsewhere. Public declarations are in `piet/routes.h`, `piet/shell.h`, `piet/settings.h`,
`piet/bundle_paths.h` and `piet/progress.h`; command execution and the frontend are not here.
