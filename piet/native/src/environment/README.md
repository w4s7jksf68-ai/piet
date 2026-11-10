# Environment maintenance

This directory holds the commands that act on the session's working environment rather
than on the dataset: working directory, `clear`, `set obs`, log files, timers, file and
directory commands, the translated `shell`, ado search paths and the offline package
commands. Each file owns one command family; the declarations are in
`piet/handlers/session_commands.h`. Start with the command owner below, then read the
`run...` function it names. The routes that call them are `route_session.cpp` and
`route_environment.cpp` in `dispatch/`.

| Command or symptom | Owner |
|---|---|
| `clear` (bare, `all`, `results`, `matrix`, `programs`, `mata`), `set obs`, `cd`, `pwd` | `workspace_commands.cpp` |
| `log using/close/off/on`, named logs, log format and file name | `logging_commands.cpp` |
| `mkdir`, `rmdir`, `erase`, `rm` | `filesystem_commands.cpp` |
| `timer on/off/clear/list`, `r(t#)`/`r(nt#)` | `timer_commands.cpp` |
| `type FILE`, `dir`, `ls` | `type_and_dir_commands.cpp` |
| `shell` verb allowlist and its translation to file operations | `shell_translation.cpp` |
| `sysdir`, `adopath`, `findfile`, ado autoload lookup (`findOnAdopath`), `net`, `ado` | `ado_paths_and_packages.cpp` |
| `which`, `ssc` (offline answers) | `package_availability.cpp` |
| `require` grammar and its accept-note | `package_requirements.cpp` |
| `sleep #`, `error #` and its message table | `sleep_and_error_commands.cpp` |
| Shared reply blocks, quote stripping, cwd, shell-style tokenizer, glob, integer parse | `environment_command_support.cpp`, `environment_command_support.h` |

`environment_command_support.h` is the only private header. It declares the helpers in
`environment_detail` that `type_and_dir_commands.cpp`, `shell_translation.cpp`,
`ado_paths_and_packages.cpp` and `sleep_and_error_commands.cpp` use; only files in this
directory may include it, and a file that imports `environment_detail` must not also
import `command_detail`. The older files (`workspace_commands.cpp`, `logging_commands.cpp`,
`filesystem_commands.cpp`, `timer_commands.cpp`, `package_availability.cpp`) instead use
`command_detail` and `session_vocab` through `engine/command_support.h`.
`package_requirements.cpp` takes its reply helpers from `language/command_reply_support.h`.

State lives in `SessionState`, never in this directory. `timer_commands.cpp` reads and
writes `session.timers` (slots 1 to 100) and the `r()` keys of the touched slot; `clear
all` in `workspace_commands.cpp` resets the same array. `ado_paths_and_packages.cpp` keeps
`session.sysdir` and `session.adopath`, seeds them on first use, and stores the `net set
ado` target and current `net from` site in the same map under lower-case keys. `cd`
also updates `session.eval.cwd` and the process-wide calendar search directory.
`logging_commands.cpp` registers no state of its own: it calls the `SessionState` log
methods, and its option vocabulary comes from `estimation/estimation_options.cpp`
(`kVocabLog`, `kVocabMkdir`). `type_and_dir_commands.cpp`, `ado_paths_and_packages.cpp`
and `package_requirements.cpp` register their own option vocabularies in file-scope
statics; keep each registration beside the handler that reads it, so an accepted option is
never dropped unread. `shell` starts no OS process: a verb outside its allowlist or an
unquoted pipe or redirect is refused by name.

Compile flags: `workspace_commands.cpp`, `logging_commands.cpp`, `filesystem_commands.cpp`,
`package_availability.cpp` and `timer_commands.cpp` are on the `-ffp-contract=off` list in
`piet/native/CMakeLists.txt`. The CMake comments give the reason for the list, not for
each file: StataBE-matching arithmetic is computed with separately rounded products and
sums, never fused multiply-add. No file here carries an in-file `#pragma clang fp
contract(off)`. Keep the flag if one of these files is moved or split.

Selftests: `selftest_file_io.cpp` (file family, environment commands, `log using`,
`mkdir`/`rmdir`), `selftest_settings_and_execution.cpp` (timer),
`selftest_program_utility_commands.cpp` (`require`) and
`selftest_replication_package_regressions.cpp` (`clear all` keeps macros).

Not here: `confirm` is in `language/confirm_command.cpp`, `preserve`/`restore` in
`datamgmt/preserve_restore.cpp`, `bcal` calendars in `utility/calendar.cpp`, `use`, `save`
and `export` in `fileio/`, and `parallel` in `parallel/`. The command
registry and the option-name resolution that feeds these handlers are in `dispatch/`.
