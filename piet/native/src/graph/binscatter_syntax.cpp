// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/variables.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "io/win_compat.h"  // to_native_path / native_exists: binscatter's savedata files
#include "piet/cobra_runner.h"
#include "piet/dispatcher.h"  // dispatch -- binscatter reruns the ado's marksample/regressions
#include "piet/display_hardening.h"  // fmtStataDefaultG — Stata's %9.0g default rendering
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"  // findUnquotedComma — binscatter reads its options as typed
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/settings.h"
#include "piet/stata_format.h"  // fmt::general — %w.0g text of file write / regress
#include "piet/strutil.h"
#include "piet/twoway_opts.h"  // twoway::layerStyle — Arc G's graph half reuses it verbatim

#include "binscatter_internal.h"
#include "estimation/engine_report_support.h"

namespace piet {
namespace handlers {

namespace engine_report_detail {

namespace {

// binscatter's syntax line: each option's full name and the length
// of its capitalised stem (the minimum abbreviation). Lower-case declarations
// must be typed in full.
struct BsOpt {
  const char* name;
  size_t minLen;
};
constexpr BsOpt kBsSyntax[] = {
    {"by", 2},          {"nquantiles", 1}, {"genxq", 3},     {"discrete", 8},
    {"xq", 2},          {"medians", 3},    {"controls", 7},  {"absorb", 6},
    {"noaddmean", 3},   {"linetype", 4},   {"rd", 2},        {"reportreg", 9},
    {"colors", 3},      {"mcolors", 2},    {"lcolors", 2},   {"msymbols", 1},
    {"savegraph", 9},   {"savedata", 8},   {"replace", 7},   {"nofastxtile", 11},
    {"randvar", 7},     {"randcut", 7},    {"randn", 5},     {"nbins", 5},
    {"create_xq", 9},   {"x_q", 3},        {"symbols", 7},   {"method", 6},
    {"unique", 6},
};

// The option tail without a trailing `, if ...` / `, in ...` qualifier. The
// tokenizer has already moved that qualifier into parsed.ifExpr/inRange
// (B4/W1, tokenizer.cpp findTrailingQualifierComma) but parsed.raw still holds
// it, and left in the tail its words would reach the catch-all and be appended
// to e(graphcmd) (`binscatter price mpg, nq(10), if foreign==0`, the shape of
// 034/DoFiles_Sept2025/Figure1_Replication.do:41). Same rule as the tokenizer:
// a top-level comma, blanks, `if` or `in`, a blank and a non-blank tail; text
// after the qualifier's own next top-level comma goes back onto the options.
std::string bsDropTrailingQualifier(const std::string& post) {
  size_t base = 0;
  while (base < post.size()) {
    const long long rel = eval::findUnquotedComma(post.substr(base));
    if (rel < 0) return post;
    const size_t at = base + static_cast<size_t>(rel);
    size_t j = at + 1;
    while (j < post.size() && std::isspace(static_cast<unsigned char>(post[j]))) ++j;
    if (j + 2 < post.size() && post[j] == 'i' && (post[j + 1] == 'f' || post[j + 1] == 'n') &&
        std::isspace(static_cast<unsigned char>(post[j + 2])) && !strutil::trim(post.substr(j + 2)).empty()) {
      std::string out = post.substr(0, at);
      const std::string qual = post.substr(at + 1);
      const long long more = eval::findUnquotedComma(qual);
      if (more >= 0) out += " " + qual.substr(static_cast<size_t>(more) + 1);
      return out;
    }
    base = at + 1;
  }
  return post;
}

}  // namespace

// Splits the option tail on blanks outside parentheses and quotes (plain "..."
// and compound `"..."'), as Stata's `syntax` sees the options.
std::vector<RawOpt> bsRawOptions(const std::string& raw) {
  std::vector<RawOpt> out;
  const long long comma = eval::findUnquotedComma(raw);
  if (comma < 0) return out;
  const std::string tail = bsDropTrailingQualifier(raw.substr(static_cast<size_t>(comma) + 1));
  size_t i = 0;
  const size_t n = tail.size();
  while (i < n) {
    while (i < n && std::isspace(static_cast<unsigned char>(tail[i]))) ++i;
    if (i >= n) break;
    const size_t start = i;
    int depth = 0;
    int compound = 0;
    bool inQuote = false;
    while (i < n) {
      const char c = tail[i];
      if (c == '`' && i + 1 < n && tail[i + 1] == '"') {
        ++compound;
        i += 2;
        continue;
      }
      if (compound > 0 && c == '"' && i + 1 < n && tail[i + 1] == '\'') {
        --compound;
        i += 2;
        continue;
      }
      if (compound == 0 && c == '"') {
        inQuote = !inQuote;
        ++i;
        continue;
      }
      if (!inQuote && compound == 0) {
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (depth <= 0 && std::isspace(static_cast<unsigned char>(c))) break;
      }
      ++i;
    }
    RawOpt o;
    o.text = tail.substr(start, i - start);
    const size_t open = o.text.find('(');
    if (open == std::string::npos) {
      o.name = o.text;
    } else {
      o.name = o.text.substr(0, open);
      const size_t close = o.text.rfind(')');
      o.arg = close != std::string::npos && close > open ? o.text.substr(open + 1, close - open - 1)
                                                         : o.text.substr(open + 1);
    }
    for (auto& ch : o.name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    out.push_back(std::move(o));
  }
  return out;
}

// The binscatter option a typed name abbreviates, or "" (it goes to the
// catch-all `*`, i.e. to twoway, ado).
std::string bsMatch(const std::string& typed) {
  for (const auto& o : kBsSyntax) {
    const std::string full = o.name;
    if (typed.size() >= o.minLen && typed.size() <= full.size() && full.compare(0, typed.size(), typed) == 0)
      return full;
  }
  return std::string();
}

// An error block with Stata's rc.
ojson bsRcError(const std::string& message, int rc, const std::string& code) {
  return ojson(eval::errorBlock(code, message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

// FU-1088: by(varname), absorb(varname) and CONTROLs(varlist numeric ts fv)
// (binscatter) are resolved by Stata's `syntax` before the ado runs,
// so a unique prefix stands for its variable, a wildcard or range expands and
// several prefix matches are r(111). The rule is THE one the by/bysort keys
// use (handlers::resolveByKeyNames); a factor-variable term keeps its
// operator (`i.`, `c.`, `ib2.`, `L.`) and its `#`/`##` joins, and only the
// name after the operator resolves (several names expand term by term, as
// fvexpand does). A parenthesised group is one term even when it holds
// spaces, and `op.(a b)` expands to `op.a op.b` as fvexpand does (each name
// resolved); a lag list `L(1/2).x` keeps its operator and resolves `x`.
// DESIGN DECISIONS (no oracle arm): a name that matches no variable is
// r(111) "variable <name> not found"; unbalanced parentheses are r(198)
// "invalid syntax"; a varname option that expands to several variables is
// r(103) "too many variables specified".
std::optional<ojson> bsResolveNames(SessionState& session, std::string& text, bool single) {
  std::vector<std::string> tokens;
  {
    std::string cur;
    int depth = 0;
    for (char c : text) {
      if (c == '(') ++depth;
      if (c == ')' && --depth < 0) return bsRcError("invalid syntax", 198);
      if (depth == 0 && (c == ' ' || c == '\t' || c == ',')) {
        if (!cur.empty()) tokens.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (depth != 0) return bsRcError("invalid syntax", 198);
    if (!cur.empty()) tokens.push_back(cur);
  }
  if (tokens.empty()) return std::nullopt;
  Datastore* ds = session.datastore();
  std::vector<std::string> outTerms;
  size_t nameCount = 0;
  auto resolveOne = [&](const std::string& name, std::vector<std::string>& got) -> std::optional<ojson> {
    std::vector<std::string> names = {name};
    if (std::optional<ojson> err = resolveByKeyNames(session, names)) return err;
    for (const auto& n : names) {
      if (ds != nullptr && !ds->has(n)) return bsRcError("variable " + n + " not found", 111);
    }
    got = names;
    return std::nullopt;
  };
  for (const auto& tok : tokens) {
    if (tok.find_first_of(".#(") == std::string::npos) {
      std::vector<std::string> got;
      if (std::optional<ojson> err = resolveOne(tok, got)) return err;
      nameCount += got.size();
      outTerms.insert(outTerms.end(), got.begin(), got.end());
      continue;
    }
    // A factor-variable term: parts joined by `#` or `##` outside parentheses
    // (a `#` inside a group belongs to that group's element, BUG-1464).
    std::vector<std::string> combos = {std::string()};
    size_t pos = 0;
    while (pos <= tok.size()) {
      size_t hash = tok.size();
      for (size_t k = pos, d = 0; k < tok.size(); ++k) {
        if (tok[k] == '(') ++d;
        else if (tok[k] == ')' && d > 0) --d;
        else if (tok[k] == '#' && d == 0) {
          hash = k;
          break;
        }
      }
      size_t next = hash;
      while (next < tok.size() && tok[next] == '#') ++next;
      const std::string part = tok.substr(pos, hash - pos);
      const std::string sep = tok.substr(hash, next - hash);
      std::vector<std::string> got;
      const size_t dotOpen = part.rfind(".(");
      const size_t open = !part.empty() && part[0] == '(' ? 0
                          : dotOpen == std::string::npos  ? std::string::npos
                                                          : dotOpen + 1;
      if (open != std::string::npos && part.back() == ')') {
        // `op.(a b)` / `(a b)`: every name of the group under the operator.
        const std::string op = part.substr(0, open);
        for (const std::string& inner : splitWsOrComma(part.substr(open + 1, part.size() - open - 2))) {
          if (inner.find('#') != std::string::npos) {
            // An interaction in the group keeps its own operators and is not
            // under the group's: Stata reads `c.(a#b z)` as `a#b c.z` (StataBE
            // 19.5, BUG-1464: regress colnames 0b.a#0b.b ... 4.a#6.b z).
            std::string sub = inner;
            if (std::optional<ojson> err = bsResolveNames(session, sub, false)) return err;
            got.push_back(sub);
            continue;
          }
          const size_t d = inner.rfind('.');
          const std::string iop = d == std::string::npos ? std::string() : inner.substr(0, d + 1);
          std::vector<std::string> names;
          if (std::optional<ojson> err = resolveOne(inner.substr(d == std::string::npos ? 0 : d + 1), names))
            return err;
          for (const auto& n : names) got.push_back(op + iop + n);
        }
        if (got.empty()) return bsRcError("invalid syntax", 198);
      } else {
        const size_t dot = part.rfind('.');
        const std::string op = dot == std::string::npos ? std::string() : part.substr(0, dot + 1);
        const std::string nm = dot == std::string::npos ? part : part.substr(dot + 1);
        std::vector<std::string> names = {nm};
        if (!nm.empty()) {
          if (std::optional<ojson> err = resolveOne(nm, names)) return err;
        }
        for (const auto& n : names) got.push_back(op + n);
      }
      std::vector<std::string> grown;
      for (const auto& c : combos)
        for (const auto& g : got) grown.push_back(c + g + sep);
      combos.swap(grown);
      if (next >= tok.size()) break;
      pos = next;
    }
    nameCount += combos.size();
    outTerms.insert(outTerms.end(), combos.begin(), combos.end());
  }
  if (single && nameCount > 1) return bsRcError("too many variables specified", 103);
  std::string joined;
  for (const auto& t : outTerms) joined += (joined.empty() ? "" : " ") + t;
  text = joined;
  return std::nullopt;
}

}  // namespace engine_report_detail

}  // namespace handlers
}  // namespace piet
