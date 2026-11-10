# Postestimation maintenance

This directory holds the commands that run after a fit and read the live `e()` or
the cached `LastModel`: `predict`, `margins`, `test`/`testparm`/`lincom`/`nlcom`,
`estat`, `boottest`, `lrtest`, `suest`, and the `r(table)` / default `e()` macros
that every fit gets. Start with the command owner below; the public entry points are
declared in `piet/handlers/postestimation.h`, `estimation.h` and `results.h`.

| Command or symptom | Owner |
|---|---|
| `predict` statistics, new column in the live frame, `predict` after nnmatch | `prediction.cpp` |
| `margins`, `margins, dydx()`, `at()` grids, the `r()` results of margins, cached data for `marginsplot` | `margins.cpp` |
| `test`, `testparm`, accumulated `test, accumulate`, `lincom`, `nlcom`, `eform` handling | `hypothesis_tests.cpp` |
| Engine refit for test/lincom/nlcom, "no previous estimation" r(301), refit and constant guards | `postestimation_context.cpp` |
| `estat` (regress diagnostics, IV `endogenous`/`overid`/`firststage`, `ic`, csdid `simple`/`event`/`calendar`/`group`/`pretrend`) | `estimation_diagnostics.cpp` |
| `boottest` (wild bootstrap tests and CIs, its refusals) | `bootstrap_inference.cpp` |
| `sdtest`, `hausman`, `chow` | `model_comparison.cpp` |
| `lrtest` of stored or live fits | `likelihood_ratio_test.cpp` |
| `suest` stacked sandwich over stored regress fits | `suest_estimation.cpp` |
| `r(table)` build, post, retire; replacement of stale `r()` after every command | `rtable_rclass_lifecycle.cpp` |
| Column order and names of `e(b)` (factor base levels, `o.` columns), `eb_stripe` | `coefficient_column_names.cpp` |
| Default `e(cmdline)`, `e(title)`, `e(predict)`, `e(estat_cmd)`, `e(marginsok)` | `fit_default_e_macros.cpp` |
| Declarations shared by the command files above | `postestimation_internal.h` |
| Declarations shared by the `r(table)` / `e(b)` / default-macro files | `rtable_internal.h` |

`postestimation_internal.h` (namespace `postest_detail`) carries the refit helpers,
the estimator allow-lists, the no-estimation errors and `nextPathEst`. It is for the
command files in this directory; `datamgmt/data_splines.cpp` also includes it for
`nextPathEst`, and `estimation/time_series_estimation.cpp` includes it as well. `rtable_internal.h`
(namespace `rtable_detail`) declares `stataColumns`, `annotateEbStripe`,
`postDefaultCmdline`, `postDefaultEMacros` and small number helpers; only
`coefficient_column_names.cpp`, `fit_default_e_macros.cpp` and
`rtable_rclass_lifecycle.cpp` include it.

State lives in `SessionState`, not in file-level statics. `margins.cpp` writes
`session.lastMargins` and `session.lastMarginsDydx`, which `marginsplot` (in `graph/`)
reads; keep both in step when a new margins path posts. `hypothesis_tests.cpp` keeps the
`test, accumulate` constraint list in an `r()` string slot keyed to a fingerprint of the
live estimation block, so a set from an earlier model is never extended. `suest_estimation.cpp`
and `likelihood_ratio_test.cpp` read `session.estimatesModels` (the stored fits). In
`rtable_rclass_lifecycle.cpp`, the identity of the live `e()` is taken by the dispatcher on
entry and compared on exit; the refusal note and generation counters live on `EvalSession`
outside every `r()` store, so a wholesale `r()` wipe cannot delete them. Refits go through
the engine session (`activeCobraSession()`); minting a handle there calls `setLastModel`,
which may move the `LastModel` a caller still holds a reference to.

Compile flags: `bootstrap_inference.cpp`, `postestimation_context.cpp`,
`hypothesis_tests.cpp`, `margins.cpp`, `prediction.cpp`, `estimation_diagnostics.cpp`
and `model_comparison.cpp` are on the `-ffp-contract=off` list in
`piet/native/CMakeLists.txt` (no in-file pragma is used here). The CMake comment gives the
reason: values such as `b +/- z*se` must round the multiply and the add separately to
match numpy-parity reference results, and a fused multiply-add changes the last bit. A
function from these files that moves must go to a file with the same flag. The other five
`.cpp` files carry no such flag.

Selftests: `selftest_postestimation.cpp`, `selftest_postestimation_margins.cpp`,
`selftest_postestimation_predict_frame.cpp`, `selftest_postestimation_predict_statistics.cpp`
and `selftest_postestimation_tests_and_rtable.cpp` cover predict, margins, test/lincom
and `r(table)`; `selftest_model_diagnostics.cpp` covers `estat` diagnostics, `sdtest` and
`hausman`; `selftest_suest_estimation.cpp` covers `suest`; `selftest_iv_and_inference.cpp`
covers `boottest`; `selftest_program_utility_commands.cpp` covers `lrtest`.

Not here: `estimates store/restore/table` and `estimates stats` are in `results/`;
`marginsplot` is in `graph/`; the fits themselves (`regress`, `glm`, `csdid`, `ivregress`,
...) are in `estimation/`; command routing and the call to `rTableAfterCommand` are in
`dispatch/`. The declarations of `runBoottest`, `runSuest`, `runSdtest`, `runHausman` and
`runChow` are in `piet/handlers/estimation.h`, not `postestimation.h`.
