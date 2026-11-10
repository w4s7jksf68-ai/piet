# Estimation maintenance

This directory holds the model-fitting commands, the support code they share, the
resampling prefixes (`bootstrap`, `jackknife`, `permute`, `rolling`, `statsby`) and the
installation of `e(sample)`. Most fitting commands are thin: they build engine arguments,
call the engine, then reshape its reply into result blocks and `e()`. Start with the
owner row below, then open `estimation_support.cpp` if the symptom is shared across
commands (option parsing, `vce()`/cluster handling, reply conversion).

| Command or symptom | Owner |
|---|---|
| `regress`, `reghdfe`, `feols` fit, `vce()`/cluster refusals, `e()` after a linear fit | `linear_estimation.cpp`, `linear_estimation_support.cpp` |
| `areg`, `ivreg2`, `xtivreg2` option normalisation onto existing estimators | `areg_ivreg2_aliases.cpp` |
| `mean`, `proportion` | `mean_proportion_estimation.cpp` |
| `xtreg` fits, `r2` aliasing, the common cache of the last fit (`cacheLastModelM4`) | `panel_estimation.cpp` |
| `xtabond` | `dynamic_panel_estimation.cpp` |
| `logit`, `probit` | `binary_estimation.cpp` |
| `ologit`, `oprobit`, `mlogit`, `zip`, `zinb`, `fracreg` | `categorical_estimation.cpp` |
| `poisson`, `ppmlhdfe`, `nbreg`, `xtpoisson`, `xtlogit, fe` | `count_estimation.cpp` |
| `tobit` | `censored_estimation.cpp` |
| `heckman` | `selection_estimation.cpp` |
| `qreg` engine path | `quantile_estimation.cpp` |
| `qreg` native path, `qreg2`, `qreg2` margins | `qreg2_estimation.cpp` |
| `ivregress`, `ivreghdfe`, first-stage tables, `ivreg2` `ffirst`/`savefirst`, `xtivreg2`, `ivprobit`, `ivtobit` | `instrumental_variables.cpp` |
| Parsing the `(endog = instruments)` block and `at()` numlists | `iv_specification.cpp` |
| `reg3`, `sureg` | `system_estimation.cpp` |
| `glm` (family/link grammar, design matrix, IRLS log) and `margins` after it | `estimation_glm.cpp` |
| `nl`, `gmm` | `nonlinear_estimation.cpp` |
| `ml model lf` evaluator (compiled stored programs, Newton steps, `technique()`) | `ml_estimation.cpp` |
| `mixed` | `mixed_estimation.cpp` |
| `mi` | `multiple_imputation.cpp` |
| `stset`, `stcox`, `streg` | `survival_estimation.cpp` |
| `svyset`, `svy:` prefix | `survey_commands.cpp` |
| `newey`, `prais`, `dfuller`, `pperron`, `wntestq` | `time_series_estimation.cpp` |
| `var`, `vargranger`, `irf` | `vector_time_series.cpp` |
| `csdid` and `estat` after it, e(b)/e(V) posting of event/simple/calendar blocks | `did_estimation.cpp` |
| `drdid` | `did_two_period.cpp` |
| `eventstudyinteract`, `did_imputation`, `sdid` | `eventstudyinteract.cpp`, `did_imputation.cpp`, `synthetic_did.cpp` |
| `rdrobust`, `rddensity`, `rdplot` | `regression_discontinuity.cpp` |
| Table geometry, option pushing and `e()` posting for the engine-reported commands above | `engine_report_support.cpp`, `engine_report_support.h` |
| `mediate` | `mediation_estimation.cpp` |
| `teffects nnmatch` (other `teffects` subcommands refuse) | `teffects_nnmatch.cpp` |
| `oaxaca` | `oaxaca_decomposition.cpp` |
| `pca` | `principal_components.cpp` |
| `spearman`, `sktest`, `signrank` | `rank_and_normality_tests.cpp` |
| `lowess`, `lpoly` | `lowess_lpoly_smoothing.cpp` |
| `rlasso`, `lasso`, `lasso` predict and coefficient reads | `lasso_estimation.cpp` |
| `acreg` | `acreg_estimation.cpp` |
| `leebounds` | `leebounds_estimation.cpp` |
| `ritest` | `ritest_inference.cpp` |
| `bootstrap:` prefix | `bootstrap_prefix.cpp` |
| `jackknife:`, `permute:` prefixes; the design note for the whole prefix family | `jackknife_permute_prefix.cpp` |
| `rolling:`, `statsby:` prefixes | `rolling_prefix.cpp`, `statsby_prefix.cpp` |
| Exp-list parsing, scratch paths, cell formatting and `e()` matrix post shared by the prefixes | `resampling_prefix_support.cpp`, `resampling_prefix_internal.h` |
| Bootstrap row draws on the session's random stream | `bsample_draws.h` |
| Engine reply to result block: coefficient table, factor labels, `vce()` argv, cluster spec refusals, if-filter, fit-filter capture | `estimation_support.cpp` |
| `e()` name lists and macros posted after regress, `xtreg, fe` and ivregress | `estimation_results.cpp` |
| Wald p/CI backfill of `coeftable` for `nbreg`, `xtpoisson`, `xtlogit` | `inference_statistics.cpp` |
| Shared engine call for the maximum-likelihood families, ancillary `e(b)` columns | `likelihood_support.cpp` |
| Registered option vocabularies (the accept/ignore net for most commands here and for post-estimation commands) | `estimation_options.cpp`, `estimation_options.h` |
| `e(sample)` mask: engine handshake, hidden column install | `estimation_sample_install.cpp` |
| `e(sample)` carried through `expand`, `collapse`, `merge`, `joinby`, `bsample` | `estimation_sample_carry.cpp` |
| Option-abbreviation rule, nested dispatch, column writer used by the community commands | `estimator_command_support.h` |
| Error blocks, name cells, column read scope, engine capability probe for statistics commands | `statistics_command_support.cpp`, `statistics_command_support.h` |

Private headers. `estimation_internal.h` is the broad header: engine-call helpers, `vcov`
and cluster-spec parsing, ancillary `e(b)` columns, `csdid` posting types. It is included by
this directory and also by post-estimation, results, tables, graph, descriptive and data
files that need a fitted model's helpers; it pulls in `estimation_sample_internal.h`, the
narrow `e(sample)` API, which `dispatch/types.h` and the data-management headers include on
its own. `estimation_families_internal.h` (likelihood, panel, survival, survey, vector and
related families) and `extended_estimation_internal.h` (IV, count, censored, DID, system,
VCE-shape reads) are private to this directory, except that `estimation_families_internal.h` is
also included by `descriptive/descriptive_statistics.cpp` and
`extended_estimation_internal.h` by `postestimation/bootstrap_inference.cpp`.
`engine_report_support.h` is also included by the three `graph/binscatter*.cpp` files. `linear_estimation_internal.h` is private to
the linear, mean/proportion and linear-support files. `resampling_prefix_internal.h` is
private to the five prefix files. `estimator_command_support.h` is used by the community
commands here plus `postestimation/suest_estimation.cpp` and `datamgmt/swindex.cpp`;
`statistics_command_support.h` is also used by a few files outside this directory
(`postestimation/coefficient_column_names.cpp`, `time/hodrick_prescott_filter.cpp`,
`descriptive/centile_intervals.cpp`, `datamgmt/xi_indicator_expansion.cpp`).

State and lifetimes. This directory keeps no process-wide counters. A fit is remembered
as `LastModel` on the `SessionState` (written by `cacheLastModelM4` in
`panel_estimation.cpp`, or directly through `session.setLastModel` by many handlers, for
example `csdid`, `reg3`, `glm` and `ml`); `e()` scalars, matrices
and macros live on the session's evaluator, and survey design lives on `session.svyDesign`.
Do not add a second store. Option vocabularies are namespace-scope constants registered once
at static-initialisation time by `estimation_options.cpp` and a few per-command files
(`registerOptVocab` unions entries per command token, so a new key in one registration
widens every command sharing that token). The `e(sample)` mask is a hidden column whose
cells hold the install's serial; the engine's `esample_n` reply key must be erased after the
handshake so it is not readable as `e(esample_n)`. Scratch files come from the shared
scratch minting helpers and are owned by the scratch census until unlinked.

Compile flags. `-ffp-contract=off` in `piet/native/CMakeLists.txt` keeps `a*b + c` as two
roundings; the CMake comment gives byte-exact agreement with the reference arithmetic
as the reason. In this directory the flag is set for `estimation_support.cpp`,
`estimation_results.cpp`, `estimation_options.cpp`, `panel_estimation.cpp`,
`likelihood_support.cpp`, `categorical_estimation.cpp`, `vector_time_series.cpp`,
`multiple_imputation.cpp`, `dynamic_panel_estimation.cpp`, `nonlinear_estimation.cpp`,
`mixed_estimation.cpp`, `survey_commands.cpp`, `survival_estimation.cpp`,
`binary_estimation.cpp`, `did_estimation.cpp`, `iv_specification.cpp`,
`instrumental_variables.cpp`, `system_estimation.cpp`, `censored_estimation.cpp`,
`selection_estimation.cpp`, `quantile_estimation.cpp`, `mediation_estimation.cpp`,
`inference_statistics.cpp`, `count_estimation.cpp`, `did_two_period.cpp`,
`time_series_estimation.cpp` and `ml_estimation.cpp` (its own comment: gradient, Hessian and
step arithmetic must round every product and sum separately). `bsample_draws.h` carries
an in-file `#pragma clang fp contract(off)` for `stataUnitTarget`, whose product and sum
must round separately before truncation. A file keeps its flag if it moves or is split.

Tests. See `SELFTESTS.md` for the full index. Relevant files: `selftest_estimation.cpp`,
`selftest_estimation_weights_and_vce.cpp`, `selftest_estimation_vce_option_shapes.cpp`,
`selftest_estimation_collinear_omission.cpp`, `selftest_estimation_xtreg_fe_weights.cpp`,
`selftest_estimation_glm.cpp`, `selftest_iv_and_inference.cpp`,
`selftest_iv_estimation_options.cpp`, `selftest_iv_ivreg2_first_stage.cpp`,
`selftest_iv_xtivreg2_fe.cpp`, `selftest_likelihood_estimation.cpp`,
`selftest_time_series_estimation.cpp`, `selftest_ml_estimation.cpp`,
`selftest_lasso_estimation.cpp`, `selftest_qreg2_estimation.cpp`,
`selftest_acreg_estimation.cpp`, `selftest_leebounds_estimation.cpp`,
`selftest_ritest_inference.cpp`, `selftest_bootstrap_prefix.cpp`,
`selftest_treatment_estimators.cpp`, `selftest_graphics_lpoly_lowess.cpp` and
`selftest_stored_estimates_esample.cpp`.

Elsewhere. The public entry-point declarations are in `piet/handlers/estimation.h` and
`piet/handlers/resampling.h`. `margins`, `predict`, `test`, `lincom`, `nlcom`, `estat`
and `suest` are in `postestimation/`; `estimates` and `eststo` storage is in `results/`
and `tables/`; the `bsample` command itself is in `datamgmt/`; `centile` is in
`descriptive/`. Option vocabularies for several post-estimation commands (`margins`,
`hausman`, `boottest`, `ttest`) are registered in `estimation_options.cpp` although the
handlers live elsewhere.
