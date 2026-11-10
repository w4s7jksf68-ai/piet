# Evaluator maintenance

This directory holds the language layer below command dispatch: the line
tokenizer, the macro expander, the expression evaluator and its function
library, and the structured error payloads. Public declarations are in
`piet/eval/*.h`; start with the owner of the symptom below.

| Command or symptom | Owner |
|---|---|
| Comments, `///` continuation, `if`/`in`/`[weight]` split, option parsing, `#delimit`/loop/macro rejection, unknown command word | `tokenizer.cpp` |
| `gettoken TOK REST : SRC` (including `(local)`/`(global)` names and the `parse()`, `bind`, `quotes` options) | `gettoken.cpp` |
| Error block shape, `unsupported_*` wording, `_rc` mapping (`coarseRc`) | `errors.cpp` |
| Option abbreviation resolution, per-command option vocabulary registry, unknown-option refusal | `errors.cpp` |
| Order of macro expansion passes, inline expansion notes, `r()`/`e()`/`s()` refusals | `macros.cpp`, `macros_internal.h` |
| `local`/`global`/`scalar` definitions, `local x : func` extended macro functions, failed-expression classification | `macro_definitions.cpp`, `macro_definitions_internal.h` |
| Regexes and hand-written scanners for definitions and references, safe-arithmetic test | `macro_scanners.cpp`, `macro_scanners_internal.h` |
| Quote protection, compound quotes, verbatim text, bare-name substitution, `_N`/`_n` | `macro_text.cpp`, `macro_text_internal.h`, `macro_name_scan.h` |
| `= expr` evaluation, numeric-to-text rendering, c-class names, variable subscripts | `macro_expressions.cpp`, `macro_expressions_internal.h` |
| `_b[]`, `_se[]`, `e()` and `r()` scalar reads, storing a result block into `r()` | `macro_results.cpp`, `macro_results_internal.h` |
| `e(b)`/`e(V)` layout, matrix element and `rowsof`/`colsof` references | `macro_matrices.cpp`, `macro_matrices_internal.h` |
| Python-style values: int/float/str, `str()` and `float()` conversion, UTF-8 slicing | `pyvalue.cpp` |
| Expression parser/evaluator: operators, chained comparison, missing-value arithmetic, math builtins | `pyeval.cpp` |
| Stata function library (math, string, date/time, distribution tails, `runiform`, `regexm`) | `exprfns.cpp` |
| Macro-layer selftests: hand-written scanners against their regex definitions, expansion semantics | `macro_selftest.cpp` |
| `--evalcase` JSON-lines driver and its case kinds | `evalcase.cpp`, `evalcase_ext.cpp`, `evalcase_internal.h` |

`macros.cpp` expands one line by repeated passes (bounded at 16) and calls the
definition and expression files, which call back into it; re-read a neighbour's
entry points before changing one file. The `macro_*_internal.h` headers and
`macros_internal.h` declare `macro_detail` helpers shared only inside the macro
files; nothing outside this directory includes them. `macro_name_scan.h` is a
template, so every instantiation sees one definition. `evalcase_internal.h` is the
seam between `evalcase.cpp` (tokenizer, gettoken and error kinds) and
`evalcase_ext.cpp` (expression and macro kinds). Other directories include the
public `piet/eval/macros.h`, `session.h` and `exprfns.h`; `stata_datemask.h` and
`stata_strfns.h` are header-only and used by `exprfns.cpp`.

State a change must respect. `EvalSession` (`piet/eval/session.h`) is the only
channel the macro files use to the real session; `macro_matrices.cpp` defines its
`resolveEMatrixFor` members, and store generation numbers come from one
process-wide counter in `session.h`. Thread-local: the inline-expansion note queue
(`macros.cpp`), the Stata-division flag (`StataDivisionScope`, `pyeval.cpp`) and the
running-sum stack (`RunningSumRowScope`, `exprfns.cpp`). Process-wide statics:
`exprfns.cpp`'s compiled-regex cache (4096 entries), `regexm` match memo and
`regexs` snapshot, so `regexs()` reads the last successful `regexm()`; and the
option-vocabulary registry in `errors.cpp` (function-local, entries unioned never
overwritten, plus an atomic late-resolution counter). No file here is on the
`-ffp-contract=off` list in `piet/native/CMakeLists.txt` or carries a
`#pragma clang fp contract`; that list covers files that must match separately
rounded products and sums. A file that gains such a need must be added to the list.

Selftests, indexed in `SELFTESTS.md`: `selftest_language_macro_functions.cpp`,
`selftest_language_macro_obs_counters.cpp`, `selftest_language_string_math_functions.cpp`,
`selftest_language_datetime_functions.cpp`, `selftest_language_variable_subscripts.cpp`,
`selftest_language_values.cpp`, `selftest_unicode_names_macros.cpp`,
`selftest_command_errors.cpp` and `selftest_bridge_transport.cpp`. The functions in
`macro_selftest.cpp` are called from the unicode-names and bridge selftests; the
`--evalcase` driver is exercised by `piet/native/tests/eval_diff`.

Elsewhere: command-word identity and abbreviations are in `dispatch/registry.cpp`
(see `dispatch/README.md`); option vocabularies are registered through
`registerOptVocab` (registry in `errors.cpp`) from `dispatch/options_support.cpp`, the
per-directory option files and individual command files. The tokenizer splits
a line and asks that registry whether the command word is routable. Row-wise evaluation of `generate`/`if` over a dataset is not here; this
directory supplies the function namespace it binds against.
