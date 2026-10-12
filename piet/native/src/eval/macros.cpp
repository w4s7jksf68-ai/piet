// C++ port of piet/backend/piet/router/macros.py + postfile.eval_macro_expr
// + matrix_op.substitute_matrix_refs + the display_hardening est-scalar
// resolvers _eval_arith depends on. The Python sources are the spec — every
// branch below mirrors a specific Python code path, bug-for-bug (including
// single-pass RHS expansion in definitions and first-occurrence string
// replacement during substitution).
#include "piet/eval/macros.h"

#include "../dispatch_profile.h"  // WS-D1: per-line regex counter (opt-in, stderr)

#ifndef _WIN32
#include <fnmatch.h>  // WP-W5: POSIX-only; the Windows branch uses globMatchPeriod below
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <regex>
#include <vector>

#include "piet/eval/exprfns.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/pyvalue.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::to_native_path (BUG-050 wide open)

namespace piet::eval {

namespace {

// ---------------------------------------------------------------------------
// Small Python-string helpers

bool isPySpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string strip(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isPySpace(s[a])) ++a;
  while (b > a && isPySpace(s[b - 1])) --b;
  return s.substr(a, b - a);
}

// ---------------------------------------------------------------------------
// D1-①: the `local NAME rest` / `global NAME rest` recogniser, as a single
// left-to-right pass instead of a std::regex match.
//
// WHY. WS-D1 profiled dispatch()'s ~3.3 us per-line fixed cost (two prior
// sessions guessed at it, implemented the guess and measured zero). The
// profile named this function: `tryCaptureDefinition` was 67.4% of the whole
// per-line budget by the zone timers and 59.6% by the sampling profiler, and
// 94.4% of ITS subtree was one symbol — std::basic_regex::__search. Two regex
// matches ran on every line that reached step 0a, and `local q 5` — the
// cheapest statement the language has — paid both.
//
// EQUIVALENCE, not approximation. These two functions reproduce
//     reLocalDef()  = ^local\s+([A-Za-z_][A-Za-z0-9_]*)\s*(.*)?$   (icase)
//     reGlobalDef() = ^global\s+([A-Za-z_][A-Za-z0-9_]*)\s*(.*)?$  (icase)
// exactly, including the greedy-then-no-backtracking parse (the trailing
// `(.*)$` can always match, so the greedy first pass is the only pass the
// regex engine ever needs). Three details are load-bearing:
//   * `\s` in ECMAScript is [ \t\n\v\f\r] — isRegexSpace below, NOT isPySpace
//     (which is the same set here, but the two classes are separate laws and
//     are kept separate on purpose).
//   * `.` does NOT match a newline, so the trailing `(.*)$` cannot span one:
//     `local x a\nb` does NOT match the regex, while `local\nx 3` DOES (there
//     the newline is inside the `\s+` run, and \s includes \n). Reproduced
//     exactly below — the scan is newline-agnostic, and the tail is then
//     rejected if it carries one. Only `#delimit ;` can deliver such a line
//     here, and it is not the hot path either way.
//   * the keyword compare is case-insensitive (`LOCAL x 3` is legal Stata).
// selftest.cpp cross-checks this function against the regexes over a corpus of
// forms, so the two cannot drift (the same discipline the plan requires of a
// generated perfect-hash table).
bool isRegexSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
bool isIdentStart(char c) {
  unsigned char u = static_cast<unsigned char>(c);
  return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || c == '_';
}
bool isIdentChar(char c) {
  unsigned char u = static_cast<unsigned char>(c);
  return isIdentStart(c) || (u >= '0' && u <= '9');
}
bool ciStartsWith(const std::string& s, const char* kw, size_t kwLen) {
  if (s.size() < kwLen) return false;
  for (size_t i = 0; i < kwLen; ++i) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != kw[i]) return false;
  }
  return true;
}

// Pre-filter for reMdyn = ^((?:local|global)\s+)(\S+)(.*)$ : its body only
// does anything when the NAME TOKEN (group 2, the maximal non-space run after
// the keyword) carries a '`' or a '$'. This answers that question with one
// scan of that token. Deliberately conservative — when it says yes the regex
// still runs and still decides; it can only skip a match whose body would have
// been a no-op.
bool macroDefNameTokenHasMacroChar(const std::string& s) {
  size_t kwLen;
  if (ciStartsWith(s, "local", 5)) {
    kwLen = 5;
  } else if (ciStartsWith(s, "global", 6)) {
    kwLen = 6;
  } else {
    return false;
  }
  size_t i = kwLen;
  if (i >= s.size() || !isRegexSpace(s[i])) return false;  // the \s+ is mandatory
  while (i < s.size() && isRegexSpace(s[i])) ++i;
  size_t j = i;
  while (j < s.size() && !isRegexSpace(s[j])) ++j;
  if (j == i) return false;  // \S+ needs at least one character
  for (size_t k = i; k < j; ++k) {
    if (s[k] == '`' || s[k] == '$') return true;
  }
  return false;
}

// Returns true iff `s` is `<kw> NAME [rest]`; fills name/rest with what the
// equivalent regex's groups 1 and 2 would hold.
bool matchMacroDef(const std::string& s, const char* kw, size_t kwLen, std::string& name,
                   std::string& rest) {
  if (!ciStartsWith(s, kw, kwLen)) return false;
  size_t i = kwLen;
  if (i >= s.size() || !isRegexSpace(s[i])) return false;  // \s+ needs at least one
  while (i < s.size() && isRegexSpace(s[i])) ++i;
  if (i >= s.size() || !isIdentStart(s[i])) return false;
  size_t j = i;
  while (j < s.size() && isIdentChar(s[j])) ++j;
  name.assign(s, i, j - i);
  size_t k = j;
  while (k < s.size() && isRegexSpace(s[k])) ++k;
  // `(.*)$` cannot cross a newline — see the note above.
  if (s.find('\n', k) != std::string::npos) return false;
  rest.assign(s, k, s.size() - k);
  return true;
}

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `toLower(...)` call sites
// didn't need touching. Pure dedup, no behavior change. (Note: strip()
// above is a distinct, wider-whitespace helper and is left untouched.)
std::string toLower(std::string s) { return strutil::lower(std::move(s)); }

std::vector<std::string> pySplit(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (isPySpace(c)) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// Python str.replace(a, b, 1) — replace FIRST occurrence anywhere.
std::string replaceFirst(const std::string& s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  size_t pos = s.find(from);
  if (pos == std::string::npos) return s;
  return s.substr(0, pos) + to + s.substr(pos + from.size());
}

std::string regexEscape(const std::string& s) {
  static const std::string meta = R"(\^$.|?*+()[]{})";
  std::string out;
  for (char c : s) {
    if (meta.find(c) != std::string::npos) out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

// re.sub with a callback (replaces ALL matches).
std::string regexSubFn(const std::regex& re, const std::string& input,
                        const std::function<std::string(const std::smatch&)>& fn) {
  std::string out;
  auto begin = std::sregex_iterator(input.begin(), input.end(), re);
  auto end = std::sregex_iterator();
  size_t last = 0;
  for (auto it = begin; it != end; ++it) {
    const std::smatch& m = *it;
    out.append(input, last, static_cast<size_t>(m.position(0)) - last);
    out.append(fn(m));
    last = static_cast<size_t>(m.position(0)) + static_cast<size_t>(m.length(0));
  }
  out.append(input, last, input.size() - last);
  return out;
}

// Snapshot of finditer results: (full match text, group1 text).
std::vector<std::pair<std::string, std::string>> findAll(const std::regex& re,
                                                          const std::string& s) {
  std::vector<std::pair<std::string, std::string>> out;
  auto begin = std::sregex_iterator(s.begin(), s.end(), re);
  for (auto it = begin; it != std::sregex_iterator(); ++it) {
    out.emplace_back(it->str(0), it->size() > 1 ? it->str(1) : std::string());
  }
  return out;
}

// Python repr(str) — used in the non-convergence error message ({line!r}).
std::string pyRepr(const std::string& s) {
  bool hasSingle = s.find('\'') != std::string::npos;
  bool hasDouble = s.find('"') != std::string::npos;
  char quote = (hasSingle && !hasDouble) ? '"' : '\'';
  std::string out(1, quote);
  for (unsigned char c : s) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == static_cast<unsigned char>(quote)) {
      out += "\\";
      out += static_cast<char>(c);
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c < 0x20 || c == 0x7f) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02x", c);
      out += buf;
    } else {
      // ASCII printable and UTF-8 continuation/lead bytes: Python 3 repr
      // keeps printable non-ASCII literal (corpus avoids non-printable
      // unicode).
      out += static_cast<char>(c);
    }
  }
  out += quote;
  return out;
}

// ---------------------------------------------------------------------------
// Patterns (macros.py) — names mirror the Python constants.

const std::regex& reScalarDef() {
  static const std::regex r(R"(^scalar\s+(?:define\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$)",
                             std::regex::icase);
  return r;
}
const std::regex& reLocalDef() {
  static const std::regex r(R"(^local\s+([A-Za-z_][A-Za-z0-9_]*)\s*(.*)?$)", std::regex::icase);
  return r;
}
const std::regex& reGlobalDef() {
  static const std::regex r(R"(^global\s+([A-Za-z_][A-Za-z0-9_]*)\s*(.*)?$)", std::regex::icase);
  return r;
}
const std::regex& reLocalRef() {
  static const std::regex r(R"(`([A-Za-z_][A-Za-z0-9_]*|[0-9]+)')");
  return r;
}
const std::regex& reGlobalRefBrace() {
  static const std::regex r(R"(\$\{([A-Za-z_][A-Za-z0-9_]*)\})");
  return r;
}
const std::regex& reGlobalRefBare() {
  static const std::regex r(R"(\$([A-Za-z_][A-Za-z0-9_]*))");
  return r;
}
const std::regex& reRRef() {
  static const std::regex r(R"(\br\(([A-Za-z_][A-Za-z0-9_]*)\))");
  return r;
}
const std::regex& reRRefDelim() {
  static const std::regex r(R"(`r\(([A-Za-z_][A-Za-z0-9_]*)\)')");
  return r;
}
const std::regex& reERef() {
  // RISK-A: negative-lookahead keeps e(b)/e(V) for the matrix layer.
  static const std::regex r(R"(\be\((?!b\)|V\))([A-Za-z_][A-Za-z0-9_]*)\))");
  return r;
}
// BUG-409: `e(name)' — the DELIMITED (macro-expansion) form, the twin of
// reRRefDelim. Stata's e() holds macros as well as scalars and `estadd local
// controls "Yes"` is the standard way a paper puts a "Controls: Yes" row into a
// table; without this arm the backtick survived expansion and tripped the
// generic "does not expand macros" tripwire, so the ONE spelling every estout
// example uses was a hard error. Unlike reERef this has no b/V exclusion: a
// delimited reference is never the matrix layer's.
const std::regex& reERefDelim() {
  static const std::regex r(R"(`e\(([A-Za-z_][A-Za-z0-9_]*)\)')");
  return r;
}
// P4: `s(name)' — sreturn-local read (sclass programs). Delimited form
// ONLY: Stata's s() is a string-only namespace here, and the P4 task scope
// is the minimal subset the answerbook needs — no bare `s(name)` form, no
// numeric variant (unlike r(), which has both — see reRRef/reRRefDelim
// just below what this sits next to).
const std::regex& reSRefDelim() {
  static const std::regex r(R"(`s\(([A-Za-z_][A-Za-z0-9_]*)\)')");
  return r;
}
// MUST stay byte-identical to program.cpp's kSClassKeyPrefix — see that
// file's top-of-file note for the full rationale (no shared header exists
// for this constant; sreturn-local values live in session.rStrings, the
// SAME store `return local`/r() uses, under this reserved key prefix
// rather than a new session store).
const char* const kSClassKeyPrefix = "\x01s:";
const std::regex& reSafeArith() {
  static const std::regex r(R"(^[0-9\s\+\-\*/\(\)\.\^eE]+$)");
  return r;
}
// BUG-206: `%` is in the EXTENDED alphabet, and only the extended one.
//
// Every Stata display format begins with `%`, so while this class refused the
// character the entire two-argument `string(value, "fmt")` form was dead — and
// with it every OTHER expression whose text happens to contain a percent sign:
// measured pre-fix, `strpos("50%","%")`, `subinstr("a%b","%","pct",1)`,
// `upper("x%y")` and `length("%9.0g")` were all refused before evaluation and
// echoed as their own literal source text. The defect was never about `string()`;
// `string()` is just where it was noticed.
//
// BUG-206's entry warned that admitting `%` would also admit Python's modulo
// operator, and that the one-line widening therefore needed a probe rather than
// an assertion. The probe says the hazard does not exist in THIS engine:
// pyeval.cpp's token set (`enum class Tok`) has no modulo token at all, and its
// lexer's `switch` ends in `default: fail("unexpected character ...")`. So a `%`
// outside a string literal reaches the evaluator, raises SyntaxError, and
// evalArith returns the SAME nullopt the whitelist used to return — identical
// observable behaviour, not new semantics. Corroborated from two more
// directions: a sweep of `tests/**/*.do` + `waves/**/*.do` finds zero uses of
// `%` as an operator (two hits, both inside comment text — "95% CI"), and real
// Stata answers `1%2` and `5 % 3` with r(198), so there is no Stata modulo
// semantics to preserve in the first place.
//
// Deliberately NOT added to reSafeArith()/basic. That is evalArith's FAST path
// (25.6% of S02's profile) and nothing needs `%` there: a `%` expression is by
// construction a formatting or string expression, which needs the extended
// namespace anyway. Keeping the basic alphabet byte-identical means the hot path
// cannot move at all — no added test, no added branch, no reordering.
const std::regex& reSafeArithExt() {
  // BUG-660 fix round: `&`/`|` added — Stata's logical AND/OR (`display
  // . & 1`, `local n = . & 1`) reach evalPyExprDetailed via pyeval.cpp's own
  // new Tok::Amp/Tok::Pipe now, and both must be admitted here or the
  // whitelist rejects the expression before the parser ever sees it. Must
  // stay in lockstep with SafeArithTables::ext below (selftestSafeArithScan-
  // MatchesRegex cross-checks the two).
  static const std::regex r(R"(^[0-9\s\+\-\*/\(\)\.\^eEa-zA-Z_,"'<>=!%&\|]+$)");
  return r;
}
const std::regex& rePostfileSafeArith() {
  static const std::regex r(R"(^[0-9\s\+\-\*/\(\)\.\^eEa-zA-Z_,]+$)");
  return r;
}
const std::regex& reArithMacro() {
  static const std::regex r(R"(`=([^']+)')");
  return r;
}

// ---------------------------------------------------------------------------
// EXPRLAYER-2: the two whitelist gates as character scans.
//
// WHY. reSafeArith() and reSafeArithExt() are `^[<set>]+$` — a pure character
// class, no alternation, no backtracking, no groups. std::regex_match still
// builds per-execution state for them, and the EXPRLAYER zone profile measured
// 81.8 ms of S01 and 18.7 ms of S02 inside these two calls alone (70,582 and
// 6,008 executions). A 256-entry table lookup answers the same question.
//
// EQUIVALENCE. `regex_match` requires the WHOLE string to match and `+`
// requires at least one character, so an empty string does NOT match — both
// reproduced below. ECMAScript `\s` is [ \t\n\v\f\r] (isRegexSpace above, the
// same class this file already transcribed for matchMacroDef), and `$` without
// the multiline flag anchors at end-of-string, so a trailing newline is simply
// another `\s` member. selftestSafeArithScanMatchesRegex() cross-checks both
// scans against both regexes over a corpus, so they cannot drift.
struct SafeArithTables {
  bool basic[256] = {false};
  bool ext[256] = {false};
  SafeArithTables() {
    // ^[0-9\s\+\-\*/\(\)\.\^eE]+$
    for (char c : std::string("0123456789 \t\n\v\f\r+-*/().^eE")) {
      basic[static_cast<unsigned char>(c)] = true;
    }
    // ^[0-9\s\+\-\*/\(\)\.\^eEa-zA-Z_,"'<>=!%]+$   (eE are inside a-zA-Z)
    // `%` is BUG-206 — see reSafeArithExt() above for the measurement. It must
    // be added to BOTH this table and that regex or selftestSafeArithScanMatches-
    // Regex reports the drift, which is the whole point of the twin.
    for (int i = 0; i < 256; ++i) ext[i] = basic[i];
    for (char c = 'a'; c <= 'z'; ++c) ext[static_cast<unsigned char>(c)] = true;
    for (char c = 'A'; c <= 'Z'; ++c) ext[static_cast<unsigned char>(c)] = true;
    // BUG-660 fix round: `&`/`|` — see reSafeArithExt()'s comment above.
    for (char c : std::string("_,\"'<>=!%&|")) ext[static_cast<unsigned char>(c)] = true;
  }
};
const SafeArithTables& safeArithTables() {
  static const SafeArithTables t;
  return t;
}
// BUG-501. Everything inside a BALANCED string literal is DATA, and every
// pre-pass in evalArith rewrites OPERATORS and REFERENCES -- so each one was
// mangling literal content, silently. Measured against
// tests/eval/golden/aer_w1/w1b_quoteblind.log and w1b_braceliteral.log:
//
//   strpos("a^b", "b")          the `^` became `**`
//   strpos("A[1,1]", "1")       substituteMatrixRefs substituted the subscript
//   strpos("sc", "c")           resolveEstScalarRefs substituted the scalar
//   strpos("r(mean)", "mean")   the r() resolver RAISED
//   strpos("a{b}c", "b")        isSafeArithExt rejected the whole expression,
//                               so `display` echoed the source text at rc 0
//
// Protected ONCE here rather than by teaching four pre-passes (and their four
// selftest surfaces) about quotes. Each literal is swapped for a placeholder
// built only from characters the whitelists already allow and that NO resolver
// can match: `0L<k>L0` cannot be a Stata name, which must start with a letter or
// an underscore. So the filter keeps its exact character set -- and its regex
// twin keeps holding -- because it never sees literal content at all. Restored
// immediately before the evaluator, whose own lexer handles string literals
// correctly (pyeval.cpp says so at its date-literal case).
//
// ONE-SIDED, like the display scanner's BUG-293(c) fix: an UNTERMINATED quote is
// left exactly where it is, so every degenerate expression this function ever
// judged is judged the same way.
std::string protectStringLiterals(const std::string& s, std::vector<std::string>* out) {
  out->clear();
  if (s.find('"') == std::string::npos) return s;  // the common case, untouched
  std::string r;
  r.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '"') {
      const size_t close = s.find('"', i + 1);
      if (close != std::string::npos) {
        out->push_back(s.substr(i, close - i + 1));  // the quotes travel with it
        r += "\"0L" + std::to_string(out->size() - 1) + "L0\"";
        i = close + 1;
        continue;
      }
    }
    r += s[i++];
  }
  return r;
}

// BUG-507, the convergence guard's half. `hasMacroRefs` reports an r()/e()/s()
// reference through its `X(` patterns, and the splices above now correctly LEAVE
// one that sits inside a string literal -- so the guard would read that literal as
// unexpanded work and report a circular macro definition for
// `display strpos("r(mean)", "mean")`.
//
// Masking the whole literal is wrong here: Stata DOES expand `` `x' `` and `$x`
// inside double quotes, so an unexpanded MACRO reference in a literal is still a
// real failure and has to stay visible. Only the PARENTHESES are blanked, which
// is exactly the discrimination needed -- every class-reference pattern needs one
// and no macro pattern does (`${...}` uses braces).
std::string maskParensInQuotes(const std::string& s) {
  if (s.find('"') == std::string::npos) return s;
  std::string out = s;
  bool inQ = false;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] == '"') {
      inQ = !inQ;
      continue;
    }
    if (inQ && (out[i] == '(' || out[i] == ')')) out[i] = '_';
  }
  return out;
}

std::string restoreStringLiterals(const std::string& s, const std::vector<std::string>& spans) {
  if (spans.empty()) return s;
  std::string r;
  r.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '"' && i + 4 < s.size() && s[i + 1] == '0' && s[i + 2] == 'L') {
      size_t j = i + 3;
      std::string digits;
      while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) digits += s[j++];
      if (!digits.empty() && j + 2 < s.size() && s[j] == 'L' && s[j + 1] == '0' &&
          s[j + 2] == '"') {
        size_t k = std::string::npos;
        try {
          k = static_cast<size_t>(std::stoul(digits));
        } catch (const std::exception&) {
        }
        if (k < spans.size()) {
          r += spans[k];
          i = j + 3;
          continue;
        }
      }
    }
    r += s[i++];
  }
  return r;
}

bool isSafeArith(const std::string& s) {
  if (s.empty()) return false;
  const bool* tbl = safeArithTables().basic;
  for (unsigned char c : s) {
    if (!tbl[c]) return false;
  }
  return true;
}
bool isSafeArithExt(const std::string& s) {
  if (s.empty()) return false;
  const bool* tbl = safeArithTables().ext;
  for (unsigned char c : s) {
    if (!tbl[c]) return false;
  }
  return true;
}

// EXPRLAYER-3: a strict pre-filter for reRRef() = `\br\(...`.
//
// WHY. expandPass guarded its whole r() block with `line.find("r(")`, and the
// literal "r(" occurs inside `scalar(`, `subinstr(`, `strvar(` — any identifier
// ending in r. S02's hot line is
//   scalar bp_s = scalar(bp_s) + sqrt(`i') + log(`i'+1) - exp(-`i'/1000)
// which contains no r() reference at all and paid TWO full sregex_iterator
// scans per evaluation to discover that: 44.1 ms of expandPass's own 44.9 ms in
// the S02 zone profile — 98% of the function. This asks the question the regex
// actually asks: is there an `r` that is (a) at a \b, i.e. at the start or
// preceded by a non-[A-Za-z0-9_] character, and (b) followed by '('.
// Also used for reERef() = `\be\((?!b\)|V\))...` — same shape, and "e(" is even
// more common as a suffix ("the(", "true(", "size(").
bool hasBareLetterParen(const std::string& s, char letter) {
  const char pat[3] = {letter, '(', '\0'};
  for (size_t i = s.find(pat); i != std::string::npos; i = s.find(pat, i + 1)) {
    if (i == 0) return true;
    const unsigned char p = static_cast<unsigned char>(s[i - 1]);
    const bool wordChar = (p >= 'A' && p <= 'Z') || (p >= 'a' && p <= 'z') ||
                          (p >= '0' && p <= '9') || p == '_';
    if (!wordChar) return true;
  }
  return false;
}
// EXPRLAYER-7: `` `NAME' `` detection as a scan, for hasMacroRefs.
//
// WHY. reLocalRef() = `` `([A-Za-z_][A-Za-z0-9_]*|[0-9]+)' `` is the ONE regex
// every ordinary macro line still executed, and hasMacroRefs is called about
// three times per such line (tryCaptureDefinition once, expandMacros once per
// pass). S01's form 1 — 30,000 iterations of `local acc = `acc' + `i'` — paid
// all of them. The grammar is the same one expandPass's splicer already walks
// character by character a few dozen lines below; this is that walk, asking
// only whether a reference exists.
//
// EQUIVALENCE: regex_search finds the leftmost match, retrying one character
// later on failure. So does this. selftestLocalRefScanMatchesRegex holds it to
// reLocalRef() over the corpus.
bool hasLocalMacroRef(const std::string& s) {
  for (size_t t = s.find('`'); t != std::string::npos; t = s.find('`', t + 1)) {
    size_t j = t + 1;
    if (j < s.size() && isIdentStart(s[j])) {
      ++j;
      while (j < s.size() && isIdentChar(s[j])) ++j;
    } else if (j < s.size() && s[j] >= '0' && s[j] <= '9') {
      ++j;
      while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
    } else {
      continue;  // nothing after the backtick can start either alternative
    }
    if (j < s.size() && s[j] == '\'') return true;
  }
  return false;
}

// hasBareRParen / hasBareEParen: defined at namespace scope just below the
// anonymous namespace (BUG-456 made hasBareEParen the dispatcher's row-predicate
// trigger too; declared in eval/macros.h). hasBareLetterParen stays file-local.

// ---------------------------------------------------------------------------
// EXPRLAYER-5: the three remaining hot regexes on the `scalar NAME = expr`
// path, as left-to-right scans. S02's whole body is 3,000 evaluations of
//   scalar bp_s = scalar(bp_s) + sqrt(`i') + log(`i'+1) - exp(-`i'/1000)
// and the zone profile put 6.5 us/line inside resolveEstScalarRefs' scalar()
// substitution and ~9 us/line inside tryCaptureDefinition's two unguarded
// regex_match calls — the two case-insensitive `scalar ...` recognisers.
//
// Each function below is cross-checked against the regex it replaces, case by
// case, by selftestScalarScansMatchRegex() — the same discipline
// selftestMacroDefScanMatchesRegex() already applies to matchMacroDef.

// `\bscalar\(\s*([A-Za-z_]\w*)\s*\)` -> the stored scalar's value, or the
// matched text unchanged when the name is not a defined scalar.
std::string substituteScalarCalls(const std::string& in, const EvalSession& session) {
  std::string out;
  size_t i = 0, last = 0;
  bool any = false;
  while (true) {
    const size_t p = in.find("scalar(", i);
    if (p == std::string::npos) break;
    // The pattern's leading \b: 's' is a word char, so the preceding character
    // must be a non-word one (or the match must start the string). On failure
    // the regex engine retries one character later — so does this scan.
    if (p > 0 && isIdentChar(in[p - 1])) {
      i = p + 1;
      continue;
    }
    size_t q = p + 7;  // one past the '('
    while (q < in.size() && isRegexSpace(in[q])) ++q;
    const size_t nameStart = q;
    if (q < in.size() && isIdentStart(in[q])) {
      ++q;
      while (q < in.size() && isIdentChar(in[q])) ++q;
    }
    if (q == nameStart) {
      i = p + 1;
      continue;
    }
    size_t r = q;
    while (r < in.size() && isRegexSpace(in[r])) ++r;
    if (r >= in.size() || in[r] != ')') {
      i = p + 1;
      continue;
    }
    const size_t end = r + 1;
    const std::string* v = session.scalars.find(in.substr(nameStart, q - nameStart));
    out.append(in, last, p - last);
    if (v != nullptr) {
      out.append(*v);
    } else {
      out.append(in, p, end - p);  // undefined: leave the token verbatim
    }
    last = end;
    i = end;
    any = true;
  }
  if (!any) return in;
  out.append(in, last, in.size() - last);
  return out;
}


// Shared tail of `... \s*(.+)$` with no multiline flag: `.` never matches a
// newline and `$` anchors at end-of-string, so the greedy `\s*` gives back at
// most one character to satisfy `.+`. `wsFrom` is where the optional/mandatory
// whitespace run starts, `minWs` how many characters of it are mandatory.
bool matchGreedyTail(const std::string& s, size_t wsFrom, size_t minWs, std::string& rest) {
  size_t m = wsFrom;
  while (m < s.size() && isRegexSpace(s[m])) ++m;
  if (m - wsFrom < minWs) return false;
  if (m < s.size()) {
    if (s.find('\n', m) != std::string::npos) return false;  // `.` cannot cross a newline
    rest.assign(s, m, s.size() - m);
    return true;
  }
  // Nothing left: `.+` must take back one whitespace character, and only a
  // non-newline one, and only if the run can still satisfy its own minimum.
  if (m == wsFrom || (m - 1) - wsFrom < minWs || s[m - 1] == '\n') return false;
  rest.assign(1, s[m - 1]);
  return true;
}

// `^scalar\s+drop\s+(.+)$` (icase).
bool matchScalarDrop(const std::string& s, std::string& rest) {
  if (!ciStartsWith(s, "scalar", 6)) return false;
  size_t i = 6;
  if (i >= s.size() || !isRegexSpace(s[i])) return false;
  while (i < s.size() && isRegexSpace(s[i])) ++i;
  if (!ciStartsWith(s.substr(i, 4), "drop", 4)) return false;
  i += 4;
  return matchGreedyTail(s, i, 1, rest);
}

// `^scalar\s+(?:define\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$` (icase).
bool matchScalarDef(const std::string& s, std::string& name, std::string& rest) {
  if (!ciStartsWith(s, "scalar", 6)) return false;
  size_t i0 = 6;
  if (i0 >= s.size() || !isRegexSpace(s[i0])) return false;
  while (i0 < s.size() && isRegexSpace(s[i0])) ++i0;

  // The optional `define\s+` group is greedy: the engine tries it first and
  // falls back to skipping it, which is how `scalar define = 3` parses with
  // NAME="define". Both arms are tried here, in that order.
  for (int withDefine = 1; withDefine >= 0; --withDefine) {
    size_t i = i0;
    if (withDefine == 1) {
      if (!ciStartsWith(s.substr(i, 6), "define", 6)) continue;
      size_t j = i + 6;
      if (j >= s.size() || !isRegexSpace(s[j])) continue;
      while (j < s.size() && isRegexSpace(s[j])) ++j;
      i = j;
    }
    if (i >= s.size() || !isIdentStart(s[i])) continue;
    size_t j = i;
    ++j;
    while (j < s.size() && isIdentChar(s[j])) ++j;
    size_t k = j;
    while (k < s.size() && isRegexSpace(s[k])) ++k;
    if (k >= s.size() || s[k] != '=') continue;
    std::string tail;
    if (!matchGreedyTail(s, k + 1, 0, tail)) continue;
    name.assign(s, i, j - i);
    rest = tail;
    return true;
  }
  return false;
}

constexpr int kMaxExpansionPasses = 16;

// ---------------------------------------------------------------------------
// display_hardening resolvers (the est-scalar subset _eval_arith needs)

// _E_SCALAR_KEY: MOVED out of this anonymous namespace to piet::eval proper
// (AER-SCOPE1 bundle E, 2026-09-04) so handlers_aer_tables.cpp can freeze the
// same e() scalar names into an eststo snapshot. The body is unchanged and now
// lives just below the anonymous namespace's closing brace; declared in
// eval/macros.h. Exposing it was the alternative to a THIRD copy of the table --
// there are already two (the twin is display_hardening.cpp's std::map version,
// identical in content today and commented as such in both files).

std::optional<double> jsonToFloat(const nlohmann::ordered_json& v) {
  if (v.is_number()) return v.get<double>();
  if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
  if (v.is_string()) return pythonFloat(v.get<std::string>());
  return std::nullopt;  // null / object / array -> TypeError
}

std::string resolveEScalar(const std::string& name, const EvalSession& session) {
  // BUG-421 (2026-09-06): a handler-POSTED e-scalar lives in `eScalars`, not in
  // the estimation JSON. `pca` posts e(rho)/e(f)/e(trace) there and `estadd
  // scalar` posts whatever the user names; reading only lastModelResult made
  // every one of them answer `.` on a surface that had the value in hand.
  // The model result still wins when both carry the name, so a fitted e(N)
  // cannot be shadowed by a posted one.
  if (const double* posted = session.eScalars.find(name);
      posted != nullptr && !session.lastModelResult) {
    return pythonFloatRepr(*posted);
  }
  if (!session.lastModelResult) return ".";
  const auto& result = *session.lastModelResult;
  if (result.is_null() || (result.is_object() && result.empty())) {
    if (const double* p2 = session.eScalars.find(name)) return pythonFloatRepr(*p2);
    return ".";
  }
  std::string key = name;
  for (const auto& [k, v] : eScalarKey()) {
    if (k == name) {
      key = v;
      break;
    }
  }
  if (!result.contains(key) || result.at(key).is_null()) {
    if (const double* p3 = session.eScalars.find(name)) return pythonFloatRepr(*p3);
    return ".";
  }
  auto f = jsonToFloat(result.at(key));
  // A clustered fit cannot have 0 clusters: the engine's n_clusters 0 means
  // "unclustered", which Stata reports as e(N_clust) missing.
  if (f && name == "N_clust" && *f == 0.0) f.reset();
  if (!f) {
    if (const double* p4 = session.eScalars.find(name)) return pythonFloatRepr(*p4);
    return ".";
  }
  // BUG-382 (off-pin sweep, 2026-09-01): substitute at FULL precision, exactly
  // as the r-class resolver always has. fmtNumeric is %.9g, so `scalar s =
  // e(ll)` received 9 significant digits and no later format could recover the
  // rest (measured: %20.15e of the scalar disagreed with the block's own value
  // in the 10th digit). Bare `display e(ll)` is unchanged by this: display
  // RE-RENDERS a substituted numeric literal through its own %9.0g default —
  // the r-class precedent proves it, since r() substitutes pythonFloatRepr and
  // bare `display r(mean)` still prints Stata's short form (and 173 freeze
  // fixtures pin exactly that rendering). The entry's original one-line-fix
  // refutation reasoned display would echo the text; the architecture says it
  // re-renders, and the freeze battery is the verifier.
  return pythonFloatRepr(*f);
}

// BUG-158 (P4 wave): Stata names the intercept `_cons`; this engine's no-FE
// coeftable names it `(intercept)` (build_model's design-matrix column name),
// while the FE path already emits `_cons` (feols.cpp's reconstructed reghdfe
// intercept). Both spellings therefore coexist in ONE build, so the alias has
// to be resolved at the consumer. Without it `_b[_cons]` — the spelling almost
// all real Stata code uses — matched nothing and fell through to ".", i.e. a
// silent missing with rc 0.
// NOTE: this resolver is DUPLICATED (macros.cpp for expressions/if-conditions,
// display_hardening.cpp for `display`). Both copies must carry this alias or
// `display _b[_cons]` and `scalar q = _b[_cons]` disagree — the exact
// two-layer split that makes a wrong answer silent. Change them together.
// BUG-456 (2026-09-08): `err` is the r(111) channel the display twin has had
// since BUG-162 (display_hardening.cpp resolveBRef). A coefficient NAME that is
// not in the model is r(111) "[nosuch] not found" on EVERY Stata surface --
// local, scalar, gen, count, assert, if (refute3_capture.log V3-V8) -- and
// returning `.` for it turned a typo into a missing that `assert _b[nosuch]==0`
// then reported as r(9), a wrong-reason error. When `err` is null the historical
// `.` is kept so no caller changes behaviour without opting in. The NO-MODEL
// case stays `.` here on purpose: the row surfaces leave the `_b[` text alone
// with no model (BUG-432's ruling, dispatcher.cpp substituteRowExprRefs) so the
// engine refuses it honestly, and the macro surfaces keep their pre-existing
// `.` (Stata: r(111) "_b not allowed when e(b) is not present" -- a named
// divergence, not claimed by this change).
std::string resolveBRef(const std::string& varname, const std::string& kind,
                         const EvalSession& session, std::string* err = nullptr) {
  if (!session.lastModelResult) return ".";  // no estimation in memory
  const auto& result = *session.lastModelResult;
  // Try the requested spelling first, then its intercept alias.
  std::vector<std::string> tryNames{varname};
  if (varname == "_cons") tryNames.push_back("(intercept)");
  else if (varname == "(intercept)") tryNames.push_back("_cons");
  if (!result.is_null() && result.contains("coeftable")) {
    for (const auto& want : tryNames) {
      for (const auto& row : result.at("coeftable")) {
        if (row.contains("name") && row.at("name") == want) {
          const char* field = (kind == "b") ? "b" : "se";
          if (!row.contains(field) || row.at(field).is_null()) return ".";
          auto f = jsonToFloat(row.at(field));
          if (!f) return ".";
          return pythonFloatRepr(*f);  // BUG-382: _b[]/_se[] carry full precision too
        }
      }
    }
  }
  // Name not in the model -- r(111), same text as the display twin (BUG-162).
  if (err != nullptr && err->empty()) *err = varname + " not found";
  return ".";
}

// BUG-456 (2026-09-08): the bare-form e(name) resolver's STRING arm. Measured
// on StataNow 19.5 BE (plans/bug456-20260908/refute3_capture.log, 40/40 macro
// shapes): a bare e-MACRO is a STRING in every expression context -- `ereturn
// local m "5"` then `display e(m)+1` is r(109), `assert e(m)=="5"` rc 0,
// `assert e(m)==5` r(109), `local t = e(m)` keeps the TEXT. [U] 18.9 rule 1.3
// as printed ("examined to determine whether its contents can be interpreted
// as a number") is NOT what the binary does, so the numeric-looking text is
// never turned into a number here. The literal is built exactly like the bare
// r(name) string arm in expandPass (BUG-374): a double-quoted literal with any
// interior `"` escaped, so it reaches evalArith / the row fold as a string
// VALUE and not as text to re-evaluate. MEASURED CORRECTION (2026-09-09, review
// finding 2): the ESCAPE does not survive the evaluator -- `estadd local qq
// "a\"b"` then `local t = e(qq)` is r(198) "invalid syntax in 'e(qq)'", where
// Stata passes `a"b` through at rc 0 (REFUTE-0 §3, T1-T9). It is an honest
// error and it is unreachable today (PIET refuses compound double quotes
// outright -- `estadd local qq `"a"b"'` is an engine_error piet_gap -- so no
// e-string can currently carry a `"`), but the escape is a claim this code
// cannot yet make: named, not fixed. An ABSENT name (and an empty macro,
// which setEString never stores) is numeric missing, never "" -- `assert
// e(nosuch)==.` rc 0, `assert e(nosuch)==""` r(109) (A8/A9/N1) -- which is
// what falling through to resolveEScalar answers. Guard order at the caller is
// matrix -> string -> scalar: after `ereturn local b "x"` the bare e(b) is
// still the coefficient matrix in Stata (W1-W8).
std::string resolveERef(const std::string& name, const EvalSession& session) {
  if (const std::string* s = session.eStrings.find(name); s != nullptr && !s->empty()) {
    std::string lit = "\"";
    for (char ch : *s) {
      if (ch == '"') lit += "\\\"";
      else lit += ch;
    }
    lit += "\"";
    return lit;
  }
  return resolveEScalar(name, session);
}

// BUG-456 follow-up (2026-09-09) — A NUMBER IN A STRING-TYPED ARGUMENT SLOT.
//
// The e()/`_b[]` resolvers above substitute a NUMBER for everything that is not
// an e-string: an absent name becomes `.`, an e-scalar becomes its value. That
// is right for arithmetic and for the row plane's constant compares, and WRONG
// the moment the reference sits inside a STRING function -- `strpos(e(nosuch),
// "reg")` became `strpos(.,"reg")`, which the expression evaluator's pythonStr()
// coercion answered 0 at rc 0, and `strlen(e(N))` answered the decimal width of
// the substituted number. Stata refuses every one of those shapes.
//
// MEASURED on StataNow 19.5 BE (scratch oracle.log, 2026-09-09, sysuse auto +
// regress price mpg): `count if strpos(e(nosuch),"reg")` r(109) · `inlist(
// e(nosuch),"regress")` r(109) · `strlen(e(nosuch))==7` r(109) · `substr(
// e(nosuch),1,3)=="reg"` r(109) · `upper(e(nosuch))=="REG"` r(109) · `local p =
// strpos(e(nosuch),"reg")` r(109) · `gen za = strpos(e(nosuch),"reg")` r(109) ·
// `replace price = 1 if strpos(e(nosuch),"reg")` r(109) · with a PRESENT
// e-scalar the same: `strpos(e(N),"7")` r(109), `inlist(e(N),"74")` r(109),
// `local q = strlen(e(N))` r(109), `display strlen(e(nosuch))` r(109). The
// accepting arm is unchanged: `strpos(e(cmd),"reg")` 74, `inlist(e(cmd),
// "regress")` 74, `substr(e(cmd),1,3)=="reg"` 74, `local c = strpos(e(cmd),
// "gres")` 3 -- an e-STRING is a string, so a string function takes it.
//
// The rule is the ARGUMENT's type, not where it came from: Stata gives r(109)
// for a numeric LITERAL (`strpos(5,"a")`, `strlen(5)`), for a numeric VARIABLE
// (`strpos(mpg,"7")`) and for `_b[mpg]`/`_se[]` too (O17-O21 in the same log).
// So the test here is on the resolved TEXT and claims every numeric CONSTANT in
// a string slot, whoever wrote it. A numeric VARIABLE is a data-plane type the
// text cannot see -- named gap, not claimed.
//
// inlist()/inrange() are polymorphic in Stata (all arguments one type): a
// MIXED call is the r(109) (`inlist(e(N),"74")`), while all-numeric is fine
// (`inlist(e(N),74)` -> 74 rows, measured O23).
//
// cond() (review finding 1, 2026-09-09) is polymorphic in a SUBSET of its
// slots, which is why the whole-arglist `poly` flag above could not simply be
// reused for it: slot 0 is the numeric CONDITION and slots 1/2 are the value
// pair that must agree with each other. MEASURED on StataNow 19.5 BE
// (/private/tmp/bug456rev/cond_oracle.log, sentinel CONDQ-ORACLE-END, sysuse
// auto + regress price mpg): `local cd = cond(1,e(nosuch),"x")` r(109) (Q1) ·
// `count if cond(1,e(nosuch),"x")=="x"` r(109) (Q2) · `local c5 =
// cond(1,5,"x")` r(109) (Q3) · `local c6 = cond(1,e(N),"x")` r(109) (Q4). The
// ACCEPTING arms are rc 0 and must stay: `count if cond(1,e(cmd),"x")==
// "regress"` -> 74 (Q5, an e-string is a string) and `local c7 =
// cond(1,"a","b")` -> "a" (Q6) -- a whole-arg-list poly test would turn Q6
// into a false r(109) because slot 0 is the number 1.

bool numericConstToken(const std::string& t) {
  if (t.empty()) return false;
  // `.`, `.a`..`.z` (Stata's extended missings) and any ordinary numeric
  // literal, with the sign the substituted value may carry.
  if (t == ".") return true;
  if (t.size() == 2 && t[0] == '.' && std::isalpha(static_cast<unsigned char>(t[1]))) return true;
  static const std::regex reNum(R"(^[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][+-]?[0-9]+)?$)");
  return std::regex_match(t, reNum);
}

// The string-typed argument slots of the string family this engine registers
// (exprfns.cpp addStataFns). Three kinds of row, checked in this order:
//   `poly`      -- EVERY argument must be one type (inlist/inrange).
//   `polySlots` -- the listed slots must agree with EACH OTHER, the rest are
//                  not claimed (cond: the value pair 1/2, never the condition).
//   `stringSlots` -- fixed string-typed slots; a numeric constant there is the
//                  r(109).
struct StringFnArgSpec {
  const char* name;
  std::vector<size_t> stringSlots;  // empty + poly=true -> the mixed-type rule
  bool poly = false;
  std::vector<size_t> polySlots;  // the mixed-type rule over a SUBSET of slots
};

const std::vector<StringFnArgSpec>& stringFnArgSpecs() {
  static const std::vector<StringFnArgSpec> specs = {
      {"strlen", {0}},   {"length", {0}},     {"strpos", {0, 1}},  {"substr", {0}},
      {"upper", {0}},    {"lower", {0}},      {"strupper", {0}},   {"strlower", {0}},
      {"proper", {0}},   {"trim", {0}},       {"strtrim", {0}},    {"ltrim", {0}},
      {"rtrim", {0}},    {"strltrim", {0}},   {"strrtrim", {0}},   {"stritrim", {0}},
      {"word", {0}},     {"abbrev", {0}},     {"real", {0}},       {"regexm", {0, 1}},
      {"regexr", {0, 1, 2}}, {"subinstr", {0, 1, 2}}, {"soundex", {0}}, {"soundex_nara", {0}},
      {"ustrfix", {0}},
      {"inlist", {}, true}, {"inrange", {}, true},
      // AER scope-2 B2-3: the business-calendar pair. bofd(calname, date) and
      // dofb(bdate, calname) each carry ONE string slot, and a numeric
      // constant there is Stata's r(109) -- which is also how a SWAPPED call
      // (`bofd(td(15jan2020), "sp500")`) is caught, measured rc 109.
      {"bofd", {0}}, {"dofb", {1}},
      // cond(c, a, b [, missval]): slots 1 and 2 are the polymorphic pair.
      {"cond", {}, false, {1, 2}},
      // BUG-660 fix round: the new macro-layer string functions this
      // exprfns.cpp landing added. strproper/usubinstr are literal ALIASES
      // of proper/subinstr (same function pointer, exprfns.cpp's own
      // registration comment) -- omitting them here would have made
      // `strproper(5)` rc 0 while `proper(5)` is r(109), a contradiction for
      // the SAME underlying call. reverse/strrpos/wordcount/strtoname/
      // strmatch/ustrunescape/ustrregexra are new functions with no sibling
      // entry; added by the same general rule (a string function given a
      // numeric CONSTANT is r(109), the BUG-456 contract this whole table
      // implements) rather than left unclaimed.
      {"strproper", {0}}, {"usubinstr", {0, 1, 2}}, {"reverse", {0}},
      {"strrpos", {0, 1}}, {"wordcount", {0}}, {"strtoname", {0}},
      {"strmatch", {0, 1}}, {"ustrunescape", {0}}, {"ustrregexra", {0, 1, 2}},
  };
  return specs;
}

// Splits the argument list of the call whose '(' is at `open`. Returns false if
// the parentheses do not close (malformed text this pass does not own).
bool splitCallArgs(const std::string& e, size_t open, std::vector<std::string>* args,
                   size_t* closeOut) {
  int depth = 0;
  bool inStr = false;
  size_t argStart = open + 1;
  for (size_t i = open; i < e.size(); ++i) {
    const char c = e[i];
    if (inStr) {
      if (c == '\\' && i + 1 < e.size()) { ++i; continue; }
      if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') { inStr = true; continue; }
    if (c == '(') { ++depth; continue; }
    if (c == ')') {
      --depth;
      if (depth == 0) {
        args->push_back(e.substr(argStart, i - argStart));
        *closeOut = i;
        return true;
      }
      continue;
    }
    if (c == ',' && depth == 1) {
      args->push_back(e.substr(argStart, i - argStart));
      argStart = i + 1;
    }
  }
  return false;
}

// Argument kinds for the type test: 1 = string, 2 = numeric constant, 0 = the
// pass does not know (a variable, a call, arithmetic) and does not claim it.
int argKind(const std::string& raw) {
  std::string t = strip(raw);
  if (t.empty()) return 0;
  if (t.front() == '"') return 1;  // a literal (or a literal-headed concat)
  if (numericConstToken(t)) return 2;
  return 0;
}

// Finds the first string-typed slot holding a numeric constant. Returns true
// and fills *call with the offending call's text.
bool stringFnNumericArgImpl(const std::string& e, std::string* call) {
  if (e.find('(') == std::string::npos) return false;
  bool inStr = false;
  for (size_t i = 0; i < e.size(); ++i) {
    const char c = e[i];
    if (inStr) {
      if (c == '\\' && i + 1 < e.size()) { ++i; continue; }
      if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') { inStr = true; continue; }
    if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '_')) continue;
    if (i > 0 && isIdentChar(e[i - 1])) continue;  // mid-identifier
    size_t j = i;
    while (j < e.size() && isIdentChar(e[j])) ++j;
    const std::string name = e.substr(i, j - i);
    size_t k = j;
    while (k < e.size() && std::isspace(static_cast<unsigned char>(e[k]))) ++k;
    if (k >= e.size() || e[k] != '(') { i = j - 1; continue; }
    const StringFnArgSpec* spec = nullptr;
    for (const auto& s : stringFnArgSpecs())
      if (name == s.name) { spec = &s; break; }
    if (spec == nullptr) { i = j - 1; continue; }
    std::vector<std::string> args;
    size_t close = 0;
    if (!splitCallArgs(e, k, &args, &close)) { i = j - 1; continue; }
    bool bad = false;
    if (spec->poly) {
      bool anyStr = false, anyNum = false;
      for (const auto& a : args) {
        const int kd = argKind(a);
        if (kd == 1) anyStr = true;
        else if (kd == 2) anyNum = true;
      }
      bad = anyStr && anyNum;
    } else if (!spec->polySlots.empty()) {
      // The mixed-type rule over the named slots only. Slots the call does not
      // supply are simply absent from the group (`cond(1,"a")` is a malformed
      // call this pass does not own).
      bool anyStr = false, anyNum = false;
      for (size_t slot : spec->polySlots) {
        if (slot >= args.size()) continue;
        const int kd = argKind(args[slot]);
        if (kd == 1) anyStr = true;
        else if (kd == 2) anyNum = true;
      }
      bad = anyStr && anyNum;
    } else {
      for (size_t slot : spec->stringSlots)
        if (slot < args.size() && argKind(args[slot]) == 2) { bad = true; break; }
    }
    if (bad) {
      std::string text = e.substr(i, close + 1 - i);
      if (text.size() > 72) text = text.substr(0, 69) + "...";
      *call = text;
      return true;
    }
    i = j - 1;  // keep scanning: nested calls are found by this same walk
  }
  return false;
}

}  // namespace

bool hasBareRParen(const std::string& s) { return hasBareLetterParen(s, 'r'); }

// B4-W3b: is this line one of the MATRIX verbs — the only surface that can
// consume an r-class matrix reference (handlers_m3.cpp:1978 / :1955)? Stata's
// own abbreviations of `matrix` are mat/matr/matri/matrix; `matlist` is the
// separate display verb that reads the same store.
bool isMatrixVerbLine(const std::string& line) {
  size_t i = line.find_first_not_of(" \t");
  if (i == std::string::npos) return false;
  size_t j = i;
  while (j < line.size() && (std::isalpha(static_cast<unsigned char>(line[j])) != 0)) ++j;
  std::string verb = line.substr(i, j - i);
  for (auto& c : verb) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return verb == "mat" || verb == "matr" || verb == "matri" || verb == "matrix" ||
         verb == "matlist";
}
bool hasBareEParen(const std::string& s) { return hasBareLetterParen(s, 'e'); }

// MOVED OUT of the anonymous namespace 2026-09-06 (BUG-432): the dispatcher's
// row-predicate substitution point needs this exact function, and a second copy
// of a `\b` rule is how two layers start disagreeing about what a bare name is.
// Declared in eval/macros.h; nothing else about it changed.
// `\bNAME\b(?!\s*\()` -> `value`, all occurrences, value inserted literally.
// NAME is a scalar name, so the old code built (and COMPILED) one std::regex
// per scalar per expression — the single most expensive shape a hot loop can
// have. The \b rule is implemented in general form rather than assuming NAME
// begins and ends with a word character, so a store key that somehow does not
// still behaves exactly like the regex.
//
// BUG-512: the scan is a template over WHERE THE VALUE COMES FROM, so `_N` can
// ask for the observation count only once a bare `_N` has actually been found
// (substituteObsSpecials below). Same `\b` rule, still one copy of it; the
// public function is the eager instantiation and is byte-for-byte what it was.
namespace {
template <typename ValueFn>
std::string substituteBareNameWith(const std::string& in, const std::string& name,
                                   ValueFn&& valueOf) {
  if (name.empty()) return in;  // the regex `\b\b(?!...)` case never arises
  const bool nameStartsWord = isIdentChar(name.front());
  const bool nameEndsWord = isIdentChar(name.back());
  std::string out;
  size_t i = 0, last = 0;
  bool any = false;
  while (true) {
    const size_t p = in.find(name, i);
    if (p == std::string::npos) break;
    const size_t e = p + name.size();
    // \b holds iff exactly one side of the position is a word character.
    const bool leftWord = (p > 0) && isIdentChar(in[p - 1]);
    const bool rightWord = (e < in.size()) && isIdentChar(in[e]);
    if (leftWord == nameStartsWord || rightWord == nameEndsWord) {
      i = p + 1;
      continue;
    }
    // (?!\s*\() — a following call-parenthesis spares the name.
    size_t q = e;
    while (q < in.size() && isRegexSpace(in[q])) ++q;
    if (q < in.size() && in[q] == '(') {
      i = p + 1;
      continue;
    }
    out.append(in, last, p - last);
    out.append(valueOf());
    last = e;
    i = e;
    any = true;
  }
  if (!any) return in;
  out.append(in, last, in.size() - last);
  return out;
}
}  // namespace

std::string substituteBareName(const std::string& in, const std::string& name,
                               const std::string& value) {
  return substituteBareNameWith(in, name, [&value]() -> const std::string& { return value; });
}

// BUG-512: `_N` / `_n` in a MACRO-context expression.
//
// Stata evaluates `local n = _N`, `scalar s = _N`, `global G = _N`,
// `` `=_N-1' `` and `return scalar N = _N` like any other expression, on
// observation 1: `_N` is the observation count and `_n` is 1
// (tests/eval/golden/aer_w2/w2_smalls.log section A -- local_N 12, local_n 1,
// local_N_expr 25, scalar_N 12, inline_N 12|11, global_N 12). Here neither name
// existed below SessionState, so both reached the evaluator as undefined
// identifiers: the `=` forms fell into classifyFailedEqExpr's unclaimed
// bare-identifier arm and stored their own SOURCE TEXT at rc 0 (`_N*2 + 1`),
// `scalar s = _N` stored `_N`, `return scalar N = _N` posted missing, `display
// _N*2` echoed `_N*2`, and the inline form never converged.
//
// ONE substitution, called by both expression entry points of this layer
// (evalArith and evalMacroExpr), which between them are every consumer:
// local/global/scalar `=`, display, `return scalar`, the resampling exp_list and
// every expandMacros call site. A text substitution like `_rc` beside it, and
// for `_rc`'s reason -- `_N*2 + 1` becomes `12*2 + 1` and stays on evalArith's
// fast path, where a namespace binding would have pushed it off.
//
// Quote-aware on its own account (a caller outside this file has not run
// protectStringLiterals): `"_N"` inside a literal is data and stays text.
// The count is PULLED through EvalSession::obsCount, and only once a bare `_N`
// is really there -- `my_N` or `r2_n` must not cost a Datastore open.
// No hook (evalcase harness, session-less callers) = unchanged input.
std::string substituteObsSpecials(const std::string& in, const EvalSession& session) {
  if (!session.obsCount) return in;
  const bool hasBigN = in.find("_N") != std::string::npos;
  const bool hasSmallN = in.find("_n") != std::string::npos;
  if (!hasBigN && !hasSmallN) return in;  // memchr-class, the common case
  std::vector<std::string> spans;
  std::string s = protectStringLiterals(in, &spans);
  if (hasBigN) {
    std::string countText;
    bool asked = false;
    s = substituteBareNameWith(s, "_N", [&]() -> const std::string& {
      if (!asked) {
        countText = std::to_string(session.obsCount());
        asked = true;
      }
      return countText;
    });
  }
  if (hasSmallN) {
    static const std::string kOne = "1";
    s = substituteBareNameWith(s, "_n", []() -> const std::string& { return kOne; });
  }
  return restoreStringLiterals(s, spans);
}

// _E_SCALAR_KEY (display_hardening.py)
const std::vector<std::pair<std::string, std::string>>& eScalarKey() {
  static const std::vector<std::pair<std::string, std::string>> k = {
      {"r2", "r2"},       {"r2_a", "adj_r2"}, {"adj_r2", "adj_r2"}, {"rmse", "rmse"},
      {"mss", "mss"},     {"rss", "rss"},     {"df_m", "df_m"},     {"df_r", "df_r"},
      {"F", "F"},         {"N", "n_obs"},     {"N_clust", "n_clusters"},
      {"rank", "k_vars"}, {"p", "p_F"},       {"ll", "ll"},
      // BUG-156 (wiring half, P4 wave): the engine ALREADY emits the correct
      // within-R2 as "r2_within" (measured 0.414404 against Stata's 0.4144 on
      // the P2 fixture) — it was simply unreachable under its Stata name
      // `e(r2_w)`, so a correct number read as missing. The rest of BUG-156
      // (poisson e(ll)/e(chi2)/e(r2_p), xtreg between/overall/rho) is engine
      // COMPUTE work and stays open; only this unreachable-but-correct entry
      // is closed here.
      // Duplicated table — the twin lives in the other of
      // {eval/macros.cpp, display_hardening.cpp}. Keep them in step or
      // `display e(r2_w)` and `scalar q = e(r2_w)` disagree.
      {"r2_w", "r2_within"},
      // BUG-156 (wiring half, P2-PREP) — the rest of the names, so the Stata
      // spellings stop being unreachable the moment the engine emits the
      // keys. `ll` is already above; `ll_0`/`chi2` happen to map onto their
      // own cobra key names, which is why they need an entry at all (the
      // lookup below only rewrites names it FINDS in this table — an absent
      // name falls through as key==name, so these two are strictly
      // documentation; sigma_u/sigma_e/rho likewise). `r2_p` and `corr` are
      // the two that genuinely rename: poisson's pseudo-R2 and feols'
      // corr(u_i, Xb).
      {"ll_0", "ll_0"},     {"chi2", "chi2"},     {"r2_p", "r2_p"},
      // W1a-5d (2026-09-18): `r2_pseudo` is the SECOND name the same quantity
      // arrives under, and it was absent from this table, so it never reached
      // a STORED estimate's escalars -- `esttab <logit>, pr2` rendered a blank
      // cell. Measured on this engine: poisson publishes `r2_p` (.0126126) and
      // logit/probit publish `r2_pseudo` (.0321488 / .0345996), each leaving
      // the other missing, and nbreg publishes neither. `estimation_m4.cpp:430`
      // already emits it; only the travel was missing.
      {"r2_pseudo", "r2_pseudo"},
      {"sigma_u", "sigma_u"}, {"sigma_e", "sigma_e"}, {"rho", "rho"},
      {"corr", "corr_u_xb"},
  };
  return k;
}

// macros._resolve_est_scalar_refs — exposed (not file-local) for loops.cpp's
// if/while condition evaluator (M3a). Helpers above (resolveBRef,
// resolveEScalar, regexSubFn) stay file-local; only this entry point is
// part of the public macros.h surface.
std::string resolveEstScalarRefs(std::string expr, const EvalSession& session,
                                 std::string* err) {
  dprof::EScope _z(dprof::kEResolveEst);
  // AER scope-2 B2 (2026-09-08): `_b[eq:name]` -- Stata's equation-qualified
  // spelling (`_b[overall:difference]` after oaxaca, `_b[ATE:r1vs0.t]` after
  // teffects). The coeftable row NAME carries the same `eq:name` string, so the
  // lookup stays an exact match; only the reference grammar widens. The twin in
  // display_hardening.cpp (reBRef/reSeRef) MUST stay in step.
  // B2-4 (2026-09-09) widened the tail class by ONE character, `.`: the comment
  // above already named `_b[ATE:r1vs0.t]` as the target spelling, but `[\w:]`
  // has no dot, so the reference fell through to the undefined-matrix arm and
  // came back "matrix _b not found" (r(111)) -- an honest error, and the wrong
  // one. Stata's teffects colname carries the treatment VARIABLE after the
  // r#vs#, so the dot is structural, not decorative.
  static const std::regex reB(R"(_b\[([A-Za-z_][\w:.]*)\])");
  static const std::regex reSe(R"(_se\[([A-Za-z_][\w:.]*)\])");
  static const std::regex reECall(R"(\be\(\s*([A-Za-z_]\w*)\s*\))");

  // A3: each of these four ran a full regex scan over EVERY expression, whether
  // or not the text could possibly contain its construct. A profile of a pure
  // macro-arithmetic loop (200k iterations, symbolized `sample`, 2026-08-06) put
  // ~18% of samples inside libc++'s regex engine and ~40% in malloc/free, most
  // of the latter being the regex engine's own per-execution state vectors —
  // so the cheapest scan is the one that never runs. A literal `find()` is a
  // memchr-class loop and is a strict pre-filter: every one of these patterns
  // requires its literal substring to appear, so no match can be missed.
  // (Deliberately NOT a single combined regex: the substitutions must stay
  // ordered and independent, and combining them would be a semantic change
  // dressed as an optimization.)
  if (expr.find("_b[") != std::string::npos) {
    expr = regexSubFn(reB, expr, [&session, err](const std::smatch& m) {
      return resolveBRef(m.str(1), "b", session, err);
    });
  }
  if (expr.find("_se[") != std::string::npos) {
    expr = regexSubFn(reSe, expr, [&session, err](const std::smatch& m) {
      return resolveBRef(m.str(1), "se", session, err);
    });
  }
  // scalar(NAME) function form -> the stored scalar value (or the literal
  // token unchanged when undefined).
  if (expr.find("scalar(") != std::string::npos) {
    expr = substituteScalarCalls(expr, session);  // EXPRLAYER-5: was reScalarCall
  }
  // e(name) scalar — NEVER a matrix. The exclusion used to be the literal pair
  // b/V, which was complete while those were the only e-matrices that existed.
  // Once pca posts e(Ev)/e(L)/e(C)/e(Psi)/e(means)/e(sds)/e(components), the
  // literal list silently under-covers: this pass would resolve e(Ev) to
  // MISSING before the matrix pass ever saw it, so `e(Ev)[1,2]` evaluated as
  // `.[1,2]` and `colsof(e(Ev))` as `colsof(.)`. Asking the store instead of a
  // hardcoded pair keeps the same rule ("a matrix is not a scalar") true for
  // every matrix, including ones added later.
  if (hasBareEParen(expr)) {  // EXPRLAYER-3: reECall is \be\( — the \b is the guard
    expr = regexSubFn(reECall, expr, [&session, err](const std::smatch& m) {
      std::string inner = m.str(1);
      if (inner == "b" || inner == "V") return m.str(0);
      if (session.findClassMatrix('e', inner) != nullptr) return m.str(0);
      // W2 predictframe (2026-09-19): e(sample) is Stata's per-ROW estimation
      // sample mask, not a scalar, and this build keeps no such mask. It used to
      // fall through to resolveERef as an ABSENT name -> `.`, and a missing
      // condition is TRUE in every row plane, so after `regress y x2 t if id < 4`
      // (9 in-sample rows) `predict p if e(sample)` filled all 12 at rc 0, and
      // `count`/`gen`/`summarize`/`regress`/`keep if e(sample)` all answered
      // over 12 (measured on the frozen main binary, 2026-09-19; Stata: 9 --
      // esttab-beta.log:488 `predict ... if e(sample)` is the served shape).
      // Refused BY NAME on the same channel as r(111)/r(109) below, so every
      // caller maps it the same way; with no channel the token is left as text
      // for the evaluator to refuse in its own words -- never a silent `.`.
      if (inner == "sample") {
        if (err != nullptr && err->empty()) {
          *err = "r(198): e(sample) is not available in this version of PIET: the "
                 "estimation sample is not kept as a row mask, so `if e(sample)` cannot be "
                 "evaluated. Restate the sample with the estimation command's own if/in "
                 "condition.";
        }
        return m.str(0);
      }
      // BUG-456: matrix (above) -> string -> scalar. The string arm is what
      // `local c = e(cmd)`, `if e(cmd)=="regress" {`, `assert e(cmd)=="regress"`
      // and `gen z = (e(cmd)=="regress")` all lacked; every one of them saw `.`.
      return resolveERef(inner, session);
    });
  }
  // BUG-456 follow-up: a NUMBER that has landed in a STRING-typed argument slot
  // is r(109) "type mismatch" (see stringFnNumericArgImpl's measurement block).
  // Reported on the SAME `err` channel as the r(111) above, distinguished by an
  // explicit `r(NNN): ` prefix that splitErrCode() hands the callers -- an
  // r(111) already found wins, it was raised earlier in the expression.
  if (err != nullptr && err->empty()) {
    std::string call;
    if (stringFnNumericArgImpl(expr, &call)) *err = stringSlotMismatchError(call);
  }
  return expr;
}

// The same test, exposed for the ROW/GEN planes. They cannot rely on the call
// above: a PRESENT e-scalar is substituted by the MACRO expansion layer before
// the dispatcher ever sees the line (macros.cpp expandPass), so by then the
// text reads `strpos(20.0,"2")` with no `e(` left in it -- the dispatcher's
// `wantsEst` prefilter is false and the resolver never runs. Measured with a
// debug print at the row call site (2026-09-09): `count if strpos(e(N),"2")`
// arrives as `strpos(20.0,"2")`, while `strpos(e(nosuch),"x")` (an ABSENT name,
// which the macro layer leaves alone) still arrives unresolved. One test, two
// entry points -- never a second copy of the rule.
bool stringFnNumericArg(const std::string& expr, std::string* call) {
  return stringFnNumericArgImpl(expr, call);
}

std::string stringSlotMismatchError(const std::string& call) {
  return "r(109): type mismatch: `" + call + "` passes a number where a string is required";
}

// BUG-456 follow-up: the resolver's `err` channel carries TWO codes now -- the
// historical r(111) (unknown coefficient name, no prefix) and the r(109) the
// string-slot type test raises with an explicit `r(NNN): ` prefix. One splitter
// so the five call sites (evalArith, `scalar =`, if/while, the row plane, gen)
// cannot drift into two spellings of the same mapping.
std::string splitErrCode(std::string* msg) {
  if (msg == nullptr || msg->rfind("r(", 0) != 0) return "r(111)";
  const size_t close = msg->find(')');
  if (close == std::string::npos || close + 1 >= msg->size() || (*msg)[close + 1] != ':') {
    return "r(111)";
  }
  const std::string digits = msg->substr(2, close - 2);
  if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
    return "r(111)";
  }
  const std::string code = msg->substr(0, close + 1);
  *msg = strip(msg->substr(close + 2));
  return code;
}

std::string fmtNumeric(double v) {
  if (std::isnan(v)) return ".";
  if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  return std::string(buf);
}

// ---------------------------------------------------------------------------
// matrix_op.substitute_matrix_refs + get_matrix_element

namespace {

// Throws PyExc on out-of-range (ValueError) — caller treats like Python.
double readMatrixElement(const EvalMatrix* mat, long long i, long long j) {
  if (mat == nullptr) throw PyExc("KeyError: matrix not defined");
  long long nrows = static_cast<long long>(mat->data.size());
  long long ncols = nrows > 0 ? static_cast<long long>(mat->data[0].size()) : 0;
  if (!(1 <= i && i <= nrows)) throw PyExc("ValueError: row index out of range");
  if (!(1 <= j && j <= ncols)) throw PyExc("ValueError: column index out of range");
  return mat->data[static_cast<size_t>(i - 1)][static_cast<size_t>(j - 1)];
}

double getMatrixElement(const EvalSession& session, const std::string& name, long long i,
                         long long j) {
  return readMatrixElement(session.findMatrix(name), i, j);
}

}  // namespace

// A matrix cell can hold Stata's missing, and pythonFloatRepr spells that "nan",
// which is a NameError once it is substituted back into the expression. Emit `.`
// instead -- replaceBareDotMissing runs after this pass and turns it into the
// sentinel. Reachable from B3(c) onward because `J(3,2,.)` is the corpus's own
// spelling (24 uses) and paper 049 reads those cells straight back.
std::string matrixElementText(double v) {
  if (std::isnan(v)) return ".";
  return pythonFloatRepr(v);
}

std::string substituteMatrixRefs(const std::string& expr, const EvalSession& session) {
  static const std::regex reElem(R"(\b([A-Za-z_]\w*)\s*\[\s*(\d+)\s*,\s*(\d+)\s*\])");
  static const std::regex reColsof(R"(\bcolsof\s*\(\s*([A-Za-z_]\w*)\s*\))", std::regex::icase);
  static const std::regex reRowsof(R"(\browsof\s*\(\s*([A-Za-z_]\w*)\s*\))", std::regex::icase);
  // AER scope 2 / B3(c): colnumb(M,"name") -> the 1-based column index, or
  // missing when the name is not a column. Paper 020 drives an entire loop off
  // it (`local idx_row = colnumb(RHO_D`dd'_res, "rho_Dy`cc'_Dr`cc'")`). It
  // joins the family here rather than in the function namespace because that is
  // where every other matrix reference in an expression is resolved -- element
  // access, colsof and rowsof are all text substitutions against the session
  // store, and a fourth spelling of the same idea would be a second mechanism.
  // Stata takes either the bare column name or the equation-qualified one
  // ("a" and "eq1:a" both answer 1), so both are compared.
  static const std::regex reColnumb(
      // Custom raw-string delimiter: the pattern contains `)"`, which would close
      // a plain R"( ... )" early.
      R"RX(\bcolnumb\s*\(\s*([A-Za-z_]\w*)\s*,\s*"([^"]*)"\s*\))RX", std::regex::icase);
  // e(<name>)[i,j] / r(<name>)[i,j], and the same wrapper inside colsof/rowsof.
  // These run FIRST because the three bare-name patterns above cannot express
  // them: `\b([A-Za-z_]\w*)\s*\[` stops at the ')' in `e(Ev)[1,1]`, so before
  // AER bundle H there was no syntax at all for reading an element out of a
  // posted class matrix -- pca could publish e(Ev) and nothing could read it.
  static const std::regex reClsElem(
      R"(\b([er])\(\s*([A-Za-z_]\w*)\s*\)\s*\[\s*(\d+)\s*,\s*(\d+)\s*\])");
  static const std::regex reClsColsof(
      R"(\bcolsof\s*\(\s*([er])\(\s*([A-Za-z_]\w*)\s*\)\s*\))", std::regex::icase);
  static const std::regex reClsRowsof(
      R"(\browsof\s*\(\s*([er])\(\s*([A-Za-z_]\w*)\s*\)\s*\))", std::regex::icase);
  // el(NAME,i,j) / el(e(NAME),i,j) / el(r(NAME),i,j) -- Stata's function form
  // of NAME[i,j] (papers 058 and 044). Joins the same family for the reason
  // colsof/rowsof/colnumb do: every matrix reference in an expression is a
  // text substitution against the session store. The class-wrapped form runs
  // first for the same reason reClsElem does: `el(e(Ev),1,2)`'s inner
  // identifier is "e" up to the "(", so the bare pattern below never actually
  // matches it (the literal "," it requires next is a "(" instead), but
  // running the specific form first keeps that guarantee explicit rather than
  // incidental. Both corpus call sites (058's `` `z' ``, 044's r()-matrix
  // index) have already gone through macro expansion by the time evalArith
  // reaches this pass, so both indices are plain integer literals -- only
  // \d+ is accepted here, exactly like reElem. An arithmetic index such as
  // `el(M,1,`z'+1)` is a named omission, not a silent wrong value: it fails
  // to match, is left unsubstituted, and the parser refuses it the same way
  // an out-of-range NAME[i,j] does below.
  static const std::regex reClsEl(
      R"(\bel\s*\(\s*([er])\(\s*([A-Za-z_]\w*)\s*\)\s*,\s*(\d+)\s*,\s*(\d+)\s*\))",
      std::regex::icase);
  static const std::regex reEl(
      R"(\bel\s*\(\s*([A-Za-z_]\w*)\s*,\s*(\d+)\s*,\s*(\d+)\s*\))", std::regex::icase);

  std::string pre = regexSubFn(reClsElem, expr, [&session](const std::smatch& m) -> std::string {
    try {
      const EvalMatrix* mat = session.findClassMatrix(m.str(1)[0], m.str(2));
      double val = readMatrixElement(mat, std::stoll(m.str(3)), std::stoll(m.str(4)));
      return matrixElementText(val);
    } catch (const PyExc&) {
      return m.str(0);  // KeyError/ValueError — leave unchanged, as below
    } catch (const std::exception&) {
      return m.str(0);
    }
  });
  pre = regexSubFn(reClsEl, pre, [&session](const std::smatch& m) -> std::string {
    try {
      const EvalMatrix* mat = session.findClassMatrix(m.str(1)[0], m.str(2));
      double val = readMatrixElement(mat, std::stoll(m.str(3)), std::stoll(m.str(4)));
      return matrixElementText(val);
    } catch (const PyExc&) {
      return m.str(0);  // KeyError/ValueError — leave unchanged, as below
    } catch (const std::exception&) {
      return m.str(0);
    }
  });
  pre = regexSubFn(reClsColsof, pre, [&session](const std::smatch& m) -> std::string {
    const EvalMatrix* mat = session.findClassMatrix(m.str(1)[0], m.str(2));
    if (mat == nullptr) return m.str(0);
    long long ncols = mat->data.empty() ? 0 : static_cast<long long>(mat->data[0].size());
    return pythonFloatRepr(static_cast<double>(ncols));
  });
  pre = regexSubFn(reClsRowsof, pre, [&session](const std::smatch& m) -> std::string {
    const EvalMatrix* mat = session.findClassMatrix(m.str(1)[0], m.str(2));
    if (mat == nullptr) return m.str(0);
    return pythonFloatRepr(static_cast<double>(mat->data.size()));
  });

  pre = regexSubFn(reColnumb, pre, [&session](const std::smatch& m) -> std::string {
    const EvalMatrix* mat = nullptr;
    for (const auto& p : session.matrices)
      if (p.first == m.str(1)) { mat = &p.second; break; }
    if (mat == nullptr) return m.str(0);  // unknown matrix: leave for the parser to reject
    const std::string want = m.str(2);
    for (size_t i = 0; i < mat->colnames.size(); ++i) {
      const std::string& c = mat->colnames[i];
      if (c == want) return pythonFloatRepr(static_cast<double>(i + 1));
      const size_t colon = c.find(':');
      if (colon != std::string::npos && c.substr(colon + 1) == want)
        return pythonFloatRepr(static_cast<double>(i + 1));
    }
    return ".";  // a name that is not a column is MISSING, not an error
  });

  std::string out = regexSubFn(reElem, pre, [&session](const std::smatch& m) -> std::string {
    try {
      double val = getMatrixElement(session, m.str(1), std::stoll(m.str(2)), std::stoll(m.str(3)));
      return matrixElementText(val);
    } catch (const PyExc&) {
      return m.str(0);  // KeyError/ValueError — leave unchanged
    } catch (const std::exception&) {
      return m.str(0);  // stoll overflow etc. — leave unchanged
    }
  });
  out = regexSubFn(reEl, out, [&session](const std::smatch& m) -> std::string {
    try {
      double val = getMatrixElement(session, m.str(1), std::stoll(m.str(2)), std::stoll(m.str(3)));
      return matrixElementText(val);
    } catch (const PyExc&) {
      return m.str(0);  // KeyError/ValueError — leave unchanged
    } catch (const std::exception&) {
      return m.str(0);  // stoll overflow etc. — leave unchanged
    }
  });
  out = regexSubFn(reColsof, out, [&session](const std::smatch& m) -> std::string {
    const EvalMatrix* mat = session.findMatrix(m.str(1));
    if (mat == nullptr) return m.str(0);
    long long ncols = mat->data.empty() ? 0 : static_cast<long long>(mat->data[0].size());
    return pythonFloatRepr(static_cast<double>(ncols));  // repr(float(ncols))
  });
  out = regexSubFn(reRowsof, out, [&session](const std::smatch& m) -> std::string {
    const EvalMatrix* mat = session.findMatrix(m.str(1));
    if (mat == nullptr) return m.str(0);
    return pythonFloatRepr(static_cast<double>(mat->data.size()));
  });
  return out;
}

// ---------------------------------------------------------------------------
// has_macro_refs

bool hasMacroRefs(const std::string& rawLine) {
  // A3: every macro reference this function can report needs one of exactly
  // three characters to be present — a backtick (`local'), a dollar ($global,
  // ${global}), or a parenthesis (r()/e()/s() refs, whose patterns all match
  // `X(`). A line with none of them cannot contain a reference, and the seven
  // regex searches below were being run on such lines anyway. The guard is a
  // three-character scan and it can only skip work the regexes would have
  // reported false for.
  if (rawLine.find_first_of("`$(") == std::string::npos) return false;
  // P2-PERF ARC-A/A3-b: the same pre-filter argument, applied PER PATTERN
  // rather than once for all seven. The blanket guard above lets a line
  // through on any one of three characters and then runs all seven regexes,
  // so `local acc = \`acc' + \`i'` — one backtick, no dollar, no paren — paid
  // for the two global patterns, the three class-reference patterns and the
  // arithmetic-macro pattern as well as the one that could match. Each guard
  // below is strict: the pattern's own literal must be present for it to have
  // any chance of matching.
  const bool tick = rawLine.find('`') != std::string::npos;
  const bool dollar = rawLine.find('$') != std::string::npos;
  std::smatch m;
  if (tick && hasLocalMacroRef(rawLine)) return true;  // EXPRLAYER-7: was reLocalRef()
  if (dollar && std::regex_search(rawLine, m, reGlobalRefBrace())) return true;
  if (dollar && std::regex_search(rawLine, m, reGlobalRefBare())) return true;
  // EXPRLAYER-3: the guards are the patterns' own `\b` requirement, not the
  // bare two-character literal — "r(" and "e(" both occur inside ordinary
  // identifiers (`scalar(`, `subinstr(`, `true(`, `size(`), and every one of
  // those was buying a full regex scan.
  if (hasBareRParen(rawLine) && std::regex_search(rawLine, m, reRRef())) return true;
  if (hasBareEParen(rawLine) && std::regex_search(rawLine, m, reERef())) return true;
  if (tick && rawLine.find("s(") != std::string::npos &&
      std::regex_search(rawLine, m, reSRefDelim()))
    return true;
  if (rawLine.find("`=") != std::string::npos && std::regex_search(rawLine, m, reArithMacro()))
    return true;
  return false;
}

// ---------------------------------------------------------------------------
// _eval_arith (macros.py)

// A bare `.` (Stata system-missing) that is not part of a number or an
// ident.member — mirrors strmutate.cpp's replaceBareDot. Mapped to Stata's
// max-double missing sentinel so e.g. `substr(s, 3, .)` (missing length ==
// "to end of string") reaches fnSubstr as a huge length and slices to the end,
// and arithmetic on missing propagates the sentinel rather than a Python
// SyntaxError. Word-adjacency guards leave `.` inside 3.14 / a."b" untouched.
static std::string replaceBareDotMissing(const std::string& s) {
  auto isWordOrDot = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
  };
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '.') {
      bool prev = (i > 0 && isWordOrDot(s[i - 1]));
      bool next = (i + 1 < s.size() && isWordOrDot(s[i + 1]));
      if (!prev && !next) { out += "8.988465674311579e+307"; continue; }
      // BUG-249: EXTENDED missing `.a`..`.z` — a single lowercase letter with
      // non-word neighbours on both sides ( `.ab` and `x.a` and `1.e5` all
      // pass through untouched). Each code maps to its own kind-carrying
      // sentinel (exprfns.h) so string(.a) can answer `.a` instead of `.`,
      // and every existing >= sentinel missing test still fires.
      if (!prev && i + 1 < s.size() && s[i + 1] >= 'a' && s[i + 1] <= 'z' &&
          !(i + 2 < s.size() && isWordOrDot(s[i + 2]))) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", stataMissingWithKind(s[i + 1] - 'a' + 1));
        out += buf;
        ++i;  // consume the kind letter
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

// BUG-249: the way OUT of the evaluator for a kind-carrying missing — `.a`
// renders `.a`, anything else in the missing band renders `.` (including the
// negated band, which Stata shows as `.`; see isStataMissingResult below).
static std::string stataMissingText(double d) {
  auto kind = stataExtendedMissingKind(d);
  if (kind && *kind >= 1) return std::string(".") + static_cast<char>('a' + *kind - 1);
  return ".";
}

// BUG-660: evalArith's non-integer-real return uses pythonFloatRepr —
// Python's shortest-round-trip repr(), up to 17 significant digits — for
// BOTH its fast-arithmetic and extended (function-call) paths, and stays
// round-trip-exact THERE (see evalArith's own comment: `scalar s = expr`
// needs the full precision, since std::stod reads it back at full width
// downstream). Stata's own local/global `= <realexpr>` STORAGE is not
// round-trip-exact, though: it caps at 16 significant digits (verified
// against four independent w2c_functions.log macro arms with genuinely
// irrational values — sqrt_macro, normal_macro, invt/invttail/ttail's df1
// and df10 points — every one needs exactly 16 sig figs, never 17) and,
// like every other Stata real renderer in this file (see gFormat above),
// drops the leading `0` before the decimal point (`.33`, not `0.33`).
// `%.16g` already implements Python's own fixed/scientific threshold (switch
// to scientific at exponent>=precision), so this is gFormat's own "strip one
// leading 0" idiom at a different precision, not a new algorithm. Applied at
// the local/global STORE call sites via capNumericTextToStataLocalDigits
// below (BUG-660 fix round), not inside evalArith itself — a `scalar =` call
// site shares evalArith and must NOT be capped (see that function's
// evalArith(d) comment for the regression this move fixes).
//
// Fix round 2: ALSO applied to evalMacroExpr (the `` `=expr' `` twin) below,
// for the same reason -- see that function's own comment at its float-return
// arm. The original BUG-660 landing note said this surface stayed on
// pythonFloatRepr because "no oracle arm exercises it with an irrational
// value"; that was true only because postfileMathNamespace() had no Stata
// functions to BE irrational with. This same fix round widened it to
// postfileStataMathNamespace(), which does, so the gap became live.
// Review finding (v3): pythonIntStrFromDouble renders -0.0 as "-0" (%.0f),
// and -0.0 reaches the renderers below from ordinary arithmetic
// (`max(-0.4,-0.5)*0`, `-0.0`, std::round(-0.4)). StataBE 19.5 measured:
// all of these are 0 in local, global and display -- Stata has no negative
// zero. Every integer rendering in this layer goes through here.
static std::string stataIntText(double d) { return pythonIntStrFromDouble(d == 0.0 ? 0.0 : d); }

// Fix round v4 (review): the 16-significant-digit rule above was not Stata's
// rule. StataBE 19.5 measured on 35 values (local, global, `=exp' and
// `=scalar(s)' all agree): the stored text is exactly
// strtrim(string(x, "%18.0g")), i.e. at most 17 characters for the magnitude
// (plus a leading `-`):
//   - fixed point when the integer part has at most 16 digits and, for
//     |x| < 1, when the 16-decimal form keeps at least as many significant
//     digits as e-notation would (.0000333333333333 fixed; 1e-5/3 ->
//     3.33333333333e-06); trailing fraction zeros are dropped, no leading 0;
//   - otherwise e-notation with the mantissa filling the 17 characters
//     (11 decimals for a 2-digit exponent, 10 for a 3-digit one), zeros kept:
//     1e16 -> 1.00000000000e+16, 1e-300 -> 1.0000000000e-300.
// The table-driven selftest arm local_text_rule_18_0g pins every measured row.
static std::string stataLocalRealText(double d) {
  if (d == 0.0) return "0";  // also -0.0: Stata has no negative zero
  const std::string sign = d < 0 ? "-" : "";
  const double a = std::fabs(d);
  char buf[400];
  // e-notation candidate: 12 significant digits, 11 for a 3-digit exponent.
  std::snprintf(buf, sizeof(buf), "%.11e", a);
  std::string eText(buf);
  if (eText.size() > 17) {
    std::snprintf(buf, sizeof(buf), "%.10e", a);
    eText = buf;
  }
  const int eDigits = static_cast<int>(eText.find('e')) - 1;  // mantissa digits (minus the '.')
  if (a < 1.0) {
    std::snprintf(buf, sizeof(buf), "%.16f", a);
    std::string f(buf);  // "0.dddddddddddddddd" or "1.0000000000000000" after rounding
    if (f[0] == '0') {
      const size_t firstNz = f.find_first_not_of('0', 2);
      const int fixedSig = firstNz == std::string::npos ? 0 : static_cast<int>(f.size() - firstNz);
      if (fixedSig < eDigits) return sign + eText;
      f.erase(f.find_last_not_of('0') + 1);
      if (f.back() == '.') f.pop_back();
      return sign + f.substr(1);
    }
    return sign + "1";  // rounded up to 1 at 16 decimals
  }
  // |x| >= 1: integer digits k (after rounding at 16-k decimals) must be <= 16.
  std::snprintf(buf, sizeof(buf), "%.0f", std::floor(a));
  const int k = static_cast<int>(std::strlen(buf));
  if (k > 16) return sign + eText;
  std::snprintf(buf, sizeof(buf), "%.*f", 16 - k, a);
  std::string f(buf);
  const size_t dot = f.find('.');
  if ((dot == std::string::npos ? f.size() : dot) > 16) return sign + eText;
  if (dot != std::string::npos) {
    f.erase(f.find_last_not_of('0') + 1);
    if (f.back() == '.') f.pop_back();
  }
  return sign + f;
}

// BUG-660 fix round: the local/global STORAGE sites apply Stata's macro
// real-to-text rule (stataLocalRealText, %18.0g) on top of evalArith's own
// round-trip-exact text, instead of evalArith doing it for every caller (see
// evalArith's own comment on its two float-return arms). Re-parses `text`
// (evalArith's numeric output, pythonFloatRepr-formatted) and re-renders it;
// anything that does not parse as a bare finite double whole-string — a
// STRING result, a missing token (".", ".a") — is returned unchanged, since
// only a genuinely computed real needs the rule.
static std::string capNumericTextToStataLocalDigits(const std::string& text) {
  // An INTEGER rendering of at most 16 digits is already Stata's text
  // (%18.0g keeps up to 16 integer digits). Longer integers are re-rendered:
  // StataBE stores `local x = 1e16` as 1.00000000000e+16 (fix round v4).
  {
    const size_t start = (!text.empty() && text[0] == '-') ? 1 : 0;
    if (text.size() > start && text.size() - start <= 16 &&
        text.find_first_not_of("0123456789", start) == std::string::npos) {
      return text;
    }
  }
  // strtod, not std::stod: stod throws out_of_range on a subnormal (ERANGE),
  // and `local x = 5e-324` is an ordinary value (StataBE: 4.9406564584e-324).
  if (text.empty() || std::isspace(static_cast<unsigned char>(text[0]))) return text;
  char* end = nullptr;
  const double d = std::strtod(text.c_str(), &end);
  if (end != text.c_str() + text.size() || !std::isfinite(d)) return text;
  return stataLocalRealText(d);
}

// BUG-196: inside the evaluator missing is the sentinel double; on the way out
// it has to be a `.` again. MAGNITUDE, not signed value: Stata renders `-.` as
// `.` (measured — tests/kitchensink/fmt_oracle/missing_range_oracle.log, row
// `negated-dot`), and PIET's evaluator reaches that case as literally
// -kStataMissingSentinel because it negates the sentinel like any other double.
// `2*.` overflows to inf, which this also admits, and Stata agrees (row
// `dot-times-two`). NaN compares false against both bounds and is handled by
// its own paths.
//
// Used ONLY to decide whether a bare-dot retry landed on a clean missing —
// deliberately not applied to evalArith's ordinary numeric returns; see the
// retry site for why.
static bool isStataMissingResult(double d) {
  return std::fabs(d) >= kStataMissingSentinel;
}

// Review finding (v2): defined in exprfns.cpp next to runiform()'s draw
// helper; forward-declared here for the same reason addStataMacroOnlyFns is
// (piet/eval/exprfns.h is outside this task's write manifest).
void addSessionRngFns(PyNamespace& ns, EvalSession& session);

std::optional<std::string> evalArith(const std::string& expr, EvalSession* session,
                                     std::string* raisedError, bool* resultIsString) {
  dprof::EScope _z(dprof::kEEvalArith);
  dprof::ezoneBegin(dprof::kEEvalArithPre);
  std::string cleaned = strip(expr);
  // BUG-501: literals out of harm's way before any pre-pass runs (see
  // protectStringLiterals). No-op for an expression with no quote in it.
  std::vector<std::string> litSpans;
  cleaned = protectStringLiterals(cleaned, &litSpans);
  // BUG-512: `_N` / `_n` (see substituteObsSpecials). Ahead of the matrix
  // pre-pass on purpose: that pass only recognises LITERAL subscripts, and an
  // index is an expression like any other.
  if (session != nullptr) cleaned = substituteObsSpecials(cleaned, *session);
  // ^ -> ** (Stata exponentiation)
  {
    std::string tmp;
    tmp.reserve(cleaned.size() + 8);
    for (char c : cleaned) {
      if (c == '^') {
        tmp += "**";
      } else {
        tmp.push_back(c);
      }
    }
    cleaned = tmp;
  }
  // \bln\b -> log
  {
    // A3: `ln` is a two-character literal; the regex only fires when it is
    // present, so let a find() decide instead of scanning every expression.
    static const std::regex reLn(R"(\bln\b)");
    if (cleaned.find("ln") != std::string::npos)
      cleaned = std::regex_replace(cleaned, reLn, "log");
  }

  // WS-C matrix pre-substitution.
  if (session != nullptr) {
    std::string lower = toLower(cleaned);
    if (cleaned.find('[') != std::string::npos || lower.find("colsof") != std::string::npos ||
        lower.find("rowsof") != std::string::npos || lower.find("colnumb") != std::string::npos ||
        lower.find("el(") != std::string::npos) {
      cleaned = substituteMatrixRefs(cleaned, *session);
    }
  }

  if (session != nullptr) {
    std::string bErr;
    cleaned = resolveEstScalarRefs(cleaned, *session, &bErr);
    if (!bErr.empty()) {
      // BUG-456: `_b[nosuch]` is r(111) on the local/scalar/display surfaces
      // too, not a silent `.` -- forwarded on the BUG-246/249 `r(NNN): ...`
      // channel that classifyFailedEqExpr already honours.
      // BUG-456 follow-up: r(111) (unknown coefficient) or r(109) (a number
      // in a string-typed slot); splitErrCode keeps whichever the resolver
      // raised instead of stamping r(111) on both.
      if (raisedError) {
        const std::string code = splitErrCode(&bErr);
        *raisedError = code + ": " + bErr;
      }
      dprof::ezoneEnd(dprof::kEEvalArithPre);
      return std::nullopt;
    }
    // _rc — the bare last-command return code local.
    // A3: same shape — `_rc` cannot match without the literal.
    static const std::regex reRc(R"(\b_rc\b)");
    std::smatch m;
    if (cleaned.find("_rc") != std::string::npos && std::regex_search(cleaned, m, reRc)) {
      std::string rcv = session->locals.get("_rc", "0");
      if (!pythonFloat(rcv)) rcv = "0";
      cleaned = std::regex_replace(cleaned, reRc, rcv);
    }
    // Bare scalar-name substitution (insertion order; (?!\s*\() spares
    // same-named function CALLS; value inserted literally).
    dprof::EScope _zs(dprof::kEEvalArithScalars);
    for (const auto& [sn, sv] : session->scalars.items()) {
      if (cleaned.find(sn) != std::string::npos) {
        // EXPRLAYER-5: was a std::regex CONSTRUCTED (i.e. compiled) per scalar
        // per expression — `\bNAME\b(?!\s*\()` walked directly instead.
        cleaned = substituteBareName(cleaned, sn, sv);
      }
    }
    // BUG-456 follow-up (2026-09-09): the string-slot type test runs AGAIN here,
    // after the stored scalars have been substituted -- `scalar one = 1` then
    // `local sf = strpos(one,"1")` answered 1 at rc 0 where Stata is r(109)
    // (measured, oracle2.log P10), because the resolver's own copy of this test
    // ran before this loop existed to substitute anything.
    if (raisedError != nullptr && raisedError->empty()) {
      std::string badCall;
      if (stringFnNumericArgImpl(cleaned, &badCall)) {
        *raisedError = stringSlotMismatchError(badCall);
        dprof::ezoneEnd(dprof::kEEvalArithPre);
        return std::nullopt;
      }
    }
  }

  dprof::ezoneEnd(dprof::kEEvalArithPre);

  // Fast path: pure-arithmetic whitelist, EMPTY eval namespace.
  dprof::ezoneBegin(dprof::kEEvalArithWhitelist);
  const bool safeArith = isSafeArith(cleaned);  // EXPRLAYER-2: was reSafeArith()
  dprof::ezoneEnd(dprof::kEEvalArithWhitelist);
  if (safeArith) {
    PyNamespace empty;
    PyEvalOutcome out = evalPyExprDetailed(cleaned, empty);
    if (out.status != PyEvalOutcome::Status::Value) {
      // BUG-196: `display (.)` printed the literal `(.)`.
      //
      // A bare `.` is Stata system-missing and has to reach the evaluator as
      // the missing sentinel, or Python raises SyntaxError on it. That
      // substitution lived below, on the EXTENDED path only — and `.`, `(.)`
      // and `2+.` are built entirely from fast-path characters, so they took
      // THIS branch, failed, returned nullopt, and `display` fell through to
      // its echo-the-literal-text fallback. `display .` looked right the whole
      // time purely because echoing the literal text of `.` happens to be the
      // correct answer; nothing was evaluating it.
      //
      // Applied as a RETRY rather than hoisted above the whitelist fork,
      // because evalArith is a hot path (25.6% of S02's profile) and this
      // substitution allocates and walks char by char, with no cheap guard
      // available — `find('.')` does not discriminate, since ordinary decimal
      // literals contain dots. On this shape the success path pays nothing at
      // all: the only inputs that reach the retry are the ones that return
      // nullopt today, so the change is strictly additive in cost as well as
      // in behaviour.
      //
      // It is additive in behaviour because within the fast path's alphabet,
      // [0-9\s+\-*/().^eE], a bare `.` cannot occur in anything Python
      // evaluates successfully: `.` is legal there only inside a float literal
      // — which replaceBareDotMissing leaves alone, since it rewrites a `.`
      // only when neither neighbour is alnum/`_`/`.`, so `3.14`, `.5` and `1.`
      // all pass through untouched — or as attribute access, which needs an
      // identifier, and the only letters admitted here are e/E, undefined in
      // the empty namespace.
      //
      // The retry is TRUSTED ONLY WHEN IT LANDS ON A CLEAN MISSING. PIET has
      // no missing TYPE — the sentinel is an ordinary double and Python
      // arithmetic treats it as one — so an expression like `./2` comes back
      // as 4.49e+307, a number Stata would never print (Stata says `.`; row
      // `dot-div-two`). Emitting it would trade a visibly-unevaluated echo for
      // a confidently wrong number, which is the worse of the two failures. So
      // anything that is not recognisably missing falls back to the nullopt
      // this branch already returned before BUG-196, leaving those cases
      // exactly as they were. Real missing propagation belongs to a missing
      // type in the expression layer and is not claimed here.
      const std::string withMissing = replaceBareDotMissing(cleaned);
      if (withMissing == cleaned) return std::nullopt;  // no bare dot to retry with
      const PyEvalOutcome retry = evalPyExprDetailed(withMissing, empty);
      if (retry.status != PyEvalOutcome::Status::Value) return std::nullopt;
      std::optional<double> rd;
      if (retry.value.isFloat()) rd = retry.value.floatVal();
      else if (retry.value.isInt()) rd = static_cast<double>(retry.value.intVal());
      // BUG-249: kind-preserving — a bare `.e` (the one extended code whose
      // letter is inside the fast alphabet) must come back `.e`, not `.`.
      if (rd && isStataMissingResult(*rd)) return stataMissingText(*rd);
      return std::nullopt;
    }
    const PyValue& r = out.value;
    // Python: isinstance(result,(int,float)) — bool unreachable on this
    // path (no comparison chars in the whitelist).
    if (r.isInt() || r.isBool()) return std::to_string(r.intVal());
    if (r.isFloat()) {
      double d = r.floatVal();
      if (pyIsInteger(d)) return stataIntText(d);
      // BUG-660 fix round: evalArith itself stays ROUND-TRIP-EXACT
      // (pythonFloatRepr) for every caller -- `scalar s = expr` stores this
      // text straight into session.scalars, and program.cpp's return/
      // ereturn scalar and handlers_resample.cpp's exp_list both parse it
      // back with std::stod expecting the full precision. Only local/global
      // storage wants Stata's capped 16-sig-fig macro convention, applied by
      // their own call sites (see capNumericTextToStataLocalDigits below),
      // not here.
      return pythonFloatRepr(d);
    }
    return std::nullopt;
  }

  // Extended path (string fns like substr, math namespace): a bare `.` missing
  // must reach the evaluator as the Stata sentinel, not a Python SyntaxError.
  // Unconditional here — this path is not the hot one, and substr(s, 3, .)
  // depends on it (missing length == "to end of string").
  cleaned = replaceBareDotMissing(cleaned);

  dprof::ezoneBegin(dprof::kEEvalArithWhitelist);
  const bool safeArithExt = isSafeArithExt(cleaned);  // EXPRLAYER-2: was reSafeArithExt()
  dprof::ezoneEnd(dprof::kEEvalArithWhitelist);
  if (!safeArithExt) {
    return std::nullopt;  // contains characters that are unsafe
  }
  // BUG-501: the literals go back BEFORE the evaluator, which lexes them itself.
  cleaned = restoreStringLiterals(cleaned, litSpans);

  // EXPRLAYER-1c: this path added NOTHING to the registry, so it can use the
  // process-wide one directly instead of rebuilding 39 map entries per call.
  PyNamespace extNs;
  const PyNamespace* nsPtr = &stataMathNamespace();
  // BUG-660: fileexists() needs the session's cwd, which the process-wide,
  // session-less registry above cannot close over. Added as a per-call
  // override ONLY when the expression actually names it, so the
  // EXPRLAYER-1c fast path stays untouched (no map rebuild) for every other
  // extended-path call — the same "own map wins over base" lookup order
  // PyNamespace::base documents.
  //
  // Review finding (v2): runiform()/runiformint() likewise need the session
  // (its `set seed`, EvalSession::rngSeed) and get the same per-call install.
  if (session != nullptr && cleaned.find("runiform") != std::string::npos) {
    extNs.base = &stataMathNamespace();
    addSessionRngFns(extNs, *session);
    nsPtr = &extNs;
  }
  if (session != nullptr && cleaned.find("fileexists(") != std::string::npos) {
    extNs.base = &stataMathNamespace();
    const std::string cwd = session->cwd;
    extNs.fns["fileexists"] = [cwd](const std::vector<PyValue>& args) -> PyValue {
      if (args.size() != 1) throw PyExc("fileexists: TypeError arity");
      namespace fs = std::filesystem;
      std::string base = !cwd.empty() ? cwd : fs::current_path().u8string();
      fs::path p(pythonStr(args[0]));
      if (p.is_relative()) p = cecon::to_native_path(base) / p;
      std::error_code ec;
      return PyValue::boolean(fs::exists(p, ec));
    };
    nsPtr = &extNs;
  }
  PyEvalOutcome out = evalPyExprDetailed(cleaned, *nsPtr);
  // BUG-374(3): Python's math functions RAISE for an out-of-domain argument
  // (pyeval's mSqrt/mLog/mLog2/mLog10 throw "ValueError: math domain error");
  // Stata returns MISSING. `display sqrt(-1)` was reported as a parse gap and is
  // not one — the grammar handles unary minus fine (parseFactor), the call just
  // threw, and evalArith forwards only NameError/r(NNN) to the caller, so the
  // ValueError was dropped and display classified the empty error as r(198)
  // "invalid syntax". The engine's own expression evaluator already takes the
  // Stata side of this (frame.cpp's fin(): out-of-domain math becomes missing),
  // so this makes the display layer agree with it.
  if (out.status == PyEvalOutcome::Status::Raised &&
      out.error.find("math domain error") != std::string::npos) {
    return std::string(".");
  }
  if (out.status != PyEvalOutcome::Status::Value) {
    // BUG-088: distinguish a genuine raised error (an undefined name — e.g. an
    // unsupported extended macro function `_request(...)`) from a legitimately
    // non-arithmetic expression. Only a NameError is signalled to the caller; a
    // FnObject, a SyntaxError (`foo bar` trailing tokens), or a bad-arity/other
    // failure stays a silent nullopt so ordinary display output is byte-identical.
    // BUG-246/249 widen the channel: an `r(NNN): message` error is a function
    // announcing Stata's EXACT code (fnString's r(109) type mismatch is the
    // first) — forwarded whole, same contract as the NameError arm.
    if (raisedError && out.status == PyEvalOutcome::Status::Raised &&
        (out.error.rfind("NameError", 0) == 0 || out.error.rfind("r(", 0) == 0)) {
      *raisedError = out.error;
    }
    return std::nullopt;
  }
  const PyValue& r = out.value;
  // Stata fns return Python bools; coerce to 1/0 BEFORE numeric handling.
  if (r.isBool()) return r.boolVal() ? "1" : "0";
  if (r.isInt()) return std::to_string(r.intVal());
  if (r.isFloat()) {
    double d = r.floatVal();
    // BUG-249: a result still in the missing band comes back as its missing
    // TOKEN, kind preserved. Before the kind sentinels existed, `.a` reached
    // Python as a SyntaxError and fell back to the caller's literal echo —
    // rewriting it into a finite double (replaceBareDotMissing above) without
    // this gate would have PRINTED that double (`local x = .a` storing
    // 8.99e+307), a confident wrong number replacing a visible non-answer.
    // Also catches what was already leaking here: `sqrt(4)+.` used to render
    // the sentinel itself as 8.988465674311579e+307, where Stata says `.` —
    // any value at or beyond the sentinel is missing BY CONSTRUCTION in this
    // engine's own representation. Bounded change: doubles below 2^1023 are
    // untouched, and no real computation lands there (Stata's own numeric
    // ceiling is the same sentinel).
    if (isStataMissingResult(d)) return stataMissingText(d);
    // BUG-374(2): NaN is Stata MISSING and prints as ".", not "nan". It reaches
    // here from any legitimately-missing result — `display mod(7,0)` is the
    // reported case — and slipped through because isStataMissingResult is
    // magnitude-based (its own comment says NaN escapes it) and pyIsInteger
    // rejects non-finite, so the value fell to pythonFloatRepr, which is
    // faithfully Python and answers "nan". The `` `=expr' `` twin in this same
    // file has always had this arm (see evalMacroExpr's `if (std::isnan(fval))
    // return "."`), so the two expression entry points disagreed about what a
    // missing result looks like.
    if (std::isnan(d)) return ".";
    if (pyIsInteger(d)) return stataIntText(d);
    // BUG-660 fix round: round-trip-exact here too — see the fast-path arm's
    // comment above.
    return pythonFloatRepr(d);
  }
  if (r.isStr()) {
    if (resultIsString != nullptr) *resultIsString = true;  // BUG-293
    return r.strVal();
  }
  return std::nullopt;
}

// BUG-660 fix round: addStataMacroOnlyFns is defined in exprfns.cpp (this
// TU's namespace-mate) but not declared in piet/eval/exprfns.h — that header
// is outside this task's write manifest. Same signature as the definition;
// forward-declared locally rather than left unreachable from here.
void addStataMacroOnlyFns(PyNamespace& ns);

// BUG-660: evalMacroExpr's (the `` `=expr' `` inline form) own combined
// registry — mathNamespace(true) (postfile semantics, inf/nan bound) +
// addStataFns(), the same pairing exprfns.cpp's stataMathNamespace() builds
// for evalArith's withInfNan=false base. Before this, `local x = round(...)`
// and the inline `` `=round(...)' `` disagreed on whether round()/scalar()/
// invttail() etc. existed at all — postfileMathNamespace() alone has none of
// them, so every Stata-function call through the inline form was a
// NameError -> r(133) no matter what the function was. Static/process-wide
// for the same EXPRLAYER-1c reason stataMathNamespace() is: rebuilding
// ~115 fn-map entries on every `` `=expr' `` call would undo the whole
// optimization.
static const PyNamespace& postfileStataMathNamespace() {
  static const PyNamespace kNs = [] {
    PyNamespace n = mathNamespace(/*withInfNan=*/true);
    addStataFns(n);
    addStataMacroOnlyFns(n);  // BUG-660 fix round: sum/group/runiform(int) here only
    return n;
  }();
  return kNs;
}

// ---------------------------------------------------------------------------
// eval_macro_expr (postfile.py)

std::optional<std::string> evalMacroExpr(const std::string& expr, EvalSession& session) {
  dprof::EScope _z(dprof::kEEvalMacroExpr);
  std::string cleaned = strip(expr);
  if (cleaned.empty()) return std::nullopt;
  // BUG-512: `` `=_N' `` / `` `=_N-1' `` (w2_smalls.log inline_N 12|11). The
  // name passed this function's whitelist and then raised in the evaluator, so
  // the reference was left in the line and expandMacros reported a circular
  // definition after 16 passes -- `forvalues i = 1/`=_N'` died on it.
  cleaned = substituteObsSpecials(cleaned, session);

  {
    std::string tmp;
    tmp.reserve(cleaned.size() + 8);
    for (char c : cleaned) {
      if (c == '^') {
        tmp += "**";
      } else {
        tmp.push_back(c);
      }
    }
    cleaned = tmp;
  }
  {
    // A3: `ln` is a two-character literal; the regex only fires when it is
    // present, so let a find() decide instead of scanning every expression.
    static const std::regex reLn(R"(\bln\b)");
    if (cleaned.find("ln") != std::string::npos)
      cleaned = std::regex_replace(cleaned, reLn, "log");
  }

  {
    std::string lower = toLower(cleaned);
    if (cleaned.find('[') != std::string::npos || lower.find("colsof") != std::string::npos ||
        lower.find("rowsof") != std::string::npos || lower.find("el(") != std::string::npos) {
      cleaned = substituteMatrixRefs(cleaned, session);
    }
  }

  // Namespace: postfile._MATH_NS (has inf/nan) + session scalars + r()
  // values as bare float names. dict-update order means a scalar named
  // after a function SHADOWS the function (consts win in evalPyExpr).
  // EXPRLAYER-1c: registry by reference; the per-call scalar/r() bindings go
  // into the own map, which outranks the base — the same shadowing the
  // dict-update order produced ("a scalar named after a function SHADOWS the
  // function": a const always outranks a fn, at any level).
  PyNamespace ns;
  ns.base = &postfileStataMathNamespace();  // BUG-660: was postfileMathNamespace() (no Stata fns)
  // Review finding (v2): session-bound runiform()/runiformint() (see
  // evalArith's own install for the rationale).
  if (expr.find("runiform") != std::string::npos) addSessionRngFns(ns, session);
  for (const auto& [sname, sval] : session.scalars.items()) {
    auto f = pythonFloat(sval);
    if (f) ns.consts[sname] = PyValue::floating(*f);
  }
  for (const auto& [rname, rval] : session.rValues.items()) {
    ns.consts[rname] = PyValue::floating(rval);
  }

  // Whitelist on the SPACE-stripped text (spaces only — not tabs; mirrors
  // cleaned.replace(" ", "")).
  {
    std::string nospace;
    nospace.reserve(cleaned.size());
    for (char c : cleaned) {
      if (c != ' ') nospace.push_back(c);
    }
    std::smatch wm;
    if (!std::regex_match(nospace, wm, rePostfileSafeArith())) return std::nullopt;
  }

  PyEvalOutcome out = evalPyExprDetailed(cleaned, ns);
  if (out.status == PyEvalOutcome::Status::Raised) return std::nullopt;
  if (out.status == PyEvalOutcome::Status::FnObject) {
    // Python: `return str(result)` — a bare builtin name evaluates to the
    // function object; str() of the math builtins is deterministic.
    return "<built-in function " + out.fnName + ">";
  }
  const PyValue& r = out.value;
  // bool -> int, then int/float formatting.
  if (r.isBool()) return r.boolVal() ? "1" : "0";
  if (r.isInt() || r.isFloat()) {
    double fval = r.isFloat() ? r.floatVal() : static_cast<double>(r.intVal());
    if (std::isnan(fval)) return ".";
    // Python: `if fval == int(fval) and not isinf(fval)` — int(inf) raises
    // OverflowError BEFORE the isinf guard, and the whole body sits in a
    // broad try/except -> ±inf yields None (the guard is dead code).
    if (std::isinf(fval)) return std::nullopt;
    // str(int(fval)), no "-0"; 17+ integer digits take Stata's e-notation
    // (fix round v4: `=1e16' is 1.00000000000e+16 in StataBE, fmt2.log).
    if (pyIsInteger(fval) && std::fabs(fval) < 1e16) return stataIntText(fval);
    // BUG-660 fix round 2: was pythonFloatRepr (Python's 17-sig-fig
    // round-trip repr) unconditionally -- fine for a plain arithmetic
    // expression, but this namespace now also reaches Stata's own functions
    // (round/normal/invttail/... -- postfileStataMathNamespace() above), and
    // Stata renders THEIR results the same 16-sig-fig, no-leading-zero way
    // local/global do (stataLocalRealText, applied to that surface via
    // capNumericTextToStataLocalDigits). Before this namespace widening these
    // calls were a NameError this function turned into std::nullopt (left
    // unexpanded); giving them Python-repr precision now would be a NEW
    // silent wrong answer, not the honest refusal they used to be.
    return stataLocalRealText(fval);
  }
  if (r.isStr()) return r.strVal();  // unreachable (whitelist has no quotes)
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// _expand_pass

ExpandResult expandPass(const std::string& lineIn, EvalSession& session,
                        bool unsetRAsMissing) {
  dprof::EScope _z(dprof::kEExpandPass);
  std::string line = lineIn;

  auto errorBlock = [](const std::string& message) {
    ExpandResult r;
    r.isError = true;
    r.error = nlohmann::ordered_json{
        {"type", "error"}, {"code", "engine_error"}, {"message", message}};
    return r;
  };


  // A3 pre-filters. Each block below opens with a full regex scan of the line;
  // every one of these constructs requires a literal character that a memchr-
  // class find() settles first (a backtick for `local'/`=expr'/`r()'/`s()', a
  // dollar for $global, `e(`/`r(` for the bare class refs). A profile of a pure
  // macro loop put ~18% of samples in libc++'s regex engine and ~40% in
  // malloc/free — largely that engine's own per-execution state vectors — so
  // the scan that never runs is the one worth removing. Strict pre-filters:
  // a pattern cannot match text that lacks its own literal.
  // Recomputed at each site, never hoisted: a substitution can INTRODUCE a
  // marker the line did not have (a local whose VALUE contains `$G` puts a
  // dollar into the line during the local pass, and the bare-global pass below
  // must still see it). find() is a memchr-class scan, so paying it per site is
  // still orders of magnitude below the regex it guards.

  // --- `=expr' arithmetic-macro form — runs FIRST ---
  // P2-PERF ARC-A/A3-b: the guard is "`=", not "`". The pattern is `=…' and a
  // bare backtick — every `local' reference in the language — cannot match it,
  // so the ordinary macro loop was paying a full regex scan per line to learn
  // that.
  for (const auto& [full, inner] : line.find("`=") != std::string::npos
                                       ? findAll(reArithMacro(), line)
                                           : std::vector<std::pair<std::string, std::string>>{}) {
    std::smatch m;
    if (!std::regex_search(inner, m, reLocalRef()) &&
        !std::regex_search(inner, m, reGlobalRefBrace()) &&
        !std::regex_search(inner, m, reGlobalRefBare()) &&
        !std::regex_search(inner, m, reRRef()) && !std::regex_search(inner, m, reERef())) {
      auto result = evalMacroExpr(inner, session);
      if (result) line = replaceFirst(line, full, *result);
    }
  }

  // --- e() scalar references (WS-C) ---
  // Substitute ONLY names present in the estat-posted _e_scalars store;
  // anything else stays for the display-path resolver.
  // BUG-507: `r(name)`/`e(name)` in their BARE form are NOT macros -- Stata
  // resolves them in the EXPRESSION evaluator, so a double-quoted literal never
  // sees one: `strpos("r(mean)", "mean")` is 3 there and was 0 here, the literal
  // having become its own value before the expression was ever evaluated
  // (tests/eval/golden/aer_w1/w1b_quoteblind.log QB|rclass|3| / QB|eclass|3|).
  //
  // The DELIMITED forms are the opposite case and are NOT protected: `` `r(name)' ``
  // IS macro expansion, Stata expands it inside double quotes, and that is how a
  // varlist gets spliced into a command -- selftest BUG-374(1) holds
  // `display "x: `r(levels)'"` to exactly that and caught a first version of this
  // fix which protected both forms at once.
  //
  // Protection rather than a position-aware skip because the substitution is
  // text-based: `replaceFirst(line, full, value)` rewrites the FIRST occurrence of
  // the matched TEXT, so with `"r(mean)" + string(r(mean))` it would hit the quoted
  // one even when the match that drove it was not.
  {
    std::smatch m;
    if (hasBareEParen(line) && std::regex_search(line, m, reERef())) {  // EXPRLAYER-3
      std::vector<std::string> eSpans;
      line = protectStringLiterals(line, &eSpans);
      for (const auto& [full, ename] : findAll(reERef(), line)) {
        const double* v = session.eScalars.find(ename);
        if (v != nullptr) {
          line = replaceFirst(line, full, pythonFloatRepr(*v));  // repr(float(...))
        }
      }
      line = restoreStringLiterals(line, eSpans);
    }
  }

  // --- r() references: delimited form `r(name)' FIRST, then bare r(name).
  // String r() (from `return local`) takes precedence over numeric.
  // EXPRLAYER-3: one guard per pattern, each the literal that pattern requires
  // — the delimited form needs "`r(", the bare form needs an `r` at a word
  // boundary followed by '('. The old single `find("r(")` let `scalar(`,
  // `subinstr(` and every other identifier ending in r through to two full
  // regex scans. The guard is recomputed per pass because the delimited pass
  // rewrites the line.
  dprof::ezoneBegin(dprof::kEExpandRBlock);
  const bool matrixVerbLine = isMatrixVerbLine(line);
  for (const std::regex* rx : {&reRRefDelim(), &reRRef()}) {
    const bool reachable = (rx == &reRRefDelim()) ? line.find("`r(") != std::string::npos
                                                  : hasBareRParen(line);
    if (!reachable) continue;
    // BUG-507: the BARE pass only (see the e() block above for why, and why the
    // delimited pass must NOT be protected).
    std::vector<std::string> rSpans;
    const bool barePass = (rx == &reRRef());
    if (barePass) line = protectStringLiterals(line, &rSpans);
    struct RRestore {
      std::string* l;
      const std::vector<std::string>* sp;
      bool on;
      ~RRestore() {
        if (on) *l = restoreStringLiterals(*l, *sp);
      }
    } rRestore{&line, &rSpans, barePass};
    for (const auto& [full, name] : findAll(*rx, line)) {
      // B4-W3b: on a MATRIX verb, an r-class matrix reference is left in
      // place, exactly as the e() layer below leaves e(b) for the matrix layer
      // ("splicing it here would take e(b) away from the matrix layer"). It was
      // not: `matrix T = r(table)` — sktest's own posted matrix, which
      // handlers_m3.cpp:1978 already knows how to copy — died HERE with
      // "r(table) is not set", because this loop only ever looked in
      // rStrings/rValues and then declared the name unknown; nothing
      // downstream could see the reference to answer it.
      //
      // Keyed on the VERB, not on the name alone, so the change reaches
      // exactly the surface that can consume a matrix. `display r(table)` is
      // still this loop's to refuse, and keeps the refusal it always gave —
      // widening the exemption to every line would have swapped that for a
      // stranger message on a line the matrix layer cannot help either.
      if (rx == &reRRef() && matrixVerbLine && session.findClassMatrix('r', name) != nullptr)
        continue;
      // BUG-560: the same exemption for el()'s matrix argument on ANY verb --
      // `local F1=el(r(singleresults),1,4)` (paper 044) is answered by
      // substituteMatrixRefs' reClsEl, which only ever sees the reference if
      // this loop leaves it in place. Keyed on the posted matrix AND the
      // el(r(NAME), spelling, so a bare `display r(table)` keeps its refusal.
      if (rx == &reRRef() && session.findClassMatrix('r', name) != nullptr &&
          std::regex_search(line, std::regex(R"(\bel\s*\(\s*r\()" + name + R"(\)\s*,)",
                                             std::regex::icase)))
        continue;
      const std::string* s = session.rStrings.find(name);
      if (s != nullptr) {
        // BUG-374(1): the two spellings mean different things and used to share
        // this arm.
        //   `r(name)'  is MACRO EXPANSION — raw text substitution, Stata's own
        //              semantics, and it must stay raw (it is how a varlist gets
        //              spliced into a command).
        //   r(name)    is a VALUE reference in an EXPRESSION, so a string value
        //              has to arrive as a string LITERAL.
        // Splicing it raw made `display r(...)` re-evaluate the text, which
        // failed loudly for a value the arithmetic grammar cannot parse
        // (r(datasignature): "336:34(...)" — ':' is outside the whitelist) and,
        // worse, could go SILENT: a single-word value such as r(varlist) with one
        // variable resolved as a DATASET VARIABLE and printed that column's first
        // observation instead of the name.
        if (rx == &reRRef()) {
          std::string lit = "\"";
          for (char ch : *s) {
            if (ch == '"') lit += "\\\"";
            else lit += ch;
          }
          lit += "\"";
          line = replaceFirst(line, full, lit);
        } else {
          line = replaceFirst(line, full, *s);
        }
      } else if (const double* v = session.rValues.find(name)) {
        line = replaceFirst(line, full, pythonFloatRepr(*v));  // repr(float(val))
      } else if (unsetRAsMissing) {
        // Stata: an unset r() in a numeric expression is missing (`.`). Used by
        // the if/while condition path so `!missing(r(estimate))` guards work
        // after a failed capture (the `.` becomes the missing sentinel in
        // evalBoolExpr's replaceBareDot). No error — this is legal Stata.
        line = replaceFirst(line, full, ".");
      } else {
        dprof::ezoneEnd(dprof::kEExpandRBlock);
        return errorBlock("r(" + name + ") is not set — run an r-class command "
                           "(e.g. `summarize varname`) first.");
      }
    }
  }
  dprof::ezoneEnd(dprof::kEExpandRBlock);

  // --- e() references: the DELIMITED form `e(name)' only (BUG-409). The BARE
  // form e(name) is an EXPRESSION reference and belongs to resolveEstScalarRefs
  // further down, which knows to leave a matrix name alone; splicing it here
  // would take e(b) away from the matrix layer. Strings win over scalars, as
  // they do for r(): `e(cmd)` is a macro, `e(N)` is a number, and the store a
  // name lives in is what decides.
  if (line.find("`e(") != std::string::npos) {
    for (const auto& [full, name] : findAll(reERefDelim(), line)) {
      if (const std::string* es = session.eStrings.find(name)) {
        line = replaceFirst(line, full, *es);
      } else if (const double* ev = session.eScalars.find(name)) {
        line = replaceFirst(line, full, pythonFloatRepr(*ev));
      } else {
        // Stata macro semantics: an unset e() macro reference expands to the
        // empty string at rc 0 — the same rule BUG-294 measured for an
        // undefined local, and the reason `if "`e(nosuch)'" == ""` works.
        const std::string sub = resolveEScalar(name, session);
        line = replaceFirst(line, full, sub == "." ? std::string() : sub);
      }
    }
  }

  // --- s() references (P4, sreturn local): delimited form `s(name)' only —
  // same site as r()/e() just above. Values live in session.rStrings (the
  // same store r()-strings use) under kSClassKeyPrefix so they can never
  // collide with a real r()-name (those are captured by an identifier-only
  // regex — reRRefDelim/reRRef above — so no r()-set key can ever equal our
  // prefixed form). Mirrors r()'s own unset-reference handling exactly,
  // including the unsetRAsMissing if/while-condition carve-out.
  for (const auto& [full, name] : (line.find('`') != std::string::npos &&
                                   line.find("s(") != std::string::npos)
                                     ? findAll(reSRefDelim(), line)
                                     : std::vector<std::pair<std::string, std::string>>{}) {
    const std::string* s = session.rStrings.find(std::string(kSClassKeyPrefix) + name);
    if (s != nullptr) {
      line = replaceFirst(line, full, *s);
    } else if (unsetRAsMissing) {
      line = replaceFirst(line, full, ".");
    } else {
      return errorBlock("s(" + name + ") is not set — run an sclass program's "
                         "`sreturn local` first.");
    }
  }

  // --- local macro references: `name' ---
  //
  // P2-PERF ARC-A/A3-②: ONE left-to-right splice into one output buffer, in
  // place of a regex scan that collected every match plus a replaceFirst per
  // match — and replaceFirst built a whole new string each time
  // (`s.substr(0,pos) + to + s.substr(pos+n)`), so `local acc = `acc' + `i''
  // allocated three strings to substitute two macros. This is the single
  // hottest text operation in the interpreter: it runs on every line of every
  // loop body.
  //
  // The name grammar is reLocalRef()'s, transcribed rather than approximated:
  // an identifier ([A-Za-z_][A-Za-z0-9_]*) or a positional digit run ([0-9]+),
  // closed by an apostrophe. Anything else after a backtick is not a reference,
  // and — exactly like the regex iterator — scanning resumes one character
  // past that backtick, so a nested `` `x' `` inside a non-reference is still
  // found.
  //
  // Ordering note, deliberate and equivalent: the old code searched the
  // PROGRESSIVELY MODIFIED line for each match's literal text, so a macro whose
  // VALUE happened to contain another reference could have that inner reference
  // consumed in this same pass. The splicer never rescans a substituted value.
  // Both converge to the same line, because expandMacros loops until no
  // reference remains — only the intermediate differs.
  if (line.find('`') != std::string::npos) {
    std::string out;
    out.reserve(line.size() + 32);
    size_t i = 0;
    bool spliced = false;
    while (i < line.size()) {
      const size_t tick = line.find('`', i);
      if (tick == std::string::npos) {
        out.append(line, i, std::string::npos);
        i = line.size();
        break;
      }
      size_t j = tick + 1;
      const size_t nameStart = j;
      if (j < line.size() &&
          (std::isalpha(static_cast<unsigned char>(line[j])) || line[j] == '_')) {
        ++j;
        while (j < line.size() &&
               (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_')) {
          ++j;
        }
      } else if (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) {
        ++j;
        while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) ++j;
      }
      if (j == nameStart || j >= line.size() || line[j] != '\'') {
        out.append(line, i, tick + 1 - i);  // not a reference: copy through the backtick
        i = tick + 1;
        continue;
      }
      const std::string name = line.substr(nameStart, j - nameStart);
      const std::string* v = session.locals.find(name);
      // BUG-294: an UNDEFINED macro reference expands to the empty string at
      // rc 0 — measured against Stata 19.5 IC (`display "[\`nosuchlocal']"` ->
      // `[]`, _rc 0; the global arms below likewise). PIET used to hard-error
      // and STOP the do-file here, which turned BUG-216/246/247's deliberate
      // leave-unset-on-error into a downstream halt the oracle does not have,
      // and derailed the `capture` + inspect idiom real do-files are written
      // in. Nothing downstream is weakened: an empty expansion that leaves a
      // syntactically broken line still errors, on the line Stata errors on.
      static const std::string kEmpty;
      if (v == nullptr) v = &kEmpty;
      out.append(line, i, tick - i);
      out += *v;
      i = j + 1;
      spliced = true;
    }
    if (spliced) {
      if (i < line.size()) out.append(line, i, std::string::npos);
      line = std::move(out);
    }
  }

  // --- global macro references: ${name} then $name ---
  for (const auto& [full, name] : line.find('$') != std::string::npos
                                      ? findAll(reGlobalRefBrace(), line)
                                      : std::vector<std::pair<std::string, std::string>>{}) {
    const std::string* v = session.globals.find(name);
    static const std::string kEmpty;  // BUG-294 — see the local arm above.
    if (v == nullptr) v = &kEmpty;
    line = replaceFirst(line, full, *v);
  }
  for (const auto& [full, name] : line.find('$') != std::string::npos
                                      ? findAll(reGlobalRefBare(), line)
                                      : std::vector<std::pair<std::string, std::string>>{}) {
    const std::string* v = session.globals.find(name);
    static const std::string kEmpty;  // BUG-294 — see the local arm above.
    if (v == nullptr) v = &kEmpty;
    line = replaceFirst(line, full, *v);
  }

  ExpandResult r;
  r.line = line;
  return r;
}

// ---------------------------------------------------------------------------
// expand_macros

ExpandResult expandMacros(const std::string& rawLine, EvalSession& session,
                          bool unsetRAsMissing) {
  std::string line = rawLine;

  for (int pass = 0; pass < kMaxExpansionPasses; ++pass) {
    if (!hasMacroRefs(line)) break;  // fully resolved
    ExpandResult result = expandPass(line, session, unsetRAsMissing);
    if (result.isError) return result;  // propagate error immediately
    if (result.line == line) break;     // no progress — stuck
    line = result.line;
  }

  // Compound double-quotes `"..."' are STRING delimiters — strip to plain
  // "..." so the tokenizer doesn't mistake the ` for an unexpanded macro.
  if (line.find("`\"") != std::string::npos) {
    static const std::regex reCompound(R"re(`"(.*?)"')re");
    line = std::regex_replace(line, reCompound, "\"$1\"");
  }

  std::string lineNoEBuf;  // storage for the stripped copy, when one is needed
  // Convergence guard — e() refs not in _e_scalars are NOT a failure (the
  // display-path resolver owns them); strip them before the check.
  //
  // P2-PERF ARC-A/A3-b: this regex_replace ran on EVERY expanded line,
  // unguarded — the one regex execution a line with no macro references at all
  // could not avoid. `local q = 5` never enters the pass loop (hasMacroRefs is
  // false on the first look) and still paid for it. A line with no "e(" cannot
  // contain an e() reference, so the strip is the identity and the check can
  // read the line itself.
  // BUG-507: a class reference inside a string literal is TEXT, and the splices
  // above now leave it -- so the guard reads a copy with the parentheses inside
  // literals blanked. Macro references there stay visible on purpose.
  const std::string lineMasked = maskParensInQuotes(line);
  const std::string& lineNoE = hasBareEParen(lineMasked)  // EXPRLAYER-3
                                   ? (lineNoEBuf = std::regex_replace(lineMasked, reERef(), ""))
                                   : lineMasked;
  // B4-W3b: the SAME exemption for an r-class MATRIX reference. The bare-r()
  // pass above deliberately leaves `r(table)` / `r(Stat1)` in place for the
  // matrix layer (see its comment), so without this the guard would read an
  // intentionally-unexpanded reference as a runaway macro and report a
  // circular definition for a line that is simply not the macro layer's to
  // finish. Modelled on the e() strip immediately above, and as narrow: only
  // names that ARE posted r-matrices are removed.
  std::string lineNoRBuf;
  const std::string* checkLine = &lineNoE;
  if (hasBareRParen(*checkLine) && !session.rMatrices.empty() && isMatrixVerbLine(line)) {
    lineNoRBuf = *checkLine;
    for (const auto& [nm, mtx] : session.rMatrices) {
      (void)mtx;
      const std::string needle = "r(" + nm + ")";
      size_t pos = 0;
      while ((pos = lineNoRBuf.find(needle, pos)) != std::string::npos) {
        lineNoRBuf.erase(pos, needle.size());
      }
    }
    checkLine = &lineNoRBuf;
  } else if (hasBareRParen(*checkLine) && !session.rMatrices.empty() &&
             checkLine->find("el(") != std::string::npos) {
    // BUG-560: the bare-r() pass also leaves el()'s matrix argument
    // (`el(r(table),1,4)`) for substituteMatrixRefs. Strip only that
    // spelling, so any other r() left on the line still reads as unexpanded.
    lineNoRBuf = *checkLine;
    for (const auto& [nm, mtx] : session.rMatrices) {
      (void)mtx;
      lineNoRBuf = std::regex_replace(
          lineNoRBuf, std::regex(R"(\bel\s*\(\s*r\()" + nm + R"(\))", std::regex::icase), "el(");
    }
    checkLine = &lineNoRBuf;
  }
  if (hasMacroRefs(*checkLine)) {
    ExpandResult r;
    r.isError = true;
    r.error = nlohmann::ordered_json{
        {"type", "error"},
        {"code", "engine_error"},
        {"message",
         "Macro expansion did not converge after " + std::to_string(kMaxExpansionPasses) +
             " passes. This usually indicates a self-referential or circular macro "
             "definition. Line after expansion: " + pyRepr(line)}};
    return r;
  }

  ExpandResult r;
  r.line = line;
  return r;
}

// ---------------------------------------------------------------------------
// _try_extended_macro_fn

namespace {

// BUG-216/217 helpers: the variable-metadata plane SessionState projects into
// eval.varLabels under reserved keys (see SessionState::syncEvalVarLabels).
//
// MUST stay byte-identical to session_state.h's kEvalVar*Prefix constants — the
// same discipline kSClassKeyPrefix above documents for the s-class store, and for
// the same reason: two spellings of one key is a silent-miss generator.
const char* const kVarExistsPrefix = "\x01v:x:";
const char* const kVarFormatPrefix = "\x01v:f:";
const char* const kVarLblsetPrefix = "\x01v:l:";
const char* const kVarTypePrefix = "\x01v:t:";  // BUG-250: declared storage subtype
// The AUTHORITY sentinel. Its presence means "a SessionState has projected this
// plane, so absence of a variable's exists-key really does mean the variable does
// not exist". Absence means no SessionState ever synced — the evalcase harness
// and the session-less selftest callers, which write eval.varLabels by hand — and
// there r(111) is UNDECIDABLE, exactly as it was before this fix. So the reject
// is gated on it and the fallback is the pre-fix lenient answer: one-sided by
// construction, and it cannot turn a harness fixture into an error.
const char* const kVarPlaneKey = "\x01v:@";

bool varPlaneIsAuthoritative(const EvalSession& session) {
  return session.varLabels.find(std::string(kVarPlaneKey)) != nullptr;
}
// true = the variable is known ABSENT (and only ever true when the plane is
// authoritative, so a session-less caller never reaches an r(111)).
bool varKnownMissing(const EvalSession& session, const std::string& name) {
  if (!varPlaneIsAuthoritative(session)) return false;
  return session.varLabels.find(std::string(kVarExistsPrefix) + name) == nullptr;
}

std::optional<std::string> tryExtendedMacroFn(const std::string& rhsIn, EvalSession& session,
                                              MacroFnError* err) {
  std::string rhs = strip(rhsIn);
  if (rhs.empty() || rhs[0] != ':') return std::nullopt;  // not a function form

  // BUG-217: `variable X not found` + r(111) — Stata's own wording and code for
  // every one of these readers, captured this stint (StataNow 19.5, `capture
  // noisily`, in-log marker): `: variable label nosuchvar` and
  // `: format nosuchvar` BOTH print "variable nosuchvar not found" at r(111).
  auto rejectMissingVar = [&](const std::string& name) {
    if (err != nullptr) {
      err->rc = "111";
      err->message = "variable " + name + " not found";
    }
  };

  std::smatch m;

  // word count of <list>
  static const std::regex reWordCount(R"(^:?\s*word\s+count\s+(?:of\s+)?(.*)$)",
                                       std::regex::icase);
  if (std::regex_match(rhs, m, reWordCount)) {
    std::string lst = strip(m.str(1));
    auto words = lst.empty() ? std::vector<std::string>{} : pySplit(lst);
    return std::to_string(words.size());
  }

  // word N of <list>
  static const std::regex reWordN(R"(^:?\s*word\s+(\d+)\s+of\s+(.*)$)", std::regex::icase);
  if (std::regex_match(rhs, m, reWordN)) {
    long long n = 0;
    try {
      n = std::stoll(m.str(1));
    } catch (const std::exception&) {
      return std::string();  // absurd index -> out of range -> ""
    }
    std::string lst = strip(m.str(2));
    auto words = lst.empty() ? std::vector<std::string>{} : pySplit(lst);
    if (1 <= n && n <= static_cast<long long>(words.size())) {
      return words[static_cast<size_t>(n - 1)];
    }
    return std::string();  // out of range -> empty (Stata-faithful)
  }

  // length local <name>  |  length "<str>"
  static const std::regex reLength(R"re(^:?\s*length\s+(?:local\s+([A-Za-z_][A-Za-z0-9_]*)|"([^"]*)")$)re",
                                    std::regex::icase);
  if (std::regex_match(rhs, m, reLength)) {
    if (m[1].matched) {
      return std::to_string(utf8Len(session.locals.get(m.str(1), "")));
    }
    return std::to_string(utf8Len(m.str(2)));
  }

  // subinstr local <name> "<old>" "<new>"[, all]
  static const std::regex reSubinstrLocal(
      R"re(^:?\s*subinstr\s+local\s+([A-Za-z_][A-Za-z0-9_]*)\s+"([^"]*)"\s+"([^"]*)"(?:\s*,\s*all)?$)re",
      std::regex::icase);
  if (std::regex_match(rhs, m, reSubinstrLocal)) {
    std::string val = session.locals.get(m.str(1), "");
    std::string oldS = m.str(2);
    std::string newS = m.str(3);
    // ,all searched on the WHOLE rhs (Python quirk — bug-for-bug).
    static const std::regex reAll(R"(,\s*all)", std::regex::icase);
    std::smatch am;
    bool allFlag = std::regex_search(rhs, am, reAll);
    if (oldS.empty()) {
      // Python str.replace("", new): inserts `new` at every position
      // (between chars and at both ends); with count=1, only at the start.
      // Codepoint boundaries, like Python.
      if (!allFlag) return newS + val;
      std::string out = newS;
      size_t i = 0;
      while (i < val.size()) {
        size_t step = 1;
        unsigned char c = static_cast<unsigned char>(val[i]);
        if (c >= 0xF0) step = 4;
        else if (c >= 0xE0) step = 3;
        else if (c >= 0xC0) step = 2;
        out += val.substr(i, step) + newS;
        i += step;
      }
      return out;
    }
    if (allFlag) {
      // replace all occurrences
      std::string out;
      size_t pos = 0;
      while (true) {
        size_t hit = val.find(oldS, pos);
        if (hit == std::string::npos) break;
        out.append(val, pos, hit - pos);
        out.append(newS);
        pos = hit + oldS.size();
      }
      out.append(val, pos, val.size() - pos);
      return out;
    }
    return replaceFirst(val, oldS, newS);
  }

  // list uniq <name>
  static const std::regex reListUniq(R"(^:?\s*list\s+uniq\s+([A-Za-z_][A-Za-z0-9_]*)$)",
                                      std::regex::icase);
  if (std::regex_match(rhs, m, reListUniq)) {
    auto words = pySplit(session.locals.get(m.str(1), ""));
    std::vector<std::string> seen;
    for (const auto& w : words) {
      if (std::find(seen.begin(), seen.end(), w) == seen.end()) seen.push_back(w);
    }
    std::string out;
    for (size_t i = 0; i < seen.size(); ++i) {
      if (i) out += " ";
      out += seen[i];
    }
    return out;
  }

  // sizeof <name>
  static const std::regex reSizeof(R"(^:?\s*sizeof\s+([A-Za-z_][A-Za-z0-9_]*)$)",
                                    std::regex::icase);
  if (std::regex_match(rhs, m, reSizeof)) {
    std::string val = session.locals.get(m.str(1), "");
    return std::to_string(val.empty() ? 0 : pySplit(val).size());
  }

  // variable label <varname> — NOTE: requires the leading ':' (^: not ^:?).
  //
  // BUG-208: `session.varLabels` is NOT a store this layer's callers are expected
  // to have filled by hand. Until that fix its only writer in the whole tree was
  // the evalcase test harness, so under the live dispatcher it was a permanently
  // empty map and this returned "" for a label the session had just set and
  // `describe` was displaying — a legal value, indistinguishable from "no label",
  // and `if "\`l'" == ""` is the idiomatic use, so do-files branched wrong in
  // silence. It is now a PROJECTION of SessionState's live varMeta/resident card,
  // refreshed by SessionState::syncEvalVarLabels() at the dispatcher call site
  // just above tryCaptureDefinition. This layer stays as it was, and deliberately:
  // piet::eval sits BELOW SessionState and must not learn about it.
  //
  // The "" return therefore now means what it says for a variable that EXISTS and
  // has no label, which is Stata-faithful.
  //
  // BUG-217 closes the other half: for a variable that does NOT exist this used to
  // return "" as well, conflating "no label" with "no variable", so a loop over a
  // mistyped varlist produced empty labels instead of stopping at the typo. Stata
  // raises r(111). The distinction is decidable now only because the projection
  // carries an explicit existence marker — the label map alone could not express
  // it, since an empty label was simply not stored.
  static const std::regex reVarLabel(R"(^:\s*var(?:iable)?\s+label\s+(\w+)\s*$)",
                                      std::regex::icase);
  if (std::regex_match(rhs, m, reVarLabel)) {
    if (varKnownMissing(session, m.str(1))) {
      rejectMissingVar(m.str(1));
      return std::nullopt;
    }
    return session.varLabels.get(m.str(1), "");
  }

  // BUG-216: `: format <varname>` and `: value label <varname>`.
  //
  // Both were unimplemented, and the unrecognized-function tail below returned the
  // LITERAL MACRO TEXT — so `local f : format x` put `: format x` into `f`, which
  // `format v \`f'` then applies as a garbage format with no diagnostic anywhere.
  // `describe` renders the format correctly in the same session, so the
  // information always existed and was reachable; this was a missing READER, and
  // it now reads the same projected card `describe` renders.
  //
  // Oracle, StataNow 19.5, `StataBE -b`, in-log marker, on
  // `gen double x = _n` + `format x %9.3f` + a `gv` labelled `mylbl`:
  //     : format x            -> [%9.3f]      : format nosuchvar     -> r(111)
  //     : value label x       -> []  rc 0     : value label gv       -> [mylbl]
  //                                           : value label nosuchvar-> r(111)
  // Note `: value label x` on a variable with no value label is "" at rc 0, NOT an
  // error — the same shape as `: variable label` on an unlabelled variable, which
  // is why the absent-key default is "" and only a MISSING VARIABLE rejects.
  //
  // `: type` is deliberately absent, and its absence is a measurement, not an
  // oversight: this engine does not record Stata numeric subtypes for
  // engine-created variables at all. Live-probed this stint — `gen double x` and
  // `gen float y` both land as dtype "f64" and `describe` reports BOTH as "float",
  // because VarMeta::stataType is populated only by a .dta import. Stata answers
  // `: type x` with "double". Implementing it off what we have would emit "float"
  // there: a silent wrong answer replacing a visible non-answer, which is the
  // trade this whole stint exists to avoid. Filed as BUG-250 with the probe.
  // Until then `: type` takes the r(101) tail — honest, and wrong in the direction
  // that stops a do-file instead of feeding it a bad value.
  static const std::regex reVarFormat(R"(^:\s*format\s+(\w+)\s*$)", std::regex::icase);
  if (std::regex_match(rhs, m, reVarFormat)) {
    if (varKnownMissing(session, m.str(1))) {
      rejectMissingVar(m.str(1));
      return std::nullopt;
    }
    return session.varLabels.get(std::string(kVarFormatPrefix) + m.str(1), "%9.0g");
  }
  static const std::regex reVarLblset(R"(^:\s*value\s+label\s+(\w+)\s*$)", std::regex::icase);
  if (std::regex_match(rhs, m, reVarLblset)) {
    if (varKnownMissing(session, m.str(1))) {
      rejectMissingVar(m.str(1));
      return std::nullopt;
    }
    return session.varLabels.get(std::string(kVarLblsetPrefix) + m.str(1), "");
  }

  // BUG-250: `: type <varname>` — the reader half of the storage-subtype plane.
  //
  // BUG-216 left this at the r(101) tail ON PURPOSE, because the engine
  // recorded no subtype for its own columns and answering `float` for a
  // `gen double` would have been a silent wrong value. The plane now records
  // the DECLARED subtype at creation (dispatcher gen/egen arms ->
  // VarMeta::stataType) and SessionState projects it here under kVarTypePrefix
  // with describe's own fallback (numeric-without-declaration = "float", which
  // is also Stata's `gen` default). Oracle (exprfns_contract_oracle.log, J
  // rows): double/float/byte/int/long/str8 answered verbatim; undeclared `gen
  // z = _n` -> float; `: type nosuchvar` -> r(111) "variable ... not found".
  //
  // A variable the projection has NO type row for — a string column whose
  // width was never declared or imported (encode/tostring/CSV output) — falls
  // through to the r(101) tail below, deliberately: its true `str#` width is
  // not recorded anywhere in this engine, and a fabricated one would be the
  // exact silent-wrong-answer BUG-216 exists to prevent. Named in BUG-250's
  // ledger close as the measured remainder.
  static const std::regex reVarType(R"(^:\s*type\s+(\w+)\s*$)", std::regex::icase);
  if (std::regex_match(rhs, m, reVarType)) {
    if (varKnownMissing(session, m.str(1))) {
      rejectMissingVar(m.str(1));
      return std::nullopt;
    }
    if (const std::string* t = session.varLabels.find(std::string(kVarTypePrefix) + m.str(1))) {
      return *t;
    }
    // fall through: unrecorded subtype (or a plane that never synced) keeps
    // the honest r(101) reject.
  }

  // WS-C (C-5): matrix introspection.
  static const std::regex reColnames(R"(^:?\s*colnames\s+([A-Za-z_]\w*)$)", std::regex::icase);
  if (std::regex_match(rhs, m, reColnames)) {
    const EvalMatrix* mat = session.findMatrix(m.str(1));
    if (mat == nullptr) return std::string();  // undefined matrix -> ""
    std::string out;
    for (size_t i = 0; i < mat->colnames.size(); ++i) {
      if (i) out += " ";
      out += mat->colnames[i];
    }
    return out;
  }
  static const std::regex reRownames(R"(^:?\s*rownames\s+([A-Za-z_]\w*)$)", std::regex::icase);
  if (std::regex_match(rhs, m, reRownames)) {
    const EvalMatrix* mat = session.findMatrix(m.str(1));
    if (mat == nullptr) return std::string();
    std::string out;
    for (size_t i = 0; i < mat->rownames.size(); ++i) {
      if (i) out += " ";
      out += mat->rownames[i];
    }
    return out;
  }
  static const std::regex reExtColsof(R"(^:?\s*colsof\s+([A-Za-z_]\w*)$)", std::regex::icase);
  if (std::regex_match(rhs, m, reExtColsof)) {
    const EvalMatrix* mat = session.findMatrix(m.str(1));
    if (mat == nullptr) return std::string("0");
    return std::to_string(mat->data.empty() ? 0 : mat->data[0].size());
  }
  static const std::regex reExtRowsof(R"(^:?\s*rowsof\s+([A-Za-z_]\w*)$)", std::regex::icase);
  if (std::regex_match(rhs, m, reExtRowsof)) {
    const EvalMatrix* mat = session.findMatrix(m.str(1));
    if (mat == nullptr) return std::string("0");
    return std::to_string(mat->data.size());
  }

#ifdef _WIN32
  // WP-W5: MinGW-w64 has no <fnmatch.h>. Minimal glob matcher for the ONE
  // call site below (`: dir ... files "PATTERN"`): `*`, `?`, `[set]` with
  // `!`/`^` negation and ranges, plus the FNM_PERIOD rule via the wrapper
  // (a leading '.' in the name is only matched by a literal '.' in the
  // pattern — glob.glob parity, same as the POSIX branch). The POSIX branch
  // keeps calling the system fnmatch — bit-identical macOS behavior.
  struct GlobWin {
    static bool chars(const char* p, const char* s) {
      for (;;) {
        char pc = *p;
        if (pc == '\0') return *s == '\0';
        if (pc == '*') {
          while (*p == '*') ++p;  // collapse consecutive '*'
          if (*p == '\0') return true;
          for (const char* t = s;; ++t) {
            if (chars(p, t)) return true;
            if (*t == '\0') return false;
          }
        }
        if (*s == '\0') return false;
        if (pc == '?') { ++p; ++s; continue; }
        if (pc == '[') {
          const char* q = p + 1;
          bool neg = (*q == '!' || *q == '^');
          if (neg) ++q;
          bool matched = false;
          bool first = true;
          char c = *s;
          const char* scan = q;
          while (*scan != '\0' && (first || *scan != ']')) {
            first = false;
            char lo = *scan;
            if (scan[1] == '-' && scan[2] != '\0' && scan[2] != ']') {
              if (lo <= c && c <= scan[2]) matched = true;
              scan += 3;
            } else {
              if (lo == c) matched = true;
              ++scan;
            }
          }
          if (*scan != ']') {  // unterminated set: '[' is a literal (fnmatch behavior)
            if (c != '[') return false;
            ++p; ++s; continue;
          }
          if (matched == neg) return false;
          p = scan + 1; ++s; continue;
        }
        if (pc != *s) return false;
        ++p; ++s;
      }
    }
    static bool matchPeriod(const std::string& pattern, const std::string& name) {
      if (!name.empty() && name[0] == '.' &&
          (pattern.empty() || pattern[0] != '.')) {
        return false;  // FNM_PERIOD: wildcards never match a leading dot
      }
      return chars(pattern.c_str(), name.c_str());
    }
  };
#endif

  // U7/S9: `: dir "DIRNAME" files "PATTERN"` — directory glob.
  static const std::regex reDir(
      R"re(^:?\s*dir\s+(?:"([^"]*)"|(\S+))\s+files\s+(?:"([^"]*)"|(\S+)))re", std::regex::icase);
  if (std::regex_search(rhs, m, reDir)) {  // re.match = prefix match
    std::string dirPart = m[1].matched ? m.str(1) : (m[2].matched ? m.str(2) : ".");
    std::string pattern = m[3].matched ? m.str(3) : (m[4].matched ? m.str(4) : "*");
    try {
      namespace fs = std::filesystem;
      std::string base = !session.cwd.empty() ? session.cwd : fs::current_path().u8string();
      fs::path dir(dirPart);
      if (dir.is_relative()) dir = cecon::to_native_path(base) / dir;
      dir = dir.lexically_normal();
      std::vector<std::string> names;
      std::error_code ec;
      for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::string name = entry.path().filename().u8string();
        // FNM_PERIOD: '*'/'?' don't match a leading '.', like glob.glob.
#ifdef _WIN32
        if (GlobWin::matchPeriod(pattern, name)) {
#else
        if (fnmatch(pattern.c_str(), name.c_str(), FNM_PERIOD) == 0) {
#endif
          names.push_back(name);
        }
      }
      std::sort(names.begin(), names.end());
      std::string out;
      for (size_t i = 0; i < names.size(); ++i) {
        if (i) out += " ";
        out += names[i];
      }
      return out;
    } catch (const std::exception&) {
      return std::string();
    }
  }

  // BUG-216, and this is the more important half because it is GENERIC: it applies
  // to EVERY extended macro function this engine has not implemented, not only the
  // three named in the entry.
  //
  // This used to `return std::nullopt`, which sent the caller on to its
  // store-the-literal-RHS path — so `local f : notafunction x` stored the ten
  // characters `: notafunction x`. That is a third outcome, and worse than either
  // of the other two: not the answer, not empty, not an error, and no consumer can
  // tell it from data. BUG-208's failure at least returned "", a value the variable
  // might legitimately have had.
  //
  // Stata's answer is an ERROR — so the fall-through was never a lenient version
  // of Stata's behaviour, it was a different one. Oracle (StataNow 19.5, `capture
  // noisily`, in-log marker): `: notafunction x` -> "notafunction not allowed",
  // r(101); a bare `: nosuchfn` with no argument -> r(101) as well.
  //
  // BLAST RADIUS, measured before landing rather than assumed. Every
  // `local|global NAME : ...` line in tests/ and waves/ was enumerated and
  // classified. The only ones PIET actually executes are
  // tests/kitchensink/pangram/p2perf_bench.do:144-149 — `word count`, `word N of`,
  // `length local`, `subinstr local`, `list uniq`, `variable label` — all six
  // implemented arms above. The forms that are NOT implemented
  // (`: list xvars - v`, `: colnames e(b)`, `: colfullnames e(b)`) appear only in
  // tests/validate/*.do and tests/eval/golden/*.do, which are STATA-side truth
  // capture scripts: each declares "Stata truth" in its own header, each has a
  // sibling .log, they use Stata-only commands (`sysuse`, SSC `csdid`), and
  // run_eval.py never invokes a .do file at all (it reads golden CSVs). So zero
  // PIET-executed lines depended on the literal echo, and nothing was consuming
  // the echoed text as data.
  if (err != nullptr) {
    // Stata names the offending FUNCTION token, not the whole tail.
    std::string tok = strip(rhs.substr(1));
    const size_t cut = tok.find_first_of(" \t");
    if (cut != std::string::npos) tok = tok.substr(0, cut);
    err->rc = "101";
    err->message = (tok.empty() ? std::string("extended macro function")
                                : tok) + " not allowed";
  }
  return std::nullopt;  // unknown function form
}

// BUG-246: `local/global NAME = <expr>` where <expr> fails to evaluate.
//
// The fall-through used to store the raw text — so `local e = 1%2` put the
// three characters `1%2` into `e`, the same literal-echo family BUG-216 closed
// on the colon forms, and Stata's answer is an ERROR that stops the do-file.
// The fallback is still load-bearing for exactly two shapes, and only those:
//   * a STRING-literal RHS (`local x = "a:b c[1]"` — starts with `"` or the
//     compound `` `" ``): a legal Stata string expression this evaluator may
//     not cover, so it keeps evaluating-by-unquoting exactly as before;
//   * a BARE-IDENTIFIER NameError (`local x = mpg`): in Stata that reads the
//     variable's first observation — a capability this layer does not have,
//     and the variable plane is not synced at every call site, so erroring
//     here could reject a legitimate dataset variable. Unclaimed, unchanged.
// Everything else converts to the rc the oracle assigns
// (exprfns_contract_oracle.log, section H — each mapped row is a captured
// fact): `r(NNN):` channel from a function (string("abc",fmt) -> 109) wins;
// `scalar(UNDEF)` -> 111 "UNDEF not found"; unknown FUNCTION -> 133 "unknown
// function f()"; NAME[...] with no such matrix -> 111 "NAME not found"; an
// empty or otherwise unparseable expression -> 198 "invalid syntax" (Stata's
// own message for the bare `local x =`; its 198 texts vary by shape — `1%2
// invalid name`, `invalid '%'` — and only the CODE is claimed here, the
// message stays the generic one).
//
// Returns true when converted (err filled); false = keep the legacy fallback.
// Gated on err != nullptr, so every session-less caller (the evalcase
// differential harness, the selftest corpus) keeps byte-identical behaviour —
// the same gate BUG-216 used for the colon forms.
// Review finding (v3): true when `e` uses one of Stata's logical operators
// `&`, `|` or a bare `!` (not `!=`) outside a "..." string literal.
static bool hasStataLogicalOperator(const std::string& e) {
  bool inStr = false;
  for (size_t i = 0; i < e.size(); ++i) {
    const char c = e[i];
    if (c == '"') inStr = !inStr;
    if (inStr) continue;
    if (c == '&' || c == '|') return true;
    if (c == '!' && (i + 1 >= e.size() || e[i + 1] != '=')) return true;
  }
  return false;
}

bool classifyFailedEqExpr(const std::string& exprIn, const std::string& raised,
                          MacroFnError* err) {
  if (err == nullptr) return false;
  const std::string e = strip(exprIn);
  const bool stringLiteralRhs = !e.empty() && (e[0] == '"' || e.rfind("`\"", 0) == 0);
  // Review finding (v3): the `r(NNN):` channel is checked BEFORE the
  // string-literal exemption. A function or operator that raised it named a
  // Stata error on purpose, and a leading string literal does not make that
  // a form this evaluator failed to cover: `local a = "a" & 1` and
  // `local k = "a" + 1` are r(109) in StataBE 19.5, but the exemption stored
  // the literal text `a & 1` / `"a" + 1` with rc 0.
  if (stringLiteralRhs && raised.rfind("r(", 0) != 0) return false;  // string-literal RHS
  if (raised.rfind("r(", 0) == 0) {
    const size_t close = raised.find(')');
    if (close != std::string::npos && close > 2) {
      const std::string digits = raised.substr(2, close - 2);
      if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos) {
        const size_t colon = raised.find(": ");
        err->rc = digits;
        err->message = (colon != std::string::npos) ? raised.substr(colon + 2)
                                                    : std::string("expression error");
        return true;
      }
    }
  }
  static const char* kNameErrPrefix = "NameError: name '";
  if (raised.rfind(kNameErrPrefix, 0) == 0) {
    const size_t start = std::string(kNameErrPrefix).size();
    const size_t end = raised.find('\'', start);
    const std::string name = (end == std::string::npos) ? std::string()
                                                        : raised.substr(start, end - start);
    if (name == "scalar") {
      static const std::regex reScalarArg(R"(scalar\(\s*([A-Za-z_]\w*)\s*\))");
      std::smatch sm;
      if (std::regex_search(e, sm, reScalarArg)) {
        err->rc = "111";
        err->message = sm.str(1) + " not found";
        return true;
      }
    }
    if (!name.empty()) {
      if (e.find(name + "(") != std::string::npos) {
        err->rc = "133";
        err->message = "unknown function " + name + "()";
        return true;
      }
      // Review finding (v3): an unbound name under `&`, `|` or `!` keeps the
      // r(198) refusal these operators had before the lexer learned them.
      // `local c = foo | bar` over dataset variables is 1 in Stata (the
      // first observation); the literal echo stored `foo | bar` with rc 0.
      // Reading the first observation is the missing capability named
      // above, so this refuses rather than echo.
      if (!hasStataLogicalOperator(e)) {
        return false;  // bare identifier — the dataset-variable shape, unclaimed
      }
      err->rc = "198";
      err->message = "invalid syntax in " + pyRepr(e);
      return true;
    }
  }
  static const std::regex reSubscript(R"(^([A-Za-z_]\w*)\s*\[)");
  std::smatch sm;
  if (std::regex_search(e, sm, reSubscript)) {
    err->rc = "111";
    err->message = sm.str(1) + " not found";
    return true;
  }
  // BUG-339 diagnostic. An expression that still carries a macro reference is a
  // different fact from "invalid syntax", and saying so is what turns a
  // three-week log-archaeology into a one-line read. The rc stays 198 — only
  // the CODE is claimed to match Stata (see the note above this function), the
  // message text is ours — and the offending text is named rather than
  // discarded, since it is the one thing the reader needs and the one thing
  // this function had in hand and threw away.
  err->rc = "198";
  if (e.find('`') != std::string::npos || e.find("${") != std::string::npos) {
    err->message = "invalid syntax — unresolved macro reference in " + pyRepr(e);
  } else {
    err->message = "invalid syntax in " + pyRepr(e);
  }
  return true;
}

// _strip_outer_dquotes
std::string stripOuterDquotes(const std::string& rhs) {
  if (rhs.size() >= 2 && rhs.front() == '"' && rhs.back() == '"') {
    std::string inner = rhs.substr(1, rhs.size() - 2);
    if (inner.find('"') == std::string::npos) return inner;
  }
  return rhs;
}

}  // namespace

// BUG-293(b): public face of classifyFailedEqExpr (see macros.h).
bool classifyFailedExpr(const std::string& expr, const std::string& raised, MacroFnError* err) {
  return classifyFailedEqExpr(expr, raised, err);
}

// ---------------------------------------------------------------------------
// try_capture_definition

namespace {

// The corpus every EXPRLAYER drift guard below runs over. Deliberately wider
// than the benchmark: the whole point of a hand-written twin is that it must
// agree on the shapes the benchmark never produces.
const std::vector<std::string>& exprLayerCorpus() {
  static const std::vector<std::string> k = {
      // plain arithmetic (reSafeArith territory)
      "1+1", "0 + 1", "3.14*2", "2e3", "  12 / 4  ", "(1+2)^3", "1E-9", "-5", "",
      " ", "\t\n", "1\n+2",
      // extended (letters, quotes, comparisons)
      "sqrt(4)", "upper(\"abc\")", "cond(1>0,1,0)", "a<b", "x!=y", "mod(17,5)",
      "substr(\"benchmark1\",1,7)", "1,2", "'q'", "x_1", "log(2)+exp(-1)",
      // BUG-206: `%` is in the EXTENDED alphabet and NOT the basic one, so each
      // of these must answer ext=1 / basic=0 in both the scan and the regex.
      // `1%2` moved here FROM the outside-both group below when the character was
      // admitted — left in the corpus rather than deleted, because the boundary it
      // probes (a bare `%` as an operator) is exactly the one the widening had to
      // be measured against, and it is now an ext case rather than a rejected one.
      "1%2", "5 % 3", "string(42, \"%9.0g\")", "\"50%\"", "%", "strpos(\"50%\",\"%\")",
      "%9.0g", "upper(\"x%y\")",
      // characters OUTSIDE both whitelists
      "a[1]", "a{1}", "1;2", "x@y", "a\\b", "a#b", "a&b", "a|b", "a:b", "a$b",
      "`x'", "a?b", "1~2",
      // r()/e() shapes, both real and decoy
      "r(mean)", "`r(mean)'", "scalar(bp_s)", "subinstr(\"a\",\"b\",\"c\",1)",
      "e(N)", "true(1)", "size(x)", "myr(3)", "r (mean)", "_r(x)", "xe(1)",
      "e(b)", "e(V)", "1+r(N)", "sqrt(r(N))", "counter(3)",
      // scalar-command shapes
      "scalar x = 1", "scalar define x = 1", "scalar  define   x  =  1",
      "SCALAR X = 1", "Scalar define = 3", "scalar define = 3", "scalar x=1",
      "scalar x =", "scalar x =  ", "scalar x = \n", "scalar x", "scalar",
      "scalar drop x", "scalar drop x y", "scalar drop _all", "SCALAR DROP x",
      "scalar  drop   x  ", "scalar drop", "scalar drop ", "scalardrop x",
      "scalar\tdrop\tx", "scalar\ndrop\nx", "scalar list", "scalar dir",
      "scalar x = scalar(y) + 1", "scalar 1x = 2", "scalar _x = 2",
      "scalar x = a\nb", "scalar define define = 1",
  };
  return k;
}

}  // namespace

bool selftestSafeArithScanMatchesRegex(std::string* firstDisagreement) {
  for (const auto& c : exprLayerCorpus()) {
    std::smatch m;
    const bool reBasic = std::regex_match(c, m, reSafeArith());
    const bool reExt = std::regex_match(c, m, reSafeArithExt());
    if (reBasic != isSafeArith(c) || reExt != isSafeArithExt(c)) {
      if (firstDisagreement) {
        *firstDisagreement = "line=[" + c + "] regex=(" + (reBasic ? "1" : "0") +
                             (reExt ? "1" : "0") + ") scan=(" + (isSafeArith(c) ? "1" : "0") +
                             (isSafeArithExt(c) ? "1" : "0") + ")";
      }
      return false;
    }
  }
  return true;
}

bool selftestLocalRefScanMatchesRegex(std::string* firstDisagreement) {
  static const std::vector<std::string> kExtra = {
      "`x'", "`acc' + `i'", "local acc = `acc' + `i'", "`_u'", "`1'", "`12'", "`x",
      "``x''", "`'", "``'", "`x`y''", "a`b'c", "`9x'", "`x9'", "`x-y'", "`x y'",
      "no ticks here", "`", "``", "`\"a b\"'", "`=2+3'", "`r(mean)'", "`s(k)'",
      "`x' `y'", "``", "`0'", "`x_1'", "`X'", "`_'",
  };
  auto one = [&](const std::string& c) {
    std::smatch m;
    const bool re = std::regex_search(c, m, reLocalRef());
    const bool scan = hasLocalMacroRef(c);
    if (re != scan) {
      if (firstDisagreement) {
        *firstDisagreement = "line=[" + c + "] regex=" + (re ? "yes" : "no") + " scan=" +
                             (scan ? "yes" : "no");
      }
      return false;
    }
    return true;
  };
  for (const auto& c : exprLayerCorpus()) {
    if (!one(c)) return false;
  }
  for (const auto& c : kExtra) {
    if (!one(c)) return false;
  }
  return true;
}

bool selftestBarePrefilterIsStrict(std::string* firstDisagreement) {
  // A pre-filter is sound iff it never says "no" to text the regex matches.
  // (Saying "yes" where the regex does not match only costs a wasted scan.)
  for (const auto& c : exprLayerCorpus()) {
    std::smatch m;
    if (std::regex_search(c, m, reRRef()) && !hasBareRParen(c)) {
      if (firstDisagreement) *firstDisagreement = "reRRef matched but guard said no: [" + c + "]";
      return false;
    }
    if (std::regex_search(c, m, reERef()) && !hasBareEParen(c)) {
      if (firstDisagreement) *firstDisagreement = "reERef matched but guard said no: [" + c + "]";
      return false;
    }
    if (std::regex_search(c, m, reRRefDelim()) && c.find("`r(") == std::string::npos) {
      if (firstDisagreement) {
        *firstDisagreement = "reRRefDelim matched but guard said no: [" + c + "]";
      }
      return false;
    }
  }
  return true;
}

bool selftestScalarScansMatchRegex(std::string* firstDisagreement) {
  static const std::regex reScalarDropRx(R"(^scalar\s+drop\s+(.+)$)", std::regex::icase);
  static const std::regex reScalarCallRx(R"(\bscalar\(\s*([A-Za-z_]\w*)\s*\))");

  // A session with a couple of scalars so the substitutions have something to
  // find, plus one name that also occurs as a function call (`scalar(...)`) so
  // the (?!\s*\() lookahead is exercised on both arms.
  EvalSession s;
  s.scalars.set("bp_s", "12.5");
  s.scalars.set("y", "7");
  s.scalars.set("log", "99");  // shadows a function name on purpose

  for (const auto& c : exprLayerCorpus()) {
    std::smatch m;

    // scalar( ... ) substitution
    const std::string viaRegex = regexSubFn(reScalarCallRx, c, [&s](const std::smatch& mm) {
      const std::string* v = s.scalars.find(mm.str(1));
      return v ? *v : mm.str(0);
    });
    const std::string viaScan = substituteScalarCalls(c, s);
    if (viaRegex != viaScan) {
      if (firstDisagreement) {
        *firstDisagreement = "scalar() subst [" + c + "] regex=[" + viaRegex + "] scan=[" +
                             viaScan + "]";
      }
      return false;
    }

    // bare-name substitution, once per stored scalar
    for (const auto& [sn, sv] : s.scalars.items()) {
      std::regex reName("\\b" + regexEscape(sn) + "\\b(?!\\s*\\()");
      std::string value = sv;
      const std::string bRegex =
          regexSubFn(reName, c, [&value](const std::smatch&) { return value; });
      const std::string bScan = substituteBareName(c, sn, sv);
      if (bRegex != bScan) {
        if (firstDisagreement) {
          *firstDisagreement = "bare-name subst name=" + sn + " [" + c + "] regex=[" + bRegex +
                               "] scan=[" + bScan + "]";
        }
        return false;
      }
    }

    // scalar drop / scalar define recognisers
    const bool dropRe = std::regex_match(c, m, reScalarDropRx);
    const std::string dropReRest = dropRe ? m.str(1) : std::string();
    std::string dropScanRest;
    const bool dropScan = matchScalarDrop(c, dropScanRest);
    if (dropRe != dropScan || (dropRe && dropReRest != dropScanRest)) {
      if (firstDisagreement) {
        *firstDisagreement = "scalar drop [" + c + "] regex=" +
                             (dropRe ? "MATCH(" + dropReRest + ")" : "no") + " scan=" +
                             (dropScan ? "MATCH(" + dropScanRest + ")" : "no");
      }
      return false;
    }

    const bool defRe = std::regex_match(c, m, reScalarDef());
    const std::string defReName = defRe ? m.str(1) : std::string();
    const std::string defReRest = defRe ? m.str(2) : std::string();
    std::string defScanName, defScanRest;
    const bool defScan = matchScalarDef(c, defScanName, defScanRest);
    if (defRe != defScan || (defRe && (defReName != defScanName || defReRest != defScanRest))) {
      if (firstDisagreement) {
        *firstDisagreement =
            "scalar def [" + c + "] regex=" +
            (defRe ? "MATCH(" + defReName + "|" + defReRest + ")" : "no") + " scan=" +
            (defScan ? "MATCH(" + defScanName + "|" + defScanRest + ")" : "no");
      }
      return false;
    }
  }
  return true;
}

bool selftestExpansionSemantics(std::string* firstFailure) {
  // The S32-class parity corpus. Each leg is (setup-free) input -> expected
  // outcome of expandMacros on a session seeded below. These assert TODAY's
  // behavior, whatever it is — the point is that no performance change may move
  // any of them, not that any particular one is Stata-perfect.
  EvalSession s;
  s.locals.set("x", "5");
  s.locals.set("y", "mpg");
  s.locals.set("inner", "x");
  s.locals.set("empty", "");
  s.locals.set("withdollar", "$G");
  s.locals.set("1", "first");
  s.globals.set("G", "gval");
  s.rValues.set("mean", 3.5);
  s.rStrings.set("lbl", "hello");
  s.eScalars.set("N", 42.0);

  struct Leg {
    const char* in;
    bool wantError;
    const char* want;  // meaningful only when !wantError
  };
  static const Leg kLegs[] = {
      // plain reference, and a line with none at all
      {"display `x'", false, "display 5"},
      {"summarize mpg", false, "summarize mpg"},
      // an EMPTY macro substitutes nothing (not a space, not a quote)
      {"display [`empty']", false, "display []"},
      // nested: `\`inner'' resolves through two passes
      {"display ``inner''", false, "display 5"},
      // a macro whose VALUE carries a $global is expanded on the next pass
      {"display `withdollar'", false, "display gval"},
      // positional macro
      {"display `1'", false, "display first"},
      // `=expr' inline arithmetic, including one that references a macro
      {"display `=2+3'", false, "display 5"},
      {"display `=sqrt(16)'", false, "display 4"},
      // global forms, braced and bare
      {"display $G", false, "display gval"},
      {"display ${G}", false, "display gval"},
      // r() numeric and r() string (the string store wins)
      {"display r(mean)", false, "display 3.5"},
      {"display `r(lbl)'", false, "display hello"},
      // e() scalar out of the posted store
      {"display e(N)", false, "display 42.0"},
      // BUG-294: an UNDEFINED macro expands to the EMPTY STRING at rc 0 —
      // Stata 19.5 IC, measured (`display "[`nosuchlocal']"` -> `[]`, _rc 0;
      // the global forms identically). These two rows used to pin the opposite
      // (`true` = error), which is what stopped a do-file dead one line after a
      // deliberately-captured failed definition.
      {"display `nosuch'", false, "display "},
      {"display $NOSUCH", false, "display "},
      // a bare backtick that is not a reference is copied through
      {"display `", false, "display `"},
      {"display a`b", false, "display a`b"},
      // compound double-quotes are unwrapped to plain quotes
      {"display `\"a b\"'", false, "display \"a b\""},
      // an identifier ending in r/e followed by '(' is NOT a class reference
      {"display subinstr(\"a\",\"b\",\"c\",1)", false, "display subinstr(\"a\",\"b\",\"c\",1)"},
      {"display size(3)", false, "display size(3)"},
      {"scalar q = scalar(bp_s) + 1", false, "scalar q = scalar(bp_s) + 1"},
  };

  for (const auto& leg : kLegs) {
    ExpandResult r = expandMacros(leg.in, s);
    if (leg.wantError) {
      if (!r.isError) {
        if (firstFailure) *firstFailure = std::string("[") + leg.in + "] expected error, got [" +
                                          r.line + "]";
        return false;
      }
      continue;
    }
    if (r.isError || r.line != leg.want) {
      if (firstFailure) {
        *firstFailure = std::string("[") + leg.in + "] want [" + leg.want + "] got " +
                        (r.isError ? std::string("ERROR") : "[" + r.line + "]");
      }
      return false;
    }
  }
  return true;
}

bool selftestMacroDefScanMatchesRegex(std::string* firstDisagreement) {
  static const std::vector<std::string> kCases = {
      // ordinary forms
      "local q 5", "local acc = `acc' + `i'", "local x", "local x=3", "local x  =  3",
      "local mylist \"alpha beta\"", "local _u 1", "local x9 hello", "local ++ctr",
      "global G 7", "global G = 2+3", "global _g x", "global g",
      // case variants (Stata accepts them)
      "LOCAL Q 5", "Local q 5", "GLOBAL G 7", "gLoBaL g 1",
      // whitespace shapes
      "local\tq\t5", "local\t \tq 5", "local   q   5   ", "local\nq 5", "global\vg 1",
      // non-matches
      "localq 5", "local", "local ", "local  ", "local 1x 3", "local -x 3", "local `k' 5",
      "globalg 1", "global", "global 9z 1", "scalar a = 1", "display 1", "",
      // the newline boundary: `.` cannot cross one, so the tail decides
      "local q a\nb", "global g a\nb", "local q 5\n",
      // name/rest boundaries
      "local q:word count x", "local q:", "local q,", "local q)", "global g:",
  };
  for (const auto& c : kCases) {
    for (int which = 0; which < 2; ++which) {
      const char* kw = which == 0 ? "local" : "global";
      const size_t kwLen = which == 0 ? 5 : 6;
      const std::regex& re = which == 0 ? reLocalDef() : reGlobalDef();
      std::smatch rm;
      const bool reOk = std::regex_match(c, rm, re);
      std::string sName, sRest;
      const bool scanOk = matchMacroDef(c, kw, kwLen, sName, sRest);
      bool agree = (reOk == scanOk);
      if (agree && reOk) agree = (rm.str(1) == sName && rm.str(2) == sRest);
      if (!agree) {
        if (firstDisagreement) {
          *firstDisagreement = std::string(kw) + " | line=[" + c + "] regex=" +
                               (reOk ? "MATCH(" + rm.str(1) + "|" + rm.str(2) + ")" : "no") +
                               " scan=" + (scanOk ? "MATCH(" + sName + "|" + sRest + ")" : "no");
        }
        return false;
      }
    }
  }
  return true;
}

bool tryCaptureDefinition(const std::string& rawLine, EvalSession& session,
                          MacroFnError* errorOut) {
  std::string stripped = strip(rawLine);
  // AER-SCOPE1 bundle A: `loc x = 1`, `gl G "..."` and `sca de p = .05` never
  // reach a registry lookup -- a macro DEFINITION is captured here, before
  // dispatch, so an abbreviation the registry knows would still not be seen.
  // Folded onto the canonical head once, so every matcher below (the scalar
  // prefix test, reMdyn, and the two matchMacroDef calls keyed to "local" and
  // "global") keeps working unchanged. `de`/`def` are folded to `define` for
  // the same reason: matchScalarDef compares the six-character word.
  {
    size_t sp = stripped.find_first_of(" \t");
    std::string head = sp == std::string::npos ? stripped : stripped.substr(0, sp);
    std::string headLower;
    for (char c : head) headLower += static_cast<char>(std::tolower((unsigned char)c));
    const char* canon = nullptr;
    if (headLower == "loc") canon = "local";
    else if (headLower == "gl") canon = "global";
    else if (headLower == "sca") canon = "scalar";
    if (canon != nullptr) {
      stripped = std::string(canon) + (sp == std::string::npos ? std::string() : stripped.substr(sp));
    }
    // `scalar de|def NAME = expr` -> `scalar define NAME = expr`.
    static const std::regex reScalarDefAbbrev(R"(^scalar\s+def?\s+)", std::regex::icase);
    std::smatch dm;
    if (std::regex_search(stripped, dm, reScalarDefAbbrev)) {
      stripped = "scalar define " + stripped.substr(dm[0].str().size());
    }
  }
  std::smatch m;

  // --- scalar commands: define / drop / list ---
  // A3: `^scalar\b` is a prefix test, not a search — a compare answers it
  // without building regex state. Case-insensitive like the pattern it replaces,
  // and the \b is reproduced by requiring a non-identifier char after it.
  auto isScalarPrefix = [](const std::string& t) {
    if (t.size() < 6) return false;
    static const char* kS = "scalar";
    for (int i = 0; i < 6; ++i)
      if (std::tolower(static_cast<unsigned char>(t[i])) != kS[i]) return false;
    if (t.size() == 6) return true;
    char c = t[6];
    return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_');
  };
  if (isScalarPrefix(stripped)) {
    std::string scLine = stripped;
    if (hasMacroRefs(scLine)) {
      ExpandResult exp = expandPass(scLine, session);
      if (!exp.isError) scLine = exp.line;
    }
    // EXPRLAYER-5: matchScalarDrop/matchScalarDef walk the same two grammars
    // left to right; selftestScalarScansMatchRegex() holds them to the regexes.
    std::string sdRest;
    if (matchScalarDrop(scLine, sdRest)) {
      auto targets = pySplit(sdRest);
      if (targets.size() == 1 && targets[0] == "_all") {
        session.scalars.clear();
      } else {
        for (const auto& t : targets) session.scalars.erase(t);
      }
      return true;
    }
    // BS-038 (BETA campaign C3): `scalar list`/`scalar dir` used to be
    // intercepted HERE and swallowed — this function's signature is
    // bool-only (tryCaptureDefinition's "did I handle this line" success
    // signal), with no way to carry a display block back to dispatch()'s
    // caller, so returning true unconditionally silenced the command
    // entirely (SCOPE BS-038: "deliberate silent no-op today"). Falling
    // through instead (removed the match here) lets the line reach the
    // dispatcher's own EXISTING `cmd=="scalar" && varlist[0] in
    // {list,dir}` arm (dispatcher.cpp), which already calls
    // handlers::runScalarList — that handler's own comment covers the
    // rest of the fix (oracle-format column alignment). reScalarDef()
    // below requires an `=`, so it does not accidentally re-swallow
    // "scalar list"/"scalar dir" either — both fall all the way through
    // this function to `return false` at the very end, unchanged.
    std::string sdName, sdExpr;
    if (matchScalarDef(scLine, sdName, sdExpr)) {
      std::string name = sdName;
      std::string expr = strip(sdExpr);
      {
        // BUG-456: an unknown coefficient name is r(111) here as everywhere
        // else (Stata V4: `scalar s = _b[nosuch]` -> r(111)); the scalar stays
        // unset, as after any failed `scalar =` under capture.
        std::string bErr;
        expr = resolveEstScalarRefs(expr, session, &bErr);
        if (!bErr.empty()) {
          if (errorOut != nullptr) {
            // BUG-456 follow-up: r(109) too (`scalar s = strlen(e(nosuch))`).
            const std::string code = splitErrCode(&bErr);
            errorOut->rc = code.substr(2, code.size() - 3);
            errorOut->message = bErr;
          }
          return true;
        }
      }
      dprof::ezoneBegin(dprof::kECapDefScalarSub);
      for (const auto& [sname, sval] : session.scalars.items()) {
        if (expr.find(sname) != std::string::npos) {
          expr = substituteBareName(expr, sname, sval);  // EXPRLAYER-5
        }
      }
      dprof::ezoneEnd(dprof::kECapDefScalarSub);
      std::string raisedSc;
      auto arith = evalArith(expr, &session, &raisedSc);
      // BUG-456: a function announcing Stata's EXACT code (`("regress"==1)` ->
      // pyEquals' r(109)) is an error here as it is on `local =`, not the
      // legacy store-the-text fallback -- which kept `scalar s = (e(cmd)==1)`
      // at rc 0 with the expression text as its value. Only the r(NNN) channel
      // is claimed; every other unevaluable shape keeps the fallback.
      if (!arith && raisedSc.rfind("r(", 0) == 0) {
        MacroFnError scErr;
        if (classifyFailedEqExpr(expr, raisedSc, &scErr)) {
          if (errorOut != nullptr) *errorOut = scErr;
          return true;
        }
      }
      session.scalars.set(name, arith ? *arith : expr);
      return true;
    }
    // unrecognized `scalar` line — fall through (don't swallow)
  }

  // --- dynamic macro-named local/global: `local cell_`k' = ...` ---
  static const std::regex reMdyn(R"(^((?:local|global)\s+)(\S+)(.*)$)", std::regex::icase);
  // D1-①: the body below fires only when the NAME token carries a '`' or a
  // '$', so scan that token instead of running a case-insensitive alternation
  // regex over the whole line to discover it. Same A3 idiom this file already
  // uses ahead of its other guardable regexes; the regex itself is untouched
  // and still decides every line that gets past the scan.
  //
  // The first cut of this guard tested the WHOLE line for '`'/'$' and left
  // 41.8% of tryCaptureDefinition still inside std::regex on S01's form 1
  // (`local acc = `acc' + `i''), whose backtick is in the VALUE, never the
  // name — measured, then narrowed to the token that actually gates the body.
  if (macroDefNameTokenHasMacroChar(stripped)) {
  dprof::noteRegex();
  if (std::regex_match(stripped, m, reMdyn)) {
    std::string nameTok = m.str(2);
    if ((nameTok.find('`') != std::string::npos || nameTok.find('$') != std::string::npos) &&
        hasMacroRefs(nameTok)) {
      ExpandResult nameExp = expandPass(nameTok, session);
      if (!nameExp.isError && nameExp.line.find('`') == std::string::npos &&
          nameExp.line.find('$') == std::string::npos) {
        stripped = m.str(1) + nameExp.line + m.str(3);
      } else {
        // Name macro didn't resolve: surface the real undefined-macro error
        // at step 0b instead of capturing a truncated name.
        return false;
      }
    }
  }
  }  // D1-①: backtick/'$' pre-filter

  // --- local ++NAME / --NAME (case-SENSITIVE PREFIX form only) ---
  // BS-035 (D10 locked decision, BETA campaign C3): the POSTFIX form
  // (`local i++` / `local i--`) used to ALSO increment/decrement here —
  // invented semantics with no real-Stata counterpart. Real Stata has no
  // postfix `local` operator at all: `local i++` is just an ordinary
  // `local NAME rest` assignment where NAME="i" and the LITERAL text "++"
  // becomes i's new value (live-verified this stint, StataBE: `local i =
  // 7` then `local i++` -> i=[++], NOT i=[8]). Removed the postfix regex/
  // branch entirely — reLocalDef() below (`^local\s+(\w+)\s*(.*)?$`)
  // already correctly parses "local i++" as NAME="i", rest="++" (no space
  // needed — `\w*` simply stops at the non-word "+"), and its own generic
  // "no `=`, no macro refs, no quotes -> store rest verbatim" fallback
  // stores "++" byte-for-byte, so this compat change needed no new code,
  // only deleting the old postfix branch. PREFIX (`local ++i`) is
  // UNCHANGED — it IS real Stata increment syntax.
  // A3: the prefix-increment form needs a literal `++` or `--`.
  static const std::regex reIncPre(R"(^local\s+(\+\+|--)\s*([A-Za-z_]\w*)\s*$)");
  if ((stripped.find("++") != std::string::npos || stripped.find("--") != std::string::npos) &&
      std::regex_match(stripped, m, reIncPre)) {
    std::string incOp = m.str(1);
    std::string incName = m.str(2);
    double val = 0.0;
    auto f = pythonFloat(session.locals.get(incName, "0"));
    if (f) val = *f;
    val += (incOp == "++") ? 1.0 : -1.0;
    session.locals.set(incName,
                        pyIsInteger(val) ? pythonIntStrFromDouble(val) : pythonFloatRepr(val));
    return true;
  }

  // --- local NAME rest ---
  // D1-①: `reLocalDef()` is still the spec (and selftest still compares this
  // scan against it, form by form); matchMacroDef is the same grammar walked
  // once, left to right, with no regex state to build and tear down.
  std::string mdName, mdRest;
  if (matchMacroDef(stripped, "local", 5, mdName, mdRest)) {
    std::string name = mdName;
    std::string rhs = strip(mdRest);

    // Compound double-quote RHS: `local x `"...."'`
    if (rhs.size() >= 4 && rhs.rfind("`\"", 0) == 0 &&
        rhs.compare(rhs.size() - 2, 2, "\"'") == 0) {
      std::string inner = rhs.substr(2, rhs.size() - 4);
      if (hasMacroRefs(inner)) {
        ExpandResult exp = expandPass(inner, session);
        if (!exp.isError) inner = exp.line;
      }
      session.locals.set(name, inner);
      return true;
    }
    // Python slice rhs[2:-2] on a 3-char rhs (`"') yields "" — cover the
    // startswith/endswith overlap explicitly.
    if (rhs.size() == 3 && rhs.rfind("`\"", 0) == 0 && rhs.compare(1, 2, "\"'") == 0) {
      session.locals.set(name, "");
      return true;
    }

    // Extended macro function form: local NAME : <func> ...
    std::string extRhs = rhs;
    if (!rhs.empty() && rhs[0] == ':' && hasMacroRefs(rhs)) {
      ExpandResult extExp = expandPass(rhs, session);
      if (!extExp.isError) extRhs = extExp.line;
    }
    auto fnResult = tryExtendedMacroFn(extRhs, session, errorOut);
    if (fnResult) {
      session.locals.set(name, *fnResult);
      return true;
    }
    // BUG-216/217: a colon form that FAILED is still a form this layer consumed.
    // Return true so the caller stops treating the line as "not a definition" and
    // falling through to store-the-literal-text, and let it read errorOut to
    // surface the rc. The local is deliberately left UNSET — Stata's own behaviour
    // (measured: after `capture local z : notafunction x`, `z` is empty), so a
    // do-file that captures the error does not then find garbage in the macro.
    if (errorOut != nullptr && errorOut->failed()) return true;

    // `= expr` form
    if (!rhs.empty() && rhs[0] == '=') {
      std::string expr = strip(rhs.substr(1));
      if (hasMacroRefs(expr)) {
        // BUG-339: expandMacros, not expandPass. One pass resolves one LEVEL of
        // nesting, and `local rr = `cell_rel_`i''` has two: the pass copies the
        // outer backtick through untouched while resolving the inner `i', so
        // the RHS arrives at evalArith as `cell_rel_1' — still a live macro
        // reference, whose backtick is in neither arith whitelist. evalArith
        // returns nullopt, classifyFailedEqExpr falls through to its last
        // branch, and the line dies as a bare r(198) "invalid syntax".
        //
        // Every other consumer of a possibly-nested right-hand side already
        // loops (dispatcher's step 0b, loops.cpp's three sites). This branch
        // and its global twin were the only single-pass ones; the fix is to
        // stop being the exception, not to special-case the shape.
        //
        // The bug was invisible for three weeks because it is only reachable
        // through a do-file that nests twice: BUG-246's own reliance sweep
        // enumerated all 364 in-repo `local NAME =` lines and found no
        // double-nested RHS, and the acceptance corpus (which has exactly two)
        // is outside the repo. Before BUG-246 an unevaluable `= expr` stored
        // its literal text and dispatcher's LOOPING step-0b resolved it later —
        // so the corpus was accidentally right by deferral, and making the
        // failure honest is what exposed the missing pass here.
        ExpandResult exp = expandMacros(expr, session);
        if (exp.isError) {
          // Expansion error — store just the expr part (not the `=` prefix).
          session.locals.set(name, expr);
          return true;
        }
        expr = exp.line;
      }
      std::string raised246;
      bool isStrResult246 = false;
      auto arith = evalArith(expr, &session, &raised246, &isStrResult246);
      // BUG-246: an expression that FAILED to evaluate is an error, not a
      // value — see classifyFailedEqExpr for the mapping and for the two
      // shapes that deliberately keep the legacy literal fallback. The local
      // stays UNSET on the error path (Stata: after `capture local e = 1%2`,
      // `e` is empty — oracle section H).
      if (!arith && classifyFailedEqExpr(expr, raised246, errorOut)) return true;
      // NEVER store the `= expr` form with the leading `=`.
      //
      // stripOuterDquotes on the non-arithmetic fallback: in Stata `local x =
      // "abc"` stores abc, not "abc" — the `=` form EVALUATES its right-hand
      // side, and a bare string literal evaluates to its contents. Without
      // this the quotes were stored as part of the value, which `display`
      // hides (adjacent quoted segments concatenate) but a path expansion
      // does not: `local k = c(pwd)` then `use "`k'/f.csv"` produced
      //   File not found: <cwd>/"<cwd>"/f.csv
      // because c() string values arrive here already quoted by
      // substituteCRefs (correct for expression contexts, wrong to keep in a
      // macro body). The helper only strips a WHOLE "..." with no interior
      // quote, so `= "a" + "b"` and every arithmetic form are untouched.
      //
      // BUG-660 fix round: a NUMERIC arith result (not resultIsString) gets
      // Stata's 16-sig-fig macro cap here, at the local STORE site — not
      // inside evalArith, which other callers (`scalar =`) need round-trip-
      // exact (see evalArith's own comment).
      session.locals.set(name, arith ? (isStrResult246 ? *arith
                                                         : capNumericTextToStataLocalDigits(*arith))
                                      : stripOuterDquotes(expr));
      return true;
    }

    // Append / nested idiom: expand RHS refs (ONE pass) before storing.
    if (hasMacroRefs(rhs)) {
      ExpandResult exp = expandPass(rhs, session);
      if (!exp.isError) rhs = exp.line;
      // expansion failure (undefined ref): store the literal RHS
    }

    rhs = stripOuterDquotes(rhs);
    session.locals.set(name, rhs);
    return true;
  }

  // --- global NAME rest ---
  // D1-① (same as local above).
  if (matchMacroDef(stripped, "global", 6, mdName, mdRest)) {
    std::string name = mdName;
    std::string rhs = strip(mdRest);

    // BUG-247: extended macro function form `global NAME : <func> ...` — the
    // `local` twin's dispatch, which this branch never had: every colon form
    // that answered under `local` stored its own source text under `global`
    // (`global G1 : word count a b c` -> `$G1` = `: word count a b c`, where
    // Stata answers 3 — oracle section I). Mirrors the local branch exactly,
    // including BUG-216/217's error propagation: an unimplemented function is
    // r(101), a missing variable r(111), and the global is left UNSET on the
    // error path (oracle: `$G4` is empty after the captured r(101)).
    // Blast radius measured before landing: zero `global NAME : ...` lines
    // exist in any PIET-executed do-file in the tree (the BUG-247 entry's
    // sweep, re-run this stint — the only extended-macro forms the corpus
    // executes are the six `local` ones in p2perf_bench.do).
    std::string extRhsG = rhs;
    if (!rhs.empty() && rhs[0] == ':' && hasMacroRefs(rhs)) {
      ExpandResult extExp = expandPass(rhs, session);
      if (!extExp.isError) extRhsG = extExp.line;
    }
    auto fnResultG = tryExtendedMacroFn(extRhsG, session, errorOut);
    if (fnResultG) {
      session.globals.set(name, *fnResultG);
      return true;
    }
    if (errorOut != nullptr && errorOut->failed()) return true;

    // `= expr` form — same contract as local's above, which globals were
    // missing entirely: `global x = "abc"` stored the literal `= abc` (the
    // `=` kept as part of the value and nothing evaluated), so `global n = 2+3`
    // stored `= 2+3` instead of 5. Mirror local's branch exactly.
    if (!rhs.empty() && rhs[0] == '=') {
      std::string expr = strip(rhs.substr(1));
      if (hasMacroRefs(expr)) {
        // BUG-339, global twin — see the local branch above for the mechanism.
        ExpandResult exp = expandMacros(expr, session);
        if (exp.isError) {
          session.globals.set(name, expr);
          return true;
        }
        expr = exp.line;
      }
      std::string raised246g;
      bool isStrResult246g = false;
      auto arith = evalArith(expr, &session, &raised246g, &isStrResult246g);
      // BUG-246, global twin — same conversion, same two retained fallbacks.
      if (!arith && classifyFailedEqExpr(expr, raised246g, errorOut)) return true;
      // BUG-660 fix round: same cap-at-store-site rule as local's branch above.
      session.globals.set(name, arith ? (isStrResult246g
                                              ? *arith
                                              : capNumericTextToStataLocalDigits(*arith))
                                       : stripOuterDquotes(expr));
      return true;
    }

    if (hasMacroRefs(rhs)) {
      ExpandResult exp = expandPass(rhs, session);
      if (!exp.isError) rhs = exp.line;
    }
    rhs = stripOuterDquotes(rhs);
    session.globals.set(name, rhs);
    return true;
  }

  return false;
}

// ---------------------------------------------------------------------------
// capture_r_from_block

void captureRFromBlock(const nlohmann::ordered_json& block, EvalSession& session) {
  const std::string blockType = block.value("type", "");

  // --- Branch 1: summarize -> r() from the LAST variable's stats ---
  if (blockType == "summarize") {
    if (!block.contains("stats") || !block.at("stats").is_object() || block.at("stats").empty()) {
      return;
    }
    const auto& stats = block.at("stats");
    // Stata stores r() from the last variable in a multi-var summarize —
    // last key in dict order (ordered_json preserves document order).
    std::string lastVar;
    for (const auto& [k, v] : stats.items()) lastVar = k;
    const auto& varStats = stats.at(lastVar);

    static const std::vector<std::pair<std::string, std::string>> kSummarizeRMap = {
        {"N", "N"},           {"mean", "mean"},   {"sd", "sd"},   {"min", "min"},
        {"max", "max"},       {"sum", "sum"},     {"sum_w", "sum_w"}, {"Var", "Var"},
        {"p1", "p1"},         {"p5", "p5"},       {"p10", "p10"}, {"p25", "p25"},
        {"p50", "p50"},       {"p75", "p75"},     {"p90", "p90"}, {"p95", "p95"},
        {"p99", "p99"},       {"skewness", "skewness"}, {"kurtosis", "kurtosis"},
    };

    OrderedFloatDict newR;
    for (const auto& [statKey, rName] : kSummarizeRMap) {
      if (varStats.contains(statKey)) {
        auto f = jsonToFloat(varStats.at(statKey));
        if (f) newR.set(rName, *f);  // skip non-numeric (TypeError/ValueError)
      }
    }
    // Var = sd^2 when sd present but Var not already in stats.
    if (newR.contains("sd") && !newR.contains("Var")) {
      double sd = *newR.find("sd");
      newR.set("Var", sd * sd);
    }
    // median alias for p50.
    if (newR.contains("p50") && !newR.contains("median")) {
      newR.set("median", *newR.find("p50"));
    }
    session.rValues = newR;  // replace entire r() store (Stata semantics)
    return;
  }

  // --- Branch 3: test -> r-class Wald result (W:P5) ---
  // `test` is r-class in Stata (r(F), r(p), r(df), r(df_r), r(chi2), r(drop),
  // r(dropped)) but this codebase only ever wired summarize and lincom/nlcom,
  // so `test 1.treated` followed by `display r(p)` errored with "r(p) is not
  // set". The engine's test block already carries every value under its own
  // key names (F/p_F/df1/df2, plus chi2/p_chi2 on the chi-square form) — this
  // maps them onto Stata's r() spellings. Keep the lincom branch below
  // untouched: the two estimators publish different shapes.
  if (blockType == "estimation" && block.value("estimator", "") == "test") {
    // BUG-205(3), CORRECTED at the p5-wave regression (2026-08-08): the first
    // version mapped `p_chi2 -> p` unconditionally, on the stated premise that
    // "only one of the two source keys is ever present on a given block".
    // MEASURED FALSE: `test 1.treated` after `regress ..., vce(cluster id)`
    // emits BOTH (p_F=6.277e-26, p_chi2=9.705e-40), and the chi2 mapping ran
    // last, so r(p) silently became the chi2 tail on an F-form test — caught
    // by the p5 answerbook (want = Stata's F-based 6.277e-26). Stata's rule:
    // the DISPLAYED form owns r() — F-form sets {F, df, df_r, p}; the ML/chi2
    // form (no F on the block) sets {chi2, df, p}. The chi2 keys are now a
    // FALLBACK, applied only when the block carries no F.
    OrderedFloatDict newR;
    const bool fForm = block.contains("p_F") && !block.at("p_F").is_null();
    static const std::vector<std::pair<std::string, std::string>> kFMap = {
        {"F", "F"}, {"p_F", "p"}, {"df1", "df"}, {"df2", "df_r"}};
    static const std::vector<std::pair<std::string, std::string>> kChi2Map = {
        {"chi2", "chi2"}, {"p_chi2", "p"}, {"df1", "df"}};
    for (const auto& [src, dst] : (fForm ? kFMap : kChi2Map)) {
      if (block.contains(src) && !block.at(src).is_null()) {
        auto f = jsonToFloat(block.at(src));
        if (f) newR.set(dst, *f);
      }
    }
    // A malformed/empty test block must leave r() alone rather than clearing
    // it — F is the one key every shape of this block carries.
    if (!newR.contains("F") && !newR.contains("chi2")) return;
    session.rValues = newR;
    return;
  }

  // --- Branch 2: lincom / nlcom -> r-class post-estimation (WS-E) ---
  if (blockType == "estimation") {
    const std::string estimator = block.value("estimator", "");
    if (estimator != "lincom" && estimator != "nlcom") return;
    if (!block.contains("coeftable") || !block.at("coeftable").is_array() ||
        block.at("coeftable").empty()) {
      return;
    }
    const auto& row = block.at("coeftable").at(0);
    static const std::vector<std::pair<std::string, std::string>> kMap = {
        {"b", "estimate"}, {"se", "se"}, {"t", "t"}, {"p", "p"}, {"ci_lo", "lb"}, {"ci_hi", "ub"},
    };
    OrderedFloatDict newR;
    for (const auto& [src, dst] : kMap) {
      if (row.contains(src) && !row.at(src).is_null()) {
        auto f = jsonToFloat(row.at(src));
        if (f) newR.set(dst, *f);
      }
    }
    if (block.contains("df") && !block.at("df").is_null()) {
      auto f = jsonToFloat(block.at("df"));
      if (f) newR.set("df", *f);
    }
    session.rValues = newR;
    return;
  }

  // All other block types: do not touch r().
}

}  // namespace piet::eval
