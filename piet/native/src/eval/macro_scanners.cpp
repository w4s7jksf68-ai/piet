// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// macro scanners: see macro_modules.md for ownership and call flow.
#include "piet/eval/macros.h"
#include "estimators/postest_names.h"  // coefFullName: the engine's coefficient-name rule

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
#include <functional>
#include <regex>
#include <set>
#include <unordered_set>
#include <vector>

#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/pyvalue.h"
#include "piet/strutil.h"
#include "core/ident.h"  // Unicode names: the shared identifier byte tests
#include "core/expr_blanks.h"  // FU-875: the blanks Stata drops inside ( ) and [ ]
#include "data/stata_dates.h"  // the literal pseudo-function scan (liftDateLiterals)
#include "io/win_compat.h"  // cecon::to_native_path (BUG-050 wide open)

#include "macro_scanners_internal.h"
#include "macro_text_internal.h"

namespace piet::eval { namespace macro_detail {} }

namespace piet::eval::macro_detail {


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
//     reLocalDef()  = ^local\s+([A-Za-z_][A-Za-z0-9_]*|[0-9]+(?![A-Za-z0-9_]))\s*(.*)?$
//                                                                  (icase; BUG-923)
//     reGlobalDef() = ^global\s+([A-Za-z_][A-Za-z0-9_]*)\s*(.*)?$  (icase)
// (each identifier class also admits the bytes \x80-\xff -- Unicode names)
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
  if (i >= s.size()) return false;
  size_t j = i;
  if (isIdentStart(s[i])) {
    while (j < s.size() && isIdentChar(s[j])) ++j;
    // Unicode names: a global's name starts with a letter (`global ٥ 3` is
    // r(198) in StataBE; `local €x 3` is rc 0) -- see globalNameStartOk.
    if (kwLen == 6 && !globalNameStartOk(s.substr(i, j - i))) return false;
  } else if (kwLen == 5 && std::isdigit(static_cast<unsigned char>(s[i]))) {
    // BUG-923: a positional local (`local 1 "alpha"`, the do-file-level form
    // `args` reads) is a digit run; `local 1x 3` stays a non-match.
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
    if (j < s.size() && isIdentChar(s[j])) return false;
  } else {
    return false;
  }
  name.assign(s, i, j - i);
  size_t k = j;
  while (k < s.size() && isRegexSpace(s[k])) ++k;
  // `(.*)$` cannot cross a newline — see the note above.
  if (s.find('\n', k) != std::string::npos) return false;
  rest.assign(s, k, s.size() - k);
  return true;
}


// ---------------------------------------------------------------------------
// Patterns (macros.py) — names mirror the Python constants.

const std::regex& reScalarDef() {
  static const std::regex r(R"(^scalar\s+(?:define\s+)?([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*)\s*=\s*(.+)$)",
                             std::regex::icase);
  return r;
}

const std::regex& reLocalDef() {
  // BUG-923: or a positional digit run (`local 1 "alpha"`); see matchMacroDef.
  static const std::regex r(R"(^local\s+([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*|[0-9]+(?![A-Za-z0-9_\x80-\xff]))\s*(.*)?$)",
                             std::regex::icase);
  return r;
}

const std::regex& reGlobalDef() {
  static const std::regex r(R"(^global\s+([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*)\s*(.*)?$)", std::regex::icase);
  return r;
}

const std::regex& reLocalRef() {
  static const std::regex r(R"(`([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*|[0-9]+)')");
  return r;
}

const std::regex& reGlobalRefBrace() {
  static const std::regex r(R"(\$\{([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*)\})");
  return r;
}

const std::regex& reGlobalRefBare() {
  static const std::regex r(R"(\$([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*))");
  return r;
}

const std::regex& reRRef() {
  static const std::regex r(R"(\br\(([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*)\))");
  return r;
}

const std::regex& reRRefDelim() {
  static const std::regex r(R"(`r\(([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*)\)')");
  return r;
}

const std::regex& reERef() {
  // RISK-A: negative-lookahead keeps e(b)/e(V) for the matrix layer.
  static const std::regex r(R"(\be\((?!b\)|V\))([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*)\))");
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
  static const std::regex r(R"(`e\(([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*)\)')");
  return r;
}

// P4: `s(name)' — sreturn-local read (sclass programs). Delimited form
// ONLY: Stata's s() is a string-only namespace here, and the P4 task scope
// is the minimal subset the answerbook needs — no bare `s(name)` form, no
// numeric variant (unlike r(), which has both — see reRRef/reRRefDelim
// just below what this sits next to).
const std::regex& reSRefDelim() {
  static const std::regex r(R"(`s\(([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*)\)')");
  return r;
}

// BUG-1006: `c(name)' -- the delimited c-class reference. Like `e(name)' it is
// macro expansion (Stata expands it inside double quotes); the value comes from
// EvalSession::cValue.
const std::regex& reCRefDelim() {
  static const std::regex r(R"(`c\(([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*)\)')");
  return r;
}

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
  // FU-844: `~` too -- Stata's other spelling of `!` and `!=` (pyeval.cpp lexer).
  // Unicode names (lane M): `\x80-\xff` -- see SafeArithTables::ext.
  static const std::regex r(R"(^[0-9\s\+\-\*/\(\)\.\^eEa-zA-Z_,"'<>=!%&\|~\x80-\xff]+$)");
  return r;
}

const std::regex& rePostfileSafeArith() {
  // Unicode names (lane M): `\x80-\xff`, so a UTF-8 scalar name (`=سعر*2')
  // is admitted as the identifier it is.
  static const std::regex r(R"(^[0-9\s\+\-\*/\(\)\.\^eEa-zA-Z_,\x80-\xff]+$)");
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
    for (char c : std::string("_,\"'<>=!%&|~")) ext[static_cast<unsigned char>(c)] = true;  // ~: FU-844
    // Unicode names (lane M): every byte >= 0x80 -- a UTF-8 name such as
    // `ملاحظات[1]` or `سعر` reaches this gate as identifier bytes (a continuation
    // byte never collides with an ASCII operator), so `` `=ملاحظات[1]' `` was
    // refused here before anything evaluated it. `basic` stays ASCII: the fast
    // path is the empty-namespace arithmetic one and has no names at all.
    for (int i = 0x80; i < 256; ++i) ext[i] = true;
  }
};

const SafeArithTables& safeArithTables() {
  static const SafeArithTables t;
  return t;
}


// BUG-446. The literal pseudo-functions take unquoted date TEXT, and a clock
// literal has a ':' in it -- which the extended whitelist below refuses, so
// `display tc(11:00)` echoed its own text at rc 0, `local w = tc(12:00)` was
// r(198), and hh(tc(10:30)) never reached hh() (w2y_fn_datetime fd_tc_edges,
// fd_tc_full, fd_hhmm_edges, fd_minutes_edges). Each call is replaced by its
// value, parenthesised, before any pre-pass sees it -- the value the evaluator's
// own lexer would have lifted (pyeval.cpp's date-literal case), from the same
// cecon:: parser. An impossible literal is Stata's r(198) (fd_tq_bad, fd_tm_bad,
// fd_td_bad): `*bad` names it and the caller raises. Runs after
// protectStringLiterals, so text inside a string literal is never read.
std::string liftDateLiterals(const std::string& s, std::string* bad) {
  if (s.find('(') == std::string::npos) return s;
  std::string out;
  size_t copied = 0;
  cecon::for_each_datetime_literal(
      s, [&](const std::string& name, const std::string& raw, size_t begin, size_t end) {
        cecon::LiteralScale ls;
        cecon::literal_scale_for(name, ls);
        const double v = cecon::stata_datetime_literal(raw, ls);
        if (std::isnan(v)) {
          *bad = name + "(" + raw + ")";
          return false;
        }
        out.append(s, copied, begin - copied);
        out += "(" + pythonFloatRepr(v) + ")";
        copied = end;
        return true;
      });
  if (copied == 0) return s;
  out.append(s, copied, std::string::npos);
  return out;
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
// BUG-1419: a STRING scalar's value goes in as a string literal, as an e()
// string does (resolveERef): `scalar t = scalar(s1) + "y"` with s1 = "-3" is
// -3y in StataBE 19.5 and `gen str5 g = scalar(s1) + "x"` -3x; both read the
// text as the number -3 and failed r(109). A value holding `"` is carried
// compound-quoted; one that cannot be (`"'` inside) is left as the call.
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
    const std::string name = in.substr(nameStart, q - nameStart);
    const std::string* v = session.scalars.find(name);
    out.append(in, last, p - last);
    if (v != nullptr && session.isStringScalar(name)) {
      if (v->find('"') == std::string::npos) {
        out.append("\"" + *v + "\"");
      } else if (v->find("\"'") == std::string::npos && v->find("`\"") == std::string::npos) {
        out.append("`\"" + *v + "\"'");
      } else {
        out.append(in, p, end - p);
      }
    } else if (v != nullptr) {
      out.append(valueRefText(*v));  // FU-842: one operand
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

}

namespace piet::eval {
using namespace macro_detail;


bool hasBareRParen(const std::string& s) { return hasBareLetterParen(s, 'r'); }

bool hasBareEParen(const std::string& s) { return hasBareLetterParen(s, 'e'); }

}

namespace piet::eval::macro_detail {



// e(sample) T2: the one spelling of the token, applied to text whose string
// literals protectStringLiterals has already swapped out.
const std::regex& reEsampleToken() {
  static const std::regex re(R"(\be\(\s*sample\s*\))");
  return re;
}

}

namespace piet::eval {
using namespace macro_detail;


bool hasEsampleToken(const std::string& s) {
  if (s.find("sample") == std::string::npos || !hasBareEParen(s)) return false;
  std::vector<std::string> spans;
  const std::string bare = protectStringLiterals(s, &spans);
  return std::regex_search(bare, reEsampleToken());
}


std::string replaceEsampleToken(const std::string& s, const std::string& with) {
  if (!hasEsampleToken(s)) return s;
  std::vector<std::string> spans;
  const std::string bare = protectStringLiterals(s, &spans);
  // `with` is spliced verbatim: regex_replace would read a `$` in it as a group.
  std::string out;
  auto it = std::sregex_iterator(bare.begin(), bare.end(), reEsampleToken());
  size_t last = 0;
  for (; it != std::sregex_iterator(); ++it) {
    out.append(bare, last, static_cast<size_t>(it->position(0)) - last);
    out += with;
    last = static_cast<size_t>(it->position(0) + it->length(0));
  }
  out.append(bare, last, std::string::npos);
  return restoreStringLiterals(out, spans);
}

}
