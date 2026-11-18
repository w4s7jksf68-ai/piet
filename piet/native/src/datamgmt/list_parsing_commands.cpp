// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Programmer list parsers that store their answer: `numlist` (r(numlist)),
// `tsunab` (a local) and `fvexpand` (r(varlist), r(fvops), r(tsops)).
//
// Their natural home is beside `fvunab` in language/; they live here because the
// vs-stata repair lane that added them (2026-11-11) does not own language/.
// Each reuses the machinery that already exists rather than growing a second
// copy: numlist's ranges go through loops::expandNumlist (forvalues' expander),
// tsunab is fvunab restricted to time-series operators, and fvexpand evaluates
// a time-series operand through generate (ScratchColumns).
//
// Behaviour is StataBE 19.5's as the vs-stata truth bank of 2026-11-10 records it
// (cells `numlist`, `extfuncs`, `varlist_ts`, `varlist_fv`, `fv_weights_more`),
// with [P] numlist, [P] unab and [P] fvexpand for the wording.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <regex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "engine/scratch_columns.h"
#include "language/command_reply_support.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"  // resolveVarName
#include "piet/eval/errors.h"
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/cclass.h"  // kSetMarkerPrefix: `set fvbase off`
#include "piet/handlers/variables.h"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"
#include "piet/strutil.h"
#include "vendor/nlohmann/json.hpp"

namespace {

// [P] numlist: asc(ending) desc(ending) int(eger) miss(ingokay) min() max()
// r(ange)() sort, and the undocumented unique. `r` is below the two-character
// abbreviation floor, so it is declared; the other minimums are in
// dispatch/options_support.cpp's kOptMinimumsLater.
const piet::eval::OptVocab& kVocabNumlist = piet::eval::registerOptVocab(
    {"numlist"}, {{"ascending", "descending", "integer", "missingokay", "min", "max", "range", "sort",
                   "unique"},
                  {},
                  {{"r", "range"}}});

// [P] unab: tsunab takes fvunab's options.
const piet::eval::OptVocab& kVocabTsunab =
    piet::eval::registerOptVocab({"tsunab"}, {{"min", "max", "name"}, {}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace command_reply_detail;

namespace {

// ===========================================================================
// numlist
// ===========================================================================

// The bank brackets numlist's default element cap: `numlist "1/2500"` runs and
// `numlist "1/3000"` is r(123) "too many elements" (StataBE 19.5; [P] numlist's
// "max(1600)" is out of date). 2500 is the largest measured count that runs.
constexpr long long kNumlistDefaultMax = 2500;

// One numlist element: a number, or a missing value (`.` = kind 0, `.a`.. = 1..26).
struct NlValue {
  double v = 0.0;
  int missingKind = -1;  // -1 = a number
  bool missing() const { return missingKind >= 0; }
};

// Stata's sort order: every number, then `.`, `.a`, ..., `.z`.
bool nlLess(const NlValue& a, const NlValue& b) {
  if (a.missing() != b.missing()) return !a.missing();
  if (a.missing()) return a.missingKind < b.missingKind;
  return a.v < b.v;
}

std::string nlText(const NlValue& x) {
  if (x.missing()) return x.missingKind == 0 ? "." : std::string(".") + static_cast<char>('a' + x.missingKind - 1);
  return fmt::general(x.v == 0.0 ? 0.0 : x.v, 15, 0);
}

// The tokens of a numlist: numbers (as typed), missing values, `/`, `:`, `to`,
// and a step in parentheses or brackets (kept as its inner text).
struct NlToken {
  enum Kind { kNumber, kMissing, kSlash, kColon, kTo, kStep } kind;
  std::string text;
  NlValue value;
};

bool scanNumber(const std::string& s, size_t& i, std::string* out) {
  const size_t start = i;
  size_t j = i;
  if (j < s.size() && (s[j] == '+' || s[j] == '-')) ++j;
  size_t digits = 0;
  while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j, ++digits;
  if (j < s.size() && s[j] == '.') {
    ++j;
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j, ++digits;
  }
  if (digits == 0) return false;
  if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
    size_t k = j + 1;
    if (k < s.size() && (s[k] == '+' || s[k] == '-')) ++k;
    size_t ed = 0;
    while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k, ++ed;
    if (ed == 0) return false;
    j = k;
  }
  *out = s.substr(start, j - start);
  i = j;
  return true;
}

bool tokenizeNumlist(const std::string& s, std::vector<NlToken>* out) {
  size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
      ++i;
      continue;
    }
    NlToken t{NlToken::kNumber, std::string(), {}};
    if (c == '/') {
      t.kind = NlToken::kSlash;
      ++i;
    } else if (c == ':') {
      t.kind = NlToken::kColon;
      ++i;
    } else if (c == '(' || c == '[') {
      const char close = c == '(' ? ')' : ']';
      const size_t end = s.find(close, i + 1);
      if (end == std::string::npos) return false;
      t.kind = NlToken::kStep;
      t.text = strutil::trim(s.substr(i + 1, end - i - 1));
      i = end + 1;
    } else if ((c == 't' || c == 'T') && i + 1 < s.size() && (s[i + 1] == 'o' || s[i + 1] == 'O') &&
               (i + 2 == s.size() || !std::isalnum(static_cast<unsigned char>(s[i + 2])))) {
      t.kind = NlToken::kTo;
      i += 2;
    } else if (c == '.' && (i + 1 == s.size() || !std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
      // `.` or `.a`..`.z`
      t.kind = NlToken::kMissing;
      t.value.missingKind = 0;
      ++i;
      if (i < s.size() && s[i] >= 'a' && s[i] <= 'z' &&
          (i + 1 == s.size() || !std::isalnum(static_cast<unsigned char>(s[i + 1])))) {
        t.value.missingKind = s[i] - 'a' + 1;
        ++i;
      }
    } else {
      std::string num;
      if (!scanNumber(s, i, &num)) return false;
      if (i < s.size() && (std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_'))
        return false;  // `1a`
      t.text = num;
      try {
        t.value.v = std::stod(num);
      } catch (...) {
        return false;
      }
    }
    out->push_back(std::move(t));
  }
  return true;
}

std::string exactText(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

// Expands one range through the shared expander. *tooMany when the range alone
// would pass the expander's own element cap.
bool expandRange(const std::string& start, const std::string& step, const std::string& end,
                 std::vector<NlValue>* out, bool* tooMany) {
  double a = 0, d = 0, b = 0;
  try {
    a = std::stod(start);
    b = std::stod(end);
    d = step.empty() ? 1.0 : std::stod(step);
  } catch (...) {
    return false;
  }
  if (d == 0.0) return false;
  if (std::fabs((b - a) / d) > 9999.0) {
    *tooMany = true;
    return false;
  }
  std::vector<std::string> elements;
  const std::string spec = step.empty() ? start + "/" + end : start + "(" + step + ")" + end;
  if (!loops::expandNumlist(spec, &elements)) return false;
  for (const auto& e : elements) {
    NlValue x;
    try {
      x.v = std::stod(e);
    } catch (...) {
      return false;
    }
    out->push_back(x);
  }
  return true;
}

// The whole numlist grammar ([P] numlist, [U] 11.1.8): `#`, `.`, `#/#`,
// `#(#)#`, `#[#]#`, `# # : #` and `# # to #`. false = r(121); *tooMany = r(123).
bool parseNumlistText(const std::string& s, std::vector<NlValue>* out, bool* tooMany) {
  std::vector<NlToken> toks;
  if (!tokenizeNumlist(s, &toks)) return false;
  // How many trailing elements of `out` came from plain numbers, so `# # : #` can
  // take its two numbers back.
  size_t plainRun = 0;
  std::vector<std::string> plainText;  // the typed text of those plain numbers
  for (size_t i = 0; i < toks.size(); ++i) {
    const NlToken& t = toks[i];
    if (t.kind == NlToken::kMissing) {
      out->push_back(t.value);
      plainRun = 0;
      plainText.clear();
      continue;
    }
    if (t.kind != NlToken::kNumber) {
      if (t.kind != NlToken::kColon && t.kind != NlToken::kTo) return false;
      // `#1 #2 : #3`: from #1 by (#2 - #1) to #3.
      if (plainRun < 2 || i + 1 >= toks.size() || toks[i + 1].kind != NlToken::kNumber) return false;
      const std::string first = plainText[plainText.size() - 2];
      const std::string second = plainText.back();
      const double d = std::stod(second) - std::stod(first);
      out->pop_back();
      out->pop_back();
      if (!expandRange(first, exactText(d), toks[i + 1].text, out, tooMany)) return false;
      ++i;
      plainRun = 0;
      plainText.clear();
      continue;
    }
    // A number: alone, or the start of `#/#` or `#(#)#`.
    if (i + 2 < toks.size() && toks[i + 1].kind == NlToken::kSlash) {
      if (toks[i + 2].kind != NlToken::kNumber) return false;
      if (!expandRange(t.text, std::string(), toks[i + 2].text, out, tooMany)) return false;
      i += 2;
      plainRun = 0;
      plainText.clear();
      continue;
    }
    if (i + 1 < toks.size() && toks[i + 1].kind == NlToken::kSlash) return false;
    if (i + 2 < toks.size() && toks[i + 1].kind == NlToken::kStep) {
      if (toks[i + 2].kind != NlToken::kNumber) return false;
      std::string step;
      size_t k = 0;
      if (!scanNumber(toks[i + 1].text, k, &step) || k != toks[i + 1].text.size()) return false;
      if (!expandRange(t.text, step, toks[i + 2].text, out, tooMany)) return false;
      i += 2;
      plainRun = 0;
      plainText.clear();
      continue;
    }
    if (i + 1 < toks.size() && toks[i + 1].kind == NlToken::kStep) return false;
    out->push_back(t.value);
    ++plainRun;
    plainText.push_back(t.text);
  }
  return true;
}

// `range(>=1 <=3)`: each bound is an operator glued to a number.
struct NlBound {
  std::string op;
  double v = 0;
};
bool parseRange(const std::string& text, std::vector<NlBound>* out) {
  const std::vector<std::string> words = splitWs(text);
  if (words.empty() || words.size() > 2) return false;
  for (const auto& w : words) {
    NlBound b;
    size_t k = 0;
    if (w.compare(0, 2, "<=") == 0 || w.compare(0, 2, ">=") == 0) k = 2;
    else if (!w.empty() && (w[0] == '<' || w[0] == '>')) k = 1;
    else return false;
    b.op = w.substr(0, k);
    std::string num;
    size_t j = k;
    if (!scanNumber(w, j, &num) || j != w.size()) return false;
    b.v = std::stod(num);
    out->push_back(b);
  }
  return true;
}

bool inBounds(double v, const std::vector<NlBound>& bounds) {
  for (const auto& b : bounds) {
    if (b.op == "<" && !(v < b.v)) return false;
    if (b.op == "<=" && !(v <= b.v)) return false;
    if (b.op == ">" && !(v > b.v)) return false;
    if (b.op == ">=" && !(v >= b.v)) return false;
  }
  return true;
}

// The text inside one pair of plain or compound double quotes, else as typed.
std::string dequoteArgument(const std::string& raw) {
  std::string s = strutil::trim(raw);
  if (s.size() >= 4 && s[0] == '`' && s[1] == '"' && s[s.size() - 2] == '"' && s.back() == '\'')
    return s.substr(2, s.size() - 4);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
  return s;
}

std::optional<long long> intOpt(const eval::ParsedCommand& parsed, const std::string& key,
                                long long dflt, ojson* err) {
  const auto v = optStr(parsed.opts, key);
  if (!v.has_value()) return dflt;
  const auto d = parseNumber(strutil::trim(*v));
  if (!d.has_value() || *d != std::floor(*d)) {
    *err = envError("option " + key + "() incorrectly specified", 198);
    return std::nullopt;
  }
  return static_cast<long long>(*d);
}

}  // namespace

ojson runNumlist(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabNumlist.known,
                                            kVocabNumlist.unimplemented))
    return ojson(*err);
  ojson err;
  const auto minN = intOpt(parsed, "min", 1, &err);
  if (!minN) return err;
  const auto maxN = intOpt(parsed, "max", kNumlistDefaultMax, &err);
  if (!maxN) return err;
  std::vector<NlBound> bounds;
  if (const auto r = optStr(parsed.opts, "range"); r.has_value()) {
    if (!parseRange(*r, &bounds)) return envError("option range() incorrectly specified", 198);
  }
  const bool ascending = hasOpt(parsed.opts, "ascending");
  const bool descending = hasOpt(parsed.opts, "descending");
  const bool integer = hasOpt(parsed.opts, "integer");
  const bool missingOk = hasOpt(parsed.opts, "missingokay");
  const bool sortIt = hasOpt(parsed.opts, "sort");

  std::vector<NlValue> values;
  bool tooMany = false;
  if (!parseNumlistText(dequoteArgument(parsed.argText), &values, &tooMany)) {
    if (tooMany) return envError("invalid numlist has too many elements", 123);
    return envError("invalid numlist", 121);
  }
  const long long n = static_cast<long long>(values.size());
  if (n < *minN) return envError("invalid numlist has too few elements", 122);
  if (n > *maxN) return envError("invalid numlist has too many elements", 123);
  for (const auto& x : values)
    if (x.missing() && !missingOk) return envError("invalid numlist has missing values", 127);
  if (integer)
    for (const auto& x : values)
      if (!x.missing() && x.v != std::floor(x.v))
        return envError("invalid numlist has noninteger elements", 126);
  if (!bounds.empty())
    for (const auto& x : values)
      if (!x.missing() && !inBounds(x.v, bounds))
        return envError("invalid numlist has elements outside of allowed range", 125);
  for (size_t i = 1; i < values.size(); ++i) {
    if ((ascending && !nlLess(values[i - 1], values[i])) ||
        (descending && !nlLess(values[i], values[i - 1])))
      return envError("invalid numlist has elements out of order", 124);
  }
  if (sortIt) std::stable_sort(values.begin(), values.end(), nlLess);
  // `unique` (undocumented, StataBE 19.5: `numlist "5 1 3 1", unique` is
  // 5 1 3, truth bank cell 98aac070735d6a6b): each value once, first place kept.
  if (hasOpt(parsed.opts, "unique")) {
    std::vector<NlValue> kept;
    for (const auto& x : values) {
      bool seen = false;
      for (const auto& k : kept)
        seen = seen || (!nlLess(x, k) && !nlLess(k, x));
      if (!seen) kept.push_back(x);
    }
    values = std::move(kept);
  }

  std::string joined;
  for (size_t i = 0; i < values.size(); ++i) joined += (i ? " " : "") + nlText(values[i]);
  eval::replaceRClass(session.eval);
  // An empty r(numlist) is an unset one, as for any macro.
  if (!joined.empty()) session.eval.rStrings.set("numlist", joined);
  eval::declareRPosted(session.eval, "numlist", joined.empty() ? std::vector<std::string>{}
                                                               : std::vector<std::string>{"numlist"});
  return ojson::array();
}

// ===========================================================================
// tsunab
// ===========================================================================

namespace {

// Whether one operator group (the text between depth-0 dots before the variable
// part of a term) is a factor-variable operator: i, c, o, b#, bn, i(...), ...
bool isFactorOperatorGroup(const std::string& g) {
  if (g.empty()) return false;
  bool tsOnly = true;
  size_t i = 0;
  while (i < g.size()) {
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(g[i])));
    if (c != 'L' && c != 'F' && c != 'D' && c != 'S') {
      tsOnly = false;
      break;
    }
    ++i;
    if (i < g.size() && g[i] == '(') {
      const size_t close = g.find(')', i);
      if (close == std::string::npos) {
        tsOnly = false;
        break;
      }
      i = close + 1;
    } else {
      while (i < g.size() && std::isdigit(static_cast<unsigned char>(g[i]))) ++i;
    }
  }
  if (tsOnly) return false;
  const char c0 = static_cast<char>(std::tolower(static_cast<unsigned char>(g[0])));
  return c0 == 'i' || c0 == 'c' || c0 == 'o' || c0 == 'b' || std::isdigit(static_cast<unsigned char>(g[0]));
}

// Splits text on depth-0 blanks (defined with fvexpand below).
std::vector<std::string> depthZeroWords(const std::string& text);

// True when the varlist uses a factor-variable operator or an interaction.
bool usesFactorOperators(const std::string& text) {
  int depth = 0;
  for (char c : text) {
    if (c == '(') ++depth;
    if (c == ')') --depth;
    if (c == '#' && depth == 0) return true;
  }
  // Each blank-separated term: its depth-0 dot groups before the last dot.
  std::string term;
  auto judge = [&](const std::string& t) {
    std::vector<std::string> groups;
    std::string cur;
    int d = 0;
    for (char c : t) {
      if (c == '(') ++d;
      if (c == ')') --d;
      if (c == '.' && d == 0) {
        groups.push_back(cur);
        cur.clear();
        continue;
      }
      cur.push_back(c);
    }
    for (const auto& g : groups)
      if (isFactorOperatorGroup(g)) return true;
    return false;
  };
  depth = 0;
  for (char c : text + " ") {
    if (c == '(') ++depth;
    if (c == ')') --depth;
    if (std::isspace(static_cast<unsigned char>(c)) && depth == 0) {
      if (!term.empty() && judge(term)) return true;
      term.clear();
      continue;
    }
    term.push_back(c);
  }
  return false;
}

}  // namespace

ojson runTsunab(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabTsunab.known,
                                            kVocabTsunab.unimplemented))
    return ojson(*err);
  const std::string& text = parsed.argText;
  const size_t colon = text.find(':');
  if (colon != std::string::npos && usesFactorOperators(text.substr(colon + 1)))
    return envError("factor-variable operators not allowed", 101);
  // An operator applies to named variables: `L.*` (every variable) is "* invalid
  // name" r(198) in StataBE 19.5 (truth bank cell bb90ac8e59e879bd), where
  // fvunab's varlist expansion would list them all.
  if (colon != std::string::npos) {
    for (const auto& word : depthZeroWords(text.substr(colon + 1))) {
      int depth = 0;
      size_t lastDot = std::string::npos;
      for (size_t i = 0; i < word.size(); ++i) {
        if (word[i] == '(') ++depth;
        else if (word[i] == ')') --depth;
        else if (word[i] == '.' && depth == 0) lastDot = i;
      }
      if (lastDot != std::string::npos && word.substr(lastDot + 1) == "*")
        return envError("* invalid name", 198);
    }
  }
  // Everything else is fvunab's grammar with only time-series operators in it.
  return runFvunab(parsed, session);
}

// ===========================================================================
// fvexpand
// ===========================================================================
//
// [P] fvexpand: the general factor varlist, expanded to the specific one over the
// data (if/in). The rules, each read off the bank's fvexpand cells:
//   * a factor atom lists its levels ascending, the base marked `b` (default the
//     smallest; ib#/ib(#) that level, added when unobserved -- ib9.rep78 ends
//     `9b.rep78`; ib(first|last|freq); ibn marks the first level `bn` and has no
//     base); i(numlist) keeps only those levels; io(numlist)/io# marks them `o`;
//     a variable's base is set once for the whole varlist; a variable with no
//     operator inside `#` is a factor. (`set fvbase off`, which would drop the
//     base marks, is refused upstream until estimation honours it.)
//   * `c.x` alone is `x`, `o.x` alone is `o.x`; inside an interaction an `o.` on a
//     continuous variable reads as `c.` (o.mpg#i.foreign);
//   * `A##B##C` is A, B, A#B, C, A#C, B#C, A#B#C, a repeated subterm once
//     (c.mpg##c.mpg is `mpg c.mpg#c.mpg`); an atom repeated inside one
//     interaction is one atom (i.foreign#i.foreign is i.foreign);
//   * an interaction's cells run with the first factor slowest, factor atoms
//     first, then the continuous ones (`0b.foreign#c.mpg`). A cell whose factor
//     atoms are all at base, with nothing continuous, is the base. A cell with an
//     atom at base is omitted when the term left without those atoms is in the
//     varlist anywhere; an empty cell (no observation with those levels) is
//     omitted. Omitted marks an `o` on each atom off its base (`1bno`, `co`);
//   * a time-series operator on a factor atom is its levels' suffix (`0bL2.c`),
//     the lag running slowest;
//   * a factor variable with a negative or non-integer value is r(452).
// r(fvops) is "true" when a factor atom, an interaction or `o.` appears, r(tsops)
// when a time-series operator does.

namespace {

const piet::eval::OptVocab& kVocabFvexpand =
    piet::eval::registerOptVocab({"fvexpand"}, {{}, {}});

struct FxAtom {
  std::string var;
  std::string ts;          // canonical ts prefix ("" none), e.g. "L2", "LD"
  bool factor = false;
  bool omittedOp = false;  // `o.` on a continuous variable
  // factor only:
  int baseRule = 0;        // 0 smallest, 1 explicit, 2 last, 3 freq, 4 none (bn), 5 #k-th level
  double base = 0;         // the level (rule 1) or k (rule 5)
  bool explicitBase = false;  // an ib*/ibn operator was written on this atom
  bool explicitC = false;     // `c.` was written on this atom
  std::optional<std::vector<double>> only;   // i(numlist)
  std::vector<double> omitLevels;            // io(numlist)
  std::vector<double> levels;                // resolved
  std::vector<double> values;                // per row (NaN = missing / out)
  bool hasBase() const { return baseRule != 4; }
  double resolvedBase = 0;
  std::string key() const { return (factor ? "i" : "c") + ts + "." + var; }
};

struct FxTerm {
  std::vector<FxAtom> atoms;
};

// A factor level as an operator writes it: a nonnegative integer (fvunab's own
// edge, 2147483646, bounds it). `ib(-1).`, `ib(2.5).`, `i(2.5 3).` and
// `io(2.5).` are invalid operators.
bool isLevelNumber(double v) { return v >= 0 && v == std::floor(v) && v <= 2147483646.0; }

std::optional<std::vector<double>> numlistValues(const std::string& text) {
  std::vector<NlValue> v;
  bool tooMany = false;
  if (!parseNumlistText(text, &v, &tooMany)) return std::nullopt;
  std::vector<double> out;
  for (const auto& x : v) {
    if (x.missing()) return std::nullopt;
    out.push_back(x.v);
  }
  return out;
}

std::optional<std::vector<double>> levelList(const std::string& text) {
  auto v = numlistValues(text);
  if (!v) return std::nullopt;
  for (double x : *v)
    if (!isLevelNumber(x)) return std::nullopt;
  return v;
}

// Splits a term on depth-0 `#` (`##` = factorial). nullopt when `#` and `##` mix.
std::optional<std::vector<std::string>> splitInteraction(const std::string& term, bool* factorial) {
  std::vector<std::string> parts;
  std::string cur;
  int depth = 0;
  bool single = false, dbl = false;
  for (size_t i = 0; i < term.size(); ++i) {
    const char c = term[i];
    if (c == '(') ++depth;
    if (c == ')') --depth;
    if (c == '#' && depth == 0) {
      if (i + 1 < term.size() && term[i + 1] == '#') {
        dbl = true;
        ++i;
      } else {
        single = true;
      }
      parts.push_back(cur);
      cur.clear();
      continue;
    }
    cur.push_back(c);
  }
  parts.push_back(cur);
  if (single && dbl) return std::nullopt;
  *factorial = dbl;
  return parts;
}

// One atom's operator groups -> its spec (variables resolved by the caller).
// `lags` receives the time-series alternatives (one per numlist value).
std::optional<ojson> parseAtomOps(const std::vector<std::string>& groups, FxAtom* a,
                                  std::vector<std::string>* lags) {
  static const std::regex reIb(R"(^i?b(\d+)$)", std::regex::icase);
  static const std::regex reIbParen(R"(^i?b\((.+)\)$)", std::regex::icase);
  static const std::regex reIo(R"(^i?o\((.+)\)$)", std::regex::icase);
  static const std::regex reIoNum(R"(^i?o(\d+)$)", std::regex::icase);
  static const std::regex reIList(R"(^i\((.+)\)$)", std::regex::icase);
  static const std::regex reLevel(R"(^(\d+)$)");
  long long l = 0, d = 0, sTotal = 0;
  std::vector<long long> multi;  // a numlist on one ts atom
  char multiLetter = 0;
  bool anyTs = false;
  for (const auto& g0 : groups) {
    const std::string g = g0;
    const std::string lg = strutil::lower(g);
    std::smatch m;
    if (lg == "c") {
      a->explicitC = true;
      continue;
    }
    if (lg == "i") {
      a->factor = true;
      continue;
    }
    if (lg == "o") {
      a->omittedOp = true;
      continue;
    }
    if (lg == "ibn" || lg == "bn") {
      a->factor = true;
      a->baseRule = 4;
      a->explicitBase = true;
      continue;
    }
    if (lg == "ib" || lg == "b") return envError("invalid varlist", 198);
    if (std::regex_match(g, m, reIb)) {
      a->factor = true;
      a->explicitBase = true;
      a->baseRule = 1;
      a->base = std::stod(m.str(1));
      if (!isLevelNumber(a->base)) return envError(g + ":  operator invalid", 198);
      continue;
    }
    if (std::regex_match(g, m, reIbParen)) {
      a->factor = true;
      a->explicitBase = true;
      const std::string inner = strutil::lower(strutil::trim(m.str(1)));
      if (inner == "first") a->baseRule = 0;
      else if (inner == "last") a->baseRule = 2;
      else if (inner == "freq") a->baseRule = 3;
      else if (!inner.empty() && inner[0] == '#') {  // ib(#k): the k-th level
        auto v = levelList(inner.substr(1));
        if (!v || v->size() != 1 || v->front() < 1) return envError(g + ":  operator invalid", 198);
        a->baseRule = 5;
        a->base = v->front();
      } else {
        auto v = levelList(inner);
        if (!v || v->size() != 1) return envError(g + ":  operator invalid", 198);
        a->baseRule = 1;
        a->base = v->front();
      }
      continue;
    }
    if (std::regex_match(g, m, reIo) || std::regex_match(g, m, reIoNum)) {
      a->factor = true;
      auto v = levelList(m.str(1));
      if (!v) return envError(g + ":  operator invalid", 198);
      a->omitLevels = *v;
      continue;
    }
    if (std::regex_match(g, m, reIList)) {
      a->factor = true;
      auto v = levelList(m.str(1));
      if (!v) return envError(g + ":  operator invalid", 198);
      a->only = *v;
      continue;
    }
    if (std::regex_match(g, m, reLevel)) {  // `2.rep78`: that one level, no base
      a->factor = true;
      a->only = std::vector<double>{std::stod(m.str(1))};
      if (!isLevelNumber(a->only->front())) return envError(g + ":  operator invalid", 198);
      a->baseRule = 4;
      continue;
    }
    // A run of time-series atoms: L F D S, each with digits or a (numlist).
    size_t i = 0;
    while (i < g.size()) {
      const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(g[i])));
      if (c != 'L' && c != 'F' && c != 'D' && c != 'S')
        return envError(g + ":  operator invalid", 198);
      ++i;
      std::vector<long long> vals{1};
      if (i < g.size() && g[i] == '(') {
        const size_t close = g.find(')', i);
        if (close == std::string::npos) return envError(g + ":  operator invalid", 198);
        auto v = numlistValues(g.substr(i + 1, close - i - 1));
        if (!v || v->empty()) return envError(g + ":  operator invalid", 198);
        vals.clear();
        for (double x : *v) {
          if (x != std::floor(x)) return envError(g + ":  operator invalid", 198);
          vals.push_back(static_cast<long long>(x));
        }
        i = close + 1;
      } else if (i < g.size() && std::isdigit(static_cast<unsigned char>(g[i]))) {
        size_t j = i;
        while (j < g.size() && std::isdigit(static_cast<unsigned char>(g[j]))) ++j;
        vals = {std::stoll(g.substr(i, j - i))};
        i = j;
      }
      anyTs = true;
      if (vals.size() > 1) {
        if (!multi.empty())
          return envError(g + "\nnumlist in operator invalid", 198);
        multi = vals;
        multiLetter = c;
        continue;
      }
      switch (c) {
        case 'L': l += vals[0]; break;
        case 'F': l -= vals[0]; break;
        case 'D': d += vals[0]; break;
        default: sTotal += vals[0]; break;
      }
    }
  }
  lags->clear();
  if (!anyTs) {
    lags->push_back(std::string());
    return std::nullopt;
  }
  auto text = [](long long ll, long long dd, long long ss) {
    std::string out;
    auto part = [&](const char* c, long long k) {
      if (k <= 0) return;
      out += c;
      if (k > 1) out += std::to_string(k);
    };
    if (ll > 0) part("L", ll);
    else if (ll < 0) part("F", -ll);
    part("D", dd);
    part("S", ss);
    return out;
  };
  if (multi.empty()) {
    lags->push_back(text(l, d, sTotal));
  } else {
    for (long long v : multi) {
      long long ll = l, dd = d, ss = sTotal;
      switch (multiLetter) {
        case 'L': ll += v; break;
        case 'F': ll -= v; break;
        case 'D': dd += v; break;
        default: ss += v; break;
      }
      lags->push_back(text(ll, dd, ss));
    }
  }
  return std::nullopt;
}

// The text of a factor level as a column name prefix: 2, 1000000, ...
std::string levelText(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.0f", v);
  return buf;
}

// Splits text on depth-0 blanks.
std::vector<std::string> depthZeroWords(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  for (char c : text) {
    if (c == '(') ++depth;
    if (c == ')') --depth;
    if (std::isspace(static_cast<unsigned char>(c)) && depth == 0) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
      continue;
    }
    cur.push_back(c);
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

}  // namespace

ojson runFvexpand(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabFvexpand.known,
                                            kVocabFvexpand.unimplemented))
    return ojson(*err);
  Datastore* ds = session.datastore();
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  bool fvbaseOn = true;
  if (const std::string* set = session.eval.globals.find(std::string(kSetMarkerPrefix) + "fvbase"))
    fvbaseOn = *set != "off";

  // Terms: depth-0 words, each split on `#`, each atom's operators parsed and
  // its variables resolved; a parenthesised variable group multiplies the term.
  bool fvops = false, tsops = false;
  std::vector<std::vector<FxAtom>> terms;  // expanded, in order
  for (const auto& word : depthZeroWords(parsed.argText)) {
    bool factorial = false;
    const auto parts = splitInteraction(word, &factorial);
    if (!parts) return envError("invalid varlist", 198);
    if (parts->size() > 1) fvops = true;
    std::vector<std::vector<FxAtom>> positions;
    for (const auto& part : *parts) {
      if (part.empty()) return envError("invalid varlist", 198);
      // The variable part follows the last depth-0 dot.
      size_t lastDot = std::string::npos;
      int depth = 0;
      for (size_t i = 0; i < part.size(); ++i) {
        if (part[i] == '(') ++depth;
        else if (part[i] == ')') --depth;
        else if (part[i] == '.' && depth == 0) lastDot = i;
      }
      std::vector<std::string> groups;
      if (lastDot != std::string::npos) {
        std::string cur;
        int d = 0;
        for (char c : part.substr(0, lastDot) + ".") {
          if (c == '(') ++d;
          if (c == ')') --d;
          if (c == '.' && d == 0) {
            groups.push_back(cur);
            cur.clear();
            continue;
          }
          cur.push_back(c);
        }
      }
      std::string varspec = lastDot == std::string::npos ? part : part.substr(lastDot + 1);
      if (varspec.size() >= 2 && varspec.front() == '(' && varspec.back() == ')')
        varspec = varspec.substr(1, varspec.size() - 2);
      if (strutil::trim(varspec).empty()) return envError("invalid varlist", 198);
      FxAtom proto;
      std::vector<std::string> lags;
      if (auto e = parseAtomOps(groups, &proto, &lags)) return *e;
      // In an interaction, the levels-free `o.` on a continuous variable reads as c.
      // A variable with no factor or `c.` operator there is a factor: "use of #
      // implies the i. prefix" ([U] 11.4.3.1), so `foreign#rep78` is
      // i.foreign#i.rep78.
      if (parts->size() > 1) {
        if (!proto.factor && !proto.explicitC && !proto.omittedOp) proto.factor = true;
        proto.omittedOp = false;
      }
      if (proto.factor || proto.omittedOp) fvops = true;
      if (!lags.front().empty()) tsops = true;
      auto vr = loops::expandVarlist(varspec, session);
      if (vr.isError) return vr.error;
      if (auto e = loops::checkKnownVarlist(vr.tokens, session)) return *e;
      std::vector<FxAtom> alts;
      for (const auto& v : vr.tokens)
        for (const auto& lag : lags) {
          FxAtom a = proto;
          a.var = v;
          a.ts = lag;
          alts.push_back(a);
        }
      positions.push_back(std::move(alts));
    }
    // The cartesian product of the positions, the first slowest.
    std::vector<std::vector<FxAtom>> combos{{}};
    for (const auto& alts : positions) {
      std::vector<std::vector<FxAtom>> next;
      for (const auto& c : combos)
        for (const auto& a : alts) {
          auto cc = c;
          cc.push_back(a);
          next.push_back(std::move(cc));
        }
      combos = std::move(next);
    }
    for (auto& atomsIn : combos) {
      // One atom per factor variable inside an interaction.
      std::vector<FxAtom> atoms;
      for (auto& a : atomsIn) {
        bool dup = false;
        if (a.factor)
          for (const auto& b : atoms) dup = dup || (b.factor && b.var == a.var && b.ts == a.ts);
        if (!dup) atoms.push_back(a);
      }
      if (!factorial || atoms.size() < 2) {
        terms.push_back(atoms);
        continue;
      }
      std::set<std::vector<std::string>> seen;
      std::vector<std::vector<FxAtom>> subs;
      for (const auto& a : atoms) {
        const size_t n0 = subs.size();
        subs.push_back({a});
        for (size_t i = 0; i < n0; ++i) {
          auto c = subs[i];
          c.push_back(a);
          subs.push_back(std::move(c));
        }
      }
      for (auto& sub : subs) {
        std::vector<std::string> id;
        for (const auto& a : sub) id.push_back(a.key());
        if (seen.insert(id).second) terms.push_back(sub);
      }
    }
  }

  // A variable's base is set once for the whole varlist ([U] 11.4.3.2): an atom
  // without its own ib*/ibn takes the one written elsewhere (`ib3.rep78
  // i.rep78#i.foreign` uses base 3 in the interaction); a second ib* must name
  // the same base level ("You will get an error if you attempt to change base
  // levels in midsentence"), checked once the levels are known below.
  {
    std::map<std::string, const FxAtom*> chosen;
    for (const auto& t : terms)
      for (const auto& a : t) {
        if (!a.factor || !a.explicitBase) continue;
        if (chosen.find(a.ts + "." + a.var) == chosen.end()) chosen[a.ts + "." + a.var] = &a;
      }
    std::map<std::string, FxAtom> rules;
    for (const auto& kv : chosen) rules[kv.first] = *kv.second;
    for (auto& t : terms)
      for (auto& a : t) {
        if (!a.factor || a.explicitBase) continue;
        auto it = rules.find(a.ts + "." + a.var);
        if (it == rules.end()) continue;
        a.baseRule = it->second.baseRule;
        a.base = it->second.base;
      }
  }

  // The sample: if/in.
  ScratchColumns scratch(session);
  std::string qualifier;
  if (parsed.ifExpr.has_value() && !strutil::trim(*parsed.ifExpr).empty())
    qualifier += " if " + *parsed.ifExpr;
  if (parsed.inRange.has_value() && !strutil::trim(*parsed.inRange).empty())
    qualifier += " in " + *parsed.inRange;
  const size_t rows = static_cast<size_t>(session.datastore()->nRows());
  std::vector<char> inSample(rows, 1);
  if (!qualifier.empty()) {
    std::string mark;
    if (auto e = scratch.add("1", qualifier, &mark)) return *e;
    const auto m = scratch.values(mark);
    for (size_t r = 0; r < rows; ++r) inSample[r] = std::isnan(m[r]) ? 0 : 1;
  }

  // Each atom's values (through generate when it carries a ts operator), the
  // factor checks and the levels.
  std::map<std::string, std::vector<double>> valueCache;
  auto valuesOf = [&](const FxAtom& a, std::vector<double>* out) -> std::optional<ojson> {
    const std::string key = a.ts + "." + a.var;
    auto it = valueCache.find(key);
    if (it != valueCache.end()) {
      *out = it->second;
      return std::nullopt;
    }
    Datastore* d = session.datastore();
    if (d->dtype(a.var) == "factor") {
      if (a.factor)
        return envError(a.var + ":  string variables may not be used as factor variables", 109);
      if (!a.ts.empty())
        return envError(a.var + ":  may not use time-series operators on string variables", 109);
      return envError("string variables not allowed in varlist;\n" + a.var +
                          " is a string variable",
                      109);
    }
    std::vector<double> v;
    if (a.ts.empty()) {
      const double* cells = d->col(a.var).f64();
      v.assign(cells, cells + d->nRows());
    } else {
      if (!session.timevar.has_value() || session.timevar->empty())
        return envError("time variable not set", 111);
      std::string col;
      if (auto e = scratch.add(a.ts + "." + a.var, "", &col)) return *e;
      v = scratch.values(col);
    }
    for (size_t r = 0; r < v.size(); ++r)
      if (!inSample[r]) v[r] = std::numeric_limits<double>::quiet_NaN();
    valueCache[key] = v;
    *out = std::move(v);
    return std::nullopt;
  };
  for (auto& t : terms)
    for (auto& a : t) {
      // A plain variable alone is its own name; nothing about its values matters.
      if (!a.factor && a.ts.empty() && t.size() == 1) continue;
      if (auto e = valuesOf(a, &a.values)) return *e;
      if (!a.factor) continue;
      std::map<double, long long> counts;
      for (double x : a.values) {
        if (std::isnan(x)) continue;
        if (x != std::floor(x))
          return envError(a.var + ":  factor variables may not contain noninteger values", 452);
        if (x < 0)
          return envError(a.var + ":  factor variables may not contain negative values", 452);
        ++counts[x];
      }
      for (const auto& kv : counts)
        if (!a.only || std::find(a.only->begin(), a.only->end(), kv.first) != a.only->end())
          a.levels.push_back(kv.first);
      if (a.levels.empty()) continue;
      if (a.baseRule == 5 && a.base > static_cast<double>(a.levels.size()))
        return envError(a.var + ":  base level #" + levelText(a.base) + " not found", 198);
      switch (a.baseRule) {
        case 1: a.resolvedBase = a.base; break;
        case 5: a.resolvedBase = a.levels[static_cast<size_t>(a.base) - 1]; break;
        case 2: a.resolvedBase = a.levels.back(); break;
        case 3: {
          long long best = -1;
          for (double lv : a.levels)
            if (counts[lv] > best) best = counts[lv], a.resolvedBase = lv;
          break;
        }
        default: a.resolvedBase = a.levels.front(); break;
      }
      if (a.baseRule == 1 && !std::binary_search(a.levels.begin(), a.levels.end(), a.resolvedBase)) {
        a.levels.push_back(a.resolvedBase);
        std::sort(a.levels.begin(), a.levels.end());
      }
    }

  // Two ib* on one variable naming different base levels (`ib2.rep78 ib3.rep78`).
  {
    std::map<std::string, std::pair<bool, double>> baseOf;
    for (const auto& t : terms)
      for (const auto& a : t) {
        if (!a.factor || !a.explicitBase || a.levels.empty()) continue;
        const std::pair<bool, double> b{a.hasBase(), a.hasBase() ? a.resolvedBase : 0.0};
        auto it = baseOf.find(a.ts + "." + a.var);
        if (it == baseOf.end()) baseOf[a.ts + "." + a.var] = b;
        else if (it->second != b)
          return envError(a.var + ":  base level specified more than once", 198);
      }
  }

  // Every term's identity, for the "rest of the cell is in the varlist" rule.
  auto keyOf = [](std::vector<std::string> ids) {
    std::sort(ids.begin(), ids.end());
    std::string k;
    for (const auto& x : ids) k += x + "#";
    return k;
  };
  std::set<std::string> modelKeys;
  for (const auto& t : terms) {
    std::vector<std::string> ids;
    for (const auto& a : t) ids.push_back(a.key());
    modelKeys.insert(keyOf(ids));
  }

  std::vector<std::string> names;
  std::set<std::string> factorTermsSeen;
  for (const auto& t : terms) {
    std::vector<const FxAtom*> fac, con;
    for (const auto& a : t) (a.factor ? fac : con).push_back(&a);
    if (fac.empty() && con.size() == 1) {  // a continuous variable alone
      const FxAtom& a = *con.front();
      std::string nm = a.ts.empty() ? a.var : a.ts + "." + a.var;
      names.push_back(a.omittedOp ? "o." + nm : nm);
      continue;
    }
    {
      std::vector<std::string> ids;
      for (const auto& a : t) ids.push_back(a.key());
      const std::string k = keyOf(ids);
      if (!fac.empty() && !factorTermsSeen.insert(k).second) continue;  // a repeated factor term
    }
    // The cells: the factor atoms' levels, the first slowest.
    // The factor-level combinations observed where every atom of the term is
    // non-missing: one pass over the rows, so a cell's emptiness is a lookup.
    std::set<std::vector<double>> observed;
    if (fac.size() + con.size() > 1) {
      for (size_t r = 0; r < rows; ++r) {
        std::vector<double> tuple;
        bool ok = true;
        for (size_t k = 0; k < fac.size() && ok; ++k) {
          ok = !std::isnan(fac[k]->values[r]);
          tuple.push_back(fac[k]->values[r]);
        }
        for (const FxAtom* a : con) ok = ok && !std::isnan(a->values[r]);
        if (ok) observed.insert(std::move(tuple));
      }
    }
    std::vector<size_t> pos(fac.size(), 0);
    bool anyLevels = true;
    for (const FxAtom* a : fac) anyLevels = anyLevels && !a->levels.empty();
    if (!anyLevels) continue;
    for (bool more = true; more;) {
      bool anyBase = false;
      std::vector<std::string> restIds;
      std::vector<bool> atBase(fac.size(), false);
      for (size_t k = 0; k < fac.size(); ++k) {
        const double lv = fac[k]->levels[pos[k]];
        atBase[k] = fac[k]->hasBase() && lv == fac[k]->resolvedBase;
        if (atBase[k]) anyBase = true;
        else restIds.push_back(fac[k]->key());
      }
      for (const FxAtom* a : con) restIds.push_back(a->key());
      const bool isBase = anyBase && restIds.empty();
      bool omitted = false;
      if (!isBase && fac.size() + con.size() > 1) {
        if (anyBase && modelKeys.count(keyOf(restIds)) != 0) omitted = true;
        if (!omitted) {
          // An empty cell: no observation with these levels where every atom of
          // the term is non-missing.
          std::vector<double> cell;
          for (size_t k = 0; k < fac.size(); ++k) cell.push_back(fac[k]->levels[pos[k]]);
          omitted = observed.count(cell) == 0;
        }
      }
      std::string name;
      for (size_t k = 0; k < fac.size(); ++k) {
        const FxAtom& a = *fac[k];
        const double lv = a.levels[pos[k]];
        std::string mark;
        if (fvbaseOn) {
          if (atBase[k]) mark = "b";
          else if (!a.hasBase() && pos[k] == 0 && a.baseRule == 4 && !a.only) mark = "bn";
        }
        const bool ioOmitted =
            std::find(a.omitLevels.begin(), a.omitLevels.end(), lv) != a.omitLevels.end();
        if ((omitted && !atBase[k]) || ioOmitted) mark += "o";
        name += (name.empty() ? "" : "#") + levelText(lv) + mark + a.ts + "." + a.var;
      }
      for (const FxAtom* a : con) {
        name += (name.empty() ? "" : "#") + std::string("c") + (omitted ? "o" : "") + a->ts + "." +
                a->var;
      }
      names.push_back(name);
      more = false;
      for (size_t k = fac.size(); k-- > 0;) {
        if (++pos[k] < fac[k]->levels.size()) {
          more = true;
          break;
        }
        pos[k] = 0;
      }
      if (fac.empty()) more = false;
    }
  }
  scratch.dropAll();

  std::string joined;
  for (size_t i = 0; i < names.size(); ++i) joined += (i ? " " : "") + names[i];
  eval::replaceRClass(session.eval);
  std::vector<std::string> posted;
  if (!joined.empty()) {
    session.eval.rStrings.set("varlist", joined);
    posted.push_back("varlist");
  }
  if (fvops) {
    session.eval.rStrings.set("fvops", "true");
    posted.push_back("fvops");
  }
  if (tsops) {
    session.eval.rStrings.set("tsops", "true");
    posted.push_back("tsops");
  }
  eval::declareRPosted(session.eval, "fvexpand", posted);
  return ojson::array();
}

}  // namespace piet::handlers
