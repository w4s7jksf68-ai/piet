// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "macro_text_internal.h"
#include "macro_scanners_internal.h"

namespace piet::eval::macro_detail {
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
}
