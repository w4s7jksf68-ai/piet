// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command entry points: tsset/xtset declarations, tsfill, delta text.
// Implemented under piet/native/src/time/.
#ifndef PIET_NATIVE_HANDLERS_TIME_PANEL_H
#define PIET_NATIVE_HANDLERS_TIME_PANEL_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

// -- M3a: handler-surface expansion (ROADMAP M3a item 4) ---------------------

// One tsset/xtset declaration (`tsset [panelvar] timevar` and the two-variable
// `xtset`). Validates the variables, the format, the time values and delta,
// sorts by (panelvar) timevar, installs session.panelvar/timevar/delta + the
// _dta chars and posts the declaration's r() set under `rCmd`. BUG-896.
struct TsDeclare {
  std::optional<std::string> panelvar;  // nullopt = time series only
  std::string timevar;
  std::optional<double> delta;          // delta(#); nullopt = 1
  std::string period;                   // "yearly", ... ("" = none)
  std::string format;                   // format(%fmt) ("" = none)
  // The display options tsset and xtset take: panelname()
  // relabels the panel line and displayindent() indents every line, each label
  // right-aligned to the longest; noblank and mi change nothing this build draws
  // (it draws no blank line). `display` = one was typed.
  std::string panelLabel = "Panel";
  int indent = 0;
  bool display = false;
};
// Every declaration answers with the panel/time/delta report lines (BUG-1161).
nlohmann::ordered_json runTsDeclare(const TsDeclare& d, SessionState& session,
                                     const std::string& rCmd);
// Bare `tsset` / `xtset`: re-run the live declaration (Stata's echo and r()).
// `given` carries the options typed with the bare command; `anyOption` = some
// period/format/delta option was typed (then only those are used, as in Stata).
nlohmann::ordered_json runTsRedisplay(SessionState& session, const std::string& rCmd,
                                       const TsDeclare& given, bool anyOption);
// The panel-only form: `xtset ivar` (declare) or its bare re-display. `look`
// carries the display options (TsDeclare's last three fields).
nlohmann::ordered_json runXtsetPanelOnly(const std::string& ivar, SessionState& session,
                                          bool declare, const TsDeclare& look = TsDeclare{});

// `xtset ivar [tvar] [, delta(#)]` — the two-variable form is runTsDeclare
// (validated like tsset since BUG-896); one variable is runXtsetPanelOnly.
nlohmann::ordered_json runXtset(const std::string& ivar, const std::optional<std::string>& tvar,
                                 SessionState& session, std::optional<double> delta = std::nullopt);

// `tsfill [, full]` — BS-081 (BETA D4 batch). Fills gaps in the CURRENT
// tsset/xtset time range (never extrapolates past the observed max — see
// the .cpp header comment for the D4f oracle's own byte-for-byte count
// match, 335 not 336). Requires a prior `tsset timevar` (or `xtset panelvar
// timevar`); real Stata's `full` option (materializes every panelvar x
// timevar combination even for panels that never had ANY observation at a
// given period) has no engine equivalent — honest-rejected, not silently
// dropped.

// `tsfill [, full]` (BUG-897: varlist/if/in are tsfill.ado's r(101); `full`
// forwards cobra tsfill --full).
nlohmann::ordered_json runTsfill(const eval::ParsedCommand& parsed, SessionState& session);

// tsset's delta phrase ("1 unit", "2 quarters", the %tc breakdown); defined in
// time/time_panel_state.cpp beside the tsset/xtset arm that prints it too.
std::string tsDeltaText(double delta, const std::string& fmt);

}  // namespace piet::handlers

#endif  // PIET_NATIVE_HANDLERS_TIME_PANEL_H
