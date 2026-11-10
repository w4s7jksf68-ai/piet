# Descriptive command maintenance

Read-only reports over the data in memory: describing variables, summary statistics,
counts and listings, tabulation, comparisons and percentile reports. None of these
commands changes the dataset. Start with the command owner below; the public entry
points are declared in `piet/handlers/descriptive.h`, except `runCentile`
(`piet/handlers/estimation.h`) and `runLookfor`/`runFindname`
(`piet/handlers/variables.h`).

| Command or symptom | Owner |
|---|---|
| `describe` table (type, format, label, distinct count per variable) | `describe.cpp` |
| `summarize`, `summarize, detail`, `[aw]`/`[fw]`/`[iw]` weights, `if`/`in` restriction | `summarize.cpp` |
| `tabstat`, `correlate`/`pwcorr` (`runCorr`), `ttest` | `descriptive_statistics.cpp` |
| `count [if] [in]` and its native `if` evaluator (also used by `assert`), `list` | `data_listing.cpp` |
| `levelsof` (values, `local()`, `clean`) | `levels_command.cpp` |
| `tabulate`, `tab1`, `tab2`, one- and two-way tables, chi-squared statistics, `table` | `tabulate.cpp` |
| `inspect`, `xtsum`, `xttab` | `panel_descriptive_statistics.cpp` |
| `xtdescribe` (participation patterns, `T_i` distribution) | `panel_pattern_description.cpp` |
| `compare var1 var2` | `variable_compare.cpp` |
| `cf varlist using file` (data in memory against a `.dta` on disk) | `dataset_comparison.cpp` |
| `findname` (labels matching a glob) | `variable_search.cpp` |
| `lookfor` (name or label contains a word) | `lookfor.cpp` |
| `centile` (binomial, normal-theory and mean/sd limits) | `centile_intervals.cpp` |
| Which route calls a handler; `codebook`, `ds` | `route_describe.cpp`, `route_tabulation.cpp` in `dispatch/` |

This directory has no private header of its own. Its files include private headers
owned by other directories, and only those directories' files should do so. The list is
not exhaustive; for example:
`datamgmt/variable_metadata_support.h` (the `variable_metadata_detail` helpers used by
`cf`, `xtdescribe` and `findname`), `datamgmt/dataset_shape_support.h` and
`dataset_shape_options.h` (`compare`), `datamgmt/data_combine_internal.h`
(`panel_descriptive_statistics.cpp` defines `stataG` and `sampleSdDV` for it), and the
`estimation/*_internal.h` and `estimation_options.h` headers (`tabstat`, `corr`, `ttest`,
`summarize`; `estimation_internal.h` is also used by `describe.cpp`,
`centile_intervals.cpp` and `variable_compare.cpp`), the engine headers
`engine/command_support.h`, `engine/engine_call_internal.h` and
`engine/scratch_columns.h`, `estimation/statistics_command_support.h`,
`estimation/bsample_draws.h` and `matrix/lapack_dsyevr.h`. Do not add a second copy of a helper here when one of these already
declares it.

State and lifetimes. `describe`, `summarize`, `count`, `list`, `levelsof` and
`centile` read the session's resident card or datastore and write nothing back except
`r()` values (and, for `levelsof, local()`, the named local macro). Numeric `summarize` and `tabstat` call the cobra engine through
`runCobraJson`; an `if`/`in` restriction first writes a filtered scratch file that a
guard deletes on return, and `SessionState::StatelessRead` keeps that read from evicting
the real head. `levelsof` registers its filtered copy with `noteProjectionScratch`.
`runCount` tries a native evaluator over a small expression whitelist and falls back to
the engine whenever it throws `CountFastBail`; keep an accepted expression bit-identical
to the engine's answer. `cf`, `xtdescribe` and `findname` are r-class: once
options are validated they call `replaceRClass` and `declareRPosted` before any later
return, so a stop after option checking leaves `r()` empty; an option or syntax error
leaves the previous `r()` untouched. Their option vocabularies (`kVocabCf`, `kVocabXtdescribe`, `kVocabFindname`) are
registered by static initialisation and must stay in step with the options each
handler reads. `lookfor` builds its table by calling `runDescribe`. There are no
program-wide counters in this directory.

Compile flags. `descriptive_statistics.cpp` is on the `-ffp-contract=off` list in
`piet/native/CMakeLists.txt` (the numpy-parity group: a fused multiply-add rounds once
where the reference arithmetic rounds twice, which moves the last bit). `data_listing.cpp`
and `levels_command.cpp` are on the same list, in the group the CMake comment explains
by `matrix` expressions, where StataBE rounds every product and sum separately. The
other files here are not on the list and carry no `#pragma clang fp contract(off)`.
A file on the list must keep the flag if it moves or is split.

Selftests: `selftest_descriptive_statistics.cpp` (summarize, tabstat, levelsof, corr,
ttest), `selftest_descriptive_lookfor.cpp`, `selftest_metadata_utility_commands.cpp`
(`findname`, `xtdescribe`, `cf`), `selftest_language_in_range_qualifier.cpp` (`in` on
`list`, `summarize`, `count`), `selftest_label_metadata.cpp` (tabulate value labels),
and `selftest_unicode_names_output_width.cpp` (centile, xtsum, summarize widths); all
are indexed in `SELFTESTS.md`.

Not here: `pctile`, `cumul` and the gtools quantile commands are in `datamgmt/`;
`assert` is in `datamgmt/assert_command.cpp` and calls `runCount`; `esttab` and
coefficient tables are in `tables/`; `egen` statistics are in `datamgmt/`; the dispatcher
wiring of every command above is in `dispatch/`.
