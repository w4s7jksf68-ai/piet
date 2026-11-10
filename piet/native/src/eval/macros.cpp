// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// macros: see README.md for ownership and call flow.
#include "piet/eval/macros.h"
#include "estimators/postest_names.h"  // coefFullName: the engine's coefficient-name rule

#include "dispatch/dispatch_profile.h"  // WS-D1: per-line regex counter (opt-in, stderr)

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

#include "macro_definitions_internal.h"
#include "macro_expressions_internal.h"
#include "macro_matrices_internal.h"
#include "macro_results_internal.h"
#include "macro_scanners_internal.h"
#include "macro_text_internal.h"
#include "macros_internal.h"

namespace piet::eval { namespace macro_detail {} }

namespace piet::eval::macro_detail {

// MUST stay byte-identical to program.cpp's kSClassKeyPrefix — see that
// file's top-of-file note for the full rationale (no shared header exists
// for this constant; sreturn-local values live in session.rStrings, the
// SAME store `return local`/r() uses, under this reserved key prefix
// rather than a new session store).
const char* const kSClassKeyPrefix = "\x01s:";


constexpr int kMaxExpansionPasses = 16;

}

namespace piet::eval {
using namespace macro_detail;


// B4-W3b: is this line one of the MATRIX verbs — the only surface that can
// consume an r-class matrix reference (matrix/matrix_commands.cpp)? Stata's
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

// W2AK: an `estadd` line (after any capture/noisily/quietly prefixes). estadd
// reads `r(name)` ITSELF -- `estadd r(mean)` posts e(mean) (estadd)
// and `estadd scalar r(sd)` posts e(sd) (:249-255) -- so the bare r() pass
// leaves those references for the handler instead of splicing a number into
// the subcommand position (w2ak ek_ad_r_bare, ek_ad_scalar_rname).
// AER tier-1: `matselrc` too -- its row()/col() options abbreviate to r()/c(),
// so `matselrc A B, r(r3)` names a row and must not become `r(.)` (StataBE
// 2026-10-28: rc 0, one row r3; internal capture
// t1_matselrc_onename.log).
bool isEstaddLine(const std::string& line) {
  static const std::set<std::string> kPrefix = {
      "cap",  "capt", "captu", "captur", "capture", "n",      "no",     "noi",    "nois",
      "noisi", "noisil", "noisily", "qui",  "quie",  "quiet", "quietl", "quietly"};
  size_t i = 0;
  for (;;) {
    i = line.find_first_not_of(" \t", i);
    if (i == std::string::npos) return false;
    size_t j = i;
    while (j < line.size() && std::isalpha(static_cast<unsigned char>(line[j])) != 0) ++j;
    std::string w = line.substr(i, j - i);
    for (auto& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (w == "estadd" || w == "matselrc") {
      return j >= line.size() || line[j] == ' ' || line[j] == '\t';
    }
    if (kPrefix.count(w) == 0) return false;
    i = j;
    if (i < line.size() && line[i] == ':') ++i;
  }
}

}

namespace piet::eval::macro_detail {




// BUG-629 (eval half): the spellings in which a bare r(NAME) is a MATRIX operand
// that substituteMatrixRefs answers on every verb -- r(NAME)[i,j],
// rowsof(r(NAME)), colsof(r(NAME)) and el(r(NAME),i,j). w2c_postest.log family
// (1) reads a posted r(table) as `display rowsof(r(table))`, `display %18.0g
// r(table)[1,1]` and `local x : display ...`; family (5) reads nlcom's r(b) as
// `r(b)[1,1]`. Returns `s` with each operand occurrence of r(NAME) cut out, so an
// r(NAME) still present afterwards is one used as a SCALAR. NAME is an
// identifier (reRRef's grammar), so it is safe inside a pattern.
std::string cutRMatrixOperands(const std::string& s, const std::string& name) {
  if (s.find("r(" + name + ")") == std::string::npos) return s;
  const std::string ref = R"(r\()" + name + R"(\))";
  std::string out = std::regex_replace(s, std::regex(R"(\b)" + ref + R"((?=\s*\[))"), "");
  out = std::regex_replace(
      out, std::regex(R"(\b(colsof|rowsof)(\s*\(\s*))" + ref + R"((?=\s*\)))", std::regex::icase),
      "$1$2");
  out = std::regex_replace(
      out, std::regex(R"(\b(el)(\s*\(\s*))" + ref + R"((?=\s*,))", std::regex::icase), "$1$2");
  return out;
}


// The inline extended macro functions, `` `: fn args' `` anywhere in a line, all
// answered by tryExtendedMacroFn -- the arm that answers `local x : fn args`.
// BUG-1384/1379/1380: this was six per-function patterns (the matrix stripes of
// BUG-629, `: word`, the variable readers of BUG-719 with `: label (V) #` only,
// `: sortedby`, `: e(kind)`, `: char`), and every other inline function tripped
// the tokenizer's "does not expand macros" refusal (r(198)) although its two-line
// form answered: `` `: label ul 1' `` (StataBE 19.5 `Yes`), `` `: var lab u' ``,
// `` `: list A - B' ``, `` `: pwd' ``, and `` `: word count `"a b"' c' `` (2), whose
// compound quotes the `: word` pattern could not hold.
//
// A span is READY once it holds no macro reference of its own: `` `: word `i' of
// `plist'' `` waits for `i' and `plist' to expand on the pass before (w2x
// fx_nullmat_macro), and a $global in it likewise. A compound-quoted string
// inside it is an argument, not a reference. `display` is not this layer's: the
// dispatcher splices `` `: display ...' `` before the line is expanded
// (substituteInlineDisplay), so it is left alone here. Nor is a span escaped as
// `` \`: fn' ``: Stata keeps it literal (`display "\`: list sizeof A'"` prints
// `` `: list sizeof A' ``), which this layer cannot express, so it stays for the
// tokenizer's refusal rather than turning into `\3`. Returns the start of the
// rightmost ready span beginning before `before` (its closing quote in *end when
// `end` is given), or npos.
size_t findReadyInlineMacroFn(const std::string& line, size_t before, size_t* end) {
  static const std::regex reDisplayHead(R"(^\s*(?:display|displa|displ|disp|dis|di)(?:\s|$))",
                                        std::regex::icase);
  for (size_t p = before == 0 ? std::string::npos : line.rfind("`:", before - 1);
       p != std::string::npos; p = p == 0 ? std::string::npos : line.rfind("`:", p - 1)) {
    if (p > 0 && line[p - 1] == '\\') continue;
    size_t i = p + 2, quote = std::string::npos;
    int depth = 0;
    while (i < line.size()) {
      if (startsCompound(line, i)) {
        ++depth;
        i += 2;
      } else if (line[i] == '`') {
        break;  // a macro reference still to expand
      } else if (depth > 0 && line[i] == '\'') {
        // BUG-1340: a nested pair closes at its first quote, `"' or not:
        // `"[`: word count `"a b c'"'|"` prints [1| in StataBE 19.5 (the list
        // is `"a b c'"`).
        --depth;
        ++i;
      } else if (line[i] == '\'' && depth == 0) {
        quote = i;
        break;
      } else {
        ++i;
      }
    }
    if (quote == std::string::npos) continue;
    const std::string body = line.substr(p + 2, quote - p - 2);
    if (body.find('$') != std::string::npos &&
        (hasGlobalRef(reGlobalRefBrace(), body) || hasGlobalRef(reGlobalRefBare(), body)))
      continue;
    if (std::regex_search(body, reDisplayHead)) continue;
    if (end != nullptr) *end = quote;
    return p;
  }
  return std::string::npos;
}


bool usesRRefAsScalar(const std::string& s, const std::string& name) {
  const std::string cut = cutRMatrixOperands(s, name);
  if (cut.find("r(" + name + ")") == std::string::npos) return false;
  return std::regex_search(cut, std::regex(R"(\br\()" + name + R"(\))"));
}


// Stata's code for reading an r() matrix that is not there. r(NAME) is then the
// missing scalar, so `r(NAME)[i,j]` is `.[i,j]` -- r(198) "invalid syntax"
// (w2c_postest.log:363-370, rtable_survives_summarize) -- and colsof/rowsof of it
// is a scalar where a matrix is required -- r(109) "type mismatch"
// (w2c_postest.log:377-384, rtable_replaced_by_test). Decided by the FIRST bare
// reference, the one Stata's parser stops at. "" = neither spelling.
std::string absentClassMatrixRc(const std::string& s, char cls, const std::string& name) {
  std::smatch m;
  if (!std::regex_search(s, m, std::regex(R"(\b)" + std::string(1, cls) + R"(\()" + name + R"(\))")))
    return std::string();
  const size_t at = static_cast<size_t>(m.position(0));
  size_t k = at + static_cast<size_t>(m.length(0));
  while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
  if (k < s.size() && s[k] == '[') return "198";
  size_t j = at;
  while (j > 0 && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
  if (j == 0 || s[j - 1] != '(') return std::string();
  --j;
  while (j > 0 && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
  size_t w = j;
  while (w > 0 && (std::isalnum(static_cast<unsigned char>(s[w - 1])) || s[w - 1] == '_')) --w;
  std::string fn = s.substr(w, j - w);
  for (auto& c : fn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return (fn == "colsof" || fn == "rowsof" || fn == "el") ? "109" : std::string();
}


std::string absentRMatrixRc(const std::string& s, const std::string& name) {
  return absentClassMatrixRc(s, 'r', name);
}


ExpandResult rcErrorResult(const std::string& message, const std::string& rc) {
  ExpandResult r;
  r.isError = true;
  r.error = nlohmann::ordered_json(
      errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                 "r(" + rc + ")"));
  return r;
}


// The absent r(NAME) rule (internal capture
// RULING.md C.1). Both are asked only for a name in neither rStrings
// nor rValues; a posted r() matrix is never theirs (it has its own 109 below).
//
// Rule 1: Stata posts NAME for the live r-class command and PIET did not
// (EvalSession::rUnposted, recorded by handlers::rTableAfterCommand). The read is
// a named r(111) in every form and on every path -- `.` there would be a silent
// wrong answer (lincom's r(level), pinned 95 at w2h_postest_refit.log:191).
std::optional<std::string> unpostedRRefusal(const EvalSession& session, const std::string& name) {
  if (session.findClassMatrix('r', name) != nullptr) return std::nullopt;
  if (std::find(session.rUnposted.begin(), session.rUnposted.end(), name) ==
      session.rUnposted.end())
    return std::nullopt;
  if (name == "table" && !session.rTableRefusal.empty()) return session.rTableRefusal;
  return "r(" + name + ") after `" + session.rClassCmd +
         "`: Stata posts this result and this version of PIET does not, so it is refused "
         "rather than read as missing (.)";
}


// Rule 2: the live r-class command's Stata set is not recorded (class U), so an
// absent bare or delimited read cannot be told apart from a result PIET omitted.
// The if/while path keeps `.` (it never asks).
std::optional<std::string> unknownSetRRefusal(const EvalSession& session,
                                              const std::string& name) {
  if (!session.rClassSetUnknown || session.findClassMatrix('r', name) != nullptr)
    return std::nullopt;
  return "r(" + name + ") is not set after `" + session.rClassCmd +
         "`, and this version of PIET does not record which results Stata posts there; "
         "refused rather than read as missing (.)";
}


// BUG-629: `local x = r(table)[1,1]` with no r(table) died in expansion, and the
// `= expr` forms answer an expansion error by STORING THE TEXT at rc 0 -- here the
// seven characters `r(table)[1,1]`, a silent wrong value where Stata stops at
// r(198). An expansion error that carries Stata's own code (reason "r(NNN)", only
// the coded refusals above) is surfaced instead; every other expansion error keeps
// the legacy fallback. Gated on err like classifyFailedEqExpr.
bool takeCodedExpandError(const ExpandResult& exp, MacroFnError* err) {
  if (err == nullptr || !exp.isError) return false;
  const std::string reason = exp.error.value("reason", std::string());
  if (reason.size() < 4 || reason.compare(0, 2, "r(") != 0 || reason.back() != ')') return false;
  err->rc = reason.substr(2, reason.size() - 3);
  err->message = exp.error.value("message", std::string());
  return true;
}

}

namespace piet::eval {
using namespace macro_detail;


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
  // LBL-DOLLAR: `\$` is Stata's escape for a literal dollar (the backslash goes):
  // `label var m "Risk Aversion \$(r)\$"` stores `Risk Aversion $(r)$`
  // (w2ao_label_family.log:176). A line carrying one is the expander's to rewrite.
  if (rawLine.find("\\$") != std::string::npos) return true;
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
  // EM3a: `macval(NAME)' is a local reference too (expandPass splices it).
  if (tick && rawLine.find("`macval(") != std::string::npos) {
    for (size_t t = rawLine.find("`macval("); t != std::string::npos;
         t = rawLine.find("`macval(", t + 1)) {
      size_t j = t + 8;
      const size_t st = j;
      while (j < rawLine.size() && isIdentChar(rawLine[j])) ++j;
      if (j > st && j + 1 < rawLine.size() && rawLine[j] == ')' && rawLine[j + 1] == '\'')
        return true;
    }
  }
  if (dollar && hasGlobalRef(reGlobalRefBrace(), rawLine)) return true;  // Unicode names
  if (dollar && hasGlobalRef(reGlobalRefBare(), rawLine)) return true;
  // EXPRLAYER-3: the guards are the patterns' own `\b` requirement, not the
  // bare two-character literal — "r(" and "e(" both occur inside ordinary
  // identifiers (`scalar(`, `subinstr(`, `true(`, `size(`), and every one of
  // those was buying a full regex scan.
  if (hasBareRParen(rawLine) && std::regex_search(rawLine, m, reRRef())) return true;
  if (hasBareEParen(rawLine) && std::regex_search(rawLine, m, reERef())) return true;
  if (tick && rawLine.find("s(") != std::string::npos &&
      std::regex_search(rawLine, m, reSRefDelim()))
    return true;
  if (tick && rawLine.find("`c(") != std::string::npos &&
      std::regex_search(rawLine, m, reCRefDelim()))
    return true;  // BUG-1006
  if (rawLine.find("`=") != std::string::npos && std::regex_search(rawLine, m, reArithMacro()))
    return true;
  if (rawLine.find("`:") != std::string::npos &&
      findReadyInlineMacroFn(rawLine, rawLine.size(), nullptr) != std::string::npos)
    return true;  // BUG-629, BUG-719, EM13, BUG-1384
  return false;
}

}

namespace piet::eval::macro_detail {


// name-binding-display-literals: the messages of inline `=exp' references that
// failed and expanded to nothing (expandPass), for the dispatcher to print
// (takeInlineExpansionNotes). Bounded: a caller that never drains cannot grow it.
thread_local std::vector<std::string> tl_inlineNotes;

void noteInlineFailure(const std::string& message) {
  if (tl_inlineNotes.size() < 64) tl_inlineNotes.push_back(message);
}

}

namespace piet::eval {
using namespace macro_detail;


std::vector<std::string> takeInlineExpansionNotes() {
  std::vector<std::string> out;
  out.swap(tl_inlineNotes);
  return out;
}


ExpandResult expandPass(const std::string& lineIn, EvalSession& session,
                        bool unsetRAsMissing, bool verbatim) {
  dprof::EScope _z(dprof::kEExpandPass);
  std::string line = lineIn;

  // The inline extended macro functions (findReadyInlineMacroFn), the leftmost
  // ready span first: Stata expands a line left to right, and a span is ready
  // only once the spans inside it are answered, so an inner span still comes
  // before the span around it. The order shows when a span writes a macro
  // (`: subinstr ..., count(local n)'): it runs after every reference to its
  // left and before every one to its right. With x = `a b a' and n = `old',
  // StataBE 19.5 prints O1|old|b b b|2| for `display "O1|`n'|`: subinstr local
  // x "a" "b", all count(local n)'|`n'|"', and 3|z b a|1 for `: length local
  // n' on either side of the count; so such a span's prefix is expanded first,
  // and the references to its right wait for the spans and passes after it.
  // They run FIRST so a class argument (`` `: colnames r(table)' ``, `` `:
  // e(scalars)' ``) is consumed before the bare-r()/e() passes would read it as
  // a scalar (BUG-629, w2c_postest.log:102/107/757). A function that fails stops
  // the line at its rc, as the `local x : fn` form does -- but for BUG-1338: a
  // reader that fails as Stata's own reader fails (`: type nosuch', `: format
  // nosuch', `: char nosuch[x]', `: label (nosuch) 1', a stripe or `: word' of
  // nothing; r(111) or r(198)) prints its message and expands to NOTHING at rc 0,
  // as StataBE does on any line and under any prefix (`display "P|" 7 "|`: type
  // nosuch'|"` prints `variable nosuch not found` and then `P|7||`; `local a
  // "A`: type nosuch'B"` stores AB). It stopped the line at r(111). e() name
  // lists and the other functions keep their refusal.
  if (line.find("`:") != std::string::npos) {
    // BUG-719: the variable readers see a plane refreshed first
    // (EvalSession::syncVarPlane: the dispatcher syncs it only before
    // `local`/`global`/`label` lines), once per line.
    bool planeSynced = false;
    static const std::regex reCountOpt(R"(,[\s\S]*\bc[a-z]*\s*\()");
    for (;;) {
      size_t p = std::string::npos, quote = 0;
      for (size_t b = line.size(), at, q; (at = findReadyInlineMacroFn(line, b, &q)) != std::string::npos;
           b = at) {
        p = at;
        quote = q;
      }
      if (p == std::string::npos) break;
      const std::string body = line.substr(p + 2, quote - p - 2);
      size_t h = 0;
      while (h < body.size() && isPySpace(body[h])) ++h;
      size_t he = h;
      while (he < body.size() && isIdentChar(body[he])) ++he;
      const std::string head = extMacroFnName(body.substr(h, he - h));
      if (!planeSynced && session.syncVarPlane &&
          (head == "type" || head == "format" || head == "label" || head == "char" ||
           head == "variable" || head == "value")) {
        session.syncVarPlane();
        planeSynced = true;
      }
      if (head == "subinstr" && std::regex_search(body, reCountOpt)) {
        std::string prefix = line.substr(0, p);
        for (int pass = 0; pass < kMaxExpansionPasses && hasMacroRefs(prefix); ++pass) {
          ExpandResult done = expandPass(prefix, session, unsetRAsMissing, verbatim);
          if (done.isError) return done;
          if (done.line == prefix) break;
          prefix = done.line;
        }
        quote = quote - p + prefix.size();
        line = prefix + line.substr(p);
        p = prefix.size();
      }
      MacroFnError fe;
      auto v = tryExtendedMacroFn(": " + body, session, &fe);
      if (!v) {
        if (!fe.failed()) break;  // left for the tokenizer's refusal
        static const std::unordered_set<std::string> kReaders = {
            "type", "format", "variable", "value", "label", "char", "sortedby", "word",
            "rownames", "colnames", "roweq", "coleq", "rowfullnames", "colfullnames"};
        if ((fe.rc != "111" && fe.rc != "198") || kReaders.count(head) == 0)
          return rcErrorResult(fe.message, fe.rc);
        noteInlineFailure(fe.message);  // BUG-1338: expands to nothing at rc 0
        line.erase(p, quote - p + 1);
        continue;
      }
      // EM13: a characteristic's text is spliced verbatim.
      line.replace(p, quote - p + 1, verbatim && head == "char" ? encodeVerbatim(*v) : *v);
    }
  }

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
    // `=e(N)'` / `=r(mean)'` inside a double-quoted string: the e()/r() layers
    // below protect string literals (BUG-507), so a bare reference INSIDE an
    // inline expression in quotes was never substituted, this guard skipped it
    // forever, and `display "N=`=e(N)'"` died on "did not converge" (w2j_e3_rlasso
    // _e3_x `s'=`=e(`s')'). Stata evaluates the expression: a scalar is its value,
    // a macro its string, and a name that is neither is missing (`.`). A complete
    // inner text only (no macro reference left); a posted matrix stays for the
    // matrix layer.
    std::string innerSub = inner;
    if (innerSub.find('`') == std::string::npos && innerSub.find('$') == std::string::npos &&
        (std::regex_search(innerSub, m, reERef()) || std::regex_search(innerSub, m, reRRef()))) {
      bool matrixLeft = false;
      for (const std::regex* rx : {&reERef(), &reRRef()}) {
        const char cls = (rx == &reERef()) ? 'e' : 'r';
        for (const auto& [ref, nm] : findAll(*rx, innerSub)) {
          std::string text;
          if (session.findClassMatrix(cls, nm) != nullptr || nm == "sample") {
            matrixLeft = true;  // the matrix layer's / e(sample)'s, as before
            continue;
          }
          if (cls == 'e') {
            // The evaluator's own e() reading (resolveEstScalarRefs' arm):
            // string -> scalar -> `.` for an absent name.
            text = valueRefText(resolveERef(nm, session));
          } else if (const double* v = session.rValues.find(nm)) {
            text = std::isnan(*v) ? std::string(".") : valueRefText(pythonFloatRepr(*v));
          } else if (const std::string* sv = session.rStrings.find(nm);
                     sv != nullptr && sv->find('"') == std::string::npos) {
            text = "\"" + *sv + "\"";
          } else {
            matrixLeft = true;  // an r() the r layer refuses by name: its path
            continue;
          }
          innerSub = replaceFirst(innerSub, ref, text);
        }
      }
      if (matrixLeft) innerSub = inner;
    }
    if (innerSub.find('`') == std::string::npos && innerSub.find('$') == std::string::npos)
      innerSub = substituteBareCRefs(innerSub, session);  // BUG-1330
    if (!std::regex_search(innerSub, m, reLocalRef()) &&
        !hasGlobalRef(reGlobalRefBrace(), innerSub) &&
        !hasGlobalRef(reGlobalRefBare(), innerSub) &&
        !std::regex_search(innerSub, m, reRRef()) && !std::regex_search(innerSub, m, reERef())) {
      auto result = evalMacroExpr(innerSub, session);
      // name-binding-display-literals: `=exp' is the expression evaluator's,
      // as `local l = exp` is. evalMacroExpr's namespace holds numbers only, so
      // a string (`="a:b"'`), a variable (`=v'`, `=s'`), scalar() and a
      // variable before a same-named scalar were left unexpanded, and the line
      // died on "Macro expansion did not converge" (w2av_name_binding
      // bd_inl_lit, bd_inl_varsc, bd_inl_scfn, bd_inl_strvar, bd_inl_abbr ...).
      // evalArith answers them, a number in the local's own 16-digit form.
      // An expression that fails as Stata's does -- a name that binds to
      // nothing or to two variables r(111), a function Stata lacks r(133), a type
      // mismatch r(109) -- prints its message and expands to NOTHING at rc 0,
      // as StataBE does (bd_inl_unb, bd_inl_ambig, bd_inl_nofn, bd_inl_strunb,
      // bd_inl_strnum, bd_dy_ilunb: `[]` at rc 0 after `foo not found`). Any
      // other failure keeps its loud path below.
      // Only a COMPLETE expression: an inner text that still holds a macro
      // reference (`=x[`i']' is matched as `x[`i` before `i' expands) waits
      // for the pass that expands it, as before.
      if (!result && innerSub.find('`') == std::string::npos && innerSub.find('$') == std::string::npos) {
        std::string raised;
        bool isStr = false;
        if (auto ar = evalArith(innerSub, &session, &raised, &isStr)) {
          result = isStr ? *ar : capNumericTextToStataLocalDigits(*ar);
        } else {
          MacroFnError fe;
          if (classifyFailedEqExpr(innerSub, raised, &fe, &session)) {
            // The classifier names an unknown function `unknown function NAME()`.
            // A NAME that is one of Stata's functions is a capability PIET
            // lacks, not Stata's r(133): `=invFtail(2,3,.5)'` and `=invlogit(0)'
            // expanded to nothing at rc 0 where Stata gives the value
            // (corpus: 073 greshape `=ustrregexrf(...)'). That is
            // refused by name and stops the line, at the rc its `local l =`
            // sibling gives.
            std::string fn;
            static const std::string kUnknownFn = "unknown function ";
            if (fe.rc == "133" && fe.message.rfind(kUnknownFn, 0) == 0 &&
                fe.message.size() > kUnknownFn.size() + 2 &&
                fe.message.compare(fe.message.size() - 2, 2, "()") == 0) {
              fn = fe.message.substr(kUnknownFn.size(),
                                     fe.message.size() - kUnknownFn.size() - 2);
            }
            // BUG-1330: a c() nothing answered. `c(name) undefined` is
            // Stata's own failure, printed and then nothing at rc 0 as for a
            // function Stata lacks (the arm below); a real c() name this build
            // does not serve is refused by name.
            static const std::string kUndefined = ") undefined";
            const bool cRef = fe.rc == "133" && fe.message.rfind("c(", 0) == 0;
            const bool cUndefined =
                cRef && fe.message.size() > kUndefined.size() &&
                fe.message.compare(fe.message.size() - kUndefined.size(), kUndefined.size(),
                                   kUndefined) == 0;
            if (cRef && !cUndefined) {
              return rcErrorResult(fe.message + " (in `" + inner + "')", "133");
            } else if (!cRef && fe.rc == "133" && (fn.empty() || isStataFunctionName(fn))) {
              return rcErrorResult(
                  (fn.empty() ? std::string("this function") : fn + "()") +
                      " is a Stata function this version of PIET does not implement (in `" +
                      inner + "')",
                  "133");
            }
            if (fe.rc == "111" || fe.rc == "133" || fe.rc == "109") {
              noteInlineFailure(fe.message);
              result = std::string();
            }
          }
        }
      }
      if (result) {
        line = replaceFirst(line, full, *result);
      } else if (session.varObs && inner.find('[') != std::string::npos) {
        // BUG-718: a string (or missing) `VAR[i]` inside a larger expression
        // (`` `=s[1]+"z"' ``) is beyond this layer's string-free whitelist. Left
        // in place it never converged (rc 1); it is refused by name instead.
        std::optional<std::string> whole;
        bool stringLeft = false;
        substituteVarSubscripts(substituteObsSpecials(strip(inner), session), session, &whole,
                                &stringLeft);
        if (stringLeft) {
          ExpandResult r;
          r.isError = true;
          r.error = nlohmann::ordered_json(unsupportedMacro(full));
          return r;
        }
      }
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
          // A missing e() scalar is Stata's `.`, never the text "nan" (which the
          // evaluator then read as a variable name: `display %21x e(slambda)` r(111)).
          line = replaceFirst(line, full,
                              std::isnan(*v) ? std::string(".")
                                             : valueRefText(pythonFloatRepr(*v)));  // FU-842
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
  static const std::regex reMatrixList(R"(^\s*mat(?:r|ri|rix)?\s+l(?:i|is|ist)?\s)");
  const bool matrixListLine = matrixVerbLine && std::regex_search(line, reMatrixList);
  const bool estaddLine = isEstaddLine(line);
  for (const std::regex* rx : {&reRRefDelim(), &reRRef()}) {
    if (rx == &reRRef() && estaddLine) continue;
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
      // matrix/matrix_commands.cpp already knows how to copy — died HERE with
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
      // `matrix list r(NAME)` names a MATRIX: r(mean) after summarize is none,
      // and StataBE answers r(111) `matrix r(mean) not found` (2026-11-03
      // oracle, BUG-1409), so the handler answers it, never a spliced value.
      if (rx == &reRRef() && matrixListLine) continue;
      // BUG-560 / BUG-629: the same exemption on ANY verb when every reference
      // to the posted matrix on the line is a matrix OPERAND (el(r(NAME),i,j),
      // r(NAME)[i,j], rowsof/colsof(r(NAME)); see cutRMatrixOperands) --
      // substituteMatrixRefs answers those, and only ever sees them if this loop
      // leaves them in place. `local F1=el(r(singleresults),1,4)` (paper 044),
      // `display rowsof(r(table))` and `local x = r(table)[1,1]` (w2c_postest.log
      // family (1)). A bare `display r(table)` still refuses, below.
      if (rx == &reRRef() && session.findClassMatrix('r', name) != nullptr &&
          !usesRRefAsScalar(line, name))
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
        // AN-6: a value holding '"' arrives as Stata's compound literal `"..."',
        // the one spelling that can carry it (`strlen(r(labels))` over coefplot's
        // `1 `"First regressor"' ...`, w2an_coefplot_family.log:355). The old
        // `\"` escape is not Stata (BUG-502: no escapes in a literal) and ended
        // the string at the first inner quote. A value is never macro text, so
        // its own ` and $ are spliced encoded.
        if (rx == &reRRef()) {
          const std::string body = verbatim ? encodeVerbatim(*s) : *s;
          const std::string lit = s->find('"') == std::string::npos
                                      ? "\"" + body + "\""
                                      : "`\"" + body + "\"'";
          line = replaceFirst(line, full, lit);
        } else {
          line = replaceFirst(line, full, *s);
        }
      } else if (const double* v = session.rValues.find(name)) {
        // repr(float(val)); a missing r() (meanonly's r(sd)) goes in as its token.
        // FU-842: the bare r(NAME) is a value (one operand); `r(NAME)' is macro text.
        line = replaceFirst(line, full,
                            barePass ? valueRefText(matrixElementText(*v)) : macroRealText(*v));
      } else if (auto why = unpostedRRefusal(session, name)) {
        // RULING.md C.1 rule 1: a result Stata posts for the live r-class command
        // and PIET did not -- refused in every form and on every path.
        dprof::ezoneEnd(dprof::kEExpandRBlock);
        return rcErrorResult(*why, "111");
      } else if (unsetRAsMissing) {
        // Stata: an unset r() in a numeric expression is missing (`.`). Used by
        // the if/while condition path so `!missing(r(estimate))` guards work
        // after a failed capture (the `.` becomes the missing sentinel in
        // evalBoolExpr's replaceStataMissingLiterals). No error — this is legal Stata.
        line = replaceFirst(line, full, ".");
      } else if (rx == &reRRef()) {
        // BUG-629: a posted MATRIX used as a scalar (`display r(table)`). It is
        // set, so "is not set" was the wrong reason, and rc 1 no code at all.
        // r(109) "type mismatch": w2f_residuals.log:1625
        // `PREC|e_rtable_display|0|109|` (and the mirror case, a scalar where a
        // matrix is needed, w2c_postest.log:377-384).
        if (session.findClassMatrix('r', name) != nullptr) {
          dprof::ezoneEnd(dprof::kEExpandRBlock);
          return rcErrorResult("type mismatch: r(" + name +
                                   ") is a matrix; read it as r(" + name +
                                   ")[i,j], rowsof(), colsof(), or copy it with `matrix`",
                               "109");
        }
        // An estimation (or margins, or a replicating prefix) after which Stata
        // posts r(table) but this build does not -- interaction (#) columns, say --
        // leaves the reason in EvalSession::rTableRefusal (postestimation/rtable_rclass_lifecycle.cpp),
        // so a read is a NAMED refusal instead of the r(198)/r(109) of a table
        // Stata would not have either.
        if (name == "table" && !session.rTableRefusal.empty()) {
          dprof::ezoneEnd(dprof::kEExpandRBlock);
          return rcErrorResult(session.rTableRefusal, "111");
        }
        // On a MATRIX verb an absent name is the matrix layer's to answer, as a
        // posted one is (the exemption at the top of this loop): `matrix T =
        // r(nosuch)` is rc 0 in Stata (w2f_residuals.log:1675) with content no arm
        // pins yet, and that grammar lives in matrix/matrix_commands.cpp runMatrixDef.
        // A `matrix NAME = ...` definition reads an absent r(NAME) as the missing
        // value, a 1x1 `.` matrix (w2f_residuals.log:1675 `matrix T = r(nosuch)`
        // rc 0; w2n_followups.log:938 E05, :1027 E10, :1043 E11 `capture matrix
        // __w2nT = r(table)` rc 0 after r-class commands that post no table).
        if (matrixVerbLine) {
          static const std::regex reMatDefine(R"(^\s*mat(?:r|ri|rix)?\s+[A-Za-z_\x80-\xff][\w\x80-\xff]*\s*=)");
          if (std::regex_search(line, reMatDefine)) line = replaceFirst(line, full, ".");
          continue;
        }
        // RULING.md C.1 rule 2, after the matrix-verb hand-off (R5) and before
        // the 198/109 below, which assert a Stata error this build cannot know
        // Stata raises after an undeclared command.
        if (auto why = unknownSetRRefusal(session, name)) {
          dprof::ezoneEnd(dprof::kEExpandRBlock);
          return rcErrorResult(*why, "111");
        }
        const std::string rc = absentRMatrixRc(line, name);
        if (!rc.empty()) {
          dprof::ezoneEnd(dprof::kEExpandRBlock);
          return rcErrorResult(rc == "198" ? "invalid syntax: r(" + name + ") is not set"
                                           : "type mismatch: r(" + name + ") is not set",
                               rc);
        }
        // An r() name outside the posted set of the live r-class command (or
        // after a class-X command: an rclass program's return set, an emptied
        // r()) is the missing value in an expression, at rc 0 (w2n_followups.log:968
        // E07 prints `.` for r(N)/r(mean) after `regress`; RULING.md C.1 rule 3).
        // It was a refusal at rc 1 here, and `local v = r(N)` then stored the TEXT
        // `r(N)` (FU-91). The display twin, `generate` and the `count if` CiParser
        // see the line after this pass, so they inherit the `.`.
        line = replaceFirst(line, full, ".");
      } else {
        // The delimited form `r(NAME)' is MACRO expansion: outside the live
        // command's posted set an unset name expands to the empty string at rc 0,
        // as an undefined local and an unset `e(NAME)' do (the e() block below;
        // BUG-294). After an undeclared command it refuses (C.1 rule 2).
        if (auto why = unknownSetRRefusal(session, name)) {
          dprof::ezoneEnd(dprof::kEExpandRBlock);
          return rcErrorResult(*why, "111");
        }
        line = replaceFirst(line, full, std::string());
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
        line = replaceFirst(line, full, macroRealText(*ev));  // BUG-1006
      } else {
        // Stata macro semantics: an unset e() macro reference expands to the
        // empty string at rc 0 — the same rule BUG-294 measured for an
        // undefined local, and the reason `if "`e(nosuch)'" == ""` works.
        const std::string sub = resolveEScalar(name, session);
        line = replaceFirst(line, full,
                            sub == "." ? std::string() : capNumericTextToStataLocalDigits(sub));
      }
    }
  }

  // --- c() references (BUG-1006): delimited form `c(name)' only. The bare
  // c(name) is an expression reference and belongs to substituteCRefs at the
  // expression call sites. Every hk_*_g record of w2aq_histogram_family.log
  // prints `c(k)' inside a display string. A name Stata has no c() value for
  // expands to "", as in Stata (isStataCClassName). A real c() name outside
  // resolveCValue's spine is refused here, by name and with its code, as the
  // bare form is: left in the line it was stored as raw text by `local x
  // "`c(name)'"` (FU-1413), and a nested `c(`k')' never converged.
  // A numeric value is spliced as macro text, the stored-local rule `r(N)'
  // takes: `c(maxdouble)' is 8.9884656743e+307 and `c(pi)' 3.141592653589793
  // (StataBE 19.5). The hook's own text keeps every bit, for the inline
  // `=c(epsdouble)*2^52' (substituteBareCRefs), which is 1.
  if (session.cValue && line.find("`c(") != std::string::npos) {
    for (const auto& [full, name] : findAll(reCRefDelim(), line)) {
      std::optional<std::string> cv = session.cValue(name);
      if (!cv && !isStataCClassName(name)) cv = std::string();
      if (!cv) return rcErrorResult(cClassRefusalMessage(name), cClassRefusalRc(name));  // coded: `local` stops too
      if (!isStataCClassStringName(name) && !cv->empty()) {
        char* end = nullptr;
        const double d = std::strtod(cv->c_str(), &end);
        if (end == cv->c_str() + cv->size() && std::isfinite(d)) cv = stataMacroRealText(d);
      }
      line = replaceFirst(line, full, *cv);
    }
  }

  // --- s() references (P4, sreturn local): delimited form `s(name)' only —
  // same site as r()/e() just above. Values live in session.rStrings (the
  // same store r()-strings use) under kSClassKeyPrefix so they can never
  // collide with a real r()-name (those are captured by an identifier-only
  // regex — reRRefDelim/reRRef above — so no r()-set key can ever equal our
  // prefixed form). An unset s() name is a macro that expands to nothing, on
  // every path: the s() store holds exactly what sclass programs posted, and
  // regress, logit, lincom, ... empty it (BUG-1512; StataBE 19.5, rlife/s3.do:
  // `[`s(nosuch)']` is `[]` and `if "`s(foo)'" == ""` is true after `regress`).
  for (const auto& [full, name] : (line.find('`') != std::string::npos &&
                                   line.find("s(") != std::string::npos)
                                     ? findAll(reSRefDelim(), line)
                                     : std::vector<std::pair<std::string, std::string>>{}) {
    const std::string* s = session.rStrings.find(std::string(kSClassKeyPrefix) + name);
    line = replaceFirst(line, full, s != nullptr ? *s : std::string());
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
  // an identifier ([A-Za-z_][A-Za-z0-9_]*, plus the bytes >= 0x80 of a
  // Unicode name -- isIdentStart/isIdentChar) or a positional digit run ([0-9]+),
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
      // EM3a: `macval(NAME)' inserts NAME's value WITHOUT expanding the
      // references inside it (w2af_macro_text_file.log:983 mt_mv_tick `<p`mtl'q>`,
      // :995 mt_mv_dollar `<p$mtg q>`, mt_mv_undef `<>` for an undefined local).
      // A plain `NAME' re-expands them on the next pass (mt_mv_tick_plain
      // `<pLVALq>`). The NAME grammar is the reference's own; anything else after
      // `macval(` is not this form and is copied through like any non-reference.
      if (line.compare(j, 7, "macval(") == 0) {
        size_t k = j + 7;
        const size_t mvStart = k;
        if (k < line.size() && isIdentStart(line[k])) {  // Unicode names: shared rule
          ++k;
          while (k < line.size() && isIdentChar(line[k])) ++k;
        } else {
          while (k < line.size() && std::isdigit(static_cast<unsigned char>(line[k]))) ++k;
        }
        if (k > mvStart && k + 1 < line.size() && line[k] == ')' && line[k + 1] == '\'') {
          const std::string* mv = session.locals.find(line.substr(mvStart, k - mvStart));
          out.append(line, i, tick - i);
          if (mv != nullptr) out += verbatim ? encodeVerbatim(*mv) : *mv;
          i = k + 2;
          spliced = true;
          continue;
        }
      }
      const size_t nameStart = j;
      if (j < line.size() && isIdentStart(line[j])) {  // Unicode names: shared rule
        ++j;
        while (j < line.size() && isIdentChar(line[j])) ++j;
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
                                      ? findAllGlobalRefs(reGlobalRefBrace(), line)
                                      : std::vector<std::pair<std::string, std::string>>{}) {
    const std::string* v = session.globals.find(name);
    static const std::string kEmpty;  // BUG-294 — see the local arm above.
    if (v == nullptr) v = &kEmpty;
    line = replaceFirst(line, full, *v);
  }
  for (const auto& [full, name] : line.find('$') != std::string::npos
                                      ? findAllGlobalRefs(reGlobalRefBare(), line)
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

}

namespace piet::eval::macro_detail {


// ---------------------------------------------------------------------------
// expand_macros

// LBL-DOLLAR: Stata's `\$` escape -- a literal dollar that is never a global
// reference. It travels as the verbatim-dollar sentinel and decodes to `$`.
std::string encodeDollarEscapes(const std::string& s) {
  if (s.find("\\$") == std::string::npos) return s;
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '$') {
      out += kVerbatimDollar;
      ++i;
    } else {
      out += s[i];
    }
  }
  return out;
}

}

namespace piet::eval {
using namespace macro_detail;


ExpandResult expandMacros(const std::string& rawLine, EvalSession& session,
                          bool unsetRAsMissing, bool keepVerbatim) {
  std::string line = encodeDollarEscapes(rawLine);

  for (int pass = 0; pass < kMaxExpansionPasses; ++pass) {
    if (!hasMacroRefs(line)) break;  // fully resolved
    ExpandResult result = expandPass(line, session, unsetRAsMissing, /*verbatim=*/true);
    if (result.isError) return result;  // propagate error immediately
    if (result.line == line) break;     // no progress — stuck
    line = result.line;
  }

  // Compound double-quotes `"..."' are STRING delimiters. EM3b: a span whose body
  // holds no '"' becomes a plain "..." (what the tokenizer and every literal
  // scanner read); one that holds a quote, or nests, stays compound -- the old
  // non-greedy regex ended such a span at its first inner "' and handed the
  // evaluator `strlen("a "b" c")` (w2af mt_fn_strlen, w2k do:697). The scan
  // counts a macval()-encoded tick as a tick, so a value's own `"..."' nests.
  // BUG-1527: a fold only shortens the line; the unfolded one is kept for r.typed.
  std::string typedLine;
  {
    std::string folded = normalizeCompoundQuotes(line);
    if (folded.size() != line.size()) typedLine.swap(line);
    line = std::move(folded);
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
  // BUG-1006: a `c(name)' the c() pass could not resolve is left for the
  // unsupported-macro refusal downstream, which names it; it is not a runaway.
  // Cut before the mask: blanking its parentheses would leave `c_name_', which
  // reads as a local reference.
  std::string lineNoCBuf;
  if (line.find("`c(") != std::string::npos) {
    lineNoCBuf = std::regex_replace(line, reCRefDelim(), "");
  }
  const std::string lineMasked = maskParensInQuotes(lineNoCBuf.empty() ? line : lineNoCBuf);
  const std::string& lineNoE = hasBareEParen(lineMasked)  // EXPRLAYER-3
                                   ? (lineNoEBuf = std::regex_replace(lineMasked, reERef(), ""))
                                   : lineMasked;
  // B4-W3b: the SAME exemption for an r-class MATRIX reference. The bare-r()
  // pass above deliberately leaves `r(table)` / `r(Stat1)` in place for the
  // matrix layer (see its comment), so without this the guard would read an
  // intentionally-unexpanded reference as a runaway macro and report a
  // circular definition for a line that is simply not the macro layer's to
  // finish. On a matrix verb the pass leaves every bare r(NAME) it does not
  // splice -- a posted matrix, and an absent name, which the handler answers
  // (BUG-1409: `matrix list r(nosuch)` is r(111) `matrix r(nosuch) not found`
  // in StataBE, whether or not any r() matrix is posted) -- so all of them go.
  std::string lineNoRBuf;
  const std::string* checkLine = &lineNoE;
  if (hasBareRParen(*checkLine) && (isEstaddLine(line) || isMatrixVerbLine(line))) {
    // W2AK: the bare r() pass leaves an estadd line's r() references for the
    // handler (isEstaddLine); they are not a runaway.
    lineNoRBuf = std::regex_replace(*checkLine, reRRef(), "");
    checkLine = &lineNoRBuf;
  } else if (hasBareRParen(*checkLine) && !session.rMatrices.empty()) {
    // BUG-560 / BUG-629: the bare-r() pass also leaves a posted matrix's OPERAND
    // spellings (el(r(NAME),i,j), r(NAME)[i,j], rowsof/colsof(r(NAME))) for
    // substituteMatrixRefs. Strip only those, the same cut the pass decided by,
    // so any other r() left on the line still reads as unexpanded.
    lineNoRBuf = *checkLine;
    for (const auto& [nm, mtx] : session.rMatrices) {
      (void)mtx;
      lineNoRBuf = cutRMatrixOperands(lineNoRBuf, nm);
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
             "definition. Line after expansion: " + pyRepr(decodeVerbatim(line))}};
    return r;
  }

  ExpandResult r;
  r.line = keepVerbatim ? line : decodeVerbatim(line);
  if (!typedLine.empty()) r.typed = keepVerbatim ? typedLine : decodeVerbatim(typedLine);
  return r;
}


ExpandResult expandDefinitionText(const std::string& text, EvalSession& session) {
  std::string line = encodeDollarEscapes(text);
  for (int pass = 0; pass < kMaxExpansionPasses; ++pass) {
    if (!hasMacroRefs(line)) break;
    ExpandResult result = expandPass(line, session, false, /*verbatim=*/true);
    if (result.isError) {
      if (result.error.is_object() && result.error.contains("message") &&
          result.error["message"].is_string())
        result.error["message"] = decodeVerbatim(result.error["message"].get<std::string>());
      return result;
    }
    if (result.line == line) break;
    line = result.line;
  }
  ExpandResult r;
  r.line = decodeVerbatim(std::move(line));
  return r;
}

}
