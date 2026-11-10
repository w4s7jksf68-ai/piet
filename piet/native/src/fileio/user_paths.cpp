// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/user_paths.h"
#include "piet/eval/exprfns.h"  // B3(c): exprHasRunningSum

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>

#include "estimation/estimation_internal.h"  // e(sample) T2: the esample helpers' declarations
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/counters.h"  // PP-L1 use-plane split counters (runUse)
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/storage_type.h"  // BUG-471 T2: --store-type
#include "piet/strmutate.h"      // BUG-1326: assert's string-condition count
#include "core/exact_sum.h"  // cecon::ExactSum: summarize's accumulation (BUG-1013)
#include "estimators/posthest.h"  // fitted design atoms for named absorbed effects
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)
#include "io/columnar.h"  // cecon::hbin_level_unescape: .dta label text (BUG-1372)
#include "piet/strutil.h"
#include "piet/translate.h"


namespace piet {

namespace {

// Same dequote rule handlers::runUse/runCd/the U7 shims all used
// independently before this consolidation (strip surrounding whitespace,
// then one layer of matching outer quotes).
std::string dequoteUserPath(std::string s) {
  size_t a = s.find_first_not_of(" \t");
  size_t b = s.find_last_not_of(" \t");
  if (a == std::string::npos) return "";
  s = s.substr(a, b - a + 1);
  if (s.size() >= 2 &&
      ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

bool isAbsoluteUserPath(const std::string& p) {
  if (p.empty()) return false;
  if (p[0] == '/') return true;
#ifdef _WIN32
  // Drive-letter ("C:\" / "C:/") or UNC/rooted-backslash forms. Dead code
  // on the current POSIX-only build of this TU (the native-verb handlers have no other
  // _WIN32 branch yet) — added ahead of the eventual Windows port of the
  // native-verbs surface so the ONE resolver already knows the rule instead
  // of a second fork appearing when that port lands.
  if (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':') return true;
  if (p[0] == '\\') return true;
#endif
  return false;
}

}  // namespace

std::string typedUserPath(const std::string& rawPath) { return dequoteUserPath(rawPath); }

std::string resolveUserPath(const SessionState& session, const std::string& rawPath) {
  std::string path = dequoteUserPath(rawPath);

  // `~` expansion (os.path.expanduser-equivalent, HOME-relative only —
  // `~user` forms are not exercised by the corpus; matches the rule runCd
  // (environment/workspace_commands.cpp) used before this consolidation, now shared logic).
  if (!path.empty() && path[0] == '~') {
#ifdef _WIN32
    std::string home = cecon::env_utf8("USERPROFILE");
#else
    std::string home = cecon::env_utf8("HOME");
#endif
    if (!home.empty()) path = home + path.substr(1);
  }

  if (isAbsoluteUserPath(path)) return path;

  std::string base;
  if (session.cwd.has_value() && !session.cwd->empty()) {
    base = *session.cwd;
  } else {
    // Last-resort getcwd() — headless drivers (--run-block's fresh-session-
    // per-cell harness) never seed session.cwd at all.
#ifdef _WIN32
    // No getcwd() on MinGW that answers in UTF-8 — a Korean cwd would come
    // back CP949 and every path joined onto it would miss. current_path() is
    // wide underneath; u8string() puts it back in piet's one encoding.
    std::error_code cwdEc;
    std::filesystem::path cwd = std::filesystem::current_path(cwdEc);
    base = cwdEc ? "." : cwd.u8string();
#else
    char buf[4096];
    base = getcwd(buf, sizeof(buf)) ? buf : ".";
#endif
  }
  if (base.empty()) return path;  // no base to join against — return the
                                   // raw relative path rather than
                                   // fabricating a bogus "/path" (the old
                                   // resolvePath's base=="/" -> pop_back ->
                                   // "" -> "/"+path collapse conflated "no
                                   // base" with "root base"; see below).
  // Strip a trailing separator EXCEPT when base is exactly "/" (root) —
  // popping that back to "" and then unconditionally prepending "/" is
  // exactly the mechanism that used to build "/<name>" from a root cwd
  // (the M2-era bug this consolidation fixes); handling "/" explicitly here
  // keeps the join to a single '/' either way.
  if (base.size() > 1 && base.back() == '/') base.pop_back();
  if (base.back() == '/') return base + path;
  return base + "/" + path;
}

}  // namespace piet
