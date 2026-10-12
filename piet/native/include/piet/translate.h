// translate — C++ port of piet/backend/piet/router/translate.py's
// time-series operator detection + context injection (MI-083,
// CONTRACT-engine §16). M3a scope: ONLY the TS-token/_inject_ts_context
// slice needed to turn the 2 Eval M2-ratification carve-out cells GREEN
// (`feols y L.x1[...]` with/without xtset). The full build_estimation_argv
// / build_iv_argv / build_csdid_argv / ... translate surface stays on the
// Python oracle beyond this milestone (M3b+); the M2 vertical-slice
// handlers.cpp::runRegress keeps building its own argv inline and calls
// into this module only for the TS pieces.
#ifndef PIET_NATIVE_TRANSLATE_H
#define PIET_NATIVE_TRANSLATE_H

#include <string>
#include <vector>

#include "piet/session_state.h"

namespace piet::translate {

// translate.py's _RE_TS_TOKEN: true if `token` starts with a time-series
// operator sequence (L./F./D./S., optionally numbered or numlist-
// parenthesised) followed by a dot, e.g. `L.x`, `L2.x`, `L(1/3).x`.
bool isTsToken(const std::string& token);

// _has_ts_token: true if ANY token in `tokens` is a TS-operator expression.
bool hasTsToken(const std::vector<std::string>& tokens);

// BUG-370: one time-series operator found inside an expression, with the byte
// range it occupies. `token` is the WHOLE thing the engine's expression parser
// would consume — `L.y`, `L2.D.gdp` — base variable included, so a caller can
// substitute a materialised column in its place by position.
struct TsTokenRef {
  std::string token;
  size_t begin = 0;
  size_t end = 0;  // one past the last byte
};

// Every TS operator in `expr`, in order of appearance. The token boundary is
// decided by cecon::is_ts_prefix — the SAME predicate frame.cpp's parser uses to
// take the TS reading — so what this reports and what the engine resolves cannot
// drift apart. Quote-aware. exprHasTsToken is defined on top of this.
std::vector<TsTokenRef> extractTsTokens(const std::string& expr);

// BUG-369: true if an EXPRESSION (gen/replace RHS, an `if` condition) carries a
// time-series operator in the `<LFDS-run>.<ident>` form cobra's expression parser
// resolves. Quote-aware. The paren-numlist variants isTsToken accepts are
// varlist-only and deliberately not matched here.
bool exprHasTsToken(const std::string& expr);

// injectTsContext, gated on exprHasTsToken(expr) — send the session's tsset/xtset
// metadata only when the expression actually needs it, so an expression with no
// TS operator cannot start failing because a since-dropped timevar is named.
std::vector<std::string> injectTsContextForExpr(const std::vector<std::string>& argv,
                                                const std::string& expr,
                                                const SessionState& session);

// B3(c): the draw coordinates (`set seed` + a per-call stream), appended only
// when the expression contains runiform()/runiformint(). Takes a NON-const
// session because it advances the stream counter.
std::vector<std::string> injectRngContextForExpr(const std::vector<std::string>& argv,
                                                 const std::string& expr,
                                                 SessionState& session);

// _inject_ts_context: appends --xtset/--tsset to `argv` per session's
// panelvar/timevar (CONTRACT-engine §16):
//   panelvar set -> --xtset panelvar[,timevar]
//   only timevar set -> --tsset timevar
//   neither set -> no-op (cobra surfaces ts_no_tsset cleanly)
// Returns a NEW vector; does not mutate `argv`.
std::vector<std::string> injectTsContext(const std::vector<std::string>& argv,
                                          const SessionState& session);

}  // namespace piet::translate

#endif  // PIET_NATIVE_TRANSLATE_H
