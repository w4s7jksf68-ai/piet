// Shared ASCII string helpers (lower-case fold, " \t"-only trim) — hoisted
// out of the ~9 near-identical anonymous-namespace copies scattered across
// the native TUs (A5-04): dispatcher.cpp's lower()/trimWs(), handlers_m3.cpp's
// lower()/trim(), handlers.cpp's lower(), handlers_data_verbs.cpp's
// lower()/trim(), egen.cpp's lower()/trim(), postfile.cpp's lower()/trim(),
// loops.cpp's lower()/trim(), program.cpp's lower()/trim(), doexec.cpp's
// lower()/trim(), estimation_m4.cpp's lowerM4()/trimM4(), and
// eval/tokenizer.cpp's toLower(). Each of those TU-local copies has internal
// linkage (wrapped in its own anonymous namespace), so there was never an
// ODR violation at link time — just duplicated logic to keep in sync by
// hand. This header is the single source of truth going forward.
//
// TUs in this workstream's manifest (dispatcher.cpp, handlers_m3.cpp,
// eval/tokenizer.cpp) now forward their locally-named helper to these
// implementations, kept as thin one-line wrappers rather than removed
// outright — that way none of their many existing call sites needed
// touching (pure dedup, no behavior change). TUs outside the manifest
// (handlers.cpp, handlers_data_verbs.cpp, egen.cpp, postfile.cpp, loops.cpp,
// program.cpp, doexec.cpp, estimation_m4.cpp) still carry their own copy —
// left alone here and reported for a future sweep.
//
// NOT the same thing as eval::tokenizer.cpp's strip()/lstrip()/rstrip()
// family, which deliberately implements Python str.strip()'s wider
// whitespace set (space/tab/newline/CR/FF/VT/BOM-adjacent) for tokenizer
// parity — that trio is a different contract and is left untouched here.
#ifndef PIET_NATIVE_STRUTIL_H
#define PIET_NATIVE_STRUTIL_H

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <regex>
#include <string>
#include <vector>

namespace piet::strutil {

// ASCII-only case fold (std::tolower per byte) — matches every one of the
// duplicated `lower()`/`toLower()` copies this consolidates.
inline std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

// Strips leading/trailing ' '/'\t' only (NOT full Python-style whitespace —
// see eval::tokenizer.cpp's strip() for that wider variant) — matches every
// one of the duplicated `trim()`/`trimWs()` copies this consolidates.
inline std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t");
  size_t b = s.find_last_not_of(" \t");
  if (a == std::string::npos) return "";
  return s.substr(a, b - a + 1);
}

// BUG C (W3 fuzzer stabilization, kitchensink-fuzz fail category
// SILENT_SWALLOW): a new-variable name (gen's LHS, rename's target, ...)
// must be a valid Stata identifier — letters/digits/underscore, NOT
// starting with a digit, at most 32 characters, non-empty. Before this, the
// rule was enforced ONLY on strmutate.cpp's string-gen fallback path (its
// own `identRe()`, which didn't even check the length cap); the far more
// common numeric `gen`/`replace` path shipped straight to `cobra eval
// --gen` with no check at all, so `gen 9zzz = 1` silently created a column
// named "9zzz". This is now the ONE shared validator — every "new variable
// name" call site (gen numeric, gen/replace string-eval, rename) calls this
// same function so the rule can never drift between them again.
inline bool isValidStataIdentifier(const std::string& name) {
  if (name.empty() || name.size() > 32) return false;
  unsigned char c0 = static_cast<unsigned char>(name[0]);
  if (!(std::isalpha(c0) || c0 == '_')) return false;
  for (char c : name) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (!(std::isalnum(uc) || uc == '_')) return false;
  }
  return true;
}

// Shared human-readable explanation of the rule above — every "invalid
// variable name" error message quotes this SAME sentence (not a
// per-call-site paraphrase) so the wording can't drift either.
inline std::string stataIdentifierRuleText() {
  return "Stata variable names must start with a letter or underscore, contain only letters, "
         "digits, and underscores, and be at most 32 characters long.";
}

// ---- factor-level value-label substitution (BUG-102) -----------------------
// A factor-variable coefficient row's engine name is `<level>.<var>` (BUG-101,
// model.cpp:526), or `<level>.<var>#<level>.<var>` for interactions; real Stata
// substitutes the attached value label for the bare level when displaying such a
// row (verified live, StataBE 19.5). PIET's flat one-row-per-coefficient table
// keeps the ".varname" suffix — there is no shared group header the way Stata's
// ASCII table has one — so only the level segment is substituted.
//
// WHY IT LIVES HERE. This was TWO byte-identical hand-written copies —
// handlers.cpp's `substituteFactorLevelLabel` (the M2/regress slice) and
// estimation_m4.cpp's `substituteFactorLevelLabelM4` — differing in ONE comment
// line and nothing else. The copy's own note said no shared header linked the
// two anonymous namespaces; the answer was to make one. Two copies of a display
// RULE that must agree is the twin class this codebase has already been bitten
// by (BUG-195's formatter twins, and BUG-112(8), where landing a fix in only the
// M4a copy left `regress` — the most common estimator — still broken).
//
// It takes the label TABLE rather than a SessionState precisely so it can live
// in this dependency-light header: the rule needs `varname -> {code: text}` and
// nothing else about a session.
inline std::string substituteFactorLevelLabel(
    const std::string& name,
    const std::map<std::string, std::map<int, std::string>>& valueLabels) {
  static const std::regex segRe(R"(^(-?\d+)\.(\w+)$)");
  if (valueLabels.empty()) return name;
  std::vector<std::string> segs;
  size_t p = 0;
  while (true) {
    size_t q = name.find('#', p);
    segs.push_back(name.substr(p, q == std::string::npos ? std::string::npos : q - p));
    if (q == std::string::npos) break;
    p = q + 1;
  }
  bool changed = false;
  for (auto& seg : segs) {
    std::smatch m;
    if (!std::regex_match(seg, m, segRe)) continue;
    auto vlIt = valueLabels.find(m[2].str());
    if (vlIt == valueLabels.end()) continue;
    // `m[1]` is regex-guaranteed `-?\d+`, so the conversion cannot fail on a
    // shape this branch accepts (clang-tidy's unchecked-conversion warning here
    // is answered by the guard, not by the call).
    auto codeIt = vlIt->second.find(std::atoi(m[1].str().c_str()));
    if (codeIt == vlIt->second.end()) continue;
    seg = codeIt->second + "." + m[2].str();
    changed = true;
  }
  if (!changed) return name;
  std::string out;
  for (size_t i = 0; i < segs.size(); ++i) {
    if (i) out += "#";
    out += segs[i];
  }
  return out;
}

// A minimal glob: `*` (any run, including empty) and `?` (one character).
// Deliberately NOT <fnmatch.h>: that header does not exist on the Windows target
// this codebase cross-compiles for, and a pattern matcher is small enough that
// adding a platform dependency for it would cost more than it saves.
//
// This is Stata's `match()` / `strmatch()` for the surfaces that need it as a
// utility. It moved here from handlers_aer_util.cpp's anonymous namespace,
// byte-identical, when handlers_aer_tables.cpp needed the SAME matcher for
// esttab/estout `keep()`/`drop()`/`order()` and `transform()`'s coefficient
// patterns -- a second copy in a sibling translation unit is how two surfaces
// end up disagreeing about what `*` means. `loops.cpp` and `estimation_m4c.cpp`
// still carry their own (BUG-496 records the count); they are not callers here.
inline bool globMatch(const std::string& pat, const std::string& s) {
  size_t p = 0, i = 0, star = std::string::npos, mark = 0;
  while (i < s.size()) {
    if (p < pat.size() && (pat[p] == '?' || pat[p] == s[i])) {
      ++p;
      ++i;
    } else if (p < pat.size() && pat[p] == '*') {
      star = p++;
      mark = i;
    } else if (star != std::string::npos) {
      p = star + 1;
      i = ++mark;
    } else {
      return false;
    }
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
}

}  // namespace piet::strutil

#endif  // PIET_NATIVE_STRUTIL_H
