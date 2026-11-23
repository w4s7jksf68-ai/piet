// SPDX-License-Identifier: GPL-3.0-or-later
#include "piet/column_fetch.h"
// Copyright (C) 2026 M.K. Kim
// Data listing.

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
#include <set>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/cclass.h"  // settingCValue: list's default linesize
#include "language/display_internal.h"  // applyStataFmtLocal: list cells as display shows them
#include "piet/loops.h"   // expandVarlist: list's sepby()/mean()/constant() varlists
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

#include "engine/command_support.h"
#include "estimation/estimation_options.h"
#include "piet/handlers/descriptive.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace command_detail {}
using namespace command_detail;
using namespace session_vocab;

namespace command_detail {

// Q2 (gate ②③): native fast path for `count if [in]`.
//
// runCount used to route every if/in through applyIfAndInFilter -> cobra
// keepif, which spawns a subprocess and writes a COMPLETE filtered .hbin to
// disk just to read back its row count (~0.66s on a 108K×192 corpus vs a
// ~µs floor). This evaluates the predicate directly over the ALREADY-mmap'd
// Datastore columns instead.
//
// CORRECTNESS-FIRST (gate ①, silent divergence forbidden): parity is with
// TODAY's cobra keepif pipeline (src/data/frame.cpp Parser+ev()+keep_if),
// which is Stata-verified. piet-native does NOT link that engine (subprocess
// only), so ev() is PORTED here 1:1 for a deliberately small whitelist, and
// ANYTHING the port can't prove identical throws CountFastBail -> runCount
// falls back to the UNCHANGED cobra path. Falling back is always safe (cobra
// is the ground truth and re-produces today's result OR today's error), so the
// only obligation is that an ACCEPTED expression computes bit-identically.
//
// Accepted grammar (mirrors frame.cpp exactly): numeric literals (same strict
// scan — a token std::stod won't fully consume, or a hex 0x.., bails), the
// bare `.` missing literal, f64 variable references, `factorvar ==/!=
// "literal"` (resolved to a level-code compare, missing code -> +inf, exactly
// like frame.cpp eqne), unary +/-/!, ( ), + - * / ^ (all wrapped in fin():
// non-finite -> missing), the six comparisons (both sides through mi():
// missing = +infinity), & | (a missing operand is TRUE, as Stata reads it),
// and missing()/inrange()/inlist(). Everything else BAILS: _n / _N
// / _pi / subscripts var[..] (the first two even ERROR in cobra's non-bysort
// keep_if — bailing reproduces that error), any other function, a factor used
// as a number or any string/number mix, and any parse error / trailing input.
//
// Missing-value semantics are the crux and are taken verbatim from frame.cpp:
// mi() maps NaN->+inf for comparisons, fin() maps non-finite->NaN for
// arithmetic, and `!`, `&`, `|` read an operand through truthy(): nonzero is
// true and a missing value is nonzero, so a missing operand is TRUE ([U]
// 13.2.4; w2c_functions.log operators_missing), and the result is always 0/1.
// Until BUG-620 both layers read a missing `&`/`|` operand as false and `!`
// passed it through as missing; they changed together, so they never disagree.
// See runCount for the keep-truthiness rule, one for if-only and if+in alike
// (BUG-534), derived from frame.cpp keep_if + the fallback's
// `((if)!=0)&(inrange)`, not guessed.
struct CountFastBail {};

constexpr double kCiNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kCiInf = std::numeric_limits<double>::infinity();
inline double ciMi(double v) { return std::isnan(v) ? kCiInf : v; }        // frame.cpp mi()
inline double ciFin(double v) { return std::isfinite(v) ? v : kCiNaN; }    // frame.cpp fin()
inline bool ciTruthy(double v) { return std::isnan(v) || v != 0.0; }       // frame.cpp truthy()
// frame.cpp scmp() — Stata's total order with the extended missings `.a`-`.z`
// ordered above `.` (data/missing.h). This fast path SHADOWS the engine for
// every `count if`, so it has to carry the same semantics or PIET answers a
// different question than cobra does for the identical expression.
inline int ciCmp(double a, double b) { return cecon::stata_cmp(a, b); }

struct CiNode {
  enum K {
    Num, VarF64, FactorCmp, FactorRef, StrLit,
    Neg, Not, Add, Sub, Mul, Div, Pow,
    Lt, Gt, Le, Ge, Eq, Ne, And, Or,
    FnMissing, FnInrange, FnInlist
  } kind = Num;
  double num = 0.0;
  const double* col = nullptr;                        // VarF64
  const int32_t* codes = nullptr;                     // FactorRef / FactorCmp
  const std::vector<std::string>* levels = nullptr;   // FactorRef (dictionary)
  long factorIdx = 0;                                 // FactorCmp: level code
  bool eq = true;                                     // FactorCmp: == vs !=
  std::string sval;                                   // StrLit
  std::unique_ptr<CiNode> a, b;
  std::vector<std::unique_ptr<CiNode>> args;          // Fn args
};

// 1:1 port of frame.cpp ev(). Only reaches node kinds the parser actually
// produces; FactorRef/StrLit are guarded out at parse time (they bail).
double ciEval(const CiNode* n, int64_t row) {
  switch (n->kind) {
    case CiNode::Num: return n->num;
    case CiNode::VarF64: return n->col[row];
    case CiNode::FactorCmp: {
      double c = n->codes[row] < 0 ? kCiInf : static_cast<double>(n->codes[row]);
      double t = static_cast<double>(n->factorIdx);
      return n->eq ? (c == t ? 1.0 : 0.0) : (c != t ? 1.0 : 0.0);
    }
    case CiNode::Neg: return -ciEval(n->a.get(), row);
    case CiNode::Not: return ciTruthy(ciEval(n->a.get(), row)) ? 0.0 : 1.0;
    case CiNode::Add: return ciFin(ciEval(n->a.get(), row) + ciEval(n->b.get(), row));
    case CiNode::Sub: return ciFin(ciEval(n->a.get(), row) - ciEval(n->b.get(), row));
    case CiNode::Mul: return ciFin(ciEval(n->a.get(), row) * ciEval(n->b.get(), row));
    case CiNode::Div: return ciFin(ciEval(n->a.get(), row) / ciEval(n->b.get(), row));
    case CiNode::Pow: {
      // frame.cpp's `^`: Stata's power (core/stata_arith.h), not libm pow.
      const double x = ciEval(n->a.get(), row);
      return ciFin(cecon::stata_pow_value(x, ciEval(n->b.get(), row)));
    }
    case CiNode::Lt: return ciCmp(ciEval(n->a.get(), row), ciEval(n->b.get(), row)) <  0 ? 1.0 : 0.0;
    case CiNode::Gt: return ciCmp(ciEval(n->a.get(), row), ciEval(n->b.get(), row)) >  0 ? 1.0 : 0.0;
    case CiNode::Le: return ciCmp(ciEval(n->a.get(), row), ciEval(n->b.get(), row)) <= 0 ? 1.0 : 0.0;
    case CiNode::Ge: return ciCmp(ciEval(n->a.get(), row), ciEval(n->b.get(), row)) >= 0 ? 1.0 : 0.0;
    case CiNode::Eq: return ciCmp(ciEval(n->a.get(), row), ciEval(n->b.get(), row)) == 0 ? 1.0 : 0.0;
    case CiNode::Ne: return ciCmp(ciEval(n->a.get(), row), ciEval(n->b.get(), row)) != 0 ? 1.0 : 0.0;
    case CiNode::And: { double x = ciEval(n->a.get(), row), y = ciEval(n->b.get(), row);
      return (ciTruthy(x) && ciTruthy(y)) ? 1.0 : 0.0; }
    case CiNode::Or: { double x = ciEval(n->a.get(), row), y = ciEval(n->b.get(), row);
      return (ciTruthy(x) || ciTruthy(y)) ? 1.0 : 0.0; }
    case CiNode::FnMissing:
      for (auto& q : n->args) if (std::isnan(ciEval(q.get(), row))) return 1.0;
      return 0.0;
    case CiNode::FnInrange: {
      double z = ciEval(n->args[0].get(), row), lo = ciEval(n->args[1].get(), row),
             hi = ciEval(n->args[2].get(), row);
      if (std::isnan(z)) return 0.0;
      bool lm = std::isnan(lo), hm = std::isnan(hi);
      if (lm && hm) return 1.0;
      if (lm) return z <= hi ? 1.0 : 0.0;
      if (hm) return z >= lo ? 1.0 : 0.0;
      return (lo <= z && z <= hi) ? 1.0 : 0.0;
    }
    case CiNode::FnInlist: {
      double z = ciEval(n->args[0].get(), row);
      for (size_t k = 1; k < n->args.size(); ++k) {
        double v = ciEval(n->args[k].get(), row);
        if ((std::isnan(z) && std::isnan(v)) || z == v) return 1.0;
      }
      return 0.0;
    }
    case CiNode::FactorRef:
    case CiNode::StrLit:
      throw CountFastBail{};  // never legal outside a factor ==/!= "literal"
  }
  return kCiNaN;
}

// Recursive-descent parser mirroring frame.cpp's grammar
// (or > and > cmp(!= > < <= >= ==, one level each) > add > mul > neg(- !) > pow(left) > unary(- !)
// > primary). Every path that
// frame.cpp's Parser would err() on, or that this port can't prove identical,
// throws CountFastBail so the caller can fall back to cobra.
struct CiParser {
  const std::string& s;
  size_t p = 0;
  Datastore& ds;
  int depth = 0;
  CiParser(const std::string& src, Datastore& d) : s(src), ds(d) {}

  void ws() { while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p; }
  bool eat(const char* tok) {
    ws();
    size_t n = 0;
    while (tok[n]) ++n;
    if (s.compare(p, n, tok) == 0) { p += n; return true; }
    return false;
  }
  static std::unique_ptr<CiNode> mk(CiNode::K k) { auto n = std::make_unique<CiNode>(); n->kind = k; return n; }
  static bool isOperand(const CiNode& n) { return n.kind != CiNode::FactorRef && n.kind != CiNode::StrLit; }
  std::unique_ptr<CiNode> bin(CiNode::K k, std::unique_ptr<CiNode> a, std::unique_ptr<CiNode> b) {
    // A factor/string operand is legal ONLY as a direct child of a factor
    // ==/!= "literal" comparison (handled in cmp/eqne). Anywhere else, cobra
    // ev() throws "string/factor used where a number is required" -> bail.
    if (!isOperand(*a) || !isOperand(*b)) throw CountFastBail{};
    auto n = mk(k); n->a = std::move(a); n->b = std::move(b); return n;
  }
  struct DepthGuard { int& d; explicit DepthGuard(int& x) : d(x) { if (++d > 1000) throw CountFastBail{}; } ~DepthGuard() { --d; } };

  std::unique_ptr<CiNode> parse() {
    auto n = orx();
    ws();
    if (p != s.size()) throw CountFastBail{};  // trailing input
    return n;
  }
  std::unique_ptr<CiNode> orx() { auto a = andx(); while (eat("|")) a = bin(CiNode::Or, std::move(a), andx()); return a; }
  std::unique_ptr<CiNode> andx() { auto a = cmp(); while (eat("&")) a = bin(CiNode::And, std::move(a), cmp()); return a; }
  // FU-844 (frame.cpp cmp/eatCmp, same levels): each comparison operator is its
  // own left-associative level, tightest first != > < <= >= == (oracle
  // w2ac_expr_relational count_<o1>_<o2>).
  static constexpr int kCmpLevels = 6;
  bool eatCmp(int lvl) {
    ws();
    static const char* const kOp[kCmpLevels] = {"==", ">=", "<=", "<", ">", "!="};
    const char* op = kOp[lvl];
    size_t n = std::strlen(op);
    if (lvl == kCmpLevels - 1 && s.compare(p, 2, "~=") == 0) { p += 2; return true; }
    if (s.compare(p, n, op) != 0) return false;
    if (n == 1 && p + 1 < s.size() && s[p + 1] == '=') return false;
    p += n;
    return true;
  }
  std::unique_ptr<CiNode> cmp(int lvl = 0) {
    if (lvl == kCmpLevels) return add();
    auto a = cmp(lvl + 1);
    while (eatCmp(lvl)) {
      auto b = cmp(lvl + 1);
      switch (lvl) {
        case 0: a = eqne(true, std::move(a), std::move(b)); break;
        case 1: a = bin(CiNode::Ge, std::move(a), std::move(b)); break;
        case 2: a = bin(CiNode::Le, std::move(a), std::move(b)); break;
        case 3: a = bin(CiNode::Lt, std::move(a), std::move(b)); break;
        case 4: a = bin(CiNode::Gt, std::move(a), std::move(b)); break;
        default: a = eqne(false, std::move(a), std::move(b)); break;
      }
    }
    return a;
  }
  // frame.cpp eqne: resolve `factorvar ==/!= "literal"` (either order) to a
  // numeric compare against the 0-based level code; a literal that is not a
  // level makes == constant-false / != constant-true.
  std::unique_ptr<CiNode> eqne(bool isEq, std::unique_ptr<CiNode> a, std::unique_ptr<CiNode> b) {
    CiNode* fac = nullptr;
    CiNode* lit = nullptr;
    if (a->kind == CiNode::FactorRef && b->kind == CiNode::StrLit) { fac = a.get(); lit = b.get(); }
    else if (b->kind == CiNode::FactorRef && a->kind == CiNode::StrLit) { fac = b.get(); lit = a.get(); }
    if (fac) {
      long idx = -1;
      for (size_t i = 0; i < fac->levels->size(); ++i)
        if ((*fac->levels)[i] == lit->sval) { idx = static_cast<long>(i); break; }
      if (idx < 0) { auto cst = mk(CiNode::Num); cst->num = isEq ? 0.0 : 1.0; return cst; }
      auto n = mk(CiNode::FactorCmp); n->codes = fac->codes; n->factorIdx = idx; n->eq = isEq;
      return n;
    }
    // lone factor/string, or a string-vs-number mix -> cobra errors -> bail.
    if (a->kind == CiNode::FactorRef || b->kind == CiNode::FactorRef ||
        a->kind == CiNode::StrLit || b->kind == CiNode::StrLit) throw CountFastBail{};
    return bin(isEq ? CiNode::Eq : CiNode::Ne, std::move(a), std::move(b));
  }
  // EM0 (frame.cpp add/sub/mul/div, same grouping): + over -, * over /, each left-associative
  // ([U] 13.2.5; oracle w2am_expr_binary_order eo_cnt_pm / eo_cnt_md).
  std::unique_ptr<CiNode> add() {
    auto a = sub();
    for (;;) { ws(); if (eat("+")) a = bin(CiNode::Add, std::move(a), sub()); else break; }
    return a;
  }
  std::unique_ptr<CiNode> sub() {
    auto a = mul();
    for (;;) { ws(); if (eat("-")) a = bin(CiNode::Sub, std::move(a), mul()); else break; }
    return a;
  }
  std::unique_ptr<CiNode> mul() {
    auto a = div();
    for (;;) { ws(); if (eat("*")) a = bin(CiNode::Mul, std::move(a), div()); else break; }
    return a;
  }
  std::unique_ptr<CiNode> div() {
    auto a = neg();
    for (;;) { ws(); if (eat("/")) a = bin(CiNode::Div, std::move(a), neg()); else break; }
    return a;
  }
  // FU-811 (frame.cpp neg/powx/unary, same tree and bail rules): negation and
  // `!` bind looser than `^`, which is left-associative, so -a^2 is -(a^2),
  // !a^0 is !(a^0), 2^3^2 is (2^3)^2 and 2^-a^2 is (2^(-a))^2
  // (oracle w2ac_expr_precedence count_neg_pow / count_not_pow0 /
  // count_chain_const / count_pow_neg_pow). `~` is `!`; a sign after `!` is
  // frame.cpp's syntax error -> bail.
  bool eatNot() {
    if (p >= s.size() || (s[p] != '!' && s[p] != '~') || (p + 1 < s.size() && s[p + 1] == '=')) return false;
    ++p;
    ws();
    if (p < s.size() && (s[p] == '-' || s[p] == '+')) throw CountFastBail{};
    return true;
  }
  std::unique_ptr<CiNode> neg() {
    DepthGuard g(depth);
    ws();
    if (eatNot()) {
      auto n = mk(CiNode::Not); n->a = neg();
      if (!isOperand(*n->a)) throw CountFastBail{};
      return n;
    }
    if (eat("-")) { auto n = mk(CiNode::Neg); n->a = neg(); if (!isOperand(*n->a)) throw CountFastBail{}; return n; }
    if (eat("+")) return neg();
    return powx();
  }
  std::unique_ptr<CiNode> powx() {
    auto a = primary();
    for (;;) {
      ws();
      if (!eat("^")) break;
      a = bin(CiNode::Pow, std::move(a), unary());
    }
    return a;
  }
  // The right operand of `^`: leading signs / `!` apply to the next primary only.
  std::unique_ptr<CiNode> unary() {
    DepthGuard g(depth);
    ws();
    if (eatNot()) {
      auto n = mk(CiNode::Not); n->a = unary();
      if (!isOperand(*n->a)) throw CountFastBail{};
      return n;
    }
    if (eat("-")) { auto n = mk(CiNode::Neg); n->a = unary(); if (!isOperand(*n->a)) throw CountFastBail{}; return n; }
    if (eat("+")) return unary();
    return primary();
  }
  std::vector<std::unique_ptr<CiNode>> arglist() {  // '(' already consumed
    std::vector<std::unique_ptr<CiNode>> v;
    ws();
    if (p < s.size() && s[p] == ')') { ++p; return v; }
    v.push_back(orx());
    while (eat(",")) v.push_back(orx());
    if (!eat(")")) throw CountFastBail{};
    return v;
  }
  std::unique_ptr<CiNode> primary() {
    ws();
    if (eat("(")) { auto n = orx(); if (!eat(")")) throw CountFastBail{}; return n; }
    if (p < s.size() && s[p] == '"') {
      ++p; size_t st = p;
      while (p < s.size() && s[p] != '"') ++p;
      if (p >= s.size()) throw CountFastBail{};  // unterminated
      auto n = mk(CiNode::StrLit); n->sval = s.substr(st, p - st); ++p; return n;
    }
    // extended missing literal `.a` … `.z` (frame.cpp) — one lowercase letter
    // after the dot that does not continue into an identifier.
    if (p + 1 < s.size() && s[p] == '.' && s[p + 1] >= 'a' && s[p + 1] <= 'z' &&
        (p + 2 >= s.size() ||
         (!cecon::ident::isIdentByte(s[p + 2]) && s[p + 2] != '.'))) {
      int kind = cecon::kMissA + (s[p + 1] - 'a');
      p += 2;
      auto n = mk(CiNode::Num);
      n->num = cecon::stata_missing(kind);
      return n;
    }
    // bare missing literal: a '.' not part of a number (frame.cpp).
    if (p < s.size() && s[p] == '.' && (p + 1 >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p + 1])))) {
      ++p; auto n = mk(CiNode::Num); n->num = kCiNaN; return n;
    }
    if (p < s.size() && (std::isdigit(static_cast<unsigned char>(s[p])) || s[p] == '.')) {
      size_t st = p;
      while (p < s.size() && (std::isalnum(static_cast<unsigned char>(s[p])) || s[p] == '.' ||
                              ((s[p] == '+' || s[p] == '-') && (s[p - 1] == 'e' || s[p - 1] == 'E'))))
        ++p;
      std::string tok = s.substr(st, p - st);
      for (char c : tok) if (c == 'x' || c == 'X') throw CountFastBail{};  // hex: Stata rejects
      size_t consumed = 0;
      double val;
      try { val = std::stod(tok, &consumed); } catch (...) { throw CountFastBail{}; }
      if (consumed != tok.size()) throw CountFastBail{};  // partial consume -> reject (frame.cpp C1-F11)
      auto n = mk(CiNode::Num); n->num = val; return n;
    }
    // A Unicode name (`قيمة > 2`) is one identifier (core/ident.h); it is looked up
    // byte-exactly against the dataset like any other.
    if (p < s.size() && cecon::ident::isIdentStartByte(s[p])) {
      size_t st = p;
      while (p < s.size() && cecon::ident::isIdentByte(s[p])) ++p;
      std::string id = s.substr(st, p - st);
      // _pi / _n / _N: not fast-pathed (_n/_N even ERROR in cobra's non-bysort
      // keep_if — bailing reproduces that; _pi is just rare here).
      if (id == "_pi" || id == "_n" || id == "_N") throw CountFastBail{};
      ws();
      if (p < s.size() && s[p] == '(') {  // function call
        ++p;
        auto args = arglist();
        for (auto& q : args) if (!isOperand(*q)) throw CountFastBail{};  // no factor/string args
        int na = static_cast<int>(args.size());
        if (id == "missing" && na >= 1) { auto n = mk(CiNode::FnMissing); n->args = std::move(args); return n; }
        if (id == "inrange" && na == 3) { auto n = mk(CiNode::FnInrange); n->args = std::move(args); return n; }
        if (id == "inlist" && na >= 2) { auto n = mk(CiNode::FnInlist); n->args = std::move(args); return n; }
        throw CountFastBail{};  // any other function -> cobra handles/errors it
      }
      if (!ds.has(id)) throw CountFastBail{};  // unknown variable -> cobra errors
      const cecon::Column& c = ds.col(id);
      if (c.dtype == cecon::DType::Factor) {
        ws();
        if (p < s.size() && s[p] == '[') throw CountFastBail{};  // subscript -> bail
        auto fv = mk(CiNode::FactorRef); fv->codes = c.codes(); fv->levels = &c.levels;
        return fv;
      }
      auto v = mk(CiNode::VarF64); v->col = c.f64();
      ws();
      if (p < s.size() && s[p] == '[') throw CountFastBail{};  // subscript -> bail
      return v;
    }
    throw CountFastBail{};  // unexpected token
  }
};

}  // namespace

ojson runCount(const std::optional<std::string>& ifExpr, SessionState& session,
              const std::optional<std::string>& inRange) {
  // `_rc` (FU-169) reaches here already resolved: every caller's condition went
  // through the dispatcher's row pre-pass (substituteRowExprRefs, BUG-1359).
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveIn = inRange.has_value() && !inRange->empty();
  // BUG-032 / BUG-412: a condition over a factor (string) column, a string
  // function or a string literal is counted row-wise; a co-occurring `in` is
  // ANDed into the same mask (BS-090). The fast path and the engine below read
  // `svar == ""` as constant-false, because a blank is not a dictionary level.
  // This used to sit in the dispatcher's `count` arm only, so `assert`, which
  // counts its contradictions here (runAssert), took the numeric path: `assert
  // g == "" in 3/4` called every blank row a contradiction.
  if (haveIf && strmutate::needsStringCount(session, ifExpr)) {
    strmutate::CountResult cr = strmutate::countRowsIf(session, *ifExpr, inRange);
    if (!cr.ok && cr.inRangeError) {
      // The one `in` refusal the numeric path below gives too: `assert g == ""
      // in 0/2` is "'0' invalid observation number" r(198), as in Stata.
      InRangeFilterResult fr;
      fr.ok = false;
      fr.refusal = cr.errMsg;
      return inRangeRejectError(fr, *inRange, "count");
    }
    if (!cr.ok) {
      // r(109) is Stata's "type mismatch" whichever row raised it (`count if s`,
      // `assert s`: StataBE 19.5).
      if (!cr.errReason.empty())
        return eval::errorBlock("engine_error",
                                cr.errReason == "r(109)" ? std::string("type mismatch") : cr.errMsg,
                                std::nullopt, std::nullopt, std::nullopt, cr.errReason);
      return eval::engineError(cr.errMsg, "engine_error");
    }
    session.eval.rValues.set("N", static_cast<double>(cr.n));
    ojson out;
    out["type"] = "display";
    out["text"] = std::to_string(cr.n);
    return out;
  }
  // C4-L2A (D1 card plane, C4-RESIDENT-READ-SCOPE-brief §1/§4): a bare
  // `count` needs only _N, which the resident card already carries
  // (residentRowsIfCurrent — same honest-currency rule as
  // shadowVarlistIfCurrent, nullopt under PIET_NO_RESIDENT_READ=1 or a
  // non-current/rows-unknown card → the unchanged datastore() path below).
  // Answering here skips the Datastore open (mmap + whole-file FNV) and, on
  // a dirty virtual head, the EMH frame snapshot that open forces. Output is
  // byte-identical to the ds->nRows() path: same r(N), same display text.
  if (!haveIf && !haveIn) {
    if (std::optional<int64_t> rows = session.residentRowsIfCurrent()) {
      session.eval.rValues.set("N", static_cast<double>(*rows));
      ojson out;
      out["type"] = "display";
      out["text"] = std::to_string(*rows);
      return out;
    }
  }
  // ONESHOT-GETCOLS2: `count if` needs the columns its predicate names, not the
  // frame. session.datastore() flushes the whole resident head to disk to supply
  // them — 56 of the 98 flushes left after the mutation entrance moved. Fetch the
  // predicate's columns instead (contract_A §38); every failure falls through to
  // the ordinary path with nothing lost.
  //
  // Row COUNT is preserved by a projection, so nRows() and the `in` bounds below
  // read the same numbers either way. The no-if/no-in case never gets here — the
  // shadow card answered it above — so a fetch is only attempted when there is a
  // predicate whose columns are worth fetching.
  std::string countMiniPath;
  std::unique_ptr<Datastore> countMini;
  if (haveIf) {
    if (auto cols = referencedColumns(session, {&*ifExpr}, ""))
      countMini = fetchColumns(session, *cols, session.workdir(), &countMiniPath);
  }
  Datastore* ds = countMini ? countMini.get() : session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  int64_t n = 0;

  // Q2 (gate ②③) native fast path — evaluate `if`/`in` over the mmap'd columns
  // rather than spawning cobra keepif + writing a full filtered .hbin. Anything
  // the native evaluator can't prove identical to cobra keep_if throws
  // CountFastBail and we drop to the UNCHANGED applyIfAndInFilter path.
  bool fastPathDone = false;
  if (!haveIf && !haveIn) {
    n = ds->nRows();
    fastPathDone = true;
  } else {
    // Resolve `in` bounds natively (same parseInRangeBounds the fallback uses),
    // clamped to the real row set exactly like cobra's `idx>=a & idx<=b` over a
    // 1-based row index. A malformed/negative bound is the SAME user error
    // either way, surfaced identically (byte-identical InRangeFilterResult).
    int64_t lo1 = 1, hi1 = ds->nRows();
    if (haveIn) {
      InRangeBounds rr = parseInRangeBounds(*inRange, ds->nRows());
      if (!rr.ok) {
        InRangeFilterResult fr;
        fr.ok = false;
        fr.refusal = rr.refusal;
        return inRangeRejectError(fr, *inRange, "count");
      }
      lo1 = std::max<int64_t>(1, rr.a);
      hi1 = std::min<int64_t>(ds->nRows(), rr.b);
    }
    if (haveIf) {
      try {
        CiParser parser(*ifExpr, *ds);
        std::unique_ptr<CiNode> ast = parser.parse();
        // A bare factor/string predicate (`count if svar`) is a number-context
        // misuse cobra keep_if errors on — bail so the fallback reproduces it.
        if (ast->kind == CiNode::FactorRef || ast->kind == CiNode::StrLit) throw CountFastBail{};
        int64_t cnt = 0;
        for (int64_t i = lo1; i <= hi1; ++i) {
          double v = ciEval(ast.get(), i - 1);
          // Keep-truthiness parity with the fallback — ONE rule, if-only or
          // if+in alike: a row counts iff `(isnan(v) || v != 0)`, a missing
          // predicate being TRUE in Stata ([U] 13.2.4). If-only, that is
          // frame.cpp keep_if itself. If+in, the fallback runs
          // `((ifExpr)!=0) & (idx-range)` (BUG-534, applyIfAndInFilter);
          // before that it ran `(ifExpr) & (idx-range)`, whose engine `&`
          // then read a missing operand as false (BUG-620), and this line
          // mirrored it (`!isnan(v) && v != 0`), so `count if x in 1/5`
          // skipped the missing-x rows Stata counts.
          bool keep = ciTruthy(v);
          if (keep) ++cnt;
        }
        n = cnt;
        fastPathDone = true;
      } catch (const CountFastBail&) {
        // if-expr uses a non-fast-pathable construct -> cobra fallback below.
      }
    } else {
      // `in`-only: keep_if on the range predicate keeps exactly the in-range
      // rows — no expression to evaluate.
      n = hi1 >= lo1 ? (hi1 - lo1 + 1) : 0;
      fastPathDone = true;
    }
  }

  if (!fastPathDone) {
    // Fallback (UNCHANGED from pre-Q2). BS-090 (BETA Phase G): `in` composes
    // with `if` via AND (piet::handlers::applyIfAndInFilter).
    InRangeFilterResult fr;
    try {
      // The filter must run on the session's REAL head, never on a §38 projection:
      // the projection carries only the predicate's columns, and this fallback's
      // result is read back as a dataset. `ds->path()` is the projection whenever
      // the fetch above succeeded, so ask the session instead.
      const std::string headPath =
          countMini ? session.currentHbin().value_or(ds->path()) : ds->path();
      fr = applyIfAndInFilter(session, headPath, ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *inRange, "count");
    Datastore filtered(fr.dataPath);
    n = filtered.nRows();
  }
  // dispatcher.py's count handler: `session._r_values["N"] = float(n)` —
  // count is an r-class command (r(N) available to a following `display
  // r(N)`), and the result block is `{"type": "display", ...}`, NOT echo.
  session.eval.rValues.set("N", static_cast<double>(n));
  ojson out;
  out["type"] = "display";
  out["text"] = std::to_string(n);
  return out;
}

// ---------------------------------------------------------------------------
// list (browse.py's run_browse in read-only list mode — dispatcher.py's
// `handler == "browse"` branch; `editable` is always false for `list`)

ojson runList(const std::optional<std::vector<std::string>>& varlist,
              const std::optional<std::string>& ifExpr, const std::optional<std::string>& inRange,
              SessionState& session, bool noLabel) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();

  int64_t browseOffset = 0;
  int64_t browseLimit = 50;
  bool haveInRange = inRange.has_value() && !inRange->empty();
  bool haveIfExpr = ifExpr.has_value() && !ifExpr->empty();
  if (haveInRange) {
    // BUG-672/673: the shared [U] 11.1.4 resolver. list's own parser clamped
    // every bound into range, so `list in -3/-1` listed row 1 and `list in
    // 1/50` on 12 rows listed all 12 with rc 0 (Stata: rows 10-12; r(198)).
    InRangeBounds rr = parseInRangeBounds(*inRange, ds->nRows());
    if (!rr.ok) {
      InRangeFilterResult fr;
      fr.ok = false;
      fr.refusal = rr.refusal;
      return inRangeRejectError(fr, *inRange, "list");
    }
    browseOffset = haveIfExpr ? 0 : rr.a - 1;
    browseLimit = rr.b - rr.a + 1;
  }

  // BUG-025: thread session.valueLabels through so value-labeled numeric
  // variables render label TEXT (mirrors browse.py's run_browse, which
  // always passes session.get_value_label_map() to Datastore.browse — list
  // and browse share the same underlying renderer in the oracle).
  // `nolabel` lists the stored codes instead of the label text: an empty map
  // is exactly a dataset with no value-label definitions attached.
  auto vlMap = noLabel ? decltype(session.valueLabelMapForBrowse()){}
                       : session.valueLabelMapForBrowse();
  // format-display-default-lifecycle: every listed column carries the format
  // its cells display in, from the one resolver (SessionState::displayFormat):
  // a `format` command's, the file's own, else the declared type's default
  // (%10.0g for a double, %12.0g for a long, %#s for a str#).
  auto fmtMap = session.displayFormatMap(ds->formatCard(varlist));

  ojson block;
  if (haveIfExpr) {
    // BUG-674: `if` and `in` compose as Stata's AND over the ORIGINAL row
    // numbers — the shared applyIfAndInFilter chain. The old path windowed the
    // `if`-filtered rows instead, so `list x if x>2 in 1/5` listed x = 3..7
    // (the first five matches) where Stata lists 3, 4, 5. An if-only call is
    // the same single --keepif this used to issue (BUG-369's ts injection
    // included).
    InRangeFilterResult fr;
    const std::string basePath = ds->path();  // a copy: the chain may touch the head
    try {
      fr = applyIfAndInFilter(session, basePath, ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *inRange, "list");
    Datastore tmpDs(fr.dataPath);
    block = tmpDs.browse(browseOffset, browseLimit, varlist, vlMap, fmtMap);
    cecon::native_unlink(fr.dataPath);
  } else {
    block = ds->browse(browseOffset, browseLimit, varlist, vlMap, fmtMap);
  }
  block["editable"] = false;
  if (haveInRange || haveIfExpr) {
    block["windowed"] = true;
    block["n_rows_total"] = static_cast<int64_t>(block.value("rows", ojson::array()).size());
  }
  return block;
}

// ---------------------------------------------------------------------------
// list's layout ([D] list): the data the frontend's listing renderer draws.

InRangeFilterResult applyIfAndInFilterKeepingIndex(SessionState& session,
                                                   const std::string& basePath,
                                                   const std::optional<std::string>& ifExpr,
                                                   const std::optional<std::string>& inRange,
                                                   std::string* indexVar);

namespace {

ojson listRefusal(const std::string& msg, int rc) {
  return ojson(eval::errorBlock(rc == 111 ? "engine_error" : "syntax_error", msg, std::nullopt,
                                std::nullopt, std::nullopt, "r(" + std::to_string(rc) + ")"));
}

std::vector<std::string> listWords(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  for (std::string w; in >> w;) out.push_back(w);
  return out;
}

// A strict non-negative integer option argument.
bool listCount(const eval::OptValue& v, long long* out) {
  if (v.isBool) return false;
  const std::string t = strutil::trim(v.strValue);
  if (t.empty()) return false;
  for (char c : t)
    if (c < '0' || c > '9') return false;
  try {
    *out = std::stoll(t);
  } catch (...) {
    return false;
  }
  return true;
}

// The width a display format asks for (`%-18s` 18, `%8.0gc` 8, `%td` 0 = none).
long long listFormatWidth(const std::string& fmt) {
  static const std::regex re(R"(^%-?0?(\d+))");
  std::smatch m;
  if (std::regex_search(fmt, m, re)) return std::stoll(m.str(1));
  return 0;
}

// A number as `display %fmt` shows it (the same renderer: date/time details,
// grouping, -0), without the field's padding.
std::string listCellNumber(double v, const std::string& fmt) {
  if (std::isnan(v)) return cecon::missing_text(cecon::missing_kind(v));
  std::string err;
  const std::string shown = display_detail::applyStataFmtLocal(
      fmt.empty() ? std::string("%9.0g") : fmt, eval::pythonFloatRepr(v), &err);
  if (!err.empty()) return strutil::trim(eval::applyStataFmt(v, fmt));
  return strutil::trim(shown);
}

}  // namespace

std::string listDisplayNumber(double v, const std::string& fmt) { return listCellNumber(v, fmt); }

std::optional<ojson> parseListLayout(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                     const std::string& raw, ListLayout* out) {
  ListLayout& lo = *out;
  auto varArg = [](const eval::OptValue& v) {
    return v.isBool ? std::vector<std::string>{} : listWords(v.strValue);
  };
  // A flag given an argument list is "option <typed>() incorrectly specified"
  // (bank 202f82eb18f228f0 `c(foreign)`: c is compress).
  static const std::set<std::string> kFlags = {
      "noobs", "clean", "divider", "compress", "nocompress", "fast", "table", "display",
      "noheader", "footer", "nolabel", "notrim", "nodotz", "subvarname", "absolute",
      "relative", "ds", "fvall"};
  for (const auto& [k, v] : opts) {
    if (v.isBool || kFlags.count(k) == 0) continue;
    std::string typed = k;
    static const std::regex reOpt(R"(([A-Za-z_]\w*)\s*\()");
    const long long comma = eval::findUnquotedComma(raw);
    if (comma >= 0) {
      const std::string tail = raw.substr(static_cast<size_t>(comma));
      for (std::sregex_iterator it(tail.begin(), tail.end(), reOpt), end; it != end; ++it) {
        const std::string w = strutil::lower((*it)[1].str());
        if (k.compare(0, w.size(), w) == 0) {
          typed = (*it)[1].str();
          break;
        }
      }
    }
    return listRefusal("option " + typed + "() incorrectly specified", 198);
  }
  bool separatorGiven = false;
  for (const auto& [k, v] : opts) {
    if (k == "separator") separatorGiven = true;
    if (k == "noobs") lo.noobs = true;
    else if (k == "clean") lo.clean = true;
    else if (k == "divider") lo.divider = true;
    else if (k == "compress") lo.compress = true;
    else if (k == "nocompress" || k == "fast") lo.nocompress = true;
    else if (k == "table") lo.table = true;
    else if (k == "display") lo.display = true;
    else if (k == "noheader") lo.noheader = true;
    else if (k == "footer") lo.footer = true;
    else if (k == "nolabel") lo.nolabel = true;
    else if (k == "notrim") lo.notrim = true;
    else if (k == "nodotz") lo.nodotz = true;
    else if (k == "subvarname") lo.subvarname = true;
    else if (k == "absolute") lo.absolute = true;
    else if (k == "relative") lo.relative = true;
    else if (k == "ds") lo.doubleSpace = true;
    else if (k == "fvall") continue;  // factor-variable levels: a varlist here has none
    else if (k == "header") {
      if (v.isBool) {
        lo.header = true;
      } else if (!listCount(v, &lo.headerEvery) || lo.headerEvery < 1) {
        return listRefusal("option header() incorrectly specified", 198);
      }
    } else if (k == "separator") {
      if (!listCount(v, &lo.separator))
        return listRefusal("option separator() incorrectly specified", 198);
    } else if (k == "abbreviate") {
      if (!listCount(v, &lo.abbreviate))
        return listRefusal("option abbreviate() incorrectly specified", 198);
    } else if (k == "string") {
      if (!listCount(v, &lo.stringWidth) || lo.stringWidth < 1)
        return listRefusal("option string() incorrectly specified", 198);
    } else if (k == "linesize") {
      if (!listCount(v, &lo.linesize) || lo.linesize < 1)
        return listRefusal("option linesize() incorrectly specified", 198);
    } else if (k == "sepby") {
      if (v.isBool) return listRefusal("option sepby() incorrectly specified", 198);
      lo.sepby = listWords(v.strValue);
    } else if (k == "mean") {
      lo.mean = varArg(v);
    } else if (k == "sum") {
      lo.sum = varArg(v);
    } else if (k == "n") {
      // N is case-sensitive in Stata: `, n` is "option n not allowed".
      static const std::regex reN(R"((^|[\s,])N(\(|\s|$))");
      const long long comma = eval::findUnquotedComma(raw);
      const std::string tail = comma >= 0 ? raw.substr(static_cast<size_t>(comma)) : std::string();
      if (!std::regex_search(tail, reN)) return listRefusal("option n not allowed", 198);
      lo.count = varArg(v);
    } else if (k == "labvar") {
      if (v.isBool) return listRefusal("option labvar() incorrectly specified", 198);
      lo.labvar = strutil::trim(v.strValue);
    } else if (k == "constant") {
      lo.constant = varArg(v);
    } else if (k == "sepbyexp") {
      return ojson(eval::unsupportedOption(
          "sepbyexp", "list's sepbyexp() (a separator wherever an expression changes) is not "
                      "implemented in this build; sepby() with a generated variable draws the "
                      "same lines."));
    }
  }
  // bank 36f8b30adbeb62e2 `sepby(foreign) sep(2)`, 62ea8e5c032e6036 `separator(0)`.
  if (separatorGiven && !lo.sepby.empty())
    return listRefusal("options separator() and sepby() may not be combined", 198);
  return std::nullopt;
}

ojson buildListing(SessionState& session, const Datastore& ds, const std::vector<int64_t>& rows,
                   const std::vector<int64_t>& obs, const std::vector<std::string>& vars,
                   const ListLayout& lo, ojson* err, const std::vector<ListLeadCol>& lead) {
  // Expands a variable-list option against the dataset (wildcards and ranges as
  // the varlist itself is expanded).
  auto expand = [&](const std::vector<std::string>& words, std::vector<std::string>* names) -> bool {
    if (words.empty()) return true;
    std::string spec;
    for (const auto& w : words) spec += (spec.empty() ? "" : " ") + w;
    auto r = piet::loops::expandVarlist(spec, session);
    if (r.isError) {
      *err = r.error;
      return false;
    }
    for (const auto& t : r.tokens) {
      if (!ds.has(t)) {
        *err = ojson(eval::engineError("variable " + t + " not found"));
        return false;
      }
      names->push_back(t);
    }
    return true;
  };

  const auto fmtPairs = session.displayFormatMap(ds.formatCard(std::nullopt));
  std::map<std::string, std::string> fmtOf(fmtPairs.begin(), fmtPairs.end());
  std::map<std::string, const std::vector<std::pair<int, std::string>>*> labelsOf;
  const auto vlMap = session.valueLabelMapForBrowse();
  if (!lo.nolabel)
    for (const auto& [name, codes] : vlMap) labelsOf[name] = &codes;
  auto charOf = [&](const std::string& var, const std::string& charname) -> std::optional<std::string> {
    for (const auto& c : session.characteristics)
      if (c.varname == var && c.charname == charname) return c.value;
    return std::nullopt;
  };

  auto cellOf = [&](const std::string& var, int64_t row) -> std::string {
    const cecon::Column& c = ds.col(var);
    if (c.dtype == cecon::DType::Factor) {
      const int32_t code = c.codes()[row];
      return code >= 0 && static_cast<size_t>(code) < c.levels.size()
                 ? std::string(c.levels[static_cast<size_t>(code)])
                 : std::string();
    }
    const double v = c.f64()[row];
    // A labelled extended missing value lists as its label (`label define l .a
    // "refused"`): the label set keys its codes as text.
    if (!lo.nolabel && std::isnan(v) && cecon::missing_kind(v) > 0) {
      auto ln = session.varLblname.find(var);
      if (ln != session.varLblname.end()) {
        auto defs = session.labelDefs.find(ln->second);
        if (defs != session.labelDefs.end()) {
          auto hit = defs->second.find(cecon::missing_text(cecon::missing_kind(v)));
          if (hit != defs->second.end()) return hit->second;
        }
      }
    }
    if (!std::isnan(v)) {
      auto it = labelsOf.find(var);
      if (it != labelsOf.end() && v == std::floor(v) && std::fabs(v) < 2147483648.0) {
        for (const auto& [code, text] : *it->second)
          if (code == static_cast<int>(v)) return text;
      }
    }
    // nodotz: a .z is listed as a field of blanks.
    if (lo.nodotz && std::isnan(v) && cecon::missing_kind(v) == cecon::kMissZ) return std::string();
    return listCellNumber(v, fmtOf[var]);
  };
  auto colOf = [&](const std::string& var) {
    ojson col;
    const bool isStr = ds.col(var).dtype == cecon::DType::Factor;
    const std::string& fmt = fmtOf[var];
    std::string head = var;
    bool charHead = false;
    if (lo.subvarname)
      if (auto ch = charOf(var, "varname")) {
        head = *ch;
        charHead = true;
      }
    col["name"] = var;
    col["head"] = head;
    if (charHead) col["charhead"] = true;
    col["str"] = isStr;
    col["left"] = fmt.size() > 1 && fmt[1] == '-';
    col["fmtw"] = listFormatWidth(fmt);
    return col;
  };

  ojson listing;
  // constant(): the listed (or named) variables that take one value over the
  // listed rows are shown once, in a box of their own, and left out of the table.
  std::vector<std::string> tableVars = vars;
  ojson constCols = ojson::array();
  if (lo.constant.has_value()) {
    std::vector<std::string> cand;
    if (lo.constant->empty()) cand = vars;
    else if (!expand(*lo.constant, &cand)) return ojson();
    for (const auto& var : cand) {
      if (rows.empty()) break;
      const std::string first = cellOf(var, rows.front());
      bool same = true;
      for (int64_t r : rows)
        if (cellOf(var, r) != first) {
          same = false;
          break;
        }
      if (!same) continue;
      ojson cc = colOf(var);
      cc["cell"] = first;
      constCols.push_back(cc);
      tableVars.erase(std::remove(tableVars.begin(), tableVars.end(), var), tableVars.end());
    }
  }

  int labvarIdx = -1;
  if (lo.labvar.has_value()) {
    for (size_t i = 0; i < tableVars.size(); ++i)
      if (tableVars[i] == *lo.labvar) labvarIdx = static_cast<int>(lead.size() + i);
    if (labvarIdx < 0) {
      *err = listRefusal("labvar() not found in varlist", 111);
      return ojson();
    }
  }

  // The computed columns, in listing order: the leading ones, the variables, the
  // trailing ones (`ord` holds -1-k for computed column k, else the variable index).
  std::vector<int> ord;
  for (size_t k = 0; k < lead.size(); ++k)
    if (!lead[k].trailing) ord.push_back(-1 - static_cast<int>(k));
  for (size_t v = 0; v < tableVars.size(); ++v) ord.push_back(static_cast<int>(v));
  for (size_t k = 0; k < lead.size(); ++k)
    if (lead[k].trailing) ord.push_back(-1 - static_cast<int>(k));
  if (labvarIdx >= 0) {
    const int v = labvarIdx - static_cast<int>(lead.size());
    labvarIdx = static_cast<int>(std::find(ord.begin(), ord.end(), v) - ord.begin());
  }
  auto leadCell = [&](const ListLeadCol& lc, size_t i) -> std::string {
    if (!lc.texts.empty()) return i < lc.texts.size() ? lc.texts[i] : std::string();
    return listCellNumber(i < lc.values.size() ? lc.values[i] : NAN, lc.format);
  };
  ojson cols = ojson::array();
  for (int o : ord) {
    if (o >= 0) {
      cols.push_back(colOf(tableVars[static_cast<size_t>(o)]));
      continue;
    }
    const ListLeadCol& lc = lead[static_cast<size_t>(-1 - o)];
    ojson col;
    col["name"] = lc.head;
    col["head"] = lc.head;
    col["str"] = !lc.texts.empty();
    col["left"] = false;
    col["fmtw"] = lc.fmtw;
    col["charhead"] = true;  // a header from a characteristic (subvarname)
    cols.push_back(col);
  }
  ojson cells = ojson::array();
  for (size_t i = 0; i < rows.size(); ++i) {
    ojson row = ojson::array();
    for (int o : ord)
      row.push_back(o >= 0 ? cellOf(tableVars[static_cast<size_t>(o)], rows[i])
                           : leadCell(lead[static_cast<size_t>(-1 - o)], i));
    cells.push_back(std::move(row));
  }

  // sepby(): a separator before every listed row whose sepby values differ from
  // the row listed before it (the variables need not be listed).
  ojson breaks = ojson::array();
  if (!lo.sepby.empty()) {
    std::vector<std::string> by;
    if (!expand(lo.sepby, &by)) return ojson();
    for (size_t i = 1; i < rows.size(); ++i) {
      bool differs = false;
      for (const auto& var : by) {
        const cecon::Column& c = ds.col(var);
        if (c.dtype == cecon::DType::Factor) {
          differs = c.codes()[rows[i]] != c.codes()[rows[i - 1]];
        } else {
          const double a = c.f64()[rows[i]], b = c.f64()[rows[i - 1]];
          differs = !(a == b || (std::isnan(a) && std::isnan(b)));
        }
        if (differs) break;
      }
      if (differs) breaks.push_back(static_cast<int64_t>(i));
    }
  }

  // mean / sum / N lines, in that order, over the listed rows.
  ojson stats = ojson::array();
  auto statLine = [&](const std::optional<std::vector<std::string>>& which, const std::string& label,
                      int kind) -> bool {
    if (!which.has_value()) return true;
    std::vector<std::string> named;
    if (!which->empty() && !expand(*which, &named)) return false;
    ojson line;
    line["label"] = label;
    ojson vals = ojson::array();
    for (int o : ord) {
      if (o >= 0) {
        const std::string& var = tableVars[static_cast<size_t>(o)];
        const bool asked = which->empty() ||
                           std::find(named.begin(), named.end(), var) != named.end();
        const cecon::Column& c = ds.col(var);
        if (!asked || c.dtype == cecon::DType::Factor) {
          vals.push_back(nullptr);
          continue;
        }
        double sum = 0.0;
        long long n = 0;
        for (int64_t r : rows) {
          const double v = c.f64()[r];
          if (std::isnan(v)) continue;
          sum += v;
          ++n;
        }
        const double out = kind == 0 ? (n > 0 ? sum / static_cast<double>(n) : NAN)
                           : kind == 1 ? sum
                                       : static_cast<double>(n);
        vals.push_back(listCellNumber(out, fmtOf[var]));
        continue;
      }
      const ListLeadCol& lc = lead[static_cast<size_t>(-1 - o)];
      if (!which->empty() || !lc.texts.empty()) {
        vals.push_back(nullptr);
        continue;
      }
      double sum = 0.0;
      long long n = 0;
      for (double v : lc.values)
        if (!std::isnan(v)) {
          sum += v;
          ++n;
        }
      const double out = kind == 0 ? (n > 0 ? sum / static_cast<double>(n) : NAN)
                         : kind == 1 ? sum
                                     : static_cast<double>(n);
      vals.push_back(listCellNumber(out, lc.format));
    }
    line["cells"] = vals;
    stats.push_back(line);
    return true;
  };
  if (!statLine(lo.mean, "Mean", 0) || !statLine(lo.sum, "Sum", 1) ||
      !statLine(lo.count, "N", 2))
    return ojson();

  ojson obsArr = ojson::array();
  for (size_t i = 0; i < obs.size(); ++i)
    obsArr.push_back(lo.relative ? static_cast<int64_t>(i + 1) : obs[i]);

  long long linesize = lo.linesize;
  if (linesize < 1) {
    linesize = 80;
    if (const auto cv = settingCValue("linesize", session)) {
      if (!cv->isString) {
        try {
          linesize = std::stoll(cv->text);
        } catch (...) {
        }
      }
    }
  }

  listing["cols"] = cols;
  listing["rows"] = cells;
  listing["obs"] = obsArr;
  listing["breaks"] = breaks;
  listing["stats"] = stats;
  listing["labvar"] = labvarIdx;
  listing["constant"] = constCols;
  ojson o;
  o["noobs"] = lo.noobs;
  o["clean"] = lo.clean;
  o["divider"] = lo.divider;
  o["compress"] = lo.compress;
  o["nocompress"] = lo.nocompress;
  o["table"] = lo.table;
  o["display"] = lo.display;
  o["header"] = lo.header;
  o["noheader"] = lo.noheader;
  o["footer"] = lo.footer;
  o["header_every"] = lo.headerEvery;
  o["notrim"] = lo.notrim;
  o["ds"] = lo.doubleSpace;
  o["separator"] = lo.separator;
  o["sepby"] = !lo.sepby.empty();
  o["abbreviate"] = lo.abbreviate;
  o["string"] = lo.stringWidth;
  o["linesize"] = linesize;
  listing["opts"] = o;
  return listing;
}

// How many observations one listing carries; past it the renderer says how many
// it left out (a listing is text in the Results pane, not the data browser).
constexpr int64_t kListingRowCap = 20000;

ojson runListStata(const std::vector<std::string>& vars, const eval::ParsedCommand& parsed,
                   SessionState& session, const ListLayout& lo) {
  std::optional<std::vector<std::string>> vl;
  if (!vars.empty()) vl = vars;
  ojson block = runList(vl, parsed.ifExpr, parsed.inRange, session, lo.nolabel);
  if (block.value("type", std::string()) != "browse") return block;
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  std::vector<std::string> names = vars;
  if (names.empty()) names = ds->names();

  const bool haveIf = parsed.ifExpr.has_value() && !parsed.ifExpr->empty();
  const bool haveIn = parsed.inRange.has_value() && !parsed.inRange->empty();
  std::vector<int64_t> rows, obs;
  std::unique_ptr<Datastore> filtered;
  std::string filteredPath;
  const Datastore* src = ds;
  if (haveIf) {
    std::string idxVar;
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilterKeepingIndex(session, ds->path(), parsed.ifExpr, parsed.inRange,
                                          &idxVar);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *parsed.inRange, "list");
    filtered = std::make_unique<Datastore>(fr.dataPath);
    if (fr.dataPath != ds->path()) filteredPath = fr.dataPath;
    src = filtered.get();
    const int64_t n = src->nRows();
    const double* idx = idxVar.empty() ? nullptr : src->col(idxVar).f64();
    for (int64_t r = 0; r < n; ++r) {
      rows.push_back(r);
      obs.push_back(idx != nullptr ? static_cast<int64_t>(idx[r]) : r + 1);
    }
  } else if (haveIn) {
    InRangeBounds rr = parseInRangeBounds(*parsed.inRange, ds->nRows());
    for (long long r = rr.a; rr.ok && r <= rr.b; ++r) {
      rows.push_back(r - 1);
      obs.push_back(r);
    }
  } else {
    for (int64_t r = 0; r < ds->nRows(); ++r) {
      rows.push_back(r);
      obs.push_back(r + 1);
    }
  }
  int64_t omitted = 0;
  if (static_cast<int64_t>(rows.size()) > kListingRowCap) {
    omitted = static_cast<int64_t>(rows.size()) - kListingRowCap;
    rows.resize(static_cast<size_t>(kListingRowCap));
    obs.resize(static_cast<size_t>(kListingRowCap));
  }
  ojson err;
  ojson listing = buildListing(session, *src, rows, obs, names, lo, &err);
  filtered.reset();
  if (!filteredPath.empty()) cecon::native_unlink(filteredPath);
  if (listing.is_null()) return err;
  listing["omitted"] = omitted;
  block["listing"] = std::move(listing);
  return block;
}

// ---------------------------------------------------------------------------
// scalar list / dir


}  // namespace piet::handlers
