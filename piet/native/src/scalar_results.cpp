// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Scalar results.

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

ojson runScalarList(const std::vector<std::string>& names, SessionState& session) {
  // BS-038 (BETA campaign C3): real Stata's `scalar list`/`scalar dir`
  // column format — name right-justified to width 10, " = ", value
  // right-justified to width 10 (oracle: `      sca1 =         42`).
  // Values are emitted AS ALREADY STORED (session.eval.scalars' own
  // decimal-string convention, e.g. "42"/"19.5" from the arithmetic
  // evaluator) rather than re-deriving Stata's full %10.0g significant-
  // digit formatter — exact for the common case; a value longer than 10
  // chars is left un-truncated rather than silently mis-rendered (a
  // disclosed, smaller gap than Stata's own overflow/scientific-notation
  // rule, not attempted here). This handler previously existed but was
  // dead code — eval/macros.cpp's tryCaptureDefinition intercepted the
  // bare `scalar list`/`scalar dir` line earlier in the pipeline and threw
  // the result away (a bool-only capture signature has no way to carry a
  // display block) before dispatch ever reached this arm; see that file's
  // own BS-038 comment for the other half of the fix.
  auto padRight10 = [](const std::string& s) {
    return s.size() >= 10 ? s : std::string(10 - s.size(), ' ') + s;
  };
  std::vector<std::string> lines;
  const auto& items = session.eval.scalars.items();
  for (const auto& [name, val] : items) {
    if (!names.empty() && std::find(names.begin(), names.end(), name) == names.end()) continue;
    lines.push_back(padRight10(name) + " = " + padRight10(val));
  }
  // "display", not "echo": `echo` is the command line, which `noisily` drops and
  // `quietly` keeps -- the reverse of what the prefixes do to a listing (BUG-1505,
  // as runEreturnList below).
  ojson out;
  out["type"] = "display";
  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) text += "\n";
    text += lines[i];
  }
  out["text"] = text;
  return out;
}

// ---------------------------------------------------------------------------
// ereturn list — BS-039 (BETA campaign C3): display-only dump of the e()
// values PIET actually stores after an estimation (session.lastModel() +
// its cached raw cobra --tidy JSON, session_state.h's LastModel — PIET's
// e()-equivalent). Bare `ereturn list` ONLY; `ereturn scalar`/`ereturn
// post`/eclass creation stay OUT of scope (BS-109) — dispatcher.cpp's arm
// only routes the bare "list" sub-form here, so any other `ereturn ...`
// form still falls through to the pre-existing generic unsupported_command
// reject, unchanged.
//
// Honest partial dump beats a fabricated complete one: every section below
// emits ONLY fields verified present in PIET's own regress/feols result —
// e(ll)/e(ll_0)/e(rank) and the Stata-internal plumbing macros (e(title),
// e(marginsok), e(properties), e(predict), e(model), e(estat_cmd)) are NOT
// tracked anywhere in this build and are silently omitted rather than
// invented. e(cmd) is a known, disclosed approximation: LastModel only
// tracks the INTERNAL engine command name (cobraCmd, e.g. "feols" for
// regress/reg/r/reghdfe alike) — there is no separate "what the user
// actually typed" field, so e(cmd) can read "feols" where real Stata would
// say "regress" (a pre-existing LastModel gap, not something this
// display-only fix should widen its diff to also close).
ojson runEreturnList(SessionState& session) {
  const std::optional<LastModel>& lm = session.lastModel();
  // eb-order T-P3: an e() can be live with no fitted model behind it (an
  // estpost posting, `ereturn post`, a restored estpost store); its stores are
  // listed like any other (SPEC C, `ereturn list` after estpost).
  if (!lm.has_value() && !session.eval.eLive()) {
    // Real Stata: `ereturn list` with nothing estimated yet errors ("last
    // estimates not found", r(301)) rather than printing an empty/
    // misleading dump.
    return engineErrorBlockRc("last estimates not found", 301);
  }
  const ojson r = lm.has_value() && lm->result.has_value() ? *lm->result : ojson::object();

  auto padLabel = [](const std::string& s) {
    return s.size() >= 22 ? s : std::string(22 - s.size(), ' ') + s;
  };
  auto fmtScalar = [](double v) {
    if (std::isnan(v)) return std::string(".");  // a missing e() scalar (jackknife's e(singleton))
    std::ostringstream oss;
    oss.precision(16);
    oss << v;
    return oss.str();
  };

  std::ostringstream out;
  out << "\nscalars:\n";
  static const std::vector<std::pair<std::string, std::string>> kScalarMap = {
      {"n_obs", "N"}, {"df_m", "df_m"}, {"df_r", "df_r"}, {"F", "F"},
      {"r2", "r2"},   {"rmse", "rmse"}, {"mss", "mss"},   {"rss", "rss"},
      {"adj_r2", "r2_a"},
  };
  std::unordered_set<std::string> emitted;
  for (const auto& [jsonKey, eName] : kScalarMap) {
    if (r.contains(jsonKey) && r.at(jsonKey).is_number()) {
      out << padLabel("e(" + eName + ")") << " =  " << fmtScalar(r.at(jsonKey).get<double>()) << "\n";
      emitted.insert(eName);
    }
  }
  // Anything a handler posted directly to the e-class store. Until AER bundle H
  // this function was driven ENTIRELY by the coeftable JSON plus the fixed map
  // above, so a command like pca -- which has no coeftable and whose returns are
  // e(rho)/e(trace)/e(cond)/e(f)/e(lndet) -- would have printed nothing at all.
  // Appended rather than merged so the regression commands' output is unchanged
  // byte for byte: for those, eScalars is empty and this loop adds no lines.
  for (const auto& [k, v] : session.eval.eScalars.items()) {
    if (emitted.count(k)) continue;  // the coeftable copy already spoke for it
    out << padLabel("e(" + k + ")") << " =  " << fmtScalar(v) << "\n";
  }

  out << "\nmacros:\n";
  // BUG-458 (2026-09-08): read the STORE, which setLastModel now publishes
  // from LastModel (stataCmd -> "regress" where this printed the engine's
  // "feols"; depvar; vce). One source for `e(cmd)', bare e(cmd) and this
  // listing, so they cannot drift. The LastModel fields stay the fallback for
  // a store someone cleared underneath a live model.
  const std::string eCmd = session.eval.eStrings.get("cmd", lm.has_value() ? lm->cobraCmd : "");
  const std::string eDepvar = session.eval.eStrings.get("depvar", lm.has_value() ? lm->depvar : "");
  const std::string eVce = session.eval.eStrings.get("vce", lm.has_value() ? lm->vcov : "");
  if (lm.has_value() || !eCmd.empty()) out << padLabel("e(cmd)") << " : \"" << eCmd << "\"\n";
  if (!eDepvar.empty()) {
    out << padLabel("e(depvar)") << " : \"" << eDepvar << "\"\n";
  }
  if (lm.has_value() || !eVce.empty()) out << padLabel("e(vce)") << " : \"" << eVce << "\"\n";
  // Same rule as the scalars: empty for every pre-H command, so their pins hold.
  for (const auto& [k, v] : session.eval.eStrings.items()) {
    if (k == "cmd" || k == "depvar" || k == "vce") continue;  // said above
    out << padLabel("e(" + k + ")") << " : \"" << v << "\"\n";
  }

  const bool haveCoefTable =
      r.contains("coeftable") && r.at("coeftable").is_array() && !r.at("coeftable").empty();
  if (haveCoefTable || !session.eval.eMatrices.empty()) {
    out << "\nmatrices:\n";
    if (haveCoefTable) {
      size_t k = r.at("coeftable").size();
      // BUG-1207: Stata's e(b) also carries the ancillary parameters the engine's
      // coefficient table leaves out, counted here from the reply (StataBE 19.5:
      // `tobit y x1, ll(5)` 1 x 3 with /var(e.y); ologit on 4 outcome levels 1 x 5
      // with /cut1-/cut3; nbreg 1 x 7 with /lnalpha).
      bool cutRows = false;
      for (const auto& row : r.at("coeftable"))
        if (row.is_object() && row.value("name", std::string()).rfind("/cut", 0) == 0) cutRows = true;
      if (eCmd == "tobit" && r.contains("variance")) {
        k += 1;
      } else if (eCmd == "nbreg" && r.contains("lnalpha")) {
        k += 1;
      } else if ((eCmd == "ologit" || eCmd == "oprobit") && !cutRows && r.contains("cuts") &&
                 r.at("cuts").is_array()) {
        k += r.at("cuts").size();
      }
      out << padLabel("e(b)") << " :  1 x " << k << "\n";
      out << padLabel("e(V)") << " :  " << k << " x " << k << "\n";
    }
    for (const auto& [k, m] : session.eval.eMatrices) {
      if (haveCoefTable && (k == "b" || k == "V")) continue;  // synthesised above
      const size_t nr = m.data.size();
      const size_t nc = nr > 0 ? m.data[0].size() : 0;
      out << padLabel("e(" + k + ")") << " :  " << nr << " x " << nc << "\n";
    }
  }

  // A listing is output, so a "display" block: the prefix filters take an "echo"
  // block for the command line, which `noisily` / `capture noisily` drop and
  // `quietly` keeps, so as "echo" the listing vanished under noisily and leaked
  // under quietly (StataBE 19.5, BUG-1505: noisily prints it, quietly does not).
  ojson block;
  block["type"] = "display";
  block["text"] = out.str();
  return block;
}

// ---------------------------------------------------------------------------
// matrix — P4 sublanguage: literal define, ' / * expressions, rownames/
// colnames, list, accum. Everything downstream that READS a matrix
// (`local : colnames X`, `X[i,j]`, `colsof()`/`rowsof()`) already resolves
// against session.eval.matrices (eval/macros.cpp's substituteMatrixRefs and
// friends) — this section only has to populate that same store, never a
// second one.

std::string matrixNameAsListed(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (size_t i = 0; i < name.size(); ++i) {
    if (name.compare(i, 3, "bn.") == 0 && i > 0 &&
        std::isdigit(static_cast<unsigned char>(name[i - 1]))) {
      ++i;  // drop `bn`, keep the `.`
      continue;
    }
    out += name[i];
  }
  return out;
}


}  // namespace piet::handlers
