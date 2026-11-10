# PIET + CoBrA — System Architecture

**Audience: an AI agent that must modify this system correctly without reading the whole tree.**

## Maintenance entrypoint — 2026-11-10

This map reflects the current layout of `piet/native/src`: sources live in domain
directories, each directory has a `README.md` that maps its files, and only the selftest
files remain directly in `piet/native/src`. It is a navigation map, not a new
numerical-accuracy or performance certificate. The numbered sections after the separator
retain their **2026-07-29 snapshot**; recheck historical versions, paths, target counts
and performance claims before using them.

Start with the symptom below and its existing behavioral checks. The repository stays
one build and one product. Functions moved without redesigning command semantics;
each file's floating-point compile flag moved with it. Paths in the table are relative
to `piet/native/src` unless they start with `piet/` or `include/`. In a frontend row,
bare `.js` file names after the first full path are relative to `piet/frontend/js`.

| Symptom | Implementation to read first | Relevant state and existing checks |
|---|---|---|
| You do not yet know which file owns the behavior | The `README.md` of the matching directory (see "Source tree" below): a command-or-symptom to owner table, private headers, state, compile flags and selftests | `SELFTESTS.md` maps every selftest file to its feature and entry functions; public declarations are in `include/piet/handlers/*.h` |
| A prefix, macro, qualifier, option abbreviation, `_rc`, or output order behaves incorrectly | `dispatch/dispatcher.cpp` shows the ordered pipeline and outer result/log lifecycle; `dispatch/stage_*.cpp` owns each step; `dispatch/route_*.cpp` owns command families | `dispatch/registry.cpp` is the command-name/abbreviation/qualifier authority; `dispatch/options_support.cpp` holds the shared option vocabularies, and per-directory option files and command files register their own through `registerOptVocab`; `dispatch/*_support.h` exposes helpers by owner. Read `dispatch/README.md`; `selftest_command_contracts.cpp`, `selftest_command_errors.cpp`; full native selftest, freeze, pangram and structural floor gates cover integration |
| The app does not start, a bridge route is unknown, settings or scratch location are wrong | `app/main.cpp` (command-line modes), `app/bridge_route_bindings.cpp`, `app/routes.cpp`, `app/settings.cpp`, `app/startup_environment.cpp` | `AppState` owns the one `SessionState`; `app/README.md`; `selftest_bridge_routes.cpp`, `selftest_bridge_transport.cpp`, `selftest_settings_and_execution.cpp`, `selftest_startup_and_paths.cpp` |
| The wrong engine binary runs, a worker dies, or an `.hbin` read is wrong | `engine/cobra_runner.cpp`, `engine/cobra_session.cpp`, `engine/datastore.cpp`, `engine/storage_type.cpp` | Resident frame and dirty bit belong to `SessionState`; `engine/README.md`; `selftest_data_storage.cpp`, `selftest_variable_storage_types.cpp` |
| `esttab`, `estout`, `outreg2` or a table file has wrong cells, escaping or spacing | `tables/esttab_block.cpp`, `tables/table_esttab.cpp`, `tables/table_estout.cpp`, `tables/table_outreg.cpp`; estout grammar/model/format helpers in `tables/estout_*.cpp`; shared prepared-table rendering in `tables/table_render.cpp` and `include/piet/table_{model,render}.h` | `tables/table_model_context.cpp`, `tables/table_number_format.cpp`, `tables/table_files.cpp`; `--suite tables`, `selftest_tables.cpp`, `selftest_tables_esttab_namelists.cpp` |
| Added estimate results or exported datasets/matrices are wrong | `tables/table_estadd.cpp`, `tables/table_estpost.cpp`, `tables/table_dataset_export.cpp`, `tables/table_matrix_export.cpp`, `tables/table_tex_files.cpp`, `tables/table_erepost.cpp` | Active/stored fits and live `e()` mirrors; same table suite plus full selftest |
| `return list`, `ereturn`, `scalar list`, `eststo` or `estimates save/use` is wrong | `results/return_list.cpp`, `results/ereturn_commands.cpp`, `results/scalar_results.cpp`, `results/stored_estimates_commands.cpp`, `results/estimates_save_use.cpp` | e() stores on the session evaluator, `session.estimatesModels`; `results/README.md`; `selftest_ereturn_*.cpp`, `selftest_stored_estimates*.cpp`, `--suite stored-estimates` |
| Macro expansion, definitions, numeric text or matrix references are wrong | `eval/macros.cpp` coordinates expansion; `eval/macro_{definitions,scanners,text,expressions,results,matrices}.cpp` own the specific work | `eval/README.md`; `selftest_language_macro_*.cpp`, `selftest_language_values.cpp`, `selftest_unicode_names_macros.cpp` |
| A do-file, loop, program, `syntax`, `display` or `tempvar` behaves incorrectly | `language/doexec.cpp`, `language/loops.cpp`, `language/program.cpp`, `language/program_syntax.cpp`, `language/display_command.cpp`, `language/scalar_expression.cpp`, `language/temporary_names.cpp` | Loop-body depth and minted names belong to `SessionState`; `language/README.md`; `selftest_language_control*.cpp`, `selftest_language_loops.cpp`, `selftest_language_do_include_scope.cpp` |
| A `matrix` command, matrix expression, `svmat`/`mkmat` or the Mata subset is wrong | `matrix/matrix_commands.cpp`, `matrix/matrix_expression.cpp`, `matrix/matrix_to_variables.cpp`, `matrix/variables_to_matrix.cpp`, `matrix/mata_subset.cpp` | `session.eval.matrices` is written in three places that must stay identical; `matrix/README.md`; `selftest_matrix_*.cpp`, `selftest_mata_subset_getmata.cpp` |
| Graph blocks, histogram/density, SVG export or live-view thinning are wrong | `graph/commands.cpp`, `graph/distribution_commands.cpp`, `graph/twoway.cpp`, `graph/svg.cpp`, `graph/files.cpp`, `graph/graph_export_formats.cpp`, `graph/view.cpp` | Graphs and sersets in `SessionState`; `graph/README.md`; `selftest_graphics*.cpp` and exact SVG comparisons |
| Uniqueness, missingness, labels, file lists, spells, rolling statistics or calendars are wrong | Named command files under `utility/`; `utility/utility_support.cpp` owns shared column/read/write helpers | `utility/README.md`; `selftest_data_commands.cpp`, `selftest_calendars.cpp`, `selftest_command_contracts.cpp` |
| A model fit has the wrong options or engine request | Named files under `estimation/`, for example `binary_estimation.cpp`, `panel_estimation.cpp`, `instrumental_variables.cpp`, `count_estimation.cpp`; shared `estimation/estimation_support.cpp`, `estimation_results.cpp`, `estimation_options.cpp` | `estimation/estimation_internal.h` and family-specific internal headers; `estimation/README.md`; `selftest_estimation*.cpp`, `selftest_iv_*.cpp`, `selftest_*_estimation.cpp` |
| `bootstrap`, `jackknife`, `permute`, `rolling` or `statsby` prefix is wrong | `estimation/bootstrap_prefix.cpp`, `estimation/jackknife_permute_prefix.cpp`, `estimation/rolling_prefix.cpp`, `estimation/statsby_prefix.cpp`, `estimation/resampling_prefix_support.cpp`; the line is recognized in `dispatch/stage_resampling.cpp` | Session random stream (`estimation/bsample_draws.h`); `selftest_bootstrap_prefix.cpp` |
| `predict`, `margins`, `test`, `lincom`, `nlcom`, or `estat` uses the wrong fit/sample | `postestimation/prediction.cpp`, `postestimation/margins.cpp`, `postestimation/hypothesis_tests.cpp`, `postestimation/estimation_diagnostics.cpp`, `postestimation/postestimation_context.cpp` | `LastModel`, fit restrictions/weights, `e(sample)`, live `e()`/`r()`; `postestimation/README.md`; `selftest_postestimation*.cpp`, `selftest_model_diagnostics.cpp` |
| `merge`, `append`, `joinby` or `reshape` loses rows, types or labels | `datamgmt/data_merge.cpp`, `datamgmt/data_append.cpp`, `datamgmt/data_joinby.cpp`, `datamgmt/data_reshape.cpp`; `datamgmt/using_dataset.h` reads the `using` file | Dataset keys/order, metadata and resident/materialized mutation results; `datamgmt/data_combine_internal.h`; `selftest_data_combine.cpp`, `selftest_data_combine_varlist_options.cpp` |
| A data command changes the wrong variables/rows | `datamgmt/variable_copy_rename.cpp`, `data_collapse.cpp`, `data_order.cpp`, `data_duplicates.cpp`, `data_recode.cpp`, `data_observation_expansion.cpp`, `data_quantiles.cpp`, `data_interpolation.cpp`; shape verbs (`split`, `xpose`, `stack`, ...) in their own `datamgmt/` files | Shared writer rules in `datamgmt/data_mutation_internal.h` and `datamgmt/datamgmt_support.h`; `datamgmt/README.md`; `selftest_data_commands.cpp`, `selftest_data_storage.cpp`, `selftest_data_rename_recode_encode.cpp` |
| `generate`, `replace`, `egen` or a row expression gives the wrong values | `datamgmt/data_generate.cpp`, `datamgmt/string_generate_replace.cpp`, `datamgmt/row_evaluation.cpp`, `datamgmt/row_conditions.cpp`, `datamgmt/egen.cpp` | Hidden qualifier columns, storage types; `selftest_data_egen_statistics.cpp`, `selftest_data_string_functions.cpp`, `selftest_variable_storage_types.cpp` |
| Save, CSV, spreadsheet or string conversion is wrong | `fileio/dataset_save.cpp`, `fileio/delimited_import.cpp`, `fileio/delimited_export.cpp`, `fileio/spreadsheet_import.cpp`, `fileio/spreadsheet_export.cpp`; `datamgmt/destring.cpp`, `tostring.cpp`, `encode.cpp`, `decode.cpp` | Native read/write services, metadata and file overwrite rules; `fileio/README.md`; `selftest_file_io.cpp`, `selftest_spreadsheets.cpp`; `--suite data-conversion` |
| `cd`, `clear`, `log`, `timer`, `shell`, `mkdir` or `ssc` behaves incorrectly | Command families under `environment/` (for example `workspace_commands.cpp`, `logging_commands.cpp`, `shell_translation.cpp`) | State lives in `SessionState`; `environment/README.md`; `selftest_file_io.cpp`, `selftest_settings_and_execution.cpp` |
| `describe`, `summarize`, `tabulate`, `count` or `list` output is wrong | `descriptive/describe.cpp`, `descriptive/summarize.cpp`, `descriptive/tabulate.cpp`, `descriptive/data_listing.cpp`, `descriptive/descriptive_statistics.cpp` | Read-only except `r()`; `descriptive/README.md`; `selftest_descriptive_statistics.cpp` |
| Stored estimates restore the wrong model or added results | `session/session_estimates.cpp`, `session/session_fit_state.cpp`; the commands are in `results/stored_estimates_commands.cpp` | Snapshot/model pins, `e(sample)`, live `e()` and added mirrors; `--suite stored-estimates` |
| Dataset replacement, scratch ownership or a resident card is stale | `session/session_dataset.cpp`, `session/session_resident_head.cpp`, `session/session_resident_card*.cpp`, `session/session_scratch.cpp`; `session/session_memory.cpp` owns memory accounting | `load`, `datastore`, materialization and pin/reclaim lifetimes; `session/README.md`; `selftest_bridge_routes.cpp`, `selftest_data_storage.cpp` |
| Time/panel declarations disappear or display incorrectly | `time/time_declarations.cpp`, `session/session_time_metadata.cpp`, `time/time_panel_state.cpp`, `time/time_fill.cpp` | Panel/time metadata, formats, characteristics and `r()`; `time/README.md`; `--suite time-declarations` |
| A `parallel` block or automatic parallel region misbehaves | `parallel/parallel_block.cpp`, `parallel/parallel.cpp`, `parallel/parallel_auto.cpp`, `parallel/parallel_effects.cpp` | Recognizer refuses whatever it cannot prove independent; `parallel/README.md`; `selftest_parallel_execution.cpp` |
| Editor contents, file reopen, command execution or cancellation is wrong | `piet/frontend/js/editor.js`, `execution.js`, `command-input.js`, `run-progress.js`, `request-dialog.js`, `recent-files.js` | Controllers own feature state; `app.js` composes narrow callbacks; `piet/frontend/js/README.md`; DOM suite and actual native UI smoke |
| Split panes, settings or window restoration is wrong | `piet/frontend/js/layout.js`, `settings.js`, `app-navigation.js`; existing `panes.js` and `piet/frontend/css/piet.css` | Local storage/native `ui_state`, DOM order and deferred controller callbacks; DOM suite and actual native UI smoke |

### Source tree

Code files are `.cpp`, `.h` and `.inc` files directly in the directory.

| Directory under `piet/native/src` | Responsibility | Code files |
|---|---|---:|
| `app/` | Process entry point, command-line modes, webview shell, bridge routes, settings, headless and UI-smoke runners | 18 |
| `dispatch/` | Ordered pipeline from a typed line to result blocks: stages, per-family routes, command registry, option vocabularies | 54 |
| `eval/` | Tokenizer, macro expansion, expression evaluator and function library, error payloads | 25 |
| `language/` | Do-file execution, loops, programs and `syntax`, `display`, `c()`, temporary names, small program commands | 23 |
| `session/` | Out-of-line members of `SessionState`: dataset lifecycle, resident head and card, scratch, logs, fits | 20 |
| `engine/` | Engine process and worker client, `.hbin` reader, engine arguments, storage types, memory pre-flight | 13 |
| `datamgmt/` | Commands that create, change, combine, reshape, convert and label data; frames; `preserve`/`restore` | 81 |
| `descriptive/` | Read-only reports: describe, summarize, tabulate, listings, comparisons, `centile` | 13 |
| `fileio/` | `use`/`save`, delimited, fixed-format, spreadsheet and ZIP files, `file`/`postfile`, typed paths | 27 |
| `environment/` | Working directory, `clear`, logs, timers, filesystem commands, `shell`, ado paths and packages | 12 |
| `estimation/` | Model fits, shared estimation support, resampling prefixes, `e(sample)` install | 66 |
| `postestimation/` | `predict`, `margins`, tests, `estat`, `boottest`, `lrtest`, `suest`, `r(table)` | 14 |
| `results/` | `return list`, `ereturn`, `scalar list`, stored estimates and `.ster` files | 7 |
| `tables/` | `esttab`, `estout`, `outreg2`, `estadd`, `estpost`, `estimates table`, table file exporters | 26 |
| `matrix/` | Matrix commands and expressions, matrix/variable conversion, Mata subset, LAPACK eigen translation | 12 |
| `graph/` | Graph blocks, twoway layers, distribution plots, sersets, SVG and export formats | 38 |
| `time/` | `tsset`/`xtset`, `tsfill`, Hodrick-Prescott filter, `sctorezone`, business-calendar library | 6 |
| `utility/` | Community data utilities (`unique`, `missings`, `rangestat`, `filelist`, `bcal` command, `geodist`) | 14 |
| `parallel/` | `parallel` block, worker processes, automatic recognizer | 5 |
| `generated/` | Generated option-minimum table included by `dispatch/options_support.cpp`; not edited by hand | 1 |
| top level | Selftest runner, shared helpers, declarations and the 99 case files indexed in `SELFTESTS.md` | 106 |

Public declarations are under `piet/native/include/piet/`. Command entry points are
declared by domain in `include/piet/handlers/*.h`; `piet/handlers.h` includes all of them.

### Boundaries that matter

`dispatch/dispatcher.cpp` is orchestration. Preserve the order of its preprocessing
stages, its recursion and the lifetime of `RcHiddenScope` / `QualifierColumnGuard`
(defined in `dispatch/types.h`). Routes run only after those common steps. Keep command
names and abbreviations in `dispatch/registry.cpp`; a route family is an implementation
grouping, not another command registry. A new ordinary command needs its implementation
file in the matching domain directory, a registry row, an arm in the matching
`dispatch/route_*.cpp` and a row in that directory's `README.md`.

Prepared-table rendering takes a table and settings and returns text. It must not fetch
a model, mutate a session or write a file. Command-specific esttab/estout/outreg writers
retain their established formatting paths: follow the actual call before changing a
shared renderer. `tables/tables_internal.h` contains implementation-private types and
helper declarations; helpers have one definition in their named source file.

`SessionState` remains the owner of the same fields and resource lifetimes. Shared value
types live in `include/piet/session_types.h`; `include/piet/session_state.h` exposes
declarations, layout and small accessors. Constructor wiring, dataset transitions,
resident head and card, scratch, live-fit posting, metadata/time metadata, logs and
memory accounting have separate files under `session/`. Splitting the implementation
did not turn session fields into separately copied state objects.

Includes: `piet/native/src` is on the include path. A header in the same directory is
included by bare name; a header in another directory by its path from `src`
(for example `dispatch/registry.h`); never with `../`. Private `*_internal.h` and
`*_support.h` headers belong to their directory or named family; each directory README
lists the few that are included from elsewhere. Do not add catch-all files: put new code
in the file that owns the command, or in a new file named for what it does.

Floating-point flags: the `-ffp-contract=off` list in `piet/native/CMakeLists.txt` and
the in-file `#pragma clang fp contract(off)` sites are part of the numerical behavior. A
file keeps its flag when it is moved or split, and a function moved out of a listed file
must go to a file with the same flag. Each directory README names its listed files.

### Build and focused checks

The application and selftest link the same `piet_core` objects. Test cases are excluded
from the application. `selftest.cpp` selects suites and calls the established full run;
219 embedded cases live in 29 files declared in `selftest_cases.h`; 78 feature entry
functions live in 70 feature files, 76 declared in `selftest_feature_cases.h` and two
(`testW2yDatetime`, `testW2eFloat`) declared at their call in `selftest.cpp`.
`SELFTESTS.md` indexes all 99 case files. Shared counters and fixture helpers have one
definition in `selftest_support.cpp`. The full run's call order is preserved, including
its final float-store fallback check. `selftest.cpp` must keep its name and directory:
cases locate the repository root and fixtures through its `__FILE__`
(`selftestSourceFile()`).

For a public checkout, use `sh scripts/build-macos.sh` and
`python3 tests/ui_smoke.py` on macOS; the build script runs the public engine and
application smoke checks. The Windows counterpart is `scripts/build-windows.sh`. (In the maintainer checkout these
files are under `public/`; the export places them at the root.)
The full maintainer selftest below depends on additional fixtures that are not
included in the public source distribution. A missing fixture is not a passing test.

For a maintainer checkout, from the repository root:

```sh
python3 tools/lint_source_layout.py
cmake --build piet/native/build -j4
piet/native/build/piet_selftest --list-suites
COBRA_BIN="$PWD/build/cobra" piet/native/build/piet_selftest --suite tables
COBRA_BIN="$PWD/build/cobra" piet/native/build/piet_selftest --suite data-conversion
COBRA_BIN="$PWD/build/cobra" piet/native/build/piet_selftest --suite stored-estimates
COBRA_BIN="$PWD/build/cobra" piet/native/build/piet_selftest --suite time-declarations
COBRA_BIN="$PWD/build/cobra" piet/native/build/piet_native --selftest
node piet/frontend/test/dom_runner.mjs
```

`tools/lint_source_layout.py` is the layout check: it fails when a non-selftest C++
file sits directly in `piet/native/src`, a source is missing from or listed twice in the
CMake build lists, a `-ffp-contract=off` path does not exist, two code files share a
basename, a file or directory name carries a development-history token or shadows an
engine include directory, a directory README omits one of its code files or names a
missing one, or a quoted include uses `../`.

Selections reuse existing test functions and assertions; they do not replace integration
checks. `--suite all` selects the unchanged full run. The nine entry points in
`selftest_unimplemented_scaffolds.cpp` have empty bodies; a matching name alone does not
prove coverage. Source-path checks must follow every relocated writer, keep their
original expected counts and assert that every file was read.

For a structural change compare output bytes, errors, state effects, registry and option
vocabulary before/after. Run the applicable `tools/merge_gate.sh` checks on the final
candidate. UI changes also need the native `--ui-smoke` journey; DOM tests alone do not
establish webview geometry, the native bridge or restoration across a restart. Keep
oracle data, tolerances, graders and command schemas unchanged.

### What stays whole

There is no line limit. These large files were deliberately left as one file because
each holds one command, one grammar or one algorithm, and a split would separate code
that shares one set of rules (sizes from `wc -l`):

- `matrix/lapack_dsyevr.cpp` (3,933 lines): a translation of one LAPACK call tree under
  one adopted-code notice. It is on the `-ffp-contract=off` list, and the fused
  multiply-adds of the reference build are written as explicit `std::fma` at the same
  sites; any piece split off would have to carry both rules.
- `eval/exprfns.cpp` (3,047): the Stata function library. `regexs()` reads the memo of
  the last `regexm()`, the regex functions share one process-wide compiled-regex cache,
  and the running-sum stack is a thread-local in the same file.
- `postestimation/margins.cpp` (2,742) and `postestimation/prediction.cpp` (2,326):
  one command each with many option paths; both are on the `-ffp-contract=off` list, so
  any piece split off must carry the flag.
- `matrix/matrix_expression.cpp` (2,384): the matrix expression grammar, `matrix list`
  rendering and `storeMatrixResult`; `matrix/matrix_expression_internal.h` is its only
  seam, to `matrix/matrix_commands.cpp`.
- `dispatch/registry.cpp` (2,259): the single table of command names, abbreviations,
  qualifier allow-lists and community names. One table is the authority the tokenizer,
  the by-prefix and the contract selftests read.
- `estimation/qreg2_estimation.cpp` (2,278) and `estimation/lasso_estimation.cpp`
  (2,119): each carries code derived from one community package (`qreg2`; lassopack's
  `rlasso` path). The adopted-code register in
  `piet/packaging/licenses/40-ADOPTED-CODE.txt` names the file that carries the derived
  code, so each port stays in one file under one notice.
- Engine `src/data/frame*.cpp`, the LAPACK translations and estimator kernels remain in
  their numerical/data-plane layer. Splitting a cohesive algorithm only to reduce line
  count would not narrow a normal command/UI maintenance task.

These boundaries do not claim that every remaining function is short or that all design
debt is gone. For normal maintenance, use the symptom map and the directory README
before widening the read scope.

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
