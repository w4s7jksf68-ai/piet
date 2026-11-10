# Session maintenance

This directory holds the out-of-line member functions of `SessionState`. The class
declaration, every data member and the inline accessors are in
`piet/session_state.h` (under `include/`); open it for the field contract, then the
file below that owns the behaviour. Command handlers live in other directories.

| Command or symptom | Owner |
|---|---|
| Constructor, evaluator callbacks wired to the session (`_N`, `runiform` seed, `c()`, characteristics, `e()` visibility) | `session_lifecycle.cpp` |
| Business-calendar cache reset at construction; `clear all` teardown of frames, graphs, filehandles, estimates, estadd bag | `session_state.cpp` |
| `load`, the lazily opened `datastore()`, resident-token release, head-backed audit, `clearData` | `session_dataset.cpp` |
| Value-label set order, browse label map, `clearMetadata`, per-variable plane erase | `session_metadata.cpp` |
| Display formats, variable labels, `.hbin` metadata carriage, label projection into the macro layer | `session_variable_metadata.cpp` |
| `tsset`/`xtset` declaration as `_dta` characteristics, checkpoint declarations, `%21x` hex text | `session_time_metadata.cpp` |
| `estimates store/restore/drop`, namelist expansion, `estadd` bag fingerprint and mirrors | `session_estimates.cpp` |
| Installing and resetting the last model, `e(V)` attach, posted `e()` supersession | `session_fit_state.cpp` |
| `e(sample)` reading and its refusals, observation-1 value | `session_estimation_sample.cpp` |
| `VAR[i]`, `c(name)` and `ereturn list` name-order answers for the evaluator | `session_eval_hooks.cpp` |
| Choosing resident frame run, stateless run or one-shot; worker creation; failure rollback | `session_engine_routing.cpp` |
| Worker sync, `preserve`/`restore` stash, virtual save/use, writing a virtual head to disk | `session_resident_head.cpp` |
| Variable card after a mutation: seeding side (missing counts, shadow card, worker varlist) | `session_resident_card.cpp` |
| Variable card after a mutation: fold side (plan capture, application, completeness guard) | `session_resident_card_fold.cpp` |
| Scratch-file ownership, reclamation, live-session registry, destructor, orphan sweep, census | `session_scratch.cpp` |
| `use` conversion and stash memos and their byte cap | `session_use_cache.cpp` |
| `log using/close/off/on`, log queries, text and SMCL transcript tee | `session_logs.cpp` |
| Progress ticks, cancel, user-input round trip, command history, do-worker thread flag | `session_run_control.cpp` |
| Live named-memory byte count used by diagnostics | `session_memory.cpp` |

`session_resident_internal.h` is the only private header. It declares two cached
environment reads (`serveDisabled`, `routelog_path`) defined once in
`session_engine_routing.cpp`; only that file, `session_resident_head.cpp` and
`session_resident_card.cpp` include it. Add a second definition elsewhere and the
once-per-process cache is split in two.

State a change must respect: one `SessionState` owns its resident token, dirty flag,
shadow card, pending delta and `use` memos, and these move together. Clearing the
token without the shadow card, or writing a pending delta without capturing its
card plan, leaves the head unbacked or the card stale. Scratch files are deleted only
when they lie inside the session work directory and are not referenced by a live head,
frame, preserve twin or memo (the orphan sweep also spares a sibling session's files); `session_scratch.cpp` keeps one
process-wide list of live sessions behind a mutex, and its orphan sweep reads it.
`session_run_control.cpp` holds the one `thread_local` do-worker flag and a mutex
for pending user-input requests. The environment switches (`PIET_NO_SERVE`,
`PIET_ROUTELOG`, `PIET_NO_SCRATCH_GC`, `PIET_USE_CACHE_MB`, `PIET_FOLDTRACE`) are
read in the file named in the table row for that area.

No file in this directory is on the `-ffp-contract=off` list in
`piet/native/CMakeLists.txt` and none carries a `#pragma clang fp contract(off)`;
that list covers numerical translations elsewhere, so the flag does not apply here.
The `session_*.cpp` files are listed individually in the `CMakeLists.txt` source list.

Selftest coverage is by feature, not by file: `selftest_data_storage.cpp` (virtual
head, scratch reclaim, orphan sweep, hbin metadata), `selftest_stored_estimates.cpp`,
`selftest_stored_estimates_esample.cpp` and `selftest_ereturn_commands.cpp`
(estimates, `e(sample)`, posted `e()`), `selftest_file_io.cpp` (`log using`),
`selftest_display_format_defaults.cpp`, `selftest_label_metadata.cpp`,
`selftest_time_declaration_persistence.cpp` and `selftest_bridge_transport.cpp`
(progress stream). The memory count and `use` memo cap have no dedicated file.

Not here: the `SessionState` class declaration (`piet/session_state.h`), the worker
client and one-shot engine runner (`engine/cobra_session.cpp`,
`engine/cobra_runner.cpp`), `mutationResult` (`engine/mutation_result.cpp`), the
`preserve` and `restore` commands (`datamgmt/preserve_restore.cpp`) and label
commands (`datamgmt/label_metadata.cpp`).
