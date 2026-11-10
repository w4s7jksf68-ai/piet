# Results maintenance

This directory holds the commands that list, post, clear, store and restore results:
`scalar list`, `return list`, `ereturn` and the `estimates`/`eststo` store. Start with
the command owner below. Routing from a typed line to these entry points is in
`dispatch/route_results.cpp`; the entry-point declarations are in
`piet/handlers/results.h` and `piet/handlers/estimates_tables.h`.

| Command or symptom | Owner |
|---|---|
| `scalar list`, `scalar dir` column format | `scalar_results.cpp` (`runScalarList`) |
| `ereturn list` dump of the stored e() scalars, macros and matrices | `scalar_results.cpp` (`runEreturnList`) |
| Matrix names as printed by listings (`bn.` factor-level marker dropped) | `scalar_results.cpp` (`matrixNameAsListed`, used by `matrix/matrix_expression.cpp`) |
| `return list` (r() scalars, macros, matrices, newest first) | `return_list.cpp` |
| `ereturn clear`, `ereturn post`, `ereturn local`, `ereturn matrix`, bare `ereturn repost`, named refusals of other sub-verbs | `ereturn_commands.cpp` |
| Emptying every e() store (`clearEResults`, also opens `ereturn post`) | `ereturn_commands.cpp` |
| `ereturn post ..., esample()` mask column | `ereturn_commands.cpp` (`installPostedMask`) |
| `eststo`, `estimates store`, stored-model snapshots, `_est_NAME` mask variables | `stored_estimates_commands.cpp` (`runEststo`) |
| `estimates dir`, `estimates drop`, `estimates clear`, `eststo clear` | `stored_estimates_commands.cpp` (`runEststoDir`, `runEststoDirNames`, `runEststoManage`, `clearStoredEstimates`) |
| `estimates restore NAME`, `estimates esample`, `estimates esample: ...` | `stored_estimates_commands.cpp` (`runEstimatesRestore`, `runEstimatesEsample`) |
| Snapshot fields kept per stored model; namelist and engine error blocks | `stored_estimates_snapshot.cpp`, declared in `stored_estimates_internal.h` |
| `estimates save`, `estimates use` (`.ster` file read and write) | `estimates_save_use.cpp` |

`stored_estimates_internal.h` is the only private header. It declares the five helpers
in `stored_estimates_detail` (`engineErrorBlockEt`, `estNamelistError`, `makeSnapshot`,
`escalarsFrom`, `emptyAddedBag`), which are defined once in `stored_estimates_snapshot.cpp`.
Three files here include it, and so do the other readers of the stored-estimates store:
`tables/esttab_block.cpp`, `tables/estimates_table.cpp` and
`graph/coefplot_estimates_block.cpp`. Other directories should not include it. The
declarations of `runEstimatesRestore` and `runEstimatesEsample` are not in the public
headers; they are in `dispatch/handler_declarations.h`.

State lives in `SessionState`, not here. The e() stores are `session.eval.eScalars`,
`eStrings` and `eMatrices`, with `lastModel()`, `lastEstBlock`, `ePostedLive` and the
`returnStaging` entries tagged `escalar`. `clearEResults` must reset all of them,
and `ereturn post` replaces the whole of e(), it does not add to it. The stored models
are `session.estimatesModels` and `activeEstimatesName`; each stored model keeps its own
copy of the e() stores and its e(sample) serial. The e(sample) mask is a hidden data
column; installing one advances the single `session.esampleColumnInstalls` counter, so
a new serial must come from that counter. `ereturn_commands.cpp` registers the
`ereturn` option vocabulary once at static initialisation (`kVocabEreturn`): an option
accepted there must be read by `runPost`, or the vocabulary entry must move to the
unimplemented list. `estimates save` writes a JSON document marked `PIETEST1` holding the
estimation block, the replayable model and, for a posting without a model, the e() stores;
`estimates use` restores from it, so a new field needs both a writer and a reader.

Compile flags: `scalar_results.cpp`, `stored_estimates_snapshot.cpp`,
`stored_estimates_commands.cpp` and `estimates_save_use.cpp` are on the
`-ffp-contract=off` list in `piet/native/CMakeLists.txt`; `return_list.cpp` and
`ereturn_commands.cpp` are not, and no file here carries an in-file fp-contract pragma.
The CMake comment gives the reason for the list as bit-exact results: products and sums
must be rounded separately, never fused into one multiply-add (the stored-estimates files
are listed next to the coefplot confidence-interval recompute, `b ± z*se`;
`scalar_results.cpp` sits under the `matrix` expression entry). A file that moves or
is split must keep the flag.

Selftests: `selftest_ereturn_commands.cpp` (`ereturn clear`/`post`/`local`),
`selftest_ereturn_coefficient_matrices.cpp`, `selftest_ereturn_matrix_functions.cpp`,
`selftest_matrix_and_returns.cpp` (results bundle, `ereturn list`),
`selftest_command_contracts.cpp` (`scalar list`, `ereturn list` against its long form),
`selftest_stored_estimates.cpp`, `selftest_stored_estimates_esample.cpp`,
`selftest_stored_estimates_eststo_prefix.cpp` and `selftest_tables_esttab_namelists.cpp`.
`piet_selftest --suite stored-estimates` runs the stored-estimates subset.

Not here: `scalar NAME = expr` is captured by the macro layer (`eval/macros.cpp`), and
`return scalar/local` and `ereturn scalar` inside a program are handled
by `language/program.cpp` and `dispatch/stage_raw_programs.cpp`. Matrix definition and
`matrix list` are in `matrix/`. `esttab`, `estimates table` and `coefplot` rendering are
in `tables/` and `graph/`; this directory only supplies the stored models they read.
