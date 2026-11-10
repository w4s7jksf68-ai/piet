// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Environment commands.

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

namespace command_detail {
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

std::string joinSpaceLocal(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += " ";
    s += v[i];
  }
  return s;
}

// A legal Stata identifier: starts with a letter (ASCII or Unicode) or underscore,
// followed by letters/digits/underscores or other non-ASCII characters, <=32
// characters. Used only by confirm's
// `names`/`name` sub-form (BS-042) — deliberately NOT Stata's full name
// grammar (no reserved-word/`str#`-collision checks — those need real
// infrastructure this handler doesn't have; see runConfirm's own doc
// comment for the honest-skip call).
// Unicode names: the one rule (strutil.h -> core/ident.h), 32 characters.
bool isValidStataName(const std::string& s) { return strutil::isValidStataIdentifier(s); }

// Same quote-aware first-argument extraction as firstArgAfter, but
// anchored to "whatever the line's own first token is" rather than one
// specific literal word — lets a single handler (runErase) serve more than
// one Stata command spelling for the SAME command. BS-045 (BETA campaign
// C4): `rm` is a genuine Stata BUILT-IN synonym of `erase` (live-probed
// `which rm` -> "built-in command: rm", not an invented alias), so
// runErase needs to extract its path argument correctly regardless of
// whether the raw line says "erase ..." or "rm ...".
std::string firstArgAfterCmd(const std::string& raw) {
  std::string t = trim(raw);
  size_t sp = t.find_first_of(" \t");
  if (sp == std::string::npos) return "";
  std::string rest = trim(t.substr(sp));
  static const std::regex reArg(R"re(^("([^"]+)"|'([^']+)'|(\S+)))re");
  std::smatch m;
  if (std::regex_search(rest, m, reArg)) {
    return m[2].matched ? m[2].str() : (m[3].matched ? m[3].str() : m[4].str());
  }
  return "";
}
}  // namespace

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

// `confirm [new] file|variable|numeric|string|integer|number|scalar|
// existence|names ARG` — existence/type check. Error block on failure (a
// `capture` prefix sets _rc via eval::coarseRc); silent (empty array) on
// success, matching Stata.
//
// BETA campaign C4 (BS-042/BS-043): every sub-form below was live-probed
// against StataNow 19.5 to pin its exact rc/message shape (see the C4
// agent's task-return report for the full probe transcript). Two root
// causes fixed this batch:
//   - "new variable" was never recognised as its own sub-command: the old
//     regex only special-cased "new file", so "confirm new variable X"
//     fell through to the varlist-tokenized fallback with sub="new" (an
//     unmatched literal), silently succeeding regardless of X's existence
//     (BS-042). Fixed by generically stripping a "new " prefix off subRaw
//     (mirrors how "new file" was already narrowed to "file") instead of
//     the old file-only `.find("file")` special case.
//   - "confirm integer number N" fed "number N" (not just "N") to the
//     integer parser, since "number" is REQUIRED Stata syntax glue between
//     "integer" and the value, not consumed by the old regex's single
//     "integer" alternative (BS-043). Fixed by stripping an optional
//     leading "number " token off arg specifically in the integer branch
//     (same pattern as the numeric/string branch's own "variable " strip
//     immediately below).
ojson runConfirm(const std::string& raw, const std::vector<std::string>& varlist,
                  SessionState& session) {
  // BUG-1103: `variable' abbreviates down to `v' -- `confirm var x', `confirm v x',
  // `confirm numeric var x' (StataBE 19.5). Spelled out only, `confirm var zz' fell
  // to the silent unknown-subtype branch below at rc 0, where Stata says r(111).
  static const std::string kVariableWord = R"(v(?:a(?:r(?:i(?:a(?:b(?:le?)?)?)?)?)?)?)";
  static const std::regex reConfirm(
      R"re(^\s*confirm\s+(new\s+)re" + kVariableWord +
          R"re(|new\s+file|number|integer|file|)re" + kVariableWord +
          R"re(|numeric|string|existence|names|name|scalar|matrix)\s+(.*))re",
      std::regex::icase);
  auto isVariableWord = [](const std::string& w) {
    return !w.empty() && w.size() <= 8 && std::string("variable").compare(0, w.size(), w) == 0;
  };
  std::smatch m;
  std::string sub, arg;
  bool isNew = false;
  if (std::regex_search(raw, m, reConfirm)) {
    std::string subRaw = lower(trim(m[1].str()));
    arg = trim(m[2].str());
    isNew = subRaw.rfind("new", 0) == 0;
    if (isNew) {
      size_t p = subRaw.find_first_not_of(" \t", 3);
      subRaw = (p == std::string::npos) ? "" : subRaw.substr(p);
    }
    if (isVariableWord(subRaw)) subRaw = "variable";
    sub = subRaw == "name" ? "names" : subRaw;
    if (sub == "numeric" || sub == "string") {
      // The type word takes `variable' (abbreviated as above); anything else there
      // is r(198) "invalid syntax" (`confirm numeric vx ab', `confirm numeric
      // variables ab'; StataBE 19.5).
      static const std::regex reVarKw("^" + kVariableWord + R"((\s+|$))", std::regex::icase);
      std::smatch vk;
      if (!std::regex_search(arg, vk, reVarKw)) return engineErrorBlockRc("invalid syntax", 198);
      arg = arg.substr(static_cast<size_t>(vk.length(0)));
    }
    if (sub == "integer") {
      // BS-043: "confirm integer number 5" — "number " is required syntax
      // glue, not part of the value; strip it the same way numeric/string
      // strip their own "variable " glue two lines up. A bare "confirm
      // integer 5" (no "number") is left untouched (harmless permissive
      // superset, never exercised by the oracle either way).
      static const std::regex reNumberKw(R"(^number\s+)", std::regex::icase);
      arg = std::regex_replace(arg, reNumberKw, "");
    }
    arg = trim(arg);
    if (arg.size() >= 2 && ((arg.front() == '"' && arg.back() == '"') ||
                            (arg.front() == '\'' && arg.back() == '\''))) {
      arg = arg.substr(1, arg.size() - 2);
    }
  } else {
    sub = !varlist.empty() ? lower(varlist[0]) : "";
    if (sub == "name") sub = "names";
    if (isVariableWord(sub)) sub = "variable";
    arg = varlist.size() > 1 ? varlist[1] : "";
    // A bare `confirm numeric` lacks its `variable` word: r(198) (StataBE 19.5).
    if ((sub == "numeric" || sub == "string") && !isVariableWord(lower(arg)))
      return engineErrorBlockRc("invalid syntax", 198);
    if (sub == "numeric" || sub == "string") arg.clear();
  }

  if (sub == "file") {
    std::string path = resolveUserPath(session, arg);
    namespace fs = std::filesystem;
    // BUG-361: ask the SESSION before asking the filesystem. A `save` to a minted
    // tempfile is routed to the engine's in-RAM virtual-file table when the
    // resident plane is live (handlers_data_verbs.cpp's isMintedTempfile arm) and
    // writes no bytes at all, so `fs::exists` answers "no" about a file the user
    // just saved and can still `use` — and answers it with _rc 601, the SAME code
    // a genuinely absent file returns, so a script cannot tell the two apart.
    // Measured 2026-08-19: plain save -> confirm _rc 0; tempfile save -> _rc 601;
    // absent file -> _rc 601, while `use` on that middle path restores all 5 rows.
    // `use` already consults the table (isVirtualSave, handlers.cpp:363); this is
    // the same question asked from the same session, so the two agree by
    // construction rather than by inspection. Both keys come from
    // resolveUserPath(), so they are the same string. A registered entry the
    // engine has since lost still reads as present here — which is what the
    // session believes, and the following `use` is where that failure belongs.
    bool exists = fs::exists(cecon::to_native_path(path)) || session.isVirtualSave(path);
    if (isNew) {
      if (exists) return engineErrorBlock("confirm new file: file already exists: " + path);
    } else {
      if (!exists) return engineErrorBlock("confirm file: file not found: " + path);
    }
    return ojson::array();
  }

  if (sub == "variable" || sub == "numeric" || sub == "string") {
    if (isNew) {
      // BS-042: `confirm new variable NAME [NAME ...]` — multi-name form,
      // ALL names must be new; report the FIRST existing conflict (matches
      // live-probed Stata: `confirm new variable newvar1 x newvar2` with x
      // pre-existing -> "variable x already defined", rc=110, stopping at
      // the first hit even though newvar2 was never checked). With no
      // dataset loaded, every name is trivially new (nothing can conflict).
      std::istringstream iss(arg);
      std::string tok;
      while (iss >> tok) {
        if (session.hasData()) {
          // C4-L2A (D1 card plane): existence is card-answerable — a CURRENT
          // card is authoritative for name presence either way (brief §1
          // confirm row). Falls back to the unchanged datastore() path when
          // the card is unavailable/non-current or under the
          // PIET_NO_RESIDENT_READ escape arm.
          bool exists;
          if (const std::vector<VarInfo>* card = session.residentReadCard()) {
            exists = cardHasName(*card, tok);
          } else {
            exists = session.datastore()->has(tok);
          }
          if (exists) {
            return engineErrorBlock("confirm new variable: variable '" + tok +
                                     "' already defined");
          }
        }
      }
      return ojson::array();
    }
    // BUG-1006: `confirm variable v, exact` (w2aq_histogram_family.do hk_gv, every
    // hk_*_g record). The option list rode into the varlist, so `v,` and `exact`
    // were looked up as names and the check answered r(111) for a variable that
    // exists. `exact` turns off the abbreviation below (BUG-1103); any other
    // option is refused by name, r(198). `exact` abbreviates to `ex`, case
    // matters, and a second one is refused too (`, exa` is exact; `, e`, `, EXACT`
    // and `, exact exact` are r(198); StataBE 19.5).
    bool exact = false;
    if (const size_t comma = arg.find(','); comma != std::string::npos) {
      std::istringstream oss(arg.substr(comma + 1));
      arg = trim(arg.substr(0, comma));
      std::string opt;
      while (oss >> opt) {
        const bool isExact = opt.size() >= 2 && std::string("exact").compare(0, opt.size(), opt) == 0;
        if (!isExact || exact) {
          return engineErrorBlockRc("option " + (isExact ? std::string("exact") : opt) +
                                        " not allowed",
                                    198);
        }
        exact = true;
      }
    }
    if (!session.hasData()) {
      return engineErrorBlock("confirm " + (sub == "variable" ? std::string("variable") : sub) +
                               ": no dataset loaded");
    }
    // BUG-429: `confirm variable A B` is a VARLIST, exactly as `confirm new
    // variable` above already treats it (BS-042). The single-name reading made
    // the whole string one name and answered `variable 'A B' not found` — rc 111
    // where Stata is rc 0 — which is how a probe written against Stata stops on
    // a dataset that has both variables. Every name is checked; the FIRST
    // failure is reported, which is Stata's own order.
    // BUG-1103: a list of NAMES, not a varlist pattern -- `x*` is the name `x` and
    // then `*`, which is "* invalid name" r(198), so a wildcard or a range never
    // expands; no name at all is "'' found where varname expected" r(7) (StataBE 19.5).
    std::vector<std::string> names;
    {
      auto nameChar = [](unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; };
      for (size_t i = 0; i < arg.size();) {
        if (arg[i] == ' ' || arg[i] == '\t') {
          ++i;
          continue;
        }
        size_t j = i + 1;
        if (nameChar(static_cast<unsigned char>(arg[i])))
          while (j < arg.size() && nameChar(static_cast<unsigned char>(arg[j]))) ++j;
        names.push_back(arg.substr(i, j - i));
        i = j;
      }
    }
    if (names.empty()) return engineErrorBlockRc("'' found where varname expected", 7);
    // C4-L2A (D1 card plane): names + dtypes are exactly what the shadow card
    // carries, in the same closed "f64"|"factor" vocabulary Datastore::dtype()
    // answers (datastore.cpp:68 — so the numeric/string verdicts below are
    // identical between planes). Card unavailable/non-current, or the
    // PIET_NO_RESIDENT_READ escape arm → the datastore() path.
    std::vector<std::pair<std::string, std::string>> vars;  // (name, dtype), dataset order
    if (const std::vector<VarInfo>* card = session.residentReadCard()) {
      for (const auto& vi : *card) vars.emplace_back(vi.name, vi.dtype);
    } else {
      Datastore* ds = session.datastore();
      for (const auto& nm : ds->names()) vars.emplace_back(nm, ds->dtype(nm));
    }
    // BUG-1103: before the names, Stata reads the list as a VARLIST, whatever
    // `exact` says, and that pass refuses first: a ts/fv operator is r(101)
    // "factor-variable and time-series operators not allowed", an interaction
    // r(101) "interactions not allowed"; a name, pattern or
    // range end that matches nothing, an ambiguous name or `~` pattern (`x`,
    // `x~`, a lone `~`) and a range out of order are r(111); a range with no end
    // is r(198) "nothing found where name expected". A `-` may carry blanks
    // (loops::expandVarlist's rule). StataBE 19.5: `ab-zz` "variable zz not found",
    // `ab-xb` "variables out of order", `q*` "variable q* not found", `x, exact`
    // "x ambiguous abbreviation"; only then does `xa-xb` meet "- invalid name".
    {
      // The one variable `tok` names or uniquely abbreviates: its index, -1 for
      // none, -2 for several.
      auto resolve = [&vars](const std::string& tok) -> long {
        long hit = -1;
        for (size_t k = 0; k < vars.size(); ++k) {
          if (vars[k].first == tok) return static_cast<long>(k);
          if (vars[k].first.compare(0, tok.size(), tok) == 0) hit = hit == -1 ? static_cast<long>(k) : -2;
        }
        return hit;
      };
      auto refuseName = [](const std::string& tok, long at) {
        return at == -2 ? engineErrorBlockRc(tok + " ambiguous abbreviation", 111)
                        : engineErrorBlockRc("variable " + tok + " not found", 111);
      };
      std::string joined;
      for (size_t i = 0; i < arg.size(); ++i) {
        if (arg[i] == '-') {
          while (!joined.empty() && (joined.back() == ' ' || joined.back() == '\t')) joined.pop_back();
          joined.push_back('-');
          while (i + 1 < arg.size() && (arg[i + 1] == ' ' || arg[i + 1] == '\t')) ++i;
          continue;
        }
        joined.push_back(arg[i]);
      }
      std::istringstream ts(joined);
      for (std::string tok; ts >> tok;) {
        if (tok.find('.') != std::string::npos)
          return engineErrorBlockRc("factor-variable and time-series operators not allowed", 101);
        // `abc#xlong`, `abc ##xlong`: "interactions not allowed" r(101) (StataBE 19.5).
        if (tok.find('#') != std::string::npos)
          return engineErrorBlockRc("interactions not allowed", 101);
        const size_t dash = tok.find('-');
        if (dash != std::string::npos && dash > 0) {
          if (dash + 1 == tok.size())
            return engineErrorBlockRc("nothing found where name expected", 198);
          const std::string a = tok.substr(0, dash), b = tok.substr(dash + 1);
          if (!strutil::isValidStataIdentifier(a) || !strutil::isValidStataIdentifier(b)) continue;
          const long ia = resolve(a);
          if (ia < 0) return refuseName(a, ia);
          const long ib = resolve(b);
          if (ib < 0) return refuseName(b, ib);
          if (ia > ib) return engineErrorBlockRc("variables out of order", 111);
          continue;
        }
        if (tok.find_first_of("*?~") != std::string::npos) {
          std::string pat = tok;
          std::replace(pat.begin(), pat.end(), '~', '*');
          static const std::regex reWild(R"([*?])");
          if (!strutil::isValidStataIdentifier(std::regex_replace(pat, reWild, "a"))) continue;
          int hits = 0;
          for (const auto& v : vars) hits += strutil::globMatch(pat, v.first) ? 1 : 0;
          if (hits == 0) return engineErrorBlockRc("variable " + tok + " not found", 111);
          if (hits > 1 && tok.find('~') != std::string::npos)
            return engineErrorBlockRc(tok + " ambiguous abbreviation", 111);
          continue;
        }
        if (tok == "_all" || !strutil::isValidStataIdentifier(tok)) continue;
        const long at = resolve(tok);
        if (at < 0) return refuseName(tok, at);
      }
    }
    for (const std::string& one : names) {
      // BUG-1103: without `exact`, a name that is no variable stands for the one
      // variable it begins; several are "NAME ambiguous abbreviation", none
      // "variable NAME not found", both r(111). A wrong type is r(7) and quotes
      // the name as typed (StataBE 19.5).
      if (!strutil::isValidStataIdentifier(one))
        return engineErrorBlockRc(one + " invalid name", 198);
      const std::pair<std::string, std::string>* hit = nullptr;
      int prefixHits = 0;
      for (const auto& v : vars) {
        if (v.first == one) {
          hit = &v;
          prefixHits = 1;
          break;
        }
        if (!exact && v.first.size() > one.size() && v.first.compare(0, one.size(), one) == 0) {
          if (prefixHits++ == 0) hit = &v;
        }
      }
      if (prefixHits > 1) return engineErrorBlockRc(one + " ambiguous abbreviation", 111);
      if (hit == nullptr) return engineErrorBlockRc("variable " + one + " not found", 111);
      if (sub == "numeric" || sub == "string") {
        const std::string& dtype = hit->second;
        bool isStrDtype = dtype.rfind("str", 0) == 0 || dtype == "factor";
        if (sub == "numeric" && isStrDtype) {
          return engineErrorBlockRc("'" + one + "' found where numeric variable expected", 7);
        }
        if (sub == "string" && !isStrDtype) {
          return engineErrorBlockRc("'" + one + "' found where string variable expected", 7);
        }
      }
    }
    return ojson::array();
  }

  if (sub == "integer") {
    try {
      size_t pos = 0;
      double val = std::stod(arg, &pos);
      if (pos != arg.size() || val != std::trunc(val)) {
        return engineErrorBlock("confirm integer: '" + arg + "' is not an integer value");
      }
    } catch (...) {
      return engineErrorBlock("confirm integer: '" + arg + "' is not an integer value");
    }
    return ojson::array();
  }

  if (sub == "number") {
    // BS-042: `confirm number ARG` — any real number (integer or not) is
    // fine; message wording mirrors Stata's own exact text (live-probed:
    // `confirm number abc` -> "'abc' found where number expected", rc=7).
    bool ok = false;
    if (!arg.empty()) {
      try {
        size_t pos = 0;
        std::stod(arg, &pos);
        std::string rest = arg.substr(pos);
        ok = rest.find_first_not_of(" \t") == std::string::npos;
      } catch (...) {
        ok = false;
      }
    }
    if (!ok) {
      return engineErrorBlock("confirm number: '" + arg + "' found where number expected");
    }
    return ojson::array();
  }

  if (sub == "scalar") {
    // BS-042: live-probed (`confirm scalar notascalar` -> "scalar
    // notascalar not found", rc=111; existing scalar -> rc=0).
    if (!session.eval.scalars.contains(arg)) {
      return engineErrorBlock("confirm scalar: scalar '" + arg + "' not found");
    }
    return ojson::array();
  }

  if (sub == "matrix") {
    // BUG-922: `confirm matrix NAME` was an unknown subtype and answered rc 0
    // for any name -- including a program's tempname matrix after the program
    // had returned (oracle w2_prog_controlflow pc_tv_gone: r(111)). The bare
    // name reads the user matrix store; `e(name)`/`r(name)` read the class
    // stores (aer_estim_oracle.log:412-414 / :516-521, `confirm matrix e(sds)`).
    static const std::regex reClassMat(std::string(R"(^([er])\(\s*()") + eval::kRxName + R"()\s*\)$)");
    std::smatch cm;
    const eval::EvalMatrix* mx = nullptr;
    if (std::regex_match(arg, cm, reClassMat)) {
      mx = session.eval.findClassMatrix(cm[1].str()[0], cm[2].str());
    } else {
      mx = session.eval.findMatrix(arg);
    }
    if (mx == nullptr) return engineErrorBlockRc("matrix " + arg + " not found", 111);
    return ojson::array();
  }

  if (sub == "names") {
    // BS-042: `confirm names NAME [NAME ...]` (Stata alias `confirm name`
    // for the single-name form, normalised to "names" above) — each token
    // must be a legal identifier; live-probed exact Stata wording is
    // unquoted ("9def invalid name", rc=7), matched verbatim below.
    // Reports the FIRST malformed token, matching Stata's own
    // left-to-right stop-at-first-error.
    std::istringstream iss(arg);
    std::string tok;
    while (iss >> tok) {
      if (!isValidStataName(tok)) {
        return engineErrorBlock("confirm names: " + tok + " invalid name");
      }
    }
    return ojson::array();
  }

  if (sub == "existence") {
    // BS-042: `confirm existence TEXT` — Stata's real semantics here are
    // just "is there anything to confirm" (non-blank), NOT a name-format
    // check (that's `names`/`name` above) — live-probed: blank arg ->
    // "'' found where something expected", rc=6 (no existing coarseRc
    // bucket for r(6); honest-skipped exact-rc fidelity for this specific
    // blank-arg corner case — see this batch's report). Any non-blank arg
    // always succeeds.
    if (trim(arg).empty()) {
      return engineErrorBlock("confirm existence: '' found where something expected");
    }
    return ojson::array();
  }

  // Unknown confirm sub-type -- silent noop (future-proof, mirrors shims.py).
  return ojson::array();
}

// `mkdir DIRNAME [, public]` — create ONE directory level.
//
// BS-045 (BETA campaign C4): live-probed against StataNow 19.5 — real
// Stata's `mkdir` has NO parents-creation option at all (the pre-existing
// `,p` here was an INVENTED option; `help mkdir`'s only option is
// `public`). A missing parent directory is a genuine failure in real
// Stata, not silently bridged: `mkdir /tmp/nonexistent_parent/child`
// fails with the SAME message/rc as re-mkdir'ing an already-existing
// directory: "could not create directory <path>", rc=693 (oracle-pinned
// for the already-exists case; live-probed for the missing-parent case
// too — identical wording both times). `,public` makes the new directory
// world-readable+executable (chmod 0755); omitting it leaves whatever the
// platform's own umask-derived create_directory result is. Unknown
// options are rejected via the shared net (A2 idiom), matching live-probed
// Stata (`mkdir DIR, bogusopt` -> "option bogusopt not allowed", rc=198).
ojson runMkdir(const std::string& raw, const std::vector<std::string>& varlist,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
               SessionState& session) {
  std::string path = trim(firstArgAfter(raw, "mkdir"));
  if (path.empty() && !varlist.empty()) path = joinSpaceLocal(varlist);
  if (path.empty()) return engineErrorBlock("mkdir: directory name required");

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kMkdirKnownOpts = kVocabMkdir.known;
  if (auto err = eval::rejectUnknownOptions(opts, kMkdirKnownOpts)) {
    return ojson(*err);
  }
  bool isPublic = false;
  for (const auto& [k, v] : opts) {
    if (k == "public" && v.isBool && v.boolValue) isPublic = true;
  }

  std::string resolved = resolveUserPath(session, path);
  namespace fs = std::filesystem;
  std::error_code ec;
  // create_directory (NOT create_directories): single level only, exactly
  // like real Stata's own mkdir — a missing parent must fail, not be
  // silently bridged. Returns false (with ec cleared, not an error) for
  // BOTH "already exists" and most creation failures; the missing-parent
  // case sets ec (ENOENT) on some platforms/libstdc++ versions and merely
  // returns false on others, so both `ec` and `!created` are treated as
  // the same real-Stata "could not create directory" outcome.
  bool created = fs::create_directory(cecon::to_native_path(resolved), ec);
  if (ec || !created) {
    return engineErrorBlock("could not create directory " + resolved);
  }
  if (isPublic) {
    fs::permissions(cecon::to_native_path(resolved),
                     fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                         fs::perms::others_read | fs::perms::others_exec,
                     fs::perm_options::replace, ec);
    // Best-effort: a chmod failure doesn't invalidate the directory that
    // was already successfully created above.
  }
  return ojson::array();  // Stata: silent on success
}

// `rmdir DIRNAME` — remove ONE empty directory.
//
// BUG-180 (P2-PREP): PIET had no `rmdir` at ALL — registry.cpp registered
// mkdir/erase/rm but never this, so the command fell through the whole
// routing table to the generic unsupported_command fallback. Measured
// consequence: the cross-engine benchmark harness clears its scratch dirs
// with `capture quietly rmdir "$BENCH_ROOT/dir_`rep'"` before each timed
// `mkdir`; under PIET that line did nothing, the directories from earlier
// runs survived, and every subsequent `mkdir` answered r(693) — the
// benchmark reported mkdir failing 10/10 for a reason that was not mkdir.
//
// Semantics live-probed against StataNow 19.5 this stint (not guessed):
//   rmdir "empty_dir"        -> rc 0, silent
//   rmdir "nonempty_dir"     -> "could not remove directory <path>", rc 693
//   rmdir "no_such_dir"      -> "could not remove directory <path>", rc 693
//   rmdir "some_file.txt"    -> "could not remove directory <path>", rc 693
//   rmdir                    -> "invalid syntax", rc 198
//   rmdir DIR, anyopt        -> "options not allowed", rc 101
// Every failure mode shares ONE message and ONE rc — hence the single
// `fail` lambda below rather than a per-case wording that real Stata does
// not actually distinguish.
//
// Two deliberate notes:
//  - The is_directory() guard is load-bearing, not defensive padding:
//    std::filesystem::remove() would happily delete a plain FILE, so
//    without it `rmdir notes.txt` would silently destroy a file that real
//    Stata refuses to touch. remove() on a NON-EMPTY directory fails on
//    its own (ENOTEMPTY), which is the "empty only" half.
//  - Options: real Stata answers r(101), a bucket this codebase has
//    nowhere else. Routed through the shared reject net instead (r(198),
//    "option `X' not allowed"), which is the uniform idiom every other
//    arm here uses — a deliberate, reported deviation rather than a new
//    one-command rc bucket.
ojson runRmdir(const std::string& raw, const std::vector<std::string>& varlist,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
               SessionState& session) {
  std::string path = trim(firstArgAfterCmd(raw));
  if (path.empty() && !varlist.empty()) path = joinSpaceLocal(varlist);
  if (path.empty()) return engineErrorBlock("rmdir: directory name required", "syntax_error");

  if (auto err = eval::rejectUnknownOptions(opts, {})) {
    return ojson(*err);
  }

  std::string resolved = resolveUserPath(session, path);
  namespace fs = std::filesystem;
  auto native = cecon::to_native_path(resolved);
  // "could not remove directory <path>" is Stata's answer to ALL of:
  // missing path, path is a file, directory not empty, permission denied.
  auto fail = [&resolved]() {
    return engineErrorBlock("could not remove directory " + resolved);
  };

  std::error_code ec;
  if (!fs::is_directory(native, ec) || ec) return fail();
  // remove() (NOT remove_all()): single EMPTY directory only, exactly like
  // real Stata — a non-empty directory must fail, never be recursively
  // erased. Returns false when nothing was removed; ec carries ENOTEMPTY.
  if (!fs::remove(native, ec) || ec) return fail();
  return ojson::array();  // Stata: silent on success
}

// `erase FILE` / `rm FILE` — delete a file. BS-045 (BETA campaign C4):
// `rm` is a genuine Stata BUILT-IN synonym of `erase` (live-probed `which
// rm` -> "built-in command: rm"), routed to this SAME handler by the
// dispatcher; firstArgAfterCmd (unlike the old firstArgAfter(raw,
// "erase")) doesn't hardcode the command word, so it extracts the path
// correctly for either spelling.
ojson runErase(const std::string& raw, const std::vector<std::string>& varlist,
               SessionState& session) {
  std::string path = trim(firstArgAfterCmd(raw));
  if (path.empty() && !varlist.empty()) path = joinSpaceLocal(varlist);
  if (path.empty()) return engineErrorBlock("erase: filename required");

  std::string resolved = resolveUserPath(session, path);
  namespace fs = std::filesystem;
  if (!fs::exists(cecon::to_native_path(resolved))) {
    // StataBE: `file sub/nofile.txt not found` r(601), the name as typed (BUG-410).
    return engineErrorBlockRc("file " + typedUserPath(path) + " not found", 601);
  }
  std::error_code ec;
  // BUG-1461: erase removes files only. fs::remove would take an empty
  // directory at rc 0; Stata refuses a directory as a file it cannot erase
  // (StataBE 19.5: "file DIR is read-only; cannot be modified or erased",
  // r(608)), and rmdir is the command that removes one.
  if (fs::is_directory(cecon::to_native_path(resolved), ec))
    return engineErrorBlockRc("file " + path + " is read-only; cannot be modified or erased", 608);
  fs::remove(cecon::to_native_path(resolved), ec);
  if (ec) return engineErrorBlock("erase: " + ec.message());
  return ojson::array();  // Stata: silent on success
}

// `timer on|off|clear|list [N]` (BS-124, BETA campaign C4) — a session-
// scoped stopwatch bank mirroring Stata's own `timer` command: 100 slots
// (1-100), each a (running?, monotonic start, accumulated total seconds,
// stop count) tuple (SessionState::timers / TimerSlot). Previously an
// honest hard-stop: "timer" sat in dispatcher.cpp's kNoDataCommands
// allow-list (correctly — timer must work with zero data loaded, same as
// the oracle's own probe) but had NO actual `if (cmd == "timer")` dispatch
// arm, so it fell through the entire routing table to the generic
// "unsupported_command" bottom fallback. Fixed by adding the arm (below,
// dispatcher.cpp) — kNoDataCommands' "timer" entry stays (removing it
// would newly break `timer clear`/`timer on` before any `use`, which real
// Stata allows).
//
// Every on/off/clear/list state-transition rule below was live-probed
// against StataNow 19.5 (not guessed):
//   - `timer off N` when N was never started: silent no-op, rc=0 (no
//     accumulation, no count increment).
//   - `timer on N` when N is ALREADY running: silent no-op (does not
//     restart the clock or double-count).
//   - `timer list N` on a slot with no data (count==0): prints nothing.
//   - `timer list` (bare): prints ONLY slots with data (count>0), in slot
//     order — not all 100.
//   - `timer list N` sets r(tN)/r(ntN) for that slot only; an empty slot
//     retires those two stale keys. Bare `timer list` refreshes all timer
//     keys while preserving unrelated r() results.
//   - slot out of 1..100: Stata's own "invalid syntax", rc=198.
// Column format is oracle-pinned EXACTLY (c-oracle-stata.log's BS-124
// block): "%4d: %9.2f / %8.0f = %12.4f" per slot (verified char-for-char
// against `   1:      0.10 /        1 =       0.1010`).

}  // namespace piet::handlers
