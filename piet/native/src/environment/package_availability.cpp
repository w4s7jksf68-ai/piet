// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// `which` and `ssc`: offline package-availability answers.

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

#include "engine/command_support.h"
#include "estimation/estimation_options.h"
#include "piet/handlers/session_commands.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace command_detail {}
using namespace command_detail;
using namespace session_vocab;

// `which NAME` — always rc0 (offline: we say every package is installed).
ojson runWhich(const std::string& raw, const std::vector<std::string>& varlist) {
  std::string name = !varlist.empty() ? varlist[0] : trim(firstArgAfter(raw, "which"));
  ojson out;
  out["type"] = "display";
  out["text"] = "(which: '" + name + "' -- PIET: assumed installed)";
  return out;
}

// `ssc install PKG [, replace]` / other ssc subcommands — noop rc0 (offline).
ojson runSsc(const std::vector<std::string>& varlist) {
  std::string sub = !varlist.empty() ? lower(varlist[0]) : "";
  std::string name = varlist.size() > 1 ? varlist[1] : "";
  if (sub == "install") {
    ojson out;
    out["type"] = "display";
    out["text"] = "(ssc install " + name + ": PIET is offline -- skipped)";
    return out;
  }
  return ojson::array();  // describe/uninstall/... -- silent noop
}

}  // namespace piet::handlers
