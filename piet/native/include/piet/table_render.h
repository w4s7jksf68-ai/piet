// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Pure table renderers: prepared table + output style -> bytes.
// Model construction, numeric formatting, session state and file IO stay with
// the calling command. These functions neither read nor modify those states.
#ifndef PIET_NATIVE_TABLE_RENDER_H
#define PIET_NATIVE_TABLE_RENDER_H

#include <cstddef>
#include <optional>
#include <string>

#include "piet/table_model.h"

namespace piet::tables {

struct TexStyle {
  bool booktabs = false;   // \toprule/\midrule/\bottomrule instead of \hline
  bool fragment = false;   // rows only -- no tabular environment at all
  bool doubleTop = false;  // estout style(tex): \hline\hline above the header
  bool escape = true;
  bool standalone = false;  // wrap in \documentclass ... \begin{document}
  // The column specification is NOT cosmetic here: the plan's Stata-semantics
  // section fixes a different spelling per command (estout writes
  // `l*{k}{c}`, outreg2 writes `l c c`), and these strings are what the frozen
  // .tex goldens compare against.
  enum Align { kPlain, kStar, kSpaced };
  Align align = kPlain;
  // W1a-5c: the LETTER inside `l*{k}{c}`. esttab's `alignment()`/`align()`
  // picks it -- `alignment(S)` writes `l*{2}{S}` (w1b_esttab_resid.log y12),
  // and `alignment(c)` is byte-identical to the control because `c` is already
  // the default (y11). Anything else is the caller's own LaTeX column type
  // (siunitx's `S`, `D{.}{.}{-1}` from dcolumn) and is written through
  // unchanged rather than validated against a list this build cannot know.
  std::string alignLetter = "c";
};

// estout's four SEAM hooks and its row prefix. On esttab these four REPLACE a
// block of the table (BUG-500); on estout there is no block to replace -- a tab
// table has no rules -- and they are purely ADDITIVE lines at the seam
// (tests/eval/golden/aer_w1/w1b_estout_opts.log qb..qe). `prefoot` emits even
// when there are no stats rows, at the position the footer would have started
// (w1b_estout_opts2.log y9), and `begin(X)` prefixes EVERY row with no separator
// (qf: `BEGIN\te1`, `BEGINmpg\t-49.51222`).
struct EstoutHooks {
  std::optional<std::string> prehead, posthead, prefoot, postfoot;
  std::string begin;
};

std::string renderTex(const Table& t, const TexStyle& st);
std::string renderFixed(const Table& t, const EstoutHooks& hk = {});
std::string renderEstoutTex(const Table& t, size_t stubW, size_t cellW,
                          size_t starW, const EstoutHooks& hk = {});
std::string renderDelim(const Table& t, char delim, const EstoutHooks& hk = {});

}  // namespace piet::tables

#endif  // PIET_NATIVE_TABLE_RENDER_H
