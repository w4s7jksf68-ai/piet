# Engine client maintenance

This directory is the PIET side of the cobra engine: how the app starts the engine, talks to it, reads the `.hbin` files it writes, and shapes the argument lists and results around those calls. It holds no command semantics. Start with `cobra_runner.cpp` for "which binary, how is it started" and `cobra_session.cpp` for "what is sent to a long-lived worker"; a command-specific symptom belongs to the command's own directory. Public declarations live in `piet/cobra_runner.h`, `piet/cobra_session.h`, `piet/datastore.h`, `piet/column_fetch.h`, `piet/inrange_filter.h`, `piet/mutation_result.h`, `piet/storage_type.h`, `piet/translate.h` and `piet/preflight.h`.

| Command or symptom | Owner |
|---|---|
| Wrong engine binary chosen (`COBRA_BIN`, bundled copy, copy beside the executable, `$PATH`) | `cobra_runner.cpp` |
| One-shot engine process: spawn, pipes, stdout/stderr drain, exit code, Windows argument quoting | `cobra_runner.cpp` |
| `runCobra` routing to the active session's worker; `runCobraJson`; `CobraError` text (`[code] message`) | `cobra_runner.cpp` |
| Long-lived `cobra serve` worker: spawn, request/response lines, respawn after a death, fall back to one-shot | `cobra_session.cpp` |
| Resident-frame requests (`frameUse`, `frameRun`, `frameSnapshot`, stash, virtual save) and the frame metadata they return | `cobra_session.cpp` |
| Engine request timing, `PIET_TRACE_ENGINE` trace, engine-settings epoch | `cobra_session.cpp` |
| Opening an `.hbin`, variable cards, missing counts, `browse` cells, factor levels | `datastore.cpp` |
| Reading only the columns an expression names (`getcols` projection) | `column_fetch.cpp` |
| `in a/b` bounds parsing, and `if` plus `in` filtering through the engine; per-group row slices | `inrange_filter.cpp` |
| `varlist_after` envelope returned by a data-changing command (`mutationResult`) | `mutation_result.cpp` |
| Storage type of a generated or replaced column (`--store-type`, promotion, type claim on rename/clone) | `storage_type.cpp` |
| Time-series operator detection, `--xtset`/`--tsset`/`--tsdelta` and RNG state added to engine arguments; RNG draws and store overflow read back | `translate.cpp` |
| Memory pre-flight before merge, joinby, append, reshape, expand, cross | `preflight.cpp` |
| Error blocks, scratch-path minting, varlist-card lookup shared by environment, matrix, results and language commands | `command_support.cpp`, `command_support.h` |
| The same error/path forwarders for the estimation, file-reading and data-generation handlers | `engine_call_internal.h` |
| Temporary helper columns for `if`, `in` and expression evaluation, dropped on every exit | `scratch_columns.h` |

`engine_call_internal.h` is a private header (namespace `engine_call_detail`) for the handlers that call the engine directly, such as `dataset_use.cpp`, `linear_estimation.cpp`, `estimation_sample_install.cpp`, `estimation_sample_carry.cpp`, `mean_proportion_estimation.cpp`, `data_generate.cpp`, `summarize.cpp` and `assert_command.cpp`. `command_support.h` is a private header (namespace `command_detail`) used by many command files outside this directory; it declares only the functions defined in `command_support.cpp`. Both wrap the same shared pieces (`eval::engineError`, `mintScratchPath`, `cobraErrorMessage`), so a change to the error text belongs in `cobraErrorMessage`, not in either wrapper. `scratch_columns.h` is included, as `engine/scratch_columns.h`, by data-management files (directly or through `datamgmt/data_io_internal.h`) and `descriptive/variable_compare.cpp`; only `cumul` and `xtile` use `ScratchColumns`; `ScratchColumns` restores `c(changed)` unless the caller says the data changed. Do not include any of these three from `piet/` public headers.

State and lifetimes a change must respect. The active-session stack in `cobra_runner.cpp` is `thread_local`: the UI thread and the do-file worker each have their own, and it nests under recursive dispatch. `CobraSession` owns only the worker process and the wire protocol; which file the resident frame corresponds to, the dirty bit and the shadow varlist card belong to `SessionState`. Every request takes the session mutex, retries once on a fresh worker, and after two consecutive deaths marks serve unavailable for the rest of the session. The engine-settings epoch, the post-estimation hit counter (`g_postestHits`) and the unrecorded-replace fallback count in `storage_type.cpp` are single program-wide atomics; selftests read them. `Datastore` caches its variable cards and counts full missing-value scans; `PIET_NO_NMISS_CARRY` and `PIET_VERIFY_NMISS` are read on every call on purpose. Scratch file names in `column_fetch.cpp` and `inrange_filter.cpp` come from process-wide counters and must stay unique per call, because a projection or slice can still be mapped when the next one is made. `mutationResult` must run its `seedNMissForPureAdd` step before the shadow card is reseeded, and the three name lists (new, dropped, rewritten) are not interchangeable.

Compile flags. Only `command_support.cpp` in this directory is on the `-ffp-contract=off` list in `piet/native/CMakeLists.txt`; the CMake comment gives the reason as `matrix` expression arithmetic that must round every product and sum separately to match the reference engine (including `inv()` on a singular matrix). It keeps that flag if it is split or moved. No file here carries an in-file `#pragma clang fp contract(off)`. The other `.cpp` files in the directory build with default flags.

Selftests. `selftest_data_storage.cpp` covers the resident frame, virtual head, scratch handling and hbin metadata; `selftest_variable_storage_types.cpp` covers storage-type resolution and the fallback count; `selftest_settings_and_execution.cpp` covers the memory pre-flight; `selftest_language_in_range_qualifier.cpp` and `selftest_data_commands.cpp` cover `in` ranges; `selftest_time_series_estimation.cpp` covers time-series operator injection; `selftest_bridge_routes.cpp` covers column projection through the bridge routes. See `piet/native/src/SELFTESTS.md` for the full index.

Not here. The engine itself (the `cobra` sources), the session state that owns the resident frame (`piet/session_state.h`), command parsing and routing (`dispatch/`), and the command implementations that call these helpers live elsewhere.
