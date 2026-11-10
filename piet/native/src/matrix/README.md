# Matrix maintenance

This directory holds the matrix commands and expression evaluator, the
matrix/variable conversions, the native Mata statement subset, and the LAPACK
`DSYEVR` translation. Find the symptom in the table and read only that owner.
Entry points are declared in `piet/handlers/results.h` and `piet/mata_subset.h`.

| Command or symptom | Owner |
|---|---|
| `matrix define`/`matrix X = ...` dispatch, `matrix list` (incl. `format()`), `matrix drop`, `matrix input`, `matrix A[r,c] = ...`, `matrix NAME = e(NAME)`/`r(NAME)`, unserved `matrix` subcommands (rc 501/198/gap) | `matrix_commands.cpp` |
| Matrix expression grammar: literals, `,`/`\` joins, `'` transpose, `#`, operators, subscripts and ranges, `I()`, `J()`, `nullmat()`, `inv()`, `invsym()`, `syminv()`, `vecdiag()`, `el()` operands | `matrix_expression.cpp` |
| `matrix rownames`/`colnames`, `matrix accum`, `matrix list` text rendering (`renderMatrixList`, cell text) | `matrix_expression.cpp` |
| Shared evaluator interface (`MxValue`, `evalMx`, `storeMatrixResult`, `renderMatrixList`, stripe/accum entry points) | `matrix_expression_internal.h` |
| `matlist` rendering and its `format()`, `title()`, `names`, `nonames`, `noblank`, `twidth` options | `matlist.cpp` |
| `svmat` (matrix columns to variables, `names()`), `getmata` (Mata vectors/matrices to variables) | `matrix_to_variables.cpp` |
| `mkmat` (variables to a matrix: `matrix()`, `rownames()`, `nomissing`, `obs`, `rowprefix`) | `variables_to_matrix.cpp` |
| `matsort`, `matselrc` row/column selection and sorting over the stored matrix | `matrix_sort_select.cpp` |
| `mata:` statements and `mata: ... end` blocks, block folding, `invsym()` in the Mata subset, Mata-namespace matrices | `mata_subset.cpp` |
| Eigen-decomposition bits (`symeigensystem()`, `ml` `difficult` step, clustered-V rank) | `lapack_dsyevr.cpp`, `lapack_dsyevr.h`, `lapack_dlaruv_mm.inc` |
| Shared error helpers for `matlist`, `svmat`, `getmata`, `mkmat` | `matrix_command_errors.h` |

`matrix_command_errors.h` carries `matrix_command_detail::aerError` and
`aerErrorRc` (the second states an exact Stata rc through the error block's
`reason`, because the generic message-to-rc mapping would otherwise answer 1). Only
`matlist.cpp`, `matrix_to_variables.cpp` and `variables_to_matrix.cpp` include it.
`matrix_expression_internal.h` is the interface between `matrix_expression.cpp`
(definitions) and `matrix_commands.cpp` (the only other includer); do not include
it from outside this directory. `lapack_dsyevr.h` is the one public header: it
declares `piet::lapack::dsyevrVAL`, called by estimation code and one selftest. The
`.inc` file is the DLARUV multiplier table, included once inside
`lapack_dsyevr.cpp`.

State a change must respect. Matrices live in `session.eval.matrices`, a list of
name/`EvalMatrix` pairs; a write replaces the existing entry in place (the lookup
returns the first match, so a second entry with the same name would be ignored) and
erases a scalar of the same name, since scalars and matrices share one namespace.
That replace-in-place store is written three times (`storeMatrixResult` in
`matrix_expression.cpp`, `storeMatrix` in `variables_to_matrix.cpp`,
`putUserMatrix` in `matrix_sort_select.cpp`); keep them identical. Mata names live
in the separate `session.eval.mataMatrices`; only `st_matrix("X")` reads the Stata
store. Option vocabularies for `matlist`, `svmat`, `getmata`, `mkmat`, `matsort`
and `matselrc` are registered by namespace-scope statics so the dispatcher sees
them before the handler runs; keep them at namespace scope and keep every accepted
option read by its handler. `svmat` and `getmata` mint scratch CSV paths through
the session scratch registry. `mata_subset.cpp` and `lapack_dsyevr.cpp` keep no
program-wide state.

Compile flags. `matrix_expression.cpp`, `matrix_commands.cpp` and `mata_subset.cpp`
are on the `-ffp-contract=off` list in `piet/native/CMakeLists.txt`: StataBE rounds
every product and sum of matrix products, `inv()` and `invsym()` separately, and
fused arithmetic changes the last bit and the zero pivot of a singular matrix.
`lapack_dsyevr.cpp` is on the list too, with the opposite rule: the fused
multiply-adds of the reference build are written as `std::fma` at the same sites and
nothing else may be contracted. A file keeps its flag if it moves or is split.

Selftests: `selftest_matrix_define_and_subscripts.cpp` (`matrix define`,
expressions), `selftest_matrix_and_returns.cpp` (class layer, `svmat`, `mkmat`,
`matlist`), `selftest_matrix_sort_select.cpp` (`matsort`, `matselrc`),
`selftest_mata_subset_getmata.cpp` (Mata subset, `getmata`),
`selftest_ereturn_coefficient_matrices.cpp` and
`selftest_ereturn_matrix_functions.cpp` (`e(b)`/`e(V)` matrices, `el()`,
`colsof()`), and `selftest_ml_estimation.cpp` (the `dsyevrVAL` bit checks).

Elsewhere: command routing is in `dispatch/route_results.cpp` and
`dispatch/registry.cpp`; `return list` and `ereturn list` are in
`results/return_list.cpp`; matrix functions in `display`, `local` and `scalar`
expressions are in `eval/macro_matrices.cpp`; `frmttable` matrix export is in
`tables/table_matrix_export.cpp`. The header comment of `matrix_command_errors.h`
still describes `return list` and one shared translation unit; the code is split as
in the table.
