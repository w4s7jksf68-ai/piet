// cclass.h — BETA Phase A4 c()-class system-value registry (BS-091/BS-101).
//
// Single source for every `c(name)` read, whichever context it appears in:
// `display`'s bespoke tokenizer (display_hardening.cpp), a `gen`/`replace`
// RHS forwarded row-wise to cobra (dispatcher.cpp), an `if`/`while` condition
// (loops.cpp::evalBoolExpr), or a `scalar NAME = expr` / `local NAME = expr`
// capture (dispatcher.cpp's tryCaptureDefinition call site). Before this
// module, c(current_date)/c(current_time) were the ONLY two keys
// implemented, and only inside display's own resolver — every other context
// (`gen k = c(N)`) had no c() support at all (BS-091's exact finding).
//
// ~15-key spine this build implements: N, k, rc, os, machine_type, username,
// pwd, current_date, current_time, version, level, seed, changed, filename,
// obs (an alias of N — Stata's own canonical observation-count key is c(N);
// c(obs) is accepted as a harmless synonym rather than a distinct concept),
// sysdir_sample (PIET-specific, in Stata's sysdir_* naming family: the
// directory holding the bundled sample datasets, bundle-resolved with the
// same order as `sysuse` — lets a do-file reach the shipped fatalities.csv
// from ANY working directory, no trailing separator).
// Any OTHER c(...) name resolves to nullopt so the caller leaves the token
// untouched and it surfaces an honest downstream NameError/unsupported
// reference instead of silently vanishing (matches the BUG-090/BUG-091
// "never swallow the whole c-class into silence" philosophy).
//
// Known simplifications (documented, not hidden):
//   - c(version): a fixed constant (this build's Stata-syntax emulation
//     target), NOT settable via `version #` — that prefix already has its
//     own, deliberately UNCHANGED, one-shot semantics (BS-041/A1) and this
//     module does not touch it.
//   - c(seed): reflects the last `set seed #` value verbatim; no cobra
//     command in this build's dispatch surface yet consumes it as an actual
//     RNG seed (wildboot/mediate/bootstrap are later D-phase waves), so this
//     is state-tracking + readback only.
//   - c(machine_type): a coarse, compile-time-detected descriptive string,
//     not byte-identical to real Stata's own (machine-specific) text.
#ifndef PIET_NATIVE_CCLASS_H
#define PIET_NATIVE_CCLASS_H

#include <optional>
#include <string>

#include "piet/session_state.h"

namespace piet {

// One resolved c(name) value. `isString` distinguishes a bare numeric token
// (already formatted ready to splice into an arithmetic expression, e.g.
// "960") from text that must be quoted to be a valid string-literal token in
// that same grammar (e.g. text "MacOSX" -> substituteCRefs splices
// "\"MacOSX\""). `text` is always the RAW (unquoted) display-ready value —
// exactly what `display`'s resolver wants directly.
struct CValue {
  bool isString = false;
  std::string text;
};

// Resolves ONE c(name) (case-sensitive, matching Stata's own mostly-lowercase
// c()-class naming). `session` is non-const: c(N)/c(k) may lazily construct
// the Datastore (SessionState::datastore() is non-const).
std::optional<CValue> resolveCValue(const std::string& name, SessionState& session);

// Substitutes every `c(name)` occurrence in `expr` with a literal token
// ready for the arithmetic-expression grammar (bare number for a numeric
// key, a double-quoted string for a text key) — the ONE pre-substitution
// pass every expression-context caller applies before forwarding to cobra's
// row-wise evaluator or to the native Python-subset evaluator. An unresolved
// name is left as literal `c(name)` text, unchanged (never silently
// dropped) — mirrors piet::eval::resolveEstScalarRefs's own established
// per-call-site pre-substitution pattern (dispatcher.cpp's gen/replace arms,
// loops.cpp's evalBoolExpr).
std::string substituteCRefs(const std::string& expr, SessionState& session);

}  // namespace piet

#endif  // PIET_NATIVE_CCLASS_H
