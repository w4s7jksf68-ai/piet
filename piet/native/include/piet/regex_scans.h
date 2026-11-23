// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Hand scans that answer exactly what a few std::regex patterns on the
// interpreter's per-command path answer, without running the regex engine.
//
// Why. A do-file loop body runs every command through the dispatcher, and each
// command paid a handful of std::regex matches whose answer is decided by its
// first few characters (`^\s*bootstrap\b...`, `^(\w+)\s*\((.*)\)\s*$`). libc++'s
// regex engine costs microseconds per call even when it fails at once; on the
// AEA package openICPSR-226844 (a 12-command loop run 39,000 times) these
// matches were about 13% of PIET's own CPU (2026-11-20 profile).
//
// The contract. Each function below names the pattern it stands for and gives
// the same answer and the same groups for every input. The character classes
// are the ECMAScript ones as libc++ reads them under the classic "C" locale,
// which is the only locale PIET runs in (nothing calls std::locale::global or
// setlocale): \s is space, \t, \n, \v, \f, \r; \w is [A-Za-z0-9_]; `.` is any
// byte but \n and \r; icase folds ASCII letters only. The selftest
// testRegexScansMatchRegex (selftest_language_hand_scans.cpp) holds every
// function to its pattern on generated inputs, the original regex being the
// judge; change one only together with that test.
//
// A `may...` function is a necessary condition only: false means the pattern
// cannot match, true means "run the regex" (the caller still does).
#ifndef PIET_NATIVE_REGEX_SCANS_H
#define PIET_NATIVE_REGEX_SCANS_H

#include <cstddef>
#include <string>
#include <vector>

namespace piet::rx {

inline bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
inline bool isWord(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
inline bool isDot(char c) { return c != '\n' && c != '\r'; }
inline char foldAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// The words `std::istringstream >> std::string` reads from `s` in the classic
// locale: maximal runs of bytes that are not isspace().
inline std::vector<std::string> splitWords(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    while (i < n && isSpace(s[i])) ++i;
    if (i >= n) break;
    size_t j = i;
    while (j < n && !isSpace(s[j])) ++j;
    out.emplace_back(s, i, j - i);
    i = j;
  }
  return out;
}

// May an icase pattern `^\s*WORD...` match? (`word` in lower case.) After any
// leading \s, `s` must begin with `word`, ASCII case folded.
inline bool mayStartWithWordCI(const std::string& s, const char* word) {
  size_t i = 0;
  while (i < s.size() && isSpace(s[i])) ++i;
  for (size_t k = 0; word[k] != '\0'; ++k, ++i) {
    if (i >= s.size() || foldAscii(s[i]) != word[k]) return false;
  }
  return true;
}

// May a pattern that needs one \s somewhere match? (`s` holds a \s byte.)
inline bool hasSpace(const std::string& s) {
  for (char c : s)
    if (isSpace(c)) return true;
  return false;
}

// regex_match(s, R"(^(\w+)\s*\((.*)\)\s*$)"): fn is group 1, args group 2.
inline bool matchFnCall(const std::string& s, std::string* fn, std::string* args) {
  const size_t n = s.size();
  size_t i = 0;
  while (i < n && isWord(s[i])) ++i;
  if (i == 0) return false;
  size_t open = i;
  while (open < n && isSpace(s[open])) ++open;
  if (open >= n || s[open] != '(') return false;
  size_t end = n;  // one past the closing ')'
  while (end > open + 1 && isSpace(s[end - 1])) --end;
  if (end <= open + 1 || s[end - 1] != ')') return false;
  for (size_t k = open + 1; k + 1 < end; ++k)
    if (!isDot(s[k])) return false;
  if (fn != nullptr) fn->assign(s, 0, i);
  if (args != nullptr) args->assign(s, open + 1, end - 1 - (open + 1));
  return true;
}

// regex_match(s, R"(^([^=]*=\s*)(\w+)\s*\((.*)\)\s*$)"): lhs, fn, args are
// groups 1-3. A caller whose group 2 is an alternation of plain words (each \w
// only) checks `fn` against its list: the alternation can only match the whole
// \w run, since the byte after it must be \s or '('.
inline bool matchAssignFnCall(const std::string& s, std::string* lhs, std::string* fn,
                              std::string* args) {
  const size_t eq = s.find('=');
  if (eq == std::string::npos) return false;
  size_t b = eq + 1;
  while (b < s.size() && isSpace(s[b])) ++b;
  if (!matchFnCall(s.substr(b), fn, args)) return false;
  if (lhs != nullptr) lhs->assign(s, 0, b);
  return true;
}

// regex_match(s, R"(^(byte|int|long|float|double|str\d+|strl)$)").
inline bool matchLowerTypeWord(const std::string& s) {
  if (s == "byte" || s == "int" || s == "long" || s == "float" || s == "double" || s == "strl")
    return true;
  if (s.size() < 4 || s.compare(0, 3, "str") != 0) return false;
  for (size_t k = 3; k < s.size(); ++k)
    if (s[k] < '0' || s[k] > '9') return false;
  return true;
}

// regex_match(s, R"(^(\w+)(\s*\(.*)$)"): head is group 1, rest group 2.
inline bool matchFnHead(const std::string& s, std::string* head, std::string* rest) {
  const size_t n = s.size();
  size_t i = 0;
  while (i < n && isWord(s[i])) ++i;
  if (i == 0) return false;
  size_t open = i;
  while (open < n && isSpace(s[open])) ++open;
  if (open >= n || s[open] != '(') return false;
  for (size_t k = open + 1; k < n; ++k)
    if (!isDot(s[k])) return false;
  if (head != nullptr) head->assign(s, 0, i);
  if (rest != nullptr) rest->assign(s, i, std::string::npos);
  return true;
}

// regex_search(s, R"(^(\w+)\s*\()"), or R"(^\s*(\w+)\s*\()" with
// `leadingSpace`: fn is group 1. (Anchored, so only position 0 can match.)
inline bool searchFnName(const std::string& s, bool leadingSpace, std::string* fn) {
  const size_t n = s.size();
  size_t a = 0;
  if (leadingSpace)
    while (a < n && isSpace(s[a])) ++a;
  size_t i = a;
  while (i < n && isWord(s[i])) ++i;
  if (i == a) return false;
  size_t open = i;
  while (open < n && isSpace(s[open])) ++open;
  if (open >= n || s[open] != '(') return false;
  if (fn != nullptr) fn->assign(s, a, i - a);
  return true;
}

// regex_match(s, R"(^(\w+)\s*\(\s*\)\s*$)"): fn is group 1. (A caller's
// alternation of plain words for group 1 is the caller's check on `fn`.)
inline bool matchFnEmptyCall(const std::string& s, std::string* fn) {
  const size_t n = s.size();
  size_t i = 0;
  while (i < n && isWord(s[i])) ++i;
  if (i == 0) return false;
  size_t k = i;
  while (k < n && isSpace(s[k])) ++k;
  if (k >= n || s[k] != '(') return false;
  ++k;
  while (k < n && isSpace(s[k])) ++k;
  if (k >= n || s[k] != ')') return false;
  ++k;
  while (k < n && isSpace(s[k])) ++k;
  if (k != n) return false;
  if (fn != nullptr) fn->assign(s, 0, i);
  return true;
}

// The next match, starting the search at `from`, of
// std::regex(R"(\s+if(?:\s+|(?=\()))", std::regex::icase) -- the successive
// matches std::sregex_iterator visits are the chain from = 0, then from = the
// end of the previous match (no match is empty).
inline bool nextSplitIf(const std::string& s, size_t from, size_t* pos, size_t* len) {
  const size_t n = s.size();
  size_t q = from;
  while (q < n) {
    if (!isSpace(s[q])) {
      ++q;
      continue;
    }
    size_t r = q;
    while (r < n && isSpace(s[r])) ++r;
    // Every start inside this run reaches the same `if`, so the run is
    // decided once.
    if (r + 2 <= n && foldAscii(s[r]) == 'i' && foldAscii(s[r + 1]) == 'f') {
      size_t e = r + 2;
      if (e < n && isSpace(s[e])) {
        while (e < n && isSpace(s[e])) ++e;
        *pos = q;
        *len = e - q;
        return true;
      }
      if (e < n && s[e] == '(') {
        *pos = q;
        *len = e - q;
        return true;
      }
    }
    q = r;
  }
  return false;
}

// One match of
//   R"((^|[^\w.$`\x80-\xff])([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\[([^\[\],]+)\])"
// (a variable subscript `name[exp]`), as std::sregex_iterator steps through the
// matches: the search starts at `from`, and `^` can match only when that is 0.
// The positions are of groups 2 (name) and 3 (index); `end` is one past `]`.
struct VarSubscriptMatch {
  size_t start = 0, namePos = 0, nameLen = 0, idxPos = 0, idxLen = 0, end = 0;
};
inline bool isHighByte(char c) { return static_cast<unsigned char>(c) >= 0x80; }
inline bool nextVarSubscript(const std::string& s, size_t from, VarSubscriptMatch* m) {
  const size_t n = s.size();
  auto nameAt = [&](size_t q, size_t start) {
    if (q >= n) return false;
    const char c0 = s[q];
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_' || isHighByte(c0)))
      return false;
    size_t r = q + 1;
    while (r < n && (isWord(s[r]) || isHighByte(s[r]))) ++r;
    size_t k = r;
    while (k < n && isSpace(s[k])) ++k;
    if (k >= n || s[k] != '[') return false;
    size_t j = k + 1;
    while (j < n && s[j] != '[' && s[j] != ']' && s[j] != ',') ++j;
    if (j == k + 1 || j >= n || s[j] != ']') return false;
    m->start = start;
    m->namePos = q;
    m->nameLen = r - q;
    m->idxPos = k + 1;
    m->idxLen = j - (k + 1);
    m->end = j + 1;
    return true;
  };
  for (size_t p = from; p < n; ++p) {
    if (p == 0 && nameAt(0, 0)) return true;  // `^`, tried first
    const char c = s[p];
    const bool separator = !(isWord(c) || c == '.' || c == '$' || c == '`' || isHighByte(c));
    if (separator && nameAt(p + 1, p)) return true;
  }
  return false;
}

// regex_match(s, R"(^\s*(-?\d+)\s*$)"): the digits (with their sign) are group 1.
inline bool matchSignedInt(const std::string& s, std::string* digits) {
  size_t a = 0, b = s.size();
  while (a < b && isSpace(s[a])) ++a;
  while (b > a && isSpace(s[b - 1])) --b;
  size_t k = a;
  if (k < b && s[k] == '-') ++k;
  if (k >= b) return false;
  for (size_t i = k; i < b; ++i)
    if (s[i] < '0' || s[i] > '9') return false;
  if (digits != nullptr) digits->assign(s, a, b - a);
  return true;
}

}  // namespace piet::rx

#endif  // PIET_NATIVE_REGEX_SCANS_H
