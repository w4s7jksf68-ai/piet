// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/loops.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "piet/cclass.h"
#include "piet/counters.h"  // BUG-350 coverage: the loop-capture drop site counts too
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/tokenizer.h"
#include "piet/parallel.h"
#include "piet/stata_format.h"  // BUG-1395: numlist element text is %15.0g
#include "piet/strutil.h"
#include "core/expr_blanks.h"  // FU-875: Stata's blank squeeze and r(132) pairing rule
#include "core/ident.h"  // Unicode names: the shared identifier byte tests
#include "data/missing.h"  // BUG-921: the extended-missing kind of a cell read by the if condition
#include "dispatch/dispatch_profile.h"  // EXPRLAYER: expression-zone attribution (opt-in, stderr)

#include "loops_internal.h"

namespace piet::loops {

using namespace loops_detail;

namespace {

// ---------------------------------------------------------------------------
// Varlist wildcard expansion (loops.py's _expand_varlist)

// The implementation stays here in the anonymous namespace; the PUBLIC entry point
// (declared in piet/loops.h since BUG-371, so `ds` and `unab` can ask the same
// question `foreach ... of varlist` asks) is the one-line forwarder below, after
// this namespace closes.
// Stata's varlist wildcards: `*` matches any run (including empty), `?` matches
// exactly one character. Iterative backtracking rather than a regex, because a
// name is short and building a std::regex per token per call is what the zone
// profile has already caught twice in this file.
// Unicode names (lane M): forwards to the shared matcher, whose `?` is ONE
// CHARACTER (StataBE: `ds ق?` lists the two-letter Arabic names) -- this copy's
// `?` was one byte, so it matched half an Arabic letter.
bool globMatch(const std::string& pat, const std::string& name) {
  return strutil::globMatch(pat, name);
}

}  // namespace

namespace loops_detail {

VarlistExpandResult expandVarlistImpl(const std::string& spec, SessionState& session) {
  VarlistExpandResult r;
  // C4-L2B (D1 card plane, brief §4): wildcard/range expansion needs only the
  // ORDERED NAME LIST, which the CURRENT resident card carries (seeded in
  // column order by every writer, the same order ds->names() reports). Reading
  // it through session.datastore() here forced, on a dirty virtual head, the
  // full EMH frame snapshot — for a `foreach ... of varlist` header that never
  // looks at a single value. Honest fallback: a stale/absent card (or the
  // PIET_NO_RESIDENT_READ escape arm, via residentReadCard's gate) drops to
  // the pre-D1 datastore path verbatim, including its no-dataset token
  // passthrough branch (a current card implies a loaded dataset, so the card
  // plane can never shadow that branch).
  std::vector<std::string> allVars;
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    allVars.reserve(card->size());
    for (const auto& v : *card) allVars.push_back(v.name);
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) {
      std::istringstream iss(spec);
      std::string tok;
      while (iss >> tok) r.tokens.push_back(tok);
      return r;
    }
    allVars = ds->names();
  }
  // AER scope 2, K1b: the range operator may carry whitespace on either side.
  // Measured on StataNow 19.5 BE (`unab a : ...` over auto.dta): `price- trunk`,
  // `price -trunk` and `price - trunk` all give the same five names as
  // `price-trunk`. The corpus spells it with the trailing space -- paper 082's
  // `tolower AL1- KONEV_L` -- and without this the two halves arrive as two
  // separate tokens and neither resolves. Collapsing is safe HERE because a
  // variable name cannot contain a hyphen (strutil::isValidStataIdentifier), so
  // in a varlist a `-` is always the range operator and never part of a name.
  std::string joined;
  joined.reserve(spec.size());
  for (size_t i = 0; i < spec.size(); ++i) {
    if (spec[i] == '-') {
      while (!joined.empty() && std::isspace(static_cast<unsigned char>(joined.back())))
        joined.pop_back();
      joined.push_back('-');
      while (i + 1 < spec.size() && std::isspace(static_cast<unsigned char>(spec[i + 1]))) ++i;
      continue;
    }
    joined.push_back(spec[i]);
  }

  std::istringstream iss(joined);
  std::string tok;
  while (iss >> tok) {
    // `_all` is the varlist keyword for every variable, and it was reaching the
    // literal-name branch below, where it answered `variable _all not found`.
    // Measured: `unab a : _all` on auto.dta is rc 0 and returns all twelve
    // names in column order. Case-insensitive because Stata's own varlist
    // keywords are.
    if (tok.size() == 4 && (tok[0] == '_') &&
        std::tolower(static_cast<unsigned char>(tok[1])) == 'a' &&
        std::tolower(static_cast<unsigned char>(tok[2])) == 'l' &&
        std::tolower(static_cast<unsigned char>(tok[3])) == 'l') {
      for (const auto& v : allVars) r.tokens.push_back(v);
      continue;
    }
    // AER scope 2, K1b: `*` matches anywhere and any number of times, and `?`
    // matches exactly one character. This used to be a TRAILING `*` only, i.e.
    // a prefix match, which silently refused three of the four shapes Stata
    // accepts. Measured on StataNow 19.5 BE against auto.dta, `unab`:
    //   t*     -> trunk turn        (this is the one the old code did)
    //   *e     -> make price
    //   tr*k   -> trunk
    //   ?rice  -> price
    //   t*n    -> turn
    //   zz*    -> r(111), no match
    //   ~e     -> r(111): `~` is NOT a wildcard here, so it stays unmatched
    // The corpus needs the others: `renvars *2010, subst(...)` is a real line
    // in it, and 948 more `drop`/`keep` lines carry a wildcard (those reach a
    // different, unwired path -- BUG-450).
    if (tok.find('*') != std::string::npos || tok.find('?') != std::string::npos) {
      bool any = false;
      for (const auto& v : allVars) {
        if (globMatch(tok, v)) {
          r.tokens.push_back(v);
          any = true;
        }
      }
      if (!any) {
        r.isError = true;
        // r(111) is what Stata answers for a pattern that matches nothing, and
        // `reason` is the only channel that carries an exact rc.
        r.error = eval::errorBlock("engine_error", "variable " + tok + " not found", std::nullopt,
                                   std::nullopt, std::nullopt, "r(111)");
        return r;
      }
      continue;
    }
    static const std::regex reRange(R"(^[\w\x80-\xff]+-[\w\x80-\xff]+$)");
    if (std::regex_match(tok, reRange)) {
      size_t dash = tok.find('-');
      std::string v1 = tok.substr(0, dash), v2 = tok.substr(dash + 1);
      auto it1 = std::find(allVars.begin(), allVars.end(), v1);
      auto it2 = std::find(allVars.begin(), allVars.end(), v2);
      if (it1 == allVars.end() || it2 == allVars.end()) {
        r.isError = true;
        r.error = errBlock("varlist expansion: '" + v1 + "' or '" + v2 + "' not found");
        return r;
      }
      auto i1 = std::distance(allVars.begin(), it1);
      auto i2 = std::distance(allVars.begin(), it2);
      for (auto k = i1; k <= i2; ++k) r.tokens.push_back(allVars[static_cast<size_t>(k)]);
      continue;
    }
    r.tokens.push_back(tok);
  }
  return r;
}

// BUG-509 (corrected)/BUG-544: cheap existence guard for `foreach ... of
// varlist ...`, mirroring dispatcher.cpp's `unab` arm's known-names check
// (dispatcher.cpp, incl. its no-card fallback). expandVarlistImpl above only
// validates WILDCARD/RANGE spellings (see its own comment): a bare unknown
// literal token passes straight through by design (9 other callers rely on
// that passthrough), so a caller that needs Stata's r(111) "variable not
// found" ahead of any iteration checks it here instead. Oracle:
// tests/eval/golden/aer_w2/w2_openbugs.log arms fe_unknown/fe_mixed/
// fe_prog_unknown — the check runs over the WHOLE expanded token list before
// runForeach ever sets the loop variable once.
std::optional<ojson> checkKnownVarTokens(std::vector<std::string>& tokens,
                                         SessionState& session) {
  std::vector<std::string> known;
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    for (const auto& vi : *card) known.push_back(vi.name);
  } else if (Datastore* ds = session.datastore()) {
    known = ds->names();
  } else {
    return ojson(eval::noDataLoaded());
  }
  // A unique prefix stands for its variable, as in every Stata varlist
  // (w2_openbugs.log:190-194 fe_abbrev: `foreach v of varlist alph` iterates
  // `alpha`, rc 0); several matches are r(111) "ambiguous abbreviation".
  for (auto& t : tokens) {
    if (std::find(known.begin(), known.end(), t) != known.end()) continue;
    std::vector<std::string> hits;
    for (const auto& k : known)
      if (k.size() > t.size() && k.compare(0, t.size(), t) == 0) hits.push_back(k);
    if (hits.size() == 1) {
      t = hits.front();
      continue;
    }
    if (hits.size() > 1)
      return ojson(eval::engineError(t + " ambiguous abbreviation", "unknown_variable"));
    return ojson(eval::engineError("variable " + t + " not found", "unknown_variable"));
  }
  return std::nullopt;
}

}  // namespace loops_detail

VarlistExpandResult expandVarlist(const std::string& spec, SessionState& session) {
  return expandVarlistImpl(spec, session);
}

std::optional<nlohmann::ordered_json> checkKnownVarlist(std::vector<std::string>& tokens,
                                                        SessionState& session) {
  return checkKnownVarTokens(tokens, session);
}

}  // namespace piet::loops
