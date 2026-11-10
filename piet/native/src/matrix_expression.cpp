// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Matrix expression.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // BUG-540: stataMissingWithKind — `.a`-`.z` identity in matrix literals
#include "piet/eval/macros.h"
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/program.h"  // BUG-922: program::restorePreservesDeeperThan is defined here
#include "piet/serset.h"  // BUG-1461: serset::storageType, signed into `datasignature`
#include "piet/strmutate.h"  // BUG-532: needsStringCount, the string-condition test under by:
#include "piet/stata_format.h"  // eb-order T-P1: `matrix list ..., format()`
#include "piet/strutil.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path / env_utf8 (BUG-050/051)
#include "data/missing.h"   // Stata `.` / `.a`-`.z` kinds + stata_cmp (shared with the engine)
#include "core/stata_arith.h"  // `^` with StataBE's bits (frame.cpp's)

#if defined(__APPLE__)
#include <sys/clonefile.h>  // Q1 (gate ③): APFS copy-on-write snapshot for preserve
#endif  // BUG-1469: the vocabularies below sat inside it, undeclared off macOS

#include "command_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace command_detail {}
using namespace command_detail;
using namespace session_vocab;

namespace command_detail {

// Forward declarations for parseMatrixLiteral: the depth-aware top-level
// splitter (defined below with the B3(c) grammar), the one scalar-cell
// evaluator (defined below, next to evalMatrixRhs) and its unary-plus check.
// `lhsSubscript` marks the element-assignment target's subscript (`matrix
// M[i,j] = ...`), the one scalar expression of the sublanguage StataBE reads
// with its general evaluator: a dataset variable there is its first
// observation (`matrix M[x1,1] = 3` is rc 0, 2026-11-03 oracle).
bool splitTopLevel(const std::string& e, std::vector<std::string>& parts,
                   std::vector<char>& ops);
bool evalMatrixScalarExpr(const std::string& expr, SessionState& session, double& outVal,
                          std::string& err, bool lhsSubscript);
std::string mxPlusRefusal(const std::string& e);

// True when the bracket at `open` ('(' or '[') is closed by the LAST character
// of `t`, counting both bracket kinds on one depth (as splitTopLevel does).
bool closingBracketIsLast(const std::string& t, size_t open) {
  if (open == std::string::npos || open >= t.size()) return false;
  int depth = 0;
  for (size_t i = open; i < t.size(); ++i) {
    const char c = t[i];
    if (c == '(' || c == '[') ++depth;
    else if (c == ')' || c == ']') {
      if (--depth == 0) return i + 1 == t.size();
    }
  }
  return false;
}

// The first BARE identifier in `expr` that is a key of session.eval.matrices,
// or "" when there is none. A cell or term that mentions a stored matrix this
// way is matrix arithmetic, not the scalar evaluator's (BUG-584). Not bare,
// and so skipped: a call head `f(`, a subscript head `M[` and everything inside
// its brackets (`M[i,j]`, `_b[x]`, scalar context), the first argument of the
// functions whose argument is itself a name -- el/colsof/rowsof/colnumb
// (substituteMatrixRefs resolves them), scalar() (the scalar by definition) and
// the class wrappers e()/r()/c()/s() -- plus string literals, numbers (`1e5`)
// and extended missing values (`.a`).
std::string firstBareMatrixName(const std::string& expr, const SessionState& session) {
  // core/ident.h: a byte >= 0x80 is a name byte, so a Unicode matrix name is one name.
  auto isIdStart = [](char c) { return cecon::ident::isIdentStartByte(c); };
  auto isIdChar = [](char c) { return cecon::ident::isIdentByte(c); };
  auto skipSpace = [&expr](size_t k) {
    while (k < expr.size() && (expr[k] == ' ' || expr[k] == '\t')) ++k;
    return k;
  };
  size_t i = 0;
  while (i < expr.size()) {
    const char c = expr[i];
    if (c == '"') {
      const size_t close = expr.find('"', i + 1);
      i = close == std::string::npos ? expr.size() : close + 1;
      continue;
    }
    if (c == '[') {
      int depth = 0;
      for (; i < expr.size(); ++i) {
        if (expr[i] == '[') ++depth;
        else if (expr[i] == ']' && --depth == 0) break;
      }
      ++i;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) != 0 || (c == '.' && i + 1 < expr.size() &&
                                                             !isIdStart(expr[i + 1]))) {
      while (i < expr.size() && (std::isdigit(static_cast<unsigned char>(expr[i])) != 0 ||
                                 expr[i] == '.'))
        ++i;
      if (i < expr.size() && (expr[i] == 'e' || expr[i] == 'E')) {
        ++i;
        if (i < expr.size() && (expr[i] == '+' || expr[i] == '-')) ++i;
      }
      while (i < expr.size() && isIdChar(expr[i])) ++i;
      continue;
    }
    if (c == '.') {  // `.a`-`.z`
      ++i;
      while (i < expr.size() && isIdChar(expr[i])) ++i;
      continue;
    }
    if (!isIdStart(c)) {
      ++i;
      continue;
    }
    const size_t start = i;
    while (i < expr.size() && isIdChar(expr[i])) ++i;
    const std::string id = expr.substr(start, i - start);
    const size_t next = skipSpace(i);
    if (next < expr.size() && expr[next] == '(') {
      std::string lower = id;
      for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      static const char* const kNameArgFns[] = {"el",     "colsof", "rowsof", "colnumb", "scalar",
                                                "e",      "r",      "c",      "s"};
      bool nameArg = false;
      for (const char* f : kNameArgFns) nameArg = nameArg || lower == f;
      i = next + 1;
      if (nameArg) {
        i = skipSpace(i);
        while (i < expr.size() && isIdChar(expr[i])) ++i;
      }
      continue;
    }
    if (next < expr.size() && expr[next] == '[') continue;  // subscript head
    for (const auto& p : session.eval.matrices)
      if (p.first == id) return id;
  }
  return std::string();
}

// Splits `(1,2\3,4)`-style literals: '\' separates rows, ',' separates
// columns. nullopt on anything malformed (ragged rows, empty cells, an
// unbalanced group) or on a cell that belongs to the matrix grammar -- the
// caller (evalMxPrimary) then evaluates the group with that grammar, which
// answers it or refuses it with Stata's rc (Doctrine 1). The split is
// depth-aware (splitTopLevel), so a cell's own comma -- `round(1/3,.01)`,
// `el(M,2,3)` -- is never read as a column separator.
//
// Cell order, first match wins (oracle w2c_functions.log:160
// matrix_mixed_literal, w2f_residuals.log:1303 round_matrix_row):
//   1. `.` and `.a`-`.z`;
//   2. a plain signed decimal;
//   3. a constant arithmetic expression (no name, no function);
//   4. decline: a nested literal, a cell that mentions a matrix name as a bare
//      identifier (`Bcol'`, `2*Bcol`; firstBareMatrixName), a matrix subscript
//      NAME[...], a matrix-valued call I()/J()/vecdiag()/nullmat(), or an
//      e()/r() class MATRIX -- these keep their names and shapes through the
//      matrix grammar (or are refused there) instead of collapsing to a scalar;
//   5. any other cell is a scalar expression, evaluated by evalMatrixScalarExpr
//      (the macro/arith layer, the same as `scalar`/`local`/`display`).
std::optional<std::vector<std::vector<double>>> parseMatrixLiteral(const std::string& body,
                                                                    SessionState& session) {
  std::string s = trim(body);
  if (s.size() < 2 || s.front() != '(' || s.back() != ')') return std::nullopt;
  s = s.substr(1, s.size() - 2);
  std::vector<std::string> parts;
  std::vector<char> ops;
  if (!splitTopLevel(s, parts, ops)) return std::nullopt;  // unbalanced group

  auto namesMatrix = [&session](const std::string& n) {
    for (const auto& p : session.eval.matrices)
      if (p.first == n) return true;
    return false;
  };
  auto cellValue = [&](const std::string& t, double& out) -> bool {
    // BUG-540 (oracle arm lit_dot): a bare `.` cell is plain system missing --
    // same NaN evalJCall's `J(r,c,.)` fill already uses, so formatMatrixCell's
    // isnan() check and the existing NaN->"."->sentinel display round-trip
    // (matrixElementText, macros.cpp) render it identically to a J() cell.
    if (t == ".") {
      out = std::numeric_limits<double>::quiet_NaN();
      return true;
    }
    // BUG-540 (oracle arm lit_extmissing): `.a`-`.z` must stay DISTINCT from
    // plain `.` under `==` (E[1,2]==.a is true, E[1,2]==. is false in the
    // oracle) -- a NaN payload survives copies but isnan()-based rendering
    // collapses every NaN to the same "." text, which would lose the kind on
    // the way back through the expression layer. The eval layer's own `.a`
    // literal already resolves to the FINITE stataMissingWithKind() sentinel
    // (exprfns.h, BUG-249), so storing that same finite double here keeps
    // identity through matrixElementText's NaN-free pythonFloatRepr path.
    if (t.size() == 2 && t[0] == '.' && t[1] >= 'a' && t[1] <= 'z') {
      out = eval::stataMissingWithKind(t[1] - 'a' + 1);
      return true;
    }
    try {
      size_t pos = 0;
      const double v = std::stod(t, &pos);
      if (pos == t.size()) {
        out = v;
        return true;
      }
    } catch (const std::exception&) {
    }
    // ml-getmata T4 (oracle w2c_ml_getmata.do:244, `matrix rchange = (0.1*`i'
    // + 0.05)` inside family (8)'s loop, rc 0 in Stata): a cell may be a
    // CONSTANT arithmetic expression. Only digits, `.`, `e`, + - * / ^ and
    // parentheses qualify -- no name, no function -- so nothing that could
    // mean a matrix, a scalar or a variable is reinterpreted here. The value
    // comes from the existing macro/arith layer (eval::evalArith), not a new
    // evaluator. A `+` that is no number's sign (`1++2`) is declined first,
    // for the matrix grammar to refuse as StataBE does (mxPlusRefusal).
    static const std::regex reConstExpr(R"(^[0-9.eE+\-*/^() \t]+$)");
    if (!mxPlusRefusal(t).empty()) return false;
    if (t.find_first_of("+-*/^(") != std::string::npos && std::regex_match(t, reConstExpr)) {
      const std::optional<std::string> r = eval::evalArith(t, nullptr);
      if (!r.has_value()) return false;
      if (*r == ".") {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
      }
      try {
        size_t pos = 0;
        const double v = std::stod(*r, &pos);
        if (pos != r->size()) return false;
        out = v;
        return true;
      } catch (const std::exception&) {
        return false;
      }
    }
    // Decline (a): a nested literal `(1,2)` / `(S1 \ .)` -- the matrix grammar's.
    if (t.size() >= 2 && t.front() == '(' && t.back() == ')') {
      std::vector<std::string> inner;
      std::vector<char> innerOps;
      if (splitTopLevel(t.substr(1, t.size() - 2), inner, innerOps) && inner.size() > 1)
        return false;
    }
    // Decline (b): a cell that mentions a matrix NAME anywhere as a bare
    // identifier -- `Bcol`, `Bcol'`, `2*Bcol`, `-Bcol` (BUG-584): matrix
    // arithmetic, which the matrix grammar (evalMx) evaluates.
    if (!firstBareMatrixName(t, session).empty()) return false;
    // Decline (c): a subscript of a stored matrix, `M[1,1]` / `M[1..2,1]` /
    // `M["rb",1]`; the matrix grammar reads it (a range keeps the source names).
    // Depth-aware: `M[1,1]+M[2,2]` is a scalar expression, not a subscript.
    static const std::regex reSub(std::string("^(") + eval::kRxName + R"()\s*\[.*\]$)");
    std::smatch nm;
    if (std::regex_match(t, nm, reSub) && closingBracketIsLast(t, t.find('[')) &&
        namesMatrix(nm.str(1)))
      return false;
    // Decline (d): a matrix-valued call.
    static const std::regex reMatFn(R"(^(I|J|vecdiag|nullmat)\s*\()");
    if (std::regex_search(t, reMatFn)) return false;
    // Decline (e): an e()/r() class MATRIX. A class SCALAR (e(N), e(r2)) is
    // not declined: evalArith resolves it below.
    static const std::regex reClass(std::string(R"(^([er])\(\s*()") + eval::kRxName + R"()\s*\)$)");
    if (std::regex_match(t, nm, reClass)) {
      const char cls = nm.str(1)[0];
      const std::string which = nm.str(2);
      if (session.eval.findClassMatrix(cls, which) != nullptr ||
          (cls == 'e' && (which == "b" || which == "V")))
        return false;
    }
    std::string cellErr;
    return evalMatrixScalarExpr(t, session, out, cellErr);
  };

  std::vector<std::vector<double>> rows;
  std::vector<double> row;
  for (size_t i = 0; i < parts.size(); ++i) {
    const std::string t = trim(parts[i]);
    if (t.empty()) return std::nullopt;
    double v = 0.0;
    if (!cellValue(t, v)) return std::nullopt;
    row.push_back(v);
    if (i + 1 < parts.size() && ops[i] == '\\') {
      rows.push_back(std::move(row));
      row.clear();
    }
  }
  rows.push_back(std::move(row));
  const size_t ncols = rows[0].size();
  for (const auto& r : rows) {
    if (r.empty() || r.size() != ncols) return std::nullopt;  // ragged
  }
  return rows;
}

// Stata's default row/col stub ("r1".."rN" / "c1".."cN") — what an
// arithmetic result (a literal, or any `*` product) gets unless the caller
// overwrites it with an explicit `matrix rownames`/`colnames` (exactly what
// the P4 fixture does immediately after `matrix P4B = P4A'*P4A`).
void applyDefaultStripe(eval::EvalMatrix& mat) {
  const size_t nrows = mat.data.size();
  const size_t ncols = nrows > 0 ? mat.data[0].size() : 0;
  mat.rownames.clear();
  for (size_t i = 1; i <= nrows; ++i) mat.rownames.push_back("r" + std::to_string(i));
  mat.colnames.clear();
  for (size_t j = 1; j <= ncols; ++j) mat.colnames.push_back("c" + std::to_string(j));
}

eval::EvalMatrix transposeMatrix(const eval::EvalMatrix& in) {
  const size_t nrows = in.data.size();
  const size_t ncols = nrows > 0 ? in.data[0].size() : 0;
  eval::EvalMatrix out;
  out.data.assign(ncols, std::vector<double>(nrows, 0.0));
  for (size_t i = 0; i < nrows; ++i) {
    for (size_t j = 0; j < ncols; ++j) out.data[j][i] = in.data[i][j];
  }
  // Transpose swaps the stripe spaces along with the data.
  out.rownames = in.colnames;
  out.colnames = in.rownames;
  return out;
}

// AER scope 2 / B3(c): J() and nullmat() (J: 24 corpus uses, papers 015 and
// 049; nullmat: paper 015's `matrix b = nullmat(b), tmp` accumulator idiom),
// now two of the functions of the matrix expression grammar (evalMx below).

// Split on ',' / '\' at paren depth 0. Returns the pieces and their separators.
// BUG-542: `[' / ']' share the same depth counter as `(' / ')' -- a submatrix
// subscript like `M[1..2,1..3]` carries its OWN top-level comma (row spec vs
// col spec), which must not be mistaken for the join's own `,`/`\` separator
// when the subscript sits inside a larger expression (`(0,0,0) \ M[1..2,1..3]`).
// The two bracket kinds always nest correctly together in this grammar, so one
// counter is exact, not an approximation.
bool splitTopLevel(const std::string& e, std::vector<std::string>& parts,
                   std::vector<char>& ops) {
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i < e.size(); ++i) {
    const char c = e[i];
    if (c == '(' || c == '[') ++depth;
    else if (c == ')' || c == ']') { if (--depth < 0) return false; }
    else if (depth == 0 && (c == ',' || c == '\\')) {
      parts.push_back(trim(e.substr(start, i - start)));
      ops.push_back(c);
      start = i + 1;
    }
  }
  if (depth != 0) return false;
  parts.push_back(trim(e.substr(start)));
  return true;
}

// True when the whole string is one parenthesised group.
bool wrappedInParens(const std::string& e) {
  if (e.size() < 2 || e.front() != '(' || e.back() != ')') return false;
  int depth = 0;
  for (size_t i = 0; i < e.size(); ++i) {
    if (e[i] == '(') ++depth;
    else if (e[i] == ')') { if (--depth == 0) return i + 1 == e.size(); }
  }
  return false;
}

// Converts eval::evalArith's text result into the double an eval::EvalMatrix
// cell stores -- the inverse of matrixElementText (macros.cpp): "." is plain
// missing, `.a`-`.z` is the extended-missing kind (BUG-249's
// stataMissingWithKind), anything else a plain decimal.
bool matrixCellDoubleFromArithResult(const std::string& s, double& out) {
  if (s == ".") {
    out = std::numeric_limits<double>::quiet_NaN();
    return true;
  }
  if (s.size() == 2 && s[0] == '.' && s[1] >= 'a' && s[1] <= 'z') {
    out = eval::stataMissingWithKind(s[1] - 'a' + 1);
    return true;
  }
  try {
    size_t pos = 0;
    out = std::stod(s, &pos);
    return pos == s.size();
  } catch (const std::exception&) {
    return false;
  }
}

bool mxMissing(double v) { return std::isnan(v) || std::fabs(v) >= eval::kStataMissingSentinel; }

size_t mxCols(const eval::EvalMatrix& m) { return m.data.empty() ? 0 : m.data[0].size(); }

size_t mxClosing(const std::string& e, size_t open);

std::string mxSqueeze(const std::string& s) {
  std::string out;
  for (const char c : s)
    if (c != ' ' && c != '\t') out += c;
  return out;
}

// StataBE has no unary plus but a number's sign: `+2`, `A * +2` and `2^+1` are
// rc 0, while a `+` before anything else is read into the name after it (2026-
// 11-03 oracle; its general evaluator does the same, `+sc invalid name`):
// `+A`, `A * +sc`, `sqrt(+sc)` and `+.a` are r(111) `+A not found` and so on,
// `+(1,2)` and `+sqrt(4)` r(133) `unknown function +()` / `+sqrt()`. A binary
// `+` with no operand after it -- another `+`, the end, a closing bracket -- is
// read into the operand before it: `A + +B` and `1++2` are `A+ not found` /
// `1+ not found`, `+A + +B` is `+A+ not found`. The refusal for the first such
// `+` in `e` as an `r(NNN): ` error, or "" when there is none.
std::string mxPlusRefusal(const std::string& e) {
  auto isIdStart = [](char c) { return cecon::ident::isIdentStartByte(c); };
  auto isIdChar = [](char c) { return cecon::ident::isIdentByte(c); };
  auto isDigit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
  for (size_t i = 0; i < e.size(); ++i) {
    if (e[i] == '"') {
      const size_t close = e.find('"', i + 1);
      if (close == std::string::npos) break;
      i = close;
      continue;
    }
    if (e[i] != '+') continue;
    size_t q = i;  // one past the last character before the `+` that is not a blank
    while (q > 0 && (e[q - 1] == ' ' || e[q - 1] == '\t')) --q;
    size_t n = i + 1;  // the first one after it
    while (n < e.size() && (e[n] == ' ' || e[n] == '\t')) ++n;
    const char before = q > 0 ? e[q - 1] : '\0';
    const char after = n < e.size() ? e[n] : '\0';
    if (before != '\0' && (isIdChar(before) || before == '.' || before == ')' || before == ']' ||
                           before == '\'' || before == '"')) {
      if (after != '\0' && after != '+' && after != ')' && after != ']' && after != ',') continue;
      size_t s = q - 1;  // the operand before: a name or number, or a group with its head
      if (before == ')' || before == ']') {
        int depth = 0;
        for (size_t k = s + 1; k-- > 0;) {
          if (e[k] == ')' || e[k] == ']') {
            ++depth;
          } else if ((e[k] == '(' || e[k] == '[') && --depth == 0) {
            s = k;
            break;
          }
        }
      }
      while (s > 0 && (isIdChar(e[s - 1]) || e[s - 1] == '.')) --s;
      return "r(111): " + mxSqueeze(e.substr(s, q - s)) + "+ not found";
    }
    if (isDigit(after) || (after == '.' && n + 1 < e.size() && isDigit(e[n + 1]))) continue;
    if (after == '(') return "r(133): unknown function +()";
    if (after == '.' || isIdStart(after)) {
      size_t j = n + 1;
      while (j < e.size() && isIdChar(e[j])) ++j;
      const std::string id = e.substr(n, j - n);
      while (j < e.size() && (e[j] == ' ' || e[j] == '\t')) ++j;
      if (after != '.' && j < e.size() && e[j] == '(')
        return "r(133): unknown function +" + id + "()";
      return "r(111): +" + id + " not found";
    }
    return "r(111): + not found";
  }
  return std::string();
}

// A subscript body's two specs, split at its top-level comma (outside quotes
// and brackets), or StataBE's refusal of a malformed one (2026-11-03 oracle):
// `A[1]`, `A[1,1,1]` and `A[,1]` are r(111) `A not found`; an empty column
// spec is r(198), or r(111) `, not found` after a range (`A[1..2,]`).
std::optional<std::pair<std::string, std::string>> mxSubscriptSpecs(const std::string& name,
                                                                   const std::string& body,
                                                                   std::string& err) {
  std::vector<std::string> parts;
  int depth = 0;
  size_t from = 0;
  for (size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (c == '"') {
      const size_t close = body.find('"', i + 1);
      if (close == std::string::npos) break;
      i = close;
    } else if (c == '(' || c == '[') {
      ++depth;
    } else if (c == ')' || c == ']') {
      --depth;
    } else if (c == ',' && depth == 0) {
      parts.push_back(trim(body.substr(from, i - from)));
      from = i + 1;
    }
  }
  parts.push_back(trim(body.substr(from)));
  if (parts.size() != 2 || parts[0].empty()) {
    err = "r(111): " + name + " not found";
    return std::nullopt;
  }
  if (parts[1].empty()) {
    err = parts[0].find("..") != std::string::npos ? "r(111): , not found" : "r(198): invalid syntax";
    return std::nullopt;
  }
  return std::make_pair(parts[0], parts[1]);
}

// True when a subscript spec asks for a submatrix: a range (`1..2`, `2...`)
// or a row/column name (`"rb"`).
bool mxRangeSpec(const std::string& spec) {
  return spec.find("..") != std::string::npos || spec.find('"') != std::string::npos;
}

// One element of `m`, each index truncated toward zero; an index that is
// missing or outside `m` reads `.` (StataBE: `A[1.7,2]` is A[1,2], `A[4,1]`,
// `A[0,1]` and `el(A,.,1)` are `.`, rc 0).
double mxElementAt(const eval::EvalMatrix& m, double r, double c) {
  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  if (mxMissing(r) || mxMissing(c)) return kNaN;
  r = std::trunc(r);
  c = std::trunc(c);
  if (r < 1 || c < 1 || r > static_cast<double>(m.data.size()) ||
      c > static_cast<double>(mxCols(m)))
    return kNaN;
  return m.data[static_cast<size_t>(r) - 1][static_cast<size_t>(c) - 1];
}

// A cell's value as evalArith text: `(.)`, `(.a)`, or the number in parentheses.
std::string mxValueText(double v) {
  if (std::isnan(v)) return "(.)";
  if (const std::optional<int> kind = eval::stataExtendedMissingKind(v))
    return *kind == 0 ? std::string("(.)") : std::string("(.") + static_cast<char>('a' + *kind - 1) + ")";
  return "(" + eval::pythonFloatRepr(v) + ")";
}

// The matrix sublanguage's reading of the names in a scalar expression, before
// the macro/arith layer sees it (StataBE 19.5, 2026-11-03 oracle):
//  * a bare name is a scalar or a matrix, never a dataset variable. A name that
//    is only a variable (or abbreviates one) is r(111) `NAME not found`
//    (`J(x1,1,0)`, `sqrt(x1)`, `(1,x1)`), and a name that is both reads the
//    scalar (`z*A` with scalar z = 7 and variable z = 5 is 7*A), where the
//    arith layer's own rule reads the variable. `x1[2]` subscripts the
//    variable and stays legal.
//  * `M[i,j]` and `el(M,i,j)` of a stored matrix read one element
//    (mxElementAt), each index a scalar expression read the same way. A range
//    or name subscript there is a matrix where a scalar is required, r(109)
//    (`sqrt(A[1..2,1])`); a malformed one is mxSubscriptSpecs' refusal; and
//    `NAME[...]` of a name that is no matrix (nor a variable, with one index)
//    is r(111) `NAME not found`.
// Quoted text, `_b[...]`/`_se[...]` and an `[eq]` prefix are copied as they are.
// `out` is the text for evalArith; false with an `r(NNN): ` error.
bool mxBindOperandText(const std::string& in, SessionState& session, std::string& out,
                       std::string& err) {
  auto isIdStart = [](char c) { return cecon::ident::isIdentStartByte(c); };
  auto isIdChar = [](char c) { return cecon::ident::isIdentByte(c); };
  const auto isVariable = [&session](const std::string& n) {
    return session.eval.varObs && session.eval.varObs(n, 1).has_value();
  };
  const auto readIndex = [&session, &err](const std::string& t, double& v) {
    return evalMatrixScalarExpr(t, session, v, err);
  };
  out.clear();
  size_t i = 0;
  while (i < in.size()) {
    const char c = in[i];
    if (c == '"') {
      const size_t close = in.find('"', i + 1);
      const size_t end = close == std::string::npos ? in.size() : close + 1;
      out.append(in, i, end - i);
      i = end;
      continue;
    }
    if (c == '[') {
      // A subscript's own bracket (after a variable or `e(b)`) is read on; an
      // `[eq]` prefix is copied whole.
      size_t q = out.size();
      while (q > 0 && (out[q - 1] == ' ' || out[q - 1] == '\t')) --q;
      if (q > 0 && (isIdChar(out[q - 1]) || out[q - 1] == ')')) {
        out += c;
        ++i;
      } else {
        const size_t close = std::min(mxClosing(in, i), in.size() - 1);
        out.append(in, i, close + 1 - i);
        i = close + 1;
      }
      continue;
    }
    if (!isIdStart(c) || (i > 0 && (isIdChar(in[i - 1]) || in[i - 1] == '.'))) {
      out += c;
      ++i;
      continue;
    }
    size_t j = i;
    while (j < in.size() && isIdChar(in[j])) ++j;
    const std::string id = in.substr(i, j - i);
    size_t k = j;
    while (k < in.size() && (in[k] == ' ' || in[k] == '\t')) ++k;
    if (k < in.size() && in[k] == '(') {
      std::string lower = id;
      for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      const size_t close = mxClosing(in, k);
      if (lower == "el" && close < in.size()) {
        std::vector<std::string> args;
        std::vector<char> ops;
        if (splitTopLevel(in.substr(k + 1, close - k - 1), args, ops) && args.size() == 3 &&
            std::all_of(ops.begin(), ops.end(), [](char o) { return o == ','; })) {
          if (const eval::EvalMatrix* m = session.eval.findMatrix(args[0])) {
            double r = 0.0, col = 0.0;
            if (!readIndex(args[1], r) || !readIndex(args[2], col)) return false;
            out += mxValueText(mxElementAt(*m, r, col));
            i = close + 1;
            continue;
          }
        }
      }
      // A call's arguments are read on; the first argument of the functions
      // whose argument is a name is that name, not an expression.
      static const char* const kNameArgFns[] = {"el",     "colsof", "rowsof", "colnumb", "rownumb",
                                                "scalar", "e",      "r",      "c",       "s"};
      bool nameArg = false;
      for (const char* f : kNameArgFns) nameArg = nameArg || lower == f;
      out.append(in, i, k + 1 - i);
      i = k + 1;
      if (nameArg) {
        while (i < in.size() && (in[i] == ' ' || in[i] == '\t')) out += in[i++];
        while (i < in.size() && isIdChar(in[i])) out += in[i++];
      }
      continue;
    }
    if (k < in.size() && in[k] == '[') {
      const size_t close = mxClosing(in, k);
      if (close >= in.size()) {  // unbalanced: the arith layer's refusal
        out.append(in, i, std::string::npos);
        break;
      }
      const std::string body = in.substr(k + 1, close - k - 1);
      if (id == "_b" || id == "_se" || id == "_coef") {
        out.append(in, i, close + 1 - i);
        i = close + 1;
        continue;
      }
      if (const eval::EvalMatrix* m = session.eval.findMatrix(id)) {
        std::optional<std::pair<std::string, std::string>> specs;
        if (!trim(body).empty()) {
          specs = mxSubscriptSpecs(id, body, err);
          if (!specs.has_value()) return false;
        }
        if (!specs.has_value() || mxRangeSpec(specs->first) || mxRangeSpec(specs->second)) {
          err = "r(109): type mismatch";
          return false;
        }
        double r = 0.0, col = 0.0;
        if (!readIndex(specs->first, r) || !readIndex(specs->second, col)) return false;
        out += mxValueText(mxElementAt(*m, r, col));
        i = close + 1;
        continue;
      }
      std::vector<std::string> idx;
      std::vector<char> idxOps;
      if (!isVariable(id) || !splitTopLevel(body, idx, idxOps) || idx.size() != 1) {
        err = "r(111): " + id + " not found";
        return false;
      }
      out.append(in, i, k - i);  // `x1[2]`: its index is read on
      i = k;
      continue;
    }
    if (session.eval.scalars.find(id) != nullptr) {
      out += isVariable(id) ? "scalar(" + id + ")" : id;
    } else if (session.eval.findMatrix(id) == nullptr && isVariable(id)) {
      err = "r(111): " + id + " not found";
      return false;
    } else {
      out += id;
    }
    i = j;
  }
  return true;
}

// The ONE scalar-expression evaluator of the matrix sublanguage: literal cells
// (parseMatrixLiteral), J()'s and I()'s arguments, a scalar operand of the
// grammar (evalMx), subscripts, and element-assignment subscripts and right-
// hand sides (runMatrixElementAssign). Its names are read the sublanguage's
// way first (mxPlusRefusal, mxBindOperandText; an element-assignment target's
// subscript excepted), and then it is the macro/arith layer (eval::evalArith,
// which substitutes colsof()/rowsof(), e()/_b[] refs and scalars itself), so a
// function answers in a matrix cell exactly as it answers in `scalar`/`local`/
// `display`; there is no second parser here.
//
// Every failure carries an `r(NNN): ` prefix (read back by eval::splitErrCode):
// a string result is r(109) (as main's element path), a failure the shared
// classifier claims keeps its rc (an unknown function is r(133)), a bare
// unknown identifier -- the shape the classifier leaves unclaimed -- is r(111)
// "NAME not found", and anything else unclaimed is r(198).
bool evalMatrixScalarExpr(const std::string& exprIn, SessionState& session, double& outVal,
                          std::string& err, bool lhsSubscript) {
  // A plain run of digits is its own value, read before the arith layer, which
  // rejects a leading-zero integer literal (`01`, as `local j : display %02.0f`
  // writes it, or `I(03)`): Stata reads 01 as 1 (BUG-587 subscripts, BUG-586
  // I()). The arith layer's own leading-zero rule is not changed (FU-201).
  {
    const std::string digits = trim(exprIn);
    if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos) {
      try {
        outVal = std::stod(digits);
        return true;
      } catch (const std::exception&) {
        // out of double range: the arith layer answers below
      }
    }
  }
  std::string expr = exprIn;
  if (!lhsSubscript) {
    err = mxPlusRefusal(exprIn);
    if (!err.empty()) return false;
    if (!mxBindOperandText(exprIn, session, expr, err)) return false;
  }
  std::string raised;
  bool isString = false;
  const std::optional<std::string> result =
      eval::evalArith(expr, &session.eval, &raised, &isString);
  if (result.has_value() && isString) {
    err = "r(109): type mismatch";
    return false;
  }
  if (!result.has_value()) {
    // A NameError on a STORED MATRIX name is a matrix where a scalar is
    // required (`J(2,2,A)`, `sqrt(A)`): StataBE's r(109) "type mismatch", never
    // r(111) "M not found" (BUG-584).
    static const std::string kNameErr = "NameError: name '";
    std::string nameErrName;
    if (raised.rfind(kNameErr, 0) == 0) {
      const size_t end = raised.find('\'', kNameErr.size());
      if (end != std::string::npos)
        nameErrName = raised.substr(kNameErr.size(), end - kNameErr.size());
    }
    // A name only called (`A(1)`) is an unknown function, r(133), which the
    // classifier below answers.
    if (!nameErrName.empty() && !firstBareMatrixName(expr, session).empty()) {
      for (const auto& p : session.eval.matrices) {
        if (p.first == nameErrName) {
          err = "r(109): type mismatch";
          return false;
        }
      }
    }
    eval::MacroFnError mfe;
    if (!raised.empty() && eval::classifyFailedExpr(expr, raised, &mfe) && mfe.failed()) {
      err = "r(" + mfe.rc + "): " + mfe.message;
      return false;
    }
    if (!nameErrName.empty()) {
      err = "r(111): " + nameErrName + " not found";
      return false;
    }
    err = "r(198): invalid syntax";
    return false;
  }
  if (!matrixCellDoubleFromArithResult(*result, outVal)) {
    err = "r(109): type mismatch";
    return false;
  }
  return true;
}

// The error block for an evalMatrixScalarExpr or evalMx failure: the
// `r(NNN): ` prefix becomes the exact rc through engineErrorBlockRc (the
// errors.h builder, BUG-187), and the message is StataBE's bare sentence
// (`conformability error`, never `matrix X: conformability error`). A message
// without a prefix is r(198); this never yields rc 1.
ojson matrixExprErrorBlock(const std::string& err) {
  std::string msg = err;
  const std::string code = eval::splitErrCode(&msg);  // "r(NNN)"; r(111) when absent
  int rc = 198;
  if (err.rfind("r(", 0) == 0 && code.size() > 3) {
    try {
      rc = std::stoi(code.substr(2, code.size() - 3));
    } catch (const std::exception&) {
      rc = 198;
    }
  }
  return engineErrorBlockRc(msg, rc);
}

// The one resolver of a class-matrix reference `e(NAME)`/`r(NAME)` on the
// matrix verbs: runMatrixDef's `matrix X = e(NAME)` form, vecdiag()'s argument
// and an e()/r() term at a join position. An e() matrix is
// EvalSession::resolveEMatrix's (eb-order T-P1): a matrix POSTED for the live
// e() wins (`estat event, post`'s Pre_avg/Post_avg/Tm#/Tp# e(b)/e(V), pca's
// Ev/L/C/Psi, margins/glm postings), and otherwise e(b)/e(V) are the live fit's
// canonical ones -- `_cons` last, never `(intercept)`, and the engine's FULL
// covariance (w2g_ebmat.log:245 `x1 x2 _cons`, :365 V[1,2]). The diagonal-of-
// se^2 synthesis that stood here (FU-98) is gone: it answered every
// off-diagonal with a silent 0. An r() matrix is the posted store's.
// nullopt with `err` (and `*rcOut`: 301 with no e() at all, 111 for an absent
// name, the resolver's named refusal of an e(V) the engine did not send).
std::optional<eval::EvalMatrix> resolveClassMatrixRef(char cls, const std::string& which,
                                                       SessionState& session, std::string& err,
                                                       int* rcOut) {
  if (cls != 'e') {
    if (const eval::EvalMatrix* posted = session.eval.findClassMatrix(cls, which)) return *posted;
    err = std::string("matrix ") + cls + "(" + which + ") not found";
    if (rcOut != nullptr) *rcOut = 111;
    return std::nullopt;
  }
  int rc = 111;
  std::optional<eval::EvalMatrix> m = session.eval.resolveEMatrix(which, &rc, &err);
  if (!m.has_value() && rcOut != nullptr) *rcOut = rc;
  return m;
}

std::optional<eval::EvalMatrix> evalMatrixRhs(const std::string& rhs,
                                              SessionState& session, std::string& err);

// The matrix expression grammar of `matrix NAME = exp` and its friends ([P]
// matrix define, "Matrix operators"; BUG-1398). StataBE 19.5 (2026-10-29
// oracle) types every operand a SCALAR or a MATRIX and checks the types at
// each operator:
//   precedence, lowest first: `\` row join, `,` column join, `+`, binary `-`,
//   `*`, `/`, `#`, unary `-`, postfix `'`, then parentheses. Each binary level
//   is left-associative, so `a + b - c` is a + (b - c) and `A*3/0.3` is
//   A*(3/0.3) -- the scalar evaluator's own order.
//   A SCALAR is a number, `.`, or a scalar expression with no matrix in it
//   (`sqrt(4)`, `sc`, `y[2]`, `(2)`, and `A[1,1]`: two scalar subscripts read
//   one element). A MATRIX is a stored NAME, a group with its own `,`/`\` (a
//   literal), J()/I()/nullmat()/inv()/invsym()/syminv()/vecdiag(), a range or
//   name subscript (`A[1..2,1]`, `A["rb",1]`), or an e()/r() matrix.
//     `\` `,`  a scalar joins as a 1x1;
//     `+` `-`  two scalars, or two matrices of one shape (else r(503)); a
//              scalar takes part as a 1x1, so `S + 1` works but `A + 1` is
//              r(503);
//     `*`      a scalar scales the other operand; two matrices multiply when
//              they conform (a 1x1 O times a 1xn B too: O's row name), and
//              otherwise a 1x1 one scales the other (`B*O` keeps B's names)
//              or it is r(503);
//     `/`      the divisor must be a scalar (`A/S` is r(109) even for a 1x1
//              S), and 0 or missing is r(504) `matrix has missing values`;
//     `#`      two matrices (`2#A` is r(109));
//   and a matrix inside a scalar-only construct (`A^2`, `sqrt(A)`) is r(109).
// Names: `+`/`-` take the right operand's, `*` the left's rows and the right's
// columns, a scaled, negated or divided matrix keeps its own, `#` pairs them
// as eq:name. An inline value (a literal, a scalar) brings none: its rows and
// columns are named by position once the whole expression is evaluated (`A \
// (5,6)` names the new row r3, `(1,2)'` is r1 r2 x c1), while a stored matrix
// and J()/I() bring theirs (`J(1,2,0) \ J(1,2,1)` is r1 r1).
// Missing cells (StataBE 19.5, 2026-11-03 oracle, with P = (.a,1,.a,.,2) and
// Q = (1,.b,.b,.c,3) stored): `a - b` is a + (-b), and a sum accumulates into
// one operand -- the first that is a computed matrix (a literal, J(), a
// transpose, a negation, any result: anything but a stored matrix itself),
// else a scalar. A cell of the sum is `.` when the other operand's cell is
// missing and keeps the accumulating operand's missing value when only that one
// is (`(.a,1) + (1,.b)` is .a ., `P + (1,.b)` cells . .b, `1 - S` is .a for a
// stored S = (.a)); two stored matrices give `.` for any missing (`P + Q` is
// . . . . 5). Negating or scaling a matrix keeps a missing cell's kind, but a
// missing scale makes every cell `.` (`P*.b`); a negated missing scalar, any
// scalar arithmetic on a missing, and a product term with a missing make `.`.
// nullmat(ARG) (StataBE 19.5, 2026-11-03 oracle) looks ARG up among the
// user's matrices only; anything else -- an undefined name, a scalar, `e(b)`
// even after a fit, `(1,2)` -- is ABSENT. An absent operand may only be an
// operand of `,` or `\`, possibly in parentheses: an absent left operand
// yields the right one, which must not be absent too (`nullmat(Zb),
// nullmat(Zc)` is `matrix Zc not found`), an absent right one yields the
// left. Any other use -- the whole expression, `'`, unary minus, another
// operator, a function's argument -- is r(111) `matrix ARG not found`, ARG
// without its blanks (`nullmat( A , B )` is `matrix A,B not found`).


// joinOperand: the text is an operand of `,`/`\`, where an absent nullmat()
// may stand (see MxValue).
std::optional<MxValue> evalMx(const std::string& text, SessionState& session, std::string& err,
                              bool joinOperand);

MxValue mxScalar(double s) {
  MxValue v;
  v.scalar = true;
  v.s = s;
  return v;
}

MxValue mxMatrix(eval::EvalMatrix m) {
  m.rownames.resize(m.data.size());
  m.colnames.resize(mxCols(m));
  MxValue v;
  v.m = std::move(m);
  return v;
}

MxValue mxStored(const eval::EvalMatrix& m) {
  MxValue v = mxMatrix(m);
  v.stored = true;
  return v;
}

// Unary minus: a matrix keeps its missing cells' kinds (and is a computed one
// now); a missing scalar is `.` (StataBE: `-.a` is `.`).
MxValue mxNegate(MxValue v) {
  if (v.scalar) {
    v.s = mxMissing(v.s) ? std::numeric_limits<double>::quiet_NaN() : -v.s;
    return v;
  }
  for (auto& row : v.m.data)
    for (double& x : row)
      if (!mxMissing(x)) x = -x;
  v.stored = false;
  return v;
}

// A scalar taking part in a join or a `+`/`-` with a matrix: an unnamed 1x1.
eval::EvalMatrix mxAsMatrix(const MxValue& v) {
  if (!v.scalar) return v.m;
  eval::EvalMatrix m;
  m.data = {{v.s}};
  m.rownames = {""};
  m.colnames = {""};
  return m;
}

// The largest row/column dimension any Stata edition allows: c(max_matdim) is
// 65,534 for Stata/MP, 11,000 for SE and 800 for BE ([P] creturn); beyond it
// Stata answers r(915) "unable to allocate matrix" ([P] error). I() and J()
// check against it BEFORE casting or allocating, so a huge argument is a
// refusal and never an out-of-range cast or a whole-run allocation failure.
constexpr double kMaxMatDim = 65534;

// A scalar operand of the sublanguage, J()'s and I()'s arguments: a matrix
// there is r(109) `type mismatch` (`J(2,2,A)`), as is a string (`J(2,2,"3")`).
bool mxScalarOperand(const std::string& text, SessionState& session, double& out,
                     std::string& err) {
  std::optional<MxValue> v = evalMx(text, session, err);
  if (!v.has_value()) return false;
  if (!v->scalar) {
    err = "r(109): type mismatch";
    return false;
  }
  out = v->s;
  return true;
}

// A dimension of J()/I(): a scalar expression rounded to the nearest integer
// ([P] matrix define; StataBE: `J(1.4,2.5,1)` is 1x3, `I(2.5)` is 3x3). One
// that is missing or rounds below 1 is r(198) (`J(0,1,0)`, `J(-0.4,1,1)`,
// `J(.a,2,1)`); one beyond the largest edition's limit r(915).
bool mxDimension(const std::string& text, SessionState& session, size_t& out, std::string& err) {
  double v = 0.0;
  if (!mxScalarOperand(text, session, v, err)) return false;
  const double r = std::floor(v + 0.5);
  if (mxMissing(v) || r < 1) {
    err = "r(198): invalid syntax";
    return false;
  }
  if (r > kMaxMatDim) {
    err = "r(915): unable to allocate matrix";
    return false;
  }
  out = static_cast<size_t>(r);
  return true;
}

// `J(r, c, v)`: r x c filled with v, each argument a scalar expression
// (BUG-1400: `J(sc,1,0)`, `J(`k',`k'-1,.)`, `J(rowsof(A),colsof(A),el(A,2,2))`
// are rc 0 in StataBE). Anything but three comma-separated arguments is
// r(198), as is a zero dimension, which is why an empty result is an error
// here rather than a 0x0 matrix.
std::optional<eval::EvalMatrix> evalJCall(const std::string& inner, SessionState& session,
                                          std::string& err) {
  std::vector<std::string> a;
  std::vector<char> ops;
  if (!splitTopLevel(inner, a, ops) || a.size() != 3 ||
      std::any_of(ops.begin(), ops.end(), [](char o) { return o != ','; }) ||
      std::any_of(a.begin(), a.end(), [](const std::string& s) { return s.empty(); })) {
    err = "r(198): invalid syntax";
    return std::nullopt;
  }
  size_t dim[2] = {0, 0};
  for (int k = 0; k < 2; ++k)
    if (!mxDimension(a[static_cast<size_t>(k)], session, dim[k], err)) return std::nullopt;
  double fill = 0.0;
  if (!mxScalarOperand(a[2], session, fill, err)) return std::nullopt;
  eval::EvalMatrix m;
  m.data.assign(dim[0], std::vector<double>(dim[1], fill));
  applyDefaultStripe(m);
  return m;
}

// Horizontal (`,`) and vertical (`\`) concatenation, with Stata's conformability
// rules. A 0x0 operand -- which is what nullmat() yields for a matrix that does
// not exist yet -- is the identity, and that is the whole point of the idiom:
// `matrix b = nullmat(b), tmp` builds a matrix up one column-block at a time
// without a special case for the first pass.
std::optional<eval::EvalMatrix> concatMatrices(const eval::EvalMatrix& a,
                                               const eval::EvalMatrix& b, char op,
                                               std::string& err) {
  if (a.data.empty()) return b;
  if (b.data.empty()) return a;

  eval::EvalMatrix out;
  if (op == ',') {
    if (a.data.size() != b.data.size()) {
      err = "matrices are not conformable for horizontal concatenation";
      return std::nullopt;
    }
    out.data = a.data;
    for (size_t i = 0; i < out.data.size(); ++i)
      out.data[i].insert(out.data[i].end(), b.data[i].begin(), b.data[i].end());
    out.rownames = a.rownames;
    out.colnames = a.colnames;
    out.colnames.insert(out.colnames.end(), b.colnames.begin(), b.colnames.end());
  } else {
    if (a.data[0].size() != b.data[0].size()) {
      err = "matrices are not conformable for vertical concatenation";
      return std::nullopt;
    }
    out.data = a.data;
    out.data.insert(out.data.end(), b.data.begin(), b.data.end());
    out.colnames = a.colnames;
    out.rownames = a.rownames;
    out.rownames.insert(out.rownames.end(), b.rownames.begin(), b.rownames.end());
  }
  if (out.rownames.size() != out.data.size() ||
      out.colnames.size() != (out.data.empty() ? 0 : out.data[0].size()))
    applyDefaultStripe(out);
  return out;
}

// BUG-542 (submatrix extraction), one axis of `M[rowspec,colspec]` when either
// spec asks for a submatrix (mxRangeSpec), as StataBE 19.5 reads it (2026-11-03
// oracle): a quoted name `"rb"` (r(111) `rb not found` when the axis has no
// such name), a range `lo..hi` or an open range `lo...` whose bounds are names
// or scalar expressions (`sc..3`, `"na"..2`), or one scalar expression. A
// number is rounded to the nearest index here (`1.5..2` starts at row 2,
// `M[1.4,1..2]` is row 1); one outside the axis, or a range that runs
// backwards, is r(503) `conformability error`; a missing single index is the
// whole axis (`M[.,1..2]`); `1..` and `...2` are r(198). The 1-based inclusive
// [lo, hi], or nullopt with an `r(NNN): ` error.
std::optional<std::pair<size_t, size_t>> mxAxisRange(const std::string& spec,
                                                     const std::vector<std::string>& names,
                                                     size_t dim, SessionState& session,
                                                     std::string& err) {
  size_t dots = std::string::npos;  // the range's `..`, outside quotes and brackets
  {
    int depth = 0;
    for (size_t i = 0; i + 1 < spec.size(); ++i) {
      const char c = spec[i];
      if (c == '"') {
        const size_t close = spec.find('"', i + 1);
        if (close == std::string::npos) break;
        i = close;
      } else if (c == '(' || c == '[') {
        ++depth;
      } else if (c == ')' || c == ']') {
        --depth;
      } else if (depth == 0 && c == '.' && spec[i + 1] == '.') {
        dots = i;
        break;
      }
    }
  }
  std::string loText = trim(spec), hiText;
  bool open = false;
  if (dots != std::string::npos) {
    loText = trim(spec.substr(0, dots));
    hiText = spec.substr(dots + 2);
    if (!hiText.empty() && hiText[0] == '.') {
      open = true;
      hiText = hiText.substr(1);
    }
    hiText = trim(hiText);
    if (loText.empty() || open != hiText.empty()) {
      err = "r(198): invalid syntax";
      return std::nullopt;
    }
  }
  // 1: an index in `out`; 0: the whole axis (a missing single index); -1: refused.
  const auto bound = [&](const std::string& t, bool single, size_t& out) -> int {
    std::string quoted = t;
    if (quoted.size() >= 4 && quoted.compare(0, 2, "`\"") == 0 &&
        quoted.compare(quoted.size() - 2, 2, "\"'") == 0)
      quoted = quoted.substr(1, quoted.size() - 2);
    if (quoted.size() >= 2 && quoted.front() == '"' && quoted.back() == '"') {
      const std::string nm = quoted.substr(1, quoted.size() - 2);
      for (size_t k = 0; k < names.size(); ++k) {
        if (names[k] == nm) {
          out = k + 1;
          return 1;
        }
      }
      err = "r(111): " + nm + " not found";
      return -1;
    }
    double v = 0.0;
    if (!evalMatrixScalarExpr(t, session, v, err)) return -1;
    if (mxMissing(v) && single) return 0;
    const double r = std::floor(v + 0.5);
    if (mxMissing(v) || r < 1 || r > static_cast<double>(dim)) {
      err = "r(503): conformability error";
      return -1;
    }
    out = static_cast<size_t>(r);
    return 1;
  };
  size_t lo = 0, hi = 0;
  const int got = bound(loText, dots == std::string::npos, lo);
  if (got < 0) return std::nullopt;
  if (got == 0) return std::make_pair(size_t{1}, dim);
  if (dots == std::string::npos) return std::make_pair(lo, lo);
  if (open) {
    hi = dim;
  } else if (bound(hiText, false, hi) < 0) {
    return std::nullopt;
  }
  if (lo > hi) {
    err = "r(503): conformability error";
    return std::nullopt;
  }
  return std::make_pair(lo, hi);
}

// `M[rowspec,colspec]` with a range or name spec: the rows and columns it
// selects, with their names.
std::optional<eval::EvalMatrix> mxSubmatrix(const eval::EvalMatrix& src, const std::string& rowSpec,
                                            const std::string& colSpec, SessionState& session,
                                            std::string& err) {
  const auto rows = mxAxisRange(rowSpec, src.rownames, src.data.size(), session, err);
  if (!rows.has_value()) return std::nullopt;
  const auto cols = mxAxisRange(colSpec, src.colnames, mxCols(src), session, err);
  if (!cols.has_value()) return std::nullopt;
  eval::EvalMatrix out;
  for (size_t i = rows->first; i <= rows->second; ++i) {
    const auto& row = src.data[i - 1];
    out.data.emplace_back(row.begin() + static_cast<std::ptrdiff_t>(cols->first - 1),
                          row.begin() + static_cast<std::ptrdiff_t>(cols->second));
    out.rownames.push_back(i - 1 < src.rownames.size() ? src.rownames[i - 1] : "r" + std::to_string(i));
  }
  for (size_t j = cols->first; j <= cols->second; ++j)
    out.colnames.push_back(j - 1 < src.colnames.size() ? src.colnames[j - 1] : "c" + std::to_string(j));
  return out;
}

// The top-level positions in `e` of the operator `op` -- outside "strings" and
// ()/[] groups. `+` and `-` count only as binary operators, after an operand
// (a name, a number, `.`/`.a`, a closing bracket, a transpose `'`, a string)
// or right after a `+`, which StataBE reads into the operand before it (`A +
// +B` is `A+` and B, `+-2` is `+` and 2; mxPlusRefusal): never a sign (`A*-1`,
// `A - -B`) or an exponent (`1e-5`). `e` is balanced (evalMx checks first).
std::vector<size_t> mxTopLevelOps(const std::string& e, char op) {
  auto isIdStart = [](char c) { return cecon::ident::isIdentStartByte(c); };
  auto isIdChar = [](char c) { return cecon::ident::isIdentByte(c); };
  auto isDigit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
  std::vector<size_t> at;
  bool afterOperand = false;
  bool afterPlus = false;
  size_t i = 0;
  while (i < e.size()) {
    const char c = e[i];
    if (c == ' ' || c == '\t') {
      ++i;
      continue;
    }
    const bool plusBefore = afterPlus;
    afterPlus = false;
    if (c == '"') {
      const size_t close = e.find('"', i + 1);
      i = close == std::string::npos ? e.size() : close + 1;
      afterOperand = true;
    } else if (c == '(' || c == '[') {
      int depth = 0;
      for (; i < e.size(); ++i) {
        if (e[i] == '(' || e[i] == '[') ++depth;
        else if ((e[i] == ')' || e[i] == ']') && --depth == 0) break;
      }
      ++i;
      afterOperand = true;
    } else if (isIdStart(c)) {
      while (i < e.size() && isIdChar(e[i])) ++i;
      afterOperand = true;
    } else if (isDigit(c) || (c == '.' && i + 1 < e.size() && isDigit(e[i + 1]))) {
      while (i < e.size() && (isDigit(e[i]) || e[i] == '.')) ++i;
      if (i < e.size() && (e[i] == 'e' || e[i] == 'E')) {
        size_t k = i + 1;
        if (k < e.size() && (e[k] == '+' || e[k] == '-')) ++k;
        if (k < e.size() && isDigit(e[k])) {
          i = k;
          while (i < e.size() && isDigit(e[i])) ++i;
        }
      }
      afterOperand = true;
    } else if (c == '.') {  // `.`, `.a`-`.z`
      ++i;
      while (i < e.size() && isIdChar(e[i])) ++i;
      afterOperand = true;
    } else if (c == '\'') {  // a postfix transpose
      ++i;
      afterOperand = true;
    } else {
      if (c == op && (afterOperand || plusBefore || (op != '+' && op != '-'))) at.push_back(i);
      afterOperand = false;
      afterPlus = c == '+';
      ++i;
    }
  }
  return at;
}

// The index of the bracket closing the one at `open`, both kinds on one depth.
size_t mxClosing(const std::string& e, size_t open) {
  int depth = 0;
  for (size_t i = open; i < e.size(); ++i) {
    if (e[i] == '(' || e[i] == '[') ++depth;
    else if ((e[i] == ')' || e[i] == ']') && --depth == 0) return i;
  }
  return e.size();
}

// True when the `[` at `open` starts a bracket group: StataBE reads `[exp]` as
// `(exp)` (w2q_matrix_edges A09, 087's `matrix A[1,3] = [`delta_M']`: .25;
// 2026-11-03 oracle: `[1+2]*3` is 9, `matrix B = [0.5]` and `([1], 2)` rc 0).
// It is not one when it subscripts what stands before it (`A[1,2]`,
// `e(b)[1,1]`) or when a name follows it, the `[eq]name` / `[#1]_b[x]`
// coefficient reference the scalar evaluator owns.
bool mxBracketGroupAt(const std::string& e, size_t open) {
  size_t before = open;
  while (before > 0 && (e[before - 1] == ' ' || e[before - 1] == '\t')) --before;
  if (before > 0) {
    const char p = e[before - 1];
    if (p == ')' || p == ']' || p == '\'' || cecon::ident::isIdentByte(p)) return false;
  }
  const size_t close = mxClosing(e, open);
  return close + 1 >= e.size() || !cecon::ident::isIdentByte(e[close + 1]);
}

// True when `e` needs the matrix grammar: it holds a matrix -- a stored NAME
// that is not a call or subscript head, a group with its own `,`/`\`, a top-
// level `,`/`\`, one of the matrix functions, a range or name subscript of a
// stored matrix or its empty one (`A[]`), an e()/r() matrix -- a postfix
// transpose, or the matrix-only
// operator `#` (`sc # sc` is r(109), not a scalar syntax error). Anything else is
// one scalar expression, evaluated whole by evalMatrixScalarExpr: a call's
// arguments are scalar context (`el(M,2,3)`, `scalar(Bcol)`), and so is
// `M[i,j]`.
bool mxNeedsGrammar(const std::string& e, SessionState& session) {
  auto isIdStart = [](char c) { return cecon::ident::isIdentStartByte(c); };
  auto isIdChar = [](char c) { return cecon::ident::isIdentByte(c); };
  auto isDigit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
  static const char* const kMatrixFns[] = {"J",   "I",      "nullmat", "inv",
                                           "invsym", "syminv", "vecdiag"};
  size_t i = 0;
  while (i < e.size()) {
    const char c = e[i];
    if (c == '"') {
      const size_t close = e.find('"', i + 1);
      i = close == std::string::npos ? e.size() : close + 1;
    } else if (c == ',' || c == '\\' || c == '\'' || c == '#') {
      return true;
    } else if (c == '(' || c == '[') {
      const size_t close = mxClosing(e, i);
      if (c == '(' && mxNeedsGrammar(e.substr(i + 1, close - i - 1), session)) return true;
      // A `[` that is no subscript (nothing it could index stands before it)
      // and no `[eq]name` coefficient reference (no name follows it) is a
      // bracket group, which the scalar evaluator does not read.
      if (c == '[' && mxBracketGroupAt(e, i)) return true;
      i = close + 1;
    } else if (isDigit(c) || (c == '.' && i + 1 < e.size() && isDigit(e[i + 1]))) {
      while (i < e.size() && (isDigit(e[i]) || e[i] == '.')) ++i;
      if (i < e.size() && (e[i] == 'e' || e[i] == 'E')) {
        ++i;
        if (i < e.size() && (e[i] == '+' || e[i] == '-')) ++i;
      }
      while (i < e.size() && isIdChar(e[i])) ++i;
    } else if (c == '.') {
      ++i;
      while (i < e.size() && isIdChar(e[i])) ++i;
    } else if (isIdStart(c)) {
      const size_t start = i;
      while (i < e.size() && isIdChar(e[i])) ++i;
      const std::string id = e.substr(start, i - start);
      size_t next = i;
      while (next < e.size() && (e[next] == ' ' || e[next] == '\t')) ++next;
      if (next < e.size() && e[next] == '(') {
        const size_t close = mxClosing(e, next);
        for (const char* f : kMatrixFns)
          if (id == f) return true;
        if (next > i && session.eval.findMatrix(id) != nullptr) return true;  // `A (1,2)`
        if (id == "e" || id == "r") {
          const std::string which = trim(e.substr(next + 1, close - next - 1));
          if (session.eval.findClassMatrix(id[0], which) != nullptr ||
              (id == "e" && (which == "b" || which == "V")))
            return true;
        }
        i = close + 1;
      } else if (next < e.size() && e[next] == '[') {
        const size_t close = mxClosing(e, next);
        const std::string body = e.substr(next + 1, close - next - 1);
        if (session.eval.findMatrix(id) != nullptr &&
            (body.find("..") != std::string::npos || body.find('"') != std::string::npos ||
             trim(body).empty()))
          return true;
        i = close + 1;
      } else if (session.eval.findMatrix(id) != nullptr) {
        return true;
      }
    } else {
      ++i;
    }
  }
  return false;
}

std::optional<MxValue> mxApply(char op, MxValue a, MxValue b, std::string& err) {
  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  if (op == '\\' || op == ',') {
    if (a.absent) {
      if (b.absent) {
        err = "r(111): matrix " + b.absentArg + " not found";
        return std::nullopt;
      }
      return b;
    }
    if (b.absent) return a;
    std::string joinErr;
    std::optional<eval::EvalMatrix> joined =
        concatMatrices(mxAsMatrix(a), mxAsMatrix(b), op, joinErr);
    if (!joined.has_value()) {
      err = "r(503): conformability error";
      return std::nullopt;
    }
    return mxMatrix(std::move(*joined));
  }
  // A result that overflows to +/-inf (or is inf * 0) is `.`: StataBE lists
  // `A*1e300*1e300` as `.` and svmat stores it missing (2026-11-03 oracle).
  const auto real = [kNaN](double v) { return std::isfinite(v) ? v : kNaN; };
  if (a.scalar && b.scalar && op != '#') {
    if (op == '/')
      return mxScalar(mxMissing(a.s) || mxMissing(b.s) || b.s == 0.0 ? kNaN : real(a.s / b.s));
    if (mxMissing(a.s) || mxMissing(b.s)) return mxScalar(kNaN);
    return mxScalar(real(op == '+' ? a.s + b.s : op == '-' ? a.s - b.s : a.s * b.s));
  }
  // Scale `m` by `s`: a missing cell keeps its kind, a missing `s` makes
  // every cell `.`.
  const auto scaled = [kNaN, &real](eval::EvalMatrix m, double s) {
    for (auto& row : m.data)
      for (double& v : row) {
        if (mxMissing(s)) v = kNaN;
        else if (!mxMissing(v)) v = real(v * s);
      }
    return mxMatrix(std::move(m));
  };
  if (op == '+' || op == '-') {
    if (op == '-') b = mxNegate(std::move(b));
    const eval::EvalMatrix x = mxAsMatrix(a);
    eval::EvalMatrix y = mxAsMatrix(b);  // the right operand's names
    if (x.data.size() != y.data.size() || mxCols(x) != mxCols(y)) {
      err = "r(503): conformability error";
      return std::nullopt;
    }
    // The operand the sum accumulates into (0 left, 1 right, -1 neither: two
    // stored matrices), as the comment above MxValue says.
    const int acc = !a.scalar && !a.stored ? 0 : !b.scalar && !b.stored ? 1 : a.scalar ? 0 : b.scalar ? 1 : -1;
    for (size_t i = 0; i < y.data.size(); ++i)
      for (size_t j = 0; j < y.data[i].size(); ++j) {
        const double l = x.data[i][j], r = y.data[i][j];
        const double kept = acc == 0 ? l : r, other = acc == 0 ? r : l;
        if (acc < 0) y.data[i][j] = mxMissing(l) || mxMissing(r) ? kNaN : real(l + r);
        else y.data[i][j] = mxMissing(other) ? kNaN : mxMissing(kept) ? kept : real(l + r);
      }
    return mxMatrix(std::move(y));
  }
  if (op == '*') {
    if (a.scalar) return scaled(std::move(b.m), a.s);
    if (b.scalar) return scaled(std::move(a.m), b.s);
    const size_t n = mxCols(a.m);
    if (n != b.m.data.size()) {
      if (a.m.data.size() == 1 && n == 1) return scaled(std::move(b.m), a.m.data[0][0]);
      if (b.m.data.size() == 1 && mxCols(b.m) == 1)
        return scaled(std::move(a.m), b.m.data[0][0]);
      err = "r(503): conformability error";
      return std::nullopt;
    }
    eval::EvalMatrix out;
    out.data.assign(a.m.data.size(), std::vector<double>(mxCols(b.m), 0.0));
    for (size_t i = 0; i < out.data.size(); ++i)
      for (size_t j = 0; j < out.data[i].size(); ++j) {
        double sum = 0.0;
        for (size_t k = 0; k < n; ++k) {
          const double l = a.m.data[i][k], r = b.m.data[k][j];
          if (mxMissing(l) || mxMissing(r)) {
            sum = kNaN;
            break;
          }
          sum += l * r;
        }
        out.data[i][j] = real(sum);
      }
    out.rownames = a.m.rownames;
    out.colnames = b.m.colnames;
    return mxMatrix(std::move(out));
  }
  if (op == '/') {
    if (!b.scalar) {
      err = "r(109): type mismatch";
      return std::nullopt;
    }
    if (b.s == 0.0 || mxMissing(b.s)) {
      err = "r(504): matrix has missing values";
      return std::nullopt;
    }
    // StataBE scales by the reciprocal: `D/10` is D*(1/10) to the last bit
    // (+1.3333333333334X-002 for 3/10), `D/3/7` is D*(1/3)*(1/7).
    return scaled(std::move(a.m), 1.0 / b.s);
  }
  // `#`: the Kronecker product; row (i,k) is named A's row i as the equation
  // and B's row k as the name (an unnamed A row by its own position, an
  // unnamed B row by its position in the result).
  if (a.scalar || b.scalar) {
    err = "r(109): type mismatch";
    return std::nullopt;
  }
  const auto subname = [](const std::string& n) {
    const size_t colon = n.rfind(':');
    return colon == std::string::npos ? n : n.substr(colon + 1);
  };
  const auto pairNames = [&subname](const std::vector<std::string>& outer,
                                    const std::vector<std::string>& inner, char kind) {
    std::vector<std::string> names;
    for (size_t i = 0; i < outer.size(); ++i) {
      const std::string eq =
          outer[i].empty() ? std::string(1, kind) + std::to_string(i + 1) : subname(outer[i]);
      for (const auto& n : inner) names.push_back(eq + ":" + subname(n));
    }
    return names;
  };
  const size_t rb = b.m.data.size(), cb = mxCols(b.m);
  eval::EvalMatrix out;
  out.data.assign(a.m.data.size() * rb, std::vector<double>(mxCols(a.m) * cb, 0.0));
  for (size_t i = 0; i < a.m.data.size(); ++i)
    for (size_t j = 0; j < a.m.data[i].size(); ++j)
      for (size_t k = 0; k < rb; ++k)
        for (size_t l = 0; l < cb; ++l) {
          const double x = a.m.data[i][j], y = b.m.data[k][l];
          out.data[i * rb + k][j * cb + l] = mxMissing(x) || mxMissing(y) ? kNaN : real(x * y);
        }
  out.rownames = pairNames(a.m.rownames, b.m.rownames, 'r');
  out.colnames = pairNames(a.m.colnames, b.m.colnames, 'c');
  return mxMatrix(std::move(out));
}

// One operand the operator levels of evalMx have left: a group, a matrix
// function, a subscript, a stored NAME or an e()/r() matrix. A matrix inside
// anything else (`A^2`, `A < 2`) is r(109).
std::optional<MxValue> evalMxPrimary(const std::string& e, SessionState& session,
                                     std::string& err, bool joinOperand) {
  // A group: one holding its own `,`/`\` is a literal (read whole by
  // parseMatrixLiteral when every cell is a scalar, unnamed); any other is
  // the expression inside, so `(2)` is a scalar (`A/(2)` divides, `(1,.a) #
  // (1)` is r(109)).
  if (wrappedInParens(e)) {
    const std::string in = trim(e.substr(1, e.size() - 2));
    if (!in.empty() && (!mxTopLevelOps(in, ',').empty() || !mxTopLevelOps(in, '\\').empty())) {
      std::optional<std::vector<std::vector<double>>> rows = parseMatrixLiteral(e, session);
      if (rows.has_value()) {
        eval::EvalMatrix m;
        m.data = std::move(*rows);
        return mxMatrix(std::move(m));
      }
    }
    return evalMx(in, session, err, joinOperand);
  }
  // A bracket group is a parenthesised one (mxBracketGroupAt).
  if (e.size() >= 2 && e[0] == '[' && mxClosing(e, 0) + 1 == e.size() && mxBracketGroupAt(e, 0))
    return evalMxPrimary("(" + e.substr(1, e.size() - 2) + ")", session, err, joinOperand);
  // A call's `(` follows its name with no blank between: StataBE reads `J
  // (2,2,0)` and `nullmat (Zb)` as the names J and nullmat, r(111) `J not
  // found`, and `A (1,2)` with A a matrix as r(198) (2026-11-03 oracle).
  {
    static const std::regex reSpacedCall(std::string("^(") + eval::kRxName + R"()[ \t]+\()");
    std::smatch sc;
    if (std::regex_search(e, sc, reSpacedCall)) {
      err = session.eval.findMatrix(sc.str(1)) != nullptr ? "r(198): invalid syntax"
                                                          : "r(111): " + sc.str(1) + " not found";
      return std::nullopt;
    }
  }
  auto callArg = [&e](const char* fn, std::string& inner) {
    const size_t n = std::strlen(fn);
    if (e.size() < n + 2 || e.compare(0, n, fn) != 0) return false;
    const size_t i = n;
    if (e[i] != '(' || mxClosing(e, i) + 1 != e.size()) return false;
    inner = trim(e.substr(i + 1, e.size() - i - 2));
    return true;
  };
  std::string inner;
  if (callArg("J", inner)) {
    std::optional<eval::EvalMatrix> m = evalJCall(inner, session, err);
    if (!m.has_value()) return std::nullopt;
    return mxMatrix(std::move(*m));
  }
  // nullmat(ARG): a stored matrix, else absent (see MxValue).
  if (callArg("nullmat", inner)) {
    const std::string arg = mxSqueeze(inner);
    if (const eval::EvalMatrix* stored = session.eval.findMatrix(arg)) return mxStored(*stored);
    if (!joinOperand) {
      err = "r(111): matrix " + arg + " not found";
      return std::nullopt;
    }
    MxValue v = mxMatrix(eval::EvalMatrix{});
    v.absent = true;
    v.absentArg = arg;
    return v;
  }
  // I(n): the n x n identity (oracle w2c_functions.log:75 I_n3), default
  // stripe; n is a dimension (`I(`K')`, `I(sc+1)`, `I(2.5)` is 3x3). More than
  // one argument is r(198), as for J() (StataBE: `I(1,2)`).
  if (callArg("I", inner)) {
    std::vector<std::string> args;
    std::vector<char> argOps;
    if (!splitTopLevel(inner, args, argOps) || args.size() != 1) {
      err = "r(198): invalid syntax";
      return std::nullopt;
    }
    size_t n = 0;
    if (!mxDimension(inner, session, n, err)) return std::nullopt;
    eval::EvalMatrix m;
    m.data.assign(n, std::vector<double>(n, 0.0));
    for (size_t k = 0; k < n; ++k) m.data[k][k] = 1.0;
    applyDefaultStripe(m);
    return mxMatrix(std::move(m));
  }
  // The one argument of vecdiag()/inv()/invsym()/syminv(): any matrix
  // expression (`inv((1,2\3,4))`). StataBE 19.5 (2026-11-03 oracle): two
  // arguments are r(198), a scalar r(109) (`inv(2)`), an unknown name r(111)
  // `NAME not found`.
  const auto matrixArg = [&session, &err](const std::string& text) -> std::optional<MxValue> {
    std::vector<std::string> args;
    std::vector<char> argOps;
    if (!splitTopLevel(text, args, argOps) || args.size() != 1) {
      err = "r(198): invalid syntax";
      return std::nullopt;
    }
    std::optional<MxValue> v = evalMx(text, session, err);
    if (v.has_value() && v->scalar) {
      err = "r(109): type mismatch";
      return std::nullopt;
    }
    return v;
  };
  // vecdiag(M): the 1 x n row of a SQUARE M's diagonal (oracle
  // w2c_functions.log:100 vecdiag_basic: row name r1, M's column names), M
  // any matrix (`vecdiag(e(V))`, `vecdiag((1,2\3,4))`); a non-square M is
  // r(503) `conformability error` (StataBE 19.5), never a truncated diagonal.
  // vecdiag is a matrix-sublanguage function only: `local x =
  // el(vecdiag(M),1,3)` stays r(133) (w2c_functions.log:110).
  // invsym(M) / syminv(M) / inv(M) ([FN] Matrix functions): the inverse of a
  // square matrix. invsym is Stata's sweep in column order: a pivot that falls
  // to (relatively) zero drops that row and column, which are returned as 0
  // (the generalized inverse). inv() is an LU decomposition with implicitly
  // scaled partial pivoting -- each row weighed by the reciprocal of its
  // largest magnitude, the pivot the largest weighed entry (the last of a
  // tie), each multiplier formed with the pivot's reciprocal -- then a forward
  // and a back substitution (dividing by the diagonal) per column of I: the
  // method whose %21x bits StataBE 19.5's inv() reproduces, 30 of 30 matrices
  // with ties and near-singular ones (2026-11-03 oracle). A zero row or an
  // exactly zero pivot is its r(504) `matrix has missing values` (`inv(T'*T)`
  // for T = (7,8)); `inv((1,2,3\4,5,6\7,8,9))`, whose rounded last pivot is
  // not zero, answers. The inverse's rows are the source's columns.
  for (const char* fnName : {"syminv", "invsym", "inv"}) {
    if (!callArg(fnName, inner)) continue;
    std::optional<MxValue> arg = matrixArg(inner);
    if (!arg.has_value()) return std::nullopt;
    const eval::EvalMatrix* src = &arg->m;
    const size_t n = src->data.size();
    if (n == 0 || src->data[0].size() != n) {
      err = "r(503): conformability error";
      return std::nullopt;
    }
    for (const auto& row : src->data)
      for (double v : row)
        if (!std::isfinite(v) || v >= eval::kStataMissingSentinel) {
          err = "r(504): matrix has missing values";
          return std::nullopt;
        }
    eval::EvalMatrix m = *src;
    if (std::string(fnName) == "inv") {
      std::vector<std::vector<double>> a = src->data;
      std::vector<size_t> perm(n);  // perm[i]: the source row now at row i
      std::vector<double> weight(n);
      for (size_t i = 0; i < n; ++i) {
        perm[i] = i;
        double big = 0.0;
        for (double v : a[i]) big = std::max(big, std::fabs(v));
        if (big == 0.0) {
          err = "r(504): matrix has missing values";
          return std::nullopt;
        }
        weight[i] = 1.0 / big;
      }
      for (size_t c = 0; c < n; ++c) {
        size_t piv = c;
        double best = -1.0;
        for (size_t r = c; r < n; ++r) {
          const double d = weight[r] * std::fabs(a[r][c]);
          if (d >= best) {
            best = d;
            piv = r;
          }
        }
        if (a[piv][c] == 0.0) {
          err = "r(504): matrix has missing values";
          return std::nullopt;
        }
        std::swap(a[piv], a[c]);
        std::swap(perm[piv], perm[c]);
        std::swap(weight[piv], weight[c]);
        const double recip = 1.0 / a[c][c];
        for (size_t r = c + 1; r < n; ++r) a[r][c] *= recip;
        for (size_t j = c + 1; j < n; ++j)
          for (size_t r = c + 1; r < n; ++r) a[r][j] -= a[r][c] * a[c][j];
      }
      std::vector<std::vector<double>> out(n, std::vector<double>(n, 0.0));
      std::vector<double> x(n);
      for (size_t col = 0; col < n; ++col) {
        for (size_t i = 0; i < n; ++i) {
          double s = perm[i] == col ? 1.0 : 0.0;
          for (size_t k = 0; k < i; ++k) s -= a[i][k] * x[k];
          x[i] = s;
        }
        for (size_t i = n; i-- > 0;) {
          double s = x[i];
          for (size_t k = i + 1; k < n; ++k) s -= a[i][k] * x[k];
          x[i] = s / a[i][i];
        }
        for (size_t i = 0; i < n; ++i) out[i][col] = x[i];
      }
      m.data = std::move(out);
    } else {
      // Symmetric sweep (Goodnight) over the symmetrized matrix.
      std::vector<std::vector<double>> a(n, std::vector<double>(n, 0.0));
      for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j) a[i][j] = 0.5 * (src->data[i][j] + src->data[j][i]);
      std::vector<double> d0(n);
      for (size_t i = 0; i < n; ++i) d0[i] = a[i][i];
      std::vector<char> dropped(n, 0);
      for (size_t k = 0; k < n; ++k) {
        const double piv = a[k][k];
        if (!(std::fabs(piv) > 1e-13 * std::max(std::fabs(d0[k]), 1e-300)) || d0[k] == 0.0) {
          dropped[k] = 1;
          continue;
        }
        const double inv = 1.0 / piv;
        for (size_t i = 0; i < n; ++i) {
          if (i == k) continue;
          for (size_t j = 0; j < n; ++j) {
            if (j == k) continue;
            a[i][j] -= a[i][k] * a[k][j] * inv;
          }
        }
        for (size_t i = 0; i < n; ++i) {
          if (i == k) continue;
          a[i][k] *= inv;
          a[k][i] *= inv;
        }
        a[k][k] = -inv;
      }
      for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j) a[i][j] = (dropped[i] || dropped[j]) ? 0.0 : -a[i][j];
      m.data = std::move(a);
    }
    // The inverse's rows are the source's columns and vice versa.
    std::swap(m.rownames, m.colnames);
    return mxMatrix(std::move(m));
  }
  if (callArg("vecdiag", inner)) {
    std::optional<MxValue> arg = matrixArg(inner);
    if (!arg.has_value()) return std::nullopt;
    const eval::EvalMatrix& src = arg->m;
    const size_t nrows = src.data.size();
    if (nrows != mxCols(src)) {
      err = "r(503): conformability error";
      return std::nullopt;
    }
    eval::EvalMatrix m;
    m.data.assign(1, std::vector<double>(nrows, 0.0));
    for (size_t k = 0; k < nrows; ++k) m.data[0][k] = src.data[k][k];
    m.rownames = {"r1"};
    m.colnames = src.colnames;
    return mxMatrix(std::move(m));
  }
  // A subscript of a stored matrix: `A[]` is A whole; two scalar subscripts
  // read one element, a scalar (mxElementAt; `matrix X = A[1,1]` is r1 x c1,
  // oracle w2q L02); a range or a row/column name in either is a submatrix
  // that keeps its names (BUG-542, mxAxisRange). A malformed body is
  // mxSubscriptSpecs' refusal.
  if (!e.empty() && e.back() == ']') {
    const size_t br = e.find('[');
    static const std::regex reIdent(std::string("^") + eval::kRxName + "$");
    if (br != std::string::npos && br > 0 && std::regex_match(e.substr(0, br), reIdent) &&
        mxClosing(e, br) + 1 == e.size()) {
      if (const eval::EvalMatrix* src = session.eval.findMatrix(e.substr(0, br))) {
        const std::string body = e.substr(br + 1, e.size() - br - 2);
        if (trim(body).empty()) return mxMatrix(*src);
        const std::optional<std::pair<std::string, std::string>> specs =
            mxSubscriptSpecs(e.substr(0, br), body, err);
        if (!specs.has_value()) return std::nullopt;
        if (!mxRangeSpec(specs->first) && !mxRangeSpec(specs->second)) {
          double r = 0.0, c = 0.0;
          if (!evalMatrixScalarExpr(specs->first, session, r, err) ||
              !evalMatrixScalarExpr(specs->second, session, c, err))
            return std::nullopt;
          return mxScalar(mxElementAt(*src, r, c));
        }
        std::optional<eval::EvalMatrix> part =
            mxSubmatrix(*src, specs->first, specs->second, session, err);
        if (!part.has_value()) return std::nullopt;
        return mxMatrix(std::move(*part));
      }
    }
  }
  if (const eval::EvalMatrix* stored = session.eval.findMatrix(e)) return mxStored(*stored);
  // An e()/r() class MATRIX, `(e(b) \ e(b))`, through the one resolver.
  {
    static const std::regex reClassRef(std::string(R"(^([er])\(\s*()") + eval::kRxName + R"()\s*\)$)");
    std::smatch cm;
    if (std::regex_match(e, cm, reClassRef)) {
      const char cls = cm.str(1)[0];
      const std::string which = cm.str(2);
      if (session.eval.findClassMatrix(cls, which) != nullptr ||
          (cls == 'e' && (which == "b" || which == "V"))) {
        std::string classErr;
        int classRc = 111;
        std::optional<eval::EvalMatrix> resolved =
            resolveClassMatrixRef(cls, which, session, classErr, &classRc);
        if (resolved.has_value()) return mxStored(*resolved);
        // The refusal carries its rc through matrixExprErrorBlock (BUG-584),
        // never runMatrixDef's code-less block (rc 1): no e() at all is r(301),
        // Stata's "last estimates not found" (w2g_ebmat.log:2229); an absent
        // name is r(111) -- the resolver says which.
        err = "r(" + std::to_string(classRc) + "): " + classErr;
        return std::nullopt;
      }
    }
  }
  // A name, `'` and more name characters with no space between (`X'X`, `A'2`)
  // is one name to StataBE, which it then does not find: r(111) `X'X not
  // found` (2026-11-03 oracle; `X' X` is two operands, r(198)).
  {
    static const std::regex reQuoteJoined(std::string("^") + eval::kRxName + "('" +
                                          eval::kRxNameChar + "+)+$");
    if (std::regex_match(e, reQuoteJoined)) {
      err = "r(111): " + e + " not found";
      return std::nullopt;
    }
  }
  // A matrix inside a scalar-only construct, `A^2`, `sqrt(A)`, `A < 2`: StataBE's
  // r(109) (BUG-584). Two operands with no operator between them (`A A`) are
  // still invalid syntax, r(198).
  if (!firstBareMatrixName(e, session).empty()) {
    double ignored = 0.0;
    std::string scalarErr;
    if (!evalMatrixScalarExpr(e, session, ignored, scalarErr) &&
        scalarErr.rfind("r(198)", 0) == 0) {
      err = scalarErr;
      return std::nullopt;
    }
    err = "r(109): type mismatch";
    return std::nullopt;
  }
  double v = 0.0;
  if (!evalMatrixScalarExpr(e, session, v, err)) return std::nullopt;
  return mxScalar(v);
}

// The operand a dangling operator is read into: the last one of `part`, or
// its first one when the operator leads (StataBE: `(1,2\)` is `2\ not found`).
std::string mxEdgeOperand(const std::string& part, bool last) {
  size_t cut = last ? 0 : part.size();
  for (const char op : {'\\', ',', '+', '-', '*', '/', '#'})
    for (const size_t p : mxTopLevelOps(part, op)) cut = last ? std::max(cut, p + 1) : std::min(cut, p);
  return mxSqueeze(last ? part.substr(cut) : part.substr(0, cut));
}

std::optional<MxValue> evalMx(const std::string& text, SessionState& session, std::string& err,
                              bool joinOperand) {
  const std::string e = trim(text);
  if (e.empty()) {
    err = "r(198): invalid syntax";
    return std::nullopt;
  }
  {
    int depth = 0;
    for (size_t i = 0; i < e.size(); ++i) {
      if (e[i] == '"') {
        const size_t close = e.find('"', i + 1);
        if (close == std::string::npos) break;
        i = close;
      } else if (e[i] == '(' || e[i] == '[') {
        ++depth;
      } else if ((e[i] == ')' || e[i] == ']') && --depth < 0) {
        err = "r(132): too many ')' or ']'";
        return std::nullopt;
      }
    }
    if (depth > 0) {
      err = "r(132): too few ')' or ']'";
      return std::nullopt;
    }
  }
  if (!mxNeedsGrammar(e, session)) {
    double v = 0.0;
    if (!evalMatrixScalarExpr(e, session, v, err)) return std::nullopt;
    return mxScalar(v);
  }
  for (const char op : {'\\', ',', '+', '-', '*', '/', '#'}) {
    const std::vector<size_t> at = mxTopLevelOps(e, op);
    if (at.empty()) continue;
    std::vector<std::string> parts;
    size_t from = 0;
    for (const size_t p : at) {
      parts.push_back(trim(e.substr(from, p - from)));
      from = p + 1;
    }
    parts.push_back(trim(e.substr(from)));
    // Left to right. A dangling operator is read into the operand before it,
    // or after it when it leads, and that name is looked up (StataBE: `A +`
    // is `A+ not found`, `A * B *` is `B* not found`, `*A * B` is `*A not
    // found`, `A + B + +C` is `B+ not found`, while `nosuch + A +` is `nosuch
    // not found`), r(111).
    std::optional<MxValue> acc;
    for (size_t k = 0; k < parts.size(); ++k) {
      std::string glued;
      if (k == 0 && parts[0].empty()) glued = std::string(1, op) + mxEdgeOperand(parts[1], false);
      else if (k + 1 < parts.size() && parts[k + 1].empty()) glued = mxEdgeOperand(parts[k], true) + op;
      if (!glued.empty()) {
        err = "r(111): " + glued + " not found";
        return std::nullopt;
      }
      std::optional<MxValue> v = evalMx(parts[k], session, err, op == '\\' || op == ',');
      if (!v.has_value()) return std::nullopt;
      acc = k == 0 ? std::move(v) : mxApply(op, std::move(*acc), std::move(*v), err);
      if (!acc.has_value()) return std::nullopt;
    }
    return acc;
  }
  if (e[0] == '-') {
    std::optional<MxValue> v = evalMx(e.substr(1), session, err);
    if (!v.has_value()) return std::nullopt;
    return mxNegate(std::move(*v));
  }
  // A leading `+` is only a number's sign (mxPlusRefusal): `+2'` is 2, `+A`
  // r(111) `+A not found`, `+(1,2)` r(133).
  if (e[0] == '+') {
    err = mxPlusRefusal(e);
    if (!err.empty()) return std::nullopt;
    return evalMx(e.substr(1), session, err);
  }
  if (e.back() == '\'') {
    std::optional<MxValue> v = evalMx(e.substr(0, e.size() - 1), session, err);
    if (v.has_value() && !v->scalar) {
      v->m = transposeMatrix(v->m);
      v->stored = false;
    }
    return v;
  }
  return evalMxPrimary(e, session, err, joinOperand);
}

// The matrix a right-hand side evaluates to (a scalar as a 1x1), its unnamed
// rows and columns named by position: r1.., c1.., and an `eq:` name from `#`
// gets the position as its name.
std::optional<eval::EvalMatrix> evalMatrixRhs(const std::string& rhs, SessionState& session,
                                              std::string& err) {
  std::optional<MxValue> v = evalMx(rhs, session, err);
  if (!v.has_value()) return std::nullopt;
  eval::EvalMatrix m = mxAsMatrix(*v);
  m.rownames.resize(m.data.size());
  m.colnames.resize(mxCols(m));
  const auto fill = [](std::vector<std::string>& names, char kind) {
    for (size_t k = 0; k < names.size(); ++k)
      if (names[k].empty() || names[k].back() == ':')
        names[k] += std::string(1, kind) + std::to_string(k + 1);
  };
  fill(m.rownames, 'r');
  fill(m.colnames, 'c');
  return m;
}

// Scalars and matrices share one namespace ([P] matrix define, "Namespace"):
// defining the matrix destroys a scalar of the same name.
void storeMatrixResult(SessionState& session, const std::string& name, eval::EvalMatrix mat) {
  session.eval.scalars.erase(name);
  session.eval.stringScalars.erase(name);
  for (auto& p : session.eval.matrices) {
    if (p.first == name) { p.second = std::move(mat); return; }
  }
  session.eval.matrices.emplace_back(name, std::move(mat));
}

// `matrix list`'s cell text: Stata's default %10.0g, trimmed ("10", ".25",
// "3.236e-07", "1.235e+08"; BUG-1411, StataBE 19.5 scratch-laneA/lead/ml/ml.do
// and the w2_smalls.log listings). It was a fixed %.4f with an integer snap, so a
// small variance listed as 0.0000.
std::string formatMatrixCell(double v) {
  if (auto miss = eval::stataMissingToken(v)) return *miss;  // `.a` is .a, not 9e+307 (BUG-1399)
  if (std::isnan(v)) return ".";
  return piet::fmt::general(v, 10, 0);
}

// `matrix list`'s "symmetric" header: every cell equals its mirror (within
// 1e-8). X'X products (A'*A, accum) are exactly symmetric in IEEE754 (same
// summation order, commutative per-term multiply).
bool isSymmetricMatrix(const eval::EvalMatrix& m) {
  const size_t n = m.data.size();
  if (n == 0 || m.data[0].size() != n) return false;
  // A missing cell mirrors only the same missing value (a NaN difference used
  // to compare as "close", so (3,. \ 6,.) listed as symmetric).
  const auto kind = [](double v) {
    return std::isnan(v) ? 0 : eval::stataExtendedMissingKind(v).value_or(0);
  };
  for (size_t i = 0; i < n; ++i) {
    if (m.data[i].size() != n) return false;
    for (size_t j = i + 1; j < n; ++j) {
      const double a = m.data[i][j], b = m.data[j][i];
      if (mxMissing(a) || mxMissing(b)) {
        if (!(mxMissing(a) && mxMissing(b) && kind(a) == kind(b))) return false;
      } else if (std::fabs(a - b) > 1e-8) {
        return false;
      }
    }
  }
  return true;
}

// `matrix list NAME` rendering, line for line as StataBE 19.5 prints it
// (BUG-1412; 2026-11-03 oracle, and w2g_ebmat.log's e(V) / teffects / oaxaca
// listings):
//
//   symmetric S[2,2]                     E[2,3]
//                e:     e:                            y:      y:  lnsig:
//                x  _cons                            x1   _cons   _cons
//       e:x      1                         eq1:a       1       2       3
//   e:_cons      2      5                 eq2:bb       4       5       6
//
// - Every column has ONE width w: the widest printed cell (symmetric -> the
//   lower triangle incl. diagonal), column name, and equation or operator
//   part plus its delimiter. Columns are 2 blanks apart; row labels are
//   right-aligned to the longest.
// - A column name `eq:name` puts eq on a header line of its own, its `:` one
//   place right of the column; `op.name` and `a#b` put the part up to the last
//   `#`, else the last `.`, on the next line the same way (`1b.` over `c`,
//   `c.x#` over `c.x`). A line appears when any column has that part.
// - A name or operator part is cut to 12 characters as abbrev() cuts it
//   (`averyveryl~e`, `1.averyver~x#`), an equation to 11 before its `:`; a
//   row label is `eq:` + its name so cut, or the whole label cut.
// - A factor level's `bn` marker is not printed (`2bn.g` lists as `2.` over
//   `g`, width 3), matrixNameAsListed.
std::string renderMatrixList(const std::string& name, const eval::EvalMatrix& m,
                             const std::function<std::string(double)>& cellText) {
  const size_t nrows = m.data.size();
  const size_t ncols = nrows > 0 ? m.data[0].size() : 0;
  // A 0x0 matrix (`matrix S = e(V)` after `ereturn post` of b alone) is
  // `symmetric S[0,0]` and nothing more.
  const bool empty = nrows == 0 && ncols == 0;
  const bool sym = empty || isSymmetricMatrix(m);
  const auto abbrev = [](const std::string& s, size_t n) {
    return s.size() <= n ? s : s.substr(0, n - 2) + "~" + s.back();
  };

  std::vector<std::string> rlabels = m.rownames;
  rlabels.resize(nrows);
  size_t rowLabelWidth = 0;
  for (size_t i = 0; i < nrows; ++i) {
    std::string& label = rlabels[i];
    if (label.empty()) label = "r" + std::to_string(i + 1);
    label = matrixNameAsListed(label);
    const size_t colon = label.find(':');
    label = colon == std::string::npos
                ? abbrev(label, 12)
                : label.substr(0, colon + 1) + abbrev(label.substr(colon + 1), 12);
    rowLabelWidth = std::max(rowLabelWidth, label.size());
  }
  // Each column's equation, operator and name parts (the first two without
  // their delimiter), cut to their widths.
  std::vector<std::string> ceq(ncols), cop(ncols), cname(ncols);
  std::vector<char> opDelim(ncols, ' ');
  bool anyEq = false, anyOp = false;
  size_t w = 0;
  for (size_t j = 0; j < ncols; ++j) {
    std::string full = j < m.colnames.size() ? matrixNameAsListed(m.colnames[j]) : std::string();
    if (full.empty()) full = "c" + std::to_string(j + 1);
    if (const size_t colon = full.find(':'); colon != std::string::npos) {
      ceq[j] = abbrev(full.substr(0, colon), 11);
      full = full.substr(colon + 1);
      anyEq = true;
      w = std::max(w, ceq[j].size() + 1);
    }
    size_t cut = full.rfind('#');
    if (cut == std::string::npos) cut = full.rfind('.');
    if (cut != std::string::npos && cut > 0 && cut + 1 < full.size()) {
      cop[j] = abbrev(full.substr(0, cut), 12);
      opDelim[j] = full[cut];
      full = full.substr(cut + 1);
      anyOp = true;
      w = std::max(w, cop[j].size() + 1);
    }
    cname[j] = abbrev(full, 12);
    w = std::max(w, cname[j].size());
  }

  std::vector<std::vector<std::string>> cells(nrows, std::vector<std::string>(ncols));
  for (size_t i = 0; i < nrows; ++i) {
    const size_t limit = sym ? std::min(i + 1, ncols) : ncols;  // lower triangle incl. diagonal
    for (size_t j = 0; j < limit; ++j) {
      cells[i][j] = cellText ? cellText(m.data[i][j]) : formatMatrixCell(m.data[i][j]);
      w = std::max(w, cells[i][j].size());
    }
  }

  auto rjust = [](const std::string& s, size_t width) {
    return s.size() >= width ? s : std::string(width - s.size(), ' ') + s;
  };

  std::ostringstream out;
  out << "\n";
  if (sym) out << "symmetric ";
  out << name << "[" << nrows << "," << ncols << "]\n";
  if (empty) return out.str();

  // An equation or operator line: each part right-aligned in its column, its
  // delimiter in the first of the two blanks after it.
  const auto partLine = [&](const std::vector<std::string>& part, const auto& delim) {
    out << std::string(rowLabelWidth + 1, ' ');
    for (size_t j = 0; j < ncols; ++j)
      out << ' ' << rjust(part[j], w) << (part[j].empty() ? ' ' : delim(j));
    out << "\n";
  };
  if (anyEq) partLine(ceq, [](size_t) { return ':'; });
  if (anyOp) partLine(cop, [&opDelim](size_t j) { return opDelim[j]; });
  out << std::string(rowLabelWidth, ' ');
  for (size_t j = 0; j < ncols; ++j) out << "  " << rjust(cname[j], w);
  out << "\n";

  for (size_t i = 0; i < nrows; ++i) {
    const size_t limit = sym ? std::min(i + 1, ncols) : ncols;
    out << rjust(rlabels[i], rowLabelWidth);
    for (size_t j = 0; j < limit; ++j) out << "  " << rjust(cells[i][j], w);
    out << "\n";
  }
  return out.str();
}

// `matrix rownames NAME = n1 n2 ...` / `matrix colnames NAME = n1 n2 ...`
// StataBE 19.5 (BUG-1409, 2026-11-03 oracle): an absent NAME is r(111)
// `matrix NAME not found`; more names than rows/columns is r(503)
// `conformability error`; fewer is accepted, the last row/column named
// naming every one left (`matrix rownames A3 = a b` on a 3-row A3 names them
// a b b). A name is a word or a quoted string, blanks and all (`"a b" c`
// names two rows); an empty one, `""`, keeps that row's name (`"" c` on r1
// r2 r3 is r1 c c), and an empty list changes nothing.
ojson runMatrixStripe(bool isRow, const std::string& name, const std::string& namesRhs,
                       SessionState& session) {
  std::vector<std::string> names;
  for (std::string rest = namesRhs; !trim(rest).empty();) {
    auto [tok, next] = eval::scanOneToken(rest, /*bind=*/true);
    names.push_back(tok);
    rest = std::move(next);
  }
  eval::EvalMatrix* mat = nullptr;
  for (auto& p : session.eval.matrices) {
    if (p.first == name) { mat = &p.second; break; }
  }
  if (mat == nullptr) return engineErrorBlockRc("matrix " + name + " not found", 111);
  const size_t nrows = mat->data.size();
  const size_t ncols = nrows > 0 ? mat->data[0].size() : 0;
  const size_t want = isRow ? nrows : ncols;
  if (names.size() > want) return engineErrorBlockRc("conformability error", 503);
  std::vector<std::string>& stripe = isRow ? mat->rownames : mat->colnames;
  stripe.resize(want);
  for (size_t k = 0; k < want; ++k) {
    if (k < names.size()) {
      if (!names[k].empty()) stripe[k] = names[k];
    } else if (!names.empty()) {
      stripe[k] = stripe[names.size() - 1];
    }
    if (stripe[k].empty()) stripe[k] = std::string(1, isRow ? 'r' : 'c') + std::to_string(k + 1);
  }
  return ojson::array();
}

// `matrix accum NAME = varlist [if] [in] [weight] [, noconstant deviations
// means(M)]` ([P] matrix accum) -- A = X'WX where X = [varlist, 1] (the
// constant column appended LAST, dropped by noconstant), over the observations
// where no listed variable and no weight is missing (a row missing ANY of them
// is dropped entirely -- the rule `regress` uses). Verified against the P4 wave
// fixture: `matrix accum P4C = x1 x2` gives casewise n = 4671 and P4C[1,1] =
// 3896.1577 (internal capture p4_stata.log).
//
// BUG-1408 (StataBE 19.5, 2026-11-03 oracle; it refused every option):
//  - noconstant (noc...) drops the _cons row and column; deviations (d...)
//    replaces the variables' block with sum_i w_i (x_i - m)(x_i - m)', m the
//    weighted means, and keeps the _cons row (sum_i w_i x_i, sum_i w_i);
//    means(M) (m...) stores the 1 x k(+1) row of those means, row _cons, 1 under
//    _cons. Any other option is r(198) `option NAME not allowed`.
//  - Weights: an aweight is rescaled to sum to the number of observations it
//    keeps (an aweight <= 0 drops its row); fweights, pweights and iweights are
//    used as given (a frequency or probability weight <= 0 drops its row, an
//    importance weight never does; a non-integer fweight is r(401)). No row left
//    is r(2000) `no observations`, as is a pweight sum below 1, but iweights
//    that sum to 0 are rc 0; an fweight or pweight sum past 2^31 is r(1400).
//  - The note: `(obs=N)` (N the observations, an fweight's sum, a pweight's sum
//    truncated, an iweight's sum %12.0g), after `(sum of wgt is S)` for
//    aweights (%18.0gc); r(N) is the observations, or the weights' sum for
//    fweights, pweights and iweights, and replaces r().
//  - Bits: a cross product is summed in row order; the aweight rescaling, the
//    means and the _cons row of `deviations` use compensated sums (an aweight
//    _cons cell is the observation count itself). That reproduces 88 of the
//    90 %21x cells of a sweep over four weight variables, aw/pw/iw, with and
//    without deviations; the other two, deviation cells, are 1-2 ulps off.
ojson runMatrixAccum(const std::string& name, const std::string& rhs, SessionState& session) {
  eval::ParsedCommand parsed;
  try {
    parsed = eval::tokenize("matrix accum " + name + " = " + rhs);
  } catch (const eval::TokenizerError& te) {
    return ojson(te.block);
  }
  std::vector<std::string> vars;
  {
    const auto eq = std::find(parsed.varlist.begin(), parsed.varlist.end(), "=");
    if (eq != parsed.varlist.end()) vars.assign(eq + 1, parsed.varlist.end());
  }
  if (vars.empty()) return engineErrorBlock("matrix accum: no variables specified");
  // Factor-variable and time-series operators (`i.g`, `L.x`, `c.x#c.z`) build
  // columns of their own, which this handler does not: refused by name rather
  // than looked up as a variable called `i.g`.
  for (const auto& v : vars) {
    if (v.find_first_of(".#") != std::string::npos)
      return engineErrorBlockRc("matrix accum: factor-variable and time-series operators (" + v +
                                    ") are not supported in this version of PIET",
                                198);
  }
  // The varlist as every Stata varlist reads: `x*`, `xa-longname` and a unique
  // abbreviation (`longn`) name the variables they stand for.
  if (auto bad = resolveByKeyNames(session, vars)) return *bad;
  bool noConstant = false, deviations = false;
  std::string meansName;
  for (const auto& [key, val] : parsed.opts) {
    const auto abbrevOf = [&key](const std::string& full, size_t shortest) {
      return key.size() >= shortest && full.compare(0, key.size(), key) == 0;
    };
    if (abbrevOf("noconstant", 3) && val.isBool) {
      noConstant = true;
    } else if (abbrevOf("deviations", 1) && val.isBool) {
      deviations = true;
    } else if (abbrevOf("means", 1) && !val.isBool) {
      meansName = trim(val.strValue);
      if (!meansName.empty() && !strutil::isValidStataIdentifier(meansName))
        return engineErrorBlockRc(meansName + " invalid name", 198);
    } else {
      return engineErrorBlockRc("option " + key + " not allowed", 198);
    }
  }
  std::string wType;
  std::string wVar = parsed.weightVar.value_or(std::string());
  if (parsed.weightType.has_value()) {
    wType = parsed.weightType->substr(0, 2);
    for (auto& ch : wType) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }

  if (!session.currentHbin().has_value() || session.datastore() == nullptr)
    return engineErrorBlock("no data in memory");
  // The weight is an expression, as in every Stata weight: a name is the
  // variable or its unique abbreviation (`[fw=nosuch]` and `[fw=nosuch*2]` are
  // r(111) `nosuch not found`), a string is r(109) `type mismatch` (`[fw=s]`,
  // `[fw=s+"b"]`), and anything else (`[fw=xa*2]`, `[aw=1/xa]`) is generated
  // over the whole frame into a hidden column before if/in select rows; the
  // column leaves the frame when the handler returns.
  struct WeightColumnGuard {
    SessionState& session;
    std::string column;
    ~WeightColumnGuard() {
      if (column.empty()) return;
      try {
        strmutate::removeConditionMask(session, column);
      } catch (...) {
      }
    }
  } weightColumn{session, {}};
  if (!wVar.empty()) {
    const std::vector<std::string> known = session.datastore()->names();
    const auto resolveName = [&known](const std::string& nm, std::string& out) -> std::optional<ojson> {
      if (std::find(known.begin(), known.end(), nm) != known.end()) {
        out = nm;
        return std::nullopt;
      }
      std::vector<std::string> hits;
      for (const auto& kn : known)
        if (kn.compare(0, nm.size(), nm) == 0) hits.push_back(kn);
      if (hits.size() != 1)
        return engineErrorBlockRc(hits.empty() ? nm + " not found" : nm + " ambiguous abbreviation",
                                  111);
      out = hits.front();
      return std::nullopt;
    };
    if (strutil::isValidStataIdentifier(wVar)) {
      if (auto bad = resolveName(wVar, wVar)) return *bad;
    } else {
      // Every name the expression reads must answer to a variable first.
      static const std::regex kName(R"([A-Za-z_][A-Za-z0-9_]*)");
      const std::string wexp = wVar;
      for (std::sregex_iterator it(wexp.begin(), wexp.end(), kName), end; it != end; ++it) {
        const size_t at = static_cast<size_t>(it->position());
        const size_t after = at + static_cast<size_t>(it->length());
        if (at > 0 && (std::isalnum(static_cast<unsigned char>(wexp[at - 1])) ||
                       wexp[at - 1] == '.' || wexp[at - 1] == '"'))
          continue;  // a number's exponent, a missing value's letter, a string
        if (after < wexp.size() && wexp[after] == '(') continue;  // a function
        std::string resolved;
        if (auto bad = resolveName(it->str(), resolved)) return *bad;
      }
      const strmutate::ConditionMaskInstall wi = strmutate::installWeightExpression(session, wexp);
      if (!wi.ok) return engineErrorBlockRc("type mismatch", 109);
      weightColumn.column = wi.column;
      wVar = wi.column;
    }
    Datastore* wds = session.datastore();
    if (wds == nullptr || !wds->has(wVar)) return engineErrorBlock("no data in memory");
    if (wds->dtype(wVar) != "f64") return engineErrorBlockRc("type mismatch", 109);
  }

  const std::optional<std::string> headPath = session.currentHbin();
  if (!headPath.has_value() || session.datastore() == nullptr)
    return engineErrorBlock("no data in memory");
  std::unique_ptr<Datastore> filtered;
  Datastore* ds = session.datastore();
  if (parsed.ifExpr.has_value() || parsed.inRange.has_value()) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, *headPath, parsed.ifExpr, parsed.inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, parsed.inRange.value_or(std::string()), "matrix accum");
    filtered = std::make_unique<Datastore>(fr.dataPath);
    ds = filtered.get();
  }
  // A string variable is missing in every observation, so StataBE's answer
  // is r(2000) `no observations` (`matrix accum A = xa s`), not a type error:
  // its column is null and drops every row below.
  std::vector<const double*> cols;
  cols.reserve(vars.size());
  for (const auto& v : vars) {
    if (!ds->has(v)) return engineErrorBlockRc("variable " + v + " not found", 111);
    cols.push_back(ds->dtype(v) == "f64" ? ds->col(v).f64() : nullptr);
  }
  const double* wcol = nullptr;
  if (!wVar.empty()) {
    wcol = ds->col(wVar).f64();
  }
  const int64_t n = ds->nRows();
  const size_t k = vars.size();
  std::vector<std::vector<double>> X;  // the rows kept, [v1..vk]
  std::vector<double> W;
  X.reserve(static_cast<size_t>(std::max<int64_t>(n, 0)));
  for (int64_t r = 0; r < n; ++r) {
    bool keep = true;
    for (size_t c = 0; c < k; ++c) {
      if (cols[c] == nullptr || cecon::is_missing(cols[c][r])) { keep = false; break; }
    }
    if (keep && wcol != nullptr) {
      const double w = wcol[r];
      keep = !cecon::is_missing(w) && (wType == "iw" || w > 0.0);
      if (keep && wType == "fw" && w != std::floor(w))
        return engineErrorBlockRc("may not use noninteger frequency weights", 401);
    }
    if (!keep) continue;
    std::vector<double> row(k);
    for (size_t c = 0; c < k; ++c) row[c] = cols[c][r];
    X.push_back(std::move(row));
    W.push_back(wcol != nullptr ? wcol[r] : 1.0);
  }
  if (X.empty()) return engineErrorBlockRc("no observations", 2000);

  // r(N) of a weighted sum is the plain running sum (`return list` after
  // `[iw=w]`, w = 1/3+_n, n = 5: 16.66666666666666).
  double plainSum = 0.0;
  for (const double w : W) plainSum += w;
  // A frequency or probability weight's sum is a count of observations: one
  // that truncates to 0 is r(2000) (`[pw=w]`, w = 1e-5*_n), one past a 4-byte
  // integer r(1400) (`[fw=w]`, w = 1e9 on 5 rows).
  if (wType == "fw" || wType == "pw") {
    if (std::trunc(plainSum) > 2147483647.0)
      return engineErrorBlockRc("numerical overflow", 1400);
    if (std::trunc(plainSum) < 1.0) return engineErrorBlockRc("no observations", 2000);
  }

  // Neumaier's compensated sum.
  const auto accurateSum = [](const std::vector<double>& v) {
    double s = 0.0, comp = 0.0;
    for (const double x : v) {
      const double t = s + x;
      comp += std::fabs(s) >= std::fabs(x) ? (s - t) + x : (x - t) + s;
      s = t;
    }
    return s + comp;
  };
  const double nObs = static_cast<double>(X.size());
  const double rawSum = accurateSum(W);
  if (wType == "aw") {
    const double scale = nObs / rawSum;
    for (double& w : W) w *= scale;
  }
  const double wTotal = accurateSum(W);
  const double sumW = wType == "aw" ? nObs : wTotal;
  std::vector<double> mean(k, 0.0);
  for (size_t c = 0; c < k; ++c) {
    std::vector<double> terms(X.size());
    for (size_t r = 0; r < X.size(); ++r) terms[r] = X[r][c] * W[r];
    mean[c] = accurateSum(terms) / wTotal;
  }

  const size_t dim = noConstant ? k : k + 1;
  eval::EvalMatrix mat;
  mat.data.assign(dim, std::vector<double>(dim, 0.0));
  std::vector<double> x(k + 1, 1.0);  // one row, the constant last
  for (size_t r = 0; r < X.size(); ++r) {
    for (size_t c = 0; c < k; ++c) x[c] = deviations ? X[r][c] - mean[c] : X[r][c];
    const size_t cross = deviations ? k : dim;
    for (size_t i = 0; i < cross; ++i) {
      const double xi = x[i] * W[r];
      if (xi == 0.0) continue;
      for (size_t j = 0; j < cross; ++j) mat.data[i][j] += xi * x[j];
    }
  }
  if (!noConstant) {
    for (size_t c = 0; c < k && deviations; ++c) mat.data[k][c] = mat.data[c][k] = mean[c] * sumW;
    if (deviations || wType == "aw") mat.data[k][k] = sumW;
  }
  mat.rownames = vars;
  if (!noConstant) mat.rownames.push_back("_cons");
  mat.colnames = mat.rownames;
  storeMatrixResult(session, name, std::move(mat));
  if (!meansName.empty()) {
    eval::EvalMatrix means;
    means.data.assign(1, mean);
    if (!noConstant) means.data[0].push_back(1.0);
    means.rownames = {"_cons"};
    means.colnames = vars;
    if (!noConstant) means.colnames.push_back("_cons");
    storeMatrixResult(session, meansName, std::move(means));
  }

  const double rN = wType.empty() || wType == "aw" ? nObs : plainSum;
  eval::replaceRClass(session.eval);
  session.eval.rValues.set("N", rN);
  eval::declareRPosted(session.eval, "matrix accum", {"N"});
  const auto format = [](double v, int width, bool commas) {
    fmt::StataFmtSpec sp;
    sp.hasWidth = true;
    sp.width = width;
    sp.conv = 'g';
    sp.groupThousands = commas;
    const std::optional<std::string> s = fmt::renderValue(v, sp);
    return s.has_value() ? trim(*s) : eval::pythonFloatRepr(v);
  };
  // One block (the dispatcher arm pushes what this returns as one block).
  std::string text;
  if (wType == "aw") text = "(sum of wgt is " + format(rawSum, 18, true) + ")\n";
  const double shown = wType == "pw" ? std::trunc(rawSum) : rN;
  text += "(obs=" + (wType == "iw" ? format(shown, 12, false) : format(shown, 18, true)) + ")";
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  return out;
}

}  // namespace

}  // namespace piet::handlers
