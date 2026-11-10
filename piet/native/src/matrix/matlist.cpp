// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/results.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path, cecon::native_exists
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // stataExtendedMissingKind (BUG-1511); stataMissingToken for a `.a` cell (BUG-1399)
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"
#include "piet/mata_subset.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/storage_type.h"
#include "piet/stata_format.h"
#include "piet/strutil.h"
#include "matrix_command_errors.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace matrix_command_detail;

namespace {

// ---------------------------------------------------------------------------
// Option vocabularies -- NAMESPACE SCOPE, not function-local statics.
//
// The registry is consulted by the DISPATCHER (canonicalizeForCommand, before
// the handler runs). A function-local static would only be constructed the
// first time the handler is entered, i.e. AFTER the one call that needed it, so
// the very first `svmat A, name(col)` in a session would canonicalize against
// an empty registry and land in the handler still spelled "name". Namespace
// scope registers at static-init time, which is what the dispatcher's ordering
// requires. registerOptVocab is documented safe there (function-local static
// registry, no cross-TU init order).

// matlist's declared-but-unimplemented set is large on purpose: every one of
// these changes the RENDERING, so accepting one silently would print a table
// that is not the table the user asked for.
const eval::OptVocab& kMatlistVocab = eval::registerOptVocab(
    {"matlist"},
    {{"format", "title", "names", "nonames", "noblank", "twidth"},
     {"cspec", "rspec", "lines", "border", "left", "right", "rightindent", "showcoleq",
      "roweqonly", "coleqonly", "aligncolnames", "nohalf", "nodotz", "underscore", "linesize"}});

// Widths count CHARACTERS (StataBE pads a Unicode name by characters:
// scratch-r150/uni-v/p4.do), so a multi-byte label stays aligned.
std::string rjust(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= w ? s : std::string(w - n, ' ') + s;
}

}  // namespace

// ---------------------------------------------------------------------------
// matlist

namespace {

// The display layer's own spec grammar (stata_format.h documents why each layer
// keeps its own regex and shares only the renderer): a FULL match, so `%td`,
// `%9s` and anything else non-numeric is refused by name instead of being
// handed to a numeric renderer that would answer something plausible for it.
bool parseNumericFmt(const std::string& raw, fmt::StataFmtSpec* sp) {
  static const std::regex re(R"(^%(-)?(0)?([0-9]+)?(?:\.([0-9]+))?([fgeFGE])$)");
  std::smatch m;
  if (!std::regex_match(raw, m, re)) return false;
  // A four-digit cap before std::stoi, not after: stoi THROWS on
  // `format(%99999999999.2f)`, and that exception has no catcher between here
  // and the dispatcher, so an unbounded width is a crash from user input rather
  // than a diagnostic. No Stata display format is wider than four digits.
  if (m[3].str().size() > 4 || m[4].str().size() > 4) return false;
  sp->leftJustify = m[1].matched;
  sp->zeroFill = m[2].matched;
  sp->hasWidth = m[3].matched;
  sp->width = m[3].matched ? std::stoi(m[3].str()) : 0;
  sp->prec = m[4].matched ? std::stoi(m[4].str()) : 0;
  sp->conv = static_cast<char>(std::tolower(static_cast<unsigned char>(m[5].str()[0])));
  return true;
}

std::string renderCell(double v, const fmt::StataFmtSpec& sp, size_t width) {
  std::string s;
  if (auto miss = eval::stataMissingToken(v)) {
    // Missing is its token at every width (`.`, or `.a` for a matrix cell that
    // holds one, BUG-1399) -- renderValue takes a finite double and has no
    // missing concept, so the check cannot be pushed down into it.
    s = *miss;
  } else if (std::optional<std::string> r = fmt::renderValue(v, sp)) {
    s = *r;
  } else {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    s = buf;
  }
  // padToField rather than a bare right-justify: right-justified IS its default,
  // so the ordinary `%9.0g` column comes out exactly as specified, and the one
  // documented exception (the `-` flag) is honoured instead of dropped. The net
  // ACCEPTS `format(%-9.2f)`, so ignoring the flag would be a silently different
  // table. A widthless spec has nothing for it to pad to, hence the column's own
  // width there.
  return sp.hasWidth ? fmt::padToField(s, sp) : rjust(s, width);
}

}  // namespace

ojson runMatlist(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kMatlistVocab.known,
                                            kMatlistVocab.unimplemented)) {
    return ojson(*err);
  }

  // EXP comes off the RAW line, not off parsed.varlist: the expression may be
  // `e(b)`, whose parentheses the varlist tokenizer has no reason to keep in one
  // piece, and re-joining split tokens would be a guess about how they were
  // split. findUnquotedComma is the same option-boundary scan the tokenizer
  // itself uses, so `format(%9.3f)`'s inner text can never be mistaken for the
  // boundary.
  std::string rest = strutil::trim(parsed.raw);
  const size_t sp0 = rest.find_first_of(" \t");
  rest = (sp0 == std::string::npos) ? std::string() : strutil::trim(rest.substr(sp0));
  const long long comma = eval::findUnquotedComma(rest);
  if (comma >= 0) rest = rest.substr(0, static_cast<size_t>(comma));
  const std::string expr = strutil::trim(rest);
  if (expr.empty()) return aerError("matlist: no matrix specified");

  eval::EvalMatrix owned;
  const eval::EvalMatrix* mat = nullptr;
  // e(<name>) / r(<name>), not just e(b)/e(V). The old spelling was `([bV])`,
  // which was right while b and V were the only e-matrices anything posted;
  // pca posts seven (Ev, L, C, Psi, means, sds, components) and sktest posts
  // r(table), and under the old regex every one of them was unreachable -- the
  // command would publish a matrix that no syntax could name.
  // Parsed by hand, not a regex: the name may be Unicode (bytes >= 0x80 are
  // identifier bytes, core/ident.h), which a narrow-char bracket range cannot
  // express portably.
  char cls = 0;
  std::string name;
  {
    size_t p = 0;
    if (expr.size() > 2 && (expr[0] == 'e' || expr[0] == 'r') && expr[1] == '(') {
      p = 2;
      while (p < expr.size() && std::isspace(static_cast<unsigned char>(expr[p]))) ++p;
      const size_t b = p;
      if (p < expr.size() && cecon::ident::isIdentStartByte(expr[p])) {
        while (p < expr.size() && cecon::ident::isIdentByte(expr[p])) ++p;
        name = expr.substr(b, p - b);
        while (p < expr.size() && std::isspace(static_cast<unsigned char>(expr[p]))) ++p;
        if (p + 1 == expr.size() && expr[p] == ')') cls = expr[0];
        else name.clear();
      }
    }
  }
  if (cls != 0) {
    if (cls == 'e') {
      // eb-order T-P1: the one resolver `matrix N = e(NAME)` uses
      // (EvalSession::resolveEMatrix), so `matlist e(b)` and the copied matrix
      // cannot disagree. Its second copy of the coeftable synthesis (with a
      // diagonal e(V)) is gone. A refusal carries the resolver's rc: r(301)
      // with no e() at all (w2g_ebmat.log:2238 e15c_fresh_matlist_eb), r(111)
      // for a name the live e() does not have -- never the coarse rc 1.
      int rc = 111;
      std::string why;
      std::optional<eval::EvalMatrix> resolved = session.eval.resolveEMatrix(name, &rc, &why);
      // A live e() with no e(b)/e(V): rc 0, nothing to list (w2g_ebmat.log:2096
      // e15c_matlist_eb after pca; w2v_estpost_ematrix.log ep13_matlist_eb).
      if (!resolved.has_value() && session.eval.eMatrixAbsentFromLiveE(name))
        resolved = eval::EvalMatrix{};
      if (!resolved.has_value()) return aerErrorRc("matlist: " + why, rc);
      owned = std::move(*resolved);
      mat = &owned;
    } else if (const eval::EvalMatrix* posted = session.eval.findClassMatrix(cls, name)) {
      mat = posted;
    } else {
      return aerErrorRc("matlist: matrix " + expr + " not found", 111);
    }
  } else {
    mat = session.eval.findMatrix(expr);
    if (mat == nullptr) return aerErrorRc("matlist: matrix " + expr + " not found", 111);
  }

  std::string fmtRaw = "%9.0g";
  std::string title;
  std::string namesArg = "all";
  bool nonames = false;
  bool noblank = false;
  int twidth = 12;
  for (const auto& kv : parsed.opts) {
    const std::string& k = kv.first;
    const eval::OptValue& v = kv.second;
    if (k == "format") {
      if (v.isBool || v.strValue.empty()) return aerError("matlist: option format() requires a %fmt");
      fmtRaw = strutil::trim(v.strValue);
    } else if (k == "title") {
      if (v.isBool) return aerError("matlist: option title() requires a string");
      title = strutil::trim(v.strValue);
      if (title.size() >= 2 && title.front() == '"' && title.back() == '"') {
        title = title.substr(1, title.size() - 2);
      }
    } else if (k == "names") {
      if (v.isBool || v.strValue.empty()) {
        return aerError("matlist: option names() requires all, rows, columns or none");
      }
      namesArg = strutil::lower(strutil::trim(v.strValue));
    } else if (k == "nonames") {
      nonames = true;
    } else if (k == "noblank") {
      noblank = true;
    } else if (k == "twidth") {
      if (v.isBool) return aerError("matlist: option twidth() requires a number");
      try {
        twidth = std::stoi(strutil::trim(v.strValue));
      } catch (const std::exception&) {
        return aerError("matlist: option twidth() requires a number");
      }
      if (twidth < 1 || twidth > 200) {
        return aerError("matlist: twidth(" + strutil::trim(v.strValue) +
                        ") is out of range; twidth() must be between 1 and 200");
      }
    }
  }

  if (namesArg != "all" && namesArg != "rows" && namesArg != "columns" && namesArg != "none") {
    return aerError("matlist: names(" + namesArg + ") is not one of all, rows, columns, none",
                    "syntax_error");
  }
  fmt::StataFmtSpec spec;
  if (!parseNumericFmt(fmtRaw, &spec)) {
    // A `%td`/`%9s` here is not a rendering nicety: matlist renders NUMBERS, and
    // silently falling back to the default would print a different table from
    // the one that was asked for, with no sign that the format was dropped.
    return aerError("matlist: format(" + fmtRaw +
                        ") is not a numeric display format (%w.Pf, %w.Pg or %w.Pe)",
                    "syntax_error");
  }
  const size_t cellWidth = spec.hasWidth && spec.width > 0 ? static_cast<size_t>(spec.width) : 9;

  const bool showRows = !nonames && (namesArg == "all" || namesArg == "rows");
  const bool showCols = !nonames && (namesArg == "all" || namesArg == "columns");

  const size_t nrows = mat->data.size();
  const size_t ncols = nrows > 0 ? mat->data[0].size() : 0;
  // A 0x0 matrix (`matrix b = e(b)` after estpost/pca, or e(b) itself there) lists
  // nothing at rc 0 (w2g_ebmat.log:2096 e15c_matlist_eb; w2v_estpost_ematrix.log
  // ep13_matlist_eb, ep15c_matlist_eb) -- it used to be an rc-1 error.
  if (nrows == 0 || ncols == 0) {
    ojson empty;
    empty["type"] = "echo";
    empty["text"] = "";
    return empty;
  }

  // StataBE 19.5's default layout (BUG-1412; 2026-11-03 oracle, line for line):
  //
  //                | e1                   | e2
  //                |         x          y |         z
  //   -------------+----------------------+----------
  //   q            |                      |
  //              a |         1          2 |         3
  //   -------------+----------------------+----------
  //   w            |                      |
  //              b |         4          5 |         6
  //
  // - A column is one blank, the cell (the format's width w, 9 for %9.0g), one
  //   blank; the first has no leading blank without row names. The row-name
  //   field is twidth() wide plus one blank, then `|`.
  // - A column-equation change puts a `|` before the column, on every line.
  //   With any equation, a header line carries each group's equation once,
  //   left-aligned in the first column's w; with any row equation, each run
  //   of rows with one equation opens with a line holding its equation (blank
  //   for none) and a separator line comes before every run but the first.
  // - A factor/operator name puts its `op.` (or the part up to the last `#`)
  //   on a line above, right-aligned to end one place past the cell, `bn`
  //   dropped. Names are cut as abbrev() cuts them, to w (columns, equations)
  //   or twidth (rows).
  // - Separator lines and the blank stretch of an equation line stop one
  //   place short of the last column's right edge.
  // - A symmetric matrix prints its lower triangle.
  const auto abbrev = [](const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    return n > 4 ? s.substr(0, n - 2) + "~" + s.back() : s.substr(0, n);
  };
  const auto splitEq = [](std::string full, std::string* eq) {
    const size_t colon = full.find(':');
    eq->clear();
    if (colon != std::string::npos) {
      *eq = full.substr(0, colon);
      full = full.substr(colon + 1);
    }
    if (*eq == "_") eq->clear();  // Stata's name for "no equation" among named ones
    return full;
  };
  const size_t tw = static_cast<size_t>(twidth);
  const size_t w = cellWidth;
  std::vector<std::string> req(nrows), rname(nrows), ceq(ncols), cname(ncols), cop(ncols);
  bool anyRowEq = false, anyColEq = false, anyOp = false;
  for (size_t i = 0; i < nrows; ++i) {
    std::string full = i < mat->rownames.size() ? matrixNameAsListed(mat->rownames[i]) : "";
    if (full.empty()) full = "r" + std::to_string(i + 1);
    rname[i] = abbrev(splitEq(full, &req[i]), tw);
    anyRowEq = anyRowEq || !req[i].empty();
  }
  for (size_t j = 0; j < ncols; ++j) {
    std::string full = j < mat->colnames.size() ? matrixNameAsListed(mat->colnames[j]) : "";
    if (full.empty()) full = "c" + std::to_string(j + 1);
    std::string name = splitEq(full, &ceq[j]);
    anyColEq = anyColEq || !ceq[j].empty();
    size_t cut = name.rfind('#');
    if (cut == std::string::npos) cut = name.rfind('.');
    if (cut != std::string::npos && cut > 0 && cut + 1 < name.size()) {
      cop[j] = abbrev(name.substr(0, cut), w) + name[cut];
      name = name.substr(cut + 1);
      anyOp = true;
    }
    cname[j] = abbrev(name, w);
  }
  bool lowerOnly = nrows == ncols;
  for (size_t i = 0; i < nrows && lowerOnly; ++i) {
    for (size_t j = 0; j < i && lowerOnly; ++j) {
      const double a = mat->data[i][j], b = mat->data[j][i];
      lowerOnly = a == b || (std::isnan(a) && std::isnan(b));
    }
  }

  const auto lead = [&](size_t j) -> size_t { return j == 0 && !showRows ? 0 : 1; };
  const auto full = [&](size_t j) { return lead(j) + w + 1; };
  const auto fill = [&](size_t j) { return full(j) - (j + 1 == ncols ? 1 : 0); };
  const auto bar = [&](size_t j) {
    return j + 1 < ncols && anyColEq && ceq[j + 1] != ceq[j] ? "|" : "";
  };
  const auto ljust = [](const std::string& s, size_t width) {
    return s.size() >= width ? s : s + std::string(width - s.size(), ' ');
  };
  const std::string headLead = showRows ? std::string(tw + 1, ' ') + "|" : std::string();
  std::ostringstream out;
  const auto separator = [&] {
    if (showRows) out << std::string(tw + 1, '-') << "+";
    for (size_t j = 0; j < ncols; ++j) out << std::string(fill(j), '-') << (*bar(j) ? "+" : "");
    out << "\n";
  };

  if (!noblank) out << "\n";
  if (!title.empty()) out << title << "\n\n";
  if (showCols && anyColEq) {
    out << headLead;
    for (size_t j = 0; j < ncols; ++j) {
      if (j == 0 || ceq[j] != ceq[j - 1])
        out << std::string(lead(j), ' ') << ljust(abbrev(ceq[j], w), w) << ' ';
      else
        out << std::string(fill(j), ' ');
      out << bar(j);
    }
    out << "\n";
  }
  if (showCols && anyOp) {
    out << headLead;
    for (size_t j = 0; j < ncols; ++j) {
      if (cop[j].empty()) out << std::string(full(j), ' ');
      else out << std::string(lead(j), ' ') << rjust(cop[j], w + 1);
      out << bar(j);
    }
    out << "\n";
  }
  if (showCols) {
    out << headLead;
    for (size_t j = 0; j < ncols; ++j)
      out << std::string(lead(j), ' ') << rjust(cname[j], w) << ' ' << bar(j);
    out << "\n";
  }
  for (size_t i = 0; i < nrows; ++i) {
    const bool newGroup = anyRowEq && (i == 0 || req[i] != req[i - 1]);
    if (i == 0 ? showCols : newGroup) separator();
    if (newGroup && showRows) {
      out << ljust(abbrev(req[i], tw), tw) << " |";
      for (size_t j = 0; j < ncols; ++j) out << std::string(fill(j), ' ') << bar(j);
      out << "\n";
    }
    if (showRows) out << rjust(rname[i], tw) << " |";
    for (size_t j = 0; j < ncols; ++j) {
      if (lowerOnly && j > i) {
        out << std::string(full(j), ' ');
      } else {
        const double v = j < mat->data[i].size() ? mat->data[i][j]
                                                 : std::numeric_limits<double>::quiet_NaN();
        out << std::string(lead(j), ' ') << renderCell(v, spec, w) << ' ';
      }
      out << bar(j);
    }
    out << "\n";
  }

  ojson block;
  block["type"] = "display";  // output, not the command echo (BUG-1505, runReturnList)
  block["text"] = out.str();
  return block;
}

}  // namespace piet::handlers
