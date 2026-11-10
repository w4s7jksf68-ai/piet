// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command support.

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

#include "command_support.h"
#include "estimation/estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace command_detail {}
using namespace command_detail;
using namespace session_vocab;

namespace command_detail {

// A5-04: forward to the shared strutil copy (piet/strutil.h) — kept as thin
// wrappers (not removed) so this file's many trim(...)/lower(...) call
// sites didn't need touching. Pure dedup, no behavior change.
std::string trim(const std::string& s) { return strutil::trim(s); }

std::string lower(std::string s) { return strutil::lower(std::move(s)); }

ojson engineErrorBlock(const std::string& message, const std::string& code) {
  return eval::engineError(message, code);
}

// B5-D14 (BUG-454): an error whose Stata rc is KNOWN but which coarseRc's
// message rules cannot recognise -- same shape as utility/utility_support.h's
// utilErrorRc. `reason` is the only channel carrying an exact rc (BUG-187)
// and it outranks the coarse mapping.
ojson engineErrorBlockRc(const std::string& message, int rc) {
  return eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                           "r(" + std::to_string(rc) + ")");
}

// BUG-038(a): forwards to the shared mapping point (piet::cobraErrorMessage,
// cobra_runner.h) — kept as a thin wrapper (not removed) so this file's
// call sites didn't need touching. Pure dedup, no behavior change here.
std::string cobraErrorStr(const CobraError& ce) { return cobraErrorMessage(ce); }

std::string nextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

// BS-001: `gsort` (real cobra verb, cmds_data.cpp's cmd_gsort) prints a
// plain-text status line to STDERR and JSON only on error — runCobraJson()'s
// "parse stdout as JSON" contract doesn't fit, so runSort (below) calls the
// raw runCobra() and builds the CobraError manually from its stderr. File-
// local copy of the identically-named helper in datamgmt/data_append.cpp (this
// codebase's established per-TU convention — see this file's own trim()/
// lower() forwarding-wrapper note above) rather than a new shared header.
CobraError errorFromRaw(const CobraRunResult& r) {
  std::string stderrText = r.stderr_text;
  while (!stderrText.empty() &&
         (stderrText.back() == '\n' || stderrText.back() == '\r' || stderrText.back() == ' ')) {
    stderrText.pop_back();
  }
  try {
    nlohmann::json payload = nlohmann::json::parse(stderrText);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                        err.at("message").get<std::string>(), err.value("detail", std::string()));
    }
  } catch (const nlohmann::json::exception&) {
  }
  std::string firstLine = stderrText;
  auto nl = firstLine.find('\n');
  if (nl != std::string::npos) firstLine = firstLine.substr(0, nl);
  if (firstLine.empty()) firstLine = "cobra exited " + std::to_string(r.exit_code);
  return CobraError("engine_error", firstLine, stderrText);
}

// fix/verb-overhead-sweep: name-only lookup over the resident shadow card
// (session.shadowVarlistIfCurrent()) — the same mechanism fix/sort-validate-
// lazy introduced for runSort below, reused here by runDrop/runKeep so
// neither needs session.datastore() (a full mmap + header parse of however
// many MB the current hbin is) just to answer "does column X exist". Plain
// linear scan over VarInfo entries, mirroring Datastore::has()'s own O(cols)
// name lookup (cecon::Dataset::has) — nothing this file does needs it to be
// more than that.
// C4-L2A sibling (D1 card plane): the same card lookup handing back the ENTRY
// rather than a yes/no — runConfirm needs dtype alongside the name. File-local
// twin of datamgmt/datamgmt_support.h's identically-named S4a helper (the
// established per-TU convention — see that file's own note); cardHasName
// forwards to it rather than keeping a second copy of the scan.
const VarInfo* cardFind(const std::vector<VarInfo>& card, const std::string& name) {
  for (const auto& info : card) {
    if (info.name == name) return &info;
  }
  return nullptr;
}

bool cardHasName(const std::vector<VarInfo>& card, const std::string& name) {
  return cardFind(card, name) != nullptr;
}

// Extract a quoted-or-bare first argument after `word` in `raw` (mirrors the
// commands.py `mkdir`/`erase` regex: `\bWORD\s+("([^"]+)"|'([^']+)'|(\S+))`).
std::string firstArgAfter(const std::string& raw, const std::string& word) {
  std::regex re("\\b" + word + "\\s+(\"([^\"]+)\"|'([^']+)'|(\\S+))", std::regex::icase);
  std::smatch m;
  if (std::regex_search(raw, m, re)) {
    return m[2].matched ? m[2].str() : (m[3].matched ? m[3].str() : m[4].str());
  }
  return "";
}

}  // namespace

// ---------------------------------------------------------------------------

}  // namespace piet::handlers
