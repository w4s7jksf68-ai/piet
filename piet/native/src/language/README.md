# Language maintenance

This directory holds the do-file language layer: running do-file source, brace blocks and loops, user programs and `syntax`, the `c()` registry, `display` and scalar-expression reading, temporary names and the small macro and program-environment commands. Start from the command or symptom below, then open the single owning file.

| Command or symptom | Owner |
|---|---|
| `do` / `run` / `include` file execution, `///` and brace aggregation, nesting limit, run-block cap, uncaught-error halt, `exit N` | `doexec.cpp` |
| `foreach`, `forvalues`, `while`, `if` blocks, `capture {}` / `quietly {}` / `noisily {}` blocks, `break` / `continue`, loop-body depth | `loops.cpp` |
| Numlist expansion (`1(2)9`, `forvalues` ranges, element cap) | `numlist_expansion.cpp` |
| Varlist wildcard/range expansion and the known-variable check | `varlist_expansion.cpp` |
| Condition text of `if` / `while` evaluated to true/false (`evalBoolExpr`) | `if_while_condition.cpp` |
| Legacy `for` statement (`for X in ...`, `for var`, `for new`) | `legacy_for_command.cpp` |
| `program define` capture, calling a program, `program drop` / `dir`, `return` / `ereturn` / `sreturn` statements, program-exit cleanup | `program.cpp` |
| `syntax` binding against the local `0` (varlist, options, `if` / `in` / `using`) | `program_syntax.cpp` |
| `c(name)` values and `c()` substitution in expressions | `cclass.cpp` |
| `display` statement: items, directives, `_request()`, verbatim fallback | `display_command.cpp` |
| `%fmt` number rendering shared by `display` and `file write` | `display_number_format.cpp` |
| Value references and scalar expressions: `_b[]`, `_se[]`, `e()`, `r()`, `c()`, `_N`, `var[i]`, `evalScalarExpr`, unbound-name refusals | `scalar_expression.cpp` |
| `tempfile`, `tempname`, `tempvar` | `temporary_names.cpp` |
| `macro drop` / `define` / `shift` / `list` / `dir` | `macro_command.cpp` |
| `_dots` progress marks | `progress_dots.cpp` |
| `markout` | `markout.cpp` |
| `confirm` (file, variable, number, scalar, matrix, names) | `confirm_command.cpp` |
| `fvunab` | `factor_varlist_unab.cpp` |
| Reply blocks, option access and number text shared by the small commands | `command_reply_support.cpp`, `command_reply_support.h` |
| Private declarations of one family | `display_internal.h`, `loops_internal.h`, `program_internal.h` |

Each private header is included only by its own family. `display_internal.h` is shared by `display_command.cpp`, `display_number_format.cpp` and `scalar_expression.cpp`; the public entry points stay in `piet/display_hardening.h`. `loops_internal.h` is shared by `loops.cpp`, `numlist_expansion.cpp`, `varlist_expansion.cpp`, `if_while_condition.cpp`, `legacy_for_command.cpp` and, from another directory, `parallel/parallel_block.cpp`; it carries `runBody`, `evalBoolExpr` and the numlist and varlist implementations. `program_internal.h` is shared by `program.cpp` and `program_syntax.cpp` and declares `bindSyntax`, which `program.cpp` calls when it meets a `syntax` line. `command_reply_support.h` is the one support header that crosses directories: besides `macro_command.cpp`, `progress_dots.cpp` and `markout.cpp` it is included by `environment/package_requirements.cpp`, `fileio/postfile_handles.cpp` and `postestimation/likelihood_ratio_test.cpp`.

State and lifetimes a change must respect. All user programs, macros, minted temporary names and the scratch directory belong to `SessionState`; nothing here keeps a copy. `tempfile` creates one scratch directory per run (`runTempdir`), and `tempfile`, `tempname` and `tempvar` record each minted name with the current preserve level so that `program.cpp` (on program return, also when an error unwinds) and `doexec.cpp` (`TempdirReapGuard`) can delete what a call created. `tempname` and `tempfile` draw from the session counter `tempSeq`; `tempvar` uses a single program-wide atomic counter in `temporary_names.cpp` and skips names already present in the dataset. Loop-body depth is a `thread_local` counter in `loops.cpp` (`LoopBodyScope`); `program.cpp` stores it into `SessionState::loopBodyFloor` on entry and the dispatcher reads it, so a loop or program body must keep going through `runBody` or `runProgram`. The option vocabularies of `markout`, `_dots` and `fvunab` are registered once through static objects at namespace scope; an option accepted there must be read by the command.

Compile flags. `temporary_names.cpp` and `confirm_command.cpp` are on the `-ffp-contract=off` list in `piet/native/CMakeLists.txt`; the CMake comment next to them is about `matrix` expressions (products and sums must be rounded separately, never fused), and the file keeps the flag if it moves or is split. `numlist_expansion.cpp` carries an in-file `#pragma clang fp contract(off)` around `start + k * step` so the end test sees the unfused value. No other file here needs the flag.

Tests. `selftest_language_control.cpp`, `selftest_language_control_flow_and_temporaries.cpp`, `selftest_language_loops.cpp` and `selftest_language_do_include_scope.cpp` cover control flow, loops, programs, temporaries and `do` / `include`. `selftest_language_values.cpp`, `selftest_language_variable_subscripts.cpp`, `selftest_language_macro_functions.cpp`, `selftest_language_macro_obs_counters.cpp`, `selftest_language_string_math_functions.cpp`, `selftest_language_datetime_functions.cpp`, `selftest_display_format_defaults.cpp`, `selftest_display_format_rendering.cpp` and `selftest_ereturn_matrix_functions.cpp` cover values, `display` and expression functions. `selftest_program_utility_commands.cpp` covers `macro`, `_dots` and `markout`, `selftest_metadata_utility_commands.cpp` covers `fvunab`, `selftest_file_io.cpp` covers `confirm`, and `selftest_unicode_names_macros.cpp` covers Unicode names in macros and loops. `piet/native/src/SELFTESTS.md` lists the rest.

Not here. Macro expansion, `local` / `global` definitions and the expression evaluators are in `eval/`. The ordered dispatch stages and the `do` / `run` route that calls `doexec.cpp` are in `dispatch/`. `parallel` blocks are in `parallel/`. `preserve` / `restore` and the `marksample` marker are in `datamgmt/` and `utility/`. Public declarations are in `piet/handlers/*.h`, `piet/loops.h`, `piet/program.h`, `piet/doexec.h`, `piet/cclass.h` and `piet/display_hardening.h`.
