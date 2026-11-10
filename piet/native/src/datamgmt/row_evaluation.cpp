// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Row evaluator — C++ port of piet/backend/piet/strmutate.py.
// See strmutate.h for the design rationale. Reuses the M1 evaluator
// (pyeval/exprfns/pyvalue), the same machinery loops.cpp::evalBoolExpr uses.
// Split: string generate/replace -> datamgmt/string_generate_replace.cpp; row
// conditions (count/keep/drop if, qualifier columns) -> datamgmt/row_conditions.cpp.
#include "piet/strmutate.h"
#include "piet/column_fetch.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/counters.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"  // protectLiterals / substituteBareName (installRowExpression)
#include "piet/eval/pyeval.h"
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"  // MECHOPT W3: the resident arm folds through it
#include "piet/strutil.h"
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "piet/storage_type.h"  // FU-849: replace's lattice (promotedType)
#include "data/missing.h"  // the data plane's extended-missing NaN payloads
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)
#include "row_evaluation_internal.h"

namespace piet::strmutate {

using namespace row_eval_detail;

namespace {

// FLOOR campaign instrumentation + test hook — see strmutate.h. Single-thread
// selftest reads these; production only ever increments the counter on the
// legacy fallback path (the slot path never writes a per-row map at all).
int64_t g_evalRowsMapWrites = 0;
bool g_forceLegacyEvalRows = false;

}  // namespace

namespace row_eval_detail {

// A5-04: NOT forwarded to strutil::trim — this trim() strips the full
// " \t\r\n\f\v" whitespace set (CR/LF/FF/VT included), while strutil::trim
// is deliberately " \t"-only (matches every OTHER duplicated trim() copy,
// but not this one). Left as its own copy; see strutil.h's header comment
// for the analogous eval::tokenizer.cpp strip()/lstrip()/rstrip() carve-out.
std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n\f\v");
  size_t b = s.find_last_not_of(" \t\r\n\f\v");
  if (a == std::string::npos) return "";
  return s.substr(a, b - a + 1);
}

}  // namespace row_eval_detail

namespace {

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `toLower(...)` call sites
// didn't need touching. Pure dedup, no behavior change.
std::string toLower(std::string s) { return strutil::lower(std::move(s)); }

}  // namespace

namespace row_eval_detail {

std::string nextPath(const std::string& workdir, const std::string& prefix) {
  auto ms = static_cast<long long>(std::time(nullptr)) * 1000;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%lld", ms);
  return workdir + "/" + prefix + "_" + buf + scratchSeqSuffix();
}

// BUG-124: runCobra() does not throw — failures come back in the RESULT (the
// serve router replaced the throwing one-shot in Stage 1, so this file's old
// `catch (CobraError)` blocks were dead and convert failures were silently
// ignored; a partially-written --out then passed the exists() check and got
// mmap'd → SIGSEGV in buildVarInfo). Manual CobraError build from stderr —
// file-local copy per the established one-copy-per-TU convention (see
// egen.cpp / datamgmt_support.h `errorFromRaw`).
CobraError errorFromRaw(const CobraRunResult& r) {
  std::string stderrText = r.stderr_text;
  while (!stderrText.empty() &&
         (stderrText.back() == '\n' || stderrText.back() == '\r' || stderrText.back() == ' ')) {
    stderrText.pop_back();
  }
  try {
    nlohmann::json payload = nlohmann::json::parse(stderrText);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                        err.at("message").get<std::string>(), err.value("detail", std::string()));
    }
  } catch (const nlohmann::json::exception&) {
  }
  std::string firstLine = stderrText;
  auto nl = firstLine.find('\n');
  if (nl != std::string::npos) firstLine = firstLine.substr(0, nl);
  if (firstLine.empty()) firstLine = "cobra exited " + std::to_string(r.exit_code);
  return CobraError("engine_error", firstLine, stderrText);
}

}  // namespace row_eval_detail

namespace {

// --- string-fn / string-literal / factor-reference detection (strmutate.py's
//     _STRING_FN_NAMES / _uses_string_fn / _has_string_literal /
//     _references_factor) -----------------------------------------------------

// The routing vocabulary: a name here sends gen/replace/count-if to this row
// evaluator even when no string literal or string column appears (`generate s
// = strofreal(x)`, `generate t = regexs(0)`). A string function missing from it
// reaches the all-numeric engine, which refuses it. The second block is the
// w2c_functions.log string functions (piet-row-evaluator-string-data-arms):
// every one is registered in exprfns.cpp's addStataFns (regexs through
// addRowPlaneRegexs). The third holds the business-calendar functions, whose
// first argument is a calendar NAME the engine cannot read (BUG-464: `egen mx =
// max(bofd("cal", bd))` and `summarize x if bofd(...)` take the routes keyed
// on this set).
const std::set<std::string>& stringFnNames() {
  static const std::set<std::string> s = {
      "lower", "upper", "strtrim", "trim", "ltrim", "rtrim", "substr",
      "strlen", "length", "strpos", "word", "real", "regexm", "regexr",
      "subinstr", "proper", "string", "char", "abbrev",
      "itrim", "regexs", "reverse", "strofreal", "strproper", "strrpos",
      "strmatch", "strtoname", "wordcount", "ustrleft", "ustrlen", "ustrregexm",
      "ustrregexra", "ustrtrim", "ustrunescape", "usubinstr",
      "bofd", "dofb",
  };
  return s;
}

// Replace every "..." string literal with an empty "" (so operators/idents
// inside a literal are not misread) — mirrors _STR_LITERAL_RE.sub('""', expr).
std::string maskStringLiterals(const std::string& expr) {
  static const std::regex reStr(R"("[^"]*")");
  return std::regex_replace(expr, reStr, "\"\"");
}

bool hasStringLiteral(const std::string& expr) {
  static const std::regex reStr(R"("[^"]*")");
  return std::regex_search(expr, reStr);
}

bool usesStringFn(const std::string& expr) {
  std::string masked = maskStringLiterals(expr);
  // Unicode names (core/ident.h): a byte >= 0x80 is a name byte. The regex takes
  // a whole name leftmost-first, and a match whose left neighbour is a name byte
  // is the tail of a longer name -- the `\b` this replaces, which an Arabic byte
  // (not `\w`) satisfied, so `مstrlen(` read as a call of strlen().
  static const std::regex reFnCall(std::string("(") + eval::kRxName + R"()\s*\()");
  auto begin = std::sregex_iterator(masked.begin(), masked.end(), reFnCall);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    const size_t pos = static_cast<size_t>(it->position(0));
    if (pos > 0 && cecon::ident::isIdentByte(masked[pos - 1])) continue;
    if (stringFnNames().count(toLower((*it)[1].str()))) return true;
  }
  return false;
}

// fix/verb-overhead-sweep S4a: the names-and-dtypes question, answered without
// re-opening the dataset.
//
// WHAT THIS COST. Every caller below is a PREDICATE — "does this expression
// mention a factor (string) column" — and the answer needs nothing but each
// column's NAME and DTYPE. It was being paid for with session.datastore(),
// which is two pieces of work, and on the hot path the FIRST one dominates:
//   1. ensureMaterializedHead() (session_state.h:313). On a resident chain the
//      session head is a VIRTUAL frame whose bytes live only in the worker's
//      RAM. Opening a Datastore forces that whole frame to be snapshotted to
//      disk — a full 164 MB write on the campaign's corpus — purely so a
//      predicate can read a column list. The resident path itself never needs
//      that snapshot: runViaCobraSession only materializes on its STATELESS
//      branch (session_state.cpp, `if (!goResident)`), so on an opted-in verb
//      like `keep if` this write is pure waste, caused by nobody but the
//      predicate that asked.
//   2. cecon::Dataset::open (src/io/columnar.cpp:64) — mmap, magic check, a
//      192-column header-JSON parse and per-column extent validation.
// NOT a provenance hash, despite what this campaign's earlier notes said. The
// byte-serial FNV1a whole-file hash is hash_file_hex (apps/cmds/common.cpp:41),
// it is engine-side, and build_provenance reaches it only for results carrying
// an "estimator" key (common.cpp:274/288). PIET's Datastore constructor never
// hashes anything. The ~263 ms figure attached to "frame load" elsewhere is a
// COBRA-side `use` cost (cecon_cli.cpp:354) and does not belong to this path;
// stating it here would send the next reader hunting a hash that isn't called.
//
// The fix is the one fix/sort-validate-lazy established for runSort and S1
// extended to runOrder/runGsort/runDrop/runKeep. This is that same mechanism at
// the one hot site those fixes did not reach: needsStringEval (gen/replace,
// dispatcher.cpp:1910/1969) and needsStringCount (drop if / keep if / count if,
// dispatcher.cpp:2533/2553/2884) both funnel through here, so the re-open taxed
// five verbs, INCLUDING the ones whose own entry validation S1 had already made
// lazy.
//
// A12 — WHY THE FAST ANSWER IS THE SAME ANSWER. This predicate decides whether
// a condition takes the row-wise string detour (BUG-032) or goes to cobra, so a
// wrong SET here silently reroutes execution: it is a correctness surface, not
// only a perf one. Three things make the card's answer identical to the
// datastore's, and the fourth is guarded rather than assumed:
//   • VOCABULARY. Datastore::dtype() returns exactly "factor" or "f64"
//     (datastore.cpp:49). Every writer of the shadow card populates dtype from
//     the same two-token vocabulary: reseedShadowFrom takes ds->varlist(),
//     whose buildVarInfo sets the identical string (datastore.cpp:81); the
//     frame-derived writers (seedShadowFromFrameVars and both fold branches)
//     take CobraFrameVar::type, which the engine emits as exactly
//     "factor"/"f64" (apps/cecon_cli.cpp:276, frame_var_desc).
//   • BASE vs RENDERED. ds->dtype() reads the hbin, i.e. the BASE type. The
//     card is likewise the base list — varlistWithMetaFrom (session_state.cpp:
//     382) is the render step, it rewrites label and level STRINGS and never
//     touches dtype. So this compares base against base.
//   • NAME SET / ORDER / DUPLICATES. shadowVarlistIfCurrent() is contracted to
//     be the current dataset's column list, which is the same reliance S1
//     already ships for name existence in drop/keep/order/gsort. Order cannot
//     diverge (the result is a std::set) and a doubly-named column cannot
//     either (a set dedupes; ds->names() cannot repeat).
//   • UNKNOWN SPELLINGS ARE REFUSED, NOT GUESSED. CobraFrameVar::type is
//     parsed with j.value("type", "") (cobra_session.cpp:48), so a malformed or
//     absent field yields "". Testing `!= "f64"` would read that as FACTOR
//     (false positive: a numeric condition wrongly rerouted); testing
//     == "factor" would read it as numeric (false negative: a genuine string
//     condition sent to an all-f64 engine — strictly worse). Neither guess is
//     taken: any dtype that is neither "factor" nor "f64" abandons the card and
//     falls through to the datastore, which cannot be wrong about its own file.
// One divergence is real and INHERITED rather than introduced: session.
// datastore() can throw on a failed materialization (BUG-124), and the card
// path does not, so that throw now surfaces from whichever later site genuinely
// opens the data instead of from here. S1's runDrop/runKeep fast paths already
// skip the same open and go straight to runCobraJson, so this is the shipped,
// ratified behaviour of the mechanism and not a new exposure.
std::set<std::string> factorColumns(SessionState& session) {
  std::set<std::string> out;
  if (!session.hasData()) return out;
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    bool trustworthy = true;
    for (const auto& info : *card) {
      if (info.dtype == "factor") {
        out.insert(info.name);
      } else if (info.dtype != "f64") {
        trustworthy = false;  // an unrecognized dtype spelling — refuse the card outright.
        break;
      }
    }
    if (trustworthy) return out;
    out.clear();  // never ship a half-scanned set; fall through to the real file.
  }
  Datastore* ds = session.datastore();
  if (ds == nullptr) return out;
  for (const auto& n : ds->names()) {
    if (ds->dtype(n) != "f64") out.insert(n);
  }
  return out;
}

// True if any bare identifier in `expr` (not immediately followed by '(') is a
// factor column — mirrors _references_factor.
bool referencesFactor(const std::string& expr, const std::set<std::string>& factorCols) {
  if (factorCols.empty()) return false;
  std::string masked = maskStringLiterals(expr);
  // A Unicode name is one name (core/ident.h): an Arabic string column is found,
  // and the ASCII tail of `دخل_2020` is never read as a column of its own.
  static const std::regex reIdent(eval::kRxName);
  auto begin = std::sregex_iterator(masked.begin(), masked.end(), reIdent);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    const std::smatch& m = *it;
    size_t after = static_cast<size_t>(m.position() + m.length());
    if (after < masked.size() && masked[after] == '(') continue;  // function call
    if (factorCols.count(m.str())) return true;
  }
  return false;
}

// --- Stata expression -> Python-eval'able rewrite (strmutate.py's
//     _rewrite_expr; the scheme of loops.cpp's evalBoolExpr less its lone
//     `|`/`&`/`!` rewrite (see rewriteExpr), replicated here per this codebase's
//     per-TU convention rather than sharing loops.cpp's anonymous-namespace
//     helpers) ------------------------------------------------------------------

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

std::string regexReplaceCb(const std::regex& re, const std::string& s,
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

}  // namespace

namespace row_eval_detail {

// _rewrite_expr: Stata scalar/boolean expr -> Python-eval'able string.
std::string rewriteExpr(const std::string& expr) {
  std::string cleaned = trim(expr);

  // Compound double-quotes `"..."' -> "..."
  static const std::regex reCompound(R"delim(`"(.*?)"')delim");
  cleaned = regexReplaceCb(reCompound, cleaned,
                           [](const std::smatch& m) { return "\"" + m.str(1) + "\""; });

  // Extract string literals before operator substitution (quote-aware).
  std::vector<std::string> placeholders;
  static const std::regex reStrLit(R"("[^"]*")");
  std::string py = regexReplaceCb(reStrLit, cleaned, [&](const std::smatch& m) {
    placeholders.push_back(m.str(0));
    return "__STR" + std::to_string(placeholders.size() - 1) + "__";
  });

  py = std::regex_replace(py, std::regex(R"(\|\|)"), " or ");
  py = std::regex_replace(py, std::regex(R"(&&)"), " and ");
  // A lone `&`, `|` and `!` stay Stata's: the parser has them (pyeval.cpp
  // Tok::Amp/Pipe/Bang, BUG-660) at Stata's precedence, and they refuse a
  // string operand with r(109). Rewritten to Python's and/or/not, as they were,
  // a string was a truth value (`count if s`, `assert s & 1`, `assert upper(s)`
  // rc 0; StataBE r(109) type mismatch) and `not` could not stand as a
  // comparison's operand (`assert y == !missing(s)` failed at obs 1; StataBE
  // reads (!missing(s))).
  py = std::regex_replace(py, std::regex(R"(~=)"), "!=");
  py = replaceLoneEquals(py);
  // Stata exponentiation ^ -> **
  {
    std::string tmp;
    tmp.reserve(py.size());
    for (char c : py) {
      if (c == '^') tmp += "**";
      else tmp += c;
    }
    py = tmp;
  }
  // Missing literals `.` and `.a`..`.z` -> their exact doubles (exprfns.h,
  // the rule macros.cpp and loops.cpp apply).
  py = eval::replaceStataMissingLiterals(py);

  // Restore string placeholders.
  for (size_t i = 0; i < placeholders.size(); ++i) {
    std::string key = "__STR" + std::to_string(i) + "__";
    size_t pos = py.find(key);
    if (pos != std::string::npos) py.replace(pos, key.size(), placeholders[i]);
  }
  return py;
}

}  // namespace row_eval_detail

namespace {

// --- per-column decode + per-row namespace value ---------------------------

struct ColView {
  bool isFactor;
  const double* f64 = nullptr;             // f64 path
  const int32_t* codes = nullptr;          // factor path
  const std::vector<std::string>* levels = nullptr;
};

// Build a fresh PyNamespace with the base fns bound (math + Stata fns +
// scalar()). Column/_n/_N values are bound per-row by the caller.
eval::PyNamespace baseNamespace(SessionState& session) {
  eval::PyNamespace ns = eval::mathNamespace(/*withInfNan=*/false);
  eval::addStataFns(ns);
  eval::addRowPlaneRegexs(ns);  // read through evalRows' RegexsRowScope
  eval::addRowPlaneSum(ns);     // running totals kept by evalRows' RunningSumRowScope
  eval::addScalarFn(ns, [&session](const std::string& name) -> std::optional<std::string> {
    const std::string* v = session.eval.scalars.find(name);
    return v ? std::optional<std::string>(*v) : std::nullopt;
  });
  return ns;
}

// A named scalar's binding in a row expression: a string scalar is its text, a
// numeric one its value. BUG-1419: `scalar s1 = "-3"` then `generate str5 g =
// s1 + "x"` is -3x in StataBE 19.5, and `replace s = s1 + "q"` -3q; the text
// read as the number -3 made both r(109), and "ab" was no binding at all.
std::optional<PyValue> namedScalarValue(const SessionState& session, const std::string& name,
                                        const std::string& value) {
  if (session.eval.isStringScalar(name)) return PyValue::str(value);
  if (auto f = eval::pythonFloat(value)) return PyValue::floating(*f);
  return std::nullopt;
}

// One column's value at row i, with the Stata missing/factor transform. SINGLE
// source of that transform: both the legacy per-row namespace bind and the
// slot-compiled fast path read row values through here, so the missing-NaN and
// factor-level rules cannot drift between the two paths.
PyValue colValue(const ColView& cv, int64_t i) {
  if (!cv.isFactor) {
    double v = cv.f64[i];
    // Missing f64 (NaN) -> the +inf sentinel so comparisons follow Stata's
    // missing-ordering (mirrors _StataNum's NaN-as-+inf, adapted to the
    // sentinel convention the C++ evaluator + exprfns::missing() use). An
    // extended missing keeps its kind (`.a` is the expression layer's kind
    // sentinel, exprfns.h stataMissingWithKind), so string(k) of a `.a` cell is
    // ".a" (w2x_fn_string_math fx_string1_cond) and `k == .` is false for it.
    if (std::isnan(v)) {
      const int kind = cecon::missing_kind(v);
      return PyValue::floating(kind > 0 ? eval::stataMissingWithKind(kind) : eval::kStataMissingSentinel);
    }
    return PyValue::floating(v);
  }
  int32_t code = cv.codes[i];
  if (code >= 0 && static_cast<size_t>(code) < cv.levels->size()) {
    return PyValue::str((*cv.levels)[static_cast<size_t>(code)]);
  }
  return PyValue::str("");  // missing factor -> "" (Stata string-missing)
}

// Bind one column's value at row i into the namespace (legacy per-row path).
void bindRowValue(eval::PyNamespace& ns, const std::string& name, const ColView& cv, int64_t i) {
  ns.consts[name] = colValue(cv, i);
  ++g_evalRowsMapWrites;  // instrumentation: one per-row ordered-map WRITE
  // FLOOR ① site class MAP-REBIND (counters.h). Billed per CELL at the width
  // really moved: 8 B for an f64, and for a factor the 4 B code PLUS the level
  // string this rebind materialises — flat-8 would understate exactly the waste
  // ① exists to catch (A1 §1.2). This is the site whose amplification is
  // ncols/(refcols+1): the legacy path rebinds EVERY column per row.
  if (!cv.isFactor) {
    counters::addRebindBytes(8);
  } else {
    int64_t levelBytes = 0;
    int32_t code = cv.codes[i];
    if (code >= 0 && static_cast<size_t>(code) < cv.levels->size()) {
      levelBytes = static_cast<int64_t>((*cv.levels)[static_cast<size_t>(code)].size());
    }
    counters::addRebindBytes(4 + levelBytes);
  }
}

// Both row loops below report only THAT a row failed. When the failure is a
// function that announced Stata's exact code on the `r(NNN): ...` channel
// (translateFn's r(198) for date("01jan2010"), pyeval's r(198) for tq(2010q5)),
// generate/replace must carry that code: they answered rc 1 where Stata answers
// 198 (w2y_fn_datetime fd_date1 gen_rc). The failing row is evaluated once more
// on the legacy binding, which keeps the error text. Failure path only.
// `*text` gets the words after the code, which count and assert print as Stata
// does (`count if s`: "type mismatch").
std::string rowFailureReason(SessionState& session,
                             const std::vector<std::pair<std::string, ColView>>& cols,
                             const std::string& expr, int64_t i, int64_t n, std::string* text) {
  eval::PyNamespace ns = baseNamespace(session);
  for (const auto& [sname, sval] : session.eval.scalars.items())
    if (auto v = namedScalarValue(session, sname, sval)) ns.consts[sname] = *v;
  for (const auto& [name, cv] : cols) bindRowValue(ns, name, cv, i);
  ns.consts["_n"] = PyValue::floating(static_cast<double>(i + 1));
  ns.consts["_N"] = PyValue::floating(static_cast<double>(n));
  ns.consts["_SMISS"] = PyValue::floating(eval::kStataMissingSentinel);
  const eval::PyEvalOutcome out = eval::evalPyExprDetailed(expr, ns);
  if (out.status != eval::PyEvalOutcome::Status::Raised || out.error.rfind("r(", 0) != 0)
    return std::string();
  const size_t close = out.error.find(')');
  if (close == std::string::npos || close <= 2) return std::string();
  const std::string digits = out.error.substr(2, close - 2);
  if (digits.find_first_not_of("0123456789") != std::string::npos) return std::string();
  std::string rest = out.error.substr(close + 1);
  if (!rest.empty() && rest[0] == ':') rest.erase(0, 1);
  *text = trim(rest);
  return "r(" + digits + ")";
}

}  // namespace

namespace row_eval_detail {

// The error block of a failed row evaluation, with the exact code when one was named.
// r(109) is Stata's bare "type mismatch" whichever row raised it (`keep if x > 0 |
// s`, `gen z = s & 1`: StataBE 19.5), as runCount already says it.
ojson rowErrBlock(const std::string& msg, const EvalRowsResult& er) {
  if (er.errReason.empty()) return errBlock(msg);
  return eval::errorBlock("engine_error", er.errReason == "r(109)" ? "type mismatch" : msg,
                          std::nullopt, std::nullopt, std::nullopt, er.errReason);
}

}  // namespace row_eval_detail

namespace {

// A condition whose value is a string is no truth value: Stata's r(109) type
// mismatch (StataBE `count if s`, `keep if s`, `generate z = 1 if s`). Python's
// truthiness answered them, a non-empty string being true.
EvalRowsResult& conditionIsString(EvalRowsResult& r, int64_t i) {
  r.ok = false;
  r.errMsg = "if condition evaluation failed at obs " + std::to_string(i + 1);
  r.errReason = "r(109)";
  r.errText = "type mismatch";
  return r;
}

}  // namespace

namespace row_eval_detail {

// _eval_rows: evaluate rhs (optional) + cond (optional) for every row.
// `rhsPy`/`condPy` are already rewrite_expr'd. If rhsPy is empty-optional,
// condition-only mode (count/filter): only `mask` is meaningful.
EvalRowsResult evalRows(SessionState& session, Datastore& ds,
                        const std::optional<std::string>& rhsPy,
                        const std::optional<std::string>& condPy) {
  EvalRowsResult r;
  int64_t n = ds.nRows();
  r.values.resize(static_cast<size_t>(n));
  r.valueIsSet.assign(static_cast<size_t>(n), false);
  r.mask.assign(static_cast<size_t>(n), true);
  // regexs() reads the regexm() state as it stood when the expression began on
  // the row (exprfns.cpp fnRegexs; w2c_functions.log regexs_whole :599).
  eval::RegexsRowScope regexsScope;
  // sum() runs over the rows (exprfns.cpp fnSumRow; BUG-1369).
  eval::RunningSumRowScope sumScope;
  // A row expression is a Stata expression: x/0, sqrt(-1), 0^-1 and an
  // out-of-domain function are `.`, never a failed row (StataDivisionScope;
  // BUG-1343, BUG-1358).
  eval::StataDivisionScope stataSemantics;

  // Decode all columns once.
  std::vector<std::pair<std::string, ColView>> cols;
  for (const auto& name : ds.names()) {
    ColView cv;
    if (ds.dtype(name) == "f64") {
      cv.isFactor = false;
      cv.f64 = ds.col(name).f64();
    } else {
      cv.isFactor = true;
      cv.codes = ds.col(name).codes();
      cv.levels = &ds.col(name).levels;
    }
    cols.emplace_back(name, cv);
  }
  // A hidden column (the e(sample) mask, a qualifier mask: datastore.h) is on no
  // varlist, so names() skips it; a condition can still name one. `assert`'s
  // claim reaches here as `(claim)==0 & ((__piet_qualif1__!=0))` when the
  // dispatcher's string-condition route masked its `if` (`assert g == "aa" if g
  // != ""` was r(1) "if condition evaluation failed"), and `count if e(sample) &
  // s == ""` reads the e(sample) column. Bind exactly the ones the text names.
  for (const std::optional<std::string>* e : {&rhsPy, &condPy}) {
    if (!e->has_value() || !namesHiddenColumn(**e)) continue;
    static const std::regex reHidden(R"(__piet_\w+)");
    for (auto it = std::sregex_iterator((*e)->begin(), (*e)->end(), reHidden);
         it != std::sregex_iterator(); ++it) {
      const std::string name = it->str();
      if (!isHiddenColumn(name) || !ds.has(name) || ds.dtype(name) != "f64") continue;
      bool bound = false;
      for (const auto& c : cols) bound = bound || c.first == name;
      if (bound) continue;
      ColView cv;
      cv.isFactor = false;
      cv.f64 = ds.col(name).f64();
      cols.emplace_back(name, cv);
    }
  }

  // === Slot-compiled fast path =============================================
  // Parse each expression ONCE and resolve every identifier ONCE to a slot /
  // const / fn (compilePreboundExpr), then run the row loop reading a flat
  // slot vector — no per-row std::map rebind, no per-row re-parse. This is the
  // whole point of the FLOOR stint: the legacy loop below did ~O(rows × cols)
  // ordered-map insertions plus a per-row parse.
  if (!g_forceLegacyEvalRows) {
    // Name -> ColView for slot filling (stable: `cols` is not modified below).
    std::map<std::string, const ColView*> colByName;
    for (const auto& [name, cv] : cols) colByName.emplace(name, &cv);

    // constNs = the CONSTANT bindings only (base math+Stata fns, the session
    // scalar() callback, and the numeric named scalars). Row-varying names are
    // compiled to slots and deliberately NOT placed here.
    eval::PyNamespace constNs = baseNamespace(session);
    for (const auto& [sname, sval] : session.eval.scalars.items())
      if (auto v = namedScalarValue(session, sname, sval)) constNs.consts[sname] = *v;
    // rowVars = every column + the three row specials. Making _n/_N/_SMISS
    // rowVars (rather than constNs consts) preserves the legacy precedence
    // EXACTLY: the old loop wrote columns and THEN the specials into ns.consts
    // each row, so specials beat columns and columns beat scalars. Here rowVars
    // beat constNs, and the filler plan below recognises the special names
    // before the column map — so a (Stata-impossible) column literally named
    // _n/_N/_SMISS still loses to the special, matching the old write order.
    std::set<std::string> rowVars;
    for (const auto& [name, cv] : cols) rowVars.insert(name);
    rowVars.insert("_n");
    rowVars.insert("_N");
    rowVars.insert("_SMISS");

    eval::PreboundExpr condCompiled, rhsCompiled;
    if (condPy.has_value()) condCompiled = eval::compilePreboundExpr(*condPy, constNs, rowVars);
    if (rhsPy.has_value()) rhsCompiled = eval::compilePreboundExpr(*rhsPy, constNs, rowVars);

    const bool condReady = !condPy.has_value() || condCompiled.ok();
    const bool rhsReady = !rhsPy.has_value() || rhsCompiled.ok();
    // Both present expressions must compile. A parse failure (e.g. a subscript
    // form `x[_n-1]` — the pyeval grammar has no `[`, A12 #6) falls through to
    // the legacy per-row path, which re-parses per row and reproduces the
    // identical failure. Nothing that compiles here differs in answer from the
    // legacy evaluator: it is the SAME parser and the SAME AST interpreter,
    // only the identifier binding is hoisted out of the row loop.
    if (condReady && rhsReady) {
      // Per-expression filler plan: for each of the expression's row slots, how
      // to produce this row's value. Special names are matched BEFORE the
      // column map, per the precedence note above.
      struct Filler {
        enum Kind { Column, SpecRowN, SpecConst } kind = Column;
        const ColView* col = nullptr;
        PyValue constVal;
      };
      auto planFillers = [&](const eval::PreboundExpr& ce) {
        std::vector<Filler> plan;
        plan.reserve(ce.rowSlotNames().size());
        for (const auto& nm : ce.rowSlotNames()) {
          Filler f;
          if (nm == "_n") {
            f.kind = Filler::SpecRowN;
          } else if (nm == "_N") {
            f.kind = Filler::SpecConst;
            f.constVal = PyValue::floating(static_cast<double>(n));
          } else if (nm == "_SMISS") {
            f.kind = Filler::SpecConst;
            f.constVal = PyValue::floating(eval::kStataMissingSentinel);
          } else {
            f.kind = Filler::Column;
            auto it = colByName.find(nm);
            f.col = (it != colByName.end()) ? it->second : nullptr;  // never null: rowVar⊇slots
          }
          plan.push_back(f);
        }
        return plan;
      };
      std::vector<Filler> condPlan = condPy.has_value() ? planFillers(condCompiled) : std::vector<Filler>{};
      std::vector<Filler> rhsPlan = rhsPy.has_value() ? planFillers(rhsCompiled) : std::vector<Filler>{};
      std::vector<PyValue> condSlots(condPlan.size());
      std::vector<PyValue> rhsSlots(rhsPlan.size());
      auto fillSlots = [](const std::vector<Filler>& plan, std::vector<PyValue>& slots, int64_t i) {
        for (size_t s = 0; s < plan.size(); ++s) {
          const Filler& f = plan[s];
          switch (f.kind) {
            case Filler::Column:
              slots[s] = (f.col != nullptr) ? colValue(*f.col, i)
                                            : PyValue::floating(eval::kStataMissingSentinel);
              break;
            case Filler::SpecRowN:
              slots[s] = PyValue::floating(static_cast<double>(i + 1));
              break;
            case Filler::SpecConst:
              slots[s] = f.constVal;
              break;
          }
        }
      };

      counters::strmutPhases().evalrowsCalls += 1;
      counters::strmutPhases().rows += n;
      PhaseTimer _pt(&counters::strmutPhases().evalrowsNs);
      for (int64_t i = 0; i < n; ++i) {
        if (condPy.has_value()) {
          fillSlots(condPlan, condSlots, i);
          regexsScope.beginExpression();
          sumScope.beginExpression(0);
          auto outcome = condCompiled.eval(condSlots);
          if (!outcome.has_value()) {
            r.ok = false;
            r.errMsg = "if condition evaluation failed at obs " + std::to_string(i + 1);
            r.errReason = rowFailureReason(session, cols, *condPy, i, n, &r.errText);
            return r;
          }
          if (outcome->isStr()) return conditionIsString(r, i);
          bool truth = outcome->truthy();
          r.mask[static_cast<size_t>(i)] = truth;
          if (!truth) continue;
        }
        if (!rhsPy.has_value()) continue;  // condition-only mode
        fillSlots(rhsPlan, rhsSlots, i);
        regexsScope.beginExpression();
        sumScope.beginExpression(1);
        auto outcome = rhsCompiled.eval(rhsSlots);
        if (!outcome.has_value()) {
          r.ok = false;
          r.errMsg = "expression evaluation failed at obs " + std::to_string(i + 1);
          r.errReason = rowFailureReason(session, cols, *rhsPy, i, n, &r.errText);
          return r;
        }
        r.values[static_cast<size_t>(i)] = *outcome;
        r.valueIsSet[static_cast<size_t>(i)] = true;
      }
      return r;
    }
  }

  // === Legacy per-row namespace-bind path ==================================
  // Fallback for anything the compiler declined (and the forced-legacy test
  // hook). Behaviour is verbatim the original evalRows: rebind every column by
  // NAME into ns.consts each row and re-parse each expression each row. It is
  // both the safety net and the answer oracle the mechanism gate compares the
  // slot path against — so it must stay byte-for-byte equivalent in result.
  eval::PyNamespace ns = baseNamespace(session);
  // Pre-bind named scalars (numeric) once; bound BEFORE columns each row so a
  // same-named column wins the per-row rebind (mirrors _eval_rows binding
  // columns after _BASE_NS/fns/scalars).
  std::map<std::string, PyValue> scalarConsts;
  for (const auto& [sname, sval] : session.eval.scalars.items())
    if (auto v = namedScalarValue(session, sname, sval)) scalarConsts[sname] = *v;
  for (const auto& [k, v] : scalarConsts) ns.consts[k] = v;

  // Same phase, legacy arm — billed too, so `evalrows` means "the row loop"
  // regardless of which arm ran (evalrows_map_writes already says which).
  counters::strmutPhases().evalrowsCalls += 1;
  counters::strmutPhases().rows += n;
  PhaseTimer _ptLegacy(&counters::strmutPhases().evalrowsNs);
  for (int64_t i = 0; i < n; ++i) {
    for (const auto& [name, cv] : cols) bindRowValue(ns, name, cv, i);
    ns.consts["_n"] = PyValue::floating(static_cast<double>(i + 1));
    ns.consts["_N"] = PyValue::floating(static_cast<double>(n));
    ns.consts["_SMISS"] = PyValue::floating(eval::kStataMissingSentinel);
    g_evalRowsMapWrites += 3;  // the three special writes (columns counted in bindRowValue)

    if (condPy.has_value()) {
      regexsScope.beginExpression();
      sumScope.beginExpression(0);
      auto outcome = eval::evalPyExpr(*condPy, ns);
      if (!outcome.has_value()) {
        r.ok = false;
        r.errMsg = "if condition evaluation failed at obs " + std::to_string(i + 1);
        r.errReason = rowFailureReason(session, cols, *condPy, i, n, &r.errText);
        return r;
      }
      if (outcome->isStr()) return conditionIsString(r, i);
      bool truth = outcome->truthy();
      r.mask[static_cast<size_t>(i)] = truth;
      if (!truth) continue;
    }
    if (!rhsPy.has_value()) continue;  // condition-only mode
    regexsScope.beginExpression();
    sumScope.beginExpression(1);
    auto outcome = eval::evalPyExpr(*rhsPy, ns);
    if (!outcome.has_value()) {
      r.ok = false;
      r.errMsg = "expression evaluation failed at obs " + std::to_string(i + 1);
      r.errReason = rowFailureReason(session, cols, *rhsPy, i, n, &r.errText);
      return r;
    }
    r.values[static_cast<size_t>(i)] = *outcome;
    r.valueIsSet[static_cast<size_t>(i)] = true;
  }
  return r;
}

// _coerce_str: one evaluated value -> Stata string.
std::string coerceStr(const PyValue& v) {
  if (v.isBool()) return v.boolVal() ? "1" : "0";
  if (v.isStr()) return v.strVal();
  if (v.isInt()) return std::to_string(v.intVal());
  // float
  double d = v.floatVal();
  if (!std::isfinite(d) || std::fabs(d) >= eval::kStataMissingSentinel) return "";  // missing -> ""
  if (eval::pyIsInteger(d) && std::fabs(d) < 1e15) {
    return eval::pythonIntStrFromDouble(d);
  }
  // Non-integer numeric in a string context: Stata's %g-like general format
  // (string(1/3)=".33333333"), NOT full-precision repr — reuse the exprfns helper.
  return eval::gFormat(d);
}

// _coerce_num: one evaluated value -> Stata numeric (double; NaN = missing).
double coerceNum(const PyValue& v) {
  if (v.isBool()) return v.boolVal() ? 1.0 : 0.0;
  if (v.isStr()) {
    auto f = eval::pythonFloat(v.strVal());
    return f ? *f : std::nan("");
  }
  double d = v.numAsDouble();
  // The band |d| >= 2^1023 is the row-binding stand-in for missing (and NaN /
  // inf are missing too); write it back as NaN, the kind kept in the payload
  // (data/missing.h). Only an exact kind double keeps its kind.
  if (!std::isfinite(d) || std::fabs(d) >= eval::kStataMissingSentinel) {
    return cecon::stata_missing(eval::stataExtendedMissingKind(d).value_or(0));
  }
  return d;
}

// CSV-escape a field (RFC4180 doubled-quote), matching postfile.cpp's convention.
std::string csvEscape(const std::string& text) {
  std::string esc;
  esc.reserve(text.size() + 2);
  esc += '"';
  for (char c : text) {
    if (c == '"') esc += '"';
    esc += c;
  }
  esc += '"';
  return esc;
}

// Format an f64 value for a CSV numeric field: blank for missing, else a
// full-precision round-trippable decimal (cobra's convert re-parses this to
// the same double). Uses the shortest round-trip repr the M1 helper provides.
std::string csvNum(double v) {
  if (std::isnan(v)) return "";  // blank -> missing on cobra convert
  return eval::pythonFloatRepr(v);
}

}  // namespace row_eval_detail

// ===========================================================================
// Public API

// FLOOR campaign instrumentation + test hook — see strmutate.h.
int64_t evalRowsMapWriteCount() { return g_evalRowsMapWrites; }
void resetEvalRowsMapWriteCount() { g_evalRowsMapWrites = 0; }
void setForceLegacyEvalRowsForTest(bool on) { g_forceLegacyEvalRows = on; }

bool needsStringEval(SessionState& session, const std::string& varname, const std::string& rhs,
                     const std::optional<std::string>& ifExpr, bool strTyped, bool isReplace) {
  if (strTyped) return true;
  std::set<std::string> factorCols = factorColumns(session);
  if (isReplace && factorCols.count(varname)) return true;
  std::string cond = ifExpr.value_or("");
  for (const std::string& part : {rhs, cond}) {
    if (part.empty()) continue;
    if (usesStringFn(part)) return true;
    if (hasStringLiteral(part)) return true;
    if (referencesFactor(part, factorCols)) return true;
  }
  return false;
}

bool needsStringCount(SessionState& session, const std::optional<std::string>& ifExpr) {
  if (!ifExpr.has_value() || ifExpr->empty()) return false;
  const std::string& e = *ifExpr;
  if (usesStringFn(e)) return true;
  if (hasStringLiteral(e)) return true;
  return referencesFactor(e, factorColumns(session));
}

// ---------------------------------------------------------------------------
// BUG-370: time-series operators in a STRING-layer `if`.
//
// This file's evaluator (`evalRows`) is a row-wise interpreter with its own
// grammar — it is what handles a predicate the cobra expression parser cannot,
// e.g. one naming a factor column or calling strlen(). It has no notion of an
// L./F./D./S. operator, so `replace s = "y" if L.x > 0.5` died with
// "if condition evaluation failed at obs 1": each half of the predicate was
// understood by the layer that could not evaluate the other half.
//
// The fix keeps both layers doing what they are good at. The ENGINE resolves the
// operators into ordinary numeric columns; this evaluator then sees a predicate
// with no operators left in it. Mixed predicates (`strlen(s)>2 & L.x>0`) fall out
// for free, which is the point — neither layer could have done that one alone.
//
// DEVIATION FROM THE PLAN (recorded deliberately). The plan had the temp columns
// materialised onto the SESSION HEAD (`session.load()`) and dropped again after
// the evaluation. Reading the four call sites made a simpler shape available: the
// temps go into a scratch frame that is never adopted as the head, and the
// evaluator is pointed at THAT. Nothing to drop, so:
//   * the user's frame is never mutated, not even transiently — so a temp column
//     can never reach a `mutationResult` varlist, which was the plan's own named
//     failure mode #3;
//   * `count` needs no `datasetChanged` save/restore (plan step B-3) — it never
//     writes;
//   * no compensating drop, so no second-order failure when the compensation is
//     the thing that fails (the risk the plan's dropTsTemps comment wrestled with).
// The cost is that a TS-carrying predicate does not take the §38 column
// projection — it reads a full scratch frame instead. That is a slow path for a
// rare predicate, traded for not touching the user's data. Correctness over the
// optimization, which is the same call `filterRowsIf`'s own fallback discipline
// makes two functions below.

// BUG-412 review: the same route serves the other forms of an `if` this file's
// evaluator cannot read and the engine can -- an explicit subscript on a numeric
// variable (`x[_n-1]`, `x[1]`) and a call of a function that reads across rows or
// draws a random number (sum(), runiform(), ...), which the row plane deliberately
// lacks (exprfns.cpp addStataMacroOnlyFns). Since `assert` counts a string claim
// row-wise, `assert g == "aa" | x[_n-1] >= 1` was r(1) "if condition evaluation
// failed at obs 1" where StataBE 19.5 says rc 0, and `count if g == "aa" | sum(x)
// > 2` was r(1) too. A subscript on a string variable is left to the evaluator:
// the engine has no string column to hand back. Outermost forms only, in order,
// quote-aware, like extractTsTokens.
static std::vector<translate::TsTokenRef> extractEngineOnlyTokens(SessionState& session,
                                                                  const std::string& expr) {
  static const std::set<std::string> kAcrossRowFns = {"sum", "runiform", "runiformint",
                                                      "rnormal", "uniform"};
  std::vector<translate::TsTokenRef> out;
  std::set<std::string> numeric;  // read only when a `[` makes it matter
  if (expr.find('[') != std::string::npos) {
    if (const auto* card = session.shadowVarlistIfCurrent()) {
      for (const auto& info : *card)
        if (info.dtype == "f64") numeric.insert(info.name);
    } else if (Datastore* ds = session.datastore()) {
      for (const auto& nm : ds->names())
        if (ds->dtype(nm) == "f64") numeric.insert(nm);
    }
  }
  bool inStr = false;
  for (size_t i = 0; i < expr.size(); ++i) {
    if (expr[i] == '"') {
      inStr = !inStr;
      continue;
    }
    if (inStr || !cecon::ident::isIdentStartByte(expr[i])) continue;
    if (i > 0 && (cecon::ident::isIdentByte(expr[i - 1]) || expr[i - 1] == '.')) continue;
    size_t j = i;
    while (j < expr.size() && cecon::ident::isIdentByte(expr[j])) ++j;
    const std::string name = expr.substr(i, j - i);
    size_t k = j;
    while (k < expr.size() && (expr[k] == ' ' || expr[k] == '\t')) ++k;
    const char open = k < expr.size() ? expr[k] : '\0';
    if (!(open == '[' && numeric.count(name)) && !(open == '(' && kAcrossRowFns.count(name))) {
      i = j - 1;
      continue;
    }
    const char close = open == '[' ? ']' : ')';
    int depth = 0;
    bool quoted = false;
    size_t e = k;
    for (; e < expr.size(); ++e) {
      if (expr[e] == '"') quoted = !quoted;
      if (quoted) continue;
      if (expr[e] == open) ++depth;
      if (expr[e] == close && --depth == 0) break;
    }
    if (e >= expr.size()) break;  // unbalanced: the evaluator's own error says so
    translate::TsTokenRef ref;
    ref.token = expr.substr(i, e + 1 - i);
    ref.begin = i;
    ref.end = e + 1;
    out.push_back(std::move(ref));
    i = e;
  }
  return out;
}

// `inRange` is the command's `in a/b`, when it has one: Stata evaluates the `if`
// only on the rows in range, so a running sum() starts at the range's first row
// and a draw is taken for those rows alone (`count if s == "" & sum(x) > 4 in 3/4`
// is 1 on x = 1..4, StataBE 19.5; over the whole frame it would be 2).
TsIfPrep materializeTsIf(SessionState& session, const std::string& ifExpr,
                         const std::optional<std::string>& inRange) {
  TsIfPrep prep;
  prep.rewrittenIf = ifExpr;
  auto toks = translate::extractTsTokens(ifExpr);
  if (std::vector<translate::TsTokenRef> more = extractEngineOnlyTokens(session, ifExpr);
      !more.empty()) {
    // An operator inside one of them (`sum(L.x)`) goes to the engine with it.
    for (const auto& t : toks) {
      bool inside = false;
      for (const auto& m : more) inside = inside || (t.begin >= m.begin && t.end <= m.end);
      if (!inside) more.push_back(t);
    }
    std::sort(more.begin(), more.end(),
              [](const translate::TsTokenRef& a, const translate::TsTokenRef& b) {
                return a.begin < b.begin;
              });
    toks = std::move(more);
  }
  if (toks.empty()) return prep;  // the overwhelmingly common case: no-op
  if (!session.currentHbin().has_value()) {
    prep.failed = true;
    prep.errMsg = "no dataset in memory";
    return prep;
  }
  // One temp per OCCURRENCE, not per distinct token. `L.x > 0 & L.x < 5` costs a
  // duplicate column and saves this code a dedup pass whose only payoff would be
  // one column in a scratch file nobody keeps.
  const auto* card = session.shadowVarlistIfCurrent();
  auto nameTaken = [&card](const std::string& n) {
    if (card == nullptr) return false;
    for (const auto& info : *card)
      if (info.name == n) return true;
    return false;
  };
  std::vector<std::string> names;
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin()};
  for (size_t k = 0; k < toks.size(); ++k) {
    // `__piet_` is not a name any Stata identifier check accepts, so a user
    // column cannot be spelled this way; the card is consulted anyway, because
    // an earlier scratch column surviving a crash is cheaper to step over than
    // to debug.
    std::string nm = "__piet_tsif" + std::to_string(k);
    for (int bump = 0; nameTaken(nm); ++bump) nm = "__piet_tsif" + std::to_string(k) + "_" + std::to_string(bump);
    names.push_back(nm);
    args.push_back("--gen");
    args.push_back(nm + "=" + toks[k].token);
  }
  const std::string outPath = nextPath(session.workdir(), "tsif") + ".hbin";
  args.push_back("--out");
  args.push_back(outPath);
  if (inRange.has_value() && !inRange->empty()) {
    std::optional<int64_t> rows = session.residentRowsIfCurrent();
    if (!rows.has_value())
      if (Datastore* head = session.datastore()) rows = head->nRows();
    // A bound that does not resolve is the caller's to refuse, in Stata's words.
    const handlers::InRangeBounds rr = handlers::parseInRangeBounds(*inRange, rows.value_or(0));
    if (rr.ok) {
      args.push_back("--gen-in");
      args.push_back(std::to_string(rr.a) + "/" + std::to_string(rr.b));
    }
  }
  // The tsset/xtset metadata does not live in the .hbin, so it rides the call —
  // gated on the predicate, which by construction carries an operator here. So
  // does the session's draw stream, which a runiform() takes its draws from and
  // moves on, as `generate` does.
  args = translate::injectTsContextForExpr(args, ifExpr, session);
  args = translate::injectRngContextForExpr(args, ifExpr, session);
  // Deliberately NOT ResidentEligible: this --out is read back AS A FILE by the
  // caller's Datastore, so it has to be real bytes rather than a virtual head.
  CobraRunResult cr = runCobra(args);
  if (!cr.ok) {
    cecon::native_unlink(outPath);
    CobraError ce = errorFromRaw(cr);
    prep.failed = true;
    prep.errMsg = ce.what();          // e.g. the honest ts_no_tsset when nothing is set
    prep.errCode = ce.code;
    return prep;
  }
  translate::absorbRngDraws(nlohmann::json::parse(cr.stdout_text, nullptr, false), session);
  // Substitute back-to-front so the earlier tokens' offsets stay valid.
  for (size_t k = toks.size(); k-- > 0;)
    prep.rewrittenIf.replace(toks[k].begin, toks[k].end - toks[k].begin, names[k]);
  prep.evalPath = outPath;
  return prep;
}

bool needsConditionMask(SessionState& session, const std::string& cond) {
  if (cond.empty() || namesHiddenColumn(cond)) return false;
  // Only a whole condition: its parentheses balance and no comma stands outside
  // them. A layered graph line hands the tokenizer `id<=32, xline(...)) (line ...`
  // as its `if` (w2ai_graph_family gf_tw_015), which is no condition at all.
  {
    const std::string masked = maskStringLiterals(cond);
    int depth = 0;
    for (char c : masked) {
      if (c == '(') ++depth;
      if (c == ')' && --depth < 0) return false;
      if (c == ',' && depth == 0) return false;
    }
    if (depth != 0) return false;
  }
  if (usesStringFn(cond)) return true;
  // A compare against the empty string: the literal `""` standing alone, not the
  // inside of a longer literal.
  static const std::regex reEmptyLit(R"re((^|[^"`])""([^"']|$))re");
  if (std::regex_search(cond, reEmptyLit)) return true;
  // Any other use of a string column. The engine's factor compare reads
  // `s ==/!= "text"` in either operand order and nothing else: `missing(s)`,
  // `inlist(s, "b", "c")`, `s > "a"`, `s == t` and `s[_n-1]` are refused
  // ("string/factor variable used where a number is required"), where StataBE
  // 19.5 serves them (`by g: count if missing(s)`, `summarize x if s > "a"`;
  // measured 2026-10-29). Take the compares it reads out and look for a string
  // column in what is left.
  const std::set<std::string> factorCols = factorColumns(session);
  if (factorCols.empty()) return false;
  static const std::string name = eval::kRxName;
  static const std::regex reFactorCompare(
      name + R"re(\s*(?:==|!=|~=)\s*""|""\s*(?:==|!=|~=)\s*)re" + name + "(?!" +
      eval::kRxNameChar + R"re(|[[(]))re");
  const std::string rest =
      std::regex_replace(maskStringLiterals(cond), reFactorCompare, std::string("0"));
  return referencesFactor(rest, factorCols);
}

bool namesObsPosition(const std::string& expr) {
  const std::string masked = maskStringLiterals(expr);
  for (size_t p = masked.find('_'); p != std::string::npos; p = masked.find('_', p + 1)) {
    if (p + 1 >= masked.size() || (masked[p + 1] != 'n' && masked[p + 1] != 'N')) continue;
    if (p > 0 && cecon::ident::isIdentByte(masked[p - 1])) continue;
    if (p + 2 < masked.size() && cecon::ident::isIdentByte(masked[p + 2])) continue;
    return true;
  }
  return false;
}

}  // namespace piet::strmutate
