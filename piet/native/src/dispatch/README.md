# Command dispatch maintenance

`../dispatcher.cpp` is the ordered entry and outer lifecycle. Read it first for
cross-cutting behavior, then the single stage or route involved in the symptom.

1. Raw blocks, do/program syntax and output/estimate/version/survey prefixes.
2. Definition capture, resampling/randomization, macro expansion and verbatim handling.
3. Tokenization, qualifiers, option resolution and common command validation.
4. `by` processing, program resolution, empty-data handling and arity checks.
5. Functional `route_*.cpp` groups call command implementations.
6. The existing outer wrapper publishes return stores / `_rc` and tees logs.

A stage returns `true` only when it completed the command; its result is in `blocks`.
References passed to stages are the original entry's variables. Do not move a guard or
profiler lifetime into a stage: early return and recursive dispatch depend on the outer
scope. Keep enclosing command returns separate from returns inside callbacks/lambdas.

`pipeline.h` declares stages/routes. `types.h` owns implementation-private lifetime
helpers. Each `*_support.h` declares only its owning `*_support.cpp` helpers;
callers include the specific headers they use. `pipeline.h` exposes stages/routes
without importing every helper declaration. The common `types.h` still carries
shared session/lifetime dependencies; this is not a set of independent libraries. `options_support.cpp` retains the original vocabulary registration
expressions/order. `registry.cpp` remains authoritative for command identity; do not
add a second identity/abbreviation registry here.

A new ordinary command usually needs its implementation, the existing registry entry
and the relevant route family. Prefixes or parsing rules need the relevant stage and
full integration checks. See `docs/ARCHITECTURE.md` for the actual tests and state map.
