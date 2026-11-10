# Command dispatch maintenance

This directory turns one typed Stata line into result blocks: it holds the ordered
pipeline, its stages, the per-family routes that call command handlers, and the command
registry. Start with `dispatcher.cpp`: `dispatchImpl` lists every stage and route in the
order they run, and `dispatch` (same file) is the outer wrapper. Then open the one stage
or route named by the symptom.

| Command or symptom | Owner |
|---|---|
| Order of stages, outer `_rc`/`r()`/`s()` residue per command, log tee, nesting depth | `dispatcher.cpp` |
| Stage and route signatures; the entry declaration `dispatchImpl` | `pipeline.h` |
| `if`/`while`/`foreach` brace blocks, `input` data block, `mata`, `parallel:` prefix | `stage_raw_blocks.cpp` |
| `do`, `run`, `include`, `!` shell escape | `stage_raw_files.cpp` |
| `continue`, `args`, `gettoken`, `return scalar/local`, `program define`, `tempvar`/`tempfile`/`tempname`, `macro` verbs | `stage_raw_programs.cpp` |
| `capture`/`quietly`/`noisily` prefixes, `eststo:`, `version:`, `svy:` prefixes | `stage_prefixes.cpp` |
| `local`/`global` definitions, `` `: display' `` splice, macro expansion of the line, verbatim lines, `xi:` | `stage_macro_expansion.cpp` |
| `bootstrap`, `jackknife`, `permute`, `rolling`, `statsby`, `ritest` lines | `stage_resampling.cpp` |
| Command word, abbreviation, glued heads, variable-name abbreviation, weights and `in` gates | `stage_tokenization.cpp` |
| `if` condition folded into a hidden mask column; per-form profile key | `stage_qualifiers.cpp` |
| Option-name resolution, community-command gate, empty-dataset rules, data-required guard, arity | `stage_validation.cpp`, `validation.cpp` |
| `by`/`bysort` prefix: sort, grouping, per-group run of `summarize`, `tabstat`, `generate`, `keep`, `drop`, `count` | `stage_by_prefix.cpp`, `prefix_support.cpp` |
| User-defined program or ado-style command invoked by name | `stage_program_resolution.cpp` |
| `use`, `save`, `import`, `export`, `file`, `post*`, `putexcel`, `copy` | `route_io.cpp` |
| `summarize`, `describe`, `codebook`, `count`, `list`, `levelsof`, `inspect`, `ds`, `xtsum` | `route_describe.cpp` |
| `rename`, `clonevar`, `format`, `label`, `labelbook` | `route_data_names.cpp` |
| `generate`, `replace`, `egen`, `encode`, `decode`, `destring`, `tostring` | `route_data_generate.cpp`, `data_support.cpp` |
| `drop`, `keep`, `sort`, `order`, `recode`, `collapse`, `expand`, `preserve`, `restore`, `xtile`, `tsfill` | `route_data_rows.cpp` |
| `merge`, `append`, `joinby`, `reshape`, `frame`/`frlink`/`frget`, `duplicates` | `route_data_combine.cpp` |
| Regression, IV, panel, count, discrete-choice, DID, RD, time-series and test commands | `route_estimation.cpp` |
| `test`, `lincom`, `nlcom`, `margins`, `predict`, `estat` | `route_postestimation.cpp` |
| `display`, `scalar`, `matrix`, `return`, `ereturn`, `estimates`, `eststo`, `esttab` | `route_results.cpp`, `results_support.cpp` |
| `tabulate`, `table`, `tabstat`, `correlate`, `pwcorr`, `ttest`, `mean`, `proportion` | `route_tabulation.cpp`, `summary_support.cpp` |
| Table-command family beyond the arms above (single call, result flattened) | `route_table_family.cpp` |
| `xtset`, `tsset`, `tset` | `route_time.cpp` |
| `clear`, `set`, `version`, `compress`, `exit`, `cd`, `log`, `confirm`, `erase`, `ssc` | `route_session.cpp`, `session_support.cpp` |
| `shell`, `dir`, `sleep`, `timer`, `bcal`, `frames`, `adopath`, `findfile`, `parallel` | `route_environment.cpp` |
| Community and utility commands (`unique`, `missings`, `rangestat`, `filelist`, `lookfor`, `drawnorm`, `input`, `sample`) | `route_extensions.cpp` |
| `graph`, `twoway`, `histogram`, `kdensity`, `coefplot`, `marginsplot`, `serset`, `line`, `scatter` | `route_graphics.cpp`, `graphics_support.cpp` |
| Inline `display`, `c()` locals, row-expression reference rewriting, `_rc` substitution | `expression_support.cpp` |
| Small text helpers: echo block, glued-head split, `joinVarlist`, result-block pushing | `text_support.cpp` |
| Command names, abbreviations, weight/`in` allow-lists, prefix registry, community-name table, `--dump-*` output | `registry.cpp` |
| Option vocabularies, minimum-abbreviation tables and the option-key resolver | `options_support.cpp` |

Headers. `types.h` is the shared private header: it includes the engine and handler
headers and defines the lifetime helpers (`RcHiddenScope`, `CallerVersionScope`,
`DoFileLevelGuard`, `QualifierColumnGuard`, `PrefixTable`) plus small record types.
`pipeline.h`, `options.h` (extern vocabulary objects and option sets, defined in
`options_support.cpp`) and `handler_declarations.h` (handler entry points the routes call
that are not in the public umbrella) are private to this directory. Each `*_support.h`
(`data_support.h`, `expression_support.h`, `graphics_support.h`, `prefix_support.h`,
`results_support.h`, `session_support.h`, `summary_support.h`, `text_support.h`) declares
only what its same-named `.cpp` defines; include the specific one you use. `varlist_join.h`
declares `joinVarlist` (defined in `text_support.cpp`) and is also included by one file
outside this directory. Two headers are public to other directories: `registry.h` (command
lookup, prefix and community tables) and `dispatch_profile.h` (the zone profiler). Callers
elsewhere include them as `dispatch/registry.h` and `dispatch/dispatch_profile.h`.

State and lifetimes. A stage returns `true` only when it finished the command and left its
result in `blocks`; references passed down are the original entry's variables. Do not move
a guard or profiler scope into a stage: early return and recursive dispatch (capture,
quietly, do-file replay) depend on the scope of `dispatchImpl` and `dispatch`.
`tl_rcHiddenDepth` (defined in `expression_support.cpp`) and `g_dispatchSerial` (in
`dispatcher.cpp`) are thread-local counters; `RcHiddenScope` must be held across the
expansion it covers. `registry.cpp` builds `commandRegistry()`, `prefixRegistry()` and the
community-name table once as function-local statics and marks plain-varlist rows after
construction; tokenizer qualifier gates and the by-prefix reject message read them at run
time, so a wrong row changes behavior. Option vocabularies in `options_support.cpp` register
at static-initialization time and keep their original order. The prefix tables in
`stage_prefixes.cpp` are built once from their declared sets. Session data, programs,
locals and `r()`/`e()` stores belong to `SessionState`; routes call handlers and do not keep
their own copies. `dispatch_profile.h` compiles to empty scopes unless
`PIET_ENABLE_DISPATCH_PROFILE` is defined; its counters are process-wide and unlocked.

Compile flags. No file in this directory is on the `-ffp-contract=off` list in
`piet/native/CMakeLists.txt`, and none carries an in-file `#pragma clang fp contract(off)`.
That list is for files whose results must byte-match separately rounded products and sums.

Tests. Contracts for the registry, abbreviation floors, prefix and qualifier tables and
option vocabularies: `selftest_command_contracts.cpp`. Error codes and unsupported-command
honesty: `selftest_command_errors.cpp`. Other pipeline regressions:
`selftest_command_stabilization.cpp`, `selftest_language_command_word_forms.cpp`,
`selftest_language_control.cpp`, `selftest_language_in_range_qualifier.cpp`,
`selftest_language_do_include_scope.cpp`, `selftest_data_by_keep_drop.cpp`,
`selftest_bootstrap_prefix.cpp`, `selftest_stored_estimates_eststo_prefix.cpp`,
`selftest_startup_and_paths.cpp` (version prefix) and `selftest_unicode_names_dispatch.cpp`.
`SELFTESTS.md` in the parent directory indexes them all.

Elsewhere. Command implementations (`handlers::` and `commands::` functions) live in the
domain directories (estimation, descriptive, datamgmt, results, tables, graph, time,
utility, environment); public declarations are in `piet/handlers.h` and
`piet/handlers/*.h`. The option-minimum table included by `options_support.cpp` is in
`generated/`. The `do`/`run` executor, loops and macro expansion engine are in `language/`
and `eval/`. A new ordinary command needs its implementation, a `registry.cpp` row and an
arm in the matching `route_*.cpp`; a new prefix or parsing rule needs the matching stage
and the integration suite.
