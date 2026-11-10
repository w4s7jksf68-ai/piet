// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/utility.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>  // std::memcpy -- the ±0-safe key bit-normalization
#include <ctime>
#include <exception>
#include <functional>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

#include "utility_support.h"
#include "utility_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace utility_detail;

// ===========================================================================
// strdist  (AER scope 2, bundle B2)
// ===========================================================================
//
//     strdist {var|"literal"} {var|"literal"} [if] [in] [, gen(name) maxdist(#)]
//
// Michael D Barker and Felix Poege's strdist.ado 1.2, whose own header names
// them. Levenshtein edit distance with unit insert/delete/substitute cost.
//
// BYTE-WISE, not codepoint-wise, because the package is: its Mata kernel calls
// `ascii()` on both strings and compares those numbers. That is not a detail to
// smooth over -- it is why `strdist "a" "A"` is 1 and not 0, measured, and why
// the package's own help file points Stata 14+ users at `ustrdist` instead.
//
// FIVE SHORTCUTS ARE THE PACKAGE'S, AND EACH CHANGES AN ANSWER:
//   a == b                            -> 0     (before any length test)
//   maxdist set, |len_a-len_b| > it   -> .     (missing, not the true distance)
//   either string empty               -> len_a + len_b
//   running row minimum > maxdist     -> .     (early stop)
//   final cell > maxdist              -> .
// `maxdist(#)` below 1 means NO limit, and the ado says so out loud when the
// argument is neither -1 nor >= 1; -1 is its own "unset" spelling.
//
// THE DEFAULT VARIABLE NAME IS THE LITERAL WORD `strdist`. `strdist a b` with no
// gen() creates a variable called `strdist` -- measured (`ds` afterwards reports
// `a b strdist`), not assumed. The two-literal form is the exception: with no
// gen() it DISPLAYS the number and returns r(d), creating nothing.
long long levenshteinBytes(const std::string& a, const std::string& b, long long maxdist,
                           bool* exceeded) {
  *exceeded = false;
  if (a == b) return 0;
  const long long la = static_cast<long long>(a.size());
  const long long lb = static_cast<long long>(b.size());
  const bool limited = maxdist >= 1;
  if (limited && std::llabs(la - lb) > maxdist) {
    *exceeded = true;
    return 0;
  }
  if (la == 0 || lb == 0) {
    // The package returns len_a+len_b here WITHOUT re-testing maxdist, and the
    // |la-lb| guard above has already refused the case that would exceed it.
    return la + lb;
  }
  std::vector<long long> v0(static_cast<size_t>(lb) + 1), v1(static_cast<size_t>(lb) + 1);
  for (long long j = 0; j <= lb; ++j) v0[static_cast<size_t>(j)] = j;
  for (long long i = 1; i <= la; ++i) {
    v1[0] = i;
    for (long long j = 1; j <= lb; ++j) {
      const long long cost = a[static_cast<size_t>(i - 1)] != b[static_cast<size_t>(j - 1)] ? 1 : 0;
      v1[static_cast<size_t>(j)] =
          std::min({v1[static_cast<size_t>(j - 1)] + 1, v0[static_cast<size_t>(j)] + 1,
                    v0[static_cast<size_t>(j - 1)] + cost});
    }
    v0.swap(v1);
    if (limited) {
      long long rowMin = v0[0];
      for (long long j = 1; j <= lb; ++j) rowMin = std::min(rowMin, v0[static_cast<size_t>(j)]);
      if (rowMin > maxdist) {
        *exceeded = true;
        return 0;
      }
    }
  }
  const long long d = v0[static_cast<size_t>(lb)];
  if (limited && d > maxdist) {
    *exceeded = true;
    return 0;
  }
  return d;
}

// A strdist argument is a LITERAL when the user quoted it, and a variable name
// otherwise. The tokenizer hands the quotes through, so they are the signal --
// the same test the ado makes with `gettoken ..., qed()`.
bool isQuotedLiteral(const std::string& tok, std::string* text) {
  if (tok.size() >= 2 && tok.front() == '"' && tok.back() == '"') {
    *text = tok.substr(1, tok.size() - 2);
    return true;
  }
  return false;
}

// `confirm new variable` with STATA'S rc. newVarGuard above answers the same
// question but tags it `bad_input`, which coarseRc maps to rc 1; the package
// answers r(110), measured (`strdist p q, gen(p)` with p present).
std::optional<ojson> strdistNewVar(SessionState& session, const std::string& name) {
  if (!strutil::isValidStataIdentifier(name))
    return utilSyntax("strdist: invalid variable name `" + name + "' -- " +
                      strutil::stataIdentifierRuleText());
  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception&) {
    ds = nullptr;
  }
  if (ds != nullptr && ds->has(name))
    return utilErrorRc("variable " + name + " already defined", 110);
  return std::nullopt;
}

ojson runStrdist(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabStrdist.known,
                                            kVocabStrdist.unimplemented))
    return ojson(*err);
  // ONE argument is legal and is not the same as two. The ado routes it to
  // `strdist12var VAR, m("")`, i.e. every value's distance from the EMPTY
  // string, which fastlev shortcuts to the string's own length. Measured:
  // `strdist p` over "kitten" is rc 0 and gives 6. Three arguments are r(103);
  // zero cannot reach here (the tokenizer needs a varlist).
  if (parsed.varlist.empty())
    return utilSyntax("strdist: syntax is `strdist {var|\"literal\"} [{var|\"literal\"}] "
                      "[, generate(name) maxdist(#)]'");
  if (parsed.varlist.size() > 2) return utilErrorRc("too many variables specified", 103);

  long long maxdist = -1;
  if (auto mv = optStr(parsed.opts, "maxdist")) {
    if (!parseStrictInt(*mv, &maxdist))
      return utilSyntax("strdist: maxdist() requires a whole number, got `" + *mv + "'");
  }
  // The ado's own normalisation, message included: anything below 1 that is not
  // its -1 "unset" spelling is announced and then treated as no limit.
  ojson blocks = ojson::array();
  if (maxdist < 1 && maxdist != -1) {
    blocks.push_back(displayBlock("Invalid argument maxdist (" + std::to_string(maxdist) +
                                 "). The Levenstein distance without limitation will be computed."));
    maxdist = 0;
  }
  if (maxdist < 1) maxdist = 0;  // 0 and -1 both mean "no limit" downstream

  std::string lit1, lit2;
  const bool isLit1 = isQuotedLiteral(parsed.varlist[0], &lit1);
  // A single argument behaves as `<arg> ""`: the ado leaves `second` empty and
  // the match string defaults to "".
  const bool oneArg = parsed.varlist.size() == 1;
  const bool isLit2 = !oneArg && isQuotedLiteral(parsed.varlist[1], &lit2);
  if (oneArg) lit2.clear();
  const std::string genName = optStr(parsed.opts, "generate").value_or(std::string());

  // ---- both literals: one number, displayed, r(d). --------------------
  if (isLit1 && (isLit2 || oneArg)) {
    bool exceeded = false;
    const long long d = levenshteinBytes(lit1, lit2, maxdist, &exceeded);
    if (genName.empty()) {
      blocks.push_back(displayBlock(exceeded ? std::string(".") : std::to_string(d)));
      session.eval.rValues.set("d", exceeded ? std::numeric_limits<double>::quiet_NaN()
                                             : static_cast<double>(d));
      return blocks;
    }
    if (!session.hasData()) return ojson(eval::noDataLoaded());
    if (auto err = strdistNewVar(session, genName)) return *err;
    Datastore* ds = session.datastore();
    if (ds == nullptr) return ojson(eval::noDataLoaded());
    const std::string cell = exceeded ? std::string() : std::to_string(d);
    std::vector<std::string> cells(static_cast<size_t>(ds->nRows()), cell);
    std::string outPath;
    if (auto err = writeColumn(session, ds->path(), genName, cells, "f64", true, &outPath))
      return *err;
    session.load(outPath);
    session.eval.rValues.set("d", exceeded ? std::numeric_limits<double>::quiet_NaN()
                                           : static_cast<double>(d));
    session.eval.rStrings.set("strdist", genName);
    blocks.push_back(mutationResult(session, "gen", {genName}, {}));
    return blocks;
  }

  // ---- at least one variable: a column of distances --------------------
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  Datastore* ds = session.datastore();
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const std::string target = genName.empty() ? std::string("strdist") : genName;
  if (auto err = strdistNewVar(session, target)) return *err;

  auto columnOf = [&](const std::string& nm) -> std::optional<std::vector<std::string>> {
    if (!ds->has(nm)) return std::nullopt;
    return columnAsStrings(*ds, nm);
  };
  std::vector<std::string> left, right;
  if (oneArg) {
    auto col = columnOf(parsed.varlist[0]);
    if (!col) return utilErrorRc("variable " + parsed.varlist[0] + " not found", 111);
    left = *col;
    right.assign(left.size(), std::string());
  } else if (isLit1) {
    auto col = columnOf(parsed.varlist[1]);
    if (!col) return utilErrorRc("variable " + parsed.varlist[1] + " not found", 111);
    right = *col;
    left.assign(right.size(), lit1);
  } else if (isLit2) {
    auto col = columnOf(parsed.varlist[0]);
    if (!col) return utilErrorRc("variable " + parsed.varlist[0] + " not found", 111);
    left = *col;
    right.assign(left.size(), lit2);
  } else {
    auto c1 = columnOf(parsed.varlist[0]);
    if (!c1) return utilErrorRc("variable " + parsed.varlist[0] + " not found", 111);
    auto c2 = columnOf(parsed.varlist[1]);
    if (!c2) return utilErrorRc("variable " + parsed.varlist[1] + " not found", 111);
    left = *c1;
    right = *c2;
  }

  // `if`/`in` are NOT accepted, and the registry row says so rather than this
  // handler silently ignoring them. The package marks rows out with a `touse`
  // and writes only into it, so an excluded row keeps a missing -- a real
  // behaviour that needs a real mask, and `applyIfAndInFilter` hands back a
  // FILTERED FRAME rather than a mask, which would drop the excluded rows
  // instead of blanking them. Accepting the qualifier and answering the wrong
  // shape is worse than refusing it; the twelve-paper corpus uses neither.
  std::vector<std::string> cells(left.size());
  for (size_t i = 0; i < left.size(); ++i) {
    bool exceeded = false;
    const long long d = levenshteinBytes(left[i], right[i], maxdist, &exceeded);
    if (!exceeded) cells[i] = std::to_string(d);
  }
  std::string outPath;
  if (auto err = writeColumn(session, ds->path(), target, cells, "f64", true, &outPath))
    return *err;
  session.load(outPath);
  session.eval.rStrings.set("strdist", target);
  blocks.push_back(mutationResult(session, "gen", {target}, {}));
  return blocks;
}


}  // namespace piet::handlers
