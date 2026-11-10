# Utility command maintenance

This directory holds the community data utilities (counting, missingness, labels,
string and geographic distance, file listing, fills, rolling windows, business
calendars). Each command lives in one file and returns result blocks to the route
that called it. Start with the owner row below, then read that file's `run*` entry
point; `utility_support.h` lists the shared helpers it uses.

| Command or symptom | Owner |
|---|---|
| `unique`, `gunique` (one handler, `gunique`-only options refused), `distinct` | `uniqueness.cpp` |
| `missings` (report, dropvars, dropobs, tag), `mdesc` | `missingness.cpp` |
| `labmask`, `renvarlab`, `splitvallabels` | `labels.cpp` |
| `strdist` (byte-wise Levenshtein, `gen()` or two-literal form) | `string_distance.cpp` |
| `marksample` (program-internal marker) | `sample.cpp` |
| `filelist` directory walk and the dataset it builds | `filelist.cpp` |
| `carryforward`, `tsspell` | `series.cpp` |
| `rangestat` window parsing, per-group two-pointer aggregation | `rolling.cpp` |
| `bcal create`, `describe`, `load`, `dir`, `check` command surface | `calendar.cpp` |
| `geodist` (Vincenty or haversine), `geonear` (brute-force nearest neighbours) | `geodesic_distance.cpp` |
| Option vocabularies of the 14 commands above (not `geodist`, `geonear`) | `utility_options.cpp`, `utility_options.h` |
| Shared errors, text and option access, column reads and writes, CSV staging | `utility_support.cpp`, `utility_support.h` |

`utility_support.h` and `utility_options.h` are private to this directory; every
file here except `geodesic_distance.cpp` includes both. `utility_support.h` carries
the `utility_detail` helpers (`utilError`, `utilSyntax`, `optStr`, `hasFlag`,
`writeColumn`, `writeAndConvert`, `viewOf`, `groupIds`, `materializeIndicator`),
`ReadScope`, `ColView` and `CsvTable`. Several of its names also exist, with
different bodies, in `datamgmt/column_write_support.h`; never include both in one
file. `geodesic_distance.cpp` does not use them: it keeps its own copies of the
cobra plumbing and column writer, and includes `graph/graph_extension_internal.h`
for `gxError`. Public declarations are in `piet/handlers/utility.h`, except
`runGeodist` and `runGeonear`, which `piet/handlers/data_values.h` declares.

State a change must respect: option vocabularies are registered at namespace scope,
once per process, by `registerOptVocab` (`kVocabGeodist` and `kVocabGeonear` inside
`geodesic_distance.cpp`, the rest in `utility_options.cpp`). Accepted options must be
read by the handler and refused ones must stay in the unimplemented list. `ReadScope`
owns its temporary projection for the length of one command; the column pointers it
gives out must not outlive it. With `if`/`in` it reads a filtered scratch copy
(registered with `session.noteProjectionScratch`), so row identity is lost: only
the read-only verbs accept a qualifier and every column-writing verb refuses one.
Writes go through the column conversion, `session.load` and `mutationResult`;
`filelist` replaces the dataset and `geonear` sorts the master by base id with
`runSort`. `rangestat` and `tsspell` write results back in original row order.
The code keeps no cache or counter of its own; scratch file names come from
`mintScratchPath`, whose single program-wide sequence lives in `piet/scratch_seq.h`.

Compile flags: no file here is on the `-ffp-contract=off` list in
`piet/native/CMakeLists.txt` and none carries a `#pragma clang fp contract(off)`. That
list exists for the numpy-parity and LAPACK-parity arithmetic of other directories,
and `geodesic_distance.cpp` stays unflagged. Keep that if the file moves or is split.

Selftests: `selftest_data_commands.cpp` (unique, counts, carryforward, rangestat,
tsspell), `selftest_label_metadata.cpp` (renvarlab), `selftest_language_values.cpp`
(strdist), `selftest_calendars.cpp` (bcal), `selftest_program_utility_commands.cpp`
(marksample), `selftest_graphics.cpp` (geodist, geonear) and
`selftest_command_contracts.cpp` (filelist and option refusals).

Elsewhere: command-word routing is in `dispatch/route_extensions.cpp` and
`dispatch/route_environment.cpp` (`bcal`), with names in `dispatch/registry.cpp`. The
business-calendar reader, writer and index arithmetic are in `time/bcal.cpp` and
`piet/bcal.h`. The primitives these commands call (`runDrop`, `runEgen`, `runSort`,
`runRename`) are in `datamgmt/`; the random-sampling verb `sample` is
`datamgmt/random_sampling.cpp`, and `lookfor` is `descriptive/lookfor.cpp`.
