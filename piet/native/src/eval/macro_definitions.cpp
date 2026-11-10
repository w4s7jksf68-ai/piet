// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// macro definitions: see README.md for ownership and call flow.
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
#include "macro_results_internal.h"
#include "macro_scanners_internal.h"
#include "macro_text_internal.h"
#include "macros_internal.h"

namespace piet::eval { namespace macro_detail {} }

namespace piet::eval::macro_detail {


// The extended macro functions tryExtendedMacroFn answers, by Stata's name and
// shortest abbreviation ([P] macro): `tok` -> the full name, "" for any other
// token. Case-sensitive, as Stata is: StataBE 19.5 answers `: t u`, `: fo u`,
// `: lab ul 1`, `: sort`, `: rown M`, `: colf M`, `: rowe M` and refuses `: T u`,
// `: TYPE u`, `: Word count a`, `: COLNAMES M`, `: colso M`, `: sor`, `: la ul 1`,
// `: dat label`, `: pw`, `: subinst` with "<tok> not allowed", r(101).
std::string extMacroFnName(const std::string& tok) {
  struct Name {
    const char* full;
    size_t shortest;
  };
  static constexpr Name kNames[] = {
      {"type", 1},     {"format", 1},   {"value", 3},        {"variable", 3},     {"data", 4},
      {"sortedby", 4}, {"label", 3},    {"char", 4},         {"list", 4},         {"word", 4},
      {"length", 6},   {"subinstr", 8}, {"pwd", 3},          {"dir", 3},          {"rownames", 4},
      {"colnames", 4}, {"roweq", 4},    {"rowfullnames", 4}, {"colfullnames", 4}, {"coleq", 4},
      {"rowsof", 6},   {"colsof", 6}};
  for (const auto& n : kNames) {
    const std::string full = n.full;
    if (tok.size() >= n.shortest && full.compare(0, tok.size(), tok) == 0) return full;
  }
  return std::string();
}


// Stata's check of a macro NAME an extended function reads or writes (`: list`
// operands, `: length`, `: subinstr` and its count()): a local is stored as `_`
// plus its name, so it may start with a digit but holds at most 31 characters; a
// global is a Stata name of up to 32. StataBE 19.5, r(198): "_abcdefghijabcdefgh
// ijabcdefghijab invalid name" for a 32-character local, "1A invalid name" for
// global(1A), "_) invalid name" for local(). "" when the name is good.
std::string macroNameError(bool global, const std::string& name) {
  const std::string stored = global ? name : "_" + name;
  bool ok = !stored.empty() && utf8Len(stored) <= 32 && cecon::ident::isIdentStartByte(stored[0]);
  for (size_t i = 0; ok && i < stored.size(); ++i) ok = isIdentChar(stored[i]);
  return ok ? std::string() : stored + " invalid name";
}


// ---------------------------------------------------------------------------
// _try_extended_macro_fn



// BUG-216/217 helpers: the variable-metadata plane SessionState projects into
// eval.varLabels under reserved keys (see SessionState::syncEvalVarLabels).
//
// MUST stay byte-identical to session_state.h's kEvalVar*Prefix constants — the
// same discipline kSClassKeyPrefix above documents for the s-class store, and for
// the same reason: two spellings of one key is a silent-miss generator.
const char* const kVarExistsPrefix = "\x01v:x:";

const char* const kVarFormatPrefix = "\x01v:f:";

const char* const kVarLblsetPrefix = "\x01v:l:";

const char* const kVarTypePrefix = "\x01v:t:";
  // BUG-250: declared storage subtype
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

  // r(198): a form whose arguments do not parse, a bad macro name
  // (macroNameError), or a variable reader with no variable (StataBE 19.5:
  // `: type`, `: format`, `: var l` "nothing found where name expected").
  auto rejectSyntax = [&](const std::string& message = "invalid syntax") {
    if (err != nullptr) {
      err->rc = "198";
      err->message = message;
    }
    return std::nullopt;
  };

  // The function's name, written out in full (extMacroFnName): `: lab ul 1` is
  // answered as `: label ul 1`. The second word of `value label`, `variable
  // label` and `data label` abbreviates to `l`; after value/variable anything
  // else is "<word> not allowed" r(101) (StataBE 19.5 `: variable LABEL u`,
  // `: var lbl u`), after data it is r(198).
  size_t tokEnd = 1;
  while (tokEnd < rhs.size() && isPySpace(rhs[tokEnd])) ++tokEnd;
  const size_t tokStart = tokEnd;
  while (tokEnd < rhs.size() && isIdentChar(rhs[tokEnd])) ++tokEnd;
  const std::string tok = rhs.substr(tokStart, tokEnd - tokStart);
  const std::string fn = extMacroFnName(tok);
  if (!fn.empty()) {
    std::string rest = rhs.substr(tokEnd);
    if (fn == "value" || fn == "variable" || fn == "data") {
      size_t w = 0;
      while (w < rest.size() && isPySpace(rest[w])) ++w;
      size_t we = w;
      while (we < rest.size() && isIdentChar(rest[we])) ++we;
      const std::string word = rest.substr(w, we - w);
      if (word.empty() || std::string("label").compare(0, word.size(), word) != 0) {
        if (fn == "data") return rejectSyntax();
        if (err != nullptr) {
          err->rc = "101";
          err->message = word + " not allowed";
        }
        return std::nullopt;
      }
      rest = " label" + rest.substr(we);
    }
    rhs = ": " + fn + rest;
  }
  // A form Stata knows that this layer does not answer: a refusal naming it, the
  // r(101) an unknown function gets.
  auto refuseUnanswered = [&]() {
    if (err != nullptr) {
      std::string head = strip(rhs.substr(1));
      head = head.substr(0, head.find_first_of(" \t"));
      err->rc = "101";
      err->message = (head.empty() ? std::string("extended macro function") : head) + " not allowed";
    }
    return std::nullopt;
  };

  std::smatch m;

  // These are names of the actual live postings, in publication order. No
  // sorting, deleted entries, or zero/missing placeholders are manufactured.
  // Fixed oracle: w2v_estpost_ematrix.log ep13_ematrices / ep13_escalars.
  // `e(` with anything else is r(198) (StataBE 19.5 `: e(SCALARS)`, `: e(foo)`,
  // `: e()`, `: e`), except Stata's own `: e(functions)`, not answered here.
  static const std::regex reENames(R"(^:\s*e\(\s*(scalars|macros|matrices)\s*\)\s*$)");
  if (tok == "e" && !std::regex_match(rhs, m, reENames)) {
    static const std::regex reEFunctions(R"(^:\s*e\(\s*functions\s*\)\s*$)");
    return std::regex_match(rhs, reEFunctions) ? refuseUnanswered() : rejectSyntax();
  }
  if (std::regex_match(rhs, m, reENames)) {
    std::string kind = m.str(1), out;
    // A fit that published Stata's ordered list for this kind (and whose e() is
    // still exactly that posting) answers in Stata's order, which is not the
    // stores' insertion order (SessionState::eNameOrderFor).
    if (session.eNameOrder) {
      if (const auto names = session.eNameOrder(kind)) {
        for (const auto& n : *names) {
          if (!out.empty()) out += ' ';
          out += n;
        }
        return out;
      }
    }
    const bool complete = session.eNameListsComplete
                              ? session.eNameListsComplete(kind)
                              : !session.lastModelResult.has_value();
    if (!complete) {
      if (err != nullptr) {
        err->rc = "111";
        err->message = ": e(" + m.str(1) + ") is not available for this fit: its complete "
                       "ordered result namespace has not been published";
      }
      return std::nullopt;
    }
    auto append = [&](const std::string& name) {
      if (name.empty() || isReservedStoreKey(name)) return;
      if (!out.empty()) out += ' ';
      out += name;
    };
    if (kind == "scalars") {
      for (const auto& item : session.eScalars.items()) append(item.first);
    } else if (kind == "macros") {
      for (const auto& item : session.eStrings.items()) append(item.first);
    } else {
      for (const auto& item : session.eMatrices) append(item.first);
    }
    return out;
  }

  // name-binding-display-literals: Stata's words -- blanks separate them and a
  // double-quoted string, simple or compound, is ONE word without its quotes.
  // The split on blanks alone kept the quotes, so `local w : word 2 of `"
  // "a-b" "c-d" "'` stored `"c-d"`, which display then had to echo as a bare
  // name (w2af_macro_text_file.log:780 `<Clickbait-Incentives>`, :840 `<10>|<0.2>`
  // for a list of quoted labels, :939 `<2>|<one two>` for `"one two" three`).
  // Each element is kept both as written (`raw`, quotes on: what `: list uniq`
  // and its siblings hand back) and as compared (`text`, quotes off).
  struct ListElem {
    std::string raw, text;
  };
  const auto stataElems = [](const std::string& lst) {
    std::vector<ListElem> words;
    size_t i = 0;
    while (i < lst.size()) {
      if (std::isspace(static_cast<unsigned char>(lst[i]))) {
        ++i;
      } else if (lst[i] == '"') {
        const size_t j = lst.find('"', i + 1);
        if (j == std::string::npos) {  // unclosed: the rest is the string (StataBE
          // 19.5: `a "b c` has sizeof 2, word 2 `b c`, retok `a "b c"`)
          words.push_back({lst.substr(i) + "\"", lst.substr(i + 1)});
          break;
        }
        words.push_back({lst.substr(i, j + 1 - i), lst.substr(i + 1, j - i - 1)});
        i = j + 1;
      } else if (lst[i] == '`' && i + 1 < lst.size() && lst[i + 1] == '"') {
        int depth = 0;
        size_t j = i;
        while (j < lst.size()) {
          if (j + 1 < lst.size() && lst[j] == '`' && lst[j + 1] == '"') {
            ++depth;
            j += 2;
          } else if (j + 1 < lst.size() && lst[j] == '"' && lst[j + 1] == '\'') {
            j += 2;
            if (--depth == 0) break;
          } else {
            ++j;
          }
        }
        const size_t innerEnd = depth == 0 ? j - 2 : lst.size();
        words.push_back({lst.substr(i, j - i),
                         lst.substr(i + 2, innerEnd > i + 2 ? innerEnd - (i + 2) : 0)});
        i = j;
      } else {
        size_t j = i;
        while (j < lst.size() && !std::isspace(static_cast<unsigned char>(lst[j]))) ++j;
        words.push_back({lst.substr(i, j - i), lst.substr(i, j - i)});
        i = j;
      }
    }
    return words;
  };
  const auto stataWords = [&stataElems](const std::string& lst) {
    std::vector<std::string> words;
    for (auto& e : stataElems(lst)) words.push_back(std::move(e.text));
    return words;
  };

  // word count of <list>
  // serset-twoway: the list may be EMPTY after expansion (`: word count `vl''` with
  // vl empty), which leaves no blank after `count`; that is 0, not `word not
  // allowed` (the same for `word # of` below: w2al tf_ss's `local sv : word `j' of
  // `svl'' with no sort-variable list, w2al_twoway_family.do:125). `of` here is
  // a word of the list (StataBE 19.5 `: word count of a b` is 3).
  static const std::regex reWordCount(R"(^:\s*word\s+count(?:\s+([\s\S]*))?$)");
  if (std::regex_match(rhs, m, reWordCount)) {
    std::string lst = strip(m.str(1));
    auto words = lst.empty() ? std::vector<std::string>{} : stataWords(lst);
    return std::to_string(words.size());
  }

  // word N of <list>; N is a positive integer (StataBE 19.5: `: word 0 of a b`,
  // `-1`, `1.5` r(198); `01` is 1).
  static const std::regex reWordN(R"(^:\s*word\s+(\d+)\s+of(?:\s+([\s\S]*))?$)");
  if (std::regex_match(rhs, m, reWordN)) {
    long long n = 0;
    try {
      n = std::stoll(m.str(1));
    } catch (const std::exception&) {
      return std::string();  // absurd index -> out of range -> ""
    }
    if (n == 0) return rejectSyntax();
    std::string lst = strip(m.str(2));
    auto words = lst.empty() ? std::vector<std::string>{} : stataWords(lst);
    if (1 <= n && n <= static_cast<long long>(words.size())) {
      return words[static_cast<size_t>(n - 1)];
    }
    return std::string();  // out of range -> empty (Stata-faithful)
  }

  // `local` and `global`, as `: length` and `: subinstr` take them: StataBE 19.5
  // answers `l`, `lo`, `loc`, `g`, `gl`, `glob` and refuses `L`, `LOCAL` r(198).
  static const std::string kLocalOrGlobal = R"((l(?:o(?:c(?:al?)?)?)?|g(?:l(?:o(?:b(?:al?)?)?)?)?))";

  // length local|global <name>. No string form: `: length "abc"` is r(198) in
  // StataBE 19.5.
  static const std::regex reLength(R"(^:\s*length\s+)" + kLocalOrGlobal +
                                   R"(\s+([\w\x80-\xff]+)\s*$)");
  if (std::regex_match(rhs, m, reLength)) {
    const bool global = m.str(1)[0] == 'g';
    const std::string bad = macroNameError(global, m.str(2));
    if (!bad.empty()) return rejectSyntax(bad);
    return std::to_string(
        utf8Len(global ? session.globals.get(m.str(2), "") : session.locals.get(m.str(2), "")));
  }

  // subinstr local|global <name> <from> <to> [, all word count(local|global <n>)]
  // EM10: <from>/<to> are "..." or compound `"..."' (nesting, inner quotes kept),
  // and the options come in any order (w2af_macro_text_file.log :1393
  // mt_xmf_subinstr_q `say QhiQ now` for `"""' -> "Q"; :1407 count(local nch)).
  // Measured on StataBE 19.5: `word` abbreviates to `w` and `count` to `c`, `all`
  // not at all, and the names are case-sensitive (`, a`, `, ALL`, `, Word`:
  // "option a not allowed" r(198)); an option given twice is "option all not
  // allowed" / "option c() not allowed"; count() holds `local|global NAME` or is
  // r(198), or "option count() incorrectly specified" with more after the name;
  // text after <to> that is not an option is "varlist not allowed" r(101).
  static const std::regex reSubinstrHead(R"(^:\s*subinstr\s+)" + kLocalOrGlobal +
                                         R"(\s+([\w\x80-\xff]+)\s+)");
  if (fn == "subinstr") {
    if (!std::regex_search(rhs, m, reSubinstrHead)) return rejectSyntax();
    const bool fromGlobal = m.str(1)[0] == 'g';
    const std::string srcName = m.str(2);
    if (const std::string bad = macroNameError(fromGlobal, srcName); !bad.empty())
      return rejectSyntax(bad);
    size_t pos = static_cast<size_t>(m.length(0));
    auto readArg = [&](std::string* out) -> bool {
      while (pos < rhs.size() && isPySpace(rhs[pos])) ++pos;
      if (startsCompound(rhs, pos)) {
        const size_t e = compoundSpanEnd(rhs, pos);
        if (e == std::string::npos) return false;
        *out = rhs.substr(pos + 2, e - pos - 4);
        pos = e;
        return true;
      }
      if (pos < rhs.size() && rhs[pos] == '"') {
        const size_t e = rhs.find('"', pos + 1);
        if (e == std::string::npos) return false;
        *out = rhs.substr(pos + 1, e - pos - 1);
        pos = e + 1;
        return true;
      }
      return false;
    };
    std::string oldS, newS;
    if (!readArg(&oldS) || !readArg(&newS)) return rejectSyntax();
    bool allFlag = false, wordFlag = false, countFlag = false;
    std::string countKind, countName;
    while (pos < rhs.size() && isPySpace(rhs[pos])) ++pos;
    if (pos < rhs.size() && rhs[pos] != ',') {
      if (err != nullptr) {
        err->rc = "101";
        err->message = "varlist not allowed";
      }
      return std::nullopt;
    }
    const std::string opts = pos < rhs.size() ? rhs.substr(pos + 1) : std::string();
    static const std::regex reOpt(R"(\s*([\w\x80-\xff]+)(\s*\(([^)]*)\))?\s*)");
    static const std::regex reCountArg(R"(^\s*)" + kLocalOrGlobal + R"(\s+([\w\x80-\xff]+)\s*([\s\S]*)$)");
    size_t op = 0;
    std::smatch om, cm;
    while (op < opts.size() && !strip(opts.substr(op)).empty()) {
      const std::string tail = opts.substr(op);
      if (!std::regex_search(tail, om, reOpt, std::regex_constants::match_continuous))
        return rejectSyntax();
      const std::string name = om.str(1);
      const bool isAll = name == "all" && !om[2].matched;
      const bool isWord = !om[2].matched && std::string("word").compare(0, name.size(), name) == 0;
      const bool isCount = om[2].matched && std::string("count").compare(0, name.size(), name) == 0;
      bool* flag = isAll ? &allFlag : isWord ? &wordFlag : isCount ? &countFlag : nullptr;
      if (flag == nullptr || *flag)
        return rejectSyntax("option " + name + (om[2].matched ? "()" : "") + " not allowed");
      *flag = true;
      if (isCount) {
        const std::string arg = om.str(3);
        if (!std::regex_match(arg, cm, reCountArg)) return rejectSyntax();
        if (!cm.str(3).empty()) return rejectSyntax("option count() incorrectly specified");
        countKind = cm.str(1)[0] == 'g' ? "global" : "local";
        countName = cm.str(2);
        if (const std::string bad = macroNameError(countKind == "global", countName); !bad.empty())
          return rejectSyntax(bad);
      }
      op += static_cast<size_t>(om.length(0));
    }
    const std::string* srcVal = fromGlobal ? session.globals.find(srcName)
                                           : session.locals.find(srcName);
    std::string val = srcVal != nullptr ? *srcVal : std::string();
    long long nSubs = 0;
    auto setCount = [&]() {
      if (countKind.empty()) return;
      if (countKind == "global") session.globals.set(countName, std::to_string(nSubs));
      else session.locals.set(countName, std::to_string(nSubs));
    };
    if (wordFlag && !oldS.empty()) {
      // whole blank-delimited words only
      std::string out;
      size_t i = 0;
      bool done = false;
      while (i < val.size()) {
        if (val[i] == ' ') {
          out += val[i++];
          continue;
        }
        size_t e = val.find(' ', i);
        if (e == std::string::npos) e = val.size();
        const std::string w = val.substr(i, e - i);
        if (!done && w == oldS) {
          out += newS;
          ++nSubs;
          if (!allFlag) done = true;
        } else {
          out += w;
        }
        i = e;
      }
      setCount();
      return out;
    }
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
      size_t at = 0;
      while (true) {
        size_t hit = val.find(oldS, at);
        if (hit == std::string::npos) break;
        out.append(val, at, hit - at);
        out.append(newS);
        at = hit + oldS.size();
        ++nSubs;
      }
      out.append(val, at, val.size() - at);
      setCount();
      return out;
    }
    nSubs = val.find(oldS) == std::string::npos ? 0 : 1;
    setCount();
    return replaceFirst(val, oldS, newS);
  }

  // BUG-1377/1378: `: list` ([P] macro lists) as ONE grammar. It served uniq,
  // posof and the set operators over blank-split words with set semantics; every
  // other function (sizeof ...) was `list not allowed` r(101), and duplicates and
  // quoted elements came out unlike Stata's. Measured on StataBE 19.5:
  //  * an element is a Stata word: a "..." or `"..."' string is one element,
  //    compared by its text and handed back as written (`a "b c" d "b c"` uniq ->
  //    `a "b c" d`; `a "a" b` uniq -> `a b`; sizeof of `a "b c" d "b c"` is 4);
  //  * the operators count duplicates: `A - B` takes one copy out of A per copy in
  //    B, from A's right end (`a b c d b` - `b d e` -> `a b c`; `a b a c a b` -
  //    `a b` -> `a b a c`); `A & B` keeps A's first min(#A, #B) copies (`a a b` &
  //    `a a a` -> `a a`); `A | B` is A then what `B - A` leaves (`a a b` | `a a a`
  //    -> `a a b a`); `A === B` is the same copies in any order (`a a b` === `a b`
  //    is 0) and `A in B` needs every copy (`a a` in `a b` is 0);
  //  * sort/rsort order by the text, stably (`z "b c" a "a"` -> `a "a" "b c" z`);
  //    clean re-adorns each element minimally (`"zz"` -> `zz`; `` `"x y"' `` ->
  //    `"x y"`) and an empty one as nothing, with one blank before every element
  //    once the result is non-empty (`"" "" a` -> `a`; `a "" b` -> `a  b`; `a ""`
  //    -> `a `, blank kept); retok[enize] hands every element back as written;
  //  * an operand is a macro NAME (bare or local(), or global()); a missing one is
  //    an empty list (`: list sizeof` is 0, `: list A -` is A); a name Stata
  //    cannot store is r(198) "<name> invalid name" (macroNameError: `global(1A)`,
  //    a 32-character local); posof's element must be quoted; the function names
  //    are case-sensitive and anything else (`: list foo A`, `: list Sort A`,
  //    `: list A ~ B`, `: list A - B C`) is r(198) "invalid syntax".
  // The w2al helper tf_ss1 runs `local rest : list vl - sv` outside capture for
  // every sorted serset (w2al_twoway_family.do:54); posof is BUG-1006's
  // (w2aq_histogram_family.log, the positions field of every hk_*_g record).
  static const std::regex reListFn(R"(^:\s*list(?:\s+([\s\S]*))?$)");
  if (std::regex_match(rhs, m, reListFn)) {
    const std::string body = strip(m.str(1));
    // Stata's tokens here: a run of name characters, `==`, or any other single
    // character; an operand is the token where one is due, so `- A` is "_-
    // invalid name" and `A ====B` "_== invalid name" (the operand after `==` is
    // the token `==`), while `A ===B` is `===`. posof's quoted element is read
    // before the rest is cut into tokens.
    std::vector<std::string> toks;
    std::string posofElem;
    bool posof = false;
    {
      size_t from = 0;
      if (body.compare(0, 5, "posof") == 0 && (body.size() == 5 || isPySpace(body[5]))) {
        posof = true;
        size_t q = 5;
        while (q < body.size() && isPySpace(body[q])) ++q;
        if (startsCompound(body, q)) {
          const size_t e = compoundSpanEnd(body, q);
          if (e == std::string::npos) return rejectSyntax();
          posofElem = body.substr(q + 2, e - q - 4);
          from = e;
        } else if (q < body.size() && body[q] == '"') {
          const size_t e = body.find('"', q + 1);
          if (e == std::string::npos) return rejectSyntax();
          posofElem = body.substr(q + 1, e - q - 1);
          from = e + 1;
        } else {
          return rejectSyntax();
        }
      }
      for (size_t i = from; i < body.size();) {
        if (isPySpace(body[i])) {
          ++i;
          continue;
        }
        size_t j = i;
        while (j < body.size() && isIdentChar(body[j])) ++j;
        if (j == i) j = i + (body.compare(i, 2, "==") == 0 ? 2 : 1);
        toks.push_back(body.substr(i, j - i));
        i = j;
      }
    }
    size_t k = 0;  // the next token
    // The operand at toks[k] (a local NAME, local(NAME) or global(NAME)) into
    // *out; none left is an empty list. False when it is not one (err is set).
    std::string operandError;
    const auto operand = [&](std::vector<ListElem>* out) {
      out->clear();
      if (k == toks.size()) return true;
      std::string name = toks[k++];
      const bool global = name == "global";
      const bool wrapped = (global || name == "local") && k < toks.size() && toks[k] == "(";
      if (wrapped) {
        ++k;
        name = k < toks.size() ? toks[k++] : std::string();
      }
      operandError = macroNameError(global && wrapped, name);
      if (operandError.empty() && wrapped) {
        if (k < toks.size() && toks[k] == ")") ++k;
        else operandError = "invalid syntax";
      }
      if (!operandError.empty()) return false;
      *out = stataElems(global && wrapped ? session.globals.get(name, "") : session.locals.get(name, ""));
      return true;
    };
    const auto join = [](const std::vector<ListElem>& es) {
      std::string out;
      for (const auto& e : es) {
        if (!out.empty()) out += ' ';
        out += e.raw;
      }
      return out;
    };
    const auto counts = [](const std::vector<ListElem>& es) {
      std::map<std::string, size_t> n;
      for (const auto& e : es) ++n[e.text];
      return n;
    };
    // The first quota[text] copies of each element of `es`, in order.
    const auto keepFirst = [](const std::vector<ListElem>& es, std::map<std::string, size_t> quota) {
      std::vector<ListElem> out;
      for (const auto& e : es) {
        auto it = quota.find(e.text);
        if (it != quota.end() && it->second > 0) {
          out.push_back(e);
          --it->second;
        }
      }
      return out;
    };
    // What is left of `a` once one copy per copy in `b` is taken out (from the right).
    const auto minus = [&](const std::vector<ListElem>& a, const std::vector<ListElem>& b) {
      auto quota = counts(a);
      const auto nb = counts(b);
      for (auto& [text, n] : quota) {
        const auto it = nb.find(text);
        n -= std::min(n, it == nb.end() ? size_t{0} : it->second);
      }
      return keepFirst(a, quota);
    };
    const auto isListFn = [](const std::string& t) {
      return t == "uniq" || t == "dups" || t == "sort" || t == "rsort" || t == "clean" ||
             t == "sizeof" || (t.size() >= 5 && std::string("retokenize").compare(0, t.size(), t) == 0);
    };
    std::vector<ListElem> a, b;
    if (posof) {
      if (toks.empty() || toks[k++] != "in") return rejectSyntax();
      if (!operand(&a)) return rejectSyntax(operandError);
      if (k != toks.size()) return rejectSyntax();
      for (size_t i = 0; i < a.size(); ++i)
        if (a[i].text == posofElem) return std::to_string(i + 1);
      return std::string("0");
    }
    if (!toks.empty() && isListFn(toks[0])) {
      const std::string lf = toks[k++];
      if (!operand(&a)) return rejectSyntax(operandError);
      if (k != toks.size()) return rejectSyntax();
      if (lf == "sizeof") return std::to_string(a.size());
      if (lf == "uniq" || lf == "dups") {
        std::vector<ListElem> out;
        std::set<std::string> seen;
        for (const auto& e : a)
          if (seen.insert(e.text).second == (lf == "uniq")) out.push_back(e);
        return join(out);
      }
      if (lf == "sort" || lf == "rsort") {
        const bool up = lf == "sort";
        std::stable_sort(a.begin(), a.end(), [up](const ListElem& x, const ListElem& y) {
          return up ? x.text < y.text : y.text < x.text;
        });
        return join(a);
      }
      if (lf == "clean") {
        std::string out;
        for (const auto& e : a) {
          if (!out.empty()) out += ' ';
          const bool blank = std::any_of(e.text.begin(), e.text.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
          });
          const bool quote = e.text.find('"') != std::string::npos;
          out += quote ? "`\"" + e.text + "\"'" : blank ? "\"" + e.text + "\"" : e.text;
        }
        return out;
      }
      return join(a);  // retokenize
    }
    if (toks.empty()) return rejectSyntax();
    if (!operand(&a)) return rejectSyntax(operandError);
    if (k == toks.size()) return rejectSyntax();
    std::string op = toks[k++];
    if (op == "==" && k < toks.size() && toks[k] == "=") {
      op = "===";
      ++k;
    }
    if (op != "-" && op != "|" && op != "&" && op != "==" && op != "===" && op != "in")
      return rejectSyntax();
    if (!operand(&b)) return rejectSyntax(operandError);
    if (k != toks.size()) return rejectSyntax();
    if (op == "-") return join(minus(a, b));
    if (op == "|") {
      auto out = a;
      for (const auto& e : minus(b, a)) out.push_back(e);
      return join(out);
    }
    if (op == "&") {
      auto quota = counts(a);
      const auto nb = counts(b);
      for (auto& [text, n] : quota) {
        const auto it = nb.find(text);
        n = std::min(n, it == nb.end() ? size_t{0} : it->second);
      }
      return join(keepFirst(a, quota));
    }
    if (op == "==") {
      bool same = a.size() == b.size();
      for (size_t i = 0; same && i < a.size(); ++i) same = a[i].text == b[i].text;
      return std::string(same ? "1" : "0");
    }
    if (op == "===") return std::string(counts(a) == counts(b) ? "1" : "0");
    return std::string(minus(a, b).empty() ? "1" : "0");  // in
  }

  // The variable readers given no variable (StataBE 19.5 r(198) for `: type`,
  // `: format`, `: var l`, `: val l`).
  static const std::regex reNoVar(R"(^:\s*(?:type|format|variable label|value label)\s*$)");
  if (std::regex_match(rhs, reNoVar)) return rejectSyntax("nothing found where name expected");

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
  //
  // BUG-1379: both words abbreviate, `var`..`variable` then `l`..`label` (paper
  // 069 writes `: var lab y`; extMacroFnName writes them out); StataBE 19.5
  // serves `var l`, `variable la`, `var labe` and refuses `va lab` ("va not
  // allowed") and `VAR LAB` ("VAR not allowed"), r(101).
  static const std::regex reVarLabel(R"(^:\s*variable label\s+([\w\x80-\xff]+)\s*$)");
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
  static const std::regex reVarFormat(R"(^:\s*format\s+([\w\x80-\xff]+)\s*$)");
  if (std::regex_match(rhs, m, reVarFormat)) {
    if (varKnownMissing(session, m.str(1))) {
      rejectMissingVar(m.str(1));
      return std::nullopt;
    }
    return session.varLabels.get(std::string(kVarFormatPrefix) + m.str(1), "%9.0g");
  }
  // EM13 / LBL-CHAR: `: char NAME[CHAR]` -- the characteristic's text, "" when it
  // is unset ([P] char; notes.ado keeps noteN/note0 there: w2ao log:1630
  // lb_note_gereg; after `xtset, clear` `_dta[_TSpanel]` is "", w2d_uncovered.log
  // :314 xtset_clear). A NAME that is neither _dta nor a variable is r(111).
  static const std::regex reCharFn(
      R"(^:\s*char\s+([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\[\s*([\w\x80-\xff]*)\s*\]\s*$)");
  if (std::regex_match(rhs, m, reCharFn)) {
    const std::string target = m.str(1);
    if (target != "_dta" && varKnownMissing(session, target)) {
      rejectMissingVar(target);
      return std::nullopt;
    }
    if (m.str(2).empty() || !session.characteristic) return std::string();
    return session.characteristic(target, m.str(2)).value_or(std::string());
  }
  // LBL-DATALABEL: `: data label` -- the dataset label, "" when none is set
  // (w2ao_label_family.log lb_data, lb_data_clear).
  // `: sortedby` -- the variables the data are sorted by, "" when none
  // (w2d_uncovered.log merge_sorted_ctl `|id|`, merge_sorted_unsorted_master `||`).
  static const std::regex reSortedBy(R"(^:\s*sortedby\s*$)");
  if (std::regex_match(rhs, m, reSortedBy)) {
    return session.sortedBy ? session.sortedBy() : std::string();
  }
  // BUG-441: `: pwd` -- the working directory, what `pwd` and c(pwd) answer
  // (paper 020's master opens with `global rootdir : pwd`). StataBE 19.5 ignores
  // anything after the word (`: pwd x`, `: pwd(x)`) and refuses `: PWD` r(101).
  if (fn == "pwd") {
    if (!session.cwd.empty()) return session.cwd;
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    return ec ? std::string() : cwd.u8string();
  }
  static const std::regex reDataLabel(R"(^:\s*data label\s*$)");
  if (std::regex_match(rhs, m, reDataLabel)) {
    return session.dataLabel ? session.dataLabel() : std::string();
  }
  // BUG-1379: `val`..`value` then `l`..`label` (StataBE 19.5: `val l u`, `value
  // labe u` -> the set name; `va l u` r(101)).
  static const std::regex reVarLblset(R"(^:\s*value label\s+([\w\x80-\xff]+)\s*$)");
  if (std::regex_match(rhs, m, reVarLblset)) {
    if (varKnownMissing(session, m.str(1))) {
      rejectMissingVar(m.str(1));
      return std::nullopt;
    }
    return session.varLabels.get(std::string(kVarLblsetPrefix) + m.str(1), "");
  }

  // FU-1111: `: label lblname # [#2] [, strict]`, `: label (varname) ...` and
  // `: label ... maxlength` ([P] macro, "label valuelabelname"). The text # maps
  // to in the set; # itself when the set is undefined, the variable has no value
  // label or # is unmapped (nothing under `strict'); #2 truncates. The returned
  // text is trimmed: w2ah_data_verbs.log ah_encode_spaces reads a label whose
  // text is two blanks as [] and w2af_macro_text_file.log mt_lab_def_069 reads
  // `"Other "' as <Other>. Oracle arms: ah_rename_lab, ah_ren_lab, ah_clone_byte,
  // ah_encode_gen/g/generate/label_new/label_again/label_exist/spaces,
  // ah_recode_g_lab. Before, this fell to the r(101) tail below. Layer: the
  // macro/arith layer only (the set is read through EvalSession::valueLabelSet).
  // Measured on StataBE 19.5: # is a number or a missing code (`: label ul abc`
  // is r(198), `1e0`, `+1`, `01` read as 1, `.a` its own code); `maxlength` and
  // `strict` are case-sensitive, strict abbreviates to `s`, and maxlength takes
  // no #2 or option (`MAXLENGTH`, `maxl`, `maxlength, strict` r(198)); a third
  // word is "varlist not allowed" r(101); `: label(u) 1` needs no blank.
  static const std::regex reLabelFn(
      R"(^:\s*label\s*(\(\s*[\w\x80-\xff]+\s*\)|[\w\x80-\xff]+)\s+([^,]*?)\s*(?:,\s*(\w*)\s*)?$)");
  if (std::regex_match(rhs, m, reLabelFn)) {
    std::string setName = m.str(1);
    const std::string args = strip(m.str(2));
    const std::string opt = m.str(3);
    if (!opt.empty() && std::string("strict").compare(0, opt.size(), opt) != 0)
      return rejectSyntax("option " + opt + " not allowed");
    const bool strict = !opt.empty();
    if (setName.front() == '(') {
      const std::string var = strip(setName.substr(1, setName.size() - 2));
      if (varKnownMissing(session, var)) {
        rejectMissingVar(var);
        return std::nullopt;
      }
      setName = session.varLabels.get(std::string(kVarLblsetPrefix) + var, "");
    }
    const std::map<std::string, std::string>* set =
        setName.empty() || !session.valueLabelSet ? nullptr : session.valueLabelSet(setName);
    const std::vector<std::string> words = args.empty() ? std::vector<std::string>{} : pySplit(args);
    if (words.size() == 1 && words[0] == "maxlength" && !m[3].matched) {
      size_t mx = 0;
      if (set != nullptr)
        for (const auto& kv : *set) mx = std::max(mx, kv.second.size());
      return std::to_string(mx);
    }
    if (words.size() > 2) {
      if (err != nullptr) {
        err->rc = "101";
        err->message = "varlist not allowed";
      }
      return std::nullopt;
    }
    static const std::regex reCode(R"(^([+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?|\.[a-z]?)$)");
    if (words.empty() || !std::regex_match(words[0], reCode)) return rejectSyntax();
    std::string key = words[0];
    {
      // labelDefs keys are the integer codes as std::to_string writes them.
      char* end = nullptr;
      const double v = std::strtod(words[0].c_str(), &end);
      if (end != nullptr && *end == '\0' && std::isfinite(v) && v == std::floor(v) &&
          std::fabs(v) < 2147483648.0)
        key = std::to_string(static_cast<long long>(v));
    }
    std::string text;
    bool mapped = false;
    if (set != nullptr) {
      auto it = set->find(key);
      if (it != set->end()) {
        text = strip(it->second);
        mapped = true;
      }
    }
    if (!mapped) text = strict ? std::string() : words[0];
    if (words.size() == 2) {
      char* end = nullptr;
      const long n2 = std::strtol(words[1].c_str(), &end, 10);
      if (end == nullptr || *end != '\0' || n2 < 0) {
        if (err != nullptr) {
          err->rc = "198";
          err->message = "invalid syntax";
        }
        return std::nullopt;
      }
      if (text.size() > static_cast<size_t>(n2)) text = text.substr(0, static_cast<size_t>(n2));
    }
    return text;
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
  // width was never declared or imported (encode/tostring/CSV output) — is
  // refused "type not allowed" r(101), deliberately: its true `str#` width is
  // not recorded anywhere in this engine, and a fabricated one would be the
  // exact silent-wrong-answer BUG-216 exists to prevent. Named in BUG-250's
  // ledger close as the measured remainder.
  static const std::regex reVarType(R"(^:\s*type\s+([\w\x80-\xff]+)\s*$)");
  if (std::regex_match(rhs, m, reVarType)) {
    if (varKnownMissing(session, m.str(1))) {
      rejectMissingVar(m.str(1));
      return std::nullopt;
    }
    if (const std::string* t = session.varLabels.find(std::string(kVarTypePrefix) + m.str(1))) {
      return *t;
    }
    // An unrecorded subtype (or a plane that never synced) keeps the honest
    // r(101) reject.
    return refuseUnanswered();
  }

  // WS-C (C-5): matrix introspection. BUG-629 adds the FULL-name forms
  // (`: rowfullnames M` / `: colfullnames M`, w2c_postest.log:102/107/757 over
  // copies of r(table) and of nlcom's posted e(b)) and a class-matrix argument
  // (`r(table)`, `e(NAME)`). The stripes are stored as Stata prints them,
  // equation prefix included, so a full name is the stored string.
  // eb-order T-P1: the short forms answer the name WITHOUT its equation, as
  // Stata does (w2g_ebmat.log:963 e6_te_colnames_eb `r1vs0.yb` for the posted
  // `ATE:r1vs0.yb`), and an e() matrix is EvalSession::resolveEMatrix's: e(b)
  // and e(V) of a fit are its canonical stripes (w2g_ebmat.log:230 `x1 x2 _cons`)
  // at rc 0, no longer the BUG-629 r(111) "copy it first" divergence.
  // gfx-binscatter: `: roweq M` / `: coleq M` answer the equation part of each
  // stripe name and `_` for a name without one (w2k_binscatter.log:248
  // BS_ctl0_c1_names `_`, :621 BS_by_c1_names `by1 by2` over `by1:= by2:=`).
  // `, quoted` (`q`..`quoted`) quotes each name as StataBE 19.5 does: `"r1"' `"r
  // 2"' for rownames, "e1" "e2" for coleq, `""e1":c1"' for colfullnames over
  // e1:c1 and `"x"' for a full name with no equation; `, QUOTED` is "option
  // QUOTED not allowed" r(198).
  static const std::regex reMatNames(
      R"(^:\s*(rownames|colnames|rowfullnames|colfullnames|roweq|coleq)\s+([A-Za-z_\x80-\xff][\w\x80-\xff]*|[er]\(\s*[A-Za-z_\x80-\xff][\w\x80-\xff]*\s*\))\s*(?:,\s*(\w*)\s*)?$)");
  if (std::regex_match(rhs, m, reMatNames)) {
    const std::string arg = m.str(2);
    const std::string opt = m.str(3);
    if (!opt.empty() && std::string("quoted").compare(0, opt.size(), opt) != 0)
      return rejectSyntax("option " + opt + " not allowed");
    const bool quoted = !opt.empty();
    const EvalMatrix* mat = nullptr;
    std::optional<EvalMatrix> resolved;
    if (arg.find('(') == std::string::npos) {
      mat = session.findMatrix(arg);
      if (mat == nullptr) return std::string();  // undefined matrix -> ""
    } else {
      const size_t open = arg.find('(');
      const std::string inner = strip(arg.substr(open + 1, arg.size() - open - 2));
      int rc = 111;
      std::string why;
      if (arg[0] == 'e') {
        resolved = session.resolveEMatrix(inner, &rc, &why, EvalSession::EReader::kNames);
        if (resolved.has_value()) mat = &*resolved;
        // `: colnames e(b)` with no e(b) is r(111) "matrix e(b) not found"
        // with or without a live e() (w2v_estpost_ematrix.log ep13_colnames_eb,
        // ep15c_colnames_eb, ep15f_colnames_eb) -- not matrix-copy's r(301).
        if (mat == nullptr && rc == 301) {
          rc = 111;
          why = "matrix e(" + inner + ") not found";
        }
      } else {
        mat = session.findClassMatrix(arg[0], inner);
        why = "matrix r(" + inner + ") not found";
      }
      // A class matrix that is not there is a named refusal at the resolver's
      // rc (111, or an e(V) the engine did not send), never the "" of an empty
      // stripe.
      if (mat == nullptr) {
        if (err != nullptr) {
          err->rc = std::to_string(rc);
          err->message = why;
        }
        return std::nullopt;
      }
    }
    const std::string which = m.str(1);
    const bool full = which.find("full") != std::string::npos;
    const bool eq = which.compare(which.size() - 2, 2, "eq") == 0;
    const std::vector<std::string>& names = which.rfind("row", 0) == 0 ? mat->rownames : mat->colnames;
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
      if (i) out += " ";
      const size_t colon = names[i].find(':');
      if (eq) {
        const std::string e = colon == std::string::npos ? std::string("_") : names[i].substr(0, colon);
        out += quoted ? "\"" + e + "\"" : e;
        continue;
      }
      std::string name = colon == std::string::npos || full ? names[i] : names[i].substr(colon + 1);
      if (quoted && full && colon != std::string::npos)
        name = "\"" + names[i].substr(0, colon) + "\"" + names[i].substr(colon);
      out += quoted ? "`\"" + name + "\"'" : name;
    }
    return out;
  }
  static const std::regex reExtColsof(R"(^:\s*colsof\s+([A-Za-z_\x80-\xff][\w\x80-\xff]*)$)");
  if (std::regex_match(rhs, m, reExtColsof)) {
    const EvalMatrix* mat = session.findMatrix(m.str(1));
    if (mat == nullptr) return std::string("0");
    return std::to_string(mat->data.empty() ? 0 : mat->data[0].size());
  }
  static const std::regex reExtRowsof(R"(^:\s*rowsof\s+([A-Za-z_\x80-\xff][\w\x80-\xff]*)$)");
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
  // `file`..`files`; `dir`..`dirs` and `other` are Stata's too and not answered
  // here (StataBE 19.5: `: dir . file *` answers, `: dir . FILES *` r(198)).
  static const std::regex reDir(
      R"re(^:\s*dir\s+(?:"([^"]*)"|(\S+))\s+(files?|dirs?|other)\s+(?:"([^"]*)"|(\S+)))re");
  if (std::regex_search(rhs, m, reDir)) {  // re.match = prefix match
    if (m.str(3)[0] != 'f') return refuseUnanswered();
    std::string dirPart = m[1].matched ? m.str(1) : (m[2].matched ? m.str(2) : ".");
    std::string pattern = m[4].matched ? m.str(4) : (m[5].matched ? m.str(5) : "*");
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
  // internal script p2perf_bench.do:144-149 — `word count`, `word N of`,
  // `length local`, `subinstr local`, `list uniq`, `variable label` — all six
  // implemented arms above. The forms that are NOT implemented
  // (`: list xvars - v`, `: colnames e(b)`, `: colfullnames e(b)`) appear only in
  // tests/validate/*.do and tests/eval/golden/*.do, which are STATA-side truth
  // capture scripts: each declares "Stata truth" in its own header, each has a
  // sibling .log, they use Stata-only commands (`sysuse`, SSC `csdid`), and
  // run_eval.py never invokes a .do file at all (it reads golden CSVs). So zero
  // PIET-executed lines depended on the literal echo, and nothing was consuming
  // the echoed text as data.
  //
  // A function this layer answers whose arguments did not parse is Stata's
  // r(198) "invalid syntax" (`: word COUNT a`, `: data LABEL`, `: sortedby x`).
  if (!fn.empty()) return rejectSyntax();
  // Stata names the offending FUNCTION token, not the whole tail, cut at the
  // first character a name cannot hold (`: E(scalars)` "E not allowed"). The
  // r()/s() readers are Stata's and not answered here: named in full.
  if (tok.empty() || ((tok == "r" || tok == "s") && tokEnd < rhs.size() && rhs[tokEnd] == '('))
    return refuseUnanswered();
  if (err != nullptr) {
    err->rc = "101";
    err->message = tok + " not allowed";
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
bool hasStataLogicalOperator(const std::string& e) {
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


// BUG-710: `session` (optional) is the caller's EvalSession. When it carries the
// variable reader (EvalSession::varObs, installed by SessionState), the bare-
// identifier shape below is no longer undecidable: evalArith has already read
// every name that IS a dataset variable (bindBareVariables), so a NameError on a
// bare name means no variable, scalar or result of that name exists, and Stata's
// answer is `NAME not found`, r(111), with nothing assigned (w2i_varsub h_03;
// h_02 for `scalar =`). Without the reader the arm stays unclaimed, as before.
bool classifyFailedEqExpr(const std::string& exprIn, const std::string& raised,
                          MacroFnError* err, const EvalSession* session ) {
  if (err == nullptr) return false;
  const std::string e = strip(exprIn);
  const bool stringLiteralRhs = !e.empty() && (e[0] == '"' || e.rfind("`\"", 0) == 0);
  // Review finding (v3): the `r(NNN):` channel is checked BEFORE the
  // string-literal exemption. A function or operator that raised it named a
  // Stata error on purpose, and a leading string literal does not make that
  // a form this evaluator failed to cover: `local a = "a" & 1` and
  // `local k = "a" + 1` are r(109) in StataBE 19.5, but the exemption stored
  // the literal text `a & 1` / `"a" + 1` with rc 0.
  static const char* kNameErrPrefix = "NameError: name '";
  // BUG-1330: a bare c(NAME) that the c() pre-pass (piet::substituteCRefs) left
  // in place is no function call. StataBE answers r(133) `c(NAME) undefined` for
  // a name it has no value for; a real name this build does not serve is
  // refused by name, with PIET's unsupported code (cClassRefusalRc; `unknown
  // function c()` before).
  const auto cClassRefusal = [&]() {
    static const std::regex reCRef(R"(\bc\(\s*([A-Za-z_]\w*)\s*\))");
    std::smatch cm;
    if (raised.rfind(std::string(kNameErrPrefix) + "c'", 0) != 0 ||
        !std::regex_search(e, cm, reCRef))
      return false;
    err->rc = cClassRefusalRc(cm.str(1));
    err->message = cClassRefusalMessage(cm.str(1));
    return true;
  };
  if (cClassRefusal()) return true;
  if (stringLiteralRhs && raised.rfind("r(", 0) != 0) {
    // FU-225 sweep (BUG-710): an unknown FUNCTION is r(133) whatever the
    // right-hand side starts with. `local z = "a" + nosuchfn_q(1)` stored the
    // text `"a" + nosuchfn_q(1)` at rc 0 under the exemption below, which is
    // for a string expression this evaluator does not cover, not for a call to
    // a function that does not exist (exprfns_contract_oracle.log:454-458).
    if (raised.rfind(kNameErrPrefix, 0) == 0) {
      const size_t start = std::string(kNameErrPrefix).size();
      const size_t end = raised.find('\'', start);
      const std::string fn = end == std::string::npos ? std::string()
                                                      : raised.substr(start, end - start);
      if (!fn.empty() && fn != "scalar" && e.find(fn + "(") != std::string::npos) {
        err->rc = "133";
        err->message = "unknown function " + fn + "()";
        return true;
      }
      // name-binding-display-literals (FU-1356): a bare name that binds to
      // nothing is r(111) beside a literal too. `local l = "a" + foo` stored
      // its own text at rc 0 and `scalar s = "a" + foo` was r(198), where
      // StataBE answers `foo not found` (w2av_name_binding bd_loc_strunb,
      // bd_sca_strunb). The reader decides, as for the bare-name shape below.
      if (!fn.empty() && fn != "scalar" && session != nullptr && session->varObs) {
        err->rc = "111";
        err->message = fn + " not found";
        return true;
      }
    }
    return false;  // string-literal RHS
  }
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
  if (raised.rfind(kNameErrPrefix, 0) == 0) {
    const size_t start = std::string(kNameErrPrefix).size();
    const size_t end = raised.find('\'', start);
    const std::string name = (end == std::string::npos) ? std::string()
                                                        : raised.substr(start, end - start);
    if (name == "scalar") {
      static const std::regex reScalarArg(R"(scalar\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\))");
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
        if (session != nullptr && session->varObs) {  // BUG-710
          err->rc = "111";
          err->message = name + " not found";
          return true;
        }
        return false;  // bare identifier — the dataset-variable shape, unclaimed
      }
      err->rc = "198";
      err->message = "invalid syntax in " + pyRepr(e);
      return true;
    }
  }
  static const std::regex reSubscript(R"(^([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\[)");
  std::smatch sm;
  if (std::regex_search(e, sm, reSubscript)) {
    err->rc = "111";
    err->message = sm.str(1) + " not found";
    return true;
  }
  // name-binding-display-literals (FU-1359): a subscripted name that is no
  // matrix and no variable is r(111) inside a larger expression too, as it is
  // alone above: `local l = 1 + Vr[1]` and `scalar s = max(0, abs(Vr[1,1]))`
  // were r(198) (w2av_name_binding bd_loc_subabs, bd_sca_subabs2). Only with a
  // session, which can tell an absent name from a present one.
  if (session != nullptr) {
    static const std::regex reAnySub(R"((?:^|[^A-Za-z0-9_.\x80-\xff])([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\[)");
    for (std::sregex_iterator it(e.begin(), e.end(), reAnySub), end; it != end; ++it) {
      const std::string name = (*it)[1].str();
      if (name == "_b" || name == "_se" || name == "_coef") continue;
      if (session->findMatrix(name) != nullptr) continue;
      if (session->varObs && session->varObs(name, 1)) continue;
      err->rc = "111";
      err->message = name + " not found";
      return true;
    }
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


// FU-225 sweep (BUG-710): the `= expr` forms' answer to an expansion error that
// carries no Stata code of its own (takeCodedExpandError took those): a named
// refusal at the block's coarse code, r(198) where that is the generic 1.
// Without an errorOut (session-less callers) the line is still consumed and
// nothing is stored. true = the error was taken.
bool refuseFailedExpansion(const ExpandResult& exp, MacroFnError* err) {
  if (!exp.isError) return false;
  if (err != nullptr) {
    std::string rc = coarseRc(exp.error);
    if (rc.empty() || rc == "1") rc = "198";
    err->rc = rc;
    err->message = exp.error.value("message", std::string("macro expansion failed"));
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

}

namespace piet::eval {
using namespace macro_detail;


// BUG-293(b): public face of classifyFailedEqExpr (see macros.h).
bool classifyFailedExpr(const std::string& expr, const std::string& raised, MacroFnError* err,
                        const EvalSession* session) {
  return classifyFailedEqExpr(expr, raised, err, session);
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
    // BUG-1381: every abbreviation Stata takes (loc/loca, gl/glo/glob/globa).
    if (headLower == "loc" || headLower == "loca") canon = "local";
    else if (headLower == "gl" || headLower == "glo" || headLower == "glob" ||
             headLower == "globa")
      canon = "global";
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
    return !isIdentChar(c);  // Unicode names: `scalarم` is one word
  };
  if (isScalarPrefix(stripped)) {
    std::string scLine = stripped;
    std::optional<ExpandResult> failedExp;
    if (hasMacroRefs(scLine)) {
      ExpandResult exp = expandPass(scLine, session);
      if (!exp.isError) scLine = exp.line;
      else failedExp = std::move(exp);
    }
    // EXPRLAYER-5: matchScalarDrop/matchScalarDef walk the same two grammars
    // left to right; selftestScalarScansMatchRegex() holds them to the regexes.
    std::string sdRest;
    if (matchScalarDrop(scLine, sdRest)) {
      auto targets = pySplit(sdRest);
      if (targets.size() == 1 && targets[0] == "_all") {
        session.scalars.clear();
        session.stringScalars.clear();  // BUG-710
      } else {
        for (const auto& t : targets) {
          session.scalars.erase(t);
          session.stringScalars.erase(t);  // BUG-710
        }
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
      // BUG-1490: an expansion that failed with a Stata code (the named r(111) for
      // an r() name PIET cannot read as missing) is that refusal, as on `local =`
      // and `display`. It used to reach evalArith unexpanded and come back r(133)
      // "unknown function r()".
      if (failedExp.has_value() && takeCodedExpandError(*failedExp, errorOut)) return true;
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
      // name-binding-display-literals (FU-1360): the stored scalars are
      // evalArith's to substitute, over literal-protected text. This branch ran
      // its own pass over the RAW right-hand side first, so a scalar's name
      // inside a string literal was replaced: with `scalar a = 7`, `scalar s =
      // "a:b"` stored `7:b` and `scalar s = "x is a"` stored `x is 7`, where
      // `local l = "a:b"` was right (w2av_name_binding bd_sca_lit, bd_sca_lit2,
      // bd_sca_litN, bd_sca_litcat, bd_sca_strvarcat).
      std::string raisedSc;
      bool scIsString = false;
      auto arith = evalArith(expr, &session, &raisedSc, &scIsString);
      // BUG-710 / FU-225 / FU-1324: an expression that FAILED to evaluate is an
      // error, never a value. The store used to keep the expression's SOURCE
      // TEXT whenever evalArith failed without an r(NNN) code (`scalar fx =
      // nosuchfn_q(1)` at rc 0, `scalar b = xd[1]` holding `xd[1]`), and every
      // later read re-evaluated that text against the data of its own moment.
      // The rc is the `local =` sibling's (classifyFailedEqExpr, the one
      // classifier; exprfns_contract_oracle.log:454-458 gives an unknown
      // function r(133)), and the scalar is neither created nor changed, as
      // after any failed `scalar =` under capture (w2i_varsub h_02). What the
      // classifier leaves unclaimed -- a string-literal right-hand side this
      // evaluator cannot finish -- is refused by name at r(198) rather than
      // stored as text; no oracle line pins that shape's rc.
      if (!arith) {
        MacroFnError scErr;
        if (!classifyFailedEqExpr(expr, raisedSc, &scErr, &session)) {
          scErr.rc = "198";
          scErr.message = "scalar " + name + " = " + expr +
                          ": the expression could not be evaluated; no scalar was stored";
        }
        if (errorOut != nullptr) *errorOut = scErr;
        return true;
      }
      session.scalars.set(name, *arith);
      // Scalars and matrices share one namespace ([P] matrix define,
      // "Namespace"): defining the scalar destroys a matrix of the same name.
      session.matrices.erase(std::remove_if(session.matrices.begin(), session.matrices.end(),
                                            [&name](const auto& p) { return p.first == name; }),
                             session.matrices.end());
      // BUG-710: the type travels with the value (EvalSession::stringScalars).
      if (scIsString) {
        session.stringScalars[name] = *arith;
      } else {
        session.stringScalars.erase(name);
      }
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
  static const std::regex reIncPre(R"(^local\s+(\+\+|--)\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*$)");
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

    // Compound double-quote RHS: `local x `"...."'` -- ONE span from the first
    // `" to the end (EM3b: nesting-aware, so `"a"' `"b"' is not one span). EM3a:
    // the body is expanded to a fixed point with macval() verbatim
    // (expandDefinitionText; w2af mt_mv_copy `<7>|<6>`).
    if (rhs.size() >= 4 && rhs.rfind("`\"", 0) == 0 && compoundSpanEnd(rhs, 0) == rhs.size()) {
      std::string inner = rhs.substr(2, rhs.size() - 4);
      if (hasMacroRefs(inner)) {
        ExpandResult exp = expandDefinitionText(inner, session);
        if (takeCodedExpandError(exp, errorOut)) return true;
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
        if (takeCodedExpandError(exp, errorOut)) return true;  // BUG-629
        // FU-225 sweep (BUG-710): an expansion that failed is an error, not a
        // value. This stored the expression's own text at rc 0 (`local x =
        // `=s[1]+"z"'` kept those characters); it is refused by name now, at the
        // block's code (198 when it names none), and the local stays unset.
        if (refuseFailedExpansion(exp, errorOut)) return true;
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
      if (!arith && classifyFailedEqExpr(expr, raised246, errorOut, &session)) return true;
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

    // Append / nested idiom: expand RHS refs before storing -- to a fixed point
    // (EM3a, expandDefinitionText).
    if (hasMacroRefs(rhs)) {
      ExpandResult exp = expandDefinitionText(rhs, session);
      // name-binding-display-literals (review, round 5): an expansion that
      // failed with a Stata code (an inline `=invFtail(2,3,.5)'` PIET refuses by
      // name) is that refusal, not the source text stored at rc 0. Under
      // `capture` the line reached here unexpanded and `_rc` came back 0.
      if (takeCodedExpandError(exp, errorOut)) return true;
      // X-1: Stata trims the blanks an expansion leaves at either end of an
      // unquoted value: `local tl `tl' `t'` from an empty tl stores `double`,
      // not ` double` (w2ai_graph_family.log:219; w2an cf_cmd_subgr_gb :742).
      if (!exp.isError) rhs = strip(exp.line);
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
        if (takeCodedExpandError(exp, errorOut)) return true;  // BUG-629, global twin
        if (refuseFailedExpansion(exp, errorOut)) return true;  // FU-225 sweep, global twin
        expr = exp.line;
      }
      std::string raised246g;
      bool isStrResult246g = false;
      auto arith = evalArith(expr, &session, &raised246g, &isStrResult246g);
      // BUG-246, global twin — same conversion, same two retained fallbacks.
      if (!arith && classifyFailedEqExpr(expr, raised246g, errorOut, &session)) return true;
      // BUG-660 fix round: same cap-at-store-site rule as local's branch above.
      session.globals.set(name, arith ? (isStrResult246g
                                              ? *arith
                                              : capNumericTextToStataLocalDigits(*arith))
                                       : stripOuterDquotes(expr));
      return true;
    }

    // EM3b: the compound-quoted RHS, as `local`'s (`global g `"g "q" x"'` stores
    // g "q" x, w2af_macro_text_file.log:887).
    if (rhs.size() >= 4 && rhs.rfind("`\"", 0) == 0 && compoundSpanEnd(rhs, 0) == rhs.size()) {
      std::string inner = rhs.substr(2, rhs.size() - 4);
      if (hasMacroRefs(inner)) {
        ExpandResult exp = expandDefinitionText(inner, session);
        if (takeCodedExpandError(exp, errorOut)) return true;
        if (!exp.isError) inner = exp.line;
      }
      session.globals.set(name, inner);
      return true;
    }
    if (hasMacroRefs(rhs)) {
      ExpandResult exp = expandDefinitionText(rhs, session);
      if (takeCodedExpandError(exp, errorOut)) return true;  // as `local` above
      if (!exp.isError) rhs = strip(exp.line);              // X-1, as `local` above
    }
    rhs = stripOuterDquotes(rhs);
    session.globals.set(name, rhs);
    return true;
  }

  return false;
}

}
