# Data management maintenance

This directory holds the commands that create, reshape, select, convert, label,
combine and summarise the dataset in memory, plus frames, `preserve`/`restore` and
the `gtools`/`ftools` name aliases. Start with the command owner below; public
entry points are declared in `piet/handlers/*.h`, and the routing from a command
word to its `run*` function is in `dispatch/`.

| Command or symptom | Owner |
|---|---|
| `generate`, `replace` (numeric), `assert` | `data_generate.cpp`, `assert_command.cpp` |
| `[bysort:] generate/replace`, `in` or `_n` forms via `--bygen` | `generate_by_group.cpp` |
| `egen` command, option checks, abbreviation and unimplemented-function tables | `egen.cpp` |
| `egen` `anyvalue`, `anycount`, `anymatch`, `cut`, `concat` | `egen_computed_functions.cpp` |
| String `generate`/`replace`, expression typing and the column writer | `string_generate_replace.cpp` |
| Row evaluator (`evalRows`), string-expression detection, `if` rewriting | `row_evaluation.cpp` |
| `count if`, `keep if`/`drop if` via a row mask, hidden qualifier/weight columns | `row_conditions.cpp` |
| `drop`, `keep`, `drop if`/`keep if`, `by:` keep/drop, `sort` | `observation_selection.cpp` |
| `rename`, `clonevar` | `variable_copy_rename.cpp` |
| `tolower`, `renvars`; `renpfix`; `move` | `variable_bulk_rename.cpp`; `variable_prefix_rename.cpp`; `variable_move.cpp` |
| `order`, `gsort` | `data_order.cpp` |
| `label variable/values/define/list/dir/drop/language`, `labelbook`, `format`, `datasignature` | `label_metadata.cpp` |
| `labvars`; `numlabel` | `bulk_variable_labels.cpp`; `value_label_numbering.cpp` |
| `notes`/`char` note table | `data_notes.cpp` |
| `encode`, `decode`, `destring`, `tostring` | `encode.cpp`, `decode.cpp`, `destring.cpp`, `tostring.cpp` |
| Skipped-variable note and `generate()` column placement shared by `destring`/`tostring` | `string_conversion_support.cpp` |
| `merge`, `append`, `reshape`, `joinby` | `data_merge.cpp`, `data_append.cpp`, `data_reshape.cpp`, `data_joinby.cpp` |
| Reading the `using` file of merge/append/joinby/cross | `using_dataset.h` |
| `collapse`, `duplicates`, `recode` | `data_collapse.cpp`, `data_duplicates.cpp`, `data_recode.cpp` |
| `isid`, `expand`, `fillin`, `cross`, `contract` | `data_observation_expansion.cpp` |
| `xtile`, `ipolate`, `tssmooth ma`, `mkspline` | `data_quantiles.cpp`, `data_interpolation.cpp`, `data_splines.cpp` |
| `pctile`, `_pctile`, weighted `xtile`; `cumul`; `bsample`; `sample`, `splitsample`; `drawnorm` | `pctile.cpp`; `cumul.cpp`; `bsample.cpp`; `random_sampling.cpp`, `drawnorm.cpp` |
| `split`, `xpose`, `recast`, `insobs`, `stack`, `separate`, `mvencode`, `input` | `string_split.cpp`, `xpose.cpp`, `recast.cpp`, `insobs.cpp`, `dataset_stack.cpp`, `separate_by_level.cpp`, `mvencode.cpp`, `input_data_block.cpp` |
| `mvdecode`, `integ`, `sencode`, `fillmissing`, `astile` | `mvdecode.cpp`, `integ.cpp`, `sencode.cpp`, `fillmissing.cpp`, `astile.cpp` |
| `winsor` (Cox); `winsor2` | `winsor.cpp`; `winsor2.cpp` |
| `ihstrans`, `swindex`, `xi` and `xi:` | `ihstrans.cpp`, `swindex.cpp`, `xi_indicator_expansion.cpp` |
| `frame put/drop` prefix, `frlink`, `frget`, frame reset | `frames.cpp` |
| `preserve`, `restore`, the nested-level unwinder | `preserve_restore.cpp` |
| `gcollapse`, `fcollapse`, `gegen`, `gduplicates`, `glevelsof`, `gisid`, `greshape`, `fmerge` aliases | `gtools_aliases.cpp` |
| `gquantiles` (`pctile`/`_pctile` forms); `gstats winsor`/`sum` | `gquantiles_pctile.cpp`; `gstats.cpp` |
| Mode-word and strict-number parsing for the `gtools` handlers | `gtools_support.cpp` |
| Option vocabularies of the shape/sample verbs (`split` through `_pctile`) | `dataset_shape_options.cpp` |
| Shared helpers of those verbs (CSV round trip, errors, `runVerb`) | `dataset_shape_support.cpp` |
| Shared helpers of `mvdecode`, `integ`, `sencode`, `fillmissing`, `astile` | `column_write_support.cpp` |
| Shared helpers of `numlabel`, `labvars`, `renpfix` | `variable_metadata_support.cpp` |

Private headers are named for the group that shares them and, unless noted, are
included only by the `.cpp` files listed with them. `datamgmt_support.h` is the
common preamble and scratch-path helpers of the former data-verb file; it is
included by `data_io_internal.h`, `data_mutation_internal.h`,
`data_combine_internal.h`, `using_dataset.h`, the four conversion commands and `string_conversion_support.cpp`.
`data_mutation_internal.h` serves `data_collapse.cpp`, `data_duplicates.cpp`,
`data_order.cpp`, `data_recode.cpp` and `variable_copy_rename.cpp`;
`data_combine_internal.h` serves merge, append and reshape; `data_io_internal.h`
serves joinby, quantiles, interpolation and observation expansion. These three are
also included from `fileio/`, `time/` and `descriptive/`, so a signature change
there needs those callers checked. `dataset_shape_support.h` and
`dataset_shape_options.h` are shared by the shape/sample verbs and by
`descriptive/variable_compare.cpp`. `egen_internal.h`, `row_evaluation_internal.h`,
`string_conversion_internal.h`, `gtools_internal.h` and
`variable_rename_move_internal.h` are each shared only by the files of their own
group. `column_write_support.h` must not be included elsewhere: several of its
names exist with different bodies in `utility/utility_support.h`.
`variable_metadata_support.h` is also included by a few `descriptive/` and
`language/` files.

State a change must respect: option vocabularies are registered at namespace scope
(`dataset_shape_options.cpp` for the shape verbs; one `kVocab*` per command file
elsewhere) and same-named registrations are unioned, so an accepted option must be
read by its handler. `data_merge.cpp` and `data_reshape.cpp` register their vocabulary
twice, in two namespaces, deliberately. Scratch paths draw on one program-wide counter
plus the process id in `datamgmt_support.h`; `frames.cpp` keeps its own counter, and
`row_conditions.cpp` keeps a process-wide counter for hidden qualifier column names
so nested commands never overwrite each other's mask. `row_evaluation.cpp` holds a
write counter and a force-legacy flag that exist for selftests. Frames live as
files in the session work directory; the preserve stack is `SessionState::preserveStack`.
`preserve_restore.cpp` pushes and pops it for `preserve` and `restore`, and
`SessionState::discardTopPreserve()` (`session/session_resident_head.cpp`, called from
`dispatch/route_data_rows.cpp` for `restore, not`) also pops it. Keep the two pop paths
consistent.

Compile flags: `data_splines.cpp`, `observation_selection.cpp`, `label_metadata.cpp`
and `preserve_restore.cpp` are on the `-ffp-contract=off` list in
`piet/native/CMakeLists.txt`; `data_splines.cpp` falls in the numpy-parity part of the
list, the other three in the part whose comment says StataBE rounds each product and
sum separately. `column_write_support.cpp`, `astile.cpp`, `mvdecode.cpp`, `integ.cpp`,
`sencode.cpp` and `fillmissing.cpp` carry `#pragma clang fp contract(off)` in the
file (the spline in `integ.cpp` must round every product and sum separately, as
StataBE does). A file must keep its flag if it moves or is split.

Selftests: `selftest_data_commands.cpp`, `selftest_data_by_keep_drop.cpp`,
`selftest_data_combine.cpp`, `selftest_data_combine_varlist_options.cpp`,
`selftest_data_egen_statistics.cpp`, `selftest_data_generation_utilities.cpp`,
`selftest_data_rename_recode_encode.cpp`, `selftest_data_storage.cpp`,
`selftest_data_string_functions.cpp`, `selftest_label_metadata.cpp`,
`selftest_metadata_utility_commands.cpp`,
`selftest_string_conversion_destring_tostring.cpp`,
`selftest_string_conversion_tostring_parity.cpp`, `selftest_datamgmt_ihstrans.cpp`,
`selftest_datamgmt_swindex.cpp` and `selftest_language_in_range_qualifier.cpp`;
`piet/native/src/SELFTESTS.md` maps each to its feature.

Elsewhere: `compare` is registered here (`kVocabCompare`) but its handler is
`descriptive/variable_compare.cpp`. File loading and saving (`use`, `save`, `import`,
`export`) are in `fileio/`; `tsset`/`xtset` and `tsfill` are in `time/`; `findname`,
`cf`, `lookfor` and listing commands are in `descriptive/`; the registry and routes
are in `dispatch/`; `unique`, `missings`, `labmask` and `filelist` are in `utility/`.
