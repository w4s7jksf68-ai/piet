// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Timer commands.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // BUG-540: stataMissingWithKind — `.a`-`.z` identity in matrix literals
#include "piet/eval/macros.h"
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/program.h"  // BUG-922: program::restorePreservesDeeperThan is defined here
#include "piet/serset.h"  // BUG-1461: serset::storageType, signed into `datasignature`
#include "piet/strmutate.h"  // BUG-532: needsStringCount, the string-condition test under by:
#include "piet/stata_format.h"  // eb-order T-P1: `matrix list ..., format()`
#include "piet/strutil.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path / env_utf8 (BUG-050/051)
#include "data/missing.h"   // Stata `.` / `.a`-`.z` kinds + stata_cmp (shared with the engine)
#include "core/stata_arith.h"  // `^` with StataBE's bits (frame.cpp's)

#if defined(__APPLE__)
#include <sys/clonefile.h>  // Q1 (gate ③): APFS copy-on-write snapshot for preserve
#endif  // BUG-1469: the vocabularies below sat inside it, undeclared off macOS

#include "command_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace command_detail {}
using namespace command_detail;
using namespace session_vocab;

ojson runTimer(const std::string& sub, const std::optional<int>& slotArg, SessionState& session) {
  auto slotOk = [](int n) { return n >= 1 && n <= 100; };
  auto formatSlot = [](int slot, const TimerSlot& t) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%4d: %9.2f / %8.0f = %12.4f", slot, t.totalSeconds,
                  static_cast<double>(t.count),
                  t.count > 0 ? t.totalSeconds / static_cast<double>(t.count) : 0.0);
    return std::string(buf);
  };
  auto retireReturns = [&session](int slot) {
    const std::string suffix = std::to_string(slot);
    session.eval.rValues.erase("t" + suffix);
    session.eval.rValues.erase("nt" + suffix);
  };
  auto setReturns = [&session](int slot, const TimerSlot& t) {
    const std::string suffix = std::to_string(slot);
    session.eval.rValues.set("t" + suffix, t.totalSeconds);
    session.eval.rValues.set("nt" + suffix, static_cast<double>(t.count));
  };

  if (sub == "on") {
    if (!slotArg.has_value() || !slotOk(*slotArg)) {
      return engineErrorBlock("timer: invalid syntax", "syntax_error");
    }
    TimerSlot& t = session.timers[static_cast<size_t>(*slotArg)];
    if (!t.running) {
      t.running = true;
      t.startedAt = std::chrono::steady_clock::now();
    }
    return ojson::array();
  }

  if (sub == "off") {
    if (!slotArg.has_value() || !slotOk(*slotArg)) {
      return engineErrorBlock("timer: invalid syntax", "syntax_error");
    }
    TimerSlot& t = session.timers[static_cast<size_t>(*slotArg)];
    if (t.running) {
      double elapsed =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - t.startedAt).count();
      t.totalSeconds += elapsed;
      t.count += 1;
      t.running = false;
    }
    return ojson::array();
  }

  if (sub == "clear") {
    if (slotArg.has_value()) {
      if (!slotOk(*slotArg)) return engineErrorBlock("timer: invalid syntax", "syntax_error");
      session.timers[static_cast<size_t>(*slotArg)] = TimerSlot{};
    } else {
      for (size_t i = 1; i < session.timers.size(); ++i) session.timers[i] = TimerSlot{};
    }
    return ojson::array();
  }

  if (sub == "list") {
    if (slotArg.has_value()) {
      if (!slotOk(*slotArg)) return engineErrorBlock("timer: invalid syntax", "syntax_error");
      const TimerSlot& t = session.timers[static_cast<size_t>(*slotArg)];
      retireReturns(*slotArg);
      if (t.count == 0) return ojson::array();  // empty slot: silent, matches Stata
      setReturns(*slotArg, t);
      ojson out;
      out["type"] = "echo";
      out["text"] = formatSlot(*slotArg, t);
      return out;
    }
    // Bare `timer list`: every slot 1..100 WITH data, in slot order.
    for (int i = 1; i <= 100; ++i) retireReturns(i);
    std::string text;
    for (int i = 1; i <= 100; ++i) {
      const TimerSlot& t = session.timers[static_cast<size_t>(i)];
      if (t.count == 0) continue;
      setReturns(i, t);
      if (!text.empty()) text += "\n";
      text += formatSlot(i, t);
    }
    if (text.empty()) return ojson::array();
    ojson out;
    out["type"] = "echo";
    out["text"] = text;
    return out;
  }

  return engineErrorBlock("timer: unrecognized sub-command" + (sub.empty() ? "" : " '" + sub + "'"),
                           "syntax_error");
}

}  // namespace piet::handlers
