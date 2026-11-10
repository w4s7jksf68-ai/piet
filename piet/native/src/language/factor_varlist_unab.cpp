// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// fvunab -- unabbreviate a factor-variable / time-series varlist into a local (variable-
// metadata family; shared helpers in datamgmt/variable_metadata_support.h).
//
//   fvunab    the varlist half is loops::expandVarlist (the `unab` arm's machinery) plus
//             display_hardening's resolveVarName for abbreviations; only the operator
//             prefixes are parsed here, because no other surface normalizes them.
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
#include "piet/handlers/variables.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // expandVarlist -- the ONE varlist wildcard/range expander
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "vendor/nlohmann/json.hpp"

#include "datamgmt/variable_metadata_support.h"

namespace {

// fvunab's own options.
const piet::eval::OptVocab& kVocabFvunab =
    piet::eval::registerOptVocab({"fvunab"}, {{"min", "max", "name"}, {}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace variable_metadata_detail;

// ===========================================================================
// fvunab
// ===========================================================================
//
// Implements fvunab per its help file; identified against StataBE probes
// (w2ax_t1_official.log fvunab_*, mx1/mx2/mx5.log FV|*):
//   * `fvunab lmacname : varlist [, min(#) max(#) name(str)]`; no colon r(198) "'' found
//     where ':' expected"; two names r(103); an invalid name r(198);
//   * the varlist is unab'd (wildcards, ranges, _all, unique abbreviations; r(111) for no
//     match / an ambiguous abbreviation / a reversed range);
//   * time-series operators need tsset/xtset (r(111) "time variable not set") and a numeric
//     variable (r(109)); they come back in canonical form: L and F net out, then D, then S,
//     a count of 1 unwritten and of 0 dropped (`l.` -> L., `l7.` -> L7., `l2f.` -> L.,
//     `LF.` -> nothing, `dl.`/`L.D.` -> LD., `sd.` -> DS., `l3l2.` -> L5.); a numlist
//     operator expands, variable by variable (`L(1/2).tau*` -> L.tau1 L2.tau1 L.tau2 ...);
//   * `c.` alone is dropped (`c.tau2` -> tau2); `i.`, `ib#.`/`b#.`, `ibn.`/`bn.`, `#.` (-> i#.)
//     are kept in lower case, and a variable's second factor term is dropped (`i.tau1
//     i.tau1 tau1` -> `i.tau1 tau1`, `ib2.tau1 i.tau1` -> `ib2.tau1`); a string variable as
//     a factor is r(109);
//   * an operator group that does not parse is r(198) "<term> invalid name";
//   * the local is set before the count check; fewer than min() r(102), more than max() r(103).
//   * an operator number past 2147483646 is r(198) "<op>:  operator invalid", a numlist of
//     more than 100 values (or two numlists in one term) r(198) "numlist in operator
//     invalid" (mx6/mx7.log); a factor level past it, which Stata wraps negative, is refused
//     the same way.
// Interactions (`#`, `##`), `o.`/`co.`, `i(numlist).`, `ib(first|last|freq).` and a factor
// operator combined with a time-series one are refused by name (r(199)): Stata accepts
// them and this build does not normalize them.
namespace {

struct TsOps {
  long long l = 0, d = 0, s = 0;
  std::string text() const {
    std::string out;
    auto part = [&](const char* c, long long k) {
      if (k <= 0) return;
      out += c;
      if (k > 1) out += std::to_string(k);
    };
    if (l > 0) part("L", l);
    else if (l < 0) part("F", -l);
    part("D", d);
    part("S", s);
    return out;
  }
};

// An operator's integer, or nullopt past 2147483646 -- the edge Stata measures
// (mx6/mx7.log: `L2147483648.` and `L(2147483647).` are r(198)); never a throwing parse.
std::optional<long long> opInt(const std::string& text) {
  size_t i = 0;
  bool neg = false;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) neg = text[i++] == '-';
  if (i >= text.size()) return std::nullopt;
  long long v = 0;
  for (; i < text.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(text[i]))) return std::nullopt;
    v = v * 10 + (text[i] - '0');
    if (v >= 2147483647LL) return std::nullopt;
  }
  return neg ? -v : v;
}

// A numlist inside an operator's parentheses: `1/3`, `3/1`, `1 3`, `1(1)3`, `-1`, `0`. At
// most 100 values (mx7.log: L(1/100). runs, L(1/101). is r(198)), counted BEFORE a range
// is filled, so `L(1/2000000000).` costs nothing.
std::optional<std::vector<long long>> opNumlist(const std::string& text) {
  constexpr long long kMax = 100;
  std::vector<long long> out;
  std::string t = text;
  std::replace(t.begin(), t.end(), ',', ' ');
  std::istringstream iss(t);
  static const std::regex reRange(R"(^([+-]?\d+)/([+-]?\d+)$)");
  static const std::regex reStep(R"(^([+-]?\d+)\(([+-]?\d+)\)([+-]?\d+)$)");
  for (std::string w; iss >> w;) {
    std::smatch m;
    if (auto one = opInt(w)) {
      out.push_back(*one);
    } else if (std::regex_match(w, m, reRange)) {
      const auto a = opInt(m.str(1)), b = opInt(m.str(2));
      if (!a || !b) return std::nullopt;
      const long long n = (*a <= *b ? *b - *a : *a - *b) + 1;
      if (static_cast<long long>(out.size()) + n > kMax) return std::nullopt;
      if (*a <= *b)
        for (long long k = *a; k <= *b; ++k) out.push_back(k);
      else
        for (long long k = *a; k >= *b; --k) out.push_back(k);
    } else if (std::regex_match(w, m, reStep)) {
      const auto a = opInt(m.str(1)), st = opInt(m.str(2)), b = opInt(m.str(3));
      if (!a || !st || !b || *st == 0) return std::nullopt;
      if ((*st > 0 && *a > *b) || (*st < 0 && *a < *b)) return std::nullopt;
      const long long n = (*st > 0 ? (*b - *a) / *st : (*a - *b) / -*st) + 1;
      if (static_cast<long long>(out.size()) + n > kMax) return std::nullopt;
      if (*st > 0)
        for (long long k = *a; k <= *b; k += *st) out.push_back(k);
      else
        for (long long k = *a; k >= *b; k += *st) out.push_back(k);
    } else {
      return std::nullopt;
    }
    if (static_cast<long long>(out.size()) > kMax) return std::nullopt;
  }
  if (out.empty()) return std::nullopt;
  return out;
}

// One parsed term: its operator prefix (already canonical) and the variables it names.
struct FvTerm {
  std::vector<std::string> tsPrefixes;  // one per numlist value ("" = no ts operator)
  std::string fvPrefix;                 // "i", "ib2", "ibn", "i2" or ""
  std::vector<std::string> vars;
};

// The term grammar is `[ops.]varspec`, where ops is a run of operator groups each ended by
// a `.` at paren depth 0 and varspec is a name/wildcard/range or a parenthesised varlist.
std::optional<ojson> parseFvTerm(const std::string& term, SessionState& session, FvTerm* out) {
  if (term.find('#') != std::string::npos)
    return notImplemented("fvunab: factor-variable interactions (`#`, `##`) are not implemented "
                          "in this version of PIET (" + term + ")");
  size_t lastDot = std::string::npos;
  int depth = 0;
  for (size_t i = 0; i < term.size(); ++i) {
    if (term[i] == '(') ++depth;
    else if (term[i] == ')') --depth;
    else if (term[i] == '.' && depth == 0) lastDot = i;
  }
  const std::string ops = lastDot == std::string::npos ? std::string() : term.substr(0, lastDot);
  std::string varspec = lastDot == std::string::npos ? term : term.substr(lastDot + 1);
  if (varspec.size() >= 2 && varspec.front() == '(' && varspec.back() == ')')
    varspec = varspec.substr(1, varspec.size() - 2);
  if (varspec.find('(') != std::string::npos || varspec.find(')') != std::string::npos)
    return rcError("nothing found where name expected", 198);  // `L(1/2)` with no variable
  if (varspec.find('.') != std::string::npos)
    return notImplemented("fvunab: an operator inside a parenthesised varlist is not implemented "
                          "in this version of PIET (" + term + ")");
  if (strutil::trim(varspec).empty()) return rcError("nothing found where name expected", 198);

  // Operator groups: split ops on depth-0 dots.
  std::vector<std::string> groups;
  {
    std::string cur;
    int d = 0;
    for (char c : ops) {
      if (c == '(') ++d;
      if (c == ')') --d;
      if (c == '.' && d == 0) {
        groups.push_back(cur);
        cur.clear();
        continue;
      }
      cur.push_back(c);
    }
    if (!ops.empty()) groups.push_back(cur);
  }
  static const std::regex reI(R"(^i$)", std::regex::icase);
  static const std::regex reC(R"(^c$)", std::regex::icase);
  static const std::regex reIb(R"(^i?b(\d+)$)", std::regex::icase);
  static const std::regex reIbParen(R"(^i?b\((\d+)\)$)", std::regex::icase);
  static const std::regex reIbn(R"(^i?bn$)", std::regex::icase);
  static const std::regex reIn(R"(^i?(\d+)$)", std::regex::icase);
  static const std::regex reFvRefused(R"(^(c?o|i\(.*\)|i?b\((first|last|freq)\))$)",
                                      std::regex::icase);
  std::string fv;
  std::vector<std::pair<char, std::vector<long long>>> tsAtoms;
  for (const auto& g : groups) {
    std::smatch m;
    if (g.empty()) return rcError(term + " invalid name", 198);
    std::string fvHere;
    if (std::regex_match(g, reC)) continue;  // `c.` is the default; dropped (fvunab_fv)
    const std::string invalidOp = g + ":  operator invalid";
    if (std::regex_match(g, reI)) {
      fvHere = "i";
    } else if (std::regex_match(g, m, reIb) || std::regex_match(g, m, reIbParen)) {
      // Past 2^31-1 Stata wraps the level to a negative one (mx7.log `ib2147483648.` ->
      // ib-2147483648); that is refused here rather than reproduced.
      auto lv = opInt(m.str(1));
      if (!lv) return rcError(invalidOp, 198);
      fvHere = "ib" + std::to_string(*lv);
    } else if (std::regex_match(g, reIbn)) {
      fvHere = "ibn";
    } else if (std::regex_match(g, m, reIn)) {
      auto lv = opInt(m.str(1));
      if (!lv) return rcError(invalidOp, 198);
      fvHere = "i" + std::to_string(*lv);
    }
    if (!fvHere.empty()) {
      if (!fv.empty())
        return notImplemented("fvunab: two factor-variable operators on one term are not "
                              "implemented in this version of PIET (" + term + ")");
      fv = fvHere;
      continue;
    }
    // A run of time-series atoms: a letter L F D S, then digits or a (numlist).
    size_t i = 0;
    bool ok = true;
    std::vector<std::pair<char, std::vector<long long>>> atoms;
    while (i < g.size()) {
      const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(g[i])));
      if (c != 'L' && c != 'F' && c != 'D' && c != 'S') {
        ok = false;
        break;
      }
      ++i;
      std::vector<long long> vals{1};
      if (i < g.size() && g[i] == '(') {
        size_t close = std::string::npos;
        int pd = 0;
        for (size_t j = i; j < g.size(); ++j) {
          if (g[j] == '(') ++pd;
          if (g[j] == ')' && --pd == 0) {
            close = j;
            break;
          }
        }
        if (close == std::string::npos) {
          ok = false;
          break;
        }
        auto nl = opNumlist(g.substr(i + 1, close - i - 1));
        if (!nl) return rcError(g + "\nnumlist in operator invalid", 198);  // mx7.log L(1/101).
        vals = *nl;
        i = close + 1;
      } else if (i < g.size() && std::isdigit(static_cast<unsigned char>(g[i]))) {
        size_t j = i;
        while (j < g.size() && std::isdigit(static_cast<unsigned char>(g[j]))) ++j;
        auto lag = opInt(g.substr(i, j - i));  // L2147483648. is r(198) (mx6.log)
        if (!lag) return rcError(g + ":  operator invalid", 198);
        vals = {*lag};
        i = j;
      }
      atoms.emplace_back(c, vals);
    }
    if (!ok) {
      if (std::regex_match(g, reFvRefused))
        return notImplemented("fvunab: the factor-variable operator `" + g + ".` is not "
                              "implemented in this version of PIET (" + term + ")");
      // A group mixing the factor letters with time-series ones (`iL.`, `Li2.`, `2b.`) is a
      // real operator this build does not normalize; any other letter is not an operator.
      bool fvLetters = false, other = false;
      for (char ch : g) {
        const char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (lc == 'i' || lc == 'c' || lc == 'b' || lc == 'n' || lc == 'o') fvLetters = true;
        else if (!(lc == 'l' || lc == 'f' || lc == 'd' || lc == 's' || std::isdigit(static_cast<unsigned char>(ch))))
          other = true;
      }
      if (fvLetters && !other)
        return notImplemented("fvunab: the operator `" + g + ".` (a factor-variable operator "
                              "combined with another) is not implemented in this version of "
                              "PIET (" + term + ")");
      return rcError(term + " invalid name", 198);
    }
    for (auto& a : atoms) tsAtoms.push_back(std::move(a));
  }
  if (!fv.empty() && !tsAtoms.empty())
    return notImplemented("fvunab: a factor-variable operator combined with a time-series "
                          "operator is not implemented in this version of PIET (" + term + ")");
  int multi = 0;
  for (const auto& a : tsAtoms)
    if (a.second.size() > 1) ++multi;
  if (multi > 1)  // mx7.log `L(1/2)D(1/2).a` r(198)
    return rcError(term + "\nnumlist in operator invalid", 198);

  std::vector<std::string> vars;
  if (auto e = unabVarlist(varspec, session, &vars)) return *e;

  out->fvPrefix = fv;
  out->tsPrefixes.clear();
  if (tsAtoms.empty()) {
    out->tsPrefixes.push_back(std::string());
  } else {
    if (!session.timevar.has_value() || session.timevar->empty())
      return rcError("time variable not set", 111);
    size_t multiAt = tsAtoms.size();
    for (size_t k = 0; k < tsAtoms.size(); ++k)
      if (tsAtoms[k].second.size() > 1) multiAt = k;
    const size_t nCombos = multiAt == tsAtoms.size() ? 1 : tsAtoms[multiAt].second.size();
    for (size_t c = 0; c < nCombos; ++c) {
      TsOps ts;
      for (size_t k = 0; k < tsAtoms.size(); ++k) {
        const long long v = k == multiAt ? tsAtoms[k].second[c] : tsAtoms[k].second.front();
        switch (tsAtoms[k].first) {
          case 'L': ts.l += v; break;
          case 'F': ts.l -= v; break;
          case 'D': ts.d += v; break;
          default: ts.s += v; break;
        }
      }
      out->tsPrefixes.push_back(ts.text());
    }
  }
  // Operators on a string variable (mx2.log FV|l.s, FV|i.s).
  if (!fv.empty() || !tsAtoms.empty()) {
    Datastore* ds = session.datastore();
    for (const auto& v : vars) {
      if (ds != nullptr && ds->has(v) && ds->dtype(v) == "factor") {
        if (!fv.empty())
          return rcError(v + ":  string variables may not be used as factor variables", 109);
        bool anyTs = false;
        for (const auto& p : out->tsPrefixes) anyTs = anyTs || !p.empty();
        if (anyTs) return rcError(v + ":  may not use time-series operators on string variables", 109);
      }
    }
  }
  out->vars = vars;
  return std::nullopt;
}

// Split the varlist text into terms at depth-0 blanks. Blanks around a depth-0 `-` go
// first (expandVarlist's own range rule: `tau1 - tau3` is one range), and an operator
// group ending in `.` followed by a blank joins the next word (`L. tau1` is L.tau1).
std::vector<std::string> fvTerms(const std::string& textIn) {
  std::string text;
  int depth = 0;
  for (size_t i = 0; i < textIn.size(); ++i) {
    const char c = textIn[i];
    if (c == '(') ++depth;
    if (c == ')') --depth;
    if (c == '-' && depth == 0) {
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
      text.push_back('-');
      while (i + 1 < textIn.size() && std::isspace(static_cast<unsigned char>(textIn[i + 1]))) ++i;
      continue;
    }
    text.push_back(c);
  }
  std::vector<std::string> raw;
  std::string cur;
  depth = 0;
  for (char c : text) {
    if (c == '(') ++depth;
    if (c == ')') --depth;
    if (std::isspace(static_cast<unsigned char>(c)) && depth == 0) {
      if (!cur.empty()) raw.push_back(cur);
      cur.clear();
      continue;
    }
    cur.push_back(c);
  }
  if (!cur.empty()) raw.push_back(cur);
  std::vector<std::string> out;
  for (size_t i = 0; i < raw.size(); ++i) {
    std::string t = raw[i];
    while (!t.empty() && t.back() == '.' && i + 1 < raw.size()) t += raw[++i];
    out.push_back(t);
  }
  return out;
}

}  // namespace

ojson runFvunab(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabFvunab.known,
                                            kVocabFvunab.unimplemented))
    return ojson(*err);
  ojson perr;
  auto minN = intOption(parsed, "min", 1, &perr);
  if (!minN) return perr;
  auto maxN = intOption(parsed, "max", 32000, &perr);
  if (!maxN) return perr;
  const std::string optName = strutil::trim(optStr(parsed, "name").value_or(std::string()));

  const std::string text = parsed.argText;
  size_t colon = std::string::npos;
  {
    const std::vector<bool> quoted = eval::quotedSpanMask(text);
    for (size_t i = 0; i < text.size(); ++i)
      if (text[i] == ':' && !quoted[i]) {
        colon = i;
        break;
      }
  }
  if (colon == std::string::npos) return rcError("'' found where ':' expected", 198);
  std::vector<std::string> names;
  {
    std::istringstream iss(text.substr(0, colon));
    for (std::string w; iss >> w;) names.push_back(w);
  }
  if (names.empty()) return rcError("nothing found where name expected", 198);
  if (names.size() > 1) return rcError("macro name:  too many specified", 103);
  const std::string lname = names.front();
  if (!strutil::isValidStataIdentifier(lname) || lname.size() > 31)
    return rcError(lname + " invalid name", 198);

  std::vector<std::string> result;
  std::set<std::string> fvSeen;  // variables already carrying a factor term
  for (const auto& term : fvTerms(text.substr(colon + 1))) {
    FvTerm ft;
    if (auto e = parseFvTerm(term, session, &ft)) return *e;
    for (const auto& v : ft.vars) {
      if (!ft.fvPrefix.empty()) {
        if (!fvSeen.insert(v).second) continue;
        result.push_back(ft.fvPrefix + "." + v);
        continue;
      }
      for (const auto& p : ft.tsPrefixes) result.push_back(p.empty() ? v : p + "." + v);
    }
  }
  const std::string joined = joinWords(result);
  session.eval.locals.set(lname, joined);  // c_local before the count check (FV|min2)

  const long long n = static_cast<long long>(result.size());
  if (*minN <= n && n <= *maxN) return silent();
  const int rc = n < *minN ? 102 : 103;
  const std::string what = n < *minN ? "too few variables specified" : "too many variables specified";
  if (optName.empty()) return rcError(joined + ":\n" + what, rc);
  std::string need;
  if (*minN == *maxN) need = std::to_string(*minN) + " variable" + (*maxN > 1 ? "s" : "") + " required";
  else if (*minN <= 0) need = *maxN == 1 ? "0 or 1 variables required" : std::to_string(*maxN) + " or fewer variables required";
  else if (*maxN >= 32000) need = std::to_string(*minN) + " or more variables required";
  else need = std::to_string(*minN) + " - " + std::to_string(*maxN) + " variables required";
  return rcError(optName + ": " + what + "\n" + std::string(optName.size() + 4, ' ') + need, rc);
}

}  // namespace piet::handlers
