#include "piet/loops.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

#include "piet/cclass.h"
#include "piet/counters.h"  // BUG-350 coverage: the loop-capture drop site counts too
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/tokenizer.h"  // BUG-547: searchUnquoted/quotedSpanMask for legacy `for`
#include "piet/parallel.h"
#include "piet/strutil.h"
#include "dispatch_profile.h"  // EXPRLAYER: expression-zone attribution (opt-in, stderr)

namespace piet::loops {

namespace {

constexpr int kWhileCap = 1000;
constexpr int kNumlistMaxElements = 10000;

// P7 (console cap, COUNCIL-20260710 D5/F6): kWhileCap/kNumlistMaxElements
// above already bound how many times a loop can ITERATE, but not how many
// blocks its body emits per iteration — a 3-command body over 10,000
// numlist-bounded iterations is already 30,000+ blocks, well past what a
// single bridge payload should ever carry. This bounds allBlocks itself,
// independent of (and in addition to) the iteration-count caps. Mirrors
// doexec.cpp's kMaxRunBlocks in magnitude — generous / classroom-day-safe,
// and (same reasoning as there) it only stops STORING blocks: the loop still
// runs to its normal completion/break/error so dataset/scalar side effects
// stay correct, only the excess console text is elided.
constexpr size_t kMaxLoopBlocks = 20000;

ojson errBlock(const std::string& msg) { return eval::engineError(msg); }

// Legacy `for`'s own validation errors (missing ':', empty/>9-list header,
// unequal list lengths, a placeholder absent from the body, an unrecognised
// list TYPE) are all Stata r(198) "invalid syntax" (oracle:
// w2_cmdtriage.log for_unequal/for_no_placeholder, both PREC|...|198|).
// coarseRc only maps code=="syntax_error" (or an explicit reason="r(NNN)")
// to 198 -- the generic errBlock()/engineError() above always sets
// code="engine_error", which falls through coarseRc's engine_error substring
// rules (none of which match "for: invalid syntax...") to the historical
// default "1". Route through errorBlock("syntax_error", ...) instead so
// runLegacyFor's validation failures actually publish _rc=198.
ojson errFor198(const std::string& msg) { return eval::errorBlock("syntax_error", msg); }

ojson loopTruncatedBlock() {
  ojson b;
  b["type"] = "display";
  b["text"] = "(output truncated: this loop exceeded " + std::to_string(kMaxLoopBlocks) +
              " blocks -- remaining iterations run, but their output is suppressed)";
  return b;
}

// Appends `b` to allBlocks unless the per-loop cap already tripped; the
// first block that would cross kMaxLoopBlocks becomes a single elision
// marker instead, and every push after that is a no-op. `truncated` is
// owned by the caller (one flag per runForeach/runWhile invocation).
void pushLoopBlock(ojson& allBlocks, bool& truncated, const ojson& b) {
  if (truncated) return;
  if (allBlocks.size() >= kMaxLoopBlocks) {
    allBlocks.push_back(loopTruncatedBlock());
    truncated = true;
    return;
  }
  allBlocks.push_back(b);
}

// A5-04: forward to the shared strutil copy (piet/strutil.h) — kept as thin
// wrappers (not removed) so this file's many lower(...)/trim(...) call
// sites didn't need touching. Pure dedup, no behavior change.
std::string lower(std::string s) { return strutil::lower(std::move(s)); }

std::string trim(const std::string& s) { return strutil::trim(s); }

std::string rstripBrace(std::string s) {
  s = trim(s);
  if (!s.empty() && s.back() == '{') {
    s.pop_back();
    s = trim(s);
  }
  return s;
}

// ---------------------------------------------------------------------------
// Header keyword detection (loops.py's _LOOP_IF_KEYWORDS)

// BS-015: also covers `capture` and its abbreviations (cap/capt/captu/
// captur/capture — same spelling family as BS-016's fix to dispatcher.cpp's
// single-line capture stripping). `capture` is not a loop/branch construct,
// but like foreach/forvalues/while/if it CAN head a brace-aggregated `{ }`
// block (`capture { ... }`), and doexec.cpp's aggregateBraces()/
// collectBody() key EVERY brace-block recognition off this one set. Without
// this, a multi-line `capture { ... }` block's body lines were aggregated
// as independent top-level statements — never wrapped by capture at all:
// each line ran uncaptured (so `_rc` reflected whatever unrelated command
// ran last, not the block's real outcome), and an error inside the block
// would incorrectly halt the entire do-file instead of being swallowed the
// way `capture` promises. See executeLoop's dedicated "capture" branch
// below (and runCaptureBlock) for the actual block semantics.
const std::unordered_set<std::string>& loopIfKeywords() {
  static const std::unordered_set<std::string> kw = {
      // AER-SCOPE1 bundle A: forval/forv/forvalue are Stata's own accepted
      // truncations of forvalues, and the corpus writes them 946 times across
      // 40 packages -- more often than the full spelling in some files. They
      // must be recognised HERE, not just in the registry: a block opener is
      // detected before any registry lookup happens, so an abbreviation the
      // registry knows and this set does not would still fail to open a block.
      "forval", "forv", "forvalue",
      "foreach", "forvalues", "while", "if",
      "capture", "captur", "captu", "capt", "cap",
      // BUG-132: quietly/noisily ALSO head brace-aggregated blocks
      // (`quietly { ... }` / `noisily { ... }`) — the same omission BS-015
      // fixed for capture. Spellings mirror the dispatcher's single-line
      // prefix stripper (kQuietlyPrefixes, dispatcher.cpp:668-670) exactly.
      // See executeLoop's dedicated quietly/noisily branch (runQuietlyBlock)
      // for the block semantics. These only ever gate block recognition when a
      // `{` is actually present — every isLoopHeader call site requires a brace
      // on the line or a look-ahead to a lone `{` line — so a plain single-line
      // `quietly cmd` / `noisily cmd` still routes to the flat dispatch path
      // (dispatcher.cpp's Pre-0 prefix stripper) unchanged.
      "quietly", "qui", "quie", "quiet",
      "noisily", "noisil", "noisi", "nois", "noi", "no", "n",
      // PLEVEL-IMPL-WAVE arc ①: `parallel[, workers(N)] { … }` — the design's
      // §2.3 option B, a real block command rather than a comment directive.
      // Registered here for the same reason capture/quietly are: this set is
      // the ONE source of brace-block recognition (doexec.cpp's
      // aggregateBraces/collectBody key off it), so without the entry the body
      // lines would run as independent top-level statements and the block head
      // would error as an unknown command. NO abbreviations: unlike
      // capture/quietly (whose spellings mirror Stata's own prefix family),
      // `parallel` is a NEW PIET-only keyword with no Stata abbreviation
      // heritage to honour — and a short form like `par` would shadow user
      // programs. Recognition still requires an actual `{` on the line (or a
      // lone `{` look-ahead), so a bare `parallel` line is untouched by this
      // and keeps whatever the dispatcher does with it today.
      "parallel"};
  return kw;
}

std::string firstWord(const std::string& line) {
  std::string s = trim(line);
  size_t sp = s.find_first_of(" \t");
  return sp == std::string::npos ? s : s.substr(0, sp);
}

// The lowered head keyword of a block line, for the loopIfKeywords() lookup.
//
// `parallel, workers(2) {` is the FIRST block head in this codebase whose
// keyword is immediately followed by an option comma, so firstWord() returns
// "parallel," and the keyword lookup misses — the body lines then run as
// independent top-level statements and the head errors as an unknown command
// (measured, not theorised: that is exactly what the first build did). Strip
// that one trailing comma.
//
// Scoped to `parallel` ON PURPOSE. Stripping it for every keyword would
// silently start treating `capture, {` / `quietly, {` — today unknown-command
// errors — as brace blocks, which is a real behaviour change to lines this arc
// was not asked to touch. If that widening is ever wanted it should arrive as
// its own change with its own fixture, not as a side effect of adding a
// keyword.
std::string headKeyword(const std::string& line) {
  std::string fw = lower(firstWord(line));
  if (fw.size() > 1 && fw.back() == ',') {
    std::string stripped = fw.substr(0, fw.size() - 1);
    if (stripped == "parallel") return stripped;
  }
  return fw;
}

// ---------------------------------------------------------------------------
// Number formatting (loops.py's _fmt_num): integer-looking -> no decimal.

std::string fmtNum(double v) {
  if (v == std::floor(v) && std::isfinite(v)) {
    long long iv = static_cast<long long>(v);
    return std::to_string(iv);
  }
  // Match Python's str(float) shortest round-trip formatting.
  return eval::pythonFloatRepr(v);
}

// ---------------------------------------------------------------------------
// Numlist expansion (loops.py's _parse_numlist)

struct NumlistResult {
  bool isError = false;
  ojson error;
  std::vector<std::string> tokens;
};

NumlistResult parseNumlist(const std::string& spec) {
  NumlistResult r;
  std::vector<double> result;

  static const std::regex reRangeStep(
      R"(^([+-]?\d+(?:\.\d+)?)\(([+-]?\d+(?:\.\d+)?)\)([+-]?\d+(?:\.\d+)?)$)");
  static const std::regex reSlash(R"(^([+-]?\d+(?:\.\d+)?)/([+-]?\d+(?:\.\d+)?)$)");

  std::istringstream iss(spec);
  std::string tok;
  while (iss >> tok) {
    std::smatch m;
    if (std::regex_match(tok, m, reRangeStep)) {
      double start = std::stod(m[1]), step = std::stod(m[2]), end = std::stod(m[3]);
      if (step == 0.0) {
        r.isError = true;
        r.error = errBlock("numlist: step cannot be zero");
        return r;
      }
      double v = start;
      int n = 0;
      while ((step > 0 && v <= end + 1e-9) || (step < 0 && v >= end - 1e-9)) {
        result.push_back(v);
        v += step;
        if (++n > kNumlistMaxElements) {
          r.isError = true;
          r.error = errBlock("numlist: more than " + std::to_string(kNumlistMaxElements) + " elements");
          return r;
        }
      }
      continue;
    }
    if (std::regex_match(tok, m, reSlash)) {
      double start = std::stod(m[1]), end = std::stod(m[2]);
      double step = start <= end ? 1.0 : -1.0;
      double v = start;
      int n = 0;
      while ((step > 0 && v <= end + 1e-9) || (step < 0 && v >= end - 1e-9)) {
        result.push_back(v);
        v += step;
        if (++n > kNumlistMaxElements) {
          r.isError = true;
          r.error = errBlock("numlist: more than " + std::to_string(kNumlistMaxElements) + " elements");
          return r;
        }
      }
      continue;
    }
    try {
      size_t pos = 0;
      double v = std::stod(tok, &pos);
      if (pos != tok.size()) throw std::invalid_argument("trailing");
      result.push_back(v);
    } catch (...) {
      r.isError = true;
      r.error = errBlock("numlist: invalid token '" + tok + "'");
      return r;
    }
  }
  if (result.size() > static_cast<size_t>(kNumlistMaxElements)) {
    r.isError = true;
    r.error = errBlock("numlist: more than " + std::to_string(kNumlistMaxElements) + " elements");
    return r;
  }
  for (double v : result) r.tokens.push_back(fmtNum(v));
  return r;
}


// ---------------------------------------------------------------------------
// foreach-in token split (quote-aware) — loops.py's _split_foreach_tokens

std::vector<std::string> splitForeachTokens(const std::string& itemsStr) {
  std::vector<std::string> tokens;
  std::string s = trim(itemsStr);
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '"') {
      size_t j = s.find('"', i + 1);
      if (j == std::string::npos) {
        tokens.push_back(s.substr(i + 1));
        break;
      }
      tokens.push_back(s.substr(i + 1, j - i - 1));
      i = j + 1;
    } else if (std::isspace(static_cast<unsigned char>(s[i]))) {
      ++i;
    } else {
      size_t j = i;
      while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
      tokens.push_back(s.substr(i, j - i));
      i = j;
    }
  }
  return tokens;
}

// ---------------------------------------------------------------------------
// BUG-547: legacy `for` command helpers.

// A same-shape private copy of dispatcher.cpp's splitTopLevelComma (egen.py-
// adjacent: split at the first comma outside parens). dispatcher.cpp's copy
// has internal linkage inside its own anonymous namespace and is not reachable
// from this translation unit; the algorithm is 8 lines, so a second copy here
// (rather than exporting a header for one caller) is the smaller change.
std::pair<std::string, std::optional<std::string>> splitTopLevelCommaFor(const std::string& s) {
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

// `for`'s two separators (list-vs-list in the header, command-vs-command in
// the body) are both a bare `\` OUTSIDE a quoted string. eval::quotedSpanMask
// is the same mask eval::searchUnquoted itself is built on, so this can never
// disagree with how the rest of the dispatcher reads a quoted span.
std::vector<std::string> splitOnUnquotedBackslash(const std::string& s) {
  std::vector<bool> mask = eval::quotedSpanMask(s);
  std::vector<std::string> parts;
  std::string cur;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && !mask[i]) {
      parts.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(s[i]);
    }
  }
  parts.push_back(cur);
  return parts;
}

// BUG-547 (for_macro_name_has_X): `for`'s blind placeholder substitution must
// not corrupt a macro REFERENCE's name -- `$XC` has to stay `$XC` (and expand
// normally once the substituted command dispatches), never become `$aC`.
// Marks every character belonging to a `` `name' ``, `$name` or `${name}`
// span in `s` so the substitution loop below can skip over it. Design brief
// (cmd-triage.json): "Do not substitute inside macro-reference names ...
// Stata expands those before `for` sees the line."
std::vector<bool> macroNameSpanMask(const std::string& s) {
  std::vector<bool> mask(s.size(), false);
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '`') {
      size_t j = s.find('\'', i + 1);
      size_t end = (j == std::string::npos) ? s.size() : j + 1;
      for (size_t k = i; k < end; ++k) mask[k] = true;
      i = end;
      continue;
    }
    if (s[i] == '$') {
      size_t j = i + 1;
      if (j < s.size() && s[j] == '{') {
        size_t close = s.find('}', j + 1);
        size_t end = (close == std::string::npos) ? s.size() : close + 1;
        for (size_t k = i; k < end; ++k) mask[k] = true;
        i = end;
        continue;
      }
      size_t k = j;
      while (k < s.size() &&
             (std::isalnum(static_cast<unsigned char>(s[k])) || s[k] == '_')) {
        ++k;
      }
      if (k > j) {
        for (size_t m = i; m < k; ++m) mask[m] = true;
        i = k;
        continue;
      }
    }
    ++i;
  }
  return mask;
}

// BUG-547: blind text substitution of every placeholder in `subs` (one pair
// per list, in list order) over `text` in a SINGLE left-to-right pass --
// design brief: "substitute every occurrence of each placeholder... also
// inside quotes and inside words" (oracle for_in_quotes: `"X"` -> `` `"ev"' ``
// -- see the whole-quoted-span rule just below; for_num: `pX` -> `p2`),
// except inside a macro-reference name (masked above). A single combined
// pass (rather than one pass per placeholder) means an inserted VALUE is
// never itself re-scanned for a later placeholder -- each position of the
// ORIGINAL text is visited exactly once.
std::string substitutePlaceholders(
    const std::string& text, const std::vector<std::pair<std::string, std::string>>& subs) {
  std::vector<bool> mask = macroNameSpanMask(text);
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  size_t n = text.size();
  while (i < n) {
    if (!mask[i] && text[i] == '"') {
      // BUG-547 (for_in_quotes): when a placeholder is the ENTIRE content of
      // a real-quoted string, Stata's `for` does not substitute the letter
      // in place and leave the delimiting `"` characters -- it replaces the
      // whole quoted span with a Stata compound double-quote token (oracle
      // w2_cmdtriage.log for_in_quotes: `s == "X"` -> `` s == `"ev"' ``, no
      // surviving literal `"` characters around the value). Only an exact
      // whole-span match is handled here (no oracle arm exercises a
      // placeholder mixed with other text inside the same quoted string);
      // anything else falls through to the plain per-character pass below,
      // which still reaches inside a non-matching quoted span unchanged.
      // PIET's tokenizer refuses compound double quotes by name (BUG-352),
      // so a value with no `"` of its own is emitted as the plain, equivalent
      // "value" string; only a value that itself contains `"` needs (and
      // honestly hits the named refusal of) the compound form. The echo
      // therefore shows "ev" where Stata's shows `"ev"' -- display only.
      size_t close = text.find('"', i + 1);
      if (close != std::string::npos) {
        std::string content = text.substr(i + 1, close - i - 1);
        bool spanMatched = false;
        for (const auto& sub : subs) {
          if (!sub.first.empty() && content == sub.first) {
            if (sub.second.find('"') == std::string::npos)
              out += "\"" + sub.second + "\"";
            else
              out += "`\"" + sub.second + "\"'";
            i = close + 1;
            spanMatched = true;
            break;
          }
        }
        if (spanMatched) continue;
      }
    }
    bool matched = false;
    if (!mask[i]) {
      for (const auto& sub : subs) {
        const std::string& ph = sub.first;
        const std::string& val = sub.second;
        if (!ph.empty() && text.compare(i, ph.size(), ph) == 0) {
          out += val;
          i += ph.size();
          matched = true;
          break;
        }
      }
    }
    if (!matched) {
      out += text[i];
      ++i;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// _eval_bool_expr — C++ port using the M1 pyeval/exprfns machinery instead of
// a fresh eval engine. Rewrite order mirrors loops.py exactly.

// Regex-based substring replace with a callback (mirrors Python re.sub).
std::string regexReplace(const std::regex& re, const std::string& s,
                          const std::function<std::string(const std::smatch&)>& fn) {
  std::string out;
  auto begin = std::sregex_iterator(s.begin(), s.end(), re);
  auto end = std::sregex_iterator();
  size_t last = 0;
  for (auto it = begin; it != end; ++it) {
    const std::smatch& m = *it;
    out.append(s, last, static_cast<size_t>(m.position()) - last);
    out += fn(m);
    last = static_cast<size_t>(m.position() + m.length());
  }
  out.append(s, last, s.size() - last);
  return out;
}

// libc++'s std::regex (ECMAScript grammar) does NOT support lookbehind
// assertions ((?<!...)/(?<=...) — a PCRE/C++23 feature, not in the
// ECMAScript std::regex grammar) — throws std::regex_error at construction.
// loops.py's Python `re` module DOES support lookbehind, so its
// `(?<!X)C(?!Y)` patterns (single-char C not preceded by X, not followed by
// Y) are ported here as manual character scans instead of regex.

// Single-char lone-operator rewrite: replace occurrences of `ch` with
// `replacement`, SKIPPING any occurrence immediately preceded OR followed by
// `pairChar` (so "||"/"&&" are left untouched — those were already rewritten
// by an earlier pass — and only a truly lone `|`/`&` becomes " or "/" and ").
std::string replaceLoneChar(const std::string& s, char ch, char pairChar,
                            const std::string& replacement) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == ch) {
      bool precededByPair = (i > 0 && s[i - 1] == pairChar);
      bool followedByPair = (i + 1 < s.size() && s[i + 1] == pairChar);
      if (!precededByPair && !followedByPair) {
        out += replacement;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

// Single `=` (assignment-in-condition) -> `==`, skipping when adjacent to
// =/!/</> (already part of a multi-char comparison operator).
std::string replaceLoneEquals(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  auto isCmpChar = [](char c) { return c == '=' || c == '!' || c == '<' || c == '>'; };
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '=') {
      bool precededByCmp = (i > 0 && isCmpChar(s[i - 1]));
      bool followedByEq = (i + 1 < s.size() && s[i + 1] == '=');
      if (!precededByCmp && !followedByEq) {
        out += "==";
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

// Bare `.` (Stata system-missing literal) -> the sentinel, skipping when
// adjacent to a word char or another `.` (i.e. part of a number/identifier,
// not a standalone missing-value token).
std::string replaceBareDot(const std::string& s, const std::string& sentinel) {
  std::string out;
  out.reserve(s.size());
  auto isWordOrDot = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
  };
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '.') {
      bool precededByWordDot = (i > 0 && isWordOrDot(s[i - 1]));
      bool followedByWordDot = (i + 1 < s.size() && isWordOrDot(s[i + 1]));
      if (!precededByWordDot && !followedByWordDot) {
        out += sentinel;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

struct BoolEvalResult {
  bool isError = false;
  ojson error;
  bool value = false;
};

BoolEvalResult evalBoolExpr(const std::string& expr, SessionState& session) {
  dprof::EScope _z(dprof::kEBoolExpr);
  BoolEvalResult r;
  std::string cleaned = trim(expr);
  if (cleaned.empty()) {
    r.isError = true;
    r.error = errBlock("if/while: empty condition");
    return r;
  }

  // EXPRLAYER-1a. Every regex below is guarded by a find() for its own
  // mandatory literal. This is the A3 idiom this file and macros.cpp already
  // use, applied to the LAST unguarded regexes on the condition path. It is a
  // strict pre-filter in every case — a pattern cannot match text that does not
  // contain the literal it requires — so no match can be missed and no
  // substitution can be skipped.
  //
  // MEASURED, not assumed: the EXPRLAYER zone profile of S01 put 30.6 ms in
  // string-literal extraction and 33.1 ms in the operator rewrite across 15,374
  // condition evaluations, on a corpus (`while `w' < 250`, `if mod(`i',3)==0`)
  // where not one of these six patterns can match anything. The whole cost was
  // the scan that proves the absence.

  // Compound double-quotes `"..."' -> "..."
  dprof::ezoneBegin(dprof::kEBoolStrlit);
  static const std::regex reCompound(R"delim(`"(.*?)"')delim");
  if (cleaned.find("`\"") != std::string::npos) {
    cleaned = regexReplace(reCompound, cleaned,
                            [](const std::smatch& m) { return "\"" + m.str(1) + "\""; });
  }

  // Quote-aware operator rewrite: extract string literals first.
  std::vector<std::string> placeholders;
  static const std::regex reStrLit(R"("[^"]*")");
  std::string pyExpr = cleaned.find('"') != std::string::npos
                           ? regexReplace(reStrLit, cleaned,
                                          [&](const std::smatch& m) {
                                            placeholders.push_back(m.str(0));
                                            return "__STR" +
                                                   std::to_string(placeholders.size() - 1) + "__";
                                          })
                           : cleaned;

  dprof::ezoneEnd(dprof::kEBoolStrlit);
  dprof::ezoneBegin(dprof::kEBoolRewrite);

  // Stata -> Python boolean/comparison operators (ORDER matters — || / &&
  // before single | / &, so a lone '|' inside '||' is not double-rewritten;
  // replaceLoneChar/replaceLoneEquals/replaceBareDot below replicate
  // loops.py's lookaround-regex ordering via manual character scans — see
  // their doc comments for why (libc++'s std::regex has no lookbehind)).
  // A3-①: these four were the only NON-static std::regex constructions on any
  // hot path in the tree — every other pattern here and in macros.cpp is already
  // a function-local static. Constructing a std::regex COMPILES it, and the
  // compile dominates: `if` lines measured 136 us against 53 us for a `local`
  // line whose only structural difference is that it does not come through here
  // (interpreter micro-probe, 2026-08-06) — ~21 us per compile, paid on every
  // single condition evaluation of every loop. Same patterns, same order, same
  // results; they are simply compiled once now.
  static const std::regex reOrOr(R"(\|\|)");
  static const std::regex reAndAnd(R"(&&)");
  static const std::regex reNotEqTilde(R"(~=)");
  static const std::regex reBang(R"(!(?!=))");
  //
  // EXPRLAYER-1a: each pass is now guarded by the literal it needs, and every
  // guard is RECOMPUTED against the output of the pass before it — never
  // hoisted. That is not caution, it is required: the `~=` pass rewrites to
  // `!=`, so it INTRODUCES the very character the `!` pass looks for. (The
  // `!` pass then correctly declines, because reBang is `!(?!=)`; the point is
  // that a hoisted guard set computed on the ORIGINAL text would have been
  // wrong here.) find() is a memchr-class scan, so paying it per pass costs
  // orders of magnitude less than the regex it guards. replaceLoneChar and
  // replaceLoneEquals are full character copies, so skipping one removes an
  // allocation as well as a pass.
  if (pyExpr.find('|') != std::string::npos) {
    pyExpr = std::regex_replace(pyExpr, reOrOr, " or ");
  }
  if (pyExpr.find('&') != std::string::npos) {
    pyExpr = std::regex_replace(pyExpr, reAndAnd, " and ");
  }
  if (pyExpr.find('|') != std::string::npos) {
    pyExpr = replaceLoneChar(pyExpr, '|', '|', " or ");
  }
  if (pyExpr.find('&') != std::string::npos) {
    pyExpr = replaceLoneChar(pyExpr, '&', '&', " and ");
  }
  if (pyExpr.find('~') != std::string::npos) {
    pyExpr = std::regex_replace(pyExpr, reNotEqTilde, "!=");
  }
  if (pyExpr.find('!') != std::string::npos) {
    pyExpr = std::regex_replace(pyExpr, reBang, " not ");
  }
  if (pyExpr.find('=') != std::string::npos) {
    pyExpr = replaceLoneEquals(pyExpr);
  }

  dprof::ezoneEnd(dprof::kEBoolRewrite);
  dprof::ezoneBegin(dprof::kEBoolTail);

  // Est-scalar refs (_b[]/_se[]/scalar()/e()) — best-effort, mirrors the
  // oracle's `try: ... except Exception: pass`.
  try {
    std::string bErr;
    pyExpr = eval::resolveEstScalarRefs(pyExpr, session.eval, &bErr);
    if (!bErr.empty()) {
      // BUG-456: `if _b[nosuch] == 0 {` is r(111) in Stata and aborts the
      // do-file even under `capture noisily {` (refute3 V9) -- an honest error
      // here, never a `.` that quietly takes the else branch.
      r.isError = true;
      // BUG-456 follow-up: r(109) when the resolver's number reached a
      // string-typed slot (`if strlen(e(nosuch))==7 {`).
      const std::string code = eval::splitErrCode(&bErr);
      r.error = eval::errorBlock("engine_error", bErr, std::nullopt, std::nullopt, std::nullopt,
                                 code);
      dprof::ezoneEnd(dprof::kEBoolTail);
      return r;
    }
  } catch (...) {
  }
  // BUG-456: an e-STRING arrives from the resolver as a double-quoted literal
  // (`"regress"`), AFTER the placeholder extraction above and BEFORE
  // replaceBareDot below -- so an e(cmdline)/e(depvar) text with a `.` in it
  // (`d.x`, `a.b`) would have been sentinelized (the BUG-443 class). Extract
  // the new literals into the same placeholder list; the restore loop below
  // puts them back untouched, and the identifier scan never sees their text.
  if (pyExpr.find('"') != std::string::npos) {
    pyExpr = regexReplace(reStrLit, pyExpr, [&](const std::smatch& m) {
      placeholders.push_back(m.str(0));
      return "__STR" + std::to_string(placeholders.size() - 1) + "__";
    });
  }

  // BS-091/BS-101 (BETA A4): c(name) refs resolve to a literal — the same
  // shared registry gen/replace/scalar/local/display use (substituteCRefs,
  // cclass.h), so `if c(N) > 500 { ... }` / `while ... c(level) ...` work.
  // Safe here (after the string-literal placeholder extraction above): any
  // literal "c(...)" text INSIDE a quoted string is already a __STRn__
  // placeholder token by this point, so only a real unquoted c(name)
  // reference can match.
  pyExpr = substituteCRefs(pyExpr, session);

  // Stata missing sentinel: scalar(NAME) unresolved + bare `.` -> the huge
  // sentinel so `missing <= 0` is False and eval never NameErrors (matches
  // Stata's missing = +infinity ordering).
  // EXPRLAYER-1a: same strict pre-filter argument. The pattern needs the
  // literal "scalar(" and replaceBareDot needs a '.'; both were unconditional,
  // and replaceBareDot allocates and copies the whole string even when it
  // changes nothing.
  static const std::regex reScalarUnresolved(R"(\bscalar\(\s*[A-Za-z_]\w*\s*\))");
  if (pyExpr.find("scalar(") != std::string::npos) {
    pyExpr = std::regex_replace(pyExpr, reScalarUnresolved, "8.988465674311579e+307");
  }
  if (pyExpr.find('.') != std::string::npos) {
    pyExpr = replaceBareDot(pyExpr, "8.988465674311579e+307");
  }

  // C4-L3 (D2a value plane): the referenced-identifier set of the FINAL
  // rewritten expression, collected while string literals are still __STRn__
  // placeholder tokens so quoted text can never contribute a name. The lazy
  // binder below binds ONLY these — outcome-identical to the historical
  // bind-everything (evaluation consults referenced names only), and what
  // lets a pure-scalar condition (`if _rc`, `if r(N)>0`, `while c<3` — the
  // corpus-dominant shapes, c4_window_attribution §3) skip
  // session.datastore() entirely: on a dirty virtual head that call WAS the
  // EMH window-closer, snapshotting the whole frame for a condition that
  // reads no data. A token immediately preceded by a digit or '.' is a
  // number's exponent tail (1e+307), not a name — skipping it can only
  // under-collect inside already-invalid expressions, where evaluation fails
  // identically in both arms.
  dprof::ezoneEnd(dprof::kEBoolTail);
  std::unordered_set<std::string> exprRefs;
  {
    dprof::EScope _zi(dprof::kEBoolIdentScan);
    // EXPRLAYER-1b: `[A-Za-z_]\w*` walked left to right, in place of a
    // sregex_iterator over the same pattern. `\w` in ECMAScript is exactly
    // [A-Za-z0-9_] (it is NOT locale-dependent, unlike Python's), and
    // sregex_iterator finds leftmost, non-overlapping matches scanning forward
    // — and the greedy `\w*` makes each of them maximal, since nothing after it
    // can force a backtrack. That is exactly this loop, character for
    // character: skip to the first identifier start, run to the end of the
    // identifier characters, resume from there. The
    // "preceded by a digit or '.'" test and the __STR prefix test are
    // transcribed unchanged. selftest cross-checks this scan against the regex
    // over a corpus of condition shapes (selftestBoolIdentScanMatchesRegex).
    const size_t n = pyExpr.size();
    for (size_t i = 0; i < n;) {
      const unsigned char c = static_cast<unsigned char>(pyExpr[i]);
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) {
        ++i;
        continue;
      }
      size_t j = i + 1;
      while (j < n) {
        const unsigned char d = static_cast<unsigned char>(pyExpr[j]);
        if ((d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z') || (d >= '0' && d <= '9') ||
            d == '_') {
          ++j;
        } else {
          break;
        }
      }
      const bool expTail =
          i > 0 && (std::isdigit(static_cast<unsigned char>(pyExpr[i - 1])) || pyExpr[i - 1] == '.');
      if (!expTail && pyExpr.compare(i, 5, "__STR") != 0) {
        exprRefs.insert(pyExpr.substr(i, j - i));
      }
      i = j;
    }
  }

  // Restore string placeholders.
  for (size_t i = 0; i < placeholders.size(); ++i) {
    std::string key = "__STR" + std::to_string(i) + "__";
    size_t pos = pyExpr.find(key);
    if (pos != std::string::npos) pyExpr.replace(pos, key.size(), placeholders[i]);
  }

  // Build the evaluation namespace: math + Stata fns + session scalars.
  // EXPRLAYER-1c: the 39-entry math+Stata registry is a process constant and is
  // referenced, not rebuilt (39 map nodes + 39 std::function constructions per
  // condition, measured at 25.9 ms across one S01 run). Only `scalar` — whose
  // closure captures this session — and the value bindings below are per-call,
  // and they go into the OWN maps, which outrank the base exactly as a later
  // insertion into the single merged map used to.
  eval::PyNamespace ns;
  {
    dprof::EScope _zn(dprof::kEBoolNs);
    ns.base = &eval::stataMathNamespace();
    eval::addScalarFn(ns, [&session](const std::string& name) -> std::optional<std::string> {
      const std::string* v = session.eval.scalars.find(name);
      return v ? std::optional<std::string>(*v) : std::nullopt;
    });
  }
  dprof::ezoneBegin(dprof::kEBoolBind);

  // _rc: session._locals["_rc"], default "0" if unset/blank/".".
  {
    dprof::EScope _zr(dprof::kEBoolBindRc);
    const std::string* rcRaw = session.eval.locals.find("_rc");
    double rcVal = 0.0;
    if (rcRaw && !trim(*rcRaw).empty() && trim(*rcRaw) != ".") {
      try {
        rcVal = std::stod(*rcRaw);
      } catch (...) {
        rcVal = 0.0;
      }
    }
    ns.consts["_rc"] = eval::PyValue::floating(rcVal);
  }

  // _N / _n + dataset column first-obs values (Stata's `while c<3` reads
  // row 1 at program-level scope).
  //
  // C4-L3 (D2a, cobra unchanged): LAZY arm first. When the resident card is
  // provably current (the same honest-currency rule every D1 consumer uses),
  // the card answers _N and column name/dtype membership without opening the
  // datastore; session.datastore() — on a dirty virtual head, a full EMH
  // frame snapshot — is paid ONLY when the condition really reads a column's
  // row-1 value (an f64 column reference with no same-named scalar override;
  // scalars are bound after columns and win, so an overridden column's value
  // is dead either way). Anything less than a current card — stale/absent,
  // no dataset, or the PIET_NO_RESIDENT_READ escape arm via
  // residentReadCard()'s gate — falls to the pre-D2 eager path VERBATIM
  // below, including its bind-every-f64 behavior and its
  // materialize-on-dirty side effect (the floor RED arm's reproduction).
  dprof::ezoneBegin(dprof::kEBoolBindCard);
  bool boundLazily = false;
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    std::optional<int64_t> cardRows = session.residentRowsIfCurrent();
    if (cardRows.has_value()) {
      if (exprRefs.count("_N") > 0) {
        ns.consts["_N"] = eval::PyValue::floating(static_cast<double>(*cardRows));
      }
      if (exprRefs.count("_n") > 0) ns.consts["_n"] = eval::PyValue::floating(1.0);
      std::vector<std::string> needValue;
      if (*cardRows > 0) {
        for (const auto& tok : exprRefs) {
          if (session.eval.scalars.find(tok) != nullptr) continue;  // scalar wins below
          for (const auto& vi : *card) {
            if (vi.name == tok) {
              if (vi.dtype == "f64") needValue.push_back(tok);
              break;  // a current card is authoritative for absence too
            }
          }
        }
      }
      if (!needValue.empty()) {
        // Honest flush: the condition genuinely reads data. Same cost and
        // semantics as the eager arm for exactly these columns.
        Datastore* dsv = session.datastore();
        if (dsv != nullptr) {
          for (const auto& tok : needValue) {
            const double* d = dsv->col(tok).f64();
            if (d != nullptr) ns.consts[tok] = eval::PyValue::floating(d[0]);
          }
        }
      }
      boundLazily = true;
    }
  }
  if (!boundLazily) {
    Datastore* ds = session.datastore();
    if (ds != nullptr) {
      // EXPRLAYER-4: bind only the names the expression REFERENCES, exactly as
      // the lazy arm above already does. The eager arm bound _N, _n and every
      // f64 column of the dataset — 25 dtype() calls, 25 col() lookups and ~25
      // std::map inserts on the P2 fixture — for conditions like `while `w' <
      // 250` that reference none of them. Measured at 33.4 ms of S01's 50.7 ms
      // of condition time, the largest remaining item in the zone table.
      //
      // OUTCOME-IDENTICAL, on the argument this file already ratified for the
      // lazy arm (C4-L3): evaluation consults REFERENCED names only, and
      // exprRefs is collected from the final rewritten expression, so a name
      // that is not in it cannot be looked up. The DATASTORE CALL IS NOT MOVED
      // — session.datastore() still runs first and unconditionally, so the
      // materialize-on-dirty side effect this arm is deliberately preserved to
      // reproduce (the floor RED arm) happens exactly as before. Only the
      // binding loop narrowed. Column-then-scalar order is untouched, so a
      // same-named scalar still wins.
      if (exprRefs.count("_N") > 0) {
        ns.consts["_N"] = eval::PyValue::floating(static_cast<double>(ds->nRows()));
      }
      if (exprRefs.count("_n") > 0) ns.consts["_n"] = eval::PyValue::floating(1.0);
      if (ds->nRows() > 0) {
        for (const auto& tok : exprRefs) {
          if (!ds->has(tok)) continue;  // col()/dtype() throw on an absent name
          if (ds->dtype(tok) != "f64") continue;
          const double* d = ds->col(tok).f64();
          if (d != nullptr) ns.consts[tok] = eval::PyValue::floating(d[0]);
        }
      }
    } else {
      ns.consts.emplace("_N", eval::PyValue::floating(0.0));
      ns.consts.emplace("_n", eval::PyValue::floating(1.0));
    }
  }

  dprof::ezoneEnd(dprof::kEBoolBindCard);

  // Named scalars (session._scalars) — bound AFTER dataset columns so a
  // same-named scalar wins (mirrors loops.py's insertion order: scalars are
  // bound last in the Python _NS dict.update sequence).
  dprof::ezoneBegin(dprof::kEBoolBindScalars);
  for (const auto& [sname, sval] : session.eval.scalars.items()) {
    auto f = eval::pythonFloat(sval);
    if (f) ns.consts[sname] = eval::PyValue::floating(*f);
  }
  dprof::ezoneEnd(dprof::kEBoolBindScalars);

  dprof::ezoneEnd(dprof::kEBoolBind);

  auto outcome = eval::evalPyExprDetailed(pyExpr, ns);
  if (outcome.status != eval::PyEvalOutcome::Status::Value) {
    r.isError = true;
    // BUG-456: a function announcing Stata's EXACT code (`r(109): type
    // mismatch` from pyEquals on `if e(cmd) == 1 {`) is carried as that code,
    // the way `local =` and display already carry it (BUG-246/249).
    if (outcome.status == eval::PyEvalOutcome::Status::Raised &&
        outcome.error.rfind("r(", 0) == 0 && outcome.error.find(')') != std::string::npos) {
      const std::string rc = outcome.error.substr(0, outcome.error.find(')') + 1);
      const size_t colon = outcome.error.find(": ");
      r.error = eval::errorBlock(
          "engine_error",
          (colon != std::string::npos ? outcome.error.substr(colon + 2) : outcome.error) +
              " (expr=" + cleaned + ")",
          std::nullopt, std::nullopt, std::nullopt, rc);
      return r;
    }
    r.error = errBlock("if/while condition evaluation failed (expr=" + cleaned + ")");
    return r;
  }
  r.value = outcome.value.truthy();
  return r;
}

// ---------------------------------------------------------------------------
// Varlist wildcard expansion (loops.py's _expand_varlist)

// The implementation stays here in the anonymous namespace; the PUBLIC entry point
// (declared in piet/loops.h since BUG-371, so `ds` and `unab` can ask the same
// question `foreach ... of varlist` asks) is the one-line forwarder below, after
// this namespace closes.
// Stata's varlist wildcards: `*` matches any run (including empty), `?` matches
// exactly one character. Iterative backtracking rather than a regex, because a
// name is short and building a std::regex per token per call is what the zone
// profile has already caught twice in this file.
bool globMatch(const std::string& pat, const std::string& name) {
  size_t p = 0, n = 0, star = std::string::npos, mark = 0;
  while (n < name.size()) {
    if (p < pat.size() && (pat[p] == '?' || pat[p] == name[n])) {
      ++p;
      ++n;
    } else if (p < pat.size() && pat[p] == '*') {
      star = p++;
      mark = n;
    } else if (star != std::string::npos) {
      p = star + 1;
      n = ++mark;
    } else {
      return false;
    }
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
}

VarlistExpandResult expandVarlistImpl(const std::string& spec, SessionState& session) {
  VarlistExpandResult r;
  // C4-L2B (D1 card plane, brief §4): wildcard/range expansion needs only the
  // ORDERED NAME LIST, which the CURRENT resident card carries (seeded in
  // column order by every writer, the same order ds->names() reports). Reading
  // it through session.datastore() here forced, on a dirty virtual head, the
  // full EMH frame snapshot — for a `foreach ... of varlist` header that never
  // looks at a single value. Honest fallback: a stale/absent card (or the
  // PIET_NO_RESIDENT_READ escape arm, via residentReadCard's gate) drops to
  // the pre-D1 datastore path verbatim, including its no-dataset token
  // passthrough branch (a current card implies a loaded dataset, so the card
  // plane can never shadow that branch).
  std::vector<std::string> allVars;
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    allVars.reserve(card->size());
    for (const auto& v : *card) allVars.push_back(v.name);
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) {
      std::istringstream iss(spec);
      std::string tok;
      while (iss >> tok) r.tokens.push_back(tok);
      return r;
    }
    allVars = ds->names();
  }
  // AER scope 2, K1b: the range operator may carry whitespace on either side.
  // Measured on StataNow 19.5 BE (`unab a : ...` over auto.dta): `price- trunk`,
  // `price -trunk` and `price - trunk` all give the same five names as
  // `price-trunk`. The corpus spells it with the trailing space -- paper 082's
  // `tolower AL1- KONEV_L` -- and without this the two halves arrive as two
  // separate tokens and neither resolves. Collapsing is safe HERE because a
  // variable name cannot contain a hyphen (strutil::isValidStataIdentifier), so
  // in a varlist a `-` is always the range operator and never part of a name.
  std::string joined;
  joined.reserve(spec.size());
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

  std::istringstream iss(joined);
  std::string tok;
  while (iss >> tok) {
    // `_all` is the varlist keyword for every variable, and it was reaching the
    // literal-name branch below, where it answered `variable _all not found`.
    // Measured: `unab a : _all` on auto.dta is rc 0 and returns all twelve
    // names in column order. Case-insensitive because Stata's own varlist
    // keywords are.
    if (tok.size() == 4 && (tok[0] == '_') &&
        std::tolower(static_cast<unsigned char>(tok[1])) == 'a' &&
        std::tolower(static_cast<unsigned char>(tok[2])) == 'l' &&
        std::tolower(static_cast<unsigned char>(tok[3])) == 'l') {
      for (const auto& v : allVars) r.tokens.push_back(v);
      continue;
    }
    // AER scope 2, K1b: `*` matches anywhere and any number of times, and `?`
    // matches exactly one character. This used to be a TRAILING `*` only, i.e.
    // a prefix match, which silently refused three of the four shapes Stata
    // accepts. Measured on StataNow 19.5 BE against auto.dta, `unab`:
    //   t*     -> trunk turn        (this is the one the old code did)
    //   *e     -> make price
    //   tr*k   -> trunk
    //   ?rice  -> price
    //   t*n    -> turn
    //   zz*    -> r(111), no match
    //   ~e     -> r(111): `~` is NOT a wildcard here, so it stays unmatched
    // The corpus needs the others: `renvars *2010, subst(...)` is a real line
    // in it, and 948 more `drop`/`keep` lines carry a wildcard (those reach a
    // different, unwired path -- BUG-450).
    if (tok.find('*') != std::string::npos || tok.find('?') != std::string::npos) {
      bool any = false;
      for (const auto& v : allVars) {
        if (globMatch(tok, v)) {
          r.tokens.push_back(v);
          any = true;
        }
      }
      if (!any) {
        r.isError = true;
        // r(111) is what Stata answers for a pattern that matches nothing, and
        // `reason` is the only channel that carries an exact rc.
        r.error = eval::errorBlock("engine_error", "variable " + tok + " not found", std::nullopt,
                                   std::nullopt, std::nullopt, "r(111)");
        return r;
      }
      continue;
    }
    static const std::regex reRange(R"(^\w+-\w+$)");
    if (std::regex_match(tok, reRange)) {
      size_t dash = tok.find('-');
      std::string v1 = tok.substr(0, dash), v2 = tok.substr(dash + 1);
      auto it1 = std::find(allVars.begin(), allVars.end(), v1);
      auto it2 = std::find(allVars.begin(), allVars.end(), v2);
      if (it1 == allVars.end() || it2 == allVars.end()) {
        r.isError = true;
        r.error = errBlock("varlist expansion: '" + v1 + "' or '" + v2 + "' not found");
        return r;
      }
      auto i1 = std::distance(allVars.begin(), it1);
      auto i2 = std::distance(allVars.begin(), it2);
      for (auto k = i1; k <= i2; ++k) r.tokens.push_back(allVars[static_cast<size_t>(k)]);
      continue;
    }
    r.tokens.push_back(tok);
  }
  return r;
}

// BUG-509 (corrected)/BUG-544: cheap existence guard for `foreach ... of
// varlist ...`, mirroring dispatcher.cpp's `unab` arm's known-names check
// (dispatcher.cpp, incl. its no-card fallback). expandVarlistImpl above only
// validates WILDCARD/RANGE spellings (see its own comment): a bare unknown
// literal token passes straight through by design (9 other callers rely on
// that passthrough), so a caller that needs Stata's r(111) "variable not
// found" ahead of any iteration checks it here instead. Oracle:
// tests/eval/golden/aer_w2/w2_openbugs.log arms fe_unknown/fe_mixed/
// fe_prog_unknown — the check runs over the WHOLE expanded token list before
// runForeach ever sets the loop variable once.
std::optional<ojson> checkKnownVarTokens(const std::vector<std::string>& tokens,
                                         SessionState& session) {
  std::vector<std::string> known;
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    for (const auto& vi : *card) known.push_back(vi.name);
  } else if (Datastore* ds = session.datastore()) {
    known = ds->names();
  } else {
    return ojson(eval::noDataLoaded());
  }
  for (const auto& t : tokens) {
    if (std::find(known.begin(), known.end(), t) == known.end()) {
      return ojson(eval::engineError("variable " + t + " not found", "unknown_variable"));
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// BUG-547: legacy `for`'s per-list parse — one `[NAME in] TYPE elements`
// segment (already split on the header's unquoted `\`).

struct ForListSpec {
  std::string placeholder;
  std::vector<std::string> tokens;
};

// `defaultName` is the positional default (X Y Z A B C D E F by list index)
// used when the segment omits the `NAME in` prefix. Every malformed segment
// (missing elements, unrecognised TYPE) is r(198) "invalid syntax" here --
// the same code the header's other malformations (unequal list lengths, a
// placeholder absent from the body, a missing colon) use, per the design
// brief's four validation cases (cmd-triage.json). TYPE resolution mirrors
// the `foreach ... of var...` abbreviation widening just below: a >=3-char
// prefix of varlist/numlist/newlist, or exactly `any` (cmd-triage.json
// design point 3; no oracle arm exercises an abbreviated numlist/newlist,
// but the SAME minimum-prefix rule the oracle proves for varlist is not a
// new invention — it is one abbreviation floor applied to all three list
// keywords the header vocabulary already names).
std::optional<ForListSpec> parseForListSegment(const std::string& seg,
                                               const std::string& defaultName,
                                               SessionState& session, ojson* errOut) {
  std::string s = trim(seg);
  std::string name = defaultName;
  std::string rest = s;
  {
    static const std::regex reNamed(R"(^(\w+)\s+in\s+(.*))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(s, m, reNamed)) {
      name = m.str(1);
      rest = m.str(2);
    }
  }
  size_t sp = rest.find_first_of(" \t");
  if (sp == std::string::npos) {
    *errOut = errFor198("for: invalid syntax");
    return std::nullopt;
  }
  std::string type = lower(rest.substr(0, sp));
  std::string elements = trim(rest.substr(sp + 1));
  if (elements.empty()) {
    *errOut = errFor198("for: invalid syntax");
    return std::nullopt;
  }
  static const std::regex reVarlist(R"(^var(l(i(s(t)?)?)?)?$)");
  static const std::regex reNumlist(R"(^num(l(i(s(t)?)?)?)?$)");
  static const std::regex reNewlist(R"(^new(l(i(s(t)?)?)?)?$)");
  ForListSpec out;
  out.placeholder = name;
  if (type == "any") {
    out.tokens = splitForeachTokens(elements);
  } else if (std::regex_match(type, reVarlist)) {
    VarlistExpandResult vr = expandVarlistImpl(elements, session);
    if (vr.isError) {
      *errOut = vr.error;
      return std::nullopt;
    }
    out.tokens = vr.tokens;
  } else if (std::regex_match(type, reNumlist)) {
    NumlistResult nr = parseNumlist(elements);
    if (nr.isError) {
      *errOut = nr.error;
      return std::nullopt;
    }
    out.tokens = nr.tokens;
  } else if (std::regex_match(type, reNewlist)) {
    out.tokens = splitForeachTokens(elements);
  } else {
    *errOut = errFor198("for: invalid syntax: unrecognised list type '" + type + "'");
    return std::nullopt;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Body runner — pipes body_lines through doexec::executeDoSource (handles
// nested blocks via the SAME brace-aggregation entry point).

struct BodyRunResult {
  ojson blocks = ojson::array();
  bool hasError = false;
  // BUG-154: set iff the halt was a CLEAN `exit [N]` (mirrors
  // doexec::ExecuteResult::stoppedAtExitCode) — runCaptureBlock uses it to
  // set the exact Stata _rc (capture { exit 5 } → _rc 5, verified against
  // real Stata 19.5) instead of the coarse "1".
  std::optional<int> exitCode;
  std::optional<std::string> control;  // "continue" | "break"
};

// BUG-483: how many LOOP bodies deep the current dispatch is. The three loop
// runners below open a scope; `if`/`capture`/`quietly`/`parallel` deliberately
// do not (see loops.h). The accessor and the scope's members are defined after
// this anonymous namespace closes, because they are declared in piet::loops.
thread_local int g_loopBodyDepth = 0;

BodyRunResult runBody(const std::vector<std::string>& bodyLines, SessionState& session,
                       const DispatchFn& dispatchFn) {
  std::string source;
  for (size_t i = 0; i < bodyLines.size(); ++i) {
    if (i) source += "\n";
    source += bodyLines[i];
  }
  doexec::ExecuteResult result = doexec::executeDoSource(source, session, dispatchFn);
  BodyRunResult out;
  out.blocks = result.blocks;
  // F3: a body aborts its enclosing loop ONLY when execution actually HALTED on
  // an uncaught error (or a cancel) — precisely the condition executeDoSource
  // records in stoppedAtLine. An error consumed by `capture` (BS-127: the
  // dispatcher tags it captured:true; it is display-only, sets _rc, and does
  // NOT halt) leaves stoppedAtLine unset, so the loop must keep iterating. The
  // old "any type==error block present" scan wrongly treated a captured error
  // as fatal, aborting foreach/forvalues/while (and nested loops) after the
  // first captured body error. Keying off stoppedAtLine mirrors doexec.cpp's
  // own top-level halt rule (type==error && !captured) exactly, so runBody can
  // never disagree with it; an UNcaptured error still sets stoppedAtLine and
  // still aborts the loop (Stata semantics for uncaught errors preserved).
  out.hasError = result.stoppedAtLine.has_value();
  // BUG-154: a CLEAN `exit [N]` halt inside this body is invisible to the
  // caller's caller. The halt rides the error channel as a sentinel block
  // ("exit"/"exit N"), and the INNER executeDoSource above both consumed and
  // ERASED it (doexec.cpp handleUncapturedError removes the sentinel so a
  // clean exit never paints a red card). An ordinary uncaptured error stays
  // in out.blocks and re-halts the outer executeDoSource when this body's
  // blocks bubble up through runIf/runForeach/runWhile — but the erased exit
  // sentinel left NOTHING to re-halt on, so `if (cond) { exit 7 }` fell
  // through and the do-file kept running (guards were dead). Re-emit the
  // sentinel here: the outer handleUncapturedError recognizes the message
  // shape, halts with the SAME code, and erases it again — cascading through
  // any nesting depth. `capture { ... exit N }` is unaffected: its body runs
  // under the capture arm, which traps the error channel and sets _rc=N by
  // design (the sentinel we append here is exactly what capture expects to
  // trap).
  if (result.stoppedAtLine.has_value() && result.stoppedAtExitCode.has_value()) {
    int code = *result.stoppedAtExitCode;
    out.exitCode = code;
    ojson halt;
    halt["type"] = "error";
    halt["message"] = "exit" + (code != 0 ? (" " + std::to_string(code)) : std::string());
    out.blocks.push_back(halt);
  }
  if (result.control.has_value()) out.control = result.control;
  return out;
}

// ---------------------------------------------------------------------------
// Runners

ojson runForeach(const std::string& varname, const std::vector<std::string>& tokens,
                  const std::vector<std::string>& bodyLines, SessionState& session,
                  const DispatchFn& dispatchFn) {
  LoopBodyScope __loopBody;  // BUG-483: `continue` asks how deep we are
  ojson allBlocks = ojson::array();
  bool truncated = false;   // P7: allBlocks storage cap (see kMaxLoopBlocks)
  for (const auto& tok : tokens) {
    session.eval.locals.set(varname, tok);
    BodyRunResult result = runBody(bodyLines, session, dispatchFn);
    for (auto& b : result.blocks) pushLoopBlock(allBlocks, truncated, b);
    if (result.hasError) break;
    if (result.control == "break") break;
    // control == "continue": fall through to next token.
  }
  return allBlocks;
}

// FULL FUNCTIONAL CHECK (2026-08-05): `forvalues` iterates its range LAZILY.
//
// It used to materialise every value into a vector<string> first
// (genForvaluesTokens) and hand that to runForeach, which is why a 10,000-
// iteration cap existed at all: the cap was a MEMORY guard on that vector, not
// a language limit. Stata has no such limit, so a legal do-file with a longer
// loop was refused — and simply raising the number would have moved the failure
// from an honest reject to an out-of-memory one. Generating each value in place
// removes the reason for the cap instead of arguing with it.
//
// The arithmetic is deliberately IDENTICAL to the materialising version —
// same `v += step` accumulation (drift and all), same `fmtNum` rendering, same
// 1e-9 end tolerance — so every existing loop assigns byte-identical macro
// text. A `foreach`-style enumerated list still goes through runForeach; only
// the numeric range stopped being built up front.
ojson runForvaluesRange(const std::string& varname, double start, double step, double end,
                        const std::vector<std::string>& bodyLines, SessionState& session,
                        const DispatchFn& dispatchFn) {
  LoopBodyScope __loopBody;  // BUG-483: `continue` asks how deep we are
  ojson allBlocks = ojson::array();
  bool truncated = false;   // P7: allBlocks storage cap (see kMaxLoopBlocks)
  for (double v = start; (step > 0 && v <= end + 1e-9) || (step < 0 && v >= end - 1e-9);
       v += step) {
    session.eval.locals.set(varname, fmtNum(v));
    BodyRunResult result = runBody(bodyLines, session, dispatchFn);
    for (auto& b : result.blocks) pushLoopBlock(allBlocks, truncated, b);
    if (result.hasError) break;
    if (result.control == "break") break;
    // control == "continue": fall through to the next value.
  }
  return allBlocks;
}

ojson runWhile(const std::string& condition, const std::vector<std::string>& bodyLines,
               SessionState& session, const DispatchFn& dispatchFn) {
  LoopBodyScope __loopBody;  // BUG-483: `continue` asks how deep we are
  ojson allBlocks = ojson::array();
  bool truncated = false;   // P7: allBlocks storage cap (see kMaxLoopBlocks)
  int nIters = 0;
  while (true) {
    std::string condExpanded = condition;
    if (eval::hasMacroRefs(condition)) {
      // unsetRAsMissing: a bare/guarded r() that isn't set evaluates to Stata
      // missing (`.`) in a while condition, not an error (FIX-RESTIMATE).
      eval::ExpandResult exp = eval::expandMacros(condition, session.eval,
                                                  /*unsetRAsMissing=*/true);
      if (exp.isError) {
        allBlocks.push_back(exp.error);
        return allBlocks;
      }
      condExpanded = exp.line;
    }
    BoolEvalResult truth = evalBoolExpr(condExpanded, session);
    if (truth.isError) {
      allBlocks.push_back(truth.error);
      return allBlocks;
    }
    if (!truth.value) break;

    if (++nIters > kWhileCap) {
      allBlocks.push_back(errBlock(
          "while: exceeded " + std::to_string(kWhileCap) +
          " iterations — possible runaway loop. Check your condition to ensure it "
          "eventually becomes false."));
      return allBlocks;
    }

    BodyRunResult result = runBody(bodyLines, session, dispatchFn);
    for (auto& b : result.blocks) pushLoopBlock(allBlocks, truncated, b);
    if (result.hasError) break;
    if (result.control == "break") break;
    // control == "continue": loop back to condition re-eval.
  }
  return allBlocks;
}

ojson runIf(const std::string& condition, const std::vector<std::string>& bodyLines,
            const std::vector<std::string>& elseBodyLines, SessionState& session,
            const DispatchFn& dispatchFn) {
  std::string condExpanded = condition;
  if (eval::hasMacroRefs(condition)) {
    // unsetRAsMissing: `if !_rc & !missing(r(estimate))` after a failed
    // capture must NOT error on the unset r() — Stata evaluates it as missing
    // (`.`). Only the control-flow condition path opts in (FIX-RESTIMATE).
    eval::ExpandResult exp = eval::expandMacros(condition, session.eval,
                                                /*unsetRAsMissing=*/true);
    if (exp.isError) return ojson::array({exp.error});
    condExpanded = exp.line;
  }
  BoolEvalResult truth = evalBoolExpr(condExpanded, session);
  if (truth.isError) return ojson::array({truth.error});

  BodyRunResult result = truth.value ? runBody(bodyLines, session, dispatchFn)
                                      : runBody(elseBodyLines, session, dispatchFn);
  ojson out = result.blocks;
  if (result.control.has_value()) {
    ojson ctrl;
    ctrl["type"] = "_control";
    ctrl["action"] = *result.control;
    out.push_back(ctrl);
  }
  return out;
}

// BS-015: `capture { line1 \n line2 \n ... }` multi-line block. Real Stata
// semantics: run the body sequentially; if a line errors, ITS error message
// is suppressed and the rest of the block does NOT run (mirrors how an
// uncaptured error normally halts execution — capture just catches this
// halt locally instead of letting it propagate out of the block), but
// lines BEFORE the failure keep whatever output they already produced
// (capture does not retroactively hide output that already happened — it
// is not `quietly`). `_rc` mirrors the existing single-line `capture cmd`
// convention (dispatcher.cpp's Pre-0 block): "1" on any error inside the
// block, "0" otherwise — the same coarse boolean this codebase uses
// everywhere today (an exact numeric _rc taxonomy is BS-100/A4, a later
// phase, not this fix). Reuses runBody (the SAME helper runForeach/runWhile/
// runIf already use) so nested blocks/control-flow inside the capture body
// work exactly like any other brace body, for free.
ojson runCaptureBlock(const std::vector<std::string>& bodyLines, SessionState& session,
                      const DispatchFn& dispatchFn) {
  BodyRunResult result = runBody(bodyLines, session, dispatchFn);
  ojson out = ojson::array();
  // BUG-116/F3: the old scan below treated ANY type=="error" block as fatal
  // to this capture block, including one already caught by a NESTED
  // `capture` (tagged captured:true by the dispatcher's capture arm, BS-127
  // — display-only, does not halt). That wrongly truncated the block's
  // output at the nested capture's line and set the OUTER _rc from an error
  // that never actually escaped to this scope. Key off result.hasError
  // instead — runBody already computes it from doexec's own halt authority
  // (stoppedAtLine), the SAME signal runForeach/runWhile/runIf rely on — so
  // a captured nested error passes through untouched and the block keeps
  // running exactly like the enclosing do-file already does around it.
  //
  // BUG-546: the halting block's own coarse rc (eval::coarseRc) is captured
  // HERE, from the same block the loop is about to suppress, rather than
  // hardcoding "1" below — a `capture noisily { summarize nosuchcol }` gave
  // _rc 1 where the single-line `capture noisily summarize nosuchcol` (which
  // already used coarseRc, dispatcher.cpp) gave the real 111. Oracle:
  // tests/eval/golden/aer_w2/w2_openbugs.log fe_unknown/fe_mixed (the SAME
  // capture-BLOCK shape wraps the foreach arms there).
  std::string haltedRc = "1";
  for (const auto& b : result.blocks) {
    if (result.hasError && b.value("type", "") == "error" && !b.value("captured", false)) {
      haltedRc = eval::coarseRc(b);
      break;  // suppress the error itself (and anything after — the block stops here)
    }
    out.push_back(b);
  }
  // BUG-154: a clean `exit N` inside the block carries its EXACT code to _rc
  // (Stata 19.5: `capture { exit 5 }` → _rc 5, block stops, file continues).
  // Every other error carries the exact coarse code (BUG-546, above).
  session.eval.locals.set("_rc", result.exitCode.has_value()
                                     ? std::to_string(*result.exitCode)
                                     : (result.hasError ? haltedRc : "0"));
  if (result.control.has_value()) {
    ojson ctrl;
    ctrl["type"] = "_control";
    ctrl["action"] = *result.control;
    out.push_back(ctrl);
  }
  return out;
}

// BUG-132: `quietly { … }` / `noisily { … }` multi-line block. Like
// `capture { … }` (runCaptureBlock above), quietly/noisily are prefix-class
// keywords that can head a brace-aggregated block — but their semantics differ
// from capture on TWO axes, so they get their own handler rather than reusing
// the capture path:
//   (1) OUTPUT visibility. `quietly` SUPPRESSES the body's normal output
//       (display/result/data_mutation cards) while still showing command
//       echoes and any error message — exactly the filter dispatcher.cpp's
//       single-line quietly branch applies (keep type=="echo"||"error";
//       dispatcher.cpp:769-773). `noisily` is the transparent case: it shows
//       everything the body produced (its purpose is to un-quiet a region
//       nested inside an enclosing quietly block; at top level it behaves like
//       a plain block).
//   (2) ERROR propagation. UNLIKE capture, neither quietly nor noisily
//       SWALLOWS an uncaught error — an error inside the block still HALTS the
//       do-file. runBody leaves the error block untagged (captured stays
//       false) and reports hasError, so passing that error block through
//       unmodified lets doexec.cpp's top-level halt rule (type=="error" &&
//       !captured) fire exactly as it does for a bare uncaught error. This
//       matches the single-line `quietly cmd` behaviour BUG-132's repro
//       confirmed correct (quietly is output-only; it never catches errors the
//       way capture does).
// No `_rc` is set here, on purpose: only `capture` owns `_rc` in this codebase,
// and real Stata's quietly/noisily are transparent to `_rc` — they neither set
// nor clear it — so leaving it untouched is the correct semantics, not an
// omission. Reuses runBody (the SAME helper runForeach/runWhile/runIf/
// runCaptureBlock use) so nested blocks/control-flow inside the body work for
// free.
ojson runQuietlyBlock(const std::vector<std::string>& bodyLines, SessionState& session,
                      const DispatchFn& dispatchFn, bool quiet) {
  // BUG-189: the filter below cannot reach `log using` — dispatch() tees each
  // body line to the log before this function ever sees the block. Declaring the
  // scope makes the tee itself aware of it.
  BodyRunResult result;
  {
    SessionState::LogVerbosityScope __lv(session, quiet);
    result = runBody(bodyLines, session, dispatchFn);
  }
  ojson out = ojson::array();
  for (const auto& b : result.blocks) {
    if (quiet) {
      // quietly: keep only command echoes + error messages; suppress the
      // body's normal output. Mirrors the single-line quietly filter.
      std::string t = b.value("type", "");
      if (t == "echo" || t == "error") out.push_back(b);
    } else {
      // noisily: fully transparent — show every block the body produced.
      out.push_back(b);
    }
  }
  if (result.control.has_value()) {
    ojson ctrl;
    ctrl["type"] = "_control";
    ctrl["action"] = *result.control;
    out.push_back(ctrl);
  }
  return out;
}

// PLEVEL-IMPL-WAVE arc ① — `parallel[, workers(N)] { … }`, the SKELETON leg.
// Design authority + arc boundary: include/piet/parallel.h. What lands here is
// header parsing (honest errors), the §3.4 task decomposition, the resolved
// worker budget, the §2.4 task spec, the stderr banner — and SERIAL execution
// of the decomposed tasks in this process. Arc ② replaces the execution loop
// below with spawn/splice over `--parallel-worker` children; everything around
// it (what a task is, what a child inherits, what the parent learns) is already
// what that arc will hand to them.
//
// Two deliberate properties of this leg:
//  · **Per-TASK dispatch, not one whole-body run.** Running the body as a
//    single doexec pass would be one line shorter, but it would make a failing
//    task halt the rest of the block — the opposite of §2.6's failure
//    isolation, which is a documented semantic of the construct and not merely
//    an artefact of running in parallel. Dispatching per task means the
//    isolation rule holds identically at N=1 and at N>1, which is exactly what
//    makes the N=1 leg a legitimate reference arm for G-1.
//  · **The banner goes to stderr.** stdout is the byte-pinned surface (§2.5;
//    freeze/run_eval/floor_gate are all byte-exact), so a stdout banner would
//    diff every fixture at once. Precedent: main.cpp's `--do scratch workdir`
//    line is stderr for the same reason.
ojson runParallelBlock(const std::string& header, const std::vector<std::string>& bodyLines,
                       SessionState& session, const DispatchFn& dispatchFn) {
  parallel::HeaderOptions opt = parallel::parseHeader(header);
  if (opt.isError) return ojson::array({opt.error});

  std::vector<std::string> tasks = parallel::decomposeTasks(bodyLines);
  // 2026-09-02: the derivation gets the WORK it is sizing for — this block's
  // task count, the dataset the workers will inherit, and the scratch volume.
  parallel::ResolveContext rctx;
  rctx.taskCount = static_cast<int64_t>(tasks.size());
  rctx.residentBytes = parallel::residentBytesEstimate(session);
  rctx.scratchDir = session.workdir();
  parallel::WorkerBudget budget = parallel::resolveWorkers(opt.workers, rctx);

  std::string banner = "[piet_native] parallel: " + std::to_string(tasks.size()) + " tasks / " +
                       std::to_string(budget.n) + " workers";
  if (!budget.reason.empty()) banner += " (" + budget.reason + ")";
  // arc ② decides here whether this block actually fans out. Three reasons not
  // to, each NAMED in the banner rather than silently degrading (§10.9: a block
  // that quietly runs serially is how "why isn't it faster" becomes unanswerable):
  //   - budget resolved to one worker (nothing to fan out to);
  //   - we ARE a worker (PIET_PARALLEL_WORKER) — one generation only;
  //   - fan-out could not be started at all, decided below.
  const bool inWorker = [] {
    const char* e = std::getenv("PIET_PARALLEL_WORKER");
    return e != nullptr && *e != '\0';
  }();
  bool fanOut = budget.n > 1 && !inWorker && tasks.size() > 1;
  if (inWorker && budget.n > 1) banner += " (nested: serial)";

  // §2.4 — the dataset travels BY PATH. datastore() materializes a virtual or
  // dirty resident head to a real file first, so what the children open is the
  // parent's actual current data rather than whatever the last save left on
  // disk. The parent executes nothing while the block runs, so the scratch head
  // cannot be reclaimed out from under them.
  std::string parentHead;
  if (fanOut && session.hasData()) {
    try {
      session.datastore();
      if (session.currentHbin().has_value()) parentHead = *session.currentHbin();
    } catch (const std::exception&) {
      parentHead.clear();
    }
    if (parentHead.empty()) {
      fanOut = false;
      banner += " (resident head could not be snapshotted: serial)";
    }
  }

  std::vector<parallel::TaskOutcome> outcomes;
  if (fanOut) {
    static std::atomic<unsigned long long> blockSeq{0};
    const std::string scratchDir = session.workdir() + "/parallel_" +
                                   std::to_string(blockSeq.fetch_add(1));
    outcomes = parallel::runTasksInWorkers(tasks, session, budget, parentHead, scratchDir);
    if (outcomes.empty()) {
      fanOut = false;
      banner += " (fan-out unavailable: serial)";
    }
  }
  if (!fanOut) banner += " [serial]";
  std::fprintf(stderr, "%s\n", banner.c_str());

  // Opt-in spec dump — kept as the schema-pinning hook selftest uses.
  parallel::dumpTaskSpecIfRequested(
      parallel::buildTaskSpec(tasks, session, budget, parentHead));

  ojson out = ojson::array();
  bool truncated = false;  // P7 storage cap — same discipline as every loop body
  int nFailed = 0;
  std::optional<std::string> control;
  // ---- arc ② replay: SOURCE order, whatever order the workers finished in ----
  if (fanOut) {
    std::optional<int> exitCode;
    size_t replayEnd = tasks.size();
    for (size_t i = 0; i < outcomes.size(); ++i) {
      if (outcomes[i].hasExitCode) {
        // §2.6 exit promotion: the earliest task (in SOURCE order) that exited
        // decides. Later tasks may already have finished; their OUTPUT is
        // discarded, which is what "the do-file stopped there" has to mean.
        exitCode = outcomes[i].exitCode;
        replayEnd = i + 1;
        break;
      }
    }
    for (size_t i = 0; i < replayEnd; ++i) {
      parallel::TaskOutcome& o = outcomes[i];
      if (!o.delivered) {
        // A worker that died owes an honest block, never silence (§2.6).
        ojson eb = errBlock("parallel: task " + std::to_string(i) + " did not complete (" +
                            (o.driverError.empty() ? std::string("worker died") : o.driverError) +
                            ")");
        eb["captured"] = true;
        pushLoopBlock(out, truncated, eb);
        ++nFailed;
        continue;
      }
      // Same per-task varlist thinning the serial leg does — a task's output is
      // what it would have been as a top-level statement.
      doexec::coalesceMutationVarlists(o.blocks);
      // G-4: fold this task's additive counters into the parent's own
      // accumulators BEFORE its blocks are replayed, so the env-gated emit at
      // the end of the run reports the whole run's work rather than only the
      // part that happened in this address space. Only the additive set folds
      // (counters.h::foldableAccumulators explains what is excluded and why);
      // error/graph counts are left to the parent's own replay so they are
      // counted exactly once.
      for (auto it = o.countersDelta.begin(); it != o.countersDelta.end(); ++it) {
        if (!it.value().is_number_integer()) continue;
        for (const auto& [name, slot] : piet::counters::foldableAccumulators()) {
          if (it.key() == name) { *slot += it.value().get<int64_t>(); break; }
        }
      }
      // G-2: the log. In every other path a statement's blocks reach an open log
      // because the OUTERMOST dispatch tees them on the way back. Replayed
      // blocks never pass through dispatch — they were produced in another
      // process, and a child is never given the parent's log (log state is
      // control, and two writers on one file is not a thing to attempt). So the
      // replay has to tee them itself, once per task, in source order. Without
      // this line the parallel run's log is missing every task's output while
      // its stdout is byte-perfect — which is precisely the split G-2 exists to
      // catch.
      session.logTee(o.blocks, 1);
      for (auto& b : o.blocks) {
        if (o.failed && b.value("type", "") == "error" && !b.value("captured", false)) {
          ojson tagged = b;
          tagged["captured"] = true;
          pushLoopBlock(out, truncated, tagged);
          continue;
        }
        pushLoopBlock(out, truncated, b);
      }
      if (o.failed) ++nFailed;
    }
    session.eval.locals.set("_rc", nFailed > 0 ? "1" : "0");
    session.eval.rValues.set("n_failed", static_cast<double>(nFailed));
    if (exitCode.has_value()) {
      // Re-raise the halt through the SAME sentinel the serial paths use, so
      // doexec's uncaptured-error rule promotes it to a clean parent halt with
      // this code (BUG-139's message shape).
      out.push_back(errBlock(*exitCode == 0 ? "exit" : "exit " + std::to_string(*exitCode)));
      if (replayEnd < tasks.size())
        std::fprintf(stderr,
                     "[piet_native] parallel: exit at task %zu — output of tasks %zu..%zu "
                     "discarded; completed tasks' file side effects were not rolled back\n",
                     replayEnd - 1, replayEnd, tasks.size() - 1);
    }
    return out;
  }

  for (const auto& task : tasks) {
    std::vector<std::string> taskLines;
    {
      std::istringstream iss(task);
      std::string l;
      while (std::getline(iss, l)) taskLines.push_back(l);
    }
    BodyRunResult r = runBody(taskLines, session, dispatchFn);
    // §2.5 byte law — one `varlist_after` card per TASK, not per block.
    // executeDoSource applies this thinning per top-level item, and a task is
    // exactly what a top-level item would have been had the wrapper not been
    // written; without this call the whole block shares one card and tasks
    // 1..K-1 silently lose theirs. cmp(A1,AN) cannot see that (both arms lose
    // them identically) — the probe's wrapper-less A0 arm is what caught it.
    doexec::coalesceMutationVarlists(r.blocks);
    for (auto& b : r.blocks) {
      // §2.6 failure isolation, the half that per-task dispatch alone does NOT
      // buy: a task's uncaught error block would otherwise escape the block
      // untagged and trip doexec's top-level halt rule (type=="error" &&
      // !captured), so the do-file would stop at the parallel block even though
      // the remaining tasks already ran. Measured on the first build: task 2
      // ran, and `stopped_at_line` was still set.
      //
      // The error stays VISIBLE (honest-error doctrine — it is pushed, not
      // swallowed); `captured` is this codebase's existing "display-only, does
      // not halt" tag, the same one the dispatcher's capture arm sets (BS-127),
      // and it is the right one here because a parallel block reports failure
      // the way capture does: through `_rc` (+ r(n_failed) below), not by
      // halting. A caller that wants the old halt behaviour writes
      // `if _rc { exit _rc }` after the block.
      //
      // ⚠ ARC ② OWED — `exit` inside a task. §2.6 requires a task's clean
      // `exit`/`exit N` to be PROMOTED to a parent halt (stop dispatching, let
      // running tasks finish, replay in source order, then halt). runBody drops
      // ExecuteResult::stoppedAtExitCode, so this arc cannot distinguish a
      // clean exit from a failure and treats both as a non-halting task
      // failure. Arc ② must widen BodyRunResult with that field — the child
      // protocol already carries it (`exit_code` in parallel.cpp's worker
      // result), so the parent half is what is missing, not the wire.
      if (r.hasError && b.value("type", "") == "error" && !b.value("captured", false)) {
        ojson tagged = b;
        tagged["captured"] = true;
        pushLoopBlock(out, truncated, tagged);
        continue;
      }
      pushLoopBlock(out, truncated, b);
    }
    if (r.hasError) ++nFailed;  // §2.6: counted, but the block keeps going
    if (r.control.has_value()) {
      // A break/continue that bubbled out of a task belongs to an ENCLOSING
      // loop, so it must not be swallowed — and no further task may run once
      // it fired (that is what break/continue mean).
      control = r.control;
      break;
    }
  }

  // §8 #4 ruling (c): the parent learns the failure count and a coarse _rc.
  // "1 on any failure, 0 otherwise" is the same convention runCaptureBlock uses
  // — an exact numeric rc taxonomy is BS-100/A4 everywhere in this codebase,
  // not something this block invents for itself.
  session.eval.locals.set("_rc", nFailed > 0 ? "1" : "0");
  session.eval.rValues.set("n_failed", static_cast<double>(nFailed));

  if (control.has_value()) {
    ojson ctrl;
    ctrl["type"] = "_control";
    ctrl["action"] = *control;
    out.push_back(ctrl);
  }
  return out;
}

}  // namespace

// BUG-483: declared in loops.h, defined here — outside the anonymous namespace
// above, which is where the counter lives (still visible for the rest of this
// translation unit).
int loopBodyDepth() { return g_loopBodyDepth; }
LoopBodyScope::LoopBodyScope() { ++g_loopBodyDepth; }
LoopBodyScope::~LoopBodyScope() { --g_loopBodyDepth; }

VarlistExpandResult expandVarlist(const std::string& spec, SessionState& session) {
  return expandVarlistImpl(spec, session);
}

bool isLoopHeader(const std::string& line) {
  return loopIfKeywords().count(headKeyword(line)) != 0;
}

bool isParallelHeader(const std::string& line) { return headKeyword(line) == "parallel"; }

// BUG-167 — see the contract in loops.h. The prefix-class spellings mirror
// loopIfKeywords() exactly (capture/quietly/noisily + their abbreviations);
// foreach/forvalues/while/if are the CONTROL class, whose header legitimately
// carries a list or condition ahead of the brace.
bool opensBraceBlock(const std::string& line) {
  if (!isLoopHeader(line)) return false;
  size_t brace = line.find('{');
  if (brace == std::string::npos) return false;

  static const std::unordered_set<std::string> kControl = {"foreach", "forvalues", "forval",
                                                          "forv",    "forvalue",  "while",
                                                          "if"};
  if (kControl.count(lower(firstWord(line))) != 0) return true;

  // Prefix class: everything before the brace must itself be prefix keywords.
  std::istringstream iss(line.substr(0, brace));
  std::string word;
  while (iss >> word) {
    std::string lw = lower(word);
    if (kControl.count(lw) != 0) return true;  // `capture if ... {` — control wins.
    if (loopIfKeywords().count(lw) == 0) return false;
  }
  return true;
}

// P4 wave — see loops.h. Splits `COND command ...` by walking whitespace-
// separated tokens at paren/quote depth 0 with a tiny operand/operator state
// machine: the command begins at the first token that arrives where an
// OPERATOR was expected. Returning false (rather than a best guess) whenever
// the walk cannot reach that state is the whole safety property — the caller
// then reports its ordinary "unsupported" error, and a malformed line can
// never silently execute a command the user did not write.
bool splitOneLineIf(const std::string& rest, std::string& cond, std::string& body) {
  static const std::vector<std::string> kBinOps = {
      "==", "!=", "~=", ">=", "<=", "&&", "||", "&", "|", "<", ">", "+", "-", "*", "/", "^"};
  auto isBinOpToken = [&](const std::string& t) {
    return std::find(kBinOps.begin(), kBinOps.end(), t) != kBinOps.end();
  };
  // A token ENDING in a binary operator (e.g. `x'!=`) still expects an operand;
  // one that merely contains a complete comparison (`x'!=0`) does not.
  auto endsWithBinOp = [&](const std::string& t) {
    for (const auto& op : kBinOps) {
      if (t.size() >= op.size() && t.compare(t.size() - op.size(), op.size(), op) == 0) return true;
    }
    return false;
  };

  size_t i = 0, n = rest.size();
  int depth = 0;
  bool inQuote = false;
  bool expectOperand = true;
  size_t splitAt = std::string::npos;
  while (i < n) {
    while (i < n && std::isspace(static_cast<unsigned char>(rest[i]))) ++i;
    if (i >= n) break;
    const size_t tokStart = i;
    while (i < n) {
      const char c = rest[i];
      if (c == '"') inQuote = !inQuote;
      else if (!inQuote && (c == '(' || c == '[')) ++depth;
      else if (!inQuote && (c == ')' || c == ']')) --depth;
      else if (!inQuote && depth == 0 && std::isspace(static_cast<unsigned char>(c))) break;
      ++i;
    }
    const std::string tok = rest.substr(tokStart, i - tokStart);
    if (inQuote || depth != 0) return false;  // unbalanced — refuse to guess
    if (expectOperand) {
      // A lone `!`/`~` is a unary prefix; anything else completes an operand
      // unless it trails a binary operator awaiting its right-hand side.
      if (tok != "!" && tok != "~") expectOperand = endsWithBinOp(tok);
    } else if (isBinOpToken(tok)) {
      expectOperand = true;
    } else {
      splitAt = tokStart;  // an operand where an operator belongs = the command
      break;
    }
  }
  if (splitAt == std::string::npos || splitAt == 0) return false;
  cond = rest.substr(0, splitAt);
  body = rest.substr(splitAt);
  // trim
  auto trimBoth = [](std::string& s2) {
    size_t a = s2.find_first_not_of(" \t");
    size_t b = s2.find_last_not_of(" \t");
    s2 = (a == std::string::npos) ? std::string() : s2.substr(a, b - a + 1);
  };
  trimBoth(cond);
  trimBoth(body);
  return !cond.empty() && !body.empty();
}

ojson runOneLineIf(const std::string& condition, const std::string& bodyLine,
                   SessionState& session, const DispatchFn& dispatchFn) {
  return runIf(condition, {bodyLine}, {}, session, dispatchFn);
}

// BUG-547: is `line` a legacy `for` header? First word is exactly `for`
// (case-insensitive), word-bounded — `format`'s minimum abbreviation is also
// `for` in real Stata, but `format` never spells its own name with a 4th
// character that is whitespace right after `for`, so the word-boundary test
// separates the two without needing to look at what follows. `forvalues`
// (and its truncations) is excluded the same way: its 4th character is never
// whitespace either. Does NOT require a colon here — a missing colon is
// runLegacyFor's own r(198) validation (cmd-triage.json design point 4), not
// a reason to fall through to "command not found".
bool isLegacyForLine(const std::string& line) {
  std::string t = trim(line);
  if (t.size() <= 3) return false;
  if (std::tolower(static_cast<unsigned char>(t[0])) != 'f' ||
      std::tolower(static_cast<unsigned char>(t[1])) != 'o' ||
      std::tolower(static_cast<unsigned char>(t[2])) != 'r') {
    return false;
  }
  return std::isspace(static_cast<unsigned char>(t[3])) != 0;
}

// BUG-547: the legacy `for` command. Full contract (cmd-triage.json design,
// StataNow docs `p.pdf` foreach/for syntax reading, oracle
// tests/eval/golden/aer_w2/w2_cmdtriage.log `for_*` arms):
//
//   for [NAME in] TYPE list [\ [NAME in] TYPE list ...] [, options]:
//       body [\ body ...]
//
// - Header = text up to the first UNQUOTED ':' (eval::searchUnquoted), same
//   rule the frame prefix uses — the BODY may itself contain a colon
//   (oracle for_by_colon_noheader: `for num 1 2, noheader: bysort g: egen
//   ...`), so only the FIRST one outside a quoted string is `for`'s own.
// - Macros expand in the HEADER only (design brief) — the body is
//   substituted below and dispatched as an ordinary command, which expands
//   its own macros exactly once when it runs (oracle
//   for_macro_name_has_X: `$XC` is NOT touched by the placeholder
//   substitution, only resolved when the generated `gen` command executes).
// - Options (dryrun, noheader, nostop, pause) are peeled at the first
//   top-level comma; any other option is refused r(198) by name. `pause`
//   is accepted and is a no-op: real Stata's pause is an interactive
//   per-iteration halt, meaningless in this engine's batch/headless
//   execution (no oracle arm exercises it).
// - Lists split on unquoted `\`; each is `[NAME in] TYPE elements`
//   (parseForListSegment). Max 9 lists (default placeholders X Y Z A B C D
//   E F exhaust there).
// - Validation, each r(198): unequal list lengths, a placeholder absent
//   from the body, a missing colon, a bad list type/segment, more than 9
//   lists.
// - Body splits on unquoted `\` into commands. Per iteration, per command:
//   blind-substitute every list's placeholder (substitutePlaceholders),
//   emit a `->  <cmd>` display block unless noheader (oracle: every
//   for_num/for_any_multi/... arm shows exactly this, two spaces after
//   `->`), then dispatch unless dryrun. The dispatched command's own echo
//   block is dropped (Stata's `for` does not double-echo — only the
//   `-> ` line appears in the oracle log).
// - The first uncaptured error STOPS the loop and propagates unmodified
//   (oracle for_stop_on_error: iteration 3 never runs, its variable is
//   never created). With `nostop`, that error is tagged captured:true (the
//   same shape dispatcher.cpp's single-line capture prefix uses) and
//   iteration continues (oracle for_nostop: all 3 iterations run, _rc 0).
ojson runLegacyFor(const std::string& rawLine, SessionState& session,
                   const DispatchFn& dispatchFn) {
  std::string line = trim(rawLine);
  static const std::regex reColon(R"(:)");
  std::smatch cm;
  if (!eval::searchUnquoted(line, reColon, cm)) {
    return ojson::array({errFor198("for: invalid syntax: missing ':'")});
  }
  std::string headerRaw = line.substr(0, static_cast<size_t>(cm.position()));
  std::string bodyRaw = line.substr(static_cast<size_t>(cm.position()) + 1);

  // Strip the leading `for` keyword itself.
  {
    size_t sp = headerRaw.find_first_of(" \t");
    headerRaw = (sp == std::string::npos) ? std::string() : headerRaw.substr(sp + 1);
  }

  if (eval::hasMacroRefs(headerRaw)) {
    eval::ExpandResult exp = eval::expandMacros(headerRaw, session.eval);
    if (exp.isError) return ojson::array({exp.error});
    headerRaw = exp.line;
  }

  auto commaSplit = splitTopLevelCommaFor(headerRaw);
  std::string listSpecText = commaSplit.first;
  bool optDryrun = false, optNoheader = false, optNostop = false;
  if (commaSplit.second.has_value()) {
    // Declared minimum abbreviations, transcribed from for.ado:91
    // `local options "Dryrun noHeader noStop Pause"` (the capitals are the
    // minimum): d / noh / nos / p. Matched case-sensitively, as Stata's
    // parse does. Not eval::resolveOptionAbbrev: its 2-char floor would
    // refuse `d` and `p`.
    struct ForOptMin {
      const char* full;
      size_t minLen;
    };
    static const ForOptMin kForOpts[] = {
        {"dryrun", 1}, {"noheader", 3}, {"nostop", 3}, {"pause", 1}};
    for (const auto& tok : splitForeachTokens(*commaSplit.second)) {
      std::string lo;
      for (const auto& om : kForOpts) {
        const std::string full(om.full);
        if (tok.size() >= om.minLen && tok.size() <= full.size() &&
            full.compare(0, tok.size(), tok) == 0) {
          lo = full;
          break;
        }
      }
      if (lo == "dryrun") {
        optDryrun = true;
      } else if (lo == "noheader") {
        optNoheader = true;
      } else if (lo == "nostop") {
        optNostop = true;
      } else if (lo != "pause") {
        // OPTION VOCABULARY: anything that is not a valid abbreviation of
        // dryrun/noheader/nostop/pause is refused by name, r(198), as
        // for.ado's parse does -- never silently dropped.
        return ojson::array({errFor198("for: option " + tok + " not allowed")});
      }
      // "pause": accepted, no-op (see the function comment above).
    }
  }

  std::vector<std::string> segs = splitOnUnquotedBackslash(listSpecText);
  if (segs.empty() || segs.size() > 9) {
    return ojson::array({errFor198("for: invalid syntax")});
  }
  static const char* const kDefaultNames[9] = {"X", "Y", "Z", "A", "B", "C", "D", "E", "F"};
  std::vector<ForListSpec> lists;
  lists.reserve(segs.size());
  for (size_t i = 0; i < segs.size(); ++i) {
    ojson err;
    std::optional<ForListSpec> spec =
        parseForListSegment(segs[i], kDefaultNames[i], session, &err);
    if (!spec.has_value()) return ojson::array({err});
    lists.push_back(*spec);
  }
  size_t n = lists[0].tokens.size();
  for (const auto& l : lists) {
    if (l.tokens.size() != n) {
      return ojson::array({errFor198("for: invalid syntax: lists have unequal length")});
    }
  }
  for (const auto& l : lists) {
    if (bodyRaw.find(l.placeholder) == std::string::npos) {
      return ojson::array(
          {errFor198("for: invalid syntax: placeholder '" + l.placeholder +
                    "' does not occur in the command")});
    }
  }

  std::vector<std::string> commands = splitOnUnquotedBackslash(bodyRaw);
  for (auto& c : commands) c = trim(c);

  ojson allBlocks = ojson::array();
  for (size_t i = 0; i < n; ++i) {
    std::vector<std::pair<std::string, std::string>> subs;
    subs.reserve(lists.size());
    for (const auto& l : lists) subs.emplace_back(l.placeholder, l.tokens[i]);
    for (const auto& cmdText : commands) {
      std::string substituted = substitutePlaceholders(cmdText, subs);
      if (!optNoheader) {
        ojson d;
        d["type"] = "display";
        d["text"] = "->  " + substituted;
        allBlocks.push_back(d);
      }
      if (optDryrun) continue;
      ojson result = dispatchFn(substituted, session);
      const ojson* errBlockPtr = nullptr;
      for (const auto& b : result) {
        if (b.is_object() && b.value("type", std::string()) == "echo") {
          continue;  // for's own "-> " line replaces the dispatched echo
        }
        bool isUncapturedError =
            b.is_object() && b.value("type", std::string()) == "error" &&
            !b.value("captured", false);
        if (isUncapturedError) {
          errBlockPtr = &b;
          if (optNostop) {
            ojson tagged = b;
            tagged["captured"] = true;
            allBlocks.push_back(tagged);
            continue;
          }
        }
        allBlocks.push_back(b);
      }
      if (errBlockPtr != nullptr && !optNostop) {
        return allBlocks;  // stop: the real error propagates unmodified
      }
    }
  }
  return allBlocks;
}

namespace {

// FULL FUNCTIONAL CHECK (2026-08-05). Peel leading PREFIX keywords off a block
// header while what remains still starts with a CONTROL keyword, e.g.
// `quietly forvalues i = 1/10 {` -> ("quietly", "forvalues i = 1/10").
//
// Why this exists: executeLoop dispatches on the header's FIRST WORD, so a
// prefixed loop header matched the prefix branch and ran the body as a bare
// `quietly { … }` block — the `forvalues i = 1/10` part was discarded outright.
// The body therefore executed ONCE with the loop macro never defined, which
// surfaces as "Local macro `i' is not defined". Stata accepts a prefix on any
// command, loop headers included. Returns false when the header is not a
// prefixed control header, leaving every existing dispatch untouched.
bool splitPrefixedControlHeader(const std::string& hdr, std::vector<std::string>& prefixes,
                                std::string& inner) {
  static const std::unordered_set<std::string> kControl = {"foreach", "forvalues", "forval",
                                                          "forv",    "forvalue",  "while",
                                                          "if"};
  std::string rest = trim(hdr);
  prefixes.clear();
  while (true) {
    std::string fw = lower(firstWord(rest));
    if (fw.empty()) return false;
    if (kControl.count(fw) != 0) {
      // `if` is BOTH a control keyword and a legal prefix-free header; only
      // treat what we peeled as prefixes if we actually peeled something.
      if (prefixes.empty()) return false;
      inner = rest;
      return true;
    }
    if (loopIfKeywords().count(fw) == 0) return false;  // not a prefix either
    prefixes.push_back(fw);
    size_t sp = rest.find_first_of(" \t");
    if (sp == std::string::npos) return false;          // prefix with no inner command
    rest = trim(rest.substr(sp));
  }
}

bool isCapturePrefix(const std::string& w) {
  return w == "capture" || w == "captur" || w == "captu" || w == "capt" || w == "cap";
}
bool isQuietlyPrefix(const std::string& w) {
  return w == "quietly" || w == "qui" || w == "quie" || w == "quiet";
}
// The noisily-class spellings splitPrefixedControlHeader accepts. Everything
// that is neither capture- nor quietly-class in a prefix chain is this, but the
// BUG-189 log scope has to say WHICH it is (noisily pushes a visible scope,
// capture pushes none), so the set is named rather than inferred by elimination.
bool isNoisilyPrefix(const std::string& w) {
  return w == "noisily" || w == "noisil" || w == "noisi" || w == "nois" ||
         w == "noi" || w == "no" || w == "n";
}

}  // namespace

ojson executeLoop(const std::string& header, const std::vector<std::string>& bodyLines,
                   const std::vector<std::string>& elseBodyLines, SessionState& session,
                   const DispatchFn& dispatchFn) {
  std::string hdr = rstripBrace(header);
  // AER-SCOPE1 bundle A: fold the accepted truncations onto the canonical head
  // ONCE, here, rather than widening the five `fw == "forvalues"` compares and
  // three `^forvalues\s+` regexes downstream. Five spellings times eight sites
  // is forty places for one of them to be forgotten; this is one place, and the
  // prefixed path (`quietly forval i=1/3 {`) reaches it too because that path
  // recurses back into executeLoop with the inner header.
  {
    size_t sp = hdr.find_first_of(" \t");
    std::string head = lower(sp == std::string::npos ? hdr : hdr.substr(0, sp));
    if (head == "forval" || head == "forv" || head == "forvalue") {
      hdr = "forvalues" + (sp == std::string::npos ? std::string() : hdr.substr(sp));
    }
  }

  // A prefixed control header runs the CONTROL construct and then applies the
  // prefix's own semantics to what it produced — innermost prefix first, so
  // `capture quietly forvalues …` quiets the loop and then captures it, the
  // same order the single-line prefix stripper composes in.
  {
    std::vector<std::string> prefixes;
    std::string inner;
    if (splitPrefixedControlHeader(hdr, prefixes, inner)) {
      // BUG-189: `quietly forvalues … {` must hide its body from the log too.
      // The stack is innermost-wins, so pushing the prefixes outermost-first
      // leaves the innermost keyword on top — the same rule the result filters
      // below implement by folding in reverse.
      std::vector<std::unique_ptr<SessionState::LogVerbosityScope>> lvScopes;
      for (const auto& p : prefixes) {
        if (isQuietlyPrefix(p)) {
          lvScopes.push_back(std::make_unique<SessionState::LogVerbosityScope>(session, true));
        } else if (isNoisilyPrefix(p)) {
          lvScopes.push_back(std::make_unique<SessionState::LogVerbosityScope>(session, false));
        }
      }
      ojson result = executeLoop(inner, bodyLines, elseBodyLines, session, dispatchFn);
      lvScopes.clear();  // close the scopes before the result filters run
      for (auto it = prefixes.rbegin(); it != prefixes.rend(); ++it) {
        if (isQuietlyPrefix(*it)) {
          // quietly: keep command echoes + errors, drop ordinary output —
          // the same filter runQuietlyBlock applies to a braced body.
          ojson kept = ojson::array();
          for (const auto& b : result) {
            std::string t = b.value("type", std::string());
            if (t == "echo" || t == "error" || t == "_control") kept.push_back(b);
          }
          result = kept;
        } else if (isCapturePrefix(*it)) {
          // capture: swallow an uncaught error and publish _rc, matching
          // runCaptureBlock. A loop that errored has already stopped itself.
          ojson kept = ojson::array();
          bool errored = false;
          // BUG-546: the exact coarse rc of the FIRST uncaptured error block,
          // not a hardcoded "1" — mirrors runCaptureBlock's own fix above, and
          // the pre-existing single-line `capture` prefix in dispatcher.cpp,
          // which already reads eval::coarseRc. Oracle: w2_openbugs.log
          // fe_unknown/fe_mixed via `capture noisily foreach ... { }`.
          std::string errRc;
          for (const auto& b : result) {
            if (b.value("type", std::string()) == "error" && !b.value("captured", false)) {
              errored = true;
              if (errRc.empty()) errRc = eval::coarseRc(b);
              // BUG-350 coverage gap, found 2026-09-01 by the leg-1D probe-2
              // audit: this is the SECOND place an error block is dropped where
              // the user cannot see it, and it was not counted. The counter's
              // whole claim is "a run that hid something says so", and a
              // `capture` on a LOOP prefix hid it silently. Counted here for the
              // same reason and by the same call as dispatcher.cpp's site.
              //
              // Note this arm drops the block even under `capture noisily`
              // (prefix iteration applies the capture filter regardless), so
              // unlike the command path there is nothing for the user to see and
              // counting it is right rather than a false positive.
              piet::counters::noteSwallowedError(b.value("message", std::string()));
              continue;
            }
            kept.push_back(b);
          }
          session.eval.locals.set("_rc", errored ? errRc : "0");
          result = kept;
        }
        // noisily: fully transparent, nothing to do.
      }
      return result;
    }
  }

  // Macro expansion of forvalues/foreach headers ONLY (while/if re-expand
  // per-iteration inside their own runners) — loops.py's has_macro_refs guard.
  std::string fw = headKeyword(hdr);
  if ((fw == "forvalues" || fw == "foreach") && eval::hasMacroRefs(hdr)) {
    eval::ExpandResult exp = eval::expandMacros(hdr, session.eval);
    if (exp.isError) return ojson::array({exp.error});
    hdr = exp.line;
  }

  // foreach VAR of varlist VARLIST
  //
  // BUG-545: the keyword accepts any >=3-char prefix of `varlist` (`var`,
  // `varl`, `vari`, `varli`, `varlis`, `varlist`), not the literal word only.
  // Corpus-measured: 19 lines spell `of var`, 1 spells `of varl`, 20 lines in
  // 8 papers total (open-bugs.json use_count_measured); oracle
  // fe_of_var (w2_openbugs.log) is rc 0 for `of var`.
  {
    static const std::regex re(R"(^foreach\s+(\w+)\s+of\s+var(?:l(?:i(?:s(?:t)?)?)?)?\s+(.*))",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      VarlistExpandResult vr = expandVarlistImpl(trim(m.str(2)), session);
      if (vr.isError) return ojson::array({vr.error});
      // BUG-509 (corrected)/BUG-544: `foreach v of varlist nosuchcol` is
      // r(111) "variable nosuchcol not found", caught BEFORE any iteration --
      // the entry's original r(199) pin was an artifact of the oracle's
      // `capture noisily foreach ... {` spelling (the 199 was the orphaned
      // `}`, not the foreach's own rc). Also fires for a MIXED list (oracle
      // fe_mixed: `alpha nosuchcol` never iterates `alpha`). KNOWN DISCLOSED
      // GAP: unique abbreviations (`alph` for `alpha`) are refused r(111)
      // here, while Stata resolves them (oracle fe_abbrev: rc 0, iterates
      // `alpha`). PIET has no variable-name abbreviation anywhere, so adding
      // it only inside foreach would be an inconsistent partial feature; the
      // refusal is honest and named (BUGLOG BUG-544 "Known gap", selftest arm
      // W2 loops/BUG-544 fe_abbrev), never a silent accept.
      if (auto err = checkKnownVarTokens(vr.tokens, session)) {
        return ojson::array({*err});
      }
      return runForeach(m.str(1), vr.tokens, bodyLines, session, dispatchFn);
    }
  }
  // foreach VAR of numlist NUMLIST
  {
    static const std::regex re(R"(^foreach\s+(\w+)\s+of\s+numlist\s+(.*))",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      NumlistResult nr = parseNumlist(trim(m.str(2)));
      if (nr.isError) return ojson::array({nr.error});
      return runForeach(m.str(1), nr.tokens, bodyLines, session, dispatchFn);
    }
  }
  // foreach VAR of local NAME
  {
    static const std::regex re(R"(^foreach\s+(\w+)\s+of\s+local\s+(\w+)\s*$)",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      std::vector<std::string> tokens = splitForeachTokens(session.eval.locals.get(m.str(2), ""));
      return runForeach(m.str(1), tokens, bodyLines, session, dispatchFn);
    }
  }
  // foreach VAR of global NAME
  {
    static const std::regex re(R"(^foreach\s+(\w+)\s+of\s+global\s+(\w+)\s*$)",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      std::vector<std::string> tokens = splitForeachTokens(session.eval.globals.get(m.str(2), ""));
      return runForeach(m.str(1), tokens, bodyLines, session, dispatchFn);
    }
  }
  // foreach VAR of newlist TOKENS (BS-034a, BETA campaign C3) — newlist is
  // a list of NEW variable NAMES (e.g. names not yet created as columns,
  // often built from `tempvar`); unlike `of varlist` above, there is NO
  // existence check against the current dataset — iteration is simply over
  // the tokens as given, same split rule as the plain "in" form (oracle:
  // `foreach v of newlist na nb nc` -> "newvar na" / "newvar nb" /
  // "newvar nc"). Must come before the plain "in" form for the SAME reason
  // the other `of ...` forms do (though there's no actual overlap risk
  // here — "newlist" the literal keyword vs "in" — ordering just mirrors
  // the existing convention).
  {
    static const std::regex re(R"(^foreach\s+(\w+)\s+of\s+newlist\s+(.*))",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      std::vector<std::string> tokens = splitForeachTokens(trim(m.str(2)));
      return runForeach(m.str(1), tokens, bodyLines, session, dispatchFn);
    }
  }
  // foreach VAR in TOKENS  (must come AFTER the `of` forms)
  {
    static const std::regex re(R"(^foreach\s+(\w+)\s+in\s+(.*))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      std::vector<std::string> tokens = splitForeachTokens(trim(m.str(2)));
      return runForeach(m.str(1), tokens, bodyLines, session, dispatchFn);
    }
  }
  // forvalues i = #1 #2 to #3  /  forvalues i = #1 #2 : #3 (BS-034b/c, BETA
  // campaign C3) — Stata's "explicit first two values" numlist form: start
  // = #1, the step is IMPLIED by the gap between the first two values (#2 -
  // #1), end = #3; `to`/`:` are interchangeable separators (oracle:
  // `forvalues i = 1 3 to 9` == `forvalues i = 1 3 : 9` -> 1 3 5 7 9). Must
  // be checked AFTER the `(STEP)` and `/` forms below it structurally
  // cannot ambiguously match this (neither uses bare whitespace as its
  // start/second-number separator) — placed here regardless, matching
  // this file's existing top-to-bottom convention. The already-working
  // `1(3)9` form is untouched — this is a syntactically distinct header.
  {
    static const std::regex re(
        R"(^forvalues\s+(\w+)\s*=\s*([+-]?\d+(?:\.\d+)?)\s+([+-]?\d+(?:\.\d+)?)\s*(?:to|:)\s*([+-]?\d+(?:\.\d+)?))",
        std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      double first = std::stod(m.str(2)), second = std::stod(m.str(3)), end = std::stod(m.str(4));
      double step = second - first;
      if (step == 0.0) {
        return ojson::array({errBlock("forvalues: step cannot be zero")});
      }
      return runForvaluesRange(m.str(1), first, step, end, bodyLines, session,
                               dispatchFn);
    }
  }
  // forvalues i=START(STEP)END
  {
    static const std::regex re(
        R"(^forvalues\s+(\w+)\s*=\s*([+-]?\d+(?:\.\d+)?)\s*\(([+-]?\d+(?:\.\d+)?)\)\s*([+-]?\d+(?:\.\d+)?))",
        std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      double start = std::stod(m.str(2)), step = std::stod(m.str(3)), end = std::stod(m.str(4));
      if (step == 0.0) {
        return ojson::array({errBlock("forvalues: step cannot be zero")});
      }
      return runForvaluesRange(m.str(1), start, step, end, bodyLines, session,
                               dispatchFn);
    }
  }
  // forvalues i=START/END  (step=1)
  {
    static const std::regex re(
        R"(^forvalues\s+(\w+)\s*=\s*([+-]?\d+(?:\.\d+)?)\s*/\s*([+-]?\d+(?:\.\d+)?))",
        std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      double start = std::stod(m.str(2)), end = std::stod(m.str(3));
      double step = start <= end ? 1.0 : -1.0;
      if (step == 0.0) {
        return ojson::array({errBlock("forvalues: step cannot be zero")});
      }
      return runForvaluesRange(m.str(1), start, step, end, bodyLines, session,
                               dispatchFn);
    }
  }
  // while EXPR
  {
    static const std::regex re(R"(^while\s+(.*))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      return runWhile(trim(m.str(1)), bodyLines, session, dispatchFn);
    }
  }
  // if EXPR [else { ... }]
  {
    static const std::regex re(R"(^if\s+(.*))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      return runIf(trim(m.str(1)), bodyLines, elseBodyLines, session, dispatchFn);
    }
  }
  // capture { ... } (BS-015) — not a loop/branch; `fw` (computed above) is
  // already the lowered first word of the header, so no re-parsing needed.
  if (fw == "capture" || fw == "captur" || fw == "captu" || fw == "capt" || fw == "cap") {
    return runCaptureBlock(bodyLines, session, dispatchFn);
  }
  // BUG-132: quietly/noisily { ... } block — not a loop/branch; suppress
  // (quietly) or pass through (noisily) the body's output, but (unlike
  // capture) never swallow an uncaught error. `fw` is the lowered first word;
  // spellings mirror the dispatcher's kQuietlyPrefixes split.
  if (fw == "quietly" || fw == "qui" || fw == "quie" || fw == "quiet") {
    return runQuietlyBlock(bodyLines, session, dispatchFn, /*quiet=*/true);
  }
  if (fw == "noisily" || fw == "noisil" || fw == "noisi" || fw == "nois" || fw == "noi" ||
      fw == "no" || fw == "n") {
    return runQuietlyBlock(bodyLines, session, dispatchFn, /*quiet=*/false);
  }
  // PLEVEL-IMPL-WAVE arc ①: `parallel[, workers(N)] { … }`.
  if (fw == "parallel") {
    return runParallelBlock(hdr, bodyLines, session, dispatchFn);
  }

  return ojson::array({errBlock("loops: unrecognised loop header: '" + hdr + "'")});
}

// ---------------------------------------------------------------------------
// EXPRLAYER drift guard (see loops.h).

bool selftestBoolIdentScanMatchesRegex(std::string* firstDisagreement) {
  static const std::vector<std::string> kCases = {
      "`w' < 250", "w < 250", "mod(i,3)==0", "i==1000", "_rc", "_N > 500", "_n == 1",
      "x1 > 0 & x2 < 1", "__STR0__ == \"a\"", "1e+307 > x", "1.5e-9 < y", "a.b",
      "_x9 + x_9", "abc", "", "   ", "3", "3x", "x3", "e(N) > 0", "scalar(q) != .",
      "__STR10__", "__STRx", "not x", "x or y", "8.988465674311579e+307",
      "f(1)+g2(3)", "_", "__", "___STR0__", "1_x", "x1e5", "9e", "a1b2c3",
  };
  static const std::regex reIdentTok(R"([A-Za-z_]\w*)");
  for (const auto& c : kCases) {
    // The regex arm — verbatim the loop this replaced.
    std::set<std::string> viaRegex;
    for (auto it = std::sregex_iterator(c.begin(), c.end(), reIdentTok);
         it != std::sregex_iterator(); ++it) {
      const size_t pos = static_cast<size_t>(it->position());
      if (pos > 0 &&
          (std::isdigit(static_cast<unsigned char>(c[pos - 1])) || c[pos - 1] == '.')) {
        continue;
      }
      std::string tok = it->str();
      if (tok.rfind("__STR", 0) == 0) continue;
      viaRegex.insert(std::move(tok));
    }
    // The scan arm — verbatim the loop in evalBoolExpr.
    std::set<std::string> viaScan;
    const size_t n = c.size();
    for (size_t i = 0; i < n;) {
      const unsigned char ch = static_cast<unsigned char>(c[i]);
      if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_')) {
        ++i;
        continue;
      }
      size_t j = i + 1;
      while (j < n) {
        const unsigned char d = static_cast<unsigned char>(c[j]);
        if ((d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z') || (d >= '0' && d <= '9') ||
            d == '_') {
          ++j;
        } else {
          break;
        }
      }
      const bool expTail =
          i > 0 && (std::isdigit(static_cast<unsigned char>(c[i - 1])) || c[i - 1] == '.');
      if (!expTail && c.compare(i, 5, "__STR") != 0) viaScan.insert(c.substr(i, j - i));
      i = j;
    }
    if (viaRegex != viaScan) {
      if (firstDisagreement) {
        std::string a, b;
        for (const auto& t : viaRegex) a += t + " ";
        for (const auto& t : viaScan) b += t + " ";
        *firstDisagreement = "line=[" + c + "] regex={" + a + "} scan={" + b + "}";
      }
      return false;
    }
  }
  return true;
}

}  // namespace piet::loops
