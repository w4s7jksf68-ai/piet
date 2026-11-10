// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// The helpers the seven variable-metadata commands share (declared in
// variable_metadata_support.h). The family's original header follows.
// AER tier-1 (2026-10-28), lane META: verbs that read or rename, never compute -- findname
// xtdescribe cf fvunab numlabel labvars renpfix.
//
// Registry rows, dispatcher arms (dispatcher.cpp kTierOne), handlers.h declarations,
// effects/floors rows, NOTICE lines and pangram lines landed with the shared-surface unit;
// this file is the lane's alone. Answers: the StataBE 19.5 captures w2ax_t1_official.log
// (xtdes_* cf_* fvunab_* numlabel_* renpfix_*) and w2ax_t1_community.log (findname_*
// labvars_*), plus the lane's own extra probes named at each rule below (mx1..mx5, run
// on the same StataBE build while this lane was written).
//
// NOTHING HERE COMPUTES A SECOND TIME WHAT PIET ALREADY COMPUTES.
//
// THE DEFECT CLASSES (dataset_shape_support.h's header names them): every block returned is a
// typed object or an array of typed objects (SHAPE); every error with a known Stata rc
// carries "r(NNN)" in `reason` (RC); every option read is registered at namespace scope
// below and every handler calls the reject net first (VOCABULARY); r() is filled here, not
// by a dispatcher capture (R-CLASS).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "data/missing.h"  // cecon::stata_cmp / missing_kind -- Stata's `!=`, extended missings
#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"  // handleDisplay (display's default number text), resolveVarName
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // applyStataFmt, fnAbbrev
#include "piet/eval/macros.h"   // captureRFromBlock
#include "piet/eval/pyvalue.h"  // pythonFloatRepr / PyValue
#include "piet/eval/session.h"  // replaceRClass / declareRPosted
#include "piet/eval/tokenizer.h"  // scanOneToken / quotedSpanMask
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // expandVarlist -- the ONE varlist wildcard/range expander
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "vendor/nlohmann/json.hpp"

#include "variable_metadata_support.h"

namespace piet::handlers::variable_metadata_detail {

// ===========================================================================
// Blocks and errors
// ===========================================================================

// An error with a KNOWN Stata rc: `reason` is the only channel carrying an exact rc.
ojson rcError(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

// A real Stata form this build does not implement: refused by name. r(199) is the
// codebase's honest "not implemented" rc (the brief's rule); Stata would have run it.
ojson notImplemented(const std::string& message) { return rcError(message, 199); }

ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

ojson silent() { return ojson::array(); }

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

const ojson* firstError(const ojson& result) {
  if (isErrorBlock(result)) return &result;
  if (result.is_array())
    for (const auto& b : result)
      if (isErrorBlock(b)) return &b;
  return nullptr;
}

// ===========================================================================
// Options
// ===========================================================================

bool hasFlag(const eval::ParsedCommand& p, const std::string& name) {
  for (const auto& [k, v] : p.opts)
    if (k == name) return !v.isBool || v.boolValue;
  return false;
}

std::optional<std::string> optStr(const eval::ParsedCommand& p, const std::string& name) {
  for (const auto& [k, v] : p.opts)
    if (k == name) return v.isBool ? std::string() : v.strValue;
  return std::nullopt;
}

namespace {

// STRICT integer: the whole token must parse.
std::optional<long long> strictInt(const std::string& s) {
  const std::string t = strutil::trim(s);
  if (t.empty()) return std::nullopt;
  size_t i = 0;
  if (t[0] == '-' || t[0] == '+') i = 1;
  if (i >= t.size()) return std::nullopt;
  for (size_t j = i; j < t.size(); ++j)
    if (!std::isdigit(static_cast<unsigned char>(t[j]))) return std::nullopt;
  try {
    return std::stoll(t);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace

// An integer option `name(#)`, or `fallback` when absent; nullopt + error when malformed.
std::optional<long long> intOption(const eval::ParsedCommand& p, const std::string& name,
                                   long long fallback, ojson* err) {
  auto v = optStr(p, name);
  if (!v.has_value()) return fallback;
  auto n = strictInt(*v);
  if (!n.has_value()) {
    *err = rcError("option " + name + "() incorrectly specified", 198);
    return std::nullopt;
  }
  return n;
}

// Stata's word tokenization (tokenize / Mata tokens()): blanks separate, a "..." or a
// compound `"..."' is one word with its quotes removed. eval::scanOneToken is the
// codebase's one scanner for exactly that; `"a""b"` is two words, as Stata counts them
// (w2ax_t1_community.log labvars_dq: `"with ""inner"" quote"` is 3 labels).
std::vector<std::string> stataWords(const std::string& text) {
  std::vector<std::string> out;
  std::string rest = text;
  while (!strutil::trim(rest).empty()) {
    auto [tok, rem] = eval::scanOneToken(rest, /*bind=*/true);
    out.push_back(tok);
    if (rem.size() >= rest.size()) break;  // no progress -- never loop forever
    rest = rem;
  }
  return out;
}

std::string joinWords(const std::vector<std::string>& v, const std::string& sep) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += sep;
    out += v[i];
  }
  return out;
}

std::string padTo(std::string s, size_t col) {  // _column(col+1): pad to `col` characters
  if (s.size() < col) s.append(col - s.size(), ' ');
  return s;
}

std::string rightAlign(const std::string& s, size_t w) {
  return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
}

// Stata's abbrev(s, n) -- the expression function itself (exprfns fnAbbrev), not a copy.
std::string stataAbbrev(const std::string& s, int n) {
  eval::PyValue r = eval::fnAbbrev({eval::PyValue::str(s), eval::PyValue::integer(n)});
  return r.isStr() ? r.strVal() : s;
}

// The text `display <number>` prints for a value (no format): display's own renderer,
// fed the full-precision literal -- nine characters, so a ten-digit id is 1.235e+09
// (review p2/p3/p8: xtdescribe's header and cf's verbose lines both print it that way).
// A missing value prints as its own token.
std::string displayNumber(double v, SessionState& session) {
  const int kind = cecon::missing_kind(v);
  if (kind >= 0) return kind == 0 ? std::string(".") : std::string(".") + char('a' + kind - 1);
  ojson d = handleDisplay(eval::pythonFloatRepr(v), session);
  if (d.is_object() && d.value("type", std::string()) == "display")
    return d.value("text", std::string());
  return eval::pythonFloatRepr(v);
}

// ===========================================================================
// Variables
// ===========================================================================

// Every variable name, dataset order; empty when nothing is loaded.
std::vector<std::string> allVarNames(SessionState& session) {
  if (!session.hasData()) return {};
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    std::vector<std::string> out;
    out.reserve(card->size());
    for (const auto& vi : *card) out.push_back(vi.name);
    return out;
  }
  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception&) {
    ds = nullptr;
  }
  return ds == nullptr ? std::vector<std::string>() : ds->names();
}

// `unab`'s answer for a varlist: wildcards and ranges by loops::expandVarlist (the one
// expander), every literal confirmed to exist or resolved as a unique abbreviation by
// resolveVarName (display's binding rule). Stata's r(111) texts: "variable x not
// found", "x ambiguous abbreviation", "variables out of order" (mx2/mx5 fvunab arms).
std::optional<ojson> unabVarlist(const std::string& spec, SessionState& session,
                                 std::vector<std::string>* out) {
  out->clear();
  const std::vector<std::string> names = allVarNames(session);
  if (names.empty()) return rcError("no variables defined", 111);
  if (spec.find('~') != std::string::npos)
    return notImplemented("the `~` varlist wildcard is not implemented in this version of PIET (" +
                          strutil::trim(spec) + ")");
  // A reversed `b-a` range is r(111) "variables out of order"; expandVarlist answers it
  // with an empty run, so the order is checked on the same collapsed text it reads.
  {
    std::string joined;
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
    static const std::regex reRange(R"(^(\w+)-(\w+)$)");
    std::istringstream iss(joined);
    for (std::string tok; iss >> tok;) {
      std::smatch m;
      if (!std::regex_match(tok, m, reRange)) continue;
      auto a = std::find(names.begin(), names.end(), m.str(1));
      auto b = std::find(names.begin(), names.end(), m.str(2));
      if (a == names.end()) return rcError("variable " + m.str(1) + " not found", 111);
      if (b == names.end()) return rcError("variable " + m.str(2) + " not found", 111);
      if (a > b) return rcError("variables out of order", 111);
    }
  }
  loops::VarlistExpandResult ex = loops::expandVarlist(spec, session);
  if (ex.isError) return ex.error;
  for (const auto& t : ex.tokens) {
    if (std::find(names.begin(), names.end(), t) != names.end()) {
      out->push_back(t);
      continue;
    }
    bool ambiguous = false;
    std::optional<std::string> hit;
    if (strutil::isValidStataIdentifier(t)) hit = resolveVarName(t, session, &ambiguous);
    if (hit.has_value() && std::find(names.begin(), names.end(), *hit) != names.end()) {
      out->push_back(*hit);
      continue;
    }
    if (ambiguous) return rcError(t + " ambiguous abbreviation", 111);
    return rcError("variable " + t + " not found", 111);
  }
  return std::nullopt;
}

// Delete a scratch file this lane minted, now, rather than at the dispatch boundary.
void dropScratch(const std::string& path) {
  if (path.empty()) return;
  cecon::native_unlink(path);
  forgetScratchMint(path);
}

}  // namespace piet::handlers::variable_metadata_detail
