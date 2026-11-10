// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Declarations of the feature selftest entry points. Each case lives in its own
// translation unit, selftest_<domain>_<feature>.cpp (the trailing comment on every
// declaration names it; SELFTESTS.md indexes them all), and reports through
// w2check(), which forwards to the suite's own counter in selftest.cpp. The calls,
// and so the run order, are listed in runSelftest(); the CMake source list names
// every file. Entry-function names are printed by --list-suites and by the
// per-suite runs, so they are fixed even where the file name is functional.
#pragma once
#include <string>

void w2check(bool cond, const std::string& label);  // selftest.cpp
// A SKIP line (counted, not a PASS): for an arm that is knowingly not met and
// is listed as such, e.g. a display-gate mismatch the case cannot close.
void w2skip(const std::string& label, const std::string& why);  // selftest.cpp

void testW2MacroN();  // selftest_language_macro_obs_counters.cpp
void testW2PredictFrame();  // selftest_postestimation_predict_frame.cpp
void testW2EststoPrefix();  // selftest_stored_estimates_eststo_prefix.cpp
void testW2LabelGlued();  // selftest_language_command_word_forms.cpp
void testW2DoInclude();  // selftest_language_do_include_scope.cpp
void testW2ByKeep();  // selftest_data_by_keep_drop.cpp
void testW2EstimOpts();  // selftest_estimation_weights_and_vce.cpp
void testW2Matrix();  // selftest_matrix_define_and_subscripts.cpp
void testW2Getmata();  // selftest_mata_subset_getmata.cpp
void testW2Loops();  // selftest_language_loops.cpp
void testW2ProgCtl();  // selftest_language_control_flow_and_temporaries.cpp
void testW2MergeAppend();  // selftest_data_combine_varlist_options.cpp
void testW2EsttabNames();  // selftest_tables_esttab_namelists.cpp
void testW2BsBootstrap();  // selftest_bootstrap_prefix.cpp
void testW2EgenRename();  // selftest_unimplemented_scaffolds.cpp (empty reserved entry point)
void testW2Esample();  // selftest_stored_estimates_esample.cpp
void testR150Ivreg2Surface();  // selftest_iv_ivreg2_first_stage.cpp
void testR150Xtivreg2Fe();  // selftest_iv_xtivreg2_fe.cpp

// Further feature cases. Entry points with an empty body are folded into
// selftest_unimplemented_scaffolds.cpp.
void testW2bLinearFe();  // selftest_estimation_xtreg_fe_weights.cpp
void testW2bJoinReshape();  // selftest_unimplemented_scaffolds.cpp (empty reserved entry point)
void testW2bStrlWriter();  // selftest_unimplemented_scaffolds.cpp (empty reserved entry point)
void testW2bDelimited();  // selftest_unimplemented_scaffolds.cpp (empty reserved entry point)
void testW2bDataOpts();  // selftest_unimplemented_scaffolds.cpp (empty reserved entry point)
void testW2bIvFamily();  // selftest_estimation_vce_option_shapes.cpp
void testW2bIv();  // selftest_iv_estimation_options.cpp
void testW2bDescXtset();  // selftest_unimplemented_scaffolds.cpp (empty reserved entry point)
void testW2bPredictStats();  // selftest_postestimation_predict_statistics.cpp
void testW2bEsttabScreen();  // selftest_unimplemented_scaffolds.cpp (empty reserved entry point)
void testW2bEgenOpts();  // selftest_unimplemented_scaffolds.cpp (empty reserved entry point)
void testW2bGlm();  // selftest_estimation_glm.cpp
void testW2bCluster3();  // selftest_unimplemented_scaffolds.cpp (empty reserved entry point)
void testW2bMargins();  // selftest_postestimation_margins.cpp
void testW2bMisc();  // selftest_ereturn_matrix_functions.cpp
void testW2cMacroFns();  // selftest_language_macro_functions.cpp
void testW2cDataFns();  // selftest_data_string_functions.cpp
void testW2bDestring();  // selftest_string_conversion_destring_tostring.cpp
void testW2XtsetPersist();  // selftest_time_declaration_persistence.cpp
void testW2Tsset();  // selftest_time_tsset_tsfill.cpp
void testW2dInRange();  // selftest_language_in_range_qualifier.cpp
void testW2dEreturn();  // selftest_ereturn_commands.cpp

// Post-estimation tests and the ml evaluator.
void testW2cPostest();  // selftest_postestimation_tests_and_rtable.cpp
void testW2Ml();  // selftest_ml_estimation.cpp

// Community commands: swindex, qreg2, lasso/rlasso, suest, ritest, acreg.
void testW2E3Swindex();  // selftest_datamgmt_swindex.cpp
void testW2E3Qreg2();  // selftest_qreg2_estimation.cpp
void testW2E3Lasso();  // selftest_lasso_estimation.cpp
void testW2E3Suest();  // selftest_suest_estimation.cpp
void testW2E3Ritest();  // selftest_ritest_inference.cpp
void testW2E3Acreg();  // selftest_acreg_estimation.cpp

// String, math and matrix expression functions; string conversion.
void testW2xFnString();  // selftest_language_string_math_functions.cpp
void testW2zTostring();  // selftest_string_conversion_tostring_parity.cpp

// Graphics (lpoly/lowess) and display-format cases.
void testW2Lpoly();  // selftest_graphics_lpoly_lowess.cpp
void testW2DisplayFormat();  // selftest_display_format_rendering.cpp
void testW2afFormat();  // selftest_display_format_defaults.cpp
void testW2AhDataVerbs();  // selftest_data_rename_recode_encode.cpp

// One coefficient source and one canonical e(b)/e(V).
void testW2Ebmat();  // selftest_ereturn_coefficient_matrices.cpp

// Utility commands: zip archives, program/macro utilities, metadata, data
// generation and matrix sort/select helpers.
void testW2T1Zip();  // selftest_file_io_zip_commands.cpp
void testW2T1Env();  // selftest_program_utility_commands.cpp
void testW2T1Meta();  // selftest_metadata_utility_commands.cpp
void testW2T1Gen();  // selftest_data_generation_utilities.cpp
void testW2T1Matrix();  // selftest_matrix_sort_select.cpp

// binscatter, graph bar and coefplot.
void testW2GfxBinscatter();  // selftest_graphics_binscatter.cpp
void testW2GfxBar();  // selftest_graphics_bar_and_coefplot.cpp

// egen statistics family.
void testW2apEgen();  // selftest_data_egen_statistics.cpp

// local/global/scalar assigned from a variable element, and bare variable reads.
void testW2iVarsub();  // selftest_language_variable_subscripts.cpp

// histogram/hist, twoway__histogram_gen and kdensity.
void testW2aqHistogram();  // selftest_graphics_histogram_kdensity.cpp

// Community commands lookfor, ihstrans, sctorezone and leebounds, plus the
// replication-package regression bundle.
void testW2T7Lookfor();  // selftest_descriptive_lookfor.cpp
void testW2T7Ihstrans();  // selftest_datamgmt_ihstrans.cpp
void testW2T7Sctorezone();  // selftest_time_sctorezone.cpp
void testW2LbLeebounds();  // selftest_leebounds_estimation.cpp
void testW2T7ClearAll();  // selftest_replication_package_regressions.cpp

// Unicode names: shared identifier rule plus one TU per scanner family.
void testW2UniCore();  // selftest_unicode_names_core.cpp
void testW2UniV();  // selftest_unicode_names_import_validation.cpp
void testW2UniM();  // selftest_unicode_names_macros.cpp
void testW2UniD();  // selftest_unicode_names_dispatch.cpp
void testW2UniW();  // selftest_unicode_names_output_width.cpp

// reghdfe/areg/xtreg,fe keep the earlier member of a collinear set.
void testW2C1067();  // selftest_estimation_collinear_omission.cpp
