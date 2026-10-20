// strmutate.cpp — C++ port of piet/backend/piet/strmutate.py.
// See strmutate.h for the design rationale. Reuses the M1 evaluator
// (pyeval/exprfns/pyvalue), the same machinery loops.cpp::evalBoolExpr uses.
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
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/counters.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"  // MECHOPT W3: the resident arm folds through it
#include "piet/strutil.h"
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)

namespace piet::strmutate {

using ojson = nlohmann::ordered_json;
using eval::PyValue;

namespace {

// Stata missing = +infinity ordering sentinel (same constant loops.cpp's
// evalBoolExpr uses so missing f64 comparisons behave Stata-faithfully via
// ordinary `>`/`<`, and exprfns::fnMissing recognises it — exprfns.cpp:214).
constexpr double kMissingSentinel = 8.988465674311579e+307;

// FLOOR campaign instrumentation + test hook — see strmutate.h. Single-thread
// selftest reads these; production only ever increments the counter on the
// legacy fallback path (the slot path never writes a per-row map at all).
int64_t g_evalRowsMapWrites = 0;
bool g_forceLegacyEvalRows = false;

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

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `toLower(...)` call sites
// didn't need touching. Pure dedup, no behavior change.
std::string toLower(std::string s) { return strutil::lower(std::move(s)); }

std::string nextPath(const std::string& workdir, const std::string& prefix) {
  auto ms = static_cast<long long>(std::time(nullptr)) * 1000;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%lld", ms);
  return workdir + "/" + prefix + "_" + buf + scratchSeqSuffix();
}

ojson errBlock(const std::string& msg, const std::string& code = "engine_error") {
  return eval::engineError(msg, code);
}

// BUG-124: runCobra() does not throw — failures come back in the RESULT (the
// serve router replaced the throwing one-shot in Stage 1, so this file's old
// `catch (CobraError)` blocks were dead and convert failures were silently
// ignored; a partially-written --out then passed the exists() check and got
// mmap'd → SIGSEGV in buildVarInfo). Manual CobraError build from stderr —
// file-local copy per the established one-copy-per-TU convention (see
// egen.cpp / handlers_data_verbs.cpp `errorFromRaw`).
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

// --- string-fn / string-literal / factor-reference detection (strmutate.py's
//     _STRING_FN_NAMES / _uses_string_fn / _has_string_literal /
//     _references_factor) -----------------------------------------------------

const std::set<std::string>& stringFnNames() {
  static const std::set<std::string> s = {
      "lower", "upper", "strtrim", "trim", "ltrim", "rtrim", "substr",
      "strlen", "length", "strpos", "word", "real", "regexm", "regexr",
      "subinstr", "proper", "string", "char", "abbrev",
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
  static const std::regex reFnCall(R"(\b([A-Za-z_]\w*)\s*\()");
  auto begin = std::sregex_iterator(masked.begin(), masked.end(), reFnCall);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
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
  static const std::regex reIdent(R"([A-Za-z_]\w*)");
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
//     _rewrite_expr; identical scheme to loops.cpp's evalBoolExpr, replicated
//     here per this codebase's per-TU convention rather than sharing loops.cpp's
//     anonymous-namespace helpers) --------------------------------------------

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
  py = replaceLoneChar(py, '|', '|', " or ");
  py = replaceLoneChar(py, '&', '&', " and ");
  py = std::regex_replace(py, std::regex(R"(~=)"), "!=");
  py = std::regex_replace(py, std::regex(R"(!(?!=))"), " not ");
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
  // Bare `.` system-missing literal -> sentinel.
  py = replaceBareDot(py, "8.988465674311579e+307");

  // Restore string placeholders.
  for (size_t i = 0; i < placeholders.size(); ++i) {
    std::string key = "__STR" + std::to_string(i) + "__";
    size_t pos = py.find(key);
    if (pos != std::string::npos) py.replace(pos, key.size(), placeholders[i]);
  }
  return py;
}

// --- T7b phase timer (report-only; see counters.h StrmutPhases) ------------
// Scoped stopwatch billing one PIET-side phase of the string gen/replace path.
// Deliberately times the SUB-INTERVALS only (the row loop, not evalRows'
// once-per-call column decode + expression compile), so that
//   verb wall - engine_ms - (evalrows+outbuild+csvwrite+reload)
// is a genuine residual rather than a definition — that residual is the check
// that this decomposition is complete, and it is what would expose the
// per-call setup if it were ever non-negligible.
struct PhaseTimer {
  int64_t* sink;
  std::chrono::steady_clock::time_point t0;
  explicit PhaseTimer(int64_t* s) : sink(s), t0(std::chrono::steady_clock::now()) {}
  ~PhaseTimer() {
    *sink += std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::steady_clock::now() - t0)
                 .count();
  }
};

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
  eval::addScalarFn(ns, [&session](const std::string& name) -> std::optional<std::string> {
    const std::string* v = session.eval.scalars.find(name);
    return v ? std::optional<std::string>(*v) : std::nullopt;
  });
  return ns;
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
    // sentinel convention the C++ evaluator + exprfns::missing() use).
    return PyValue::floating(std::isnan(v) ? kMissingSentinel : v);
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

// Per-row evaluation result: one produced value per row + the condition mask.
struct EvalRowsResult {
  bool ok = true;
  std::string errMsg;
  std::vector<PyValue> values;    // meaningful only where mask is true
  std::vector<bool> valueIsSet;   // false where rhs was null / cond false
  std::vector<bool> mask;         // condition truth per row (all true if no cond)
};

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
    for (const auto& [sname, sval] : session.eval.scalars.items()) {
      auto f = eval::pythonFloat(sval);
      if (f) constNs.consts[sname] = PyValue::floating(*f);
    }
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
            f.constVal = PyValue::floating(kMissingSentinel);
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
                                            : PyValue::floating(kMissingSentinel);
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
          auto outcome = condCompiled.eval(condSlots);
          if (!outcome.has_value()) {
            r.ok = false;
            r.errMsg = "if condition evaluation failed at obs " + std::to_string(i + 1);
            return r;
          }
          bool truth = outcome->truthy();
          r.mask[static_cast<size_t>(i)] = truth;
          if (!truth) continue;
        }
        if (!rhsPy.has_value()) continue;  // condition-only mode
        fillSlots(rhsPlan, rhsSlots, i);
        auto outcome = rhsCompiled.eval(rhsSlots);
        if (!outcome.has_value()) {
          r.ok = false;
          r.errMsg = "expression evaluation failed at obs " + std::to_string(i + 1);
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
  for (const auto& [sname, sval] : session.eval.scalars.items()) {
    auto f = eval::pythonFloat(sval);
    if (f) scalarConsts[sname] = PyValue::floating(*f);
  }
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
    ns.consts["_SMISS"] = PyValue::floating(kMissingSentinel);
    g_evalRowsMapWrites += 3;  // the three special writes (columns counted in bindRowValue)

    if (condPy.has_value()) {
      auto outcome = eval::evalPyExpr(*condPy, ns);
      if (!outcome.has_value()) {
        r.ok = false;
        r.errMsg = "if condition evaluation failed at obs " + std::to_string(i + 1);
        return r;
      }
      bool truth = outcome->truthy();
      r.mask[static_cast<size_t>(i)] = truth;
      if (!truth) continue;
    }
    if (!rhsPy.has_value()) continue;  // condition-only mode
    auto outcome = eval::evalPyExpr(*rhsPy, ns);
    if (!outcome.has_value()) {
      r.ok = false;
      r.errMsg = "expression evaluation failed at obs " + std::to_string(i + 1);
      return r;
    }
    r.values[static_cast<size_t>(i)] = *outcome;
    r.valueIsSet[static_cast<size_t>(i)] = true;
  }
  return r;
}

// _result_is_string: string-valued if ANY produced (cond-true) value is a str.
bool resultIsString(const EvalRowsResult& er) {
  for (size_t i = 0; i < er.values.size(); ++i) {
    if (er.mask[i] && er.valueIsSet[i] && er.values[i].isStr()) return true;
  }
  return false;
}

// _coerce_str: one evaluated value -> Stata string.
std::string coerceStr(const PyValue& v) {
  if (v.isBool()) return v.boolVal() ? "1" : "0";
  if (v.isStr()) return v.strVal();
  if (v.isInt()) return std::to_string(v.intVal());
  // float
  double d = v.floatVal();
  if (std::isnan(d) || d >= kMissingSentinel) return "";  // missing -> ""
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
  // The sentinel is the row-binding stand-in for missing; write it back as NaN.
  if (d >= kMissingSentinel) return std::nan("");
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

// _write_new: write a fresh hbin with `varname` set from the evaluated rows.
// C++ path uses a CSV round-trip through `cobra convert` (piet/native has no
// hbin-write API; this mirrors postfile.cpp::runPostclose's established
// pattern). replace_existing: rows where cond is false keep the column's
// existing value (replace); else become missing (gen).

ojson writeNew(SessionState& session, Datastore& ds, const std::string& varname,
               const EvalRowsResult& er, bool isString, bool replaceExisting,
               const std::string& action, const std::vector<std::string>& newVars,
               const std::string& dataPath) {
  int64_t n = ds.nRows();

  // Existing column view (for replace's cond-false keep-original path).
  bool varExists = ds.has(varname);
  bool existingIsFactor = varExists && ds.dtype(varname) != "f64";
  const double* existingF64 = (varExists && !existingIsFactor) ? ds.col(varname).f64() : nullptr;
  const int32_t* existingCodes = existingIsFactor ? ds.col(varname).codes() : nullptr;
  const std::vector<std::string>* existingLevels =
      existingIsFactor ? &ds.col(varname).levels : nullptr;

  // Compute the new column's per-row output.
  std::vector<std::string> outStrings;  // isString path
  std::vector<double> outNums;          // numeric path
  counters::strmutPhases().calls += 1;
  {
  PhaseTimer _ptOut(&counters::strmutPhases().outbuildNs);
  if (isString) {
    outStrings.resize(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
      size_t si = static_cast<size_t>(i);
      if (er.mask[si] && er.valueIsSet[si]) {
        outStrings[si] = coerceStr(er.values[si]);
      } else if (replaceExisting && existingIsFactor) {
        int32_t code = existingCodes[i];
        outStrings[si] = (code >= 0 && static_cast<size_t>(code) < existingLevels->size())
                             ? (*existingLevels)[static_cast<size_t>(code)]
                             : "";
      } else if (replaceExisting && existingF64 != nullptr) {
        double fv = existingF64[i];
        outStrings[si] = std::isnan(fv) ? "" : coerceStr(PyValue::floating(fv));
      } else {
        outStrings[si] = "";  // gen, cond false -> missing string
      }
    }
  } else {
    outNums.resize(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
      size_t si = static_cast<size_t>(i);
      if (er.mask[si] && er.valueIsSet[si]) {
        outNums[si] = coerceNum(er.values[si]);
      } else if (replaceExisting && existingF64 != nullptr) {
        outNums[si] = existingF64[i];
      } else {
        outNums[si] = std::nan("");
      }
    }
  }
  }  // end outbuild phase

  // Hand the engine's single-column `cobra setcol` verb just the one new/changed
  // column instead of re-serializing the WHOLE frame to CSV + `cobra convert`
  // (which re-factorizes every untouched factor column). The per-row
  // outStrings/outNums above already carry every gen/replace semantic; every
  // OTHER column/dictionary byte is preserved. isString => --dtype factor: a
  // genuine "" and a missing row both become an empty payload line -> setcol
  // missing (code -1), the SAME mapping the old `cobra convert` empty-factor-
  // cell path produced, so no ""-level is introduced. target present (replace)
  // => in-place patch; target new (gen) => --new appends.
  bool targetPresent = ds.has(varname);
  std::string valuesPath = nextPath(session.workdir(), "strmut") + ".csv";
  {
    PhaseTimer _ptCsv(&counters::strmutPhases().csvwriteNs);
    std::ofstream csv(cecon::to_native_path(valuesPath), std::ios::binary);
    if (!csv) return errBlock("gen/replace: could not open scratch CSV for write");
    for (int64_t i = 0; i < n; ++i) {
      size_t si = static_cast<size_t>(i);
      if (isString) csv << csvEscape(outStrings[si]);
      else csv << csvNum(outNums[si]);
      csv << "\n";
    }
    // BUG-124 sibling (BUG-109's writer-side precedent applied to this PIET
    // writer): an ofstream swallows ENOSPC/IO errors into badbit silently — a
    // short payload would setcol into an honest row-count mismatch (or, worse, a
    // silently truncated column). Flush and check before handing it to the engine.
    csv.flush();
    if (!csv) {
      cecon::native_unlink(valuesPath);
      return errBlock("gen/replace: failed writing scratch CSV (disk full or I/O error)");
    }
  }
  // FLOOR (1) site class IPC, `semantic_zero` class (counters.h): bill the
  // scratch CSV this process wrote for the single-column `cobra setcol` below
  // (destring/tostring precedent, handlers_data_verbs.cpp) — the `--out` hbin
  // is billed once at the COLUMNAR site via Datastore, not here.
  {
    std::error_code cec;
    auto csvSz = std::filesystem::file_size(cecon::to_native_path(valuesPath), cec);
    counters::addIpcBytes(cec ? 0 : static_cast<int64_t>(csvSz), /*setcolShaped=*/true);
  }

  // MECHOPT W3 (STRMUT-RESIDENT): the C4-L4 setcol residency switch, exactly
  // the destring/tostring/encode/clonevar resolution — one env for the
  // C2-specific escape, the C4-family escape disables it alongside. On the
  // resident arm the engine's cmd_setcol runs over the frame-run channel
  // (acquire_data serves the cached resident view, commit_frame stages instead
  // of writing), so the per-call FULL-FRAME `--out` hbin write — 2,560 of them
  // per acceptance-corpus run, 283 s of engine wait — never happens, and
  // neither does the PIET-side re-open chain (rl_load) that used to read it
  // back. The entry side is deliberately unchanged: evalRows reads real row
  // values, so the head materialization that built `ds` stays.
  const bool residentSetcol =
      std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr && !residentReadDisabled();

  std::string outHbin = nextPath(session.workdir(), "strmut") + ".hbin";
  std::vector<std::string> args = {"setcol",   "--data",   dataPath,
                                   "--var",    varname,    "--values",
                                   valuesPath, "--dtype",  isString ? "factor" : "f64",
                                   "--out",    outHbin};
  if (!targetPresent) args.push_back("--new");
  std::optional<SessionState::ResidentEligible> setcolResident;
  if (residentSetcol) setcolResident.emplace(session);
  CobraRunResult cr = runCobra(args);
  cecon::native_unlink(valuesPath);
  if (!cr.ok) {
    // BUG-124 (the crash): a failed setcol (ENOSPC, engine error) can leave a
    // partially-written outHbin (the engine's writer creates the file before it
    // can detect the failure). The old code discarded runCobra's result (the
    // catch was dead), so a torn file passed the exists() check below, got
    // load()ed + mmap'd, and the next varlist scan walked off the mapping
    // (SIGSEGV KERN_INVALID_ADDRESS at region end — the fair-run .ips).
    // Surface the engine's real error and remove the torn partial.
    cecon::native_unlink(outHbin);
    return errBlock(std::string("gen/replace: error writing hbin: ") + errorFromRaw(cr).what());
  }
  // BUG-124 torn-write guard: STATELESS ARM ONLY. The resident arm never writes
  // an --out file (the router strips it; the engine stages a pending frame), so
  // a native_exists probe there would turn every resident string-gen into a
  // false "disk full" — the destring precedent's exact clause.
  if (!residentSetcol && !cecon::native_exists(outHbin)) {
    return errBlock("gen/replace: failed writing output dataset (disk full or I/O error)");
  }

  if (residentSetcol) {
    // MECHOPT W3, resident arm: record the virtual head and fold the delta
    // through mutationResult — the same seam every other resident mutation
    // rides (tryResidentMutationCard consumes the engine's vars_delta; no
    // datastore re-materialization, no varlist rebuild by hand). Calling the
    // old hand-rolled tail here would `session.datastore()` the virtual head
    // back into a real file and lose everything the plane bought — the seam
    // review's named trap.
    session.load(outHbin);
    ojson residentOut =
        targetPresent
            ? piet::handlers::mutationResult(session, action, /*newVars=*/{}, /*droppedVars=*/{},
                                             /*rewrittenVars=*/{varname})
            : piet::handlers::mutationResult(session, action, {varname});
    // C4-L4 sibling of destring's memo: the values THIS process just computed
    // and committed, so a chained single-column consumer (destring/encode of
    // the value we just wrote) never re-materializes the virtual head to learn
    // what PIET already knew. After mutationResult — the fold's own vars_delta
    // pass erases the name first.
    if (session.residentHeadDirty()) {
      SessionState::ResidentComputedCol put;
      put.nRows = n;
      if (isString) {
        put.dtype = "factor";
        put.strs = std::move(outStrings);
      } else {
        put.dtype = "f64";
        put.f64 = std::move(outNums);
      }
      session.residentComputedColPut(varname, std::move(put));
    }
    return residentOut;
  }

  // Everything from here to the return is the POST-engine phase: re-open the
  // patched hbin, rebuild the varlist card, seed nMiss. Declared AFTER the
  // error returns above so only the success path is billed, and it lives to end
  // of function, so `reload` covers the whole tail.
  PhaseTimer _ptReload(&counters::strmutPhases().reloadNs);
  Datastore* ds2 = nullptr;
  {
    // rlLoad: re-open the hbin the engine just patched. Frame-load
    // materialization tax — the sub-phase C3 descriptor carry-over targets.
    PhaseTimer _ptLoad(&counters::strmutPhases().rlLoadNs);
    // ROWLOOP-RLLOAD-SUBSPLIT: arm the {materialize, open, reclaim, close}
    // sub-timers for exactly this scope. session.load()/datastore() are also
    // called by `use`, the dispatcher and other verbs, so an unarmed global
    // sub-total would not be a partition of rl_load (counters.h RlArm).
    counters::RlArm _rlArm;
    // ALLOC-C3A-ARC1-CARRY: hand the parse we are about to throw away to the
    // open that is about to rebuild it. `setcol` rewrote exactly ONE column and
    // left every other column's dictionary byte-identical (contract_A §28), so
    // re-lexing 2.4-2.8 MB of level strings — 99% of them belonging to columns
    // nothing touched — is work this frame has already done.
    //
    // Raised AFTER the native_exists guard above (brief §5-3): if the engine
    // never wrote the file, the guard has already returned and no carry is in
    // flight to be consumed against a file that does not exist. Scoped to this
    // block, so the descriptor cannot outlive the one load it describes.
    cecon::CarryDelta _carryDelta;
    _carryDelta.kind = targetPresent ? cecon::CarryKind::Replace : cecon::CarryKind::Append;
    _carryDelta.name = varname;
    _carryDelta.dtype = isString ? cecon::DType::Factor : cecon::DType::F64;
    SessionState::CarryArm _carryArm(session, outHbin, _carryDelta);
    session.load(outHbin);
    session.invalidateDatastore();
    ds2 = session.datastore();
  }
  // C1-CARD-REBUILD-REDUCE: seed the unchanged columns' missing-counts BEFORE
  // the varlist rebuild below, so each such column's buildVarInfo skips its
  // O(nrows) NaN/negative-code scan (and the whole-column mmap fault-in that
  // dominates the wide-frame "card tax"). `setcol` above patched ONLY `varname`
  // and left every other column — data AND dictionary — byte-identical, which
  // is exactly the pure-add / in-place-rewrite shape the NUMERIC gen/replace
  // path already carries through mutationResult -> seedNMissForPureAdd
  // (C2-F13a): string gen/replace simply never reached that seed because it
  // rebuilds by hand instead of through mutationResult. gen (newVars={varname})
  // hits branch (1) — carry all, scan the new column fresh; replace (newVars={},
  // rewrittenVars={varname}) hits branch (2) — carry all but the rewritten
  // target. Same PIET_NO_NMISS_CACHE A/B correctness gate as every other seed
  // site; freeze --check byte-compares varlist_after (n_missing included) as the
  // parapet. Runs before updateNMissCache below, so it reads the PRE-mutation
  // counts (this op's refresh happens at line ~820).
  std::vector<std::string> seedRewritten;
  if (replaceExisting) seedRewritten.push_back(varname);
  {
    PhaseTimer _ptNmiss(&counters::strmutPhases().rlNmissNs);
    session.seedNMissForPureAdd(ds2, action, newVars, /*droppedVars=*/{}, seedRewritten);
  }
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = action;
  ojson varlistAfter = ojson::array();
  PhaseTimer _ptVarlist(&counters::strmutPhases().rlVarlistNs);
  auto vlist = session.varlistWithMeta(ds2);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: keep the cache current for the next mutation
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = ds2 ? ds2->nRows() : 0;
  if (!newVars.empty()) out["new_vars"] = newVars;
  return out;
}

std::optional<std::pair<std::string, std::string>> splitAssign(const std::string& expr) {
  auto pos = expr.find('=');
  if (pos == std::string::npos) return std::nullopt;
  return std::make_pair(trim(expr.substr(0, pos)), trim(expr.substr(pos + 1)));
}

}  // namespace

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
struct TsIfPrep {
  std::string rewrittenIf;  // == the input when there is no operator to resolve
  std::string evalPath;     // "" when there is nothing to do; else a scratch frame
  std::string errMsg;       // non-empty iff `failed`
  std::string errCode = "engine_error";
  bool failed = false;
};

TsIfPrep materializeTsIf(SessionState& session, const std::string& ifExpr) {
  TsIfPrep prep;
  prep.rewrittenIf = ifExpr;
  auto toks = translate::extractTsTokens(ifExpr);
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
  // The tsset/xtset metadata does not live in the .hbin, so it rides the call —
  // gated on the predicate, which by construction carries an operator here.
  args = translate::injectTsContextForExpr(args, ifExpr, session);
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
  // Substitute back-to-front so the earlier tokens' offsets stay valid.
  for (size_t k = toks.size(); k-- > 0;)
    prep.rewrittenIf.replace(toks[k].begin, toks[k].end - toks[k].begin, names[k]);
  prep.evalPath = outPath;
  return prep;
}

ojson runStringGen(SessionState& session, const std::string& expr,
                   const std::optional<std::string>& ifExpr, bool strTyped) {
  if (!session.hasData()) return errBlock("generate: no dataset in memory");
  auto parts = splitAssign(expr);
  if (!parts.has_value()) return errBlock("generate: syntax is `gen newvar = expr`");
  std::string varname = parts->first, rhs = parts->second;
  // BUG C: shared validator (piet::strutil::isValidStataIdentifier) — see its
  // header comment. Was a TU-local `identRe()` regex here (no length cap,
  // and the numeric `gen` path never called it at all); now the ONE point.
  if (!strutil::isValidStataIdentifier(varname)) {
    return errBlock(
        "generate: invalid variable name `" + varname + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");
  }
  // ONESHOT-GETCOLS B2. The entrance used to be `session.datastore()`, which flushes
  // the WHOLE resident frame to disk so the row-wise evaluator below can read the two
  // or three columns `rhs`/`if` name — 154+42 of 350 head materializations on the
  // reference slice, ~161 MB each. Fetch just those columns instead (contract_A §38).
  //
  // Every failure falls back to the old path with nothing lost: this is a pure
  // optimization, and a miss must cost a fallback rather than an answer. `gen`'s
  // varname is deliberately NOT in the fetch list — it must not exist yet, and the
  // projection's `has()` returning false is exactly the check below.
  // BUG-370: resolve any L./F./D./S. in the `if` into real columns first (see
  // materializeTsIf). `tsPrep.evalPath` is empty for every predicate without one,
  // which is the path everything below already took.
  TsIfPrep tsPrep = materializeTsIf(session, ifExpr.value_or(""));
  if (tsPrep.failed) return errBlock("generate: " + tsPrep.errMsg, tsPrep.errCode);
  const std::optional<std::string> effIf =
      ifExpr.has_value() ? std::optional<std::string>(tsPrep.rewrittenIf) : std::nullopt;
  std::string miniPath;
  std::unique_ptr<Datastore> mini;
  std::unique_ptr<Datastore> tsFrame;
  if (!tsPrep.evalPath.empty()) {
    // The operator columns live only in this scratch frame, so evaluation reads it
    // whole rather than a projection. `dataPath` below stays the real head — the
    // write must land on the user's frame, never on the scratch.
    tsFrame = std::make_unique<Datastore>(tsPrep.evalPath);
  } else if (auto cols = referencedColumns(session, {&rhs, effIf.has_value() ? &*effIf : nullptr}, "")) {
    mini = fetchColumns(session, *cols, session.workdir(), &miniPath);
  }
  Datastore* ds = tsFrame ? tsFrame.get() : (mini ? mini.get() : session.datastore());
  const std::string dataPath = (tsFrame || mini)
                                   ? session.currentHbin().value_or(ds->path())
                                   : ds->path();
  // On the fetch path the projection cannot answer "does varname exist?" — it was
  // deliberately not requested. The shadow card can, and it is the same card the
  // fetch list was proved against, so the two agree by construction.
  bool alreadyDefined;
  if (tsFrame) {
    // The scratch frame carries every user column plus the temps, so it can answer
    // this directly — and it must, since the temps are not on the card.
    alreadyDefined = ds->has(varname);
  } else if (mini) {
    alreadyDefined = false;
    if (const auto* card = session.shadowVarlistIfCurrent())
      for (const auto& info : *card)
        if (info.name == varname) { alreadyDefined = true; break; }
  } else {
    alreadyDefined = ds->has(varname);
  }
  if (alreadyDefined) return errBlock("variable " + varname + " already defined");

  std::optional<std::string> rhsPy = rewriteExpr(rhs);
  std::optional<std::string> condPy =
      effIf.has_value() ? std::optional<std::string>(rewriteExpr(*effIf)) : std::nullopt;

  EvalRowsResult er = evalRows(session, *ds, rhsPy, condPy);
  if (!er.ok) return errBlock("generate: " + er.errMsg);

  bool isString = strTyped || resultIsString(er);
  return writeNew(session, *ds, varname, er, isString, /*replaceExisting=*/false, "gen", {varname},
                  dataPath);
}

ojson runStringReplace(SessionState& session, const std::string& expr,
                       const std::optional<std::string>& ifExpr, bool strTyped) {
  if (!session.hasData()) return errBlock("replace: no dataset in memory");
  auto parts = splitAssign(expr);
  if (!parts.has_value()) return errBlock("replace requires `varname = expr`");
  std::string varname = parts->first, rhs = parts->second;
  // ONESHOT-GETCOLS B2 — see runStringGen for why. `replace` DOES include varname in
  // the fetch: writeNew reads its existing values for the cond-false keep-original
  // path, and its dtype decides the string/numeric branch below.
  // BUG-370 — see runStringGen above for the shape and why the scratch frame is
  // not adopted as the head.
  TsIfPrep tsPrep = materializeTsIf(session, ifExpr.value_or(""));
  if (tsPrep.failed) return errBlock("replace: " + tsPrep.errMsg, tsPrep.errCode);
  const std::optional<std::string> effIf =
      ifExpr.has_value() ? std::optional<std::string>(tsPrep.rewrittenIf) : std::nullopt;
  std::string miniPath;
  std::unique_ptr<Datastore> mini;
  std::unique_ptr<Datastore> tsFrame;
  if (!tsPrep.evalPath.empty()) {
    tsFrame = std::make_unique<Datastore>(tsPrep.evalPath);
  } else if (auto cols = referencedColumns(
                 session, {&rhs, effIf.has_value() ? &*effIf : nullptr}, varname)) {
    mini = fetchColumns(session, *cols, session.workdir(), &miniPath);
  }
  if (mini && !mini->has(varname)) mini.reset();  // varname absent => not the target frame
  Datastore* ds = tsFrame ? tsFrame.get() : (mini ? mini.get() : session.datastore());
  const std::string dataPath = (tsFrame || mini)
                                   ? session.currentHbin().value_or(ds->path())
                                   : ds->path();
  if (!ds->has(varname)) return errBlock("variable " + varname + " not found");

  std::optional<std::string> rhsPy = rewriteExpr(rhs);
  std::optional<std::string> condPy =
      effIf.has_value() ? std::optional<std::string>(rewriteExpr(*effIf)) : std::nullopt;

  EvalRowsResult er = evalRows(session, *ds, rhsPy, condPy);
  if (!er.ok) return errBlock("replace: " + er.errMsg);

  bool targetIsFactor = ds->dtype(varname) != "f64";
  bool isString = strTyped || targetIsFactor || resultIsString(er);
  return writeNew(session, *ds, varname, er, isString, /*replaceExisting=*/true, "gen", {},
                  dataPath);
}

// BS-090 (BETA Phase G): AND a co-occurring `in a/b` into an already-built
// per-row mask (Doctrine ① — count/keep-if/drop-if's row-wise string detour
// must never silently drop an `in` that reached here alongside a
// string-referencing `if`, just because the `if` half needed this path
// instead of cobra's --keepif). Returns false (mask left UNTOUCHED) on a
// malformed/negative `in` string — caller must treat that as its own
// distinct failure, not silently proceed with an unfiltered mask.
static bool applyInRangeToMask(std::vector<bool>& mask, int64_t nRowsTotal,
                               const std::optional<std::string>& inRange, bool& outNegative,
                               std::string& outErrMsg) {
  if (!inRange.has_value() || inRange->empty()) return true;
  piet::handlers::InRangeBounds rr = piet::handlers::parseInRangeBounds(*inRange, nRowsTotal);
  if (!rr.ok) {
    outNegative = rr.negative;
    outErrMsg = rr.negative
                    ? "Negative (from-the-end) `in` bounds are not supported yet. Use a "
                      "positive 1-based row range, e.g. `in 1/10`, `in f/10`, `in 5/l`, `in f/l`."
                    : "This `in` row-range form is not supported. Supported forms: INT, "
                      "INT/INT, f/INT, INT/l, f/l (f = first row, l = last row).";
    return false;
  }
  for (int64_t i = 0; i < static_cast<int64_t>(mask.size()); ++i) {
    long long oneBased = static_cast<long long>(i) + 1;
    if (oneBased < rr.a || oneBased > rr.b) mask[static_cast<size_t>(i)] = false;
  }
  return true;
}

CountResult countRowsIf(SessionState& session, const std::string& ifExpr,
                        const std::optional<std::string>& inRange) {
  CountResult r;
  if (!session.hasData()) {
    r.ok = false;
    r.errMsg = "count: no dataset in memory";
    return r;
  }
  // ONESHOT-GETCOLS2: same shape as the mutation entrance — this counter needs the
  // predicate's columns, not the frame.
  // BUG-370: `count if grp=="a" & L.x>0` — the operator half is resolved by the
  // engine into a scratch frame, the string half by the evaluator below. Nothing
  // is loaded as the head, so this read-only command stays read-only.
  TsIfPrep tsPrep = materializeTsIf(session, ifExpr);
  if (tsPrep.failed) {
    r.ok = false;
    r.errMsg = tsPrep.errMsg;
    return r;
  }
  std::string cntMiniPath;
  std::unique_ptr<Datastore> cntMini;
  std::unique_ptr<Datastore> tsFrame;
  if (!tsPrep.evalPath.empty()) {
    tsFrame = std::make_unique<Datastore>(tsPrep.evalPath);
  } else if (auto cols = referencedColumns(session, {&tsPrep.rewrittenIf}, "")) {
    cntMini = fetchColumns(session, *cols, session.workdir(), &cntMiniPath);
  }
  Datastore* ds = tsFrame ? tsFrame.get() : (cntMini ? cntMini.get() : session.datastore());
  std::string condPy = rewriteExpr(tsPrep.rewrittenIf);
  EvalRowsResult er = evalRows(session, *ds, std::nullopt, std::optional<std::string>(condPy));
  if (!er.ok) {
    r.ok = false;
    r.errMsg = er.errMsg;
    return r;
  }
  bool neg = false;
  std::string inErr;
  if (!applyInRangeToMask(er.mask, ds->nRows(), inRange, neg, inErr)) {
    r.ok = false;
    r.inRangeError = true;
    r.errMsg = inErr;
    return r;
  }
  long long cnt = 0;
  for (bool m : er.mask) {
    if (m) ++cnt;
  }
  r.n = cnt;
  r.selected = std::move(er.mask);
  return r;
}

ojson filterRowsIf(SessionState& session, const std::string& ifExpr, bool keep,
                   const std::optional<std::string>& inRange) {
  std::string action = keep ? "keep if" : "drop if";
  if (!session.hasData()) return errBlock(action + ": no dataset in memory");
  // INSPECT-2608 ENTRANCE (contract_A §38, the same projection runStringGen and
  // runStringReplace already take): the row-wise evaluator needs the one or two
  // columns the CONDITION names, and `session.datastore()` was flushing the whole
  // resident frame to disk to give them to it. Measured on the acceptance
  // corpus's controls=yes arm, this form was the single largest item — 49.4 s
  // and 2.7 GB of touched bytes over 11 calls.
  //
  // The projection is only usable when the exit below can also avoid needing
  // every column (the fast mask path); the CSV fallback rebuilds the frame from
  // `ds` and would silently drop unprojected columns. So `dsFull` stays the
  // authority for that path and the projection is scoped to the mask.
  // BUG-370 — see materializeTsIf. Only the MASK is computed off the scratch
  // frame; both exits below re-derive their own authority from the session (the
  // fast path re-reads currentHbin(), the CSV path re-opens session.datastore()),
  // so a temp column cannot reach the filtered result.
  TsIfPrep tsPrep = materializeTsIf(session, ifExpr);
  if (tsPrep.failed) return errBlock(action + ": " + tsPrep.errMsg, tsPrep.errCode);
  std::string miniPath;
  std::unique_ptr<Datastore> mini;
  std::unique_ptr<Datastore> tsFrame;
  if (!tsPrep.evalPath.empty()) {
    tsFrame = std::make_unique<Datastore>(tsPrep.evalPath);
  } else {
    const std::string* pExpr = &tsPrep.rewrittenIf;
    if (auto cols = referencedColumns(session, {pExpr}, ""))
      mini = fetchColumns(session, *cols, session.workdir(), &miniPath);
  }
  Datastore* dsEval = tsFrame ? tsFrame.get() : (mini ? mini.get() : session.datastore());
  std::string condPy = rewriteExpr(tsPrep.rewrittenIf);
  EvalRowsResult er = evalRows(session, *dsEval, std::nullopt, std::optional<std::string>(condPy));
  if (!er.ok && mini) {
    // A projection miss must cost a fallback, never an answer: re-evaluate
    // against the full frame before believing the failure.
    mini.reset();
    dsEval = session.datastore();
    er = evalRows(session, *dsEval, std::nullopt, std::optional<std::string>(condPy));
  }
  if (!er.ok) return errBlock(action + ": " + er.errMsg);

  int64_t n = dsEval->nRows();
  // BS-090: AND a co-occurring `in a/b` into the mask BEFORE the keep/drop
  // split below — for keep-if this restricts to the in-range cond-true
  // rows directly; for drop-if the SAME AND'd mask, then negated by the
  // `== keep` comparison just below, correctly drops only the rows that
  // satisfy BOTH `if` and `in` (Stata's documented if+in intersection
  // semantics), leaving out-of-range rows untouched either way.
  if (inRange.has_value() && !inRange->empty()) {
    bool neg = false;
    std::string inErr;
    if (!applyInRangeToMask(er.mask, n, inRange, neg, inErr)) {
      return errBlock(action + ": " + inErr, "unsupported_option");
    }
  }
  // Rows to KEEP: (mask == keep) — keep-if keeps cond-true rows; drop-if keeps
  // cond-false rows.
  std::vector<char> sel(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) sel[static_cast<size_t>(i)] = (er.mask[static_cast<size_t>(i)] == keep) ? 1 : 0;

  // INSPECT-2608 EXIT — the fast path. The slow path below rewrites EVERY column
  // of the frame as text and re-converts it; what the filter actually needs is a
  // row mask. Push the mask this function already computed as ONE resident f64
  // column, then let the engine do the row slice and delete the column in a
  // single call (`eval --keepif M --drop M`, the shape inrange_filter already
  // uses). Mechanisms are all pre-existing: no new engine op, no contract change.
  //
  // Fallback discipline: ANY failure here falls through to the whole-frame CSV
  // path with nothing lost — this is a pure optimization and a miss must cost a
  // fallback, not an answer. `PIET_NO_RESIDENT_KEEPIF` forces the old path (the
  // A/B arm every probe in this file has).
  if (std::getenv("PIET_NO_RESIDENT_KEEPIF") == nullptr && !residentReadDisabled() &&
      session.currentHbin().has_value()) {
    // A name no Stata identifier can collide with (leading underscore + the
    // reserved prefix this file already uses for scratch), re-checked against
    // the live varlist card anyway.
    const std::string maskVar = "__piet_filtermask";
    bool collides = false;
    if (const auto* card = session.shadowVarlistIfCurrent())
      for (const auto& info : *card)
        if (info.name == maskVar) { collides = true; break; }
    if (!collides) {
      std::string valuesPath = nextPath(session.workdir(), "filtermask") + ".csv";
      bool wrote = false;
      {
        std::ofstream vf(cecon::to_native_path(valuesPath), std::ios::binary);
        if (vf) {
          for (int64_t i = 0; i < n; ++i) vf << (sel[static_cast<size_t>(i)] ? "1\n" : "0\n");
          vf.flush();
          wrote = static_cast<bool>(vf);
        }
      }
      if (wrote) {
        const std::string basePath = *session.currentHbin();
        std::string maskHbin = nextPath(session.workdir(), "filtermask") + ".hbin";
        std::optional<SessionState::ResidentEligible> resident;
        resident.emplace(session);
        CobraRunResult cr = runCobra({"setcol", "--data", basePath, "--var", maskVar, "--values",
                                      valuesPath, "--dtype", "f64", "--new", "--out", maskHbin});
        cecon::native_unlink(valuesPath);
        if (cr.ok) {
          session.load(maskHbin);
          std::string outHbin2 = nextPath(session.workdir(), "filterif") + ".hbin";
          CobraRunResult cr2 = runCobra({"eval", "--data", *session.currentHbin(), "--keepif",
                                         maskVar + " != 0", "--drop", maskVar, "--out", outHbin2});
          if (cr2.ok) {
            resident.reset();
            session.load(outHbin2);
            return piet::handlers::mutationResult(session, "keepif", /*newVars=*/{},
                                                  /*droppedVars=*/{});
          }
          // The slice failed with the mask column already committed. Take the
          // column back out before falling through, so the slow path below does
          // not carry a scratch column into the user's frame.
          std::string cleanHbin = nextPath(session.workdir(), "filterif") + ".hbin";
          CobraRunResult cr3 = runCobra(
              {"eval", "--data", *session.currentHbin(), "--drop", maskVar, "--out", cleanHbin});
          if (cr3.ok) session.load(cleanHbin);
          resident.reset();
          session.invalidateDatastore();
        }
      } else {
        cecon::native_unlink(valuesPath);
      }
    }
  }

  // Write a filtered CSV (all columns, selected rows) -> cobra convert.
  Datastore* ds = session.datastore();
  std::vector<std::string> names = ds->names();
  std::vector<char> isFactor(names.size());
  std::vector<const double*> f64ptrs(names.size(), nullptr);
  std::vector<const int32_t*> codeptrs(names.size(), nullptr);
  std::vector<const std::vector<std::string>*> levelptrs(names.size(), nullptr);
  std::set<std::string> factorOut;
  for (size_t c = 0; c < names.size(); ++c) {
    if (ds->dtype(names[c]) != "f64") {
      isFactor[c] = 1;
      codeptrs[c] = ds->col(names[c]).codes();
      levelptrs[c] = &ds->col(names[c]).levels;
      factorOut.insert(names[c]);
    } else {
      isFactor[c] = 0;
      f64ptrs[c] = ds->col(names[c]).f64();
    }
  }

  std::string csvPath = nextPath(session.workdir(), "filterif") + ".csv";
  {
    std::ofstream csv(cecon::to_native_path(csvPath), std::ios::binary);
    if (!csv) return errBlock(action + ": could not open scratch CSV for write");
    for (size_t c = 0; c < names.size(); ++c) {
      if (c) csv << ",";
      csv << names[c];
    }
    csv << "\n";
    for (int64_t i = 0; i < n; ++i) {
      if (!sel[static_cast<size_t>(i)]) continue;
      for (size_t c = 0; c < names.size(); ++c) {
        if (c) csv << ",";
        if (isFactor[c]) {
          int32_t code = codeptrs[c][i];
          std::string lv = (code >= 0 && static_cast<size_t>(code) < levelptrs[c]->size())
                               ? (*levelptrs[c])[static_cast<size_t>(code)]
                               : "";
          csv << csvEscape(lv);
        } else {
          csv << csvNum(f64ptrs[c][i]);
        }
      }
      csv << "\n";
    }
    // BUG-124 sibling: flush + check (see writeNew's identical guard) — a
    // silently short CSV here would drop kept rows without any error.
    csv.flush();
    if (!csv) {
      cecon::native_unlink(csvPath);
      return errBlock(action + ": failed writing scratch CSV (disk full or I/O error)");
    }
  }

  std::string outHbin = nextPath(session.workdir(), "filterif") + ".hbin";
  std::vector<std::string> args = {"convert", "--data", csvPath, "--out", outHbin, "--delim", ","};
  if (!factorOut.empty()) {
    std::string joined;
    bool first = true;
    for (const auto& nm : names) {
      if (factorOut.count(nm)) {
        if (!first) joined += ",";
        joined += nm;
        first = false;
      }
    }
    args.push_back("--factor");
    args.push_back(joined);
  }
  CobraRunResult cr = runCobra(args);
  cecon::native_unlink(csvPath);
  if (!cr.ok) {
    // BUG-124: same dead-catch hole as writeNew — surface the engine error and
    // remove the torn partial instead of load()ing it (see writeNew's comment).
    cecon::native_unlink(outHbin);
    return errBlock(action + ": error writing hbin: " + errorFromRaw(cr).what());
  }
  if (!cecon::native_exists(outHbin))
    return errBlock(action + ": failed writing output dataset (disk full or I/O error)");

  session.load(outHbin);
  session.invalidateDatastore();
  Datastore* ds2 = session.datastore();
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "keepif";
  ojson varlistAfter = ojson::array();
  auto vlist = session.varlistWithMeta(ds2);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: keep the cache current for the next mutation
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = ds2 ? ds2->nRows() : 0;
  return out;
}

}  // namespace piet::strmutate
