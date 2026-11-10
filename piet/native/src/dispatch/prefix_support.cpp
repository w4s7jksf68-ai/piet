// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "prefix_support.h"
#include "text_support.h"
#include "options.h"
#include "handler_declarations.h"
#include "piet/handlers/observations.h"

namespace piet::dispatch_detail {


char asciiLower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}


// dispatcher.py's `_strip_prefix`: if `line` starts with a complete word in
// `prefixes` (optionally followed by ':'), return the remainder; else nullopt.
std::optional<std::string> stripPrefix(const std::string& line, const PrefixTable& table) {
  // First-byte reject, ahead of the trim's allocation. trimWs() is
  // strutil::trim, whose leading edge uses the same isspace class this scan
  // does, so the byte found here is exactly the byte the old
  // `strippedLower.rfind(pfx, 0)` compared first.
  size_t b = 0;
  while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
  if (b >= line.size() || !table.first[static_cast<unsigned char>(line[b])]) return std::nullopt;

  dprof::notePrefixSetSort();
  std::string stripped = trimWs(line);
  // Longest-first so "quiet"/"quietly" don't shadow each other incorrectly
  // (mirrors Python's `sorted(prefixes, key=len, reverse=True)`).
  for (const auto& pfx : table.byLenDesc) {
    if (stripped.size() < pfx.size()) continue;
    bool eq = true;
    for (size_t i = 0; i < pfx.size(); ++i) {
      if (asciiLower(stripped[i]) != pfx[i]) {
        eq = false;
        break;
      }
    }
    if (!eq) continue;
    std::string rest = stripped.substr(pfx.size());
    if (!rest.empty() && rest[0] != ' ' && rest[0] != '\t' && rest[0] != ':') continue;
    // strip a single leading ':' then whitespace
    size_t i = 0;
    if (i < rest.size() && rest[i] == ':') ++i;
    rest = trimWs(rest.substr(i));
    if (!rest.empty()) return rest;
  }
  return std::nullopt;
}


// _parse_bysort_prefix: cmd="bysort"/"by", varlist=["foreign:","gen","idx=_n"]
// (or the colon as a standalone token), raw="bysort foreign: gen idx=_n".
BysortPrefix parseBysortPrefix(const std::string& cmdLower, const std::vector<std::string>& varlist,
                               const std::string& raw) {
  BysortPrefix p;
  if (cmdLower != "bysort" && cmdLower != "by") return p;
  p.isBysort = true;
  p.sortFirst = (cmdLower == "bysort");

  // BS-002: byVars-collection loop, now paren-aware. A token opening with
  // '(' (and everything up to the token that closes it with ')') is the
  // sort-only parenthetical — collected into sortVars instead of byVars.
  // The trailing ':' that ends the by-spec may be glued to a plain byVar
  // token, to the token that CLOSES the parenthetical, or stand alone —
  // all three are handled uniformly below.
  std::optional<size_t> colonIdx;
  bool inParen = false;
  for (size_t i = 0; i < varlist.size(); ++i) {
    std::string tok = varlist[i];

    if (tok == ":") {
      colonIdx = i;
      break;
    }

    bool endsWithColon = (!tok.empty() && tok.back() == ':');
    if (endsWithColon) tok.pop_back();

    if (!inParen && !tok.empty() && tok.front() == '(') {
      tok.erase(tok.begin());
      inParen = true;  // may close within this same token, handled just below
    }

    if (inParen) {
      size_t closeParen = tok.find(')');
      if (closeParen != std::string::npos) {
        std::string before = tok.substr(0, closeParen);
        if (!before.empty()) p.sortVars.push_back(before);
        inParen = false;
      } else if (!tok.empty()) {
        p.sortVars.push_back(tok);
      }
    } else if (!tok.empty()) {
      p.byVars.push_back(tok);
    }

    if (endsWithColon) {
      colonIdx = i;
      break;
    }
  }

  if (!colonIdx.has_value()) {
    // Malformed — no colon found; everything is by_vars, no sub_cmd.
    return p;
  }

  std::vector<std::string> subTokens(varlist.begin() + static_cast<long>(*colonIdx) + 1,
                                     varlist.end());
  if (subTokens.empty()) return p;
  p.subCmd = subTokens[0];
  std::vector<std::string> subVarlist(subTokens.begin() + 1, subTokens.end());

  size_t colonPos = raw.find(':');
  if (colonPos == std::string::npos) {
    std::string joined = p.subCmd;
    for (const auto& t : subVarlist) joined += " " + t;
    p.subRaw = joined;
  } else {
    p.subRaw = trimWs(raw.substr(colonPos + 1));
  }
  return p;
}


// egen.py-adjacent: split `s` at the first COMMA outside parens (options
// separator) — mirrors bysort.py's _split_top_level_comma, used to inject a
// by() clause into an egen sub-command's option tail.
std::pair<std::string, std::optional<std::string>> splitTopLevelComma(const std::string& s) {
  int depth = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    char ch = s[i];
    if (ch == '(') {
      ++depth;
    } else if (ch == ')') {
      --depth;
    } else if (ch == ',' && depth == 0) {
      return {s.substr(0, i), s.substr(i + 1)};
    }
  }
  return {s, std::nullopt};
}


std::string joinSpace(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += " ";
    s += v[i];
  }
  return s;
}


// P2-PERF ARC-C C1-③ (factored out for BUG-532): is the key tuple `keys`
// already lexicographically non-decreasing, row by row, under gsort's OWN key
// semantics (src/data/frame.cpp `cols_as_double` + radix_sort.h
// `gsort_radix_key`)? A factor's key is its integer CODE as a double, an f64's
// key is its value, NaN sorts LAST, and the sort is stable — so a `true` here
// means a `sort KEYS` would be the identity permutation. The by-summarize
// driver asks exactly that (skip a no-op sort). It is deliberately NOT the
// level-string check the `by:` summarize branch uses for its r(5): that one
// reads factor LEVEL STRINGS, so it calls data PIET itself just sorted on a
// string key "not sorted". Every key must exist in `ds` (callers check first).
//
// `stataStringOrder` (BUG-532, the by-keep/drop branch): a string key is read
// in STATA's order instead — its level's byte-order rank, a blank first
// (handlers::stringKeyOrder) — so `true` means the rows are in the order a
// Stata `sort KEYS` leaves them. The two answers differ only on a string key
// whose dictionary is not byte-ordered.
bool keysAlreadyGrouped(Datastore& ds, const std::vector<std::string>& keyNames,
                        bool stataStringOrder ) {
  const int64_t nBase = ds.nRows();
  // gsort's key ordering, as a plain comparison: NaN above every number.
  auto keyLess = [stataStringOrder](double a, double b) {
    if (stataStringOrder) return cecon::stata_cmp(a, b) < 0;
    bool an = std::isnan(a), bn = std::isnan(b);
    if (an || bn) return !an && bn;  // NaN last; NaN vs NaN is neither
    return a < b;
  };
  struct GsortKeyView {
    bool isFactor = false;
    const double* f64 = nullptr;
    const int32_t* codes = nullptr;
    std::vector<double> rank;  // stataStringOrder: per code; blankRank for -1
    double blankRank = 0;
    double at(int64_t r) const {
      if (!isFactor) return f64[r];
      if (rank.empty()) return static_cast<double>(codes[r]);
      return codes[r] < 0 ? blankRank : rank[static_cast<size_t>(codes[r])];
    }
  };
  std::vector<GsortKeyView> keys;
  keys.reserve(keyNames.size());
  for (const auto& bv : keyNames) {
    GsortKeyView v;
    v.isFactor = ds.dtype(bv) != "f64";
    const cecon::Column& c = ds.col(bv);
    if (v.isFactor) {
      v.codes = c.codes();
      if (stataStringOrder) {
        handlers::StringKeyOrder o = handlers::stringKeyOrder(c.levels);
        if (!o.codeOrderIsStata) {
          v.rank = std::move(o.rank);
          v.blankRank = o.blankRank;
        }
      }
    } else {
      v.f64 = c.f64();
    }
    keys.push_back(std::move(v));
  }
  for (int64_t r = 1; r < nBase; ++r) {
    for (const auto& k : keys) {
      double prev = k.at(r - 1), cur = k.at(r);
      if (keyLess(cur, prev)) return false;  // strictly decreasing on this key
      if (keyLess(prev, cur)) break;         // strictly increasing -> row pair ok
      // equal on this key -> the next key decides
    }
  }
  return true;
}


// BUG-532: Stata's sortedby marker, as far as PIET can know it, for a plain
// `by KEYS:` (which Stata refuses r(5) unless KEYS lead the marker).
//
// Every `bysort KEYS [(SORT)]: CMD` sorts PIET's rows in place into the order
// Stata's `bysort` leaves (handlers::sortInStataOrder: runBysortGen, the egen
// and summarize/regress/tabstat branches, runByKeepIf), so after any bysort
// PIET's rows ARE Stata's rows and the engine file's own sort claim is the
// marker (below). No side ledger of row orders is kept: it was tried
// (task/w2-bykeep-v2 rounds 3-4) and each review found a new way for a
// ledger mark to disagree with the rows (a macro-spelled gsort, a fresh `use`,
// a re-used file), because a ledger describes lines, not rows.

// (2) The engine's own claim: the head file's `sorted_by` header key (BUG-272).
// Never false where present — Frame::gsort mints it only for ascending f64 keys,
// and every mutation that could void it clears it — and minted exactly where
// Stata's `sort` sets its marker, so a key tuple that does not lead it is one
// Stata calls "not sorted" even when the rows happen to be grouped (arm
// by_two_halfsorted: `sort g b` then `by g h:`). Empty = no claim (factor keys,
// a claimless transform, a file from before BUG-272): the caller falls back to
// the rows themselves. Read off the head's open Datastore (Datastore::sortedBy),
// never by reopening the file.
std::vector<std::string> engineSortClaim(const Datastore& ds) {
  std::vector<std::string> keys;
  std::istringstream in(ds.sortedBy());
  for (std::string k; in >> k;) keys.push_back(k);
  return keys;
}


bool keysLeadClaim(const std::vector<std::string>& keys, const std::vector<std::string>& claim) {
  if (keys.size() > claim.size()) return false;
  for (size_t i = 0; i < keys.size(); ++i)
    if (keys[i] != claim[i]) return false;
  return true;
}
}  // namespace piet::dispatch_detail
