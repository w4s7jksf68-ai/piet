# PIET + CoBrA — System Architecture

**Audience: an AI agent that must modify this system correctly without reading the whole tree.**

## Maintenance entrypoint — 2026-11-09

This map reflects the functional restructuring including the graph, macro and dispatch helper boundaries. It is a
navigation map, not a new numerical-accuracy or performance certificate. The numbered
sections after the separator retain their **2026-07-29 snapshot**; recheck historical
versions, paths, target counts and performance claims before using them.

Start with the symptom below and its existing behavioral checks. The repository stays
one build and one product. Functions moved without redesigning command semantics;
source-specific floating-point flags follow the extracted estimation/command files.

| Symptom | Implementation to read first | Relevant state and existing checks |
|---|---|---|
| A prefix, macro, qualifier, option abbreviation, `_rc`, or output order behaves incorrectly | `piet/native/src/dispatcher.cpp` shows the ordered pipeline and outer result/log lifecycle; `dispatch/stage_*.cpp` owns each step; `dispatch/route_*.cpp` owns command families | `registry.cpp` remains the command-name/abbreviation/qualifier authority; `dispatch/*_support.h` exposes helpers by owner. Read `dispatch/README.md`; full native selftest, freeze, pangram and structural floor gates cover integration |
| `esttab`, `estout`, `outreg2` or a table file has wrong cells, escaping or spacing | `table_esttab.cpp`, `table_estout.cpp`, `table_outreg.cpp`; estout grammar/model/format helpers in `estout_*.cpp`; shared prepared-table rendering in `table_render.cpp` and `include/piet/table_{model,render}.h` | `table_model_context.cpp`, `table_number_format.cpp`, `table_files.cpp`; `--suite tables`, `selftest_tables.cpp`, `selftest_w2_esttabnames.cpp` |
| Added estimate results or exported datasets/matrices are wrong | `table_estadd.cpp`, `table_estpost.cpp`, `table_dataset_export.cpp`, `table_matrix_export.cpp`, `table_tex_files.cpp`, `table_erepost.cpp` | Active/stored fits and live `e()` mirrors; same table suite plus full selftest |
| Macro expansion, definitions, numeric text or matrix references are wrong | `eval/macros.cpp` coordinates expansion; `eval/macro_{definitions,scanners,text,expressions,results,matrices}.cpp` own the specific work | `eval/macro_modules.md`; existing macro/language and postestimation selftests |
| Graph blocks, histogram/density, SVG export or live-view thinning are wrong | `graph/commands.cpp`, `graph/distribution_commands.cpp`, `graph/twoway.cpp`, `graph/svg.cpp`, `graph/files.cpp`, `graph/view.cpp` | `graph/README.md`; graphics selftests and exact SVG comparisons |
| Uniqueness, missingness, labels, file lists, spells, rolling statistics or calendars are wrong | Named command families under `utility/`; `utility/support.cpp` owns shared column/read/write helpers | `utility/README.md`; utility/calendar selftests and common option-registration checks |
| A model fit has the wrong options or engine request | Named `*_estimation.cpp` modules, for example `binary_estimation.cpp`, `panel_estimation.cpp`, `instrumental_variables.cpp`, `count_estimation.cpp`; shared `estimation_support.cpp`, `estimation_results.cpp`, `estimation_options.cpp` | `estimation_internal.h` and family-specific internal headers; functional `selftest_*estimation.cpp` files and existing `selftest_w2*.cpp` cases |
| `predict`, `margins`, `test`, `lincom`, `nlcom`, or `estat` uses the wrong fit/sample | `prediction.cpp`, `margins.cpp`, `hypothesis_tests.cpp`, `estimation_diagnostics.cpp`, `postestimation_context.cpp` | `LastModel`, fit restrictions/weights, `e(sample)`, live `e()`/`r()`; `selftest_postestimation.cpp`, `selftest_w2b_predict.cpp`, `selftest_w2b_margins.cpp`, `selftest_w2c_postest.cpp` |
| `merge`, `append`, `joinby` or `reshape` loses rows, types or labels | `data_merge.cpp`, `data_append.cpp`, `data_joinby.cpp`, `data_reshape.cpp`; extended commands in `handlers_aer_reshape.cpp` | Dataset keys/order, metadata and resident/materialized mutation results; `selftest_data_combine.cpp`, `selftest_w2_mergeappend.cpp` |
| A data command changes the wrong variables/rows | `variable_copy_rename.cpp`, `data_collapse.cpp`, `data_order.cpp`, `data_duplicates.cpp`, `data_recode.cpp`, `data_observation_expansion.cpp`, `data_quantiles.cpp`, `data_interpolation.cpp` | Shared writer rules remain in `handlers_data_verbs_internal.h`; `selftest_data_commands.cpp`, `selftest_data_storage.cpp`, `selftest_w2ah_dataverbs.cpp` |
| Save, CSV, spreadsheet or string conversion is wrong | `dataset_save.cpp`, `delimited_import.cpp`, `delimited_export.cpp`, `spreadsheet_import.cpp`, `spreadsheet_export.cpp`, `handlers_string_conversion.cpp` | Native read/write services, metadata and file overwrite rules; `selftest_file_io.cpp`, `selftest_spreadsheets.cpp`; `--suite data-conversion` |
| Stored estimates restore the wrong model or added results | `session_estimates.cpp`, `session_fit_state.cpp` | Snapshot/model pins, `e(sample)`, live `e()` and added mirrors; `--suite stored-estimates` |
| Dataset replacement, scratch ownership or a resident card is stale | `session_dataset.cpp`, `session_state.cpp`; `session_memory.cpp` owns memory accounting | `load`, `datastore`, materialization and pin/reclaim lifetimes; `selftest_bridge_routes.cpp`, `selftest_data_storage.cpp` |
| Time/panel declarations disappear or display incorrectly | `time_declarations.cpp`, `session_time_metadata.cpp`, `time_panel_state.cpp`, `time_fill.cpp` | Panel/time metadata, formats, characteristics and `r()`; `--suite time-declarations` |
| Editor contents, file reopen, command execution or cancellation is wrong | `piet/frontend/js/editor.js`, `execution.js`, `command-input.js`, `run-progress.js`, `request-dialog.js`, `recent-files.js` | Controllers own feature state; `app.js` composes narrow callbacks; DOM suite and actual native UI smoke |
| Split panes, settings or window restoration is wrong | `piet/frontend/js/layout.js`, `settings.js`, `app-navigation.js`; existing `panes.js` and `css/piet.css` | Local storage/native `ui_state`, DOM order and deferred controller callbacks; DOM suite and actual native UI smoke |

### Boundaries that matter

`dispatcher.cpp` is now orchestration. Preserve the order of its preprocessing stages,
its recursion and the lifetime of `RcHiddenScope` / `QualifierColumnGuard`. Routes run
only after those common steps. Keep command names and abbreviations in the existing
registry; a route family is an implementation grouping, not another command registry.

Prepared-table rendering takes a table and settings and returns text. It must not fetch
a model, mutate a session or write a file. Command-specific esttab/estout/outreg writers
retain their established formatting paths: follow the actual call before changing a
shared renderer. `tables_internal.h` contains implementation-private types and helper
declarations; helpers have one definition in their named source file.

`SessionState` remains the owner of the same fields and resource lifetimes. Shared value
types live in `include/piet/session_types.h`; the session header exposes declarations,
layout and small accessors. Constructor wiring, dataset transitions, live-fit posting,
metadata/time metadata and memory accounting have separate implementation files. The
stored-estimate implementation remains in `session_estimates.cpp`. This extraction does
not turn session fields into separately copied state objects.

The old numbered `estimation_m4{,b,c}.cpp`, `handlers_m3.cpp` and
`handlers_data_verbs{,2,3}.cpp` implementations have been replaced by functional modules.
Do not recreate the numbered buckets or copy their shared helper bodies. Private
`*_internal.h` headers are for their named implementation family, not general utilities.

### Build and focused checks

The application and selftest link the same `piet_core` objects. Test cases are excluded
from the application. `selftest.cpp` selects suites and calls the established full run;
219 former embedded cases live in 29 functional files. Shared counters and fixture
helpers have one definition in `selftest_support.cpp`. The full run's call order is
preserved, including its final float-store fallback check. Cases that locate fixtures
through `__FILE__` retain the original source anchor through `selftestSourceFile()`.

For a public checkout, use `sh scripts/build-macos.sh` and
`python3 tests/ui_smoke.py` on macOS; the build script runs the public engine and
application smoke checks. The Windows counterpart is `scripts/build-windows.sh`.
The full maintainer selftest below depends on additional fixtures that are not
included in the public source distribution. A missing fixture is not a passing test.

For a maintainer checkout, from the repository root:

```sh
cmake --build piet/native/build -j4
piet/native/build/piet_selftest --list-suites
COBRA_BIN="$PWD/build/cobra" piet/native/build/piet_selftest --suite tables
COBRA_BIN="$PWD/build/cobra" piet/native/build/piet_selftest --suite data-conversion
COBRA_BIN="$PWD/build/cobra" piet/native/build/piet_selftest --suite stored-estimates
COBRA_BIN="$PWD/build/cobra" piet/native/build/piet_selftest --suite time-declarations
COBRA_BIN="$PWD/build/cobra" piet/native/build/piet_native --selftest
node piet/frontend/test/dom_runner.mjs
```

Selections reuse existing test functions and assertions; they do not replace integration
checks. `--suite all` selects the unchanged full run. Some historical W2 scaffold
functions are empty; a matching filename alone does not prove coverage. Source-path
checks must follow every relocated writer, keep their original expected counts and
assert that every file was read.

For a structural change compare output bytes, errors, state effects, registry and option
vocabulary before/after. Run the applicable `tools/merge_gate.sh` checks on the final
candidate. UI changes also need the native `--ui-smoke` journey; DOM tests alone do not
establish webview geometry, the native bridge or restoration across a restart. Keep
oracle data, tolerances, graders and command schemas unchanged.

### Other large files: deliberate scope

This restructuring addresses the planned central orchestration, numbered command
buckets, tables, session implementations, tests and frontend. It does not impose a line
limit on every algorithm. The following remain searchable implementation boundaries:

- `eval/macros.cpp` retains expansion orchestration. Definition/extended-function
  handling, lexical scans, text helpers, expressions, results and matrices now have
  `macro_*.cpp` owners. See `eval/macro_modules.md`. Expansion order and the single
  inline-note queue are unchanged; this is a language implementation boundary,
  not permission to reorder its passes.
- Graph implementation is grouped under `src/graph/`: `commands.cpp` builds simple
  graph blocks, `distribution_commands.cpp` owns histogram/density/diagnostic commands,
  `distribution_math.cpp` owns their numerical helpers, and `twoway.cpp` handles the
  layer pipeline. `svg.cpp` renders blocks, `files.cpp` handles save/combine/export,
  and `view.cpp` thins live-view points. See `graph/README.md` for the dependency map.
  All extracted graph sources retain the original floating-point compile setting.
- `handlers_aer_estim.cpp` and `handlers_aer_reshape.cpp` are existing feature-extension
  families with named commands, not unnamed continuation files.
- Engine `src/data/frame*.cpp`, LAPACK translations and estimator kernels remain in their
  numerical/data-plane layer. Splitting a cohesive algorithm only to reduce line count
  would not narrow a normal command/UI maintenance task.

These boundaries do not claim that every remaining function is short or that all design
debt is gone. For normal maintenance, use the symptom map before widening the read scope.

---

## Historical architecture snapshot — 2026-07-29

Written 2026-07-29 by the Secretary from direct inspection of the repo at `main @27f2deb`
(4-tuple `{piet B.6.2, cobra 0.34.2, contract_A 1.20.0, contract_B 2.6.0}`).

Every path below is repo-relative unless marked otherwise.
Read §14 (provenance) before trusting any claim you are about to act on.

> **Landing note (2026-07-30, maintainer).** Landed here unedited from the private coordination tree's `ARCHITECTURE.md`
> per the User commission (MSG-SE-065). The text is pinned to `main @27f2deb`; main has since
> advanced to `@89ba559`, which replaced `Dataset::open`'s DOM header parse with a streaming (SAX)
> reader — an in-memory read change only (`src/io/columnar.cpp`, disk formats and both contracts
> untouched, freeze corpus byte-identical). It does not move §4/§10/§11, but **§13's open-cost
> numbers predate it** (measured effect: `rll_open` −25.2%/−35.4%/−31.3% across three shapes,
> paired). Re-ground §13 against the current instruments before quoting it.

---

## 0 · The system in one paragraph

Two C++ programs and a web frontend. **`cobra`** is a headless econometrics engine — a CLI that
reads columnar data and prints JSON. **`piet_native`** is a desktop application that embeds a system
webview, renders an HTML/JS user interface inside it, interprets the Stata language, and drives
`cobra` as a long-lived child process to do the actual math. The user types Stata commands; PIET
parses and orchestrates; cobra computes; PIET renders. **cobra owns the truth, PIET owns how it is
shown.** Everything is local — no network, no server, no Python at runtime.

```
┌─────────────────────────────────────────────────────────────────────┐
│ piet_native  (C++ executable, one process)                          │
│                                                                     │
│  ┌───────────────────────────┐   Boundary B    ┌─────────────────┐  │
│  │ WebView (WKWebView /      │◄───────────────►│ Shell / routes  │  │
│  │ WebKit2GTK / WebView2)    │  window.        │ (C++)           │  │
│  │  · piet/frontend/*.html   │  pietNative.    └────────┬────────┘  │
│  │  · JS: app/api/grid/graph │  request(k,body)         │           │
│  └───────────────────────────┘                          │           │
│                                                ┌────────▼────────┐  │
│                                                │ SessionState    │  │
│                                                │ dispatcher      │  │
│                                                │ Stata interp.   │  │
│                                                └────────┬────────┘  │
└─────────────────────────────────────────────────────────┼───────────┘
                                                          │ Boundary A
                                       line-delimited JSON over stdin/stdout
                                                          │
                                              ┌───────────▼───────────┐
                                              │ cobra serve (child)   │
                                              │ = the same `cobra`    │
                                              │   binary, serve mode  │
                                              └───────────┬───────────┘
                                                          │ mmap
                                                 ┌────────▼────────┐
                                                 │ .hbin columnar  │
                                                 │ / .dta          │
                                                 └─────────────────┘
```

---

## 1 · Component map

| Path | What it is | Language | Produces |
|---|---|---|---|
| `src/` | cobra engine library (`cecon_core`) | C++17 | static lib |
| `apps/cecon_cli.cpp` + `apps/cmds/` | cobra CLI front end + subcommand tables | C++17 | `cobra` executable |
| `piet/native/src/` + `include/piet/` | PIET application core: Stata interpreter, session, bridge | C++17 | `piet_native` executable |
| `piet/frontend/` | the user interface | HTML/CSS/ES modules | loaded into the webview |
| `piet/packaging/` | `.app` bundling (macOS), Inno Setup (Windows) | shell / iss | shippable app bundle |
| `tests/` | engine unit tests, eval goldens, corpora, benches | C++ / Python | gates |
| `CONTRACT.md`, `CONTRACT-engine.md` | the two wire contracts (internal planning tree, not in the public tree) | Markdown | law |
| `docs/` | consolidated documentation tree | Markdown | reference |
| `docker/`, `dist/` | cross-compilation and platform artifacts | Dockerfile / binaries | Linux + Windows builds |
| `tools/` | `.hbin` reader/writer, freeze matrix | Python | dev tooling |
| `vendor/`, `piet/native/vendor/` | third-party: `nlohmann/json`, `webview` (pinned 0.10.0), WebView2 | C++ headers | vendored |

A non-git sibling directory (the private coordination tree) holds governance, releases archive, and the fleet's
operational state. It is **not** part of the product.

---

## 2 · Build system

Two independent CMake projects. They are **not** built together.

### 2.1 Engine — root `CMakeLists.txt`

- `cmake_minimum_required 3.20`, C++17, default build type `Release`.
- Optimization: `-O3 -march=native` on Apple; on non-Apple x86 `-march=${CECON_X86_ARCH}`
  (cache variable, default `x86-64-v3`) so a downstream container build can retarget without
  editing this file; `-mtune=generic` on other architectures.
- **Linear algebra is never reimplemented.** Backend selection:
  - macOS → Apple **Accelerate** (`ACCELERATE_NEW_LAPACK=1`)
  - Linux static → caller passes full `.a` paths via `CECON_STATIC_LIBS` (OpenBLAS/LAPACKE/gfortran/quadmath)
  - Linux dynamic → OpenBLAS via pkg-config, plus LAPACKE if found
- Targets: `cecon_core` (STATIC library) → `cobra` (executable, links `cecon_core`) → a fleet of
  `test_*` unit executables, each linking `cecon_core`.
- The CLI target compiles `apps/cecon_cli.cpp` plus all eight `apps/cmds/cmds_*.cpp` translation
  units plus `common.cpp` and `options_table.cpp`.

### 2.2 Application — `piet/native/CMakeLists.txt`

- Single target `piet_native`, links `Threads`.
- Platform webview backing:
  - **macOS** → `WebKit` + `Cocoa` frameworks (the vendored `webview.h` auto-detects `__APPLE__`
    and takes its WEBVIEW_COCOA path)
  - **Linux** → WebKit2GTK
  - **Windows** → WebView2, linked with `-mwindows`
- `piet_native` does **not** link `cecon_core`. It reaches the engine only by executing the `cobra`
  binary as a child process (§4). This is a deliberate isolation boundary, not an oversight.

### 2.3 Cross-platform

`docker/` carries `Dockerfile.linux`, `Dockerfile.linux-static`, `Dockerfile.windows`,
`Dockerfile.piet-windows`, a MinGW-w64 `windows-toolchain.cmake`, and `windows-compat` shims.
Built artifacts land in `dist/{linux-amd64,linux-arm64,windows-amd64,windows-amd64-piet}`.

---

## 3 · The engine: `cobra`

### 3.1 Library layout (`src/`)

| Directory | Files | Responsibility |
|---|---|---|
| `core/` | 11 | `matrix`, `linalg` (BLAS/LAPACK wrappers), `model`, `collinear`, `error`, `threads` (`parallel_for`, `ThreadPool`), `rng.h` |
| `io/` | 5 | `columnar` (the `.hbin` memory-mapped reader), `dta_reader` (native Stata `.dta` 115/117/118), `win_compat.h` |
| `data/` | 5 | `frame` (the in-memory dataset), `cow_vec.h` (copy-on-write column storage), `radix_sort.h`, `ts_operators.h` |
| `hdfe/` | 4 | `demean` (parallel alternating projections), `dof` (fixed-effect redundancy / degrees of freedom) |
| `estimators/` | 37 | one pair per estimator family (below) |
| `inference/` | 2 | `wildboot` — wild cluster bootstrap-t (WCR fast algorithm) |
| `vcov/` | 2 | variance-covariance: iid, HC1, one-way and multiway (CGM) cluster with PSD repair |

**Determinism is architectural, not incidental.** `rng.h` uses **Philox4x32-10**, a *counter-based*
generator: a draw is a pure function of `(seed, counter)`, so results are bit-identical regardless
of thread scheduling. Parallel reductions that could reorder floating-point addition are written to
reduce in a fixed order (e.g. `xtlogit`'s per-group buffers reduce in group order), so worker count
never changes the answer.

### 3.2 Estimators (`src/estimators/`)

`ols` · `feols` (high-dimensional FE OLS) · `iv2sls` · `liml` · `fepois` (PPML) · `logit` ·
`probit` · `nbreg` · `qreg` (quantile) · `tobit` · `heckman` · `reg3` (3SLS/SUR) · `xtreg` (RE/FE
panel) · `xtlogit` · `xtpois` · `csdid` (Callaway–Sant'Anna staggered DiD) · `neweyprais`
(HAC/Newey-West, Prais-Winsten) · `unitroot` (dfuller/pperron) · `posthest` · `nlcom` ·
`diagtests` · `iv_diagnostics` · `iv_weakid` · `ts_diag`.

### 3.3 CLI surface (`apps/`)

`apps/cecon_cli.cpp` is the dispatcher. `apps/cmds/` splits subcommands by family:
`cmds_linear` · `cmds_panel` · `cmds_mle` · `cmds_postest` · `cmds_diag` · `cmds_ts` ·
`cmds_summary` · `cmds_data`, with `commands.h` (67 entry points) and `options_table.cpp`
(69 entries) as the registries.

`options_table.cpp` is load-bearing beyond the CLI: subcommand `cobra options [--json]` serializes
the accepted-flag table so **PIET can validate a flag before forwarding it**. Combined with the rule
that unrecognized-flag rejection *precedes* output (cobra buffers stdout and discards it if the
unknown-flag net fires → exit 3, `bad_input`, empty stdout), this structurally kills the class of bug
where an ignored option silently produced a wrong-but-plausible answer.

---

## 4 · The engine boundary (Boundary A) — how PIET talks to cobra

**Contract:** `CONTRACT-engine.md` (internal), version **1.20.0**, pen = cobra lane.
Emitted at runtime by `cobra version --json` as `contract_version`.
Anchor (change requires direct User approval): `coeftable[{name,b,se,t,p,ci_lo,ci_hi}]`.

⚠ The contract file's title line still reads "cobra CLI ↔ **Python kernel**". That is a 1.x-era
relic — there is no Python at runtime. The governing description is "cobra CLI ↔ PIET core".

### 4.1 Three transport modes, same semantics

| Mode | Mechanism | Where | Used for |
|---|---|---|---|
| One-shot | `fork`/`exec` `cobra <subcmd> …`, read stdout | `piet/native/src/cobra_runner.cpp` (`runCobra`) | fallback; the semantic reference |
| **Serve, stateless** (§25) | persistent child, line-delimited JSON per request | `cobra_session.cpp` `sendStateless(argv)` | the universal safe path |
| **Serve, resident frame** (§26) | dataset stays in the worker's RAM across mutations | `cobra_session.cpp` `frameUse/frameRun/frameSnapshot/frameDrop` | the fast path |

`sendStateless` is **byte-equal by construction** to a one-shot invocation — the serve loop routes an
empty-argv request through the same code path. This is the property that lets the fast transport be
trusted without re-verifying every estimator.

### 4.2 Why the resident frame exists

A one-shot process per verb pays a spawn tax on *every* dispatch, and the chain-head model rewrote
and re-mmap'd a fresh scratch `.hbin` per mutation (the "F5 incident": 2,742 mutations, each writing
a full dataset). Resident mode advances the frame in place and returns a small **delta**
(`{changed, added, dropped, structure}` plus per-column `{name, type, n_missing, n_levels}`) that the
caller folds into a shadow varlist card. Data materializes to disk only on an explicit snapshot.

`CobraSession` owns **only** the worker process and the wire protocol. All resident bookkeeping —
which path the frame corresponds to, the dirty bit, the shadow card — lives on `SessionState` and is
driven through a `runCobra` intercept. If you change resident semantics, `session_state.{h,cpp}` is
where the state machine actually is.

Two metadata booleans are deliberately distinct and must not be conflated:
`structure` = write-path ("can a later `save` byte-patch the source?"), `card_stable` = read-path
("is the per-column metadata card still valid?"). One boolean once answered both, and that was a bug.

### 4.3 Data formats

- **`.hbin`** — the native columnar format. `src/io/columnar.h`: memory-mapped, **zero-copy** —
  columns are pointers into the mapping. Carries a source-path field for provenance and
  clone-patching. Writer/reader tooling: `tools/to_hbin.py`, `tools/hbin_read.py` (require numpy).
- **`.dta`** — Stata formats 115/117/118 read natively in C++ (`src/io/dta_reader.cpp`), no Python.
  Format 117 and 118 differ in strL key widths ((u4,u4) vs (u2,u6)); both sides must agree.

---

## 5 · The application: `piet_native`

40 translation units in `piet/native/src/`, 26 headers in `include/piet/`. By role:

| Role | Files |
|---|---|
| Process entry, window, modes | `main.cpp`, `shell.cpp`, `bundle_paths.cpp` |
| Bridge (Boundary B) | `routes.cpp`, `bridge_internal.cpp`, `progress.cpp` |
| Session & data plane | `session_state.cpp`, `datastore.cpp`, `cobra_session.cpp`, `cobra_runner.cpp`, `mutation_result.cpp` |
| Stata language | `eval/tokenizer.cpp`, `eval/gettoken.cpp`, `eval/macros.cpp`, `eval/exprfns.cpp`, `eval/evalcase*.cpp`, `eval/errors.cpp`, `eval/pyeval.cpp`, `eval/pyvalue.cpp` |
| Command dispatch | `dispatcher.cpp`, `handlers.cpp`, `handlers_m3.cpp`, `handlers_data_verbs.cpp`, `registry.cpp`, `routes.cpp` |
| Control flow / programs | `loops.cpp`, `program.cpp`, `run_block.cpp`, `doexec.cpp` |
| Data verbs | `strmutate.cpp`, `egen.cpp`, `winsor2.cpp`, `tabulate.cpp`, `postfile.cpp`, `inrange_filter.cpp`, `translate.cpp` |
| Estimation presentation | `estimation_m4.cpp`, `eststo_esttab.cpp` |
| Graphics | `graph_m4.cpp`, `twoway_parse.cpp`, `twoway_opts.cpp` |
| Output integrity | `display_hardening.cpp`, `cclass.cpp`, `settings.cpp` |
| Self-verification | `selftest.cpp` |

### 5.1 Entry modes (`main.cpp` argument parsing)

| Flag | Behavior |
|---|---|
| *(none)* | open the window, run the GUI |
| `--do FILE` | headless: execute a do-file, print to stdout, exit. **No webview.** |
| `--selftest` | run the in-binary check suite; prints `ALL CHECKS PASSED` or fails |
| `--ui-smoke` | scripted user journey through the real webview — the packaged-app gate |
| `--run-block` | execute one block (harness entry) |
| `--evalcase` | expression-evaluator case runner |
| `--version` | the 4-tuple |
| `--dump-registry` | dump the command registry |
| `--demo` | demo mode |

**Critical consequence of the `--do` design:** a headless run has **no webview**, therefore no
rasterizer. `graph export "…png"` cannot produce a PNG; `graph_m4.cpp` honestly substitutes an
`.svg` and emits a `rasterizer pending` note naming the substitution. Any code that assumes a `.png`
file appears after a headless run is wrong (this caused BUG-143).

### 5.2 Engine binary resolution

PIET calls **only** the main build `build/cobra` or the pinned archive
`releases/engine/current/cobra` in the private coordination tree. Dev builds are forbidden — PIET only ever sees an
engine that passed the merge gate. `resolveCobraBin()` picks the pin when `COBRA_BIN` is unset, so
**an unset `COBRA_BIN` silently measures whatever the pin points at** — a real measurement hazard.

---

## 6 · The UI boundary (Boundary B) — how the frontend talks to the core

**Contract:** `CONTRACT.md` (internal), version **2.6.0**, pen = piet lane.
Anchor: `CoefficientRow`.

### 6.1 Transport

There is **no HTTP server**. `piet::Shell` (`shell.cpp`) is a thin wrapper over the vendored
`webview` single-header library (pinned 0.10.0) and installs a JS-visible object:

```js
window.pietNative.request(routingKey, body) -> Promise
```

`Shell::installBridge()` creates it; `webview`'s `bind()` protocol resolves each call by
`(seq, status, json)`. **Bind callbacks run on the UI thread's dispatch queue** — this threading
model is documented in `shell.h` and must be respected when adding routes.

### 6.2 Routing keys are former URL paths

The frontend was originally an HTTP client. The transport swap preserved **endpoint identity**:
a path becomes a routing key by one mechanical rule — strip `/api/`, replace `/` with `.`.

```
POST /api/label/variable   ->  "label.variable"
POST /api/request/answer   ->  "request.answer"
GET  /api/var_detail       ->  "var_detail"
```

`pathToRoutingKey()` in `piet/frontend/js/api.js` implements exactly that rule, with no
per-endpoint exceptions. `api.js` detects `window.pietNative`; if absent it falls back to mocks
(that is how the DOM tests run headless).

One route handler per endpoint lives in `routes.cpp`, each bound via a lambda in `main.cpp` closing
over **one shared, persistent `SessionState`** owned by `AppState` and constructed when the window
opens. This is the live interactive kernel — distinct from `run_block.cpp`'s per-invocation fresh
session used by headless harnesses.

### 6.3 Push channel

Native → frontend events ride `window.pietEvents`. Shapes are discriminated by a `kind` field;
`RunProgress` ticks have **no** `kind` at all, which is what makes the union unambiguous. Known
kinds: `block` (live streaming), `request_dialog` (a blocked `_request()` prompt), `file_dropped`
(the real filesystem path of a dropped file — WKWebView withholds this from page script, so this
event is the only carrier).

### 6.4 Frontend

`piet/frontend/index.html` + `css/` + 11 ES modules in `js/`:
`app.js` (orchestration) · `api.js` (the bridge client) · `console.js` · `grid.js` (data browser) ·
`graph.js` · `panels.js` · `panes.js` · `varstats.js` · `find.js` · `gutter.js` · `util.js`.

Tests: `piet/frontend/test/dom_runner.mjs` with `dom_runner.suites.json` (Node + jsdom;
`node_modules` vendored in-tree). The runner has a **registered-suite manifest floor** so a suite
that silently stops registering is RED and named, rather than reporting a smaller green number.

---

## 7 · End-to-end execution trace

What happens when the user types `reghdfe y x, absorb(id) cluster(id)` and presses run:

1. **Frontend** — `app.js` collects the text, `api.js` calls `window.pietNative.request("command", {cmd})`.
2. **Bridge** — `webview` `bind()` delivers it on the UI thread; `routes.cpp::command` receives the body.
3. **Interpretation** — `dispatcher.cpp` resolves the verb through `registry.cpp`; `eval/tokenizer.cpp`
   splits it (quote-aware — a quote-blind regex here is a known defect class); `eval/macros.cpp`
   expands macros; qualifiers (`if` / `in` / `by`) and options are parsed. A qualifier that cannot be
   honored must raise an explicit error — silently dropping one answers a *different question* and is
   a gate-① violation.
4. **Option validation** — the flag is checked against cobra's own `options` table before forwarding,
   so a skew errors instead of silently producing a wrong number.
5. **Engine call** — `session_state.cpp` routes through the `runCobra` intercept into
   `cobra_session.cpp`. If the dataset is already resident, this is a `frameRun`; otherwise a
   stateless request or a one-shot.
6. **Compute** — the `cobra serve` child parses argv identically to the CLI, `data/frame` holds the
   columns (mmap'd, zero-copy from `.hbin`), `hdfe/demean` absorbs fixed effects, `estimators/feols`
   fits, `vcov/` builds the clustered covariance, and JSON goes back over stdout.
7. **Fold** — PIET parses the response; a mutation's delta folds into the shadow varlist card
   (no full rescan); `mutation_result.cpp` normalizes the shape.
8. **Presentation** — `estimation_m4.cpp` formats the coefficient table into `ResultBlock`s carrying
   `CoefficientRow` (the Boundary-B anchor); r-class results are filled at the dispatcher call site.
9. **Render** — blocks stream to the frontend as `{kind:"block"}` push events; the untransmitted tail
   comes back in the response's `blocks`; `console.js` renders.

---

## 8 · Verification architecture

Gates are the load-bearing structure of this project. Each targets a different layer, and
**a gate must cover the layer the change actually touched.**

| Gate | Command / location | Covers |
|---|---|---|
| Engine unit tests | `ctest` over `tests/unit/test_*` | engine internals (linalg, HDFE, IV, sorts, determinism) |
| **Eval goldens** | `tests/eval/run_eval.py` | gate-① truth: every emitted scalar vs a pinned Stata oracle |
| Freeze matrix | internal tool and captures, not in the public tree | byte-stability of output across builds |
| PIET selftest | `piet_native --selftest` | in-binary invariants, seeded-defect legs |
| DOM runner | `piet/frontend/test/dom_runner.mjs` | frontend behavior (headless, mocked bridge) |
| **UI smoke** | `piet_native --ui-smoke` on the **packaged** app | the real WKWebView journey |
| Serve parity | `tests/eval/` serve legs | serve output == one-shot output |
| Floor gate | counters-based | that skipped work is actually skipped |
| Bench ②/③ | `bench_gate.py` | wall/memory against pinned baselines |

Governing rules:

- **"Renders" ≠ "works."** Any release touching UI, packaging, or transport is gated by a
  demonstrated journey through the *packaged* app. A headless PASS never declares a UI release green.
- **The falsification certificate.** A gate is trusted only through a demonstrated **RED** on the
  defect class it claims to kill — a seeded defect, a reverted mechanism, or an archived-binary arm
  that fails. A gate with no RED demonstration is a hypothesis. Ledger:
  the internal verification ledger.
- **Eval inviolability.** Only the eval lane may modify goldens, tolerances, or manifests. Fix loops
  touch `src/`, `apps/`, `piet/` and never goldens. Eval truth moves only *toward* the oracle.
- **Optimization claims ship with a mechanism gate** — a counter that fails when the skipped work
  happens anyway — never a timing value alone. Timing is the noisy witness; deterministic counters
  are the sworn one.
- **A value-pinning test cannot verify that the value arrived cheaply.** That is why counter gates
  exist alongside golden gates.

Corpora: an internal bug corpus and bug ledger (not in the public tree) · `tests/validate/`
(oracle CSVs, benches) · `tests/golden/` · `demos/` · and an external campaign corpus
(outside the repo; `analysis.do`, ~1,750 lines, 160 dependent
variables — the full-run performance workload).

---

## 9 · Versioning and release

**Four independent semver axes** plus the 4-tuple that binds them:
`piet_version` · `cobra_version` · `contract_A` · `contract_B`.

- Authority file: `releases/CURRENT` in the private coordination tree.
- Engine archive: `releases/engine/<X.Y.Z>/`, with `engine/current` a symlink to the
  one archived build that is the reference point.
- Product archive: `releases/product/piet-<P>+cobra-<C>/`.
- A version becomes a release **only when stamped at merge with the release checklist completed**.
  The checklist is executable and stop-the-line — a stale pin or a failed step halts the release; it
  is never a workaround target.
- The engine may ship ahead of a paired product recut. A packaged `.app` stays self-consistent
  because its 4-tuple assertion compares `Info.plist` against its **own** bundled cobra.
- Contract grades: *additive* = MINOR (pen lane's discretion) · *rename/remove/retype* = MAJOR
  (counterpart ACK required as a named gate) · **anchor change = User approval only**.

Packaging: `piet/packaging/package_native_app.sh` (macOS `.app`; `codesign --verify` failure exits 1),
`piet_setup.iss` + `stage_native_windows.sh` (Windows), `M5_USER_CHECKLIST.md` (the human journey).

---

## 10 · The four gates (why the code looks the way it does)

Strict lexicographic order — satisfy each before optimizing the next:

1. **Identical to Stata**, input and output. Same input → same output, bit-identical run-to-run.
   Every scalar matches the pinned oracle within eval-ratified tolerance. An honest reject is parity;
   **a silent wrong answer is the cardinal defect.** Stochastic estimators satisfy this as ①-S:
   fixed-seed bit-reproducibility plus distributional agreement.
2. **Then fastest** — against the *physical floor* (memory bandwidth, SSD, IPC) for the minimal work
   the semantics require, not against the incumbent.
3. **Then lightest** — dependencies → memory → allocations → shipped size. Amplification
   (bytes touched ÷ bytes semantically required) has an optimum, and it is 1.
4. **Then portable** — Windows/Linux, without regressing ①–③ on the reference machine.

Reference machine: an 8 GB Apple-silicon Mac with Accelerate. Lightness is not aesthetics — it is
whether a student with a weak laptop can run the thing.

PIET-layer specializations: **engine-driven** (a precise, honest mirror of what cobra says — never
invent, never fake), **lightness** (adaptive in both directions), **precision** (only what belongs,
exactly where it belongs).

---

## 11 · Invariants a modifying agent must not break

1. **No network.** All compute is local; no feature may require one. Any local bridge is
   loopback-only or custom-scheme — a private conduit, never a service.
2. **No Python at runtime.** Python exists only in dev tooling (`tools/`, `tests/eval/`).
3. **The two anchors** (`coeftable`, `CoefficientRow`) change only with direct User approval.
4. **The two wire contracts (`CONTRACT.md`, `CONTRACT-engine.md`) are versioned law.** Changes to them
   go through the contract process; both live in the internal planning tree.
5. **Qualifiers reach the engine intact or the product errors.** Never silently drop `if`/`in`/`by`.
6. **Serve output must stay byte-equal to one-shot output** for the stateless family.
7. **Existing outputs stay byte-unchanged unless the change is the point.** Freeze pins are the
   detector.
8. **Bugs are fixed as classes.** A fix ships with a one-pass sweep for the same pattern elsewhere.
9. **Mocks are pinned to an observed trace of the real boundary**, never inferred from our own
   calling code.
10. **Smallest change that satisfies the gate.** No speculative features, no unrequested
    flexibility, no abstraction for single-use code.

---

## 12 · Known traps (each one has already caused a real bug)

| Trap | Consequence |
|---|---|
| Headless `--do` has no rasterizer | `graph export …png` writes `.svg` + a note; PNG-only cleanup loops delete every graph (BUG-143) |
| `COBRA_BIN` unset | `resolveCobraBin()` falls back to the pin — you may silently measure an old engine |
| Quote-blind tokenizing | qualifiers inside string literals mis-parsed; use `eval::searchUnquoted` |
| `structure` vs `card_stable` | write-path and read-path questions; one boolean cannot answer both |
| `.dta` 117 vs 118 strL key widths | (u4,u4) vs (u2,u6); mismatch = silent empty strings (BUG-072) |
| Writing to a file you are reading | `use F` then `save F, replace` destroyed user data (BUG-138); write to a sibling temp and `rename(2)` |
| Cross-volume `clonefile` | silently degrades to a full rewrite; measure write performance on one volume |
| Braced args to by-ref `vector&` | `{y,x}` deep-copies per call — O(n²) in a hot loop |
| Value-pinning tests | prove the number, not that it was computed cheaply — pair with counter gates |
| A "floor" that is a serial loop | `sort`/`gen` row loops are 1-core by construction; that is a design limit, not physics |

---

## 13 · Current performance shape (measured 2026-07-29)

The full corpus run (`analysis.do`, 160 dependent variables) in a certified-clean window:
**1042.61 s wall / 17.38 min**, user 760.19 s, sys 245.58 s, max RSS 1.945 GB.

- **CPU/wall = 0.965** → the run is effectively **single-threaded**: ~1 of 8 cores.
- Against the Stata BE reference (33 min, one core) this is **1.90× faster** — the first measurement
  in which the product beats the incumbent on this corpus, and it does so per-core.
- Against the then-ratified target bar (6–8 min) it was **2.17–2.90× over**. Not met. **The bar
  was re-ratified 2026-08-13** (User): the 6–8 min figure was a multicore-era target whose premise
  (process-level parallelism, P9) is User-DORMANT; the current bar is **"무실패 15분" — a
  failure-free full-corpus single run, wall ≤ 900 s on an idle machine**, judged by four prongs
  (0 displayed error blocks · 640 estimation blocks · 47 graphs with 47 DISTINCT md5 and one run
  banner · the wall). Canonical text: internal PLAYBOOK-v2 ADDENDUM 9.
- Remaining serial inventory is small (allocation churn ≈ 7 % of wall); the unscoped
  row-loop vectorisation wave is the open question. Reaching the bar is understood to require
  **process-level parallelism**, where the binding constraint is memory (1.81 GiB per process on an
  8 GB machine), not cores.

---

## 14 · Provenance of this document — read before acting

**Directly verified** by reading the file or its header this session: repo tree; both `CMakeLists`
(targets, flags, LA backend, webview linkage); `src/` directory contents and file counts; the
estimator list; `apps/cmds/` contents; `piet/native/src` and `include/piet` file lists;
`cobra_session.h` (the serve protocol, both request families); `serve` sections of
`apps/cecon_cli.cpp`; `routes.h`; `main.cpp` mode flags; `shell.h` (bridge + threading);
`api.js` (bridge detection, `pathToRoutingKey`); `columnar.h`; both contract headers; the internal docs index;
`tests/` layout; `docker/`, `dist/`, `tools/`, `vendor/` listings.

**Taken from governing documents** rather than code: the four gates, the verification laws, the
versioning policy, lane ownership (CONSTITUTION v2, ROUTING, VERSIONING). The §13 numbers are this
session's measurements plus the campaign records cited there.

**NOT read line-by-line**: the bodies of the 40 PIET translation units and the 37 estimator files.
Their roles in §3.2 and §5 are from filenames, headers, and cross-references — reliable for
navigation, **not** a substitute for reading a file before you modify it. §7's trace is assembled
from the component contracts and is a faithful model of the flow, not a transcription of one call
stack.

**Two known documentation defects found while writing this**: (1) `CONTRACT-engine.md`'s title says
"Python kernel" — a 1.x relic; (2) `docs/` has no current whole-system architecture document —
`PROJECT_OVERVIEW.md` was archived as describing the superseded Python-backend design, and this file
is the replacement.

**Placement note.** This file sits in the non-git private coordination tree because the git `docs/` tree is the
docs lane's write scope and the Secretary does not write there. Landing it as `docs/ARCHITECTURE.md`
is a docs-lane task.
