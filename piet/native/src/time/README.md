# Time maintenance

This directory holds the time-series and panel declarations (`tsset`, `tset`,
`xtset`), `tsfill`, the Hodrick-Prescott filter, the `sctorezone` time-zone shift,
and the business-calendar library behind `bofd()`/`dofb()`. It has no private
headers. Start with the command owner below; `time_panel_state.cpp` is where a
declaration's checks, sort, session state and report lines come together.

| Command or symptom | Owner |
|---|---|
| `tsset`/`tset`/`xtset` qualifier and option refusals, period options (`yearly`, `daily`, `clock`, ...), panel-only `xtset id` option rules, display options | `time_declarations.cpp` |
| Declaration itself: key and format checks, delta, sort by (panel) time, `session.panelvar`/`timevar`/`delta`, `_dta` characteristics, `r()` set, report lines (`Time variable`, `Delta`, balance wording) | `time_panel_state.cpp` |
| Bare `tsset`/`xtset` re-display, panel-only `xtset id` (no sort), delta phrase text (`tsDeltaText`) | `time_panel_state.cpp` |
| `tsfill` and `tsfill, full`: refusals, forwarding to the engine, repeated-time error | `time_fill.cpp` |
| `tsfilter hp newvar = y [, smooth() trend()]` | `hodrick_prescott_filter.cpp` |
| `sctorezone` shift of `%tc` variables, `only()`/`exclude()`, wording, error codes | `sctorezone.cpp` |
| `.stbcal` reader, `bofd`/`dofb`, `bcal create` induction, calendar cache and search directory | `bcal.cpp` |

`time_declarations.cpp` implements `commands::runTimeDeclaration`, declared in
`piet/time_declarations.h`; `dispatch/route_time.cpp` calls it after the dispatcher
has resolved common options and the echo. It validates the varlist and options, then
calls `runTsDeclare`, `runTsRedisplay`, `runXtsetPanelOnly` or `runXtset`, which
`time_panel_state.cpp` defines and `piet/handlers/time_panel.h` declares (as it does
`runTsfill`). `bcal.cpp` implements `piet/bcal.h`, which several other directories
include. `tsfilter hp` and `sctorezone` are reached from `dispatch/route_estimation.cpp`
and `dispatch/route_extensions.cpp`. `time_fill.cpp` is reached from
`dispatch/route_data_rows.cpp`.

State and lifetimes. A successful declaration writes the panel variable, time
variable and delta into `SessionState` and calls `writeTsChars()`; `tsfill`,
`tsfilter hp` and the engine-backed commands read those three fields, so do not
set them anywhere else. The option vocabularies for `tsfill` and `sctorezone` are
registered by namespace-scope statics in `time_fill.cpp` and `sctorezone.cpp`, so
they exist before the first dispatch; each registered option must be read by the
handler. The calendar cache and search directory in `bcal.cpp` are process-wide
function-local statics, keyed by lowercased calendar name alone and cleared only
through `bcal::clearCache()` (called when a `SessionState` is constructed). The
evaluator is single-threaded and the cache has no lock.

Compile flags. `time_panel_state.cpp` is on the `-ffp-contract=off` list in
`piet/native/CMakeLists.txt`, under the comment that StataBE rounds each product and
sum separately, so fused multiply-add changes the last bit; the flag must follow the
file if it moves or is split. `time_declarations.cpp` and the other files here have
no such flag, and no file here carries an in-file `#pragma clang fp contract`.

Tests. `selftest_time_tsset_tsfill.cpp` covers `tsset`/`xtset`/`tsfill` and their
refusal codes; `selftest_time_declaration_persistence.cpp` covers the `_dta`
characteristics; `selftest_time_sctorezone.cpp` covers `sctorezone`;
`selftest_calendars.cpp` covers `bcal.cpp`; `selftest_time_series_estimation.cpp`
also exercises `tsfill`. `tsfilter hp` has no dedicated selftest file listed in
`piet/native/src/SELFTESTS.md`.

Elsewhere. The `bcal` command (create, load, describe, options) is in
`utility/calendar.cpp`, not here; `bcal.cpp` only supplies the library. Date
functions such as `bofd()` are evaluated under `eval/`. `newey`, `prais`, `dfuller`, `pperron`
and other time-series estimators are in `estimation/`. `xtdescribe`
output is in `descriptive/`, which calls `tsDeltaText`. Command names, abbreviations
and allowed qualifiers are in `dispatch/registry.cpp`.
