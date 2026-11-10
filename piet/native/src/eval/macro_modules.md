# Macro maintenance

`piet/eval/macros.h` is the existing public interface. Private headers belong only
to this implementation family and declare the helpers needed across its files.

| Change | Owner |
|---|---|
| Expansion order, nested/inline expansion and expansion notes | `macros.cpp` |
| Local/global/scalar definitions and extended macro functions | `macro_definitions.cpp` |
| Lexical patterns and definition/reference scanners | `macro_scanners.cpp` |
| Quotes, names and string replacement | `macro_text.cpp`, `macro_name_scan.h` |
| Arithmetic, function arguments and numeric text | `macro_expressions.cpp` |
| Result-store lookup | `macro_results.cpp` |
| Matrix references, stripes and elements | `macro_matrices.cpp` |
| Existing evaluator selftests | `macro_selftest.cpp` |

Expansion and definition evaluation intentionally call each other. Preserve the
existing pass order, bounded recursion, errors and single-pass definition rules;
the file boundary does not authorize changing the language. The inline-note queue
has one thread-local definition in `macros.cpp`; regex caches stay function-local
in their owning implementation. `macro_name_scan.h` carries the shared template
definition so all instantiations see the same implementation.

Use existing macro/language and postestimation selftests plus end-to-end command
checks: successful compilation alone cannot establish expansion order or return
store behavior. Preserve both Windows and POSIX branches when changing file matching.
