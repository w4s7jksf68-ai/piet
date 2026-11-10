# Parallel maintenance

This directory holds the `parallel` block command, the worker-process machinery
behind it, and the automatic recognizer that decides when plain do-file statements
may run as independent tasks. Start with the symptom below; the block entry point is
`parallel_block.cpp`, everything it calls is in `parallel.cpp`.

| Command or symptom | Owner |
|---|---|
| `parallel[, workers(N)] { ... }` block: banner, serial leg, `_rc` and `r(n_failed)`, replay of worker output in source order, `exit` promotion | `parallel_block.cpp` |
| Unknown or misspelt `parallel` option (only `workers()` is accepted; anything else is refused) | `parseHeader` in `parallel.cpp` |
| Wrong number of workers, clamp reason in the banner | `deriveDefaultWorkers` and `resolveWorkers` in `parallel.cpp` |
| Body split into tasks (brace-depth scan) | `decomposeTasks` in `parallel.cpp` |
| Task spec contents, `PIET_PARALLEL_DUMP_SPEC` dump | `buildTaskSpec` and `dumpTaskSpecIfRequested` in `parallel.cpp` |
| `--parallel-worker` child: inherited state, task execution, result object | `runParallelWorker` in `parallel.cpp` |
| Process spawn, pull queue, per-task scratch reaping, worker environment | `runTasksInWorkers` in `parallel.cpp` |
| Automatic mode switch (`PIET_PARALLEL_AUTO`), region approval or refusal, stderr report lines | `parallel_auto.cpp` |
| Which command may do what (effect table), statement and program summaries | `parallel_effects.cpp` |
| Two tasks writing the same file, path templates through program arguments | `parallel_paths.cpp` |

`parallel.cpp` also owns the budget inputs: hardware and disk probes
(`perfCoreCount`), `scratchFreeBytes`, `residentBytesEstimate` and the memory-pressure
read. The recognizer runs only when `PIET_PARALLEL_AUTO` is set and never inside a
worker (`PIET_PARALLEL_WORKER`). Its predicate is one-sided: anything it cannot
prove independent refuses, so a missing row in the effect table in
`parallel_effects.cpp` is slowness, never wrong output. Add a command there only after
checking what it reads and writes; absence means unknown.

There are no private headers here. The four public headers `piet/parallel.h`,
`piet/parallel_auto.h`, `piet/parallel_effects.h` and `piet/parallel_paths.h` live in
`piet/native/include/piet/` and declare each `.cpp` of the same name (`parallel.h` for
`parallel.cpp`). `parallel_block.cpp` is the exception in two ways: it defines
`runParallelBlock` in namespace `piet::loops::loops_detail`, declared in
`language/loops_internal.h`, and the block dispatch in `language/loops.cpp` calls it.
Do not include `language/loops_internal.h` from anywhere else in this directory.

State and lifetimes. `blockSeq` (a static counter in `parallel_block.cpp`) numbers the
per-block scratch directories for the whole process. `tempSeq` in `parallel_paths.cpp`
mints distinct `tempfile` path symbols. The effect table is a function-local static
built once. The program summary cache and its recursion guard in `summarizeProgram`
(`parallel_effects.cpp`) are thread-local and keyed on `SessionState::programsEpoch`,
so a changed program table can never be served a stale summary; do not key it on
anything else. The parent executes nothing while workers run; the resident head is
snapshotted through `datastore()` and passed to children by path. A worker is a whole
process: counters it accumulates come back as a delta and only the additive set is
folded into the parent. Spawning is POSIX only; elsewhere `runTasksInWorkers`
returns empty and the block runs serially.

Compile flags: none of these files is on the `-ffp-contract=off` list in
`piet/native/CMakeLists.txt` (that list covers numpy-parity or StataBE-parity
arithmetic), and none carries a `#pragma clang fp contract(off)`. All five files are
built into the shared `piet_core` object library.

Selftests: `selftest_parallel_execution.cpp` (`testP9AutoRecognizer`,
`testP9ParallelBlock`, `testAerK1cParallel`) covers the recognizer, block and worker
execution; see `../SELFTESTS.md`. A fan-out started from the selftest binary executes
the sibling product binary, because only that one knows `--parallel-worker`.

Elsewhere: the `parallel` keyword routing and `loops_detail` helpers (`runBody`,
`pushLoopBlock`, `errBlock`) are in `language/loops.cpp`; the do-file loop that calls
`planRegion` and applies a plan is `language/doexec.cpp`; the `--parallel-worker`
flag is handled in `app/main.cpp`; the additive counters folded after a worker
finishes are in `piet/counters.h`; the command registry the effect table is keyed
against is `dispatch/registry.cpp`.
