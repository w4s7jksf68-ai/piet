// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// scalar_expression.cpp — C++ port of piet/backend/piet/display_hardening.py
// (M3a; the frozen Python file is the spec — see the header for design
// notes on why this module's helpers are NOT shared with
// piet/eval/macros.cpp's similarly-named anonymous-namespace helpers).
#include "piet/display_hardening.h"

#include "piet/bcal.h"  // AER scope-2 B2-3: %tbNAME rendering

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "piet/cclass.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/stata_format.h"
#include "piet/strutil.h"
#include "piet/eval/pyvalue.h"
#include "core/expr_blanks.h"  // FU-875: the blanks Stata drops inside ( ) and [ ]
#include "data/missing.h"  // BUG-341 subscript: the extended-missing kind of a cell

#include "display_internal.h"

// eval/macros.cpp (BUG-1545): an ancillary `_b[/name]`/`_se[/name]` read from the
// live fit's posted e(b)/e(V); the macro twin's resolveBRef reads it too.
namespace piet::eval {
std::optional<double> ancillaryEbCoef(const EvalSession& session, const std::string& name,
                                      const std::string& kind);
}  // namespace piet::eval

namespace piet {

using namespace display_detail;

namespace display_detail {

// ---------------------------------------------------------------------------
// strip() — file-local, mirrors Python str.strip() (ASCII whitespace only;
// matches the other M1/M2/M3 modules' own local strip() ports).
std::string strip(const std::string& s) {
  size_t a = 0, b = s.size();
  auto isSp = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
  };
  while (a < b && isSp(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && isSp(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

// ---------------------------------------------------------------------------
// _E_SCALAR_KEY — Stata e()-name -> cobra JSON key (display_hardening.py).
const std::map<std::string, std::string>& eScalarKey() {
  static const std::map<std::string, std::string> k = {
      {"r2", "r2"},       {"r2_a", "adj_r2"}, {"adj_r2", "adj_r2"}, {"rmse", "rmse"},
      {"mss", "mss"},     {"rss", "rss"},     {"df_m", "df_m"},     {"df_r", "df_r"},
      {"F", "F"},         {"N", "n_obs"},     {"N_clust", "n_clusters"},
      {"rank", "k_vars"}, {"p", "p_F"},       {"ll", "ll"},
      // BUG-156 (wiring half, P4 wave): the engine ALREADY emits the correct
      // within-R2 as "r2_within" (measured 0.414404 against Stata's 0.4144 on
      // the P2 fixture) — it was simply unreachable under its Stata name
      // `e(r2_w)`, so a correct number read as missing. The rest of BUG-156
      // (poisson e(ll)/e(chi2)/e(r2_p), xtreg between/overall/rho) is engine
      // COMPUTE work and stays open; only this unreachable-but-correct entry
      // is closed here.
      // Duplicated table — the twin lives in the other of
      // {eval/macro_results.cpp, language/scalar_expression.cpp}. Keep them in step or
      // `display e(r2_w)` and `scalar q = e(r2_w)` disagree.
      {"r2_w", "r2_within"},
      // BUG-156 (wiring half, P2-PREP) — TWIN of the same block in
      // eval/macros.cpp's eScalarKey(). See that copy for why the
      // identity-mapped names are listed rather than left to fall through.
      // The engine does not emit these keys yet (parallel workstream), so
      // `display e(r2_p)` still answers `.` today — by design; this closes
      // the NAME half so the compute half is a one-file change.
      {"ll_0", "ll_0"},     {"chi2", "chi2"},     {"r2_p", "r2_p"},
      {"sigma_u", "sigma_u"}, {"sigma_e", "sigma_e"}, {"rho", "rho"},
      {"corr", "corr_u_xb"},
  };
  return k;
}

// _RE_B_REF / _RE_SE_REF
const std::regex& reBRef() {
  // `_b[eq:name]` (AER scope-2 B2): twin of macros.cpp's reB -- keep in step.
  // The dot is B2-4's: `_b[ATE:r1vs0.t]`. Twin of macros.cpp's reB.
  // BUG-629 (display half): the head class takes a digit and the tail a `#`,
  // as macros.cpp's reB/reSe already do -- a factor-level coefficient is named
  // `2.c` (`1.c#c.x1` for an interaction cell), and `display _b[2.c]` fell
  // through to "matrix _b not found" at rc 1 while `local q = _b[2.c]` answered.
  // Unicode names: a byte >= 0x80 is a name byte (core/ident.h), so
  // `display _b[سعر2]` reads the coefficient. Twin of macros.cpp's reB.
  static const std::regex r(
      R"(_b\[((?:[A-Za-z_0-9/]|[^\x00-\x7F])(?:[A-Za-z0-9_:.#]|[^\x00-\x7F])*)\])");
  return r;
}
const std::regex& reSeRef() {
  static const std::regex r(
      R"(_se\[((?:[A-Za-z_0-9/]|[^\x00-\x7F])(?:[A-Za-z0-9_:.#]|[^\x00-\x7F])*)\])");
  return r;
}
// _RE_E_REF — e(name) reference, applied with fullmatch semantics in Python
// (re.match anchored via ^...$ in the pattern itself).
const std::regex& reERef() {
  static const std::regex r(R"(^e\(\s*(.+?)\s*\)$)");
  return r;
}
}  // namespace display_detail

namespace {

// identifier-shaped token check (step 7 of _resolve_token)
const std::regex& reIdent() {
  static const std::regex r(std::string("^") + eval::kRxName + "$");  // Unicode names too
  return r;
}
// _RE_C_REF — c(name) system-value reference (BUG-091). Whole-token match on a
// single identifier inside c(...); no nesting, matching Stata's c() namespace.
const std::regex& reCRef() {
  static const std::regex r(R"(^c\(\s*([A-Za-z_]\w*)\s*\)$)");
  return r;
}

// _SPECIAL_SCALARS
bool isSpecialScalar(const std::string& tok) { return tok == "_N" || tok == "_n"; }

// jsonToFloat — mirrors Python's `float(val)` where val came from a JSON
// scalar (number/bool/string); null/object/array -> TypeError -> nullopt.
std::optional<double> jsonToFloat(const nlohmann::ordered_json& v) {
  if (v.is_number()) return v.get<double>();
  if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
  if (v.is_string()) return eval::pythonFloat(v.get<std::string>());
  return std::nullopt;
}

// (BUG-554: the file-local `%.9g` fmtNumeric that stood here is gone -- every
// resolver below substitutes at full precision through fmtValueFullPrecision.)

// BUG-406 (rediagnosed 2026-09-06): a resolved VARIABLE VALUE is a token this
// function hands back as TEXT, which the display layer then re-renders through
// the user's format. Rendering it here at %.9g therefore threw away every digit
// past the ninth BEFORE `%22.17f` was ever applied — `display %22.17f v[1]`
// printed -77.55172410000000127 for a value the hbin stores exactly as
// -77.55172413793085, and the entry blamed the CSV transport for it. This is
// the same defect BUG-382 fixed for e() scalars in macros.cpp, in the other
// layer: substitute at FULL precision and let the format do the rounding.
// Bare `display v[1]` is unchanged — a substituted numeric literal is re-rendered
// through the %9.0g default (fmtStataDefaultG), which is what the freeze corpus pins.
std::string fmtValueFullPrecision(double v) {
  if (std::isnan(v)) return ".";
  if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
  return eval::pythonFloatRepr(v);
}

// ---------------------------------------------------------------------------
// BUG-456 (2026-09-08): the e-MACRO arm of the display twin. TWIN of
// macros.cpp's resolveERef -- same rule, same literal shape, and the same
// "always a string, never a number" ruling measured on StataNow 19.5 BE
// (internal capture refute3_capture.log, 40/40 macro shapes). Returns the
// quoted literal when the name is a non-empty e-string, else nullopt so the
// caller falls through to resolveEScalar (a scalar, or numeric missing for an
// absent name). Keep in step with macros.cpp.
std::optional<std::string> resolveEStringLiteral(const std::string& name, SessionState& session) {
  const std::string* s = session.eval.eStrings.find(name);
  if (s == nullptr || s->empty()) return std::nullopt;
  std::string lit = "\"";
  for (char ch : *s) {
    if (ch == '"') lit += "\\\"";
    else lit += ch;
  }
  lit += "\"";
  return lit;
}

// _resolve_e_scalar
//
// BUG-554 (W2D-T03): the returned TEXT is %.9g (fmtNumeric) and stays so --
// bare `display e(r2)` and every expression built on it are byte-identical to
// before. `stored`, when non-null, receives the double that text was made
// from, so an explicit display format renders the stored value instead of its
// nine-digit rounding (`display %18.0g e(r2)`, `%21x e(ll)`). Left untouched
// when the answer is missing.
std::string resolveEScalar(const std::string& name, SessionState& session,
                           double* stored = nullptr) {
  // BUG-421: this twin reads SessionState::lastModel()->result; a posted
  // e-scalar (pca's e(rho), `estadd scalar`) lives in session.eval.eScalars and
  // reached neither. Checked LAST everywhere, so a fitted key always wins.
  const auto posted = [&]() -> std::string {
    // BUG-554: full precision; the display format does the rounding.
    if (const double* v = session.eval.eScalars.find(name)) {
      if (stored) *stored = *v;
      return fmtValueFullPrecision(*v);
    }
    return ".";
  };
  const auto& lm = session.lastModel();
  if (!lm.has_value() || !lm->result.has_value()) return posted();
  const auto& result = *lm->result;
  // Python: `if not result:` — falsy for {} or None. Treat empty object the
  // same as "no result".
  if (result.is_null() || (result.is_object() && result.empty())) return posted();

  std::string key = name;
  auto it = eScalarKey().find(name);
  if (it != eScalarKey().end()) key = it->second;

  if (!result.is_object() || !result.contains(key) || result.at(key).is_null()) return posted();
  auto val = jsonToFloat(result.at(key));
  // Engine n_clusters 0 = unclustered fit -> Stata e(N_clust) missing.
  if (val && name == "N_clust" && *val == 0.0) val.reset();
  if (!val) return posted();
  if (stored) *stored = *val;
  return fmtValueFullPrecision(*val);  // BUG-554
}

// The `<eq>:name` spelling of an equation-less `_b[name]`, appended to
// `tryNames` when the first coeftable row carries an equation.
void addFirstEquationName(const ojson& coeftable, const std::string& varname,
                          std::vector<std::string>& tryNames) {
  if (varname.find(':') != std::string::npos || coeftable.empty()) return;
  const auto& first = coeftable.front();
  if (!first.is_object() || !first.contains("name") || !first.at("name").is_string()) return;
  const std::string head = first.at("name").get<std::string>();
  const size_t colon = head.find(':');
  if (colon == std::string::npos || colon == 0) return;
  tryNames.push_back(head.substr(0, colon + 1) + varname);
}

// _resolve_b_ref
//
// BUG-162 (W:P5): `err` distinguishes the two ways this can fail to produce a
// number. A coefficient that EXISTS but whose value is missing is Stata's `.`
// — a legitimate result. A coefficient NAME that does not exist in the model
// is not: Stata raises r(111), and returning `.` for it turned a typo into a
// missing value that then propagated silently through every expression built
// on it. When `err` is non-null the caller surfaces an honest error; when it
// is null (paths that have no error channel) the historical `.` is kept, so
// no caller changes behaviour without opting in.
//
// W2D-T03 (FU-88 and its sibling): both refusals carry Stata's code in the
// `r(N): ` prefix handleDisplay turns into the block's `reason`, so
// `capture noisily display _b[x]` leaves _rc 111 instead of 1.
//   * no fitted model (a fresh session, after `ereturn clear`, or after a
//     posting command such as pca): r(111) "_b not allowed when e(b) is not
//     present" (w2g_ebmat.log:2099-2105 e15c_disp_bx1, :2181-2187
//     e15c_disp_bx1_times2, :2489-2495 e15d_disp_bx1_2; w2p_estim_ev.log:409-414).
//     r(301) "last estimates not found" belongs to `matrix b = e(b)` and
//     `matlist e(b)` (w2g_ebmat.log:2223-2238), not to this read.
//   * a model without that coefficient: r(111) "[x1] not found"
//     (w2g_ebmat.log:1689-1694 e15a_disp_bx1; also e15a_oaxaca_disp_bx1,
//     e15c_disp_bx1, e15d_disp_bx1).
// `stored` is resolveEScalar's BUG-554 channel: the coefficient's double, set
// only when a number is returned.
std::string resolveBRef(const std::string& varname, const std::string& kind,
                         SessionState& session, std::string* err = nullptr,
                         double* stored = nullptr) {
  // eb-order T-P3 (SPEC part 1): the same source as the macro twin and as
  // e(b) itself, EvalSession::coefSource() -- the live e()'s estimation block --
  // not lastModel()->result, a third store that estpost/erepost/`estimates use`
  // leave on an earlier fit.
  const nlohmann::ordered_json* source = session.eval.coefSource();
  if (source == nullptr) {
    // Stata (StataNow 19.5 BE, 2026-09-29, `display _b[x]` / `display _se[x]` after
    // `clear all`): r(111) "_b not allowed when e(b) is not present" / "_se not
    // allowed when e(V) is not present". The same text and rc for a live e()
    // that has no e(b) (w2g_ebmat.log:2105 e15c_disp_bx1 after pca, :2495
    // after rddensity). The `r(NNN): ` prefix is what display's splitter
    // (eval::splitErrCode, in handleDisplay) turns into the block's `reason`;
    // without it this was "last estimates not found" at rc 1.
    if (err)
      *err = kind == "b" ? "r(111): _b not allowed when e(b) is not present"
                         : "r(111): _se not allowed when e(V) is not present";
    return ".";
  }

  // BUG-158: intercept alias — see the twin comment on macros.cpp's
  // resolveBRef. These two copies must stay in step.
  std::vector<std::string> tryNames{varname};
  if (!varname.empty() && varname.front() == '/') {
    tryNames.push_back(varname.substr(1) + ":_cons");
    tryNames.push_back("/:" + varname.substr(1));
  }
  if (varname == "_cons") tryNames.push_back("(intercept)");
  else if (varname == "(intercept)") tryNames.push_back("_cons");
  // `_b[yb:x1]` after a depvar-equation fit is the bare row (BUG-1328); the
  // macro twin does the same through the same helper.
  if (const size_t colon = varname.find(':'); colon != std::string::npos && colon > 0) {
    const std::string eq = eval::depvarEquationOf(session.eval);
    if (!eq.empty() && varname.compare(0, colon, eq) == 0 && colon == eq.size()) {
      const std::string bare = varname.substr(colon + 1);
      tryNames.push_back(bare);
      if (bare == "_cons") tryNames.push_back("(intercept)");
    }
  }
  if (const std::string o = eval::omittedColumnSpelling(varname); !o.empty()) tryNames.push_back(o);
  {
    const auto& result = *source;
    if (result.is_object() && result.contains("coeftable") && result.at("coeftable").is_array()) {
      // A name with no equation is read from e(b)'s FIRST equation when no
      // row carries it bare: after `margins, dydx(x) at(z=(0 5)) post`, e(b)
      // is `x:1bn._at x:2._at` and `_b[1._at]` is `_b[x:1._at]` (StataBE 19.5,
      // w2v_margins_corpus F1_dydx_at_post_bname1/2). Tried last, so a bare
      // row always wins. The macro twin (macros.cpp resolveBRef) does the same.
      addFirstEquationName(result.at("coeftable"), varname, tryNames);
      for (const auto& want : tryNames) {
        for (const auto& row : result.at("coeftable")) {
          if (row.is_object() && row.contains("name") && row.at("name").is_string() &&
              row.at("name").get<std::string>() == want) {
            const char* field = (kind == "b") ? "b" : "se";
            // Found the coefficient; a missing VALUE here is a real `.`.
            if (!row.contains(field) || row.at(field).is_null()) return ".";
            auto val = jsonToFloat(row.at(field));
            if (!val) return ".";
            // BUG-554: this is TEXT the display layer re-renders through the
            // user's format, so `%.9g` here cut `display %18.0g _b[a]` to nine
            // digits (w2_cmdtriage.log after_restore). Full precision, as
            // BUG-406/BUG-382 did; a bare `display _b[a]` still renders %9.0g.
            if (stored) *stored = *val;
            return fmtValueFullPrecision(*val);
          }
        }
      }
    }
  }
  // A column only the fit's recorded stripe names: a factor's base level
  // (`_b[0b.c]` = `_b[0.c]` = 0, w2f_residuals.log e_b_0bc/e_b_0c) or an `o.`
  // spelling -- the macro twin reads the same eval::ebStripeCoef.
  if (const std::optional<double> v = eval::ebStripeCoef(*source, varname, kind)) {
    if (stored) *stored = *v;
    return fmtValueFullPrecision(*v);
  }
  // An ancillary parameter only the posted e(b)/e(V) carry (BUG-1545), as the
  // macro twin reads it.
  if (const std::optional<double> v = eval::ancillaryEbCoef(session.eval, varname, kind)) {
    if (stored) *stored = *v;
    return fmtValueFullPrecision(*v);
  }
  // An abbreviated / reordered spelling of a posted column -- the macro twin's
  // eval::coefFullName.
  if (const std::string full = eval::coefFullName(*source, varname); !full.empty())
    return resolveBRef(full, kind, session, err, stored);
  // Name not in the model — BUG-162. r(111), the code the macro twin carries
  // (macros.cpp resolveBRef via splitErrCode's default): `display _b[9.c]` was rc 1
  // where `display _b[9.c]+0` and `local q = _b[9.c]` were r(111) -- two codes
  // for one read once reBRef took level names (BUG-629). The base level
  // (`_b[0b.c]`, `_b[0.c]`) is 0 in Stata (w2f_residuals.log e_b_0bc/e_b_0c) but
  // has no coeftable row here, so it is refused at r(111) on both layers: an
  // honest refusal, named in the task report as an oracle-adjudicated follow-up.
  // Text: Stata brackets the name (StataNow 19.5 BE: `display _b[9.c]` and
  // `display %18.0g _se[9.c]` both print `[9.c] not found`); the macro twin agrees.
  if (err) *err = "r(111): [" + varname + "] not found";
  return ".";
}

// _resolve_N
// BUG-512: ONE source for `_N` -- SessionState::macroObsCount(), the same value
// the macro layer's hook reads. This used to answer `.` with no data in memory
// where Stata answers 0 (w2_smalls.log: `set obs 12` reports "_N was 0" after
// `clear all`), and once evalArith learned `_N` the two spellings of one line
// would have disagreed: `display _N` -> `.`, `display _N*2` -> 0. It also stops
// opening a Datastore for a count the resident card already carries.
std::string resolveN(const std::string& token, SessionState& session) {
  if (token == "_n") return "1";
  return std::to_string(session.macroObsCount());
}

// _resolve_c — c(name) system values. BETA A4 (BS-091/BS-101): the narrow
// current_date/current_time-only resolver that used to live here (BUG-091)
// is now folded into piet::resolveCValue (piet/cclass.h) — the single
// source EVERY expression context reads c() through (display here, plus
// gen/replace/if/scalar/local via piet::substituteCRefs), not just display's
// own tokenizer. See cclass.h for the full ~15-key spine and the "honest gap
// for anything else" philosophy this preserves verbatim.

// BUG-341: VAR[idx] (1-based) -- the value display prints for a variable
// subscript; nullopt when `name` is not a dataset variable. A string variable
// answers its text verbatim (isString), a numeric one its full-precision value,
// an extended missing its own `.a`-`.z` token (the data plane keeps the kind in
// the NaN payload, data/missing.h). Out of range and index 0 give `.`, or ""
// for a string column.
struct VarObs {
  std::string text;
  bool isString = false;
  bool inRange = true;      // BUG-718: false for an index outside 1.._N
  bool colString = false;   // BUG-718: the column is a string one (set in range or not)
};

// name-binding-display-literals: `name` may be a unique abbreviation of a
// variable (resolveVarName, the one rule); the column read is the variable's.
std::optional<VarObs> readVarObs(const std::string& nameIn, long long idx, SessionState& session) {
  const auto numText = [](double v) {
    return std::isnan(v) ? std::string(cecon::missing_text(cecon::missing_kind(v)))
                         : fmtValueFullPrecision(v);
  };
  const std::optional<std::string> resolvedName = resolveVarName(nameIn, session);
  if (!resolvedName) return std::nullopt;
  const std::string& name = *resolvedName;
  // The computed-column cache answers a freshly `gen`d / `destring`d column
  // without touching disk — the same surface every other read consumer uses.
  if (const auto* cc = session.residentComputedCol(name)) {
    const bool str = cc->dtype == "factor";
    // Out of range a string column reads "" (w2ar_data_io_family.log
    // dx_fx_fc15_L1map `0||`: dxmap[1] with _N 0), a numeric one `.`.
    if (idx < 1 || idx > cc->nRows) return VarObs{str ? "" : ".", str, false, str};
    const size_t k = static_cast<size_t>(idx - 1);
    // A string variable has no missing value: a cell with no text is "".
    if (str) return VarObs{k < cc->strs.size() ? cc->strs[k] : std::string(), true, true, true};
    return VarObs{k < cc->f64.size() ? numText(cc->f64[k]) : std::string("."), false};
  }
  Datastore* ds = session.datastore();  // materializes a virtual head if needed
  if (ds == nullptr || !ds->has(name)) return VarObs{".", false};
  const bool str = ds->dtype(name) == "factor";
  if (idx < 1 || idx > ds->nRows()) return VarObs{str ? "" : ".", str, false, str};
  const size_t k = static_cast<size_t>(idx - 1);
  const cecon::Column& col = ds->col(name);
  if (str) {
    const int32_t code = col.codes()[k];
    const auto& levels = col.levels;
    if (code >= 0 && static_cast<size_t>(code) < levels.size())
      return VarObs{levels[static_cast<size_t>(code)], true, true, true};
    return VarObs{std::string(), true, true, true};
  }
  return VarObs{numText(col.f64()[k]), false};
}

// A `,` outside quotes and brackets: `NAME[i,j]` is a matrix element, not a
// variable subscript (`x[max(1,2)]` is one index).
bool hasTopLevelComma(const std::string& inner) {
  int depth = 0;
  for (size_t k = 0; k < inner.size(); ++k) {
    const char c = inner[k];
    if (c == '"') {
      const size_t close = inner.find('"', k + 1);
      if (close == std::string::npos) return false;
      k = close;
    } else if (c == '(' || c == '[') {
      ++depth;
    } else if (c == ')' || c == ']') {
      --depth;
    } else if (c == ',' && depth == 0) {
      return true;
    }
  }
  return false;
}

// BUG-1339: the observation a variable subscript names. The index is any
// numeric expression (`x[1+1]`, `x[_N-1]`, `x[_n+1]`); StataBE 19.5 truncates a
// fractional one (`x[1.5]` is x[1], `x[2.9]` x[2]) and reads a missing one as
// out of range (`x[.]`, `x[1/0]` are `.`), so both come back as an index
// readVarObs answers. nullopt = not a numeric expression.
std::optional<long long> obsIndex(const std::string& raw, SessionState& session) {
  const std::string idx = strip(eval::substituteObsSpecials(raw, session.eval));
  if (idx.empty()) return std::nullopt;
  std::string raised;
  bool isStr = false;
  auto r = eval::evalArith(idx, &session.eval, &raised, &isStr);
  if (!r.has_value() || isStr || !raised.empty() || r->empty()) return std::nullopt;
  if (piet::fmt::isMissingToken(*r)) return 0;
  char* end = nullptr;
  const double v = std::strtod(r->c_str(), &end);
  if (end == nullptr || *end != '\0') return std::nullopt;
  if (!(std::fabs(v) < 9e15)) return 0;  // NaN and the missing band too
  return static_cast<long long>(std::trunc(v));
}

}  // namespace

namespace display_detail {

// FU-640: `name[i]` naming a STRING variable -> its text; nullopt otherwise.
std::optional<std::string> stringVarSubscript(const std::string& token, SessionState& session) {
  static const std::regex reVarSub(std::string("^(") + eval::kRxName +
                                   R"()\s*\[\s*(-?\d+)\s*\]$)");
  if (token.find('[') == std::string::npos) return std::nullopt;
  const std::string subTok = eval::substituteObsSpecials(token, session.eval);
  std::smatch vs;
  if (!std::regex_match(subTok, vs, reVarSub)) return std::nullopt;
  long long idx = 0;
  try {
    idx = std::stoll(vs[2].str());
  } catch (...) {
    return std::nullopt;
  }
  auto obs = readVarObs(vs[1].str(), idx, session);
  if (!obs || !obs->isString) return std::nullopt;
  return obs->text;
}

// FU-640: every bare identifier in `expr` that names a dataset variable is
// replaced by its observation-1 value: a number (parenthesised) or a quoted
// string. Quoted text, function names (`name(`), subscripted names (`name[`)
// and member names (`.name`) are left alone. nullopt when nothing was bound,
// or when a string value holds a `"` this layer's literals cannot carry.
std::optional<std::string> bindBareVarsToObs1(const std::string& expr, SessionState& session) {
  std::string out;
  bool any = false;
  size_t i = 0;
  // core/ident.h: a byte >= 0x80 is a name byte, so a Unicode name is one name.
  const auto isId0 = [](char c) { return cecon::ident::isIdentStartByte(c); };
  const auto isId = [](char c) { return cecon::ident::isIdentByte(c); };
  while (i < expr.size()) {
    const char c = expr[i];
    if (c == '"') {
      const size_t j = expr.find('"', i + 1);
      const size_t end = j == std::string::npos ? expr.size() : j + 1;
      out.append(expr, i, end - i);
      i = end;
      continue;
    }
    if (!isId0(c) || (i > 0 && (isId(expr[i - 1]) || expr[i - 1] == '.'))) {
      out += c;
      ++i;
      continue;
    }
    size_t j = i;
    while (j < expr.size() && isId(expr[j])) ++j;
    const std::string name = expr.substr(i, j - i);
    size_t k = j;
    while (k < expr.size() && std::isspace(static_cast<unsigned char>(expr[k]))) ++k;
    const bool callOrSub = k < expr.size() && (expr[k] == '(' || expr[k] == '[');
    std::optional<VarObs> obs;
    if (!callOrSub) obs = readVarObs(name, 1, session);
    if (!obs) {
      out += name;
    } else if (obs->isString) {
      if (obs->text.find('"') != std::string::npos) return std::nullopt;
      out += "\"" + obs->text + "\"";
      any = true;
    } else {
      out += "(" + obs->text + ")";
      any = true;
    }
    i = j;
  }
  if (!any) return std::nullopt;
  return out;
}

// FU-207: a variable subscript `name[i]` INSIDE an expression -- `missing(g[5])`,
// `(g[5] >= .)` -- is replaced by the observation's value, the same reading
// resolveToken's 8b arm gives a lone `g[5]` (readVarObs: a number parenthesised,
// `.`/`.a` for a missing cell). Before this only the lone token was read;
// inside a call the subscript reached the evaluator as text and every such line
// was r(198) "invalid syntax". The index is an integer literal once `_N`/`_n`
// are substituted (the 8b rule); a name that is not a dataset variable, a
// `[i,j]` matrix subscript and quoted text are left alone. So is a STRING
// variable's subscript: a bound `"ab"` beside unquoted text is not evaluated as
// a string expression downstream but echoed (`display s[2]+"c"` printed
// `"ab"+c` at rc 0 where Stata prints `abc`), so it keeps bin/main's path
// (r(111) there). nullopt when nothing was bound.
std::optional<std::string> bindVarSubscripts(const std::string& expr, SessionState& session) {
  if (expr.find('[') == std::string::npos) return std::nullopt;
  static const std::regex reSub(std::string("^(") + eval::kRxName + R"()\s*\[([^\[\],"]*)\])");
  std::string out;
  bool any = false;
  size_t i = 0;
  const auto isId = [](char c) { return cecon::ident::isIdentByte(c); };  // core/ident.h
  while (i < expr.size()) {
    const char c = expr[i];
    if (c == '"') {
      const size_t j = expr.find('"', i + 1);
      const size_t end = j == std::string::npos ? expr.size() : j + 1;
      out.append(expr, i, end - i);
      i = end;
      continue;
    }
    std::smatch m;
    const std::string rest = expr.substr(i);
    if (cecon::ident::isIdentStartByte(c) &&
        (i == 0 || (!isId(expr[i - 1]) && expr[i - 1] != '.')) &&
        std::regex_search(rest, m, reSub)) {
      const std::string idxText =
          strip(eval::substituteObsSpecials(m.str(2), session.eval));
      long long idx = 0;
      bool intLit = !idxText.empty();
      for (size_t k = 0; intLit && k < idxText.size(); ++k)
        intLit = std::isdigit(static_cast<unsigned char>(idxText[k])) != 0 ||
                 (k == 0 && idxText[k] == '-' && idxText.size() > 1);
      if (intLit) {
        try {
          idx = std::stoll(idxText);
        } catch (...) {
          intLit = false;
        }
      }
      std::optional<VarObs> obs;
      if (intLit) obs = readVarObs(m.str(1), idx, session);
      if (obs && obs->isString) {
        out += m.str(0);
        i += static_cast<size_t>(m.length(0));
        continue;
      }
      if (obs) {
        out += "(" + obs->text + ")";
        any = true;
        i += static_cast<size_t>(m.length(0));
        continue;
      }
      out += m.str(1);
      i += m.str(1).size();
      continue;
    }
    out += c;
    ++i;
  }
  if (!any) return std::nullopt;
  return out;
}

// FU-142 / FU-853: the index one past the end of the quoted span that starts
// at `i` -- a plain "..." literal or a compound `"..."' one (which nests) --
// or `i` itself when no span starts there. An unterminated span runs to the end.
size_t skipQuotedSpan(const std::string& s, size_t i) {
  if (i < s.size() && s[i] == '"') {
    const size_t j = s.find('"', i + 1);
    return j == std::string::npos ? s.size() : j + 1;
  }
  if (i + 1 < s.size() && s[i] == '`' && s[i + 1] == '"') {
    int depth = 0;
    size_t j = i;
    while (j < s.size()) {
      if (j + 1 < s.size() && s[j] == '`' && s[j + 1] == '"') {
        ++depth;
        j += 2;
      } else if (j + 1 < s.size() && s[j] == '"' && s[j + 1] == '\'') {
        j += 2;
        if (--depth == 0) return j;
      } else {
        ++j;
      }
    }
    return s.size();
  }
  return i;
}

// The index of the bracket closing the one opened at `open` (`(` or `[`),
// quote-aware and counting both kinds; npos when it never closes.
size_t matchingClose(const std::string& s, size_t open) {
  int depth = 0;
  size_t i = open;
  while (i < s.size()) {
    const size_t q = skipQuotedSpan(s, i);
    if (q != i) {
      i = q;
      continue;
    }
    const char c = s[i];
    if (c == '(' || c == '[') {
      ++depth;
    } else if (c == ')' || c == ']') {
      if (--depth == 0) return i;
    }
    ++i;
  }
  return std::string::npos;
}

// FU-853 / FU-226 / FU-349: the subscripts inside a display expression,
// resolved by the display layer before evalArith sees it. evalArith reads a
// `NAME[...]` as a matrix element with literal indices only, so a dataset cell
// inside a call or an operation (`year(d[1])`, `missing(x[2])`, `t[2]*2`) was
// `t not found` or invalid syntax, and a matrix index that is itself an
// expression (`c[1,colsof(c)-1]`) was echoed.
//   * `VAR[i]`, VAR a dataset variable and i an integer-valued expression
//     (`_n`/`_N` included): the cell's value, a number parenthesised (so a
//     negative cell keeps its sign under `^`: `t[1]^2` is `(-1)^2`) at full
//     precision, or a quoted string. Out of range is `.`, as in 8b.
//   * `MAT[i,j]`, MAT a defined matrix: each index evaluated to its integer,
//     leaving `MAT[2,3]` for evalArith's own matrix pass.
// `_b[...]`/`_se[...]` and anything inside quotes are left alone, and so is a
// subscript whose index does not evaluate to an integer. nullopt when nothing
// changed, or when a string cell holds a `"` this layer's literals cannot carry.
// `unknown`, when non-null, receives the first `NAME[...]` whose NAME is neither
// a variable nor a matrix: Stata's r(111) `NAME not found` (the one-index form
// is w2b_dataio.log:110-113; the `[i,j]` form takes the same code, unpinned).
std::optional<std::string> bindSubscripts(const std::string& expr, SessionState& session,
                                          std::string* unknown) {
  // core/ident.h: a byte >= 0x80 is a name byte, so a Unicode name is one name.
  const auto isId0 = [](char c) { return cecon::ident::isIdentStartByte(c); };
  const auto isId = [](char c) { return cecon::ident::isIdentByte(c); };
  const auto intIndex = [&](const std::string& raw) -> std::optional<long long> {
    const std::string idx = strip(eval::substituteObsSpecials(raw, session.eval));
    if (idx.empty()) return std::nullopt;
    std::string raised;
    bool isStr = false;
    auto r = eval::evalArith(idx, &session.eval, &raised, &isStr);
    if (!r.has_value() || isStr || !raised.empty()) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(r->c_str(), &end);
    if (r->empty() || end == nullptr || *end != '\0' || !std::isfinite(v) ||
        v != std::floor(v) || std::fabs(v) > 9e15)
      return std::nullopt;
    return static_cast<long long>(v);
  };
  const auto isMatrix = [&](const std::string& name) {
    for (const auto& kv : session.eval.matrices)
      if (kv.first == name) return true;
    for (const auto& kv : session.eval.eMatrices)
      if (kv.first == name) return true;
    return false;
  };
  std::string out;
  bool any = false;
  size_t i = 0;
  while (i < expr.size()) {
    const size_t q = skipQuotedSpan(expr, i);
    if (q != i) {
      out.append(expr, i, q - i);
      i = q;
      continue;
    }
    const char c = expr[i];
    if (!isId0(c) || (i > 0 && (isId(expr[i - 1]) || expr[i - 1] == '.'))) {
      out += c;
      ++i;
      continue;
    }
    size_t j = i;
    while (j < expr.size() && isId(expr[j])) ++j;
    const std::string name = expr.substr(i, j - i);
    const size_t close = (j < expr.size() && expr[j] == '[') ? matchingClose(expr, j)
                                                              : std::string::npos;
    if (close == std::string::npos || name == "_b" || name == "_se" || name == "_coef") {
      out += name;
      i = j;
      continue;
    }
    const std::string inner = expr.substr(j + 1, close - j - 1);
    // A top-level comma splits a matrix's row and column index.
    size_t comma = std::string::npos;
    {
      int depth = 0;
      size_t k = 0;
      while (k < inner.size()) {
        const size_t qq = skipQuotedSpan(inner, k);
        if (qq != k) {
          k = qq;
          continue;
        }
        if (inner[k] == '(' || inner[k] == '[') ++depth;
        if (inner[k] == ')' || inner[k] == ']') --depth;
        if (inner[k] == ',' && depth == 0) {
          comma = k;
          break;
        }
        ++k;
      }
    }
    std::optional<std::string> replacement;
    if (comma != std::string::npos) {
      if (isMatrix(name)) {
        const auto r = intIndex(inner.substr(0, comma));
        const auto cIdx = intIndex(inner.substr(comma + 1));
        if (r && cIdx) {
          replacement = name + "[" + std::to_string(*r) + "," + std::to_string(*cIdx) + "]";
          if (*replacement == expr.substr(i, close + 1 - i)) replacement.reset();
        }
      } else if (unknown != nullptr && unknown->empty() && !readVarObs(name, 1, session)) {
        *unknown = name;
      }
    } else if (auto idx = obsIndex(inner, session)) {
      if (auto obs = readVarObs(name, *idx, session)) {
        if (obs->isString) {
          if (obs->text.find('"') != std::string::npos) return std::nullopt;
          replacement = "\"" + obs->text + "\"";
        } else {
          replacement = "(" + obs->text + ")";
        }
      } else if (unknown != nullptr && unknown->empty() && !isMatrix(name)) {
        *unknown = name;
      }
    }
    if (replacement) {
      out += *replacement;
      any = true;
    } else {
      out.append(expr, i, close + 1 - i);
    }
    i = close + 1;
  }
  if (!any) return std::nullopt;
  return out;
}

// _resolve_token
//
// BUG-554 (W2D-T03): `stored`, when non-null, receives the double behind a
// `_b[]`, `_se[]`, `e(name)` or `e(name)[i,j]` answer. The text is unchanged
// (see resolveEScalar); handleDisplay renders an explicitly formatted bare item
// from `stored` so the format sees every bit.
//
// `strValue`, when non-null, is set when the answer is a STRING variable's cell
// text, which the caller must carry as a string literal, not as expression text.
std::string resolveToken(const std::string& token, SessionState& session,
                          std::string* err, std::optional<double>* stored,
                          bool* strValue) {
  std::smatch m;
  double sv = 0.0;
  const auto keep = [&](const std::string& text, bool got) {
    if (got && stored) *stored = sv;
    return text;
  };

  // `[eq]_b[name]` / `[eq]_se[name]` are `_b[eq:name]` / `_se[eq:name]`
  // (BUG-1328), matched before the `[eq]column` form below.
  static const std::regex eqBRef(std::string(R"(\[()") + eval::kRxName +
                                 R"()\]_(b|se)\[([^\]]+)\])");
  if (std::regex_match(token, m, eqBRef)) {
    sv = std::nan("");
    const std::string t = resolveBRef(m[1].str() + ":" + m[3].str(), m[2].str(), session, err, &sv);
    return keep(t, !std::isnan(sv));
  }
  // Equation shorthand [eq]column is the same coefficient lookup.
  static const std::regex equationCoef(
      std::string(R"(\[()") + eval::kRxName +
      R"()\]((?:[A-Za-z_0-9]|[^\x00-\x7F])(?:[\w.#]|[^\x00-\x7F])*))");
  if (std::regex_match(token, m, equationCoef)) {
    sv = std::nan("");
    const std::string t = resolveBRef(m[1].str() + ":" + m[2].str(), "b", session, err, &sv);
    return keep(t, !std::isnan(sv));
  }
  // BUG-1545: an ancillary parameter named with parentheses (`_b[/var(e.yc)]`
  // after tobit), which reBRef/reSeRef do not take -- the macro twin's reAnc.
  {
    static const std::regex reAnc(R"(_(b|se)\[(/[A-Za-z_]\w*\([^()\[\]]*\))\])");
    if (std::regex_match(token, m, reAnc)) {
      sv = std::nan("");
      std::string t = resolveBRef(m[2].str(), m[1].str(), session, err, &sv);
      return keep(t, !std::isnan(sv));
    }
  }
  // 1. _b[var]
  if (std::regex_match(token, m, reBRef())) {
    sv = std::nan("");
    std::string t = resolveBRef(m[1].str(), "b", session, err, &sv);
    return keep(t, !std::isnan(sv));
  }
  // 2. _se[var]
  if (std::regex_match(token, m, reSeRef())) {
    sv = std::nan("");
    std::string t = resolveBRef(m[1].str(), "se", session, err, &sv);
    return keep(t, !std::isnan(sv));
  }
  // 3. e(name) — collapse redundant double-wrapping e(e(r2)).
  //    BUG-663: only when the token IS one reference. The anchored pattern also
  //    matches `e(cmd)==e(cmd)` (inner `cmd)==e(cmd`), which read as an absent
  //    e-scalar and printed `.` where StataBE prints 1; a `)` that closes before
  //    its `(` opens leaves the token to the expression path.
  const auto oneReference = [](const std::string& inner) {
    int depth = 0;
    for (char c : inner) {
      if (c == '(') ++depth;
      if (c == ')' && --depth < 0) return false;
    }
    return depth == 0;
  };
  if (std::regex_match(token, m, reERef()) && oneReference(m[1].str())) {
    std::string inner = m[1].str();
    while (true) {
      std::smatch mm;
      if (!std::regex_match(inner, mm, reERef())) break;
      inner = mm[1].str();
    }
    // e(sample) T2 (internal note esample/SPEC.md §5.4): the
    // estimation-sample mask is not an e-scalar, so resolveEScalar answered `.`
    // at rc 0 here (`display e(sample)`, measured on main; Stata prints 0 with no
    // results, w2_esample.log:100 A2, and observation 1's value after a fit,
    // :365 B16). Same reading as the macro layer (eval/macros.cpp
    // resolveEstScalarRefs' e(sample) arm): SessionState::esampleObs1, whose
    // refusal (a model PIET has no mask for) is the named r(198) on `err`.
    if (inner == "sample") {
      std::string why;
      if (std::optional<int> v = session.esampleObs1(&why)) return std::to_string(*v);
      if (err != nullptr && err->empty()) *err = "r(198): " + why;
      return token;
    }
    // BUG-456: string -> scalar (the e-matrix names never reach display's
    // token resolver as a bare value; `display e(b)` is refused upstream).
    if (auto lit = resolveEStringLiteral(inner, session)) return *lit;
    sv = std::nan("");
    std::string t = resolveEScalar(inner, session, &sv);
    return keep(t, !std::isnan(sv));
  }
  // 3b. W2D-T03 (FU-45): `e(NAME)[i,j]`, one element of an e-class matrix. It
  //     used to match nothing here and printed its own source text at rc 0.
  //     The element comes from the POSTED e-matrix store -- the same store the
  //     macro layer's `e(NAME)[i,j]` / `el(e(NAME),i,j)` substitution reads
  //     (eval/macros.cpp substituteMatrixRefs) -- so display and `scalar s =`
  //     agree. A fit that has not posted the matrix is refused by name rather
  //     than answered from the coeftable: the coeftable carries neither Stata's
  //     column order (_cons last, base and omitted columns) nor V's
  //     off-diagonal, so any element built from it could be silently wrong.
  //     Unpinned (no golden displays e(b)[i,j]): the r(503) out-of-range and the
  //     r(111) not-posted refusals.
  {
    static const std::regex reEElem(
        std::string(R"(^e\(\s*()") + eval::kRxName +
        R"()\s*\)\s*\[\s*(\d+)\s*,\s*(\d+)\s*\]$)");
    std::smatch em;
    if (std::regex_match(token, em, reEElem)) {
      const std::string name = em[1].str();
      const eval::EvalMatrix* mat = session.eval.findClassMatrix('e', name);
      // BUG-1350: a fit's own e(b)/e(V) come from the one e() matrix resolver
      // the macro layer's `e(NAME)[i,j]` and el() read (Stata's column order,
      // base and omitted columns, e(V) only where the engine sent it), so
      // display answers what `local x = e(b)[1,2]` answers.
      std::optional<eval::EvalMatrix> fitMat;
      bool diagonalOnly = false;
      if (mat == nullptr) {
        auto a = session.eval.resolveEMatrixFor(name, eval::EvalSession::EReader::kOperand);
        if (a.matrix.has_value()) {
          fitMat = std::move(a.matrix);
          diagonalOnly = a.diagonalOnly;
          mat = &*fitMat;
        } else if (!a.absent) {
          if (err) *err = "r(" + std::to_string(a.rc) + "): " + a.why;
          return ".";
        }
      }
      if (mat == nullptr) {
        if (err) *err = "r(111): matrix e(" + name + ") not found";
        return ".";
      }
      long long i = 0, j = 0;
      try {
        i = std::stoll(em[2].str());
        j = std::stoll(em[3].str());
      } catch (...) {
        i = 0;
      }
      if (diagonalOnly && i != j) {
        if (err) {
          const std::string* cmd = session.eval.eStrings.find("cmd");
          *err = "r(198): e(V)[" + em[2].str() + "," + em[3].str() +
                 "] is not available in this version of PIET after `" +
                 (cmd != nullptr ? *cmd : std::string()) +
                 "`: the engine sent the fit's standard errors (the diagonal of e(V)), not "
                 "its covariances";
        }
        return ".";
      }
      if (i < 1 || j < 1 || static_cast<size_t>(i) > mat->data.size() ||
          static_cast<size_t>(j) > mat->data[static_cast<size_t>(i - 1)].size()) {
        if (err) *err = "r(503): conformability error";
        return ".";
      }
      sv = mat->data[static_cast<size_t>(i - 1)][static_cast<size_t>(j - 1)];
      if (std::isnan(sv)) return ".";
      return keep(fmtValueFullPrecision(sv), true);
    }
  }
  // 4. _N / _n
  if (isSpecialScalar(token)) {
    return resolveN(token, session);
  }
  // 4b. _pi, Stata's built-in constant: it printed its own name at rc 0
  //     (w2av_name_binding bd_dy_pi `[3.1415927]`, bd_dy_fpi `%21x _pi`).
  if (token == "_pi") return fmtValueFullPrecision(M_PI);
  // 4c. A dataset variable, by its name or a unique abbreviation of it,
  //     before a stored scalar of the same name, as in Stata: observation 1,
  //     a string variable's cell text (w2av_name_binding bd_dy_varsc `[10]`
  //     with scalar v = 5 beside variable v; bd_dy_strvar `[ab1]`, where the
  //     factor code 0 printed; bd_dy_abbr `lng` -> lngdp; bd_dy_abbrsc, where
  //     scalar lngd = 3 lost to the variable lngdp it abbreviates).
  if (std::regex_match(token, reIdent())) {
    if (auto obs = readVarObs(token, 1, session)) {
      if (obs->isString && strValue != nullptr) *strValue = true;
      return obs->text;
    }
  }
  // 5. User-defined named scalars (whole-token match) take precedence over
  //    e()-class bare aliases.
  {
    const std::string* sv = session.eval.scalars.find(token);
    if (sv != nullptr) return *sv;
  }
  // 6. Bare e()-class alias.
  if (eScalarKey().count(token) > 0) {
    if (auto lit = resolveEStringLiteral(token, session)) return *lit;  // BUG-456
    sv = std::nan("");
    std::string t = resolveEScalar(token, session, &sv);
    return keep(t, !std::isnan(sv));
  }
  // 7. (The dataset variable is step 4c, ahead of the scalars.)
  // 8. c(name) — system c-class values (BUG-091; BETA A4 BS-091/BS-101:
  //    the spine of the shared piet::resolveCValue, cclass.h). Any other
  //    c(...) name falls through to evalArith and is refused downstream, r(133)
  //    as the classifier words it (BUG-1330) — never silently swallowed.
  if (std::regex_match(token, m, reCRef())) {
    auto cv = resolveCValue(m[1].str(), session);
    if (cv.has_value()) {
      // BUG-1345: a string value is a string to the format that takes it:
      // `%-20s c(current_date)` fills the field (it was `invalid display
      // format %-20s`, rc 1).
      if (cv->isString && strValue != nullptr) *strValue = true;
      return cv->text;
    }
  }
  // 8b. BUG-341 — `var[i]` variable subscript. MUST precede the matrix arm
  //     below: that arm matches any `NAME[...]`, so a variable subscript fell
  //     into it, found no matrix of that name, and answered `matrix z2 not
  //     found` — wrong twice over (wrong object kind, and the variable exists).
  //     The form was never served by the resident plane at all: before subscript
  //     support arrived PIET echoed `z2[1]` literally, and when it arrived it
  //     read a surface the virtual head does not populate.
  //
  //     Semantics measured on StataBE 2026-08-14 (not inferred): in-range gives
  //     the row's value (`v[1]`→10, `v[5]`→50 on v=_n*10); OUT of range and
  //     index 0 give missing (`v[6]`→`.`, `v[0]`→`.`); a STRING variable gives
  //     the string, not a code (`s[1]`→`ab`). A `[i,j]` index keeps the matrix
  //     route.
  //
  //     BUG-512: `display x[_N]` -- the last observation, the commonest subscript
  //     there is -- never reached this arm, because `_N` is not an integer
  //     literal; it fell into the matrix arm below and answered `matrix x not
  //     found`. The index is resolved with the macro layer's own substitution
  //     (`_N` -> the count, `_n` -> 1: w2_smalls.log section A).
  //
  //     BUG-1339: the index is any expression (obsIndex): a lone `x[1+1]`,
  //     `x[_N-1]` or `x[`k'+1]` answered `matrix x not found` at rc 1, because
  //     only an integer literal was claimed here and bindSubscripts never sees a
  //     lone item that this arm has already refused. A string variable's cell is
  //     a string to the format that takes it (`%5s s[2]` fills the field).
  {
    // `display قيمة[2]`: a Unicode variable name is one name (core/ident.h).
    static const std::regex reVarSub(std::string("^(") + eval::kRxName + R"()\s*\[(.*)\]$)");
    std::smatch vs;
    if (std::regex_match(token, vs, reVarSub) && !hasTopLevelComma(vs[2].str()) &&
        resolveVarName(vs[1].str(), session)) {
      if (std::optional<long long> idx = obsIndex(vs[2].str(), session)) {
        if (auto obs = readVarObs(vs[1].str(), *idx, session)) {
          if (obs->colString && strValue != nullptr) *strValue = true;
          return obs->text;
        }
      }
    }
  }

  // 9. BUG-162 (W:P5) — `NAME[i,j]` matrix subscript. A DEFINED matrix is
  //    resolved by the arithmetic layer downstream; an UNDEFINED one used to
  //    fall through to the `return token` below and print its own source text
  //    (`ZZZNOSUCH[1,1]`) with rc 0, which reads like output. Stata raises
  //    r(111). Only the undefined case is claimed here — a defined matrix
  //    still reaches its existing resolver untouched.
  {
    static const std::regex reMatSub(std::string("^(") + eval::kRxName + R"()\s*\[[^\]]*\]$)");
    std::smatch mm;
    if (std::regex_match(token, mm, reMatSub)) {
      const std::string name = mm[1].str();
      bool defined = false;
      for (const auto& kv : session.eval.matrices) {
        if (kv.first == name) { defined = true; break; }
      }
      if (!defined) {
        for (const auto& kv : session.eval.eMatrices) {
          if (kv.first == name) { defined = true; break; }
        }
      }
      if (!defined) {
        // `NAME[...]` that names no matrix and that the variable arm (8b) did not
        // read is Stata's r(111) `NAME not found` whatever the index:
        // w2b_dataio.log:110-113 (`display ... xs[1]` after a tostring that
        // skipped generate(xs)), and StataBE 19.5 (2026-11-03) `nosuch[1+1]`,
        // `ZZZNOSUCH[1,1]` and, for a variable x, `x[]` and `x[1,1]`; a name that
        // begins two variables' names is `NAME ambiguous abbreviation`, r(111).
        // Each was `matrix NAME not found` at rc 1 but for a one-integer index.
        bool ambiguous = false;
        (void)resolveVarName(name, session, &ambiguous);
        if (err) *err = "r(111): " + name + (ambiguous ? " ambiguous abbreviation" : " not found");
        return ".";
      }
    }
  }

  return token;
}

// name-binding-display-literals: the first bare name in display text that binds
// to nothing -- no variable (exact or unique abbreviation), stored scalar,
// matrix or built-in -- as Stata's refusal: r(111) `NAME not found`, or `NAME
// ambiguous abbreviation` when it begins two variables' names (w2av_name_binding
// bd_dy_unb, bd_dy_ambig, bd_dy_errkw: `display "[" foo "]"` printed `[foo]` at
// rc 0). Quoted text, calls (`name(`), subscripts (`name[`), members and
// missing codes (`.a`) and the inside of numbers are not names. nullopt when
// every name binds.
std::optional<std::string> unboundNameRefusal(const std::string& text, SessionState& session) {
  // core/ident.h: a byte >= 0x80 is a name byte, so a Unicode name is one name.
  const auto isId0 = [](char c) { return cecon::ident::isIdentStartByte(c); };
  const auto isId = [](char c) { return cecon::ident::isIdentByte(c); };
  size_t i = 0;
  while (i < text.size()) {
    const size_t q = skipQuotedSpan(text, i);
    if (q != i) {
      i = q;
      continue;
    }
    const char c = text[i];
    if (!isId0(c) || (i > 0 && (isId(text[i - 1]) || text[i - 1] == '.'))) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < text.size() && isId(text[j])) ++j;
    const std::string name = text.substr(i, j - i);
    size_t k = j;
    while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k]))) ++k;
    i = j;
    if (k < text.size() && (text[k] == '(' || text[k] == '[')) continue;
    if (name == "_n" || name == "_N" || name == "_pi" || name == "_rc") continue;
    if (session.eval.scalars.find(name) != nullptr) continue;
    if (session.eval.findMatrix(name) != nullptr) continue;
    bool ambiguous = false;
    if (resolveVarName(name, session, &ambiguous)) continue;
    return ambiguous ? "r(111): " + name + " ambiguous abbreviation" : "r(111): " + name + " not found";
  }
  return std::nullopt;
}

}  // namespace display_detail

// BUG-1248: see display_hardening.h.
std::optional<std::string> exprUnboundNameRefusal(const std::string& text, SessionState& session) {
  return unboundNameRefusal(text, session);
}

// name-binding-display-literals: see display_hardening.h.
std::optional<std::string> resolveVarName(const std::string& name, SessionState& session,
                                          bool* ambiguous) {
  if (ambiguous != nullptr) *ambiguous = false;
  // Stata's reserved names ([U] 11.3) are never a variable's, so never an
  // abbreviation of one either (`_pi` must not read a variable `_pix`).
  static const char* const kReserved[] = {"_all", "_b",    "byte",   "_coef", "_cons", "double",
                                          "float", "if",   "in",     "int",   "long",  "_n",
                                          "_N",    "_pi",  "_pred",  "_rc",   "_se",   "_skip",
                                          "strL",  "using", "with"};
  if (name.empty()) return std::nullopt;
  for (const char* r : kReserved)
    if (name == r) return std::nullopt;
  if (session.residentComputedCol(name) != nullptr) return name;
  // The shadow card knows without an mmap; datastore() is the honest fallback
  // when there is no current card (and for an exact name the card lacks).
  const std::vector<VarInfo>* card = session.residentReadCard();
  if (card != nullptr) {
    for (const auto& vi : *card)
      if (vi.name == name) return name;
  }
  Datastore* ds = nullptr;
  if (session.currentHbin().has_value()) {
    ds = session.datastore();
    if (ds != nullptr && ds->has(name)) return name;
  }
  std::optional<std::string> hit;
  bool many = false;
  const auto consider = [&](const std::string& v) {
    if (v.size() <= name.size() || v.compare(0, name.size(), name) != 0) return;
    if (hit) many = true;
    else hit = v;
  };
  if (card != nullptr) {
    for (const auto& vi : *card) consider(vi.name);
  } else if (ds != nullptr) {
    for (const auto& v : ds->names()) consider(v);
  }
  if (many) {
    if (ambiguous != nullptr) *ambiguous = true;
    return std::nullopt;
  }
  return hit;
}

// BUG-718: the macro layer's reader of `VAR[idx]` (SessionState::macroVarObs)
// -- display's own readVarObs, so the two spellings of one subscript agree.
std::optional<VarObservation> readVarObservation(const std::string& name, long long idx,
                                                 SessionState& session) {
  std::optional<VarObs> o = readVarObs(name, idx, session);
  if (!o) return std::nullopt;
  // Outside 1.._N display keeps its `.`; the macro layer is told the column's
  // kind so it can give a string column its "" (VarObservation).
  return VarObservation{o->text, o->inRange ? o->isString : o->colString, o->inRange};
}

// AER-SCOPE1 bundle E (TABLES) -- see the header for why this reuses display's
// own chain instead of growing a second e() reader.
std::optional<double> evalScalarExpr(const std::string& expr, SessionState& session) {
  auto toNumber = [](const std::string& t) -> std::optional<double> {
    if (t.empty()) return std::nullopt;
    const char* b = t.c_str();
    char* endp = nullptr;
    double v = std::strtod(b, &endp);
    if (endp == b || endp == nullptr || *endp != '\0') return std::nullopt;
    if (!std::isfinite(v)) return std::nullopt;
    return v;
  };

  std::string e = strutil::trim(expr);
  if (e.empty()) return std::nullopt;

  // A Stata missing value is a legitimate resolution, not a number: `.`/`.a`
  // must NOT come back as 0, which is the shape a table cell would print.
  if (e == "." || (e.size() == 2 && e[0] == '.' && std::isalpha(static_cast<unsigned char>(e[1])))) {
    return std::nullopt;
  }

  // 1. Whole-token forms, exactly the ones display resolves before arithmetic.
  //    A resolution failure leaves the token unchanged, which `toNumber` then
  //    rejects -- so an unknown name falls through to step 2 rather than being
  //    reported as a value.
  {
    std::string err;
    std::string tok = resolveToken(e, session, &err);
    if (err.empty()) {
      if (auto v = toNumber(tok)) return v;
    }
  }

  // 2. Real arithmetic. resolveEstScalarRefs is the find-anywhere pass for
  //    _b[]/_se[]/scalar()/e() (never e(b)/e(V), which are matrices) and
  //    substituteCRefs the same for c(); evalArith then evaluates. `raised`
  //    non-empty means the expression named something undefined -- an honest
  //    nullopt, not a zero.
  // e(sample) T2: with an error channel, so a refused e() read (the named
  // r(198) of a model with no sample mask, an unknown _b[] name) is an honest
  // nullopt here rather than text left for evalArith to misread.
  std::string resolveErr;
  std::string joined =
      substituteCRefs(eval::resolveEstScalarRefs(e, session.eval, &resolveErr), session);
  if (!resolveErr.empty()) return std::nullopt;
  std::string raised;
  bool isString = false;
  auto r = eval::evalArith(joined, &session.eval, &raised, &isString);
  if (!r.has_value() || isString || !raised.empty()) return std::nullopt;
  return toNumber(*r);
}

}  // namespace piet
