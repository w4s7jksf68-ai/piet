// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
//   require   an accept-note, the `ssc install`/`which` precedent.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "piet/cclass.h"             // substituteCRefs: the dispatcher's own pre-pass for a definition line
#include "piet/datastore.h"
#include "piet/dispatcher.h"         // markout: a dispatched `quietly replace`
#include "piet/display_hardening.h"  // fmtStataDefaultG: what `display <number>` prints
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"        // hasMacroRefs, tryCaptureDefinition
#include "piet/eval/pyvalue.h"       // pythonFloatRepr: a round-trippable number for $S_3/$S_6/$S_7
#include "piet/eval/session.h"       // replaceRClass
#include "piet/eval/tokenizer.h"
#include "piet/handlers/session_commands.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"       // renderValue/padToField: the one %w.pf renderer
#include "piet/strutil.h"
#include "language/command_reply_support.h"

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch (defect class 3).
namespace {

// require's own `syntax` line: INSTALL ADOPATH() FROM() DEBUG() VERBOSE STRICT.
// install/strict/verbose are READ: each answers within the accept-note (nothing
// is installed or version-checked in PIET, and the note says so). adopath()/
// from()/debug() choose WHERE a package is searched or fetched from, or feed a
// fake starbang line; none has a meaning without a package store, so each is
// refused by name rather than accepted unread. `list` is the `require, list`
// form, a different command shape.
const piet::eval::OptVocab& kVocabRequire = piet::eval::registerOptVocab(
    {"require"}, {{"install", "strict", "verbose"}, {"adopath", "from", "debug", "list"}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace command_reply_detail;

// ===========================================================================
// require (Sergio Correia, Matthew P. Seay)
// ===========================================================================
//
// Implements require's requirement grammar per require's help file;
// identified against StataBE probes (w2ax_t1_community.log require_ge,
// require_eq, require_bare, require_two; envp2 req_*).
//
// The requirement is ONE package, optionally followed by an operator and a
// version: `pkg`, `pkg>=v`, `pkg==v`, `pkg=v` (= is ==). Whatever follows the
// package name is read as the operator, so a second package name is an
// operator that is not one of those -- require_two `require estout distinct`
// is r(9) `assert failed: inlist("distinct", ">=", "==", "")`, and envp2
// req_gt `estout>3` is r(9) the same way. A syntax rule, so it is mirrored.
//
// Everything past the grammar is an installed-files fact PIET cannot see:
// PIET runs no ado-files and keeps no package store, so it neither finds the
// package (Stata: r(601) when absent) nor reads its version (r(2226)/r(2227)
// when too old). It answers rc 0 with a note that says exactly that, the
// `ssc install` / `which` accept-note precedent. `require stata>=V` IS a fact
// PIET knows -- the Stata release it answers for is 19.5 (the oracles') -- so
// V above that is Stata's r(9) (envp2 req_stata21), and V at or below it rc 0.
nlohmann::ordered_json runRequire(const eval::ParsedCommand& parsed, SessionState& session) {
  (void)session;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRequire.known,
                                            kVocabRequire.unimplemented))
    return ojson(*err);
  const std::string req = strutil::trim(parsed.argText);
  if (req.empty()) return envError("something required", 100);  // envp2 req_none
  if (req.rfind("using", 0) == 0 && (req.size() == 5 || req[5] == ' ' || req[5] == '\t'))
    return ojson(eval::refusedCommand("require using", "no_package_store"));

  auto isOpChar = [](char c) { return c == '>' || c == '='; };
  size_t p = 0;
  while (p < req.size() && !isOpChar(req[p]) && req[p] != ' ' && req[p] != '\t') ++p;
  const std::string package = req.substr(0, p);
  while (p < req.size() && (req[p] == ' ' || req[p] == '\t')) ++p;
  std::string op;
  if (p < req.size()) {
    size_t q = p;
    if (isOpChar(req[q])) {
      while (q < req.size() && isOpChar(req[q])) ++q;
    } else {
      while (q < req.size() && !isOpChar(req[q]) && req[q] != ' ' && req[q] != '\t') ++q;
    }
    op = req.substr(p, q - p);
    p = q;
  }
  const std::string version = strutil::trim(req.substr(p));
  if (op == "=") op = "==";
  if (op != ">=" && op != "==" && !op.empty())
    return envError("assert failed: inlist(\"" + op + "\", \">=\", \"==\", \"\")", 9);
  if (package.empty()) return envError("invalid syntax", 198);

  if (package == "stata") {
    // `require stata>=V` runs `version V: qui` (review d1): no operator, an
    // empty or non-decimal V (`stata>=`, `stata>=abc`, `stata>=1e1`) is the
    // version command's r(198); V below 1 or above the release is r(9)
    // (`stata>=0` R14, `stata>=19.6` R8, `stata >= 20` R3); 16, 17.0, 19,
    // 19.5 are rc 0.
    if (op.empty()) return envError("invalid syntax", 198);  // R1 `require stata`
    bool decimal = !version.empty();
    bool sawDigit = false, sawDot = false;
    for (char c : version) {
      if (std::isdigit(static_cast<unsigned char>(c))) {
        sawDigit = true;
      } else if (c == '.' && !sawDot) {
        sawDot = true;
      } else {
        decimal = false;
      }
    }
    if (!decimal || !sawDigit) return envError("invalid syntax", 198);
    const double v = parseNumber(version).value_or(0.0);
    if (v < 1.0 || v > 19.5)
      return envError("this is version 19.5 of Stata (the release PIET answers for); it cannot "
                      "run version " + fixedFmt(v, 1, 1, false) + " programs",
                      9);
    return ojson::array();
  }
  return displayBlock("(require " + req +
                      ": PIET bundles its own implementations; installation and version not checked)");
}

}  // namespace piet::handlers
