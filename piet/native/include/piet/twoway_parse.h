// twoway_parse.h — Layer-aware parser for Stata `twoway` commands (2.0
// analysis.do acceptance campaign). C++ port of
// piet/backend/piet/twoway_parse.py (~261 LOC oracle), byte-for-byte
// mirroring its algorithm so parenthesised multi-layer twoway
// `(rarea ...) (connected ...) (scatter ...)` parses identically to the
// Python backend.
//
// Operates on the RAW post-keyword string — the slice of the command line
// that follows the `twoway` (or `graph twoway`) keyword — so it sees the
// parenthesised layer groups intact (the tokenizer's ParsedCommand::varlist
// already flattens them, which is why this is a SEPARATE parse over the raw
// text rather than a reuse of ParsedCommand).
//
// Public API mirrors the oracle exactly:
//   parseTwoway(rawAfterCmd) -> {layers, globalOpts, ifExpr, inRange}
//   hasToplevelParens(rawAfterCmd) -> bool   (fast pre-check for the dispatcher)
#ifndef PIET_NATIVE_TWOWAY_PARSE_H
#define PIET_NATIVE_TWOWAY_PARSE_H

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "piet/eval/tokenizer.h"

namespace piet::twoway {

// One layer parsed from a `twoway` command (mirrors twoway_parse.py's
// LayerSpec dataclass).
struct LayerSpec {
  std::string plottype;                                       // lower-cased plot-type keyword
  std::vector<std::string> varlist;                            // variable names, Stata order
  std::optional<std::string> ifExpr;                            // per-layer `if`, stripped of "if"
  std::vector<std::pair<std::string, eval::OptValue>> opts;    // per-layer options
};

struct ParseResult {
  std::vector<LayerSpec> layers;                                // first = bottom z-layer
  std::vector<std::pair<std::string, eval::OptValue>> globalOpts;  // post-first-comma options
  std::optional<std::string> ifExpr;                            // global `if` (applies to ALL layers)
  std::optional<std::string> inRange;                           // global `in` range
};

// Parses everything that follows the `twoway` keyword. Mirrors
// twoway_parse.py's parse_twoway() exactly (see its docstring for the
// bare-single-layer vs parenthesised-multi-layer branch semantics).
ParseResult parseTwoway(const std::string& rawAfterCmd);

// Fast dispatcher pre-check: true if `rawAfterCmd` contains at least one
// top-level `(...)` group before the first structural comma. Mirrors
// twoway_parse.py's has_toplevel_parens().
bool hasToplevelParens(const std::string& rawAfterCmd);

}  // namespace piet::twoway

#endif  // PIET_NATIVE_TWOWAY_PARSE_H
