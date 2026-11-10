# PIET 2.0 native core — M0 walking skeleton.
#
# Standalone CMake subproject (its own project(), NOT add_subdirectory'd from
# the repo-root CMakeLists.txt, which is cobra's own `cecon` build). Per the
# Master Plan (docs/piet/ROADMAP.md, "2.0.0 — C++ core"), piet/native/ lives
# beside piet/backend and piet/frontend and is built/tested independently:
#
#   cmake -S piet/native -B piet/native/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build piet/native/build -j
#   ./piet/native/build/piet_native --selftest
#   ./piet/native/build/piet_native            # interactive demo window
#
# cobra itself is NOT built here — piet_native calls the pinned cobra binary
# as a subprocess (Art 7 provenance; see src/engine/cobra_runner.cpp).
cmake_minimum_required(VERSION 3.20)
project(piet_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-deprecated-declarations)

# M4a (ROADMAP §2.0.0 "M4"): numpy-parity floating-point — three
# translation units compute quantities that must byte-match numpy's
# IEEE-754 DOUBLE-rounded arithmetic (multiply, THEN add — two separate
# roundings). On arm64, clang's default -ffp-contract=fast fuses `a*b + c`
# into a single FMA instruction — ONE rounding — which diverges in the
# last ULP. That divergence is invisible to the cross_impl numeric-
# tolerance profile but breaks Eval's ZERO-TOLERANCE CoefficientRow anchor
# harness (ci_lo/ci_hi ARE anchor fields, Doctrine ①):
#   - graph/*.cpp    : bin/grid-edge `xmin + step*i` (np.linspace internals)
#   - estimation/estimation_support.cpp, estimation/did_estimation.cpp:
#                        CI bounds `b ± kZ975*se` in the margins BUG-006
#                        AME remap (cobraToEstimationM4) and csdid's
#                        _coef_row (coefRowM4, feeding estat simple/event)
#   - graph/coefplot_estimates_block.cpp: coefplot's non-default-level() CI
#                        recompute `b ± zAlpha*se` (runCoefplot)
# (Eval: ~29% of random (b,se) pairs diverge; missed pre-merge only by
# thin corpus/debug_all coverage of these exact rows. Post-fix: 24/24
# anchor GREEN + coefplot level90 adversarial GREEN, zero regression.)
# Scoped to just these numpy-parity files — NOT applied globally, since no
# other TU in this build has been shown to need it.
set_source_files_properties(
  src/graph/graph_support.cpp
  src/graph/commands.cpp
  src/graph/distribution_math.cpp
  src/graph/distribution_commands.cpp
  src/graph/twoway.cpp
  src/graph/svg.cpp
  src/graph/files.cpp
  src/graph/view.cpp
  src/estimation/estimation_support.cpp
  src/estimation/estimation_results.cpp
  src/descriptive/descriptive_statistics.cpp
  src/estimation/panel_estimation.cpp
  src/estimation/likelihood_support.cpp
  src/estimation/categorical_estimation.cpp
  src/estimation/vector_time_series.cpp
  src/estimation/multiple_imputation.cpp
  src/estimation/dynamic_panel_estimation.cpp
  src/estimation/nonlinear_estimation.cpp
  src/estimation/mixed_estimation.cpp
  src/estimation/survey_commands.cpp
  src/estimation/survival_estimation.cpp
  src/estimation/binary_estimation.cpp
  src/estimation/estimation_options.cpp
  src/estimation/did_estimation.cpp
  src/estimation/iv_specification.cpp
  src/estimation/instrumental_variables.cpp
  src/estimation/system_estimation.cpp
  src/postestimation/bootstrap_inference.cpp
  src/estimation/censored_estimation.cpp
  src/estimation/selection_estimation.cpp
  src/estimation/quantile_estimation.cpp
  src/estimation/mediation_estimation.cpp
  src/estimation/inference_statistics.cpp
  src/estimation/count_estimation.cpp
  src/estimation/did_two_period.cpp
  src/postestimation/postestimation_context.cpp
  src/postestimation/hypothesis_tests.cpp
  src/postestimation/margins.cpp
  src/postestimation/prediction.cpp
  src/postestimation/estimation_diagnostics.cpp
  src/estimation/time_series_estimation.cpp
  src/postestimation/model_comparison.cpp
  src/datamgmt/data_splines.cpp
  src/tables/esttab_block.cpp
  src/results/stored_estimates_snapshot.cpp
  src/results/stored_estimates_commands.cpp
  src/results/estimates_save_use.cpp
  src/graph/coefplot_estimates_block.cpp
  src/tables/estimates_table.cpp
  src/graph/twoway_opts.cpp
  # Mata invsym(): StataBE's invsym() rounds every product and difference
  # separately; its %21x bits match unfused arithmetic and not FMA.
  src/matrix/mata_subset.cpp
  # ml (moptimize lf): StataBE's gradient, Hessian and step arithmetic match
  # separately rounded products and sums, never FMA (scratch-r150/ml_tail).
  src/estimation/ml_estimation.cpp
  # `matrix` expressions (matrix/matrix_expression.cpp): StataBE's products and inv() round
  # every product and sum separately; fused, `(0.1,0.7)*(0.2\0.6)` differs in
  # the last bit and inv() of a singular X'X misses its zero pivot.
  src/engine/command_support.cpp
  src/time/time_panel_state.cpp
  src/datamgmt/observation_selection.cpp
  src/descriptive/data_listing.cpp
  src/results/scalar_results.cpp
  src/matrix/matrix_expression.cpp
  src/matrix/matrix_commands.cpp
  src/datamgmt/label_metadata.cpp
  src/datamgmt/preserve_restore.cpp
  src/language/temporary_names.cpp
  src/descriptive/levels_command.cpp
  src/environment/workspace_commands.cpp
  src/environment/logging_commands.cpp
  src/environment/filesystem_commands.cpp
  src/language/confirm_command.cpp
  src/environment/package_availability.cpp
  src/environment/timer_commands.cpp
  # LAPACK DSYEVR translation (Mata symeigensystem() in StataBE): the fused
  # multiply-adds of StataBE's gfortran build are written as std::fma at the
  # same sites; nothing else may be contracted.
  src/matrix/lapack_dsyevr.cpp
  PROPERTIES
  COMPILE_FLAGS "-ffp-contract=off"
)

# Monorepo root (piet/native/../..) — M2 LINKS cobra's src/io/columnar.{h,cpp}
# for hbin IO instead of reimplementing it (Master Plan: read-only compile
# reuse; piet/native never edits anything under src/).
get_filename_component(MONOREPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# R1 (selftest split, 2026-09-01): one OBJECT library holds every shared TU;
# two thin executables assemble from it. piet_native no longer links the
# 24k-line selftest.cpp — the suite ships as the sibling `piet_selftest`, over
# the SAME objects, so what is tested is byte-for-byte what is shipped.
add_library(piet_core OBJECT
  src/app/app_support.cpp
  src/app/shell.cpp
  src/app/bridge_internal.cpp
  src/app/routes.cpp
  src/engine/cobra_runner.cpp
  # MAXPERF Stage 1b: persistent `cobra serve` worker client (§25 stateless +
  # §26 resident-frame data plane) the cobra_runner intercept routes to.
  src/engine/cobra_session.cpp
  # ONESHOT-GETCOLS: the shared "fetch only the columns an expression names" pair
  # (contract_A §38). Shared deliberately — see piet/column_fetch.h.
  src/engine/column_fetch.cpp
  src/app/bundle_paths.cpp
  src/app/progress.cpp
  # 2.2.0 item D: persisted app-shell settings + hardware detection (SPEC
  # II-1/II-2) — settings.get/settings.set routes + the on-disk store.
  src/app/settings.cpp
  # M1 evaluator base (C++ port of piet/backend/piet/router/* — see
  # include/piet/eval/*.h; differential-parity harness in tests/eval_diff/)
  src/eval/errors.cpp
  src/eval/tokenizer.cpp
  src/eval/gettoken.cpp
  src/eval/pyvalue.cpp
  src/eval/pyeval.cpp
  src/eval/exprfns.cpp
  src/eval/macro_text.cpp
  src/eval/macro_scanners.cpp
  src/eval/macros.cpp
  src/eval/macro_results.cpp
  src/eval/macro_expressions.cpp
  src/eval/macro_matrices.cpp
  src/eval/macro_definitions.cpp
  src/eval/macro_selftest.cpp
  src/eval/evalcase.cpp
  src/eval/evalcase_ext.cpp
  # M2 glue + vertical slice (see ROADMAP.md §2.0.0 "M2")
  src/engine/datastore.cpp
  src/session/session_run_control.cpp
  src/session/session_state.cpp
  src/session/session_logs.cpp
  src/session/session_scratch.cpp
  src/session/session_use_cache.cpp
  src/session/session_variable_metadata.cpp
  src/session/session_resident_card.cpp
  src/session/session_engine_routing.cpp
  src/session/session_estimation_sample.cpp
  src/session/session_eval_hooks.cpp
  src/session/session_resident_head.cpp
  src/session/session_resident_card_fold.cpp
  src/session/session_estimates.cpp
  src/session/session_lifecycle.cpp
  src/session/session_dataset.cpp
  src/session/session_fit_state.cpp
  src/session/session_memory.cpp
  src/session/session_metadata.cpp
  src/session/session_time_metadata.cpp
  # BETA campaign Phase A4: the c()-class system-value registry (BS-091/
  # BS-101) — single source for every `c(name)` read (display, gen/replace's
  # cobra-forwarded RHS, if/while conditions, scalar/local `= expr`).
  src/language/cclass.cpp
  src/dispatch/dispatcher.cpp
  src/dispatch/options_support.cpp
  src/dispatch/data_support.cpp
  src/dispatch/expression_support.cpp
  src/dispatch/graphics_support.cpp
  src/dispatch/prefix_support.cpp
  src/dispatch/results_support.cpp
  src/dispatch/route_data_combine.cpp
  src/dispatch/route_data_generate.cpp
  src/dispatch/route_data_names.cpp
  src/dispatch/route_data_rows.cpp
  src/dispatch/route_describe.cpp
  src/dispatch/route_environment.cpp
  src/dispatch/route_estimation.cpp
  src/dispatch/route_extensions.cpp
  src/dispatch/route_graphics.cpp
  src/dispatch/route_io.cpp
  src/dispatch/route_postestimation.cpp
  src/dispatch/route_results.cpp
  src/dispatch/route_session.cpp
  src/dispatch/route_table_family.cpp
  src/dispatch/route_tabulation.cpp
  src/dispatch/route_time.cpp
  src/dispatch/session_support.cpp
  src/dispatch/stage_by_prefix.cpp
  src/dispatch/stage_macro_expansion.cpp
  src/dispatch/stage_prefixes.cpp
  src/dispatch/stage_program_resolution.cpp
  src/dispatch/stage_qualifiers.cpp
  src/dispatch/stage_raw_blocks.cpp
  src/dispatch/stage_raw_files.cpp
  src/dispatch/stage_raw_programs.cpp
  src/dispatch/stage_resampling.cpp
  src/dispatch/stage_tokenization.cpp
  src/dispatch/stage_validation.cpp
  src/dispatch/summary_support.cpp
  src/dispatch/text_support.cpp
  src/dispatch/validation.cpp
  # Shared mutationResult builder (see include/piet/mutation_result.h): the
  # one definition used by every data-changing handler; do not add per-file
  # copies.
  src/engine/mutation_result.cpp
  src/estimation/mean_proportion_estimation.cpp
  src/estimation/linear_estimation.cpp
  src/fileio/dataset_use.cpp
  src/estimation/linear_estimation_support.cpp
  src/datamgmt/data_generate.cpp
  src/descriptive/describe.cpp
  src/descriptive/summarize.cpp
  src/datamgmt/assert_command.cpp
  src/estimation/estimation_sample_install.cpp
  src/estimation/estimation_sample_carry.cpp
  src/fileio/user_paths.cpp
  src/estimation/ml_estimation.cpp
  # DSYEVR(V,A,L) from Netlib LAPACK 3.12.0 for ml's `difficult` step.
  src/matrix/lapack_dsyevr.cpp
  src/app/run_block.cpp
  # BETA campaign Phase A0: the command registry (registry.h/.cpp) — a
  # hand-transcribed description of dispatcher.cpp's routing table, feeding
  # `--dump-registry`, tools/freeze_matrix.py, and selftest.cpp's
  # testRegistryRoutes() drift guard (see registry.h's doc comment).
  src/dispatch/registry.cpp
  # M3a: control flow + expanded surface (see ROADMAP.md §2.0.0 "M3")
  src/engine/translate.cpp
  src/language/loops.cpp
  src/language/numlist_expansion.cpp
  src/language/if_while_condition.cpp
  src/language/varlist_expansion.cpp
  src/parallel/parallel_block.cpp
  src/language/legacy_for_command.cpp
  src/parallel/parallel.cpp
  src/parallel/parallel_auto.cpp
  # SPEC II-4 (2026-09-02): memory pre-flight gate before data-size-scaling verbs.
  src/engine/preflight.cpp
  src/parallel/parallel_effects.cpp
  src/parallel/parallel_paths.cpp
  src/language/doexec.cpp
  src/engine/command_support.cpp
  src/time/time_panel_state.cpp
  src/datamgmt/observation_selection.cpp
  src/descriptive/data_listing.cpp
  src/results/scalar_results.cpp
  src/matrix/matrix_expression.cpp
  src/matrix/matrix_commands.cpp
  src/datamgmt/label_metadata.cpp
  src/datamgmt/preserve_restore.cpp
  src/language/temporary_names.cpp
  src/descriptive/levels_command.cpp
  src/environment/workspace_commands.cpp
  src/environment/logging_commands.cpp
  src/environment/filesystem_commands.cpp
  src/language/confirm_command.cpp
  src/environment/package_availability.cpp
  src/environment/timer_commands.cpp
  src/environment/type_and_dir_commands.cpp
  src/environment/ado_paths_and_packages.cpp
  src/environment/shell_translation.cpp
  src/environment/sleep_and_error_commands.cpp
  src/environment/environment_command_support.cpp
  src/datamgmt/winsor.cpp
  src/matrix/matrix_to_variables.cpp
  src/matrix/variables_to_matrix.cpp
  src/matrix/matlist.cpp
  src/results/return_list.cpp
  src/matrix/mata_subset.cpp
  src/datamgmt/gtools_aliases.cpp
  src/datamgmt/gtools_support.cpp
  src/datamgmt/gquantiles_pctile.cpp
  src/datamgmt/gstats.cpp
  src/tables/table_options.cpp
  src/tables/table_text.cpp
  src/tables/table_number_format.cpp
  src/tables/table_files.cpp
  src/tables/table_model_context.cpp
  src/tables/table_transform.cpp
  src/tables/table_composition.cpp
  src/tables/table_estadd.cpp
  src/tables/table_estpost.cpp
  src/tables/estout_models.cpp
  src/tables/estout_grammar.cpp
  src/tables/estout_format.cpp
  src/tables/table_esttab.cpp
  src/tables/table_estout.cpp
  src/tables/table_outreg.cpp
  src/tables/table_tex_files.cpp
  src/tables/table_matrix_export.cpp
  src/tables/table_dataset_export.cpp
  src/tables/table_erepost.cpp
  src/tables/table_render.cpp
  src/tables/table_commands.cpp
  src/time/time_declarations.cpp
  # W2D-T01: `ereturn clear|post|local` (+ named refusals for the rest).
  src/results/ereturn_commands.cpp
  src/datamgmt/dataset_shape_options.cpp
  src/datamgmt/dataset_shape_support.cpp
  src/datamgmt/string_split.cpp
  src/datamgmt/xpose.cpp
  src/datamgmt/recast.cpp
  src/datamgmt/insobs.cpp
  src/datamgmt/dataset_stack.cpp
  src/datamgmt/separate_by_level.cpp
  src/datamgmt/mvencode.cpp
  src/datamgmt/random_sampling.cpp
  src/datamgmt/drawnorm.cpp
  src/datamgmt/input_data_block.cpp
  src/datamgmt/data_notes.cpp
  src/descriptive/variable_compare.cpp
  src/datamgmt/cumul.cpp
  src/datamgmt/bsample.cpp
  src/datamgmt/pctile.cpp
  src/language/program.cpp
  src/language/program_syntax.cpp
  src/language/scalar_expression.cpp
  src/language/display_number_format.cpp
  src/language/display_command.cpp
  # M3b: MI-066 data verbs via cobra 0.18.0 (see ROADMAP.md §2.0.0 "M3b")
    src/fileio/dataset_save.cpp
    src/datamgmt/variable_copy_rename.cpp
    src/datamgmt/data_collapse.cpp
    src/datamgmt/data_order.cpp
    src/datamgmt/data_recode.cpp
    src/datamgmt/data_duplicates.cpp
    src/datamgmt/data_reshape.cpp
    src/datamgmt/data_append.cpp
    src/datamgmt/data_merge.cpp
    src/descriptive/panel_descriptive_statistics.cpp
  src/datamgmt/encode.cpp
  src/datamgmt/string_conversion_support.cpp
  src/datamgmt/destring.cpp
  src/datamgmt/decode.cpp
  src/datamgmt/tostring.cpp
    src/datamgmt/data_joinby.cpp
    src/time/time_fill.cpp
    src/datamgmt/data_quantiles.cpp
    src/datamgmt/data_interpolation.cpp
    src/datamgmt/data_observation_expansion.cpp
    src/fileio/delimited_export.cpp
    src/fileio/delimited_import.cpp
    src/fileio/spreadsheet_export.cpp
    src/fileio/spreadsheet_import.cpp
  # The one delimited-text reader behind import delimited / insheet
  # (include/piet/delimited_io.h; data-io-family design-step3.md section 2).
  src/fileio/delimited_io.cpp
  # P6 wave: Stata frames (frame put/drop/prefix, frlink, frget)
  src/datamgmt/frames.cpp
  # P6 wave: bootstrap/jackknife/permute prefixes (PIET-side replicate loop)
  src/estimation/jackknife_permute_prefix.cpp
  src/estimation/bootstrap_prefix.cpp
  src/estimation/resampling_prefix_support.cpp
  src/estimation/rolling_prefix.cpp
  src/estimation/statsby_prefix.cpp
  # Arc F (P3 wave): the seven QE/RD commands' PIET half — rdrobust, rddensity,
  # rdplot, binscatter, eventstudyinteract, did_imputation, sdid. Parse + call
  # the engine + render Stata's tables; no statistics live there.
  src/estimation/regression_discontinuity.cpp
  src/graph/binscatter.cpp
  src/estimation/eventstudyinteract.cpp
  src/estimation/did_imputation.cpp
  src/estimation/synthetic_did.cpp
  src/estimation/engine_report_support.cpp
  src/graph/graph_point_layers.cpp
  src/graph/binscatter_syntax.cpp
  src/graph/binscatter_ereturn.cpp
  src/datamgmt/egen.cpp
  src/datamgmt/generate_by_group.cpp
  src/datamgmt/egen_computed_functions.cpp
  # BUG-471 T2: the one storage-type resolver (gen/egen/replace -> --store-type).
  src/engine/storage_type.cpp
  src/fileio/file_io.cpp
  src/fileio/postfile.cpp
  # serset-twoway SS: the graph-owned serset store + `serset` command family
  src/graph/serset_store.cpp
  src/graph/serset_command.cpp
  src/graph/graph_registration.cpp
  # BETA campaign Phase G (BS-090): shared `in a/b` + `if` row-filter
  # composition for every non-gen/replace command (materialize-idx-then-
  # keepif — see inrange_filter.h's doc comment for why the naive `_n`-fold
  # gen/replace uses doesn't generalize to a plain --keepif call).
  src/engine/inrange_filter.cpp
  # M4a: output-completion surface (see ROADMAP.md §2.0.0 "M4")
  src/descriptive/tabulate.cpp
  src/datamgmt/winsor2.cpp
  src/estimation/estimation_support.cpp
  src/estimation/estimation_results.cpp
  src/descriptive/descriptive_statistics.cpp
  src/estimation/panel_estimation.cpp
  src/estimation/likelihood_support.cpp
  src/estimation/categorical_estimation.cpp
  src/estimation/vector_time_series.cpp
  src/estimation/multiple_imputation.cpp
  src/estimation/dynamic_panel_estimation.cpp
  src/estimation/nonlinear_estimation.cpp
  src/estimation/mixed_estimation.cpp
  src/estimation/survey_commands.cpp
  src/estimation/survival_estimation.cpp
  src/estimation/binary_estimation.cpp
  src/estimation/estimation_options.cpp
  src/estimation/did_estimation.cpp
  src/estimation/iv_specification.cpp
  src/estimation/instrumental_variables.cpp
  src/estimation/system_estimation.cpp
  src/postestimation/bootstrap_inference.cpp
  src/estimation/censored_estimation.cpp
  src/estimation/selection_estimation.cpp
  src/estimation/quantile_estimation.cpp
  src/estimation/mediation_estimation.cpp
  src/estimation/inference_statistics.cpp
  src/estimation/count_estimation.cpp
  src/estimation/did_two_period.cpp
  src/postestimation/postestimation_context.cpp
  src/postestimation/hypothesis_tests.cpp
  src/postestimation/margins.cpp
  src/postestimation/prediction.cpp
  src/postestimation/estimation_diagnostics.cpp
  src/estimation/time_series_estimation.cpp
  src/postestimation/model_comparison.cpp
  src/datamgmt/data_splines.cpp
  src/tables/esttab_block.cpp
  src/results/stored_estimates_snapshot.cpp
  src/results/stored_estimates_commands.cpp
  src/results/estimates_save_use.cpp
  src/graph/coefplot_estimates_block.cpp
  src/tables/estimates_table.cpp
  src/graph/graph_support.cpp
  src/graph/commands.cpp
  src/graph/distribution_math.cpp
  src/graph/distribution_commands.cpp
  src/graph/twoway.cpp
  src/graph/svg.cpp
  src/graph/files.cpp
  src/graph/view.cpp
  # graph-family AI-1/AI-2: graph bar/hbar/box/hbox (bargraph_g.class) + its serset
  src/graph/graph_bar.cpp
  # graph-family AI-4/6/7: graph describe/copy/rename/display/draw
  src/graph/named_graphs.cpp
  # coefplot-family AN-1..AN-4: coefplot 1.8.8's data pipeline (generate(), r())
  src/graph/coefplot.cpp
  src/graph/coefplot_syntax.cpp
  # graph-family AI-13: graph export to png/pdf/eps (no external library)
  src/graph/graph_export_formats.cpp
  src/graph/graph_export_svg_reader.cpp
  src/graph/graph_export_vector.cpp
  src/graph/graph_export_raster.cpp
  # analysis.do acceptance campaign: parenthesised multi-layer twoway
  # (M4a-remaining) — C++ ports of twoway_parse.py (layer-aware raw-text
  # parser) + twoway_opts.py (Stata option -> SVG style-dict translation).
  src/graph/twoway_parse.cpp
  src/graph/twoway_opts.cpp
  # analysis.do acceptance campaign (BUG-032): row-wise string gen/replace/
  # count/keep-if/drop-if — C++ port of piet/backend/piet/strmutate.py, reusing
  # the M1 pyeval/exprfns evaluator; writes results back as factor columns via
  # a CSV round-trip through `cobra convert` (piet/native has no hbin writer).
  src/datamgmt/row_evaluation.cpp
  src/datamgmt/string_generate_replace.cpp
  src/datamgmt/row_conditions.cpp
  # linked, not copied — src/** stays read-only (Master Plan Doctrine ③)
  src/fileio/xlsx.cpp
  src/fileio/insheet.cpp
  src/fileio/outsheet.cpp
  src/fileio/infile.cpp
  src/fileio/fixed_column_import.cpp
  src/fileio/outfile.cpp
  src/fileio/saveold.cpp
  src/fileio/putexcel.cpp
  src/fileio/filefilter.cpp
  src/fileio/file_copy.cpp
  src/fileio/file_command_support.cpp
  src/utility/utility_support.cpp
  src/utility/utility_options.cpp
  src/utility/uniqueness.cpp
  src/utility/missingness.cpp
  src/utility/labels.cpp
  src/utility/string_distance.cpp
  src/utility/sample.cpp
  src/utility/filelist.cpp
  src/utility/series.cpp
  src/utility/rolling.cpp
  src/utility/calendar.cpp
  src/descriptive/lookfor.cpp
  src/datamgmt/ihstrans.cpp
  src/time/sctorezone.cpp
  src/estimation/leebounds_estimation.cpp
  src/graph/grc1leg.cpp
  src/graph/graph_extension_support.cpp
  src/utility/geodesic_distance.cpp
  src/graph/cdfplot.cpp
  src/graph/event_plot.cpp
  src/estimation/oaxaca_decomposition.cpp
  src/estimation/teffects_nnmatch.cpp
  src/estimation/statistics_command_support.cpp
  src/estimation/areg_ivreg2_aliases.cpp
  src/estimation/rank_and_normality_tests.cpp
  src/descriptive/centile_intervals.cpp
  src/time/hodrick_prescott_filter.cpp
  src/estimation/lowess_lpoly_smoothing.cpp
  src/estimation/principal_components.cpp
  src/datamgmt/xi_indicator_expansion.cpp
  src/postestimation/coefficient_column_names.cpp
  src/postestimation/rtable_rclass_lifecycle.cpp
  src/postestimation/fit_default_e_macros.cpp
  src/datamgmt/variable_bulk_rename.cpp
  src/datamgmt/variable_move.cpp
  # AER tier-1 (2026-10-28): one TU per lane; zip_io.h exposes xlsx.cpp's ZIP code.
  src/fileio/zip_commands.cpp
  src/environment/package_requirements.cpp
  src/language/progress_dots.cpp
  src/fileio/postfile_handles.cpp
  src/language/markout.cpp
  src/postestimation/likelihood_ratio_test.cpp
  src/language/command_reply_support.cpp
  src/language/macro_command.cpp
  src/descriptive/variable_search.cpp
  src/descriptive/panel_pattern_description.cpp
  src/descriptive/dataset_comparison.cpp
  src/language/factor_varlist_unab.cpp
  src/datamgmt/value_label_numbering.cpp
  src/datamgmt/bulk_variable_labels.cpp
  src/datamgmt/variable_prefix_rename.cpp
  src/datamgmt/variable_metadata_support.cpp
  src/datamgmt/mvdecode.cpp
  src/datamgmt/integ.cpp
  src/datamgmt/sencode.cpp
  src/datamgmt/fillmissing.cpp
  src/datamgmt/astile.cpp
  src/datamgmt/column_write_support.cpp
  src/matrix/matrix_sort_select.cpp
  # AER scope-2 B2-3: Stata business calendars (bcal create + bofd/dofb).
  # The reader/writer/index arithmetic live apart from the handler so the
  # selftest can pin the .stbcal BYTES against StataBE's own files.
  src/time/bcal.cpp
  # X2-GLM: `glm` and `margins` after it run in-process. estimation_glm.cpp is the
  # PIET half (parse, design, e() posting); the estimator is the engine TU itself,
  # linked here the way columnar.cpp is, so both binaries fit with the same code.
  src/estimation/estimation_glm.cpp
  # E3 scaffold: one refusal-stub handler TU per E3 command (native tasks replace the bodies).
  src/datamgmt/swindex.cpp
  src/estimation/qreg2_estimation.cpp
  src/estimation/lasso_estimation.cpp
  src/postestimation/suest_estimation.cpp
  src/estimation/ritest_inference.cpp
  src/estimation/acreg_estimation.cpp
  ${MONOREPO_ROOT}/src/estimators/glm.cpp
  ${MONOREPO_ROOT}/src/io/columnar.cpp
)

# Usage requirements shared by the core objects and both executables. Every
# former `target_*(piet_native PRIVATE ...)` settings line below now targets
# this INTERFACE library instead, so the three targets cannot drift.
add_library(piet_settings INTERFACE)
target_link_libraries(piet_core PRIVATE piet_settings)

add_executable(piet_native src/app/main.cpp
  src/app/frontend_window.cpp
  src/app/bridge_route_bindings.cpp
  src/app/startup_environment.cpp
  src/app/headless_do_runner.cpp
  src/app/ui_smoke.cpp
  src/app/ui_smoke_driver.cpp
  $<TARGET_OBJECTS:piet_core>)
add_executable(piet_selftest src/selftest.cpp
  src/selftest_support.cpp
  src/selftest_bridge_transport.cpp
  src/selftest_bridge_routes.cpp
  src/selftest_data_storage.cpp
  src/selftest_startup_and_paths.cpp
  src/selftest_language_values.cpp
  src/selftest_command_stabilization.cpp
  src/selftest_settings_and_execution.cpp
  src/selftest_estimation.cpp
  src/selftest_command_contracts.cpp
  src/selftest_language_control.cpp
  src/selftest_file_io.cpp
  src/selftest_data_commands.cpp
  src/selftest_parallel_execution.cpp
  src/selftest_descriptive_statistics.cpp
  src/selftest_matrix_and_returns.cpp
  src/selftest_tables.cpp
  src/selftest_data_combine.cpp
  src/selftest_label_metadata.cpp
  src/selftest_graphics.cpp
  src/selftest_spreadsheets.cpp
  src/selftest_calendars.cpp
  src/selftest_treatment_estimators.cpp
  src/selftest_command_errors.cpp
  src/selftest_postestimation.cpp
  src/selftest_model_diagnostics.cpp
  src/selftest_iv_and_inference.cpp
  src/selftest_likelihood_estimation.cpp
  src/selftest_time_series_estimation.cpp
  src/selftest_stored_estimates.cpp src/selftest_main.cpp
  src/selftest_language_macro_obs_counters.cpp
  src/selftest_postestimation_predict_frame.cpp
  src/selftest_stored_estimates_eststo_prefix.cpp
  src/selftest_language_command_word_forms.cpp
  src/selftest_language_do_include_scope.cpp
  src/selftest_data_by_keep_drop.cpp
  src/selftest_estimation_weights_and_vce.cpp
  src/selftest_matrix_define_and_subscripts.cpp
  src/selftest_mata_subset_getmata.cpp
  src/selftest_iv_ivreg2_first_stage.cpp
  src/selftest_iv_xtivreg2_fe.cpp
  src/selftest_language_loops.cpp
  src/selftest_language_control_flow_and_temporaries.cpp
  src/selftest_data_combine_varlist_options.cpp
  src/selftest_tables_esttab_namelists.cpp
  src/selftest_bootstrap_prefix.cpp
  src/selftest_unimplemented_scaffolds.cpp
  src/selftest_language_macro_functions.cpp
  src/selftest_data_string_functions.cpp
  src/selftest_time_declaration_persistence.cpp
  src/selftest_time_tsset_tsfill.cpp
  src/selftest_stored_estimates_esample.cpp
  src/selftest_estimation_xtreg_fe_weights.cpp
  src/selftest_estimation_vce_option_shapes.cpp
  src/selftest_postestimation_predict_statistics.cpp
  src/selftest_estimation_glm.cpp
  src/selftest_postestimation_margins.cpp
  src/selftest_ereturn_matrix_functions.cpp
  src/selftest_string_conversion_destring_tostring.cpp
  src/selftest_language_in_range_qualifier.cpp
  src/selftest_postestimation_tests_and_rtable.cpp
  src/selftest_ml_estimation.cpp
  src/selftest_variable_storage_types.cpp
  src/selftest_language_variable_subscripts.cpp
  src/selftest_language_string_math_functions.cpp
  src/selftest_string_conversion_tostring_parity.cpp
  src/selftest_language_datetime_functions.cpp
  src/selftest_iv_estimation_options.cpp
  src/selftest_ereturn_commands.cpp
  src/selftest_datamgmt_swindex.cpp
  src/selftest_qreg2_estimation.cpp
  src/selftest_lasso_estimation.cpp
  src/selftest_suest_estimation.cpp
  src/selftest_ritest_inference.cpp
  src/selftest_acreg_estimation.cpp
  src/selftest_file_io_zip_commands.cpp
  src/selftest_program_utility_commands.cpp
  src/selftest_metadata_utility_commands.cpp
  src/selftest_data_generation_utilities.cpp
  src/selftest_matrix_sort_select.cpp
  src/selftest_graphics_lpoly_lowess.cpp
  src/selftest_ereturn_coefficient_matrices.cpp
  src/selftest_graphics_binscatter.cpp
  src/selftest_graphics_bar_and_coefplot.cpp
  src/selftest_data_egen_statistics.cpp
  src/selftest_graphics_histogram_kdensity.cpp
  src/selftest_descriptive_lookfor.cpp
  src/selftest_datamgmt_ihstrans.cpp
  src/selftest_time_sctorezone.cpp
  src/selftest_leebounds_estimation.cpp
  src/selftest_replication_package_regressions.cpp
  src/selftest_unicode_names_core.cpp
  src/selftest_unicode_names_import_validation.cpp
  src/selftest_unicode_names_macros.cpp
  src/selftest_unicode_names_dispatch.cpp
  src/selftest_unicode_names_output_width.cpp
  src/selftest_estimation_collinear_omission.cpp
  src/selftest_display_format_rendering.cpp
  src/selftest_display_format_defaults.cpp
  src/selftest_data_rename_recode_encode.cpp
  $<TARGET_OBJECTS:piet_core>)
target_link_libraries(piet_native PRIVATE piet_settings)
target_link_libraries(piet_selftest PRIVATE piet_settings)

target_include_directories(piet_settings INTERFACE
  include
  src
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${MONOREPO_ROOT}/src            # for columnar.cpp's "io/columnar.h" / "core/error.h"
  ${MONOREPO_ROOT}/vendor         # for columnar.cpp's "nlohmann/json.hpp" (repo-root vendor copy)
)

# WS-D1: the dispatch zone profiler (src/dispatch/dispatch_profile.h). OFF by default —
# when off, every zone scope compiles to nothing and the binary is identical to
# one built without the header, which is what keeps the freeze fixtures and the
# answerbook pins out of its blast radius. Turn it on to reproduce the per-zone
# attribution table:
#   cmake -DPIET_ENABLE_DISPATCH_PROFILE=ON <src> && make piet_native
#   PIET_DISPATCH_PROFILE=1 ./piet_native --do <file>   # dumps to stderr at exit
option(PIET_ENABLE_DISPATCH_PROFILE "Compile the per-zone dispatch profiler" OFF)
if(PIET_ENABLE_DISPATCH_PROFILE)
  target_compile_definitions(piet_settings INTERFACE PIET_ENABLE_DISPATCH_PROFILE=1)
endif()

find_package(Threads REQUIRED)
target_link_libraries(piet_settings INTERFACE Threads::Threads)

# AER scope-2 W3a: `import delimited ..., encoding("latin1")` converts the file
# to UTF-8 before parsing, through iconv(3). glibc carries iconv in libc; macOS
# and the BSDs need the separate libiconv, which is what Iconv_LIBRARIES names
# when it is not builtin. REQUIRED rather than optional on purpose: an encoding()
# that silently did nothing would decode extended ASCII as UTF-8 garbage at rc 0.
find_package(Iconv REQUIRED)
target_link_libraries(piet_settings INTERFACE Iconv::Iconv)

# ── SINGLE VERSION SOURCE (2.0.2 / MI-138 WP-2, postmortem RC6) ──────────
# These three lines are THE one and only definition site for the piet-axis
# version tuple. Everything else DERIVES from the values compiled in here:
#   - routes.cpp's routes::version (the /version bridge route) emits the
#     macros directly;
#   - `piet_native --version` (main.cpp) prints the compiled tuple as JSON;
#   - piet/packaging/package_native_app.sh stamps Info.plist by reading
#     `--version` back OUT of the actual binary being bundled (never a
#     hand-synced copy — the 2.0.1 contract_a Info.plist drift bug class);
#   - `piet_native --selftest`'s version-single-source leg asserts
#     compiled tuple == /version route == packaged Info.plist ==
#     the private coordination tree's releases/CURRENT (per-artifact SKIP
#     where not applicable).
# Do NOT introduce another hardcoded piet version string anywhere in
# piet/native/ or piet/packaging/ — derive from the binary instead.
# (piet/pyproject.toml is EXCLUDED: frozen at 1.16.0, the deprecated Python
# backend line, deletion queued separately — see ROADMAP.)
# contract_a/contract_b mirror internal note CONTRACT-engine.md / CONTRACT.md's
# frozen version headers; bump each together with its .md when a contract
# changes (never let the compiled string drift from the doc).
#
# Held in CMake variables (rather than inline literals) so the Windows
# resource layer below can DERIVE the PE's VERSIONINFO / manifest identity
# from the very same values instead of introducing a second hand-synced copy
# — the RC strings, the manifest assemblyIdentity, and the Inno installer's
# AppVersion all trace back to PIET_VERSION_VALUE here. Editing these three
# lines is still the one and only version bump.
set(PIET_VERSION_VALUE    "A.0.2")
set(PIET_CONTRACT_A_VALUE "1.26.0")
set(PIET_CONTRACT_B_VALUE "2.10.0")
target_compile_definitions(piet_settings INTERFACE
  PIET_VERSION_STRING="${PIET_VERSION_VALUE}"
  PIET_CONTRACT_A_VERSION="${PIET_CONTRACT_A_VALUE}"
  PIET_CONTRACT_B_VERSION="${PIET_CONTRACT_B_VALUE}"
)

if(APPLE)
  # WEBVIEW_COCOA path (webview.h auto-detects __APPLE__) needs WebKit +
  # Cocoa. webview.h itself only strictly requires WebKit; Cocoa/ObjC runtime
  # come along transitively via the system frameworks on macOS toolchains.
  find_library(WEBKIT_FRAMEWORK WebKit REQUIRED)
  find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
  target_link_libraries(piet_settings INTERFACE ${WEBKIT_FRAMEWORK} ${COCOA_FRAMEWORK})
  # webview.h's Cocoa backend is Objective-C++ under the hood (uses the id/SEL
  # objc runtime directly via objc_msgSend, manual retain/release — NOT
  # written for ARC). Since we #include it from a .cpp, force Objective-C++
  # compilation for the translation unit that reaches into WEBVIEW_COCOA
  # (shell.cpp is the only one that includes webview.h) WITHOUT -fobjc-arc:
  # ARC would reject the header's raw (id) bridging casts (see MIGRATION.md
  # upstream — the library manages Cocoa object lifetimes itself).
  set_source_files_properties(src/app/shell.cpp PROPERTIES
    COMPILE_FLAGS "-x objective-c++"
  )
elseif(UNIX)
  # WEBVIEW_GTK path — needs webkit2gtk + gtk3 via pkg-config. This is the
  # Linux target Phase D ports to; M0's docker compile-check exercises this
  # branch (see docker/Dockerfile.linux-webkitgtk-check, added for this
  # milestone).
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(WEBKIT2GTK REQUIRED webkit2gtk-4.1)
  target_include_directories(piet_settings INTERFACE ${WEBKIT2GTK_INCLUDE_DIRS})
  target_link_libraries(piet_settings INTERFACE ${WEBKIT2GTK_LIBRARIES})
  target_compile_options(piet_settings INTERFACE ${WEBKIT2GTK_CFLAGS_OTHER})
elseif(WIN32)
  # MUST MATCH the engine's own define (the root CMakeLists.txt if(WIN32) block)
  # AND the mman shim's compile line (docker/Dockerfile.windows). MinGW-w64's
  # off_t is 4 bytes without this and 8 with it; piet_native compiles the engine's
  # columnar.cpp, which calls the shim's mmap(..., off_t). A mismatch is not a
  # compile error — it silently passes a half-width offset, so every .hbin mapping
  # fails at runtime ("mmap failed") on the first `use`. Found exactly that way.
  target_compile_definitions(piet_settings INTERFACE _FILE_OFFSET_BITS=64)

  # WEBVIEW_EDGE path (webview.h auto-selects it from _WIN32). Phase D / WP-W1.
  #
  # The COM interface *types* come from the vendored Microsoft WebView2 SDK
  # header (vendor/webview2/WebView2.h, BSD-3-Clause — see vendor/VENDOR.md).
  # WEBVIEW_MSWEBVIEW2_BUILTIN_IMPL=1 (webview.h's default) means the WebView2
  # loader is resolved at runtime via LoadLibrary/GetProcAddress — no import
  # library, no WebView2Loader.dll shipped.
  target_include_directories(piet_settings INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/webview2)
  # webview.h lists its Win32 import libs via `#pragma comment(lib, ...)`, but
  # those pragmas are guarded by `#ifdef _MSC_VER` — a no-op under MinGW-w64.
  # Name them explicitly for the ld link line. uuid supplies the CLSID_/IID_
  # symbols for IFileOpenDialog (WP-W3); ole32/oleaut32 for COM; shlwapi/
  # shell32/shobjidl surfaces; user32/gdi32 window; version/advapi32/dwmapi
  # per webview.h's own pragma list (dwmapi is dlopen'd at runtime, harmless
  # to also import). Matches docker/Dockerfile.piet-windows's link flags so a
  # plain cross-cmake links without external -l flags.
  target_link_libraries(piet_settings INTERFACE
    ole32 oleaut32 shlwapi shell32 user32 gdi32 version advapi32 uuid dwmapi)
  # Ship as a GUI (WINDOWS) subsystem binary so a double-click does NOT pop a
  # console window (the macOS .app has no terminal either). main() still runs
  # (MinGW's -mwindows bridges WinMain->main); the CLI modes reattach to the
  # parent console at startup (see main.cpp) so --do/--selftest still print.
  target_link_options(piet_native PRIVATE -mwindows)

  # ── Windows resource layer: icon + version info + process manifest ──────
  # Explorer/taskbar/shortcut branding, truthful version metadata, and the
  # UTF-8 / long-path / per-monitor-DPI process manifest are embedded IN the
  # PE (no sidecar .manifest file beside the exe — a sidecar is ignored once
  # a manifest resource exists, and gets lost by the installer anyway).
  #
  # Sources: resources/windows/{piet.rc.in,piet.manifest.in,resource.h}.
  # Everything version-shaped is derived from PIET_VERSION_VALUE above.
  enable_language(RC)

  # VS_FIXEDFILEINFO's FILEVERSION/PRODUCTVERSION are four 16-bit integers;
  # PIET's release axis (for example, "B.7.4") is not, so map it onto a numeric
  # tuple here and keep the human-readable string for the RC string table.
  # A -> 1.x.y.0, B -> 0.x.y.0 (B is the pre-1.0 axis, so it sorts BELOW A);
  # a plain X.Y.Z is taken literally. Anything else is a hard error rather
  # than a silently wrong "0.0.0.0" in the shipped binary.
  if(PIET_VERSION_VALUE MATCHES "^B\\.([0-9]+)\\.([0-9]+)$")
    set(PIET_WIN_VERSION_MAJOR 0)
    set(PIET_WIN_VERSION_MINOR "${CMAKE_MATCH_1}")
    set(PIET_WIN_VERSION_PATCH "${CMAKE_MATCH_2}")
    set(PIET_WIN_VERSION_BUILD 0)
  elseif(PIET_VERSION_VALUE MATCHES "^A\\.([0-9]+)\\.([0-9]+)$")
    set(PIET_WIN_VERSION_MAJOR 1)
    set(PIET_WIN_VERSION_MINOR "${CMAKE_MATCH_1}")
    set(PIET_WIN_VERSION_PATCH "${CMAKE_MATCH_2}")
    set(PIET_WIN_VERSION_BUILD 0)
  elseif(PIET_VERSION_VALUE MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    set(PIET_WIN_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(PIET_WIN_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(PIET_WIN_VERSION_PATCH "${CMAKE_MATCH_3}")
    set(PIET_WIN_VERSION_BUILD 0)
  else()
    message(FATAL_ERROR
      "PIET_VERSION_VALUE ('${PIET_VERSION_VALUE}') is not a supported release "
      "version: expected B.X.Y, A.X.Y, or X.Y.Z. Extend the mapping in "
      "piet/native/CMakeLists.txt's WIN32 branch when the version axis changes.")
  endif()

  set(PIET_WINDOWS_RESOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/windows_resources")
  file(MAKE_DIRECTORY "${PIET_WINDOWS_RESOURCE_DIR}")
  # Forward slashes throughout: an .rc string literal treats backslash as an
  # escape character, so a native Windows path baked into the generated
  # piet.rc would mangle (\b, \t, ...). CMake paths are already '/'-form;
  # TO_CMAKE_PATH makes that explicit and survives a -D override.
  file(TO_CMAKE_PATH "${PIET_WINDOWS_RESOURCE_DIR}/piet.manifest"
       PIET_WINDOWS_MANIFEST_PATH)
  file(TO_CMAKE_PATH "${MONOREPO_ROOT}/branding/PIET.ico"
       PIET_WINDOWS_ICON_PATH)

  # branding/PIET.ico is a GENERATED, checked-in artifact
  # (branding/make_windows_ico.py packs the canonical cobra-icon-<N>.png set
  # verbatim into a PNG-compressed ICO). If a checkout lacks it, try to
  # regenerate into the build tree — never into the source tree — and fall
  # back to an iconless build with a loud diagnostic rather than failing
  # windres on a missing file. Configure with -DPIET_REQUIRE_WINDOWS_ICON=ON
  # to make the fallback fatal instead (release builds should).
  option(PIET_REQUIRE_WINDOWS_ICON
         "Fail configuration if the Windows application icon is unavailable" OFF)
  set(PIET_WINDOWS_ICON_GENERATOR "${MONOREPO_ROOT}/branding/make_windows_ico.py")
  if(NOT EXISTS "${PIET_WINDOWS_ICON_PATH}" AND EXISTS "${PIET_WINDOWS_ICON_GENERATOR}")
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
      set(PIET_WINDOWS_ICON_REGEN "${PIET_WINDOWS_RESOURCE_DIR}/PIET.ico")
      execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${PIET_WINDOWS_ICON_GENERATOR}"
                --output "${PIET_WINDOWS_ICON_REGEN}"
        RESULT_VARIABLE PIET_WINDOWS_ICON_REGEN_RC
        OUTPUT_VARIABLE PIET_WINDOWS_ICON_REGEN_OUT
        ERROR_VARIABLE  PIET_WINDOWS_ICON_REGEN_ERR)
      # STREQUAL "0", not EQUAL 0: a failure to even launch the interpreter
      # puts a message string (not a number) in RESULT_VARIABLE.
      if(PIET_WINDOWS_ICON_REGEN_RC STREQUAL "0" AND EXISTS "${PIET_WINDOWS_ICON_REGEN}")
        message(STATUS
          "piet: branding/PIET.ico absent; regenerated into the build tree "
          "(${PIET_WINDOWS_ICON_REGEN})")
        file(TO_CMAKE_PATH "${PIET_WINDOWS_ICON_REGEN}" PIET_WINDOWS_ICON_PATH)
      else()
        message(STATUS
          "piet: branding/PIET.ico regeneration failed "
          "(exit ${PIET_WINDOWS_ICON_REGEN_RC}): "
          "${PIET_WINDOWS_ICON_REGEN_OUT}${PIET_WINDOWS_ICON_REGEN_ERR}")
      endif()
    endif()
  endif()

  if(EXISTS "${PIET_WINDOWS_ICON_PATH}")
    set(PIET_WINDOWS_ICON_RESOURCE "IDI_PIET ICON \"${PIET_WINDOWS_ICON_PATH}\"")
  elseif(PIET_REQUIRE_WINDOWS_ICON)
    message(FATAL_ERROR
      "Windows application icon not available: ${PIET_WINDOWS_ICON_PATH} does "
      "not exist and could not be regenerated. Run "
      "`python3 branding/make_windows_ico.py` (needs branding/cobra-icon-{16,32,"
      "64,128,256}.png and no third-party modules), then re-configure.")
  else()
    message(WARNING
      "piet: building piet_native.exe WITHOUT an application icon — "
      "${PIET_WINDOWS_ICON_PATH} does not exist and could not be regenerated "
      "(needs python3 plus branding/cobra-icon-{16,32,64,128,256}.png). "
      "Explorer, the taskbar and the installer shortcuts will show the generic "
      "executable icon. Fix: run `python3 branding/make_windows_ico.py` and "
      "re-configure; use -DPIET_REQUIRE_WINDOWS_ICON=ON to make this fatal.")
    set(PIET_WINDOWS_ICON_RESOURCE
        "/* no application icon: branding/PIET.ico was unavailable at configure time */")
  endif()

  # resource.h is copied (not substituted) so the generated piet.rc's
  # `#include "resource.h"` resolves next to itself even if windres' include
  # search differs from the compiler's.
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/resource.h"
    "${PIET_WINDOWS_RESOURCE_DIR}/resource.h"
    COPYONLY)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/piet.manifest.in"
    "${PIET_WINDOWS_MANIFEST_PATH}"
    @ONLY)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/piet.rc.in"
    "${PIET_WINDOWS_RESOURCE_DIR}/piet.rc"
    @ONLY)
  target_sources(piet_native PRIVATE "${PIET_WINDOWS_RESOURCE_DIR}/piet.rc")
  target_include_directories(piet_settings INTERFACE "${PIET_WINDOWS_RESOURCE_DIR}")

  # columnar.cpp's <sys/mman.h> mmap has no MinGW header/impl; the cross-build
  # (docker/Dockerfile.piet-windows) supplies docker/windows-compat's shim via
  # -I + a static libmman_compat.a on the link line — a build-environment
  # concern, not wired here (piet/native does not own docker/).
endif()
