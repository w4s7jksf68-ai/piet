# Tables maintenance

This directory holds the table-writing commands: `esttab`, `estout`, `outreg2`,
`estadd`, `estpost`, `estimates table` and the file exporters (`texsave`,
`frmttable`, `regsave`, `parmest`, `mat2txt`). Start with `table_commands.cpp`,
which routes the non-`esttab` commands, then open the owner of the command.

| Command or symptom | Owner |
|---|---|
| Which handler a table command word reaches; `estpost` flags a posted `e()` | `table_commands.cpp` |
| `esttab` screen block: namelist expansion, active-estimate fallback, option vocabulary, `transform()` hand-off | `esttab_block.cpp` |
| `esttab ... using FILE` and the screen form: translation of esttab options into an estout spec (`esttabImpl`) | `table_esttab.cpp` |
| `estout` command, `matrix()` mode, `r()` posted by the writer | `table_estout.cpp` |
| estout cell/label/star/stats grammar (`cells()`, `varlabels()`, `star()`, `stats()`) | `estout_grammar.cpp` |
| estout model values, row selection (`keep`, `drop`, `order`), per-mode defaults, hook lines | `estout_models.cpp` |
| estout number formats, padding, stars, and the row-by-row renderer (`eoRender`) | `estout_format.cpp` |
| `estimates table` (Stata's own comparison table) | `estimates_table.cpp` |
| `outreg2` (ledger grid, `append`, text, TeX and Excel XML output) | `table_outreg.cpp`, `outreg2_internal.h` |
| `estadd` (`local`, `scalar`, `matrix`, `ysumm`, `mean`; `beta`, `vif`, `expb`, `summ`, `sd`, `svy`, `margins` are refused) | `table_estadd.cpp` |
| `estpost summarize` / `tabstat` and the posted pseudo-estimate | `table_estpost.cpp` |
| `erepost b = M` / `V = M` | `table_erepost.cpp` |
| `transform()` parsing and scoped evaluation | `table_transform.cpp` |
| Stored estimate to `Model` (e() scalars, labels, `estadd` bag, active model), factor base-level rows, file emission | `table_model_context.cpp` |
| `texsave`, `texdoc`, `tex` line (dataset to TeX); `frmttable` (matrix to TeX) | `table_tex_files.cpp`, `table_matrix_export.cpp` |
| `regsave`, `parmest`, `mat2txt` (estimates or matrix to a dataset or text file) | `table_dataset_export.cpp` |
| Option vocabularies for every command here; `kEoNaN`, texdoc and frmttable global keys | `table_options.cpp` |
| Error blocks, splitting helpers, star ladders, `using` splitting, label pairs | `table_text.cpp` |
| `fmt()` normalisation and number/star formatting; atomic file write, overwrite guard, extensions | `table_number_format.cpp`, `table_files.cpp` |
| `cells()` sub-grammar and `buildBody` (no caller found outside this file) | `table_composition.cpp` |
| TeX, fixed-width and delimited renderers over `tables::Table` | `table_render.cpp` |

`tables_internal.h` is the private header for everything above: error and text
helpers, `Model`, `CellSpec`, `BodyOpts`, option vocabulary externs, and the
`using` aliases for `tables::Table` and the renderers (`piet/table_render.h`,
`piet/table_model.h`). `estout_internal.h` adds the estout/esttab value, cell,
spec and mode types and includes `tables_internal.h`; `outreg2_internal.h` adds
the outreg2 grid and ledger declarations and does the same. Only files in this
directory include them. The public entry points are declared in
`piet/handlers/estimates_tables.h` and `piet/table_commands.h`.

State and lifetimes. Each option vocabulary is registered at static
initialisation in `table_options.cpp` (and `kVocabEsttab` in `esttab_block.cpp`)
so the dispatcher can resolve abbreviations before routing; an option accepted
there must be read by its handler, and `tabout` is registered only so the
refusal names it. `estadd` and `estpost` write the bag held in `SessionState`
(`estAdded`, keyed by `estAddedFp`); a different active estimate discards it.
`estpost` replaces `e()` wholesale, and `table_commands.cpp` then sets
`ePostedLive` and clears `eOnFitNames`. `outreg2 append` reads the existing file
first, and `emitTable` hands the writer the concatenation, so a destination is
never open for writing while it is still a source. `texdoc` keeps its open path
and `frmttable` keeps stored inputs in `session.eval.globals` under reserved
`__piet_` keys. `TrScopedModel` swaps `lastModelResult` and `coefOverride` for
one `transform()` evaluation and restores both on exit.

Compile flags. `esttab_block.cpp` and `estimates_table.cpp` are on the
`-ffp-contract=off` list in `piet/native/CMakeLists.txt`; the comment above
that list explains the numpy-parity group (separately rounded multiply and add, no fused
multiply-add); the two table files are listed in that group without their own comment. Keep the flag if either file
moves or is split. No file here has an in-file `#pragma clang fp contract(off)`.

Tests. `selftest_tables.cpp` covers the esttab/estout bundle, value labels and
option minimums; `selftest_tables_esttab_namelists.cpp` covers esttab namelists
and `estimates store`/`restore`; `selftest_replication_package_regressions.cpp`
holds table regressions among its 23 cases; `piet_selftest --suite tables` runs
the table subset.

Elsewhere. `eststo`, `estimates store/save/use/drop` and the stored-estimate
snapshot live in `results/`; `coefplot` in `graph/`; the routes that call these
entry points are `route_results.cpp` and `route_table_family.cpp` in `dispatch/`.
`tabout` is refused there and has no writer here.
