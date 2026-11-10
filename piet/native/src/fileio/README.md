# File I/O maintenance

This directory holds the commands that move a dataset or bytes between the session
and files: `use`/`save`, delimited and fixed-format text, `.xlsx`, ZIP archives, the
`file` handle family, `postfile`, and the resolver for user-typed paths. Find the
command in the table, then open that one file; `include/piet/handlers/io.h` declares
the entry points.

| Command or symptom | Owner |
|---|---|
| `use` (`.dta`, native hbin, `.tmp`, virtual tempfile), the convert cache, `.dta` label/characteristic adoption | `dataset_use.cpp` |
| `save` (`.dta` with the metadata sidecar, `.hbin`, sibling-temp commit, refusals r(602)/r(603)) | `dataset_save.cpp` |
| `export delimited` writer, numeric and date spelling, quoting | `delimited_export.cpp` |
| `import delimited` option parsing, encoding, name and type inference | `delimited_import.cpp` |
| Delimited parser, `legalNames`, `typeColumn`, `stageAndAdopt` (shared by every text loader) | `delimited_io.cpp` |
| `export excel`, cell-range option | `spreadsheet_export.cpp` |
| `import excel`, date styles, sheet and cell-range selection | `spreadsheet_import.cpp` |
| `.xlsx` read/write, INFLATE, STORED ZIP writer, worksheet XML; ZIP primitives declared in `zip_io.h` | `xlsx.cpp`, `xlsx.h`, `zip_io.h` |
| `zipfile`, `unzipfile`, `zipsave`, `zipuse` | `zip_commands.cpp` |
| `insheet`, `outsheet`, `infile` free format | `insheet.cpp`, `outsheet.cpp`, `infile.cpp` |
| `infix`, `infile using dict`, fixed columns | `fixed_column_import.cpp`, `fixed_column_import.h` |
| `outfile`, `saveold` | `outfile.cpp`, `saveold.cpp` |
| `putexcel` (cell writes, `set`, merges, `clear`) | `putexcel.cpp` |
| `filefilter`, `copy`; `file open/write/read/seek/set/close` with binary mode | `filefilter.cpp`, `file_copy.cpp`; `file_io.cpp` |
| `postfile`, `post`, `postclose`; `postutil clear`, `postutil dir` | `postfile.cpp`, `postfile_handles.cpp` |
| Typed-path dequoting, `~`, relative-to-cwd resolution | `user_paths.cpp` |
| Shared error shapes, option and word helpers, file commit helpers for the text verbs | `file_command_support.cpp`, `file_command_support.h` |

`file_command_support.h` is private to this directory and is included only by the
text and file verbs (`insheet.cpp`, `outsheet.cpp`, `infile.cpp`,
`fixed_column_import.cpp`, `outfile.cpp`, `saveold.cpp`, `putexcel.cpp`,
`filefilter.cpp`, `file_copy.cpp`). `fixed_column_import.h` is private as well: it
exports the type-token check, the `using ("data")` repair, the empty-dataset rule and
the dictionary reader that `infile.cpp` shares with `infix`. The delimited and
spreadsheet files and `dataset_save.cpp` do not use these headers; they include the
private `datamgmt/data_io_internal.h` and `datamgmt/data_mutation_internal.h`.
`xlsx.h` is also included by other directories, and `zip_io.h` only by `xlsx.cpp`,
`zip_commands.cpp` and a selftest. `insheet`, `outsheet` and `saveold` are thin
shims over the delimited and `.dta` machinery, so keep one reader and one writer
rather than adding a second.

State to respect. Option vocabularies are registered by namespace-scope statics in
each command file, so they exist before any dispatch; an accepted option must be read
by its handler. Open `file` handles and `postfile` handles live in
`SessionState::fileHandles` and `SessionState::postfiles`; the open `putexcel`
workbook lives in `SessionState::putexcel`, reset with the session. `postutil clear`
closes each handle through `runPostclose`. `dataset_use.cpp` keys its convert cache
on inode, size and mtime of the source and treats any stat failure as "not cacheable";
`PIET_NO_USE_CACHE` disables it. `dataset_save.cpp` names its sibling temporary file
from the process id and one program-wide atomic counter. Refusals and echoes name the
path as typed, never the resolved absolute path.

Compile flags: no file in this directory is on the `-ffp-contract=off` list in
`piet/native/CMakeLists.txt` or carries a `#pragma clang fp contract(off)`. If a file
here is ever given one, it must keep it when moved or split.

Tests: `selftest_file_io.cpp` (save/use round trip, file family, text I/O, delimited
export spelling), `selftest_file_io_zip_commands.cpp` (ZIP verbs),
`selftest_spreadsheets.cpp` (`.xlsx` layer and options),
`selftest_unicode_names_import_validation.cpp` (delimited header names),
`selftest_startup_and_paths.cpp` (user-path resolution),
`selftest_time_declaration_persistence.cpp` (tsset carried through save/use),
`selftest_data_storage.cpp` (virtual saves). `selftest_unimplemented_scaffolds.cpp` has an
empty `testW2bDelimited`; delimited options have no dedicated coverage.
`piet/native/src/SELFTESTS.md` is the index.

Elsewhere: `input` is in `datamgmt/input_data_block.cpp`; `append`, `merge` and the
`.dta` byte reader and writer are in `datamgmt/` and the shared `src/io/` library;
`erase` and `mkdir` are in `environment/` and `confirm file` in `language/`; the public delimited
header is `include/piet/delimited_io.h`; command identity and abbreviations are
owned by `dispatch/registry.cpp`.
