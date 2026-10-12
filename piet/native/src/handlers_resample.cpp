// resampling prefixes — `bootstrap:`, `jackknife:`, `permute:` (P6 wave).
//
// DESIGN — the loop lives HERE, on the PIET side, and the inner command runs
// through the ORDINARY dispatcher. That is what makes these prefixes general:
// whatever PIET can run, they can wrap, and whatever the expression layer can
// evaluate against r()/e()/_b[] is a legal exp_list element. There is no
// engine-side "resample" verb and no list of blessed inner commands.
//
// The shape, once, for all three methods:
//
//     run the inner command ONCE           -> the observed statistics
//     for each replicate:
//         transform the data               (the ONLY per-method difference)
//         run the inner command again      (normal dispatch, output discarded)
//         evaluate the exp_list            (r()/e()/_b[] as of that run)
//     restore the session's dataset; aggregate; render Stata's table
//
// WHY THE EXP_LIST MUST NOT BE MACRO-EXPANDED AT PARSE TIME. dispatch() expands
// macro and r() references on the raw line BEFORE routing (step 0b). For an
// ordinary command that is right; for `bootstrap mean=r(mean), reps(30):
// summarize y` it is fatal — `r(mean)` would be substituted with whatever the
// PREVIOUS command left (or error as unset) instead of being evaluated after
// each replicate. So the resampling prefixes are intercepted BEFORE that step
// and hand the exp_list here verbatim; only the options and the inner command
// are expanded, because those may legitimately carry locals
// (`reps(`R')`, `: regress `depvar' x`).
//
// RNG — ours, not Stata's. A fixed `seed()` makes OUR stream reproducible
// (Philox4x32-10 from the engine's own rng.h: counter-based, so replicate r,
// position j is a pure function of the seed and never depends on evaluation
// order). We do not attempt to reproduce Stata's draw sequence, and there is no
// sense in which we could: what is contracted is that the ESTIMATOR MATHEMATICS
// is right and the presentation follows Stata. Consequently the deterministic
// method (jackknife) is checked against the oracle digit for digit, while the
// stochastic ones are checked on their own algebraic identities plus agreement
// with the analytic limit.
//
// MATERIALIZATION — PIET has no hbin writer, so a computed column reaches the
// data the way every other PIET-side computation does: write a CSV, run `cobra
// convert`, join it back (the strmutate.cpp precedent). Each replicate is a
// handful of engine calls; on the resident `serve` worker those are in-process,
// not process spawns. This is not free, and the wave records the cost rather
// than hiding it — the engine-side fast path is a named P2-PERF candidate, not
// something to smuggle in here.
#include "piet/handlers.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/distributions.h"  // t_critical / t_twosided_pvalue / z_critical (shared with cobra)
#include "core/rng.h"            // Philox4x32-10, the engine's own counter-based RNG
#include "io/win_compat.h"
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/display_hardening.h"  // fmtStataDefaultG — Stata's %9.0g default rendering
#include "piet/eval/macros.h"
#include "piet/session_state.h"
#include "piet/scratch_seq.h"

namespace piet {
namespace handlers {

using ojson = nlohmann::ordered_json;

namespace {

ojson rsError(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

std::string nextPath(const std::string& workdir, const std::string& prefix) {
  static std::atomic<uint64_t> counter{0};
  static const long long pid = static_cast<long long>(cecon::native_process_id());
  return workdir + "/" + prefix + "_" + std::to_string(pid) + "_" +
         std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + ".hbin";
}

CobraError errorFromRaw(const CobraRunResult& r) {
  std::string t = r.stderr_text;
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
  try {
    nlohmann::json payload = nlohmann::json::parse(t);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                        err.at("message").get<std::string>(), err.value("detail", std::string()));
    }
  } catch (const nlohmann::json::exception&) {
  }
  return CobraError("engine_error", t.empty() ? "cobra failed" : t);
}

std::string cobraErrorStr(const CobraError& ce) { return cobraErrorMessage(ce); }

// The row-index column every replicate transform joins on. Underscore-prefixed
// and long enough that it cannot collide with a user variable in practice; it
// never survives into the session's own dataset.
constexpr const char* kRowIdx = "__piet_rs_row";

struct ExpItem {
  std::string name;  // the label Stata prints in the leftmost column
  std::string expr;  // RAW, evaluated fresh after every replicate
};

// One replicate's evaluated statistics, parallel to the exp_list.
using RepValues = std::vector<double>;

// ---------------------------------------------------------------------------
// exp_list parsing. Stata's grammar is `NAME=EXPR [NAME=EXPR ...]`, and an EXPR
// may itself contain spaces and brackets (`_b[x1]`, `r(mean)`), so the split is
// on the `NAME=` boundaries, not on whitespace.
std::vector<ExpItem> parseExpList(const std::string& raw, std::string* err) {
  std::vector<ExpItem> out;
  std::string s = raw;
  size_t i = 0;
  auto skipWs = [&]() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; };
  while (true) {
    skipWs();
    if (i >= s.size()) break;
    const size_t nameStart = i;
    while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
    const std::string name = s.substr(nameStart, i - nameStart);
    skipWs();
    if (name.empty() || i >= s.size() || s[i] != '=') {
      *err = "expression list must be `name=expression [name2=expression2 ...]` (got `" + raw + "`)";
      return {};
    }
    ++i;  // '='
    // The expression runs to the next `NAME=` at bracket depth zero, or to the end.
    const size_t exprStart = i;
    int depth = 0;
    size_t exprEnd = s.size();
    size_t j = i;
    while (j < s.size()) {
      const char c = s[j];
      if (c == '(' || c == '[') ++depth;
      else if (c == ')' || c == ']') --depth;
      else if (depth == 0 && std::isspace(static_cast<unsigned char>(c))) {
        // Look ahead: is the next token a `NAME=`? If so, this space ends the
        // expression; otherwise it is a space INSIDE the expression.
        size_t k = j;
        while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
        size_t m = k;
        while (m < s.size() && (std::isalnum(static_cast<unsigned char>(s[m])) || s[m] == '_')) ++m;
        size_t n = m;
        while (n < s.size() && std::isspace(static_cast<unsigned char>(s[n]))) ++n;
        if (m > k && n < s.size() && s[n] == '=' && (n + 1 >= s.size() || s[n + 1] != '=')) {
          exprEnd = j;
          break;
        }
      }
      ++j;
    }
    if (j >= s.size()) exprEnd = s.size();
    std::string expr = s.substr(exprStart, exprEnd - exprStart);
    while (!expr.empty() && std::isspace(static_cast<unsigned char>(expr.back()))) expr.pop_back();
    while (!expr.empty() && std::isspace(static_cast<unsigned char>(expr.front()))) expr.erase(0, 1);
    if (expr.empty()) {
      *err = "empty expression for `" + name + "`";
      return {};
    }
    out.push_back(ExpItem{name, expr});
    i = exprEnd;
  }
  if (out.empty()) *err = "expression list required";
  return out;
}

// Evaluate one exp_list element AGAINST THE CURRENT SESSION STATE — i.e. after
// the inner command has just run and posted its r()/e()/_b[]. This is the same
// resolver `display r(mean)` uses, which is exactly the point: the exp_list can
// say anything a display can.
std::optional<double> evalStat(const std::string& expr, SessionState& session) {
  // TWO resolvers, in this order, because they cover different names and
  // neither is a superset of the other:
  //   expandMacros  — locals/globals, r(), e(), s()
  //   evalArith     — the estimation scalars `_b[...]`/`_se[...]`
  //                   (resolveEstScalarRefs), matrix subscripts, arithmetic
  // Using only the first leaves `_b[x1]` unresolved; using only the second
  // leaves `r(mean)` unresolved. An exp_list may hold either, so it gets both.
  std::string text = expr;
  if (eval::hasMacroRefs(text)) {
    eval::ExpandResult r = eval::expandMacros(text, session.eval);
    if (r.isError) return std::nullopt;
    text = r.line;
  }
  std::optional<std::string> folded = eval::evalArith(text, &session.eval);
  const std::string& out = folded.has_value() ? *folded : text;
  try {
    size_t pos = 0;
    const double v = std::stod(out, &pos);
    while (pos < out.size() && std::isspace(static_cast<unsigned char>(out[pos]))) ++pos;
    if (pos != out.size()) return std::nullopt;
    return v;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// A one-column CSV of int64 values, for `cobra convert` to turn into an hbin.
bool writeIndexCsv(const std::string& path, const std::string& colName,
                   const std::vector<int64_t>& vals) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  f << colName << "\n";
  for (int64_t v : vals) f << v << "\n";
  return static_cast<bool>(f);
}

// P2-PERF ARC-C C2-②: the HEADERLESS single-column payload `cobra setcol`
// reads (`Frame::read_csv_column` — one raw value per row, row count == n_rows).
// It replaces the old two-column (index, value) CSV, whose whole purpose was to
// give a 1:1 merge something to join on. permute does not need a join: the
// permuted values are already in the base frame's own row order, so the column
// can simply be written over the one it replaces.
bool writePermuteValuesCsv(const std::string& path, const std::vector<double>& vals) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  f.setf(std::ios::fmtflags(0), std::ios::floatfield);
  f.precision(17);
  for (double v : vals) {
    if (!std::isnan(v)) f << v;
    f << "\n";
  }
  return static_cast<bool>(f);
}

std::string commaGroup(int64_t v) {
  std::string s = std::to_string(v < 0 ? -v : v);
  std::string out;
  int c = 0;
  for (auto it = s.rbegin(); it != s.rend(); ++it) {
    if (c && c % 3 == 0) out.push_back(',');
    out.push_back(*it);
    ++c;
  }
  if (v < 0) out.push_back('-');
  std::reverse(out.begin(), out.end());
  return out;
}


// --- Stata table rendering ---------------------------------------------------
// The oracle log IS the layout spec: these reproduce the column widths and the
// rules of `bootstrap`/`jackknife`/`permute` output.
std::string g9(double v) { return fmtStataDefaultG(v); }

// BUG-183: a COEFFICIENT-TABLE cell is one character narrower than the shared
// %9.0g `display` renderer — `_coef_table` reserves the sign column, so the
// digits get 8 characters where `display` gets 9. `fmtStataDefaultG` is
// calibrated for `display` (its own doc comment records
// `2.157789219986937 -> "2.1577892"`, nine characters) and is correct THERE, so
// the fix is a second, narrower entry point at the table call sites rather than
// a change to the shared renderer — moving the shared one would break `display`.
//
// Exposure is narrow and was measured, not assumed: the two rules coincide below
// |1| (".2071479" is eight characters either way) and, above it, whenever the 9th
// character would be a trailing zero, which `%g` strips. So this only prints
// differently when |value| >= 1 AND its 8th significant digit is nonzero — which
// is why the P6 answerbook stayed byte-identical (its jackknife CI upper is
// `1.099479`, exactly the trailing-zero case) and why this was filed LATENT.
//
// Kept verbatim in sync with `g9cell` in handlers_qe.cpp, which is where the P-QE
// table arc first hit this wall (rendering did_imputation, whose tau values are
// all > 1). That file owns the original; this is the documented
// duplicate-and-keep-in-sync pattern this codebase already uses for TU-local
// helpers that cannot be shared without widening a header (see strutil.h's note).
// Oracle anchors for the rule itself: did_imputation tau0 1.2081958740803342
// prints `1.208196` not `1.2081959` (p3_stata.log:326); P6's jackknife CI upper
// prints `1.099479` (p6_stata.log:64).
std::string g9cell(double v) {
  std::string s = fmtStataDefaultG(v);
  const size_t sign = (!s.empty() && s[0] == '-') ? 1u : 0u;
  if (s.size() - sign <= 8) return s;
  for (int prec = 7; prec >= 1; --prec) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*g", prec, v);
    std::string t(buf);
    const size_t sg = (!t.empty() && t[0] == '-') ? 1u : 0u;
    if (t.size() > sg + 1 && t.compare(sg, 2, "0.") == 0) t.erase(sg, 1);
    if (t.size() - sg <= 8) return t;
  }
  return s;
}

std::string padLeft(const std::string& s, size_t w) {
  return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
}
std::string padRight(const std::string& s, size_t w) {
  return s.size() >= w ? s : s + std::string(w - s.size(), ' ');
}
std::string fixed2(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.2f", v);
  return std::string(buf);
}
std::string fixed3(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.3f", v);
  return std::string(buf);
}
std::string fixed4(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.4f", v);
  return std::string(buf);
}
// Stata prints a probability in (0,1) without the leading zero (".0000").
std::string prob4(double v) {
  std::string s = fixed4(v);
  if (s.size() > 1 && s[0] == '0' && s[1] == '.') s.erase(0, 1);
  return s;
}

// Clopper-Pearson exact binomial interval — what Stata's permute reports as
// [95% CI(p)]. Verified against the oracle's own row: c=30/n=30 gives a lower
// limit of .8843 (= (alpha/2)^(1/n)) and c=0/n=30 an upper limit of .1157,
// which is the TWO-sided form, not the one-sided one. Inverted by bisection on
// the regularized incomplete beta (cecon::betai) rather than a second beta
// implementation of our own.
double betaInv(double target, double a, double b) {
  if (a <= 0) return 0.0;
  if (b <= 0) return 1.0;
  double lo = 0.0, hi = 1.0;
  for (int i = 0; i < 200; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (cecon::betai(a, b, mid) < target) lo = mid; else hi = mid;
  }
  return 0.5 * (lo + hi);
}
std::pair<double, double> clopperPearson(int64_t c, int64_t n, double alpha) {
  const double lo = (c <= 0) ? 0.0
                             : betaInv(alpha / 2.0, static_cast<double>(c),
                                       static_cast<double>(n - c + 1));
  const double hi = (c >= n) ? 1.0
                             : betaInv(1.0 - alpha / 2.0, static_cast<double>(c + 1),
                                       static_cast<double>(n - c));
  return {lo, hi};
}

// Two-sided normal p-value, P(|Z| > |z|). erfc is the direct spelling; the
// engine has no normal p-value helper to share, so there is nothing to diverge
// from here.
double normalTwoSidedP(double z) { return std::erfc(std::fabs(z) * 0.7071067811865476); }

}  // namespace

// ---------------------------------------------------------------------------
// The driver.
//
// `method` is bootstrap | jackknife | permute. `expListRaw` is verbatim (never
// macro-expanded — see the file header). `optsRaw` and `innerCmd` HAVE been
// expanded by the caller, so their locals are already resolved.
ojson runResample(const std::string& method, const std::string& permuteVar,
                  const std::string& expListRaw, const std::string& optsRaw,
                  const std::string& innerCmd, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (innerCmd.empty()) return rsError(method + ": no command found after the colon");

  std::string perr;
  std::vector<ExpItem> expList = parseExpList(expListRaw, &perr);
  if (expList.empty()) return rsError(method + ": " + perr);

  // -- options ---------------------------------------------------------------
  int reps = 50;                 // Stata's own default for bootstrap/permute
  uint64_t seed = 0;
  bool haveSeed = false;
  {
    // The option list is a flat `name(value)` / `name` sequence; parse it here
    // rather than through the tokenizer because this line was deliberately kept
    // out of the normal parse path.
    size_t i = 0;
    while (i < optsRaw.size()) {
      while (i < optsRaw.size() &&
             (std::isspace(static_cast<unsigned char>(optsRaw[i])) || optsRaw[i] == ',')) ++i;
      if (i >= optsRaw.size()) break;
      const size_t ns = i;
      while (i < optsRaw.size() && (std::isalnum(static_cast<unsigned char>(optsRaw[i])) ||
                                    optsRaw[i] == '_')) ++i;
      std::string key = optsRaw.substr(ns, i - ns);
      for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      std::string val;
      if (i < optsRaw.size() && optsRaw[i] == '(') {
        int depth = 1;
        ++i;
        const size_t vs = i;
        while (i < optsRaw.size() && depth > 0) {
          if (optsRaw[i] == '(') ++depth;
          else if (optsRaw[i] == ')') --depth;
          if (depth > 0) ++i;
        }
        val = optsRaw.substr(vs, i - vs);
        if (i < optsRaw.size()) ++i;  // ')'
      }
      if (key.empty()) break;
      if (key == "reps" || key == "rep") {
        try { reps = std::stoi(val); } catch (const std::exception&) {
          return rsError(method + ": reps() needs a positive integer (got `" + val + "`)");
        }
      } else if (key == "seed") {
        try { seed = static_cast<uint64_t>(std::stoll(val)); haveSeed = true; }
        catch (const std::exception&) {
          return rsError(method + ": seed() needs an integer (got `" + val + "`)");
        }
      } else if (key == "nodots" || key == "dots" || key == "noisily" || key == "nowarn" ||
                 key == "notable" || key == "noheader" || key == "nolegend" || key == "verbose") {
        // Presentation-only switches: accepted and inert. Nothing about the
        // numbers depends on them, so silently honouring them is not a
        // narrowing — dots are progress output PIET does not emit at all.
      } else if (key == "level") {
        // handled below via session.level
        try { session.level = std::stoi(val); } catch (const std::exception&) {
          return rsError(method + ": level() needs an integer");
        }
      } else {
        return rsError(method + ": option `" + key +
                       "` is not implemented in this build. Implemented: reps(), seed(), level(), "
                       "and the presentation switches (nodots/notable/...).");
      }
    }
  }
  if (reps <= 0) return rsError(method + ": reps() must be positive");

  // The head may be a DIRTY VIRTUAL resident frame whose path names a file that
  // was never written (`keep if` deliberately leaves it that way — see
  // inrange_filter.cpp). This driver both re-opens the base as a file and
  // returns the session to it at the end, so anchor it on disk first.
  // datastore() is the established materialization point.
  try {
    (void)session.datastore();
  } catch (const std::exception& e) {
    return rsError(method + ": could not materialize the working dataset: " + e.what());
  }
  const std::string startHbin = *session.currentHbin();

  // -- 1. the observed run ---------------------------------------------------
  // Also the run that tells us what the estimation sample IS: an e-class
  // command leaves a LastModel behind, an r-class one does not.
  {
    ojson obs = dispatch(innerCmd, session);
    for (const auto& b : obs) {
      if (b.is_object() && b.value("type", "") == "error") {
        session.load(startHbin);
        return rsError(method + ": the command `" + innerCmd + "` failed, so there is nothing to " +
                       (method == "permute" ? "permute" : "resample") + ": " +
                       b.value("message", std::string("(no message)")));
      }
    }
  }
  RepValues observed;
  for (const auto& e : expList) {
    std::optional<double> v = evalStat(e.expr, session);
    if (!v.has_value()) {
      session.load(startHbin);
      return rsError(method + ": could not evaluate `" + e.expr + "` after running `" + innerCmd +
                     "` — it does not resolve to a number.");
    }
    observed.push_back(*v);
  }

  // -- 2. the resampling base ------------------------------------------------
  // Stata draws from the inner command's ESTIMATION SAMPLE when it sets one,
  // and from the whole dataset (with a warning) when it does not. Our carriage
  // for "the sample the fit actually ran on" is LastModel (the P5 BUG-163
  // mechanism): its fitDataPath/fitKeepif carry an if/in restriction, and the
  // model's own y/x lists let us drop the rows a missing value excluded.
  std::string basePath = startHbin;
  bool haveEstSample = false;
  std::vector<std::string> missingFilterVars;
  if (session.lastModel().has_value()) {
    const auto& lm = *session.lastModel();
    haveEstSample = true;
    if (!lm.fitDataPath.empty()) basePath = lm.fitDataPath;
    Datastore probe(basePath);
    auto consider = [&](const std::string& v) {
      // Only plain existing columns: a factor-expanded term like `i.region`
      // is not a column and carries no missingness of its own.
      if (!v.empty() && probe.has(v)) missingFilterVars.push_back(v);
    };
    consider(lm.y);
    for (const auto& x : lm.x) consider(x);
    std::string keepif = lm.fitKeepif;
    for (const auto& v : missingFilterVars) {
      const std::string term = "!missing(" + v + ")";
      keepif = keepif.empty() ? term : ("(" + keepif + ") & " + term);
    }
    if (!keepif.empty()) {
      const std::string filtered = nextPath(session.workdir(), method + "_base");
      CobraRunResult cr =
          runCobra({"eval", "--data", basePath, "--keepif", keepif, "--out", filtered});
      if (!cr.ok) {
        session.load(startHbin);
        CobraError ce = errorFromRaw(cr);
        return rsError(cobraErrorStr(ce), ce.code);
      }
      basePath = filtered;
    }
  }

  // Every transform joins on a row index, so materialize it once.
  const std::string basePathIdx = nextPath(session.workdir(), method + "_baseidx");
  {
    CobraRunResult cr = runCobra(
        {"eval", "--data", basePath, "--egen", std::string(kRowIdx) + " = seq()", "--out", basePathIdx});
    if (!cr.ok) {
      session.load(startHbin);
      CobraError ce = errorFromRaw(cr);
      return rsError(cobraErrorStr(ce), ce.code);
    }
  }
  // The loop below writes several scratch files per replicate, which pushes the
  // reclaim budget over and makes these two — the oldest at that moment — the
  // first candidates for eviction, while they are still the thing being drawn
  // from. Pin them for the length of this command (RAII: released on every
  // exit path, including the throw one, since the loop dispatches arbitrary
  // user commands).
  SessionState::ScratchPin pin(session, {basePath, basePathIdx, startHbin});

  int64_t nBase = 0;
  {
    Datastore d(basePathIdx);
    nBase = d.nRows();
  }
  if (nBase <= 1) {
    session.load(startHbin);
    return rsError(method + ": need more than one observation to resample (have " +
                   std::to_string(nBase) + ")");
  }

  // permute's variable must be numeric and present.
  std::vector<double> permOriginal;
  if (method == "permute") {
    if (permuteVar.empty()) {
      session.load(startHbin);
      return rsError("permute: the permutation variable is required (permute VAR name=exp: cmd)");
    }
    Datastore d(basePathIdx);
    if (!d.has(permuteVar)) {
      session.load(startHbin);
      return rsError("variable " + permuteVar + " not found");
    }
    const cecon::Column& c = d.col(permuteVar);
    if (c.dtype == cecon::DType::Factor) {
      session.load(startHbin);
      return rsError("permute: `" + permuteVar +
                     "` is a string/factor variable; this build permutes numeric variables only.");
    }
    const double* p = c.f64();
    permOriginal.assign(p, p + static_cast<size_t>(nBase));
  }

  // jackknife's replicate count is not a user choice — it is n.
  const int64_t nReps = (method == "jackknife") ? nBase : static_cast<int64_t>(reps);

  // -- 3. the replicate loop -------------------------------------------------
  std::vector<RepValues> reps_values;
  reps_values.reserve(static_cast<size_t>(nReps));
  std::string failure;
  for (int64_t b = 0; b < nReps && failure.empty(); ++b) {
    std::string repPath;
    if (method == "jackknife") {
      // Leave observation b+1 out.
      repPath = nextPath(session.workdir(), "jk_rep");
      CobraRunResult cr = runCobra({"eval", "--data", basePathIdx, "--keepif",
                                    std::string(kRowIdx) + "!=" + std::to_string(b + 1), "--out",
                                    repPath});
      if (!cr.ok) { failure = cobraErrorStr(errorFromRaw(cr)); break; }
    } else if (method == "bootstrap") {
      // n draws with replacement. Philox is counter-based, so draw (b, j) does
      // not depend on how the loop is scheduled. The modulo bias at these n is
      // ~1e-8 and we match no external stream, so it is not worth a rejection
      // loop that would only make the stream harder to reason about.
      std::vector<int64_t> idx(static_cast<size_t>(nBase));
      for (int64_t j = 0; j < nBase; ++j) {
        const uint32_t d = cecon::Philox::draw32(seed, static_cast<uint64_t>(b),
                                                 static_cast<uint64_t>(j));
        idx[static_cast<size_t>(j)] = static_cast<int64_t>(d % static_cast<uint32_t>(nBase)) + 1;
      }
      const std::string csv = session.workdir() + "/bs_idx.csv";
      registerScratchMint(csv);  // BUG-389
      if (!writeIndexCsv(csv, kRowIdx, idx)) { failure = "could not write the resample index"; break; }
      const std::string idxHbin = nextPath(session.workdir(), "bs_idx");
      CobraRunResult c1 = runCobra({"convert", "--data", csv, "--out", idxHbin, "--delim", ","});
      if (!c1.ok) { failure = cobraErrorStr(errorFromRaw(c1)); break; }
      const std::string merged = nextPath(session.workdir(), "bs_merged");
      // master = the drawn indices (with repeats), using = the base keyed by
      // the same index: an m:1 join materializes the bootstrap sample in draw
      // order, duplicates and all.
      CobraRunResult c2 = runCobra({"merge", "--data", idxHbin, "--using", basePathIdx, "--type",
                                    "m:1", "--key", kRowIdx, "--out", merged});
      if (!c2.ok) { failure = cobraErrorStr(errorFromRaw(c2)); break; }
      // A draw of n WITH REPLACEMENT leaves roughly a third of the base rows
      // undrawn, and merge's default keeps those using-only rows — which would
      // silently inflate every replicate (measured: mean replicate n = 165.2
      // instead of 120, halving the bootstrap SE). The sample is the MASTER
      // side only, so drop what came from the using side alone.
      repPath = nextPath(session.workdir(), "bs_rep");
      CobraRunResult c3 = runCobra({"eval", "--data", merged, "--keepif", "_merge!=2", "--drop",
                                    "_merge", "--out", repPath});
      if (!c3.ok) { failure = cobraErrorStr(errorFromRaw(c3)); break; }
    } else {  // permute
      std::vector<double> shuffled = permOriginal;
      // Fisher-Yates over the Philox stream.
      for (int64_t j = nBase - 1; j > 0; --j) {
        const uint32_t d = cecon::Philox::draw32(seed, static_cast<uint64_t>(b),
                                                 static_cast<uint64_t>(j));
        const int64_t k = static_cast<int64_t>(d % static_cast<uint32_t>(j + 1));
        std::swap(shuffled[static_cast<size_t>(j)], shuffled[static_cast<size_t>(k)]);
      }
      // P2-PERF ARC-C C2-②: ONE call, not three. The old shape wrote a
      // two-column CSV, converted it to an hbin, dropped the original column
      // and 1:1-joined the new one back on the row index — a whole join
      // apparatus to move n doubles into a column that was already there, in
      // the order it was already in. `cobra setcol` patches exactly one column
      // from a headerless payload and preserves every other byte, which is the
      // operation this always was. It also stops MOVING the column: the join
      // re-appended it at the end of the varlist, where an in-place replace
      // leaves it where the user put it — closer to Stata, not further.
      const std::string csv = session.workdir() + "/pm_val.csv";
      registerScratchMint(csv);  // BUG-389
      if (!writePermuteValuesCsv(csv, shuffled)) {
        failure = "could not write the permuted values";
        break;
      }
      repPath = nextPath(session.workdir(), "pm_rep");
      CobraRunResult c1 = runCobra({"setcol", "--data", basePathIdx, "--var", permuteVar, "--values",
                                    csv, "--dtype", "f64", "--out", repPath});
      if (!c1.ok) { failure = cobraErrorStr(errorFromRaw(c1)); break; }
    }

    session.load(repPath);
    ojson inner = dispatch(innerCmd, session);
    bool innerFailed = false;
    for (const auto& blk : inner) {
      if (blk.is_object() && blk.value("type", "") == "error") {
        innerFailed = true;
        break;
      }
    }
    if (innerFailed) {
      // Stata drops a replicate whose command fails and reports the loss rather
      // than averaging over a hole it never mentions. Same here: a failed
      // replicate contributes nothing and is counted.
      continue;
    }
    RepValues vals;
    bool ok = true;
    for (const auto& e : expList) {
      std::optional<double> v = evalStat(e.expr, session);
      if (!v.has_value()) { ok = false; break; }
      vals.push_back(*v);
    }
    if (ok) reps_values.push_back(std::move(vals));
  }

  // The session's own dataset is never left on a replicate.
  session.load(startHbin);
  if (!failure.empty()) return rsError(method + ": " + failure);

  const int64_t nCompleted = static_cast<int64_t>(reps_values.size());
  if (nCompleted < 2) {
    return rsError(method + ": only " + std::to_string(nCompleted) +
                   " replicate(s) completed successfully — too few to report a standard error.");
  }

  // -- 4. aggregate ----------------------------------------------------------
  const double alpha = 1.0 - static_cast<double>(session.level) / 100.0;
  ojson rows = ojson::array();
  for (size_t k = 0; k < expList.size(); ++k) {
    double sum = 0.0;
    for (const auto& rv : reps_values) sum += rv[k];
    const double mean = sum / static_cast<double>(nCompleted);
    double ss = 0.0;
    for (const auto& rv : reps_values) {
      const double d = rv[k] - mean;
      ss += d * d;
    }

    ojson row;
    row["name"] = expList[k].name;
    row["b"] = observed[k];

    if (method == "permute") {
      // Counting, not a standard error: how often a permuted statistic reaches
      // the observed one. Stata reports both one-sided counts and the two-sided
      // p; SE(p) is the binomial standard error of the Monte-Carlo proportion.
      int64_t cLower = 0, cUpper = 0;
      for (const auto& rv : reps_values) {
        if (rv[k] <= observed[k]) ++cLower;
        if (rv[k] >= observed[k]) ++cUpper;
      }
      const double n = static_cast<double>(nCompleted);
      const double pLower = static_cast<double>(cLower) / n;
      const double pUpper = static_cast<double>(cUpper) / n;
      const double pTwo = std::min(1.0, 2.0 * std::min(pLower, pUpper));
      row["c_lower"] = cLower;
      row["c_upper"] = cUpper;
      row["n"] = nCompleted;
      row["p_lower"] = pLower;
      row["p_upper"] = pUpper;
      row["p_twosided"] = pTwo;
      row["se_lower"] = std::sqrt(pLower * (1.0 - pLower) / n);
      row["se_upper"] = std::sqrt(pUpper * (1.0 - pUpper) / n);
    } else if (method == "jackknife") {
      // Deterministic, and the one place the oracle judges our arithmetic digit
      // for digit. Stata's jackknife variance is the leave-one-out scaling
      // (n-1)/n * sum((theta_i - theta_bar)^2), with t(n-1) inference — fitted
      // against the P6 oracle row (SE .1256869 on `jackknife bx1=_b[x1]:
      // regress y x1 x2`), not assumed from the first plausible formula.
      const double n = static_cast<double>(nCompleted);
      const double se = std::sqrt((n - 1.0) / n * ss);
      const double dof = n - 1.0;
      const double t = se > 0 ? observed[k] / se : std::nan("");
      row["se"] = se;
      row["t"] = t;
      row["p"] = cecon::t_twosided_pvalue(t, dof);
      const double crit = cecon::t_critical(alpha, dof);
      row["ci_lo"] = observed[k] - crit * se;
      row["ci_hi"] = observed[k] + crit * se;
      row["df"] = dof;
    } else {  // bootstrap
      // The bootstrap SE is the sample SD of the replicate statistics; Stata's
      // default table is normal-based (z, not t).
      const double se = std::sqrt(ss / static_cast<double>(nCompleted - 1));
      const double z = se > 0 ? observed[k] / se : std::nan("");
      row["se"] = se;
      row["z"] = z;
      row["p"] = normalTwoSidedP(z);
      const double crit = cecon::z_critical(alpha);
      row["ci_lo"] = observed[k] - crit * se;
      row["ci_hi"] = observed[k] + crit * se;
      row["bias"] = mean - observed[k];
    }
    rows.push_back(row);
  }

  // -- 5. render, in Stata's own layout -------------------------------------
  // Emitted as `display` text blocks: the numbers reach the console, the log
  // tee and the webview through a path that already exists, so no ResultBlock
  // variant is added and contract B does not move. The layout is copied from
  // the oracle log, which is the spec for it.
  ojson lines = ojson::array();
  auto say = [&lines](const std::string& t) {
    lines.push_back(ojson{{"type", "display"}, {"text", t}});
  };
  {
    const std::string obsStr = commaGroup(nBase);
    if (method == "permute") {
      say(padRight("Monte Carlo permutation results", 51) +
          "Number of observations = " + padLeft(obsStr, 3));
      say(padRight("Permutation variable: " + permuteVar, 51) +
          "Number of permutations = " + padLeft(std::to_string(nCompleted), 3));
    } else {
      const std::string title = (method == "bootstrap") ? "Bootstrap results" : "Linear regression";
      say(padRight(title, 59) + "Number of obs = " + padLeft(obsStr, 3));
      say(padRight("", 59) + "Replications  = " + padLeft(std::to_string(nCompleted), 3));
    }
    say("");
    say("      Command: " + innerCmd);
    for (const auto& e : expList) {
      say(padLeft(e.name, 13) + ": " + e.expr);
    }
    say("");
  }
  if (method == "permute") {
    const std::string rule(79, '-');
    say(rule);
    say("             |                                               Monte Carlo error");
    say("             |                                              -------------------");
    say("           T |    T(obs)       Test       c       n      p  SE(p)   [95% CI(p)]");
    say("-------------+" + std::string(65, '-'));
    for (const auto& row : rows) {
      const int64_t cl = row["c_lower"].get<int64_t>();
      const int64_t cu = row["c_upper"].get<int64_t>();
      const int64_t n = row["n"].get<int64_t>();
      const double pl = row["p_lower"].get<double>();
      const double pu = row["p_upper"].get<double>();
      const double p2 = row["p_twosided"].get<double>();
      const auto ciL = clopperPearson(cl, n, alpha);
      const auto ciU = clopperPearson(cu, n, alpha);
      say(padLeft(row["name"].get<std::string>(), 12) + " |" +
          padLeft(g9cell(row["b"].get<double>()), 10) + padLeft("lower", 11) +
          padLeft(std::to_string(cl), 8) + padLeft(std::to_string(n), 8) +
          padLeft(prob4(pl), 7) + padLeft(prob4(row["se_lower"].get<double>()), 7) +
          padLeft(prob4(ciL.first), 7) + padLeft(prob4(ciL.second), 7));
      say(std::string(12, ' ') + " |" + std::string(10, ' ') + padLeft("upper", 11) +
          padLeft(std::to_string(cu), 8) + padLeft(std::to_string(n), 8) +
          padLeft(prob4(pu), 7) + padLeft(prob4(row["se_upper"].get<double>()), 7) +
          padLeft(prob4(ciU.first), 7) + padLeft(prob4(ciU.second), 7));
      say(std::string(12, ' ') + " |" + std::string(10, ' ') + padLeft("two-sided", 15) +
          std::string(12, ' ') + padLeft(prob4(p2), 7) + padLeft(prob4(0.0), 7) +
          padLeft(".", 7) + padLeft(".", 7));
    }
    say(rule);
    say("Notes: For lower one-sided test, c = #{T <= T(obs)} and p = p_lower = c/n.");
    say("       For upper one-sided test, c = #{T >= T(obs)} and p = p_upper = c/n.");
    say("       For two-sided test, p = 2*min(p_lower, p_upper); SE and CI approximate.");
  } else {
    const std::string rule(78, '-');
    const bool boot = (method == "bootstrap");
    const std::string stat = boot ? "z" : "t";
    say(rule);
    if (boot) {
      say("             |   Observed   Bootstrap                         Normal-based");
      say("             | coefficient  std. err.      z    P>|z|     [95% conf. interval]");
    } else {
      say("             |              Jackknife");
      say("             | Coefficient  std. err.      t    P>|t|     [95% conf. interval]");
    }
    say("-------------+" + std::string(64, '-'));
    for (const auto& row : rows) {
      const double statv = row[stat].get<double>();
      say(padLeft(row["name"].get<std::string>(), 12) + " |" +
          padLeft(g9cell(row["b"].get<double>()), 11) + padLeft(g9cell(row["se"].get<double>()), 11) +
          padLeft(fixed2(statv), 9) + padLeft(fixed3(row["p"].get<double>()), 8) +
          padLeft(g9cell(row["ci_lo"].get<double>()), 13) + padLeft(g9cell(row["ci_hi"].get<double>()), 12));
    }
    say(rule);
  }

  ojson out;
  out["type"] = "resample";
  out["method"] = method;
  out["command"] = innerCmd;
  out["n_obs"] = nBase;
  out["n_reps"] = nCompleted;
  out["n_reps_requested"] = nReps;
  out["level"] = session.level;
  out["rows"] = rows;
  out["est_sample"] = haveEstSample;
  if (haveSeed) out["seed"] = static_cast<long long>(seed);
  if (method == "permute") out["permute_var"] = permuteVar;
  // Stata warns when the inner command sets no estimation sample, because the
  // resampling then cannot exclude the rows the command itself would have
  // dropped. Carry the same warning rather than resampling silently.
  if (!haveEstSample) {
    out["warning"] = std::string(innerCmd.substr(0, innerCmd.find(' '))) +
                     " does not set e(sample), so no observations will be excluded from the "
                     "resampling because of missing values or other reasons.";
  }
  // The structured block rides LAST so a consumer that wants the numbers has
  // them, while the human-facing text is what the console and log show. Its
  // type is new but it is never the sole carrier of a result, and the
  // ResultBlock union the frontend switches on is untouched.
  lines.push_back(out);
  return lines;
}


// ---------------------------------------------------------------------------
// rolling  — `rolling explist, window(N) saving("FILE", replace): command`
//
// Same skeleton as the resampling prefixes: the only difference is that the
// per-"replicate" transform is a WINDOW SLICE of the time variable rather than
// a resample, so the inner command stays completely general.
//
// Requires a prior `tsset`/`xtset` (Stata's own precondition). Windows run over
// the DISTINCT observed time values, so a gap in the series shortens the span
// in calendar terms exactly as it does in Stata.
ojson runRolling(const std::string& expListRaw, const std::string& optsRaw,
                 const std::string& innerCmd, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (innerCmd.empty()) return rsError("rolling: no command found after the colon");
  if (!session.timevar.has_value() || session.timevar->empty()) {
    return rsError("rolling: the data must be tsset (or xtset) first — rolling needs to know which "
                   "variable is time.");
  }
  const std::string tvar = *session.timevar;

  std::string perr;
  std::vector<ExpItem> expList = parseExpList(expListRaw, &perr);
  if (expList.empty()) return rsError("rolling: " + perr);

  int window = 0;
  std::string savingPath;
  bool savingReplace = false;
  {
    size_t i = 0;
    while (i < optsRaw.size()) {
      while (i < optsRaw.size() &&
             (std::isspace(static_cast<unsigned char>(optsRaw[i])) || optsRaw[i] == ',')) ++i;
      if (i >= optsRaw.size()) break;
      const size_t ns = i;
      while (i < optsRaw.size() && (std::isalnum(static_cast<unsigned char>(optsRaw[i])) ||
                                    optsRaw[i] == '_')) ++i;
      std::string key = optsRaw.substr(ns, i - ns);
      for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      std::string val;
      if (i < optsRaw.size() && optsRaw[i] == '(') {
        int depth = 1; ++i;
        const size_t vs = i;
        while (i < optsRaw.size() && depth > 0) {
          if (optsRaw[i] == '(') ++depth;
          else if (optsRaw[i] == ')') --depth;
          if (depth > 0) ++i;
        }
        val = optsRaw.substr(vs, i - vs);
        if (i < optsRaw.size()) ++i;
      }
      if (key.empty()) break;
      if (key == "window") {
        try { window = std::stoi(val); } catch (const std::exception&) {
          return rsError("rolling: window() needs a positive integer");
        }
      } else if (key == "saving") {
        // `saving("FILE", replace)`
        std::string v = val;
        size_t comma = v.find(',');
        if (comma != std::string::npos) {
          std::string tail = v.substr(comma + 1);
          for (auto& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          savingReplace = tail.find("replace") != std::string::npos;
          v = v.substr(0, comma);
        }
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(0, 1);
        if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') ||
                              (v.front() == '\'' && v.back() == '\''))) {
          v = v.substr(1, v.size() - 2);
        }
        savingPath = v;
      } else if (key == "nodots" || key == "dots" || key == "noisily") {
        // presentation-only
      } else {
        return rsError("rolling: option `" + key +
                       "` is not implemented in this build. Implemented: window(), saving().");
      }
    }
  }
  if (window <= 0) return rsError("rolling: window(#) is required and must be positive");
  if (savingPath.empty()) {
    // Without saving() Stata REPLACES the dataset in memory with the results.
    // That branch is not implemented here, and saying so is better than
    // quietly computing results the user never sees.
    return rsError("rolling: this build requires saving(\"FILE\") — the in-memory-replacement form "
                   "is not implemented.");
  }

  try {
    (void)session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("rolling: could not materialize the working dataset: ") + e.what());
  }
  const std::string startHbin = *session.currentHbin();
  SessionState::ScratchPin pin(session, {startHbin});

  // Distinct, sorted time values.
  std::vector<double> tvals;
  {
    Datastore d(startHbin);
    if (!d.has(tvar)) return rsError("variable " + tvar + " not found");
    const cecon::Column& c = d.col(tvar);
    if (c.dtype == cecon::DType::Factor) {
      return rsError("rolling: the time variable `" + tvar + "` must be numeric");
    }
    const double* p = c.f64();
    tvals.assign(p, p + static_cast<size_t>(d.nRows()));
    std::sort(tvals.begin(), tvals.end());
    tvals.erase(std::unique(tvals.begin(), tvals.end()), tvals.end());
  }
  if (static_cast<int>(tvals.size()) < window) {
    return rsError("rolling: the series has only " + std::to_string(tvals.size()) +
                   " periods, fewer than window(" + std::to_string(window) + ")");
  }

  std::vector<double> startCol, endCol;
  std::vector<std::vector<double>> statCols(expList.size());
  std::string failure;
  for (size_t i = 0; i + static_cast<size_t>(window) <= tvals.size() && failure.empty(); ++i) {
    const double t0 = tvals[i];
    const double t1 = tvals[i + static_cast<size_t>(window) - 1];
    const std::string slice = nextPath(session.workdir(), "roll_win");
    std::string cond = tvar + ">=" + fmtStataDefaultG(t0) + " & " + tvar +
                       "<=" + fmtStataDefaultG(t1);
    CobraRunResult cr =
        runCobra({"eval", "--data", startHbin, "--keepif", cond, "--out", slice});
    if (!cr.ok) { failure = cobraErrorStr(errorFromRaw(cr)); break; }

    session.load(slice);
    ojson inner = dispatch(innerCmd, session);
    bool innerFailed = false;
    for (const auto& blk : inner) {
      if (blk.is_object() && blk.value("type", "") == "error") { innerFailed = true; break; }
    }
    std::vector<double> vals(expList.size(), std::nan(""));
    if (!innerFailed) {
      for (size_t k = 0; k < expList.size(); ++k) {
        std::optional<double> v = evalStat(expList[k].expr, session);
        if (v.has_value()) vals[k] = *v;
      }
    }
    startCol.push_back(t0);
    endCol.push_back(t1);
    for (size_t k = 0; k < expList.size(); ++k) statCols[k].push_back(vals[k]);
  }
  session.load(startHbin);
  if (!failure.empty()) return rsError("rolling: " + failure);
  if (startCol.empty()) return rsError("rolling: no windows were produced");

  // Results -> CSV -> hbin -> .dta, reproducing the shape Stata's own saved file
  // has (decoded from the oracle artifact, not re-derived from prose): start and
  // end as int %8.0g, each statistic as float %9.0g labelled with its own
  // expression, and the dataset labelled "rolling: <command word>".
  const std::string csv = session.workdir() + "/rolling_out.csv";
  registerScratchMint(csv);  // BUG-389
  {
    std::ofstream f(cecon::to_native_path(csv));
    if (!f) return rsError("rolling: could not write the results file");
    f << "start,end";
    for (const auto& e : expList) f << "," << e.name;
    f << "\n";
    f.precision(17);
    for (size_t r = 0; r < startCol.size(); ++r) {
      f << startCol[r] << "," << endCol[r];
      for (size_t k = 0; k < expList.size(); ++k) {
        f << ",";
        // Stata stores rolling results as `float` (its default type), so the
        // saved values ARE float-rounded. We declare the same storage type in
        // the sidecar, so round to it here too — otherwise the writer sees a
        // value that a float cannot hold losslessly, promotes the column to
        // double to avoid changing it, and our file would differ from Stata's
        // in the eighth digit while claiming the same declared type.
        if (!std::isnan(statCols[k][r])) {
          f << static_cast<double>(static_cast<float>(statCols[k][r]));
        }
      }
      f << "\n";
    }
  }
  const std::string outHbin = nextPath(session.workdir(), "rolling_res");
  {
    CobraRunResult cr = runCobra({"convert", "--data", csv, "--out", outHbin, "--delim", ","});
    if (!cr.ok) return rsError("rolling: " + cobraErrorStr(errorFromRaw(cr)));
  }
  const std::string metaPath = session.workdir() + "/rolling_meta.json";
  registerScratchMint(metaPath);  // BUG-389
  {
    std::string cmdWord = innerCmd.substr(0, innerCmd.find(' '));
    ojson meta;
    meta["data_label"] = "rolling: " + cmdWord;
    ojson vars = ojson::array();
    auto var = [&](const std::string& n, const std::string& t, const std::string& fmt,
                   const std::string& lbl) {
      vars.push_back(ojson{{"name", n}, {"stata_type", t}, {"format", fmt}, {"label", lbl}});
    };
    var("start", "int", "%8.0g", "");
    var("end", "int", "%8.0g", "");
    for (const auto& e : expList) var(e.name, "float", "%9.0g", e.expr);
    meta["variables"] = vars;
    std::ofstream mf(cecon::to_native_path(metaPath));
    if (!mf) return rsError("rolling: could not write the result metadata");
    mf << meta.dump(1);
  }
  if (!savingReplace && cecon::native_exists(savingPath)) {
    return rsError("file " + savingPath + " already exists");
  }
  {
    CobraRunResult cr = runCobra({"export-dta", "--data", outHbin, "--out", savingPath,
                                  "--in-meta", metaPath});
    if (!cr.ok) return rsError("rolling: " + cobraErrorStr(errorFromRaw(cr)));
  }

  ojson lines = ojson::array();
  lines.push_back(ojson{{"type", "display"},
                        {"text", "(running " + innerCmd.substr(0, innerCmd.find(' ')) +
                                     " on estimation sample)"}});
  lines.push_back(ojson{{"type", "display"},
                        {"text", "file " + savingPath + " saved"}});
  return lines;
}

}  // namespace handlers
}  // namespace piet
