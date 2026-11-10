// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Matrix commands.

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

// `matrix A[r,c] = exp`: overwrite one cell of an EXISTING matrix with a scalar
// expression, or, when exp is a matrix, the block it covers starting at (r,c)
// (Stata's submatrix substitution). The target must exist (r(111)); a subscript
// or block that falls outside it is Stata's conformability error r(503)
// (oracle w2c_functions.log:151 matassign_oob); a string result is r(109).
//
// `subText` is the whole bracket body, found depth-aware by runMatrixDef, so
// `A[max(1,2),3]` and the 073 corpus's expanded `H[2*3+2+1,4]` arrive intact.
// Each subscript is a scalar expression (evalMatrixScalarExpr, read as StataBE
// reads a target's subscript: a variable is its first observation), rounded to
// the nearest integer, halves up, and range-checked as a double BEFORE any
// cast: NaN, a missing value, or a rounded value < 1 or above the dimension is
// r(503). StataBE 19.5 (w2q_matrix_edges A05, 2026-11-03 oracle): `A[1.7,2]`
// and `A[1.5,1]` write row 2, `A[2.5,1]` row 3, `A[0.4,1]` and, on a 4-row A,
// `A[4.5,1]` are r(503) -- a target rounds where a read (mxElementAt)
// truncates. Row/column NAMES as subscripts are not implemented and refuse
// through the same evaluator. Every refusal carries an exact rc, never rc 1,
// and StataBE's bare message.
ojson runMatrixElementAssign(const std::string& name, const std::string& subText,
                             const std::string& rhs, SessionState& session) {
  eval::EvalMatrix* target = nullptr;
  for (auto& p : session.eval.matrices) {
    if (p.first == name) { target = &p.second; break; }
  }
  if (target == nullptr) return engineErrorBlockRc("matrix " + name + " not found", 111);
  std::vector<std::string> subParts;
  std::vector<char> subOps;
  if (!splitTopLevel(subText, subParts, subOps) || subParts.size() != 2 || subOps[0] != ',' ||
      subParts[0].empty() || subParts[1].empty()) {
    return engineErrorBlockRc("invalid syntax", 198);
  }
  const size_t dims[2] = {target->data.size(),
                          target->data.empty() ? size_t{0} : target->data[0].size()};
  size_t idx[2] = {0, 0};
  for (int k = 0; k < 2; ++k) {
    double v = 0.0;
    std::string subErr;
    if (!evalMatrixScalarExpr(subParts[static_cast<size_t>(k)], session, v, subErr, true))
      return matrixExprErrorBlock(subErr);
    if (std::isnan(v) || v >= eval::kStataMissingSentinel)
      return engineErrorBlockRc("conformability error", 503);
    v = std::floor(v + 0.5);
    if (v < 1.0 || v > static_cast<double>(dims[k]))
      return engineErrorBlockRc("conformability error", 503);
    idx[k] = static_cast<size_t>(v);
  }
  const size_t r = idx[0], c = idx[1];
  // A scalar exp is one cell; a matrix-valued one (`-A`, `2*A[1,1..2]`,
  // `MN["rb",1]`) is Stata's block substitution. A refusal keeps its
  // classified rc (an unknown function r(133)), never rc 1.
  eval::EvalMatrix block;
  {
    std::string rhsErr;
    std::optional<MxValue> value = evalMx(rhs, session, rhsErr);
    if (!value.has_value()) return matrixExprErrorBlock(rhsErr);
    block = mxAsMatrix(*value);
  }
  // Re-find: evaluation does not add matrices today, but the pointer is into
  // a vector the evaluation could in principle grow.
  target = nullptr;
  for (auto& p : session.eval.matrices) {
    if (p.first == name) { target = &p.second; break; }
  }
  if (target == nullptr) return engineErrorBlockRc("matrix " + name + " not found", 111);
  const size_t rows = target->data.size();
  const size_t cols = rows > 0 ? target->data[0].size() : 0;
  const size_t br = block.data.size();
  const size_t bc = br > 0 ? block.data[0].size() : 0;
  if (br == 0 || bc == 0 || r - 1 + br > rows || c - 1 + bc > cols) {
    return engineErrorBlockRc("conformability error", 503);
  }
  for (size_t i = 0; i < br; ++i)
    for (size_t j = 0; j < bc; ++j) target->data[r - 1 + i][c - 1 + j] = block.data[i][j];
  return ojson::array();  // matrix define is silent
}

// `matrix input NAME = (# [,#...] [\ #[,#...] ...])` ([P] matrix define,
// "Inputting matrices by hand"): numbers only, a row's elements separated by
// commas or blanks, the rows by `\`, named r1.. and c1... StataBE 19.5
// (2026-11-03 oracle): an element is a decimal number with an optional sign,
// `.` or `.a`-`.z` (`1+1`, a name, `0x1` are r(198)); a comma before a row's
// first element or after its last is ignored, two with no element between are
// r(198); rows of unequal length are r(503); `()` is a 1x0 matrix; text after
// the closing parenthesis is ignored, and no parentheses is r(198).
ojson runMatrixInput(const std::string& name, const std::string& rhs, SessionState& session) {
  const std::string s = trim(rhs);
  const size_t close = s.find(')');
  if (s.empty() || s[0] != '(' || close == std::string::npos)
    return engineErrorBlockRc("invalid syntax", 198);
  const std::string body = s.substr(1, close - 1);
  static const std::regex reNumber(R"(^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$)");
  eval::EvalMatrix m;
  size_t from = 0;
  for (size_t at = 0; at <= body.size(); ++at) {
    if (at < body.size() && body[at] != '\\') continue;
    const std::string rowText = body.substr(from, at - from);
    from = at + 1;
    std::vector<double> row;
    bool comma = false;  // a comma since the last element
    size_t i = 0;
    while (i < rowText.size()) {
      const char c = rowText[i];
      if (c == ' ' || c == '\t') {
        ++i;
        continue;
      }
      if (c == ',') {
        if (comma) return engineErrorBlockRc("invalid syntax", 198);
        comma = true;
        ++i;
        continue;
      }
      size_t j = i;
      while (j < rowText.size() && rowText[j] != ' ' && rowText[j] != '\t' && rowText[j] != ',') ++j;
      const std::string tok = rowText.substr(i, j - i);
      if (tok == ".") {
        row.push_back(std::numeric_limits<double>::quiet_NaN());
      } else if (tok.size() == 2 && tok[0] == '.' && tok[1] >= 'a' && tok[1] <= 'z') {
        row.push_back(eval::stataMissingWithKind(tok[1] - 'a' + 1));
      } else if (std::regex_match(tok, reNumber)) {
        try {
          row.push_back(std::stod(tok));
        } catch (const std::exception&) {
          return engineErrorBlockRc("invalid syntax", 198);
        }
      } else {
        return engineErrorBlockRc("invalid syntax", 198);
      }
      comma = false;
      i = j;
    }
    if (!m.data.empty() && row.size() != m.data[0].size())
      return engineErrorBlockRc("conformability error", 503);
    m.data.push_back(std::move(row));
  }
  applyDefaultStripe(m);
  storeMatrixResult(session, name, std::move(m));
  return ojson::array();
}

}  // namespace

// The `matrix` command: e(b)/e(V) capture (the original M3 path below, tried
// FIRST so its output can never shift), `matrix define` with the whole matrix
// expression grammar (evalMx), `matrix input`, element assignment, drop, list,
// rownames/colnames and accum. Everything downstream (`local : colnames b`,
// `b[1,j]`, `scalar = sqrt(V[j,j])`) already resolves against
// session.eval.matrices, so each form here only has to populate/read that one
// store. Any other matrix form is an explicit error, never a silent no-op
// (Doctrine 1): a Stata matrix subcommand this version lacks is named as
// such, and a word that is none is StataBE's r(501).
// The name a matrix is CREATED under follows the one strict rule (strutil.h ->
// core/ident.h). The patterns above take any byte >= 0x80 as a name byte so a
// Unicode name stays whole; this is where `matrix ١ = (1,2)` / `matrix €M = ...`
// are refused, as StataBE refuses them: "١ invalid name", r(198)
// (scratch-r150/uni-d/badnames.log).
static std::optional<ojson> refuseInvalidMatrixName(const std::string& name) {
  if (strutil::isValidStataIdentifier(name)) return std::nullopt;
  return engineErrorBlockRc(name + " invalid name", 198);
}

ojson runMatrixDef(const std::string& raw, SessionState& session) {
  // BUG-543: `matrix define NAME = ...` / `mat def NAME = ...` — `define`/
  // `def` is an OPTIONAL keyword before an assignment target in real Stata
  // (59 corpus lines spell it), but a matrix can be literally NAMED `def` or
  // `define` (`matrix define = ...`), so the keyword is stripped only when a
  // SECOND identifier and `=` follow it — never when `=` sits directly after
  // `def(ine)?` itself. An element target follows it too: `matrix define
  // A[3,3] = 4` is `matrix A[3,3] = 4` (w2q_matrix_edges A10, StataBE rc 0),
  // while `matrix define[1,1] = 5` writes the matrix named define.
  // Unicode matrix names (core/ident.h): eval::kRxName is the name in every
  // pattern below, so `matrix قيم = (1,2)` names one matrix.
  static const std::regex reDefKeyword(std::string(R"(^(\s*mat(?:rix)?\s+)def(?:i(?:n(?:e)?)?)?(\s+)") +
                                       eval::kRxName + R"(\s*[=\[].*)$)");
  std::string work = raw;
  {
    std::smatch km;
    if (std::regex_search(raw, km, reDefKeyword)) work = km.str(1) + km.str(2);
  }
  static const std::regex reMat(std::string(R"(^\s*mat(?:rix)?\s+()") + eval::kRxName +
                                R"()\s*=\s*([er])\(\s*()" + eval::kRxName +
                                R"()\s*\)\s*$)");
  std::smatch m;
  if (!std::regex_search(work, m, reMat)) {
    // Not the e(b)/e(V) form — try the P4 sublanguage before giving up.
    // eb-order T-P1: the matrix is a bare name or a class matrix (`matrix list
    // e(b)` is rc 0 after a fit, w2g_ebmat.log:409 e1_list_eb, :495, :1909,
    // :2020), with Stata's format() option (`matrix list V, format(%21x)`,
    // w2g_ebmat.log:216 e1_list_V).
    static const std::regex reList(std::string(R"(^\s*mat(?:rix)?\s+l(?:i(?:s(?:t)?)?)?\s+()") +
                                   eval::kRxName + R"(|[er]\(\s*)" + eval::kRxName +
                                   R"(\s*\))\s*(?:,\s*(.*?))?\s*$)");
    static const std::regex reStripe(std::string(R"(^\s*mat(?:rix)?\s+(rown|coln)(?:a(?:m(?:e(?:s)?)?)?)?\s+()") +
                                     eval::kRxName + R"()\s*=\s*(.*)$)");
    static const std::regex reAccum(std::string(R"(^\s*mat(?:rix)?\s+ac(?:c(?:u(?:m)?)?)?\s+()") +
                                    eval::kRxName + R"()\s*=\s*(.+)$)");
    static const std::regex reDefine(std::string(R"(^\s*mat(?:rix)?\s+()") + eval::kRxName +
                                     R"()\s*=\s*(.+)$)");
    // ml-getmata T4: element (and block) assignment, `matrix A[r,c] = exp`.
    // Oracle w2c_ml_getmata.do:312-315 fills a J(3,3,.) this way before family
    // (9)'s Mata block, and paper 087 fills all_results cell by cell the same
    // way (gen_sensitivity_stats_1.do). The bracket body is found depth-aware
    // (its matching `]`), so a subscript or right-hand side with its own
    // brackets/commas -- `A[max(1,2),3] = 6`, `H[2,3] = H[3,2]` -- splits at
    // the right place; a bracket with no `= exp` after it is r(198).
    static const std::regex reElemHead(std::string(R"(^\s*mat(?:rix)?\s+()") + eval::kRxName +
                                       R"()\s*\[)");
    std::smatch mm;

    if (std::regex_search(work, mm, reElemHead)) {
      const size_t open = static_cast<size_t>(mm.length(0)) - 1;
      int depth = 0;
      size_t close = std::string::npos;
      for (size_t i = open; i < work.size(); ++i) {
        const char c = work[i];
        if (c == '(' || c == '[') ++depth;
        else if (c == ')' || c == ']') {
          if (--depth == 0) { close = i; break; }
        }
      }
      static const std::regex reAssignTail(R"(^\s*=\s*(.+)$)");
      std::smatch tm;
      const std::string tail = close == std::string::npos ? std::string() : work.substr(close + 1);
      if (close == std::string::npos || !std::regex_match(tail, tm, reAssignTail)) {
        return engineErrorBlockRc("invalid syntax", 198);
      }
      return runMatrixElementAssign(mm.str(1), work.substr(open + 1, close - open - 1),
                                    trim(tm.str(1)), session);
    }

    // `matrix drop NAMES` / `matrix drop _all` ([P] matrix utility). It was not
    // wired at all: every `capture matrix drop b` was a silent rc-1 no-op, so
    // the matrix it meant to remove survived into the next record
    // (w2v_estpost_ematrix.do ep15f: `capture matrix drop b` before `matrix b =
    // e(b)` with no e(); Stata leaves no b, ep15f_b_shape r(111), where PIET
    // still held the previous section's b). A name that is not a matrix is
    // refused at r(111) before anything is dropped -- design decision: that rc
    // is Stata's "matrix not found" code everywhere else (no capture pins this
    // form; the goldens all run it under `capture`).
    static const std::regex reDrop(R"(^\s*mat(?:rix)?\s+drop\s+(.+?)\s*$)");
    if (std::regex_search(work, mm, reDrop)) {
      std::vector<std::string> names;
      {
        std::istringstream iss(mm.str(1));
        for (std::string w; iss >> w;) names.push_back(w);
      }
      const auto silentEcho = []() {
        ojson out;
        out["type"] = "echo";
        out["text"] = "";
        return out;
      };
      auto& store = session.eval.matrices;
      if (names.size() == 1 && names[0] == "_all") {
        store.clear();
        return silentEcho();
      }
      for (const auto& n : names) {
        const bool found = std::any_of(store.begin(), store.end(),
                                       [&n](const auto& p) { return p.first == n; });
        if (!found) return engineErrorBlockRc("matrix " + n + " not found", 111);
      }
      for (const auto& n : names)
        store.erase(std::remove_if(store.begin(), store.end(),
                                   [&n](const auto& p) { return p.first == n; }),
                    store.end());
      return silentEcho();
    }

    if (std::regex_search(work, mm, reList)) {
      std::string name = mm.str(1);
      std::function<std::string(double)> cellText;
      const std::string opts = trim(mm.str(2));
      if (!opts.empty()) {
        // format(%fmt) is the one option this renderer implements; any other
        // is refused by name rather than accepted and ignored.
        static const std::regex reFmtOpt(R"(^f(?:o(?:r(?:m(?:a(?:t)?)?)?)?)?\(\s*(%[^)\s]+)\s*\)$)");
        std::smatch fm;
        if (!std::regex_match(opts, fm, reFmtOpt))
          // Stata's `matrix list` has noblank/nohalf/noheader/nonames/title()
          // ([P] matrix utility); this version renders only format() (the one
          // option any golden uses), so the rest are unsupported, not "not
          // allowed" (eb-order DIAGNOSIS ruling 4d).
          return ojson(eval::unsupportedOption(opts, "", std::nullopt, std::string("matrix list"),
                                               std::string("r(198)")));
        const std::string f = fm.str(1);
        static const std::regex reHex(R"(^%-?21x$)");
        static const std::regex reNum(R"(^%(-)?(0)?([0-9]+)?(?:\.([0-9]+))?([fgeFGE])(c)?$)");
        std::smatch nm;
        if (std::regex_match(f, reHex)) {
          cellText = [](double v) -> std::string {
            return std::isnan(v) ? std::string(".") : SessionState::stataHex21(v);
          };
        } else if (std::regex_match(f, nm, reNum)) {
          fmt::StataFmtSpec sp;
          sp.leftJustify = nm[1].matched;
          sp.zeroFill = nm[2].matched;
          sp.hasWidth = nm[3].matched;
          if (sp.hasWidth) sp.width = std::stoi(nm.str(3));
          sp.prec = nm[4].matched ? std::stoi(nm.str(4)) : 0;
          sp.conv = static_cast<char>(std::tolower(static_cast<unsigned char>(nm.str(5)[0])));
          sp.groupThousands = nm[6].matched;
          cellText = [sp](double v) -> std::string {
            if (auto miss = eval::stataMissingToken(v)) return *miss;
            const auto r = fmt::renderValue(v, sp);
            return r.has_value() ? trim(*r) : formatMatrixCell(v);
          };
        } else {
          return engineErrorBlockRc("matrix list: invalid format " + f, 120);
        }
      }
      std::optional<eval::EvalMatrix> classMat;
      const eval::EvalMatrix* mat = nullptr;
      if (name.find('(') != std::string::npos) {
        const size_t open = name.find('(');
        const char cls = name[0];
        const std::string which = trim(name.substr(open + 1, name.size() - open - 2));
        name = std::string(1, cls) + "(" + which + ")";
        std::string classErr;
        int classRc = 111;
        classMat = resolveClassMatrixRef(cls, which, session, classErr, &classRc);
        // `matrix list e(b)` with no e(b) is r(111) "matrix e(b) not found"
        // whether an e() is live or not (w2v_estpost_ematrix.log ep13_list_eb,
        // ep15f_list_eb; w2g_ebmat.log:2486 e15d_list_eb2) -- not the r(301) of
        // `matrix b = e(b)` in a fresh session.
        if (!classMat.has_value() && classRc == 301)
          return engineErrorBlockRc("matrix " + name + " not found", 111);
        if (!classMat.has_value()) return engineErrorBlockRc(classErr, classRc);
        mat = &*classMat;
      } else {
        for (const auto& p : session.eval.matrices) {
          if (p.first == name) { mat = &p.second; break; }
        }
        // Stata: r(111) "matrix NOSUCH not found" (w2g_ebmat.log:2577
        // s1_matrix_list_nosuch); this was the code-less rc 1.
        if (mat == nullptr) return engineErrorBlockRc("matrix " + name + " not found", 111);
      }
      ojson block;
      block["type"] = "display";  // output, not the command echo (BUG-1505, runEreturnList)
      block["text"] = renderMatrixList(name, *mat, cellText);
      return block;
    }
    if (std::regex_search(work, mm, reStripe)) {
      return runMatrixStripe(mm.str(1) == "rown", mm.str(2), mm.str(3), session);
    }
    if (std::regex_search(work, mm, reAccum)) {
      if (auto bad = refuseInvalidMatrixName(mm.str(1))) return *bad;
      return runMatrixAccum(mm.str(1), mm.str(2), session);
    }
    if (std::regex_search(work, mm, reDefine)) {
      const std::string name = mm.str(1);
      if (auto bad = refuseInvalidMatrixName(name)) return *bad;
      const std::string rhs = trim(mm.str(2));
      // The whole matrix expression grammar (evalMx): literals, joins, the
      // operators, the functions, subscripts, e()/r() matrices, and a scalar
      // expression as a 1x1 (`matrix A = 5`, `matrix B = .`, and `matrix T =
      // r(nosuch)`, an absent r() being `.`: w2f_residuals.log:1675
      // e_matrix_nosuch rc 0, w2n_followups.log:938 E05 rc 0). Every refusal
      // carries its Stata rc (BUG-1398: an unsupported shape was rc 1). An
      // absent nullmat() on its own is r(111) (w2x_fn_string_math
      // fx_nullmat_bare: `matrix NX = nullmat(NOSUCH2)`).
      std::string cerr;
      std::optional<eval::EvalMatrix> built = evalMatrixRhs(rhs, session, cerr);
      if (!built.has_value()) return matrixExprErrorBlock(cerr);
      storeMatrixResult(session, name, std::move(*built));
      return ojson::array();
    }

    static const std::regex reInput(R"(^\s*mat(?:rix)?\s+in(?:p(?:u(?:t)?)?)?\s+([^\s=]+)\s*=\s*(.*)$)");
    if (std::regex_search(work, mm, reInput)) {
      if (auto bad = refuseInvalidMatrixName(mm.str(1))) return *bad;
      return runMatrixInput(mm.str(1), mm.str(2), session);
    }

    // A word that abbreviates one of [P] matrix's subcommands, down to that
    // subcommand's shortest abbreviation (StataBE 19.5, 2026-11-03 oracle:
    // `matrix d` is dir, `matrix de`, `dr`, `ro` and `re` are none). The stripe
    // commands look their matrix up first (`matrix rown Zq` and `matrix rowe
    // Zq` are r(111) `matrix Zq not found`, `matrix rown A` r(198)); `matrix
    // list` with no name is r(100) `matrix name required`; any other served
    // one that reached here is malformed, r(198), as is one this version
    // lacks without its syntax's `=` ([P] matrix: `matrix svd Zq`), `matrix
    // dir` with an argument and `matrix rename` without its two names. One
    // this version lacks is otherwise named as such. Any other word is r(501)
    // `matrix operation not found` (`matrix nosuchsub Q = 1`, `matrix A B`,
    // `matrix`).
    static const std::regex reSubcmd(R"(^\s*mat(?:rix)?(?:\s+([A-Za-z_]\w*))?\s*(.*)$)");
    std::smatch sm;
    std::regex_search(work, sm, reSubcmd);
    const std::string sub = sm[1].matched ? sm.str(1) : std::string();
    const std::string args = trim(sm.str(2));
    struct Subcommand {
      const char* full;
      size_t shortest;
      bool served;
    };
    static const Subcommand kSubcommands[] = {
        {"accum", 2, true},         {"colnames", 4, true},       {"define", 3, true},
        {"drop", 4, true},          {"input", 2, true},          {"list", 1, true},
        {"rownames", 4, true},      {"coleq", 4, false},         {"coljoinbyname", 7, false},
        {"dir", 1, false},          {"dissimilarity", 3, false}, {"eigenvalues", 8, false},
        {"glsaccum", 4, false},     {"opaccum", 7, false},       {"rename", 3, false},
        {"roweq", 4, false},        {"rowjoinbyname", 7, false}, {"score", 3, false},
        {"svd", 3, false},          {"symeigen", 4, false},      {"vecaccum", 4, false}};
    const Subcommand* hit = nullptr;
    for (const auto& s : kSubcommands)
      if (sub.size() >= s.shortest && std::string(s.full).compare(0, sub.size(), sub) == 0) hit = &s;
    if (hit == nullptr) return engineErrorBlockRc("matrix operation not found", 501);
    const std::string full = hit->full;
    const bool stripe =
        full == "rownames" || full == "colnames" || full == "roweq" || full == "coleq";
    if (stripe) {
      const std::string target = args.substr(0, args.find_first_of(" \t="));
      if (!target.empty() && session.eval.findMatrix(target) == nullptr)
        return engineErrorBlockRc("matrix " + target + " not found", 111);
    }
    if (full == "list" && args.empty())
      return engineErrorBlockRc("matrix name required", 100);
    size_t words = 0;
    {
      std::istringstream iss(args);
      for (std::string w; iss >> w;) ++words;
    }
    const bool malformed = hit->served || (full == "dir"      ? words != 0
                                           : full == "rename" ? words != 2
                                                              : args.find('=') == std::string::npos);
    if (malformed) return engineErrorBlockRc("invalid syntax", 198);
    return ojson(eval::unsupportedCommand("matrix " + full, std::nullopt, std::nullopt,
                                           "piet_gap",
                                           eval::UnsupportedCommandKind::kNotImplemented));
  }
  const std::string name = m.str(1);
  if (auto bad = refuseInvalidMatrixName(name)) return *bad;
  const char cls = m.str(2)[0];        // 'e' or 'r'
  const std::string which = m.str(3);  // was "b"/"V" only; now any posted name

  // One resolver (resolveClassMatrixRef), shared with vecdiag()'s argument.
  // An absent name keeps today's refusal text and rc (the absent-r() content
  // is a separate task, FIX-SPEC section 8).
  // eb-order T-P3/T-P1: the refusal carries the resolver's rc through the
  // errors.h builder -- r(301) in a fresh session (w2g_ebmat.log:2229
  // e15c_fresh_matrix_b), r(111) for a name the live e() does not have -- never
  // the code-less rc 1 it had.
  std::string classErr;
  int classRc = 111;
  std::optional<eval::EvalMatrix> resolved =
      resolveClassMatrixRef(cls, which, session, classErr, &classRc);
  // A live e() with no e(b)/e(V) (pca, rddensity, estpost): rc 0 and a 0x0
  // matrix (w2v_estpost_ematrix.log ep13_b_shape / ep15c_b_shape `0|0|0||`,
  // w2g_ebmat.log:2087 e15c_matrix_b), never the fit before it.
  if (!resolved.has_value() && cls == 'e' && session.eval.eMatrixAbsentFromLiveE(which))
    resolved = eval::EvalMatrix{};
  // A posting with no fit behind it -- `ereturn post`, binscatter's own
  // posting -- is an e() PIET holds whole: every name it has is posted here.
  // Its absent e(b)/e(V) is the 0x0 matrix (StataBE 19.5: `ereturn post bb`
  // with no V, then `matrix S5 = e(V)` lists `symmetric S5[0,0]`, rc 0).
  const bool postingOnly = cls == 'e' && session.ePostedLive &&
                           !session.lastModel().has_value() && !session.lastEstBlock.has_value();
  if (!resolved.has_value() && postingOnly && (which == "b" || which == "V") &&
      session.eval.findClassMatrix('e', which) == nullptr)
    resolved = eval::EvalMatrix{};
  // gfx-binscatter: any other e(NAME) that is not a matrix is read as the
  // scalar expression it is, so `matrix M = e(NAME)` is 1 x 1, row r1, column
  // c1: the e() scalar's value, or missing when e() holds no NAME at all.
  // binscatter's line(none)/line(connect) post no e(y1_coefs), and Stata's
  // `matrix M = e(y1_coefs)` is that 1 x 1 missing at rc 0
  // (w2k_binscatter.log:863-864 BS_line_none_c1, :940-941 BS_line_connect_c1).
  // The missing answer needs the live e() to be one whose names PIET knows in
  // full: binscatter's own posting (runBinscatter's `ereturn post` replaces
  // e() whole -- no model, no estimation block, e(graphcmd) -- and posts
  // nothing PIET does not hold; so does `ereturn post`, postingOnly above).
  // After any other fit PIET cannot tell a matrix
  // Stata never posts (e(ilog) after regress) from one this version lacks
  // (e(V_modelbased) after `regress, vce(robust)`, e(gradient) after logit),
  // so a missing value there keeps the r(111) refusal instead of a 1 x 1
  // missing that would be a wrong answer at rc 0. A held, non-missing e()
  // scalar answers its value on every e(). e(b)/e(V) keep their own rules
  // above (r(301) with no e(), 0x0 after a posting without them, r(111)
  // refusals). The value comes from the scalar evaluator
  // (evalMatrixScalarExpr), the same reader as a scalar cell, so a fit's e(N)
  // answers its own N.
  // BUG-1414 (w2q_matrix_edges S05; StataBE 19.5, 2026-11-03 oracle): the
  // 1 x 1 missing is also the answer where PIET does know every e() matrix:
  // with no e() at all (`matrix S = e(nosuch)` in a fresh session or after
  // `ereturn clear`), and after a fit that publishes its whole e(matrices)
  // list (`regress`: b V beta) when NAME is not on it. An e() string macro is
  // r(109) `type mismatch` (`matrix S = e(cmd)` after regress).
  if (!resolved.has_value() && cls == 'e' && which != "b" && which != "V" &&
      session.eval.eStrings.find(which) != nullptr)
    return engineErrorBlockRc("type mismatch", 109);
  if (!resolved.has_value() && cls == 'e' && which != "b" && which != "V") {
    bool notAMatrix = !session.eval.eLive();
    if (!notAMatrix && session.eval.eNameOrder) {
      const std::optional<std::vector<std::string>> mats = session.eval.eNameOrder("matrices");
      notAMatrix = mats.has_value() && std::find(mats->begin(), mats->end(), which) == mats->end();
    }
    double v = 0.0;
    std::string scalarErr;
    if (evalMatrixScalarExpr("e(" + which + ")", session, v, scalarErr) &&
        (postingOnly || notAMatrix || !std::isnan(v))) {
      eval::EvalMatrix one;
      one.data = {{v}};
      applyDefaultStripe(one);
      resolved = std::move(one);
    }
  }
  if (!resolved.has_value()) return engineErrorBlockRc(classErr, classRc);
  storeMatrixResult(session, name, std::move(*resolved));
  return ojson::array();  // matrix define is silent
}

// ---------------------------------------------------------------------------
// label variable / label values


}  // namespace piet::handlers
