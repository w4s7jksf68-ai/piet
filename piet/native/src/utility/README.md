# Utility command maintenance

The former `handlers_aer_util.cpp` mixed unrelated data, filesystem and calendar
commands. Start with the command owner:

| Command or symptom | Owner |
|---|---|
| `unique`, `gunique`, `distinct` counts | `uniqueness.cpp` |
| `missings`, `mdesc` | `missingness.cpp` |
| `labmask`, `renvarlab`, `splitvallabels` | `labels.cpp` |
| Byte-wise `strdist` | `string_distance.cpp` |
| Program `marksample` marker | `sample.cpp` |
| `filelist` directory traversal and dataset output | `filelist.cpp` |
| `carryforward`, `tsspell` | `series.cpp` |
| `rangestat` window parsing and aggregation | `rolling.cpp` |
| `bcal` command, sample selection and calendar options | `calendar.cpp` |

`options.cpp` registers all original vocabularies once, in the original order.
Keep option acceptance and the consuming handler synchronized; an accepted option
must not disappear unread. Public command entry points remain in `piet/handlers.h`.

`support.cpp` owns shared errors, text/option access, data views and writes. Its
private `support.h` exposes only the helpers consumers use, plus `ReadScope`,
`ColView` and `CsvTable`. `ReadScope` retains sole ownership of its temporary
projection for the duration of a command; do not copy the borrowed column pointers
past that scope. Session state remains owned by the original `SessionState`.

Writes still use the established column conversion, `session.load` and
`mutationResult` paths. The calendar module calls the existing `bcal` library;
rolling and spell results are placed back in original row order. Keep these
boundaries when changing a command instead of adding a second evaluator or store.

This is a source-ownership change. Existing utility, calendar, qualifier and
state-lifecycle selftests plus the integration suite remain the behavior checks;
function bodies and option registrations are preserved by the extraction.
