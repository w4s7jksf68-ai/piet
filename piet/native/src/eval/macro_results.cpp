// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// macro results: see macro_modules.md for ownership and call flow.
#include "piet/eval/macros.h"
#include "estimators/postest_names.h"  // coefFullName: the engine's coefficient-name rule

#include "../dispatch_profile.h"  // WS-D1: per-line regex counter (opt-in, stderr)

#ifndef _WIN32
#include <fnmatch.h>  // WP-W5: POSIX-only; the Windows branch uses globMatchPeriod below
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <regex>
#include <set>
#include <unordered_set>
#include <vector>

#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/pyvalue.h"
#include "piet/strutil.h"
#include "core/ident.h"  // Unicode names: the shared identifier byte tests
#include "core/expr_blanks.h"  // FU-875: the blanks Stata drops inside ( ) and [ ]
#include "data/stata_dates.h"  // the literal pseudo-function scan (liftDateLiterals)
#include "io/win_compat.h"  // cecon::to_native_path (BUG-050 wide open)

#include "macro_expressions_internal.h"
#include "macro_matrices_internal.h"
#include "macro_results_internal.h"
#include "macro_scanners_internal.h"
#include "macro_text_internal.h"
#include "macros_internal.h"

namespace piet::eval { namespace macro_detail {} }

namespace piet::eval::macro_detail {
std::string resolveBRef(const std::string& varname, const std::string& kind,
                         const EvalSession& session, std::string* err = nullptr);
}

namespace piet::eval::macro_detail {


// ---------------------------------------------------------------------------
// display_hardening resolvers (the est-scalar subset _eval_arith needs)

// _E_SCALAR_KEY: MOVED out of this anonymous namespace to piet::eval proper
// (AER-SCOPE1 bundle E, 2026-09-04) so handlers_aer_tables.cpp can freeze the
// same e() scalar names into an eststo snapshot. The body is unchanged and now
// lives just below the anonymous namespace's closing brace; declared in
// eval/macros.h. Exposing it was the alternative to a THIRD copy of the table --
// there are already two (the twin is display_hardening.cpp's std::map version,
// identical in content today and commented as such in both files).

std::optional<double> jsonToFloat(const nlohmann::ordered_json& v) {
  if (v.is_number()) return v.get<double>();
  if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
  if (v.is_string()) return pythonFloat(v.get<std::string>());
  return std::nullopt;  // null / object / array -> TypeError
}


// A posted e() scalar as expression text: Stata's `.` for a missing one, never
// pythonFloatRepr's "nan" (which the evaluator reads as a variable name).
std::string postedEText(double v) {
  return std::isnan(v) ? std::string(".") : pythonFloatRepr(v);
}


std::string resolveEScalar(const std::string& name, const EvalSession& session) {
  // BUG-421 (2026-09-06): a handler-POSTED e-scalar lives in `eScalars`, not in
  // the estimation JSON. `pca` posts e(rho)/e(f)/e(trace) there and `estadd
  // scalar` posts whatever the user names; reading only lastModelResult made
  // every one of them answer `.` on a surface that had the value in hand.
  // The model result still wins when both carry the name, so a fitted e(N)
  // cannot be shadowed by a posted one.
  if (const double* posted = session.eScalars.find(name);
      posted != nullptr && !session.lastModelResult) {
    return postedEText(*posted);
  }
  if (!session.lastModelResult) return ".";
  const auto& result = *session.lastModelResult;
  if (result.is_null() || (result.is_object() && result.empty())) {
    if (const double* p2 = session.eScalars.find(name)) return postedEText(*p2);
    return ".";
  }
  std::string key = name;
  for (const auto& [k, v] : eScalarKey()) {
    if (k == name) {
      key = v;
      break;
    }
  }
  if (!result.contains(key) || result.at(key).is_null()) {
    if (const double* p3 = session.eScalars.find(name)) return postedEText(*p3);
    return ".";
  }
  auto f = jsonToFloat(result.at(key));
  // A clustered fit cannot have 0 clusters: the engine's n_clusters 0 means
  // "unclustered", which Stata reports as e(N_clust) missing.
  if (f && name == "N_clust" && *f == 0.0) f.reset();
  if (!f) {
    if (const double* p4 = session.eScalars.find(name)) return postedEText(*p4);
    return ".";
  }
  // BUG-382 (off-pin sweep, 2026-09-01): substitute at FULL precision, exactly
  // as the r-class resolver always has. fmtNumeric is %.9g, so `scalar s =
  // e(ll)` received 9 significant digits and no later format could recover the
  // rest (measured: %20.15e of the scalar disagreed with the block's own value
  // in the 10th digit). Bare `display e(ll)` is unchanged by this: display
  // RE-RENDERS a substituted numeric literal through its own %9.0g default —
  // the r-class precedent proves it, since r() substitutes pythonFloatRepr and
  // bare `display r(mean)` still prints Stata's short form (and 173 freeze
  // fixtures pin exactly that rendering). The entry's original one-line-fix
  // refutation reasoned display would echo the text; the architecture says it
  // re-renders, and the freeze battery is the verifier.
  return pythonFloatRepr(*f);
}


// BUG-158 (P4 wave): Stata names the intercept `_cons`; this engine's no-FE
// coeftable names it `(intercept)` (build_model's design-matrix column name),
// while the FE path already emits `_cons` (feols.cpp's reconstructed reghdfe
// intercept). Both spellings therefore coexist in ONE build, so the alias has
// to be resolved at the consumer. Without it `_b[_cons]` — the spelling almost
// all real Stata code uses — matched nothing and fell through to ".", i.e. a
// silent missing with rc 0.
// NOTE: this resolver is DUPLICATED (macros.cpp for expressions/if-conditions,
// display_hardening.cpp for `display`). Both copies must carry this alias or
// `display _b[_cons]` and `scalar q = _b[_cons]` disagree — the exact
// two-layer split that makes a wrong answer silent. Change them together.
// BUG-456 (2026-09-08): `err` is the r(111) channel the display twin has had
// since BUG-162 (display_hardening.cpp resolveBRef). A coefficient NAME that is
// not in the model is r(111) "[nosuch] not found" on EVERY Stata surface --
// local, scalar, gen, count, assert, if (refute3_capture.log V3-V8) -- and
// returning `.` for it turned a typo into a missing that `assert _b[nosuch]==0`
// then reported as r(9), a wrong-reason error. When `err` is null the historical
// `.` is kept so no caller changes behaviour without opting in.
// eb-order T-P3 (SPEC part 1): the coefficients come from coefSource() -- the
// live e()'s estimation block, the store e(b) is built from and the one the
// display twin reads -- never from lastModelResult while a session is attached
// (after estpost, erepost and `estimates use` that mirror describes an earlier
// e()). A null source -- a live e() without an e(b) (pca, rdrobust, rddensity,
// estpost) or no e() at all -- is Stata's r(111) "_b not allowed when e(b) is
// not present" on every surface, never `.` at rc 0 (w2g_ebmat.log:2155
// e15c_local_bx1, :2169, :2187 after pca; w2v_estpost_ematrix.log
// ep15f_local_bx1 and ep15f_disp_bx1_times2 after `ereturn clear`, ep13_local_bx1
// after estpost). The no-e() `.` the SPEC first kept as a named divergence is
// gone: the capture refuted it (FU-949).
std::string resolveBRef(const std::string& varname, const std::string& kind,
                         const EvalSession& session, std::string* err ) {
  const nlohmann::ordered_json* source = session.coefSource();
  if (source == nullptr) {
    if (err != nullptr && err->empty())
      *err = kind == "b" ? "_b not allowed when e(b) is not present"
                         : "_se not allowed when e(V) is not present";
    return ".";
  }
  const auto& result = *source;
  // Try the requested spelling first, then its intercept alias.
  std::vector<std::string> tryNames{varname};
  if (!varname.empty() && varname.front() == '/') {
    tryNames.push_back(varname.substr(1) + ":_cons");
    tryNames.push_back("/:" + varname.substr(1));
  }
  if (varname == "_cons") tryNames.push_back("(intercept)");
  else if (varname == "(intercept)") tryNames.push_back("_cons");
  // `_b[yb:x1]` after a depvar-equation fit is the bare row (BUG-1328); the
  // display twin does the same through the same helper.
  if (const size_t colon = varname.find(':'); colon != std::string::npos && colon > 0) {
    const std::string eq = depvarEquationOf(session);
    if (!eq.empty() && varname.compare(0, colon, eq) == 0 && colon == eq.size()) {
      const std::string bare = varname.substr(colon + 1);
      tryNames.push_back(bare);
      if (bare == "_cons") tryNames.push_back("(intercept)");
    }
  }
  if (const std::string o = omittedColumnSpelling(varname); !o.empty()) tryNames.push_back(o);
  if (!result.is_null() && result.contains("coeftable")) {
    // An equation-less name falls back to e(b)'s FIRST equation, tried last
    // (`_b[1._at]` after `margins, dydx(x) at() post` is `_b[x:1._at]`,
    // w2v_margins_corpus F1_dydx_at_post_bname1/2) -- the display twin's
    // addFirstEquationName.
    const auto& ct = result.at("coeftable");
    if (varname.find(':') == std::string::npos && ct.is_array() && !ct.empty() &&
        ct.front().is_object() && ct.front().contains("name") && ct.front().at("name").is_string()) {
      const std::string head = ct.front().at("name").get<std::string>();
      const size_t colon = head.find(':');
      if (colon != std::string::npos && colon > 0) tryNames.push_back(head.substr(0, colon + 1) + varname);
    }
    for (const auto& want : tryNames) {
      for (const auto& row : result.at("coeftable")) {
        if (row.contains("name") && row.at("name") == want) {
          const char* field = (kind == "b") ? "b" : "se";
          if (!row.contains(field) || row.at(field).is_null()) return ".";
          auto f = jsonToFloat(row.at(field));
          if (!f) return ".";
          return pythonFloatRepr(*f);  // BUG-382: _b[]/_se[] carry full precision too
        }
      }
    }
  }
  // A column only the fit's recorded stripe names (a factor's base level, an
  // `o.` spelling): eval::ebStripeCoef, shared with the display twin.
  if (const std::optional<double> v = ebStripeCoef(*source, varname, kind))
    return pythonFloatRepr(*v);
  // An ancillary parameter only the posted e(b)/e(V) carry (BUG-1545).
  if (const std::optional<double> v = ancillaryEbCoef(session, varname, kind))
    return pythonFloatRepr(*v);
  // An abbreviated / reordered spelling of a posted column (coefFullName).
  if (const std::string full = coefFullName(*source, varname); !full.empty())
    return resolveBRef(full, kind, session, err);
  // Name not in the model -- r(111), same text as the display twin (BUG-162):
  // Stata's `[name] not found`, bracketed.
  if (err != nullptr && err->empty()) *err = "[" + varname + "] not found";
  return ".";
}


// BUG-456 (2026-09-08): the bare-form e(name) resolver's STRING arm. Measured
// on StataNow 19.5 BE (internal capture refute3_capture.log, 40/40 macro
// shapes): a bare e-MACRO is a STRING in every expression context -- `ereturn
// local m "5"` then `display e(m)+1` is r(109), `assert e(m)=="5"` rc 0,
// `assert e(m)==5` r(109), `local t = e(m)` keeps the TEXT. [U] 18.9 rule 1.3
// as printed ("examined to determine whether its contents can be interpreted
// as a number") is NOT what the binary does, so the numeric-looking text is
// never turned into a number here. The literal is built exactly like the bare
// r(name) string arm in expandPass (BUG-374): a double-quoted literal with any
// interior `"` escaped, so it reaches evalArith / the row fold as a string
// VALUE and not as text to re-evaluate. MEASURED CORRECTION (2026-09-09, review
// finding 2): the ESCAPE does not survive the evaluator -- `estadd local qq
// "a\"b"` then `local t = e(qq)` is r(198) "invalid syntax in 'e(qq)'", where
// Stata passes `a"b` through at rc 0 (REFUTE-0 §3, T1-T9). It is an honest
// error and it is unreachable today (PIET refuses compound double quotes
// outright -- `estadd local qq `"a"b"'` is an engine_error piet_gap -- so no
// e-string can currently carry a `"`), but the escape is a claim this code
// cannot yet make: named, not fixed. An ABSENT name (and an empty macro,
// which setEString never stores) is numeric missing, never "" -- `assert
// e(nosuch)==.` rc 0, `assert e(nosuch)==""` r(109) (A8/A9/N1) -- which is
// what falling through to resolveEScalar answers. Guard order at the caller is
// matrix -> string -> scalar: after `ereturn local b "x"` the bare e(b) is
// still the coefficient matrix in Stata (W1-W8).
std::string resolveERef(const std::string& name, const EvalSession& session) {
  if (const std::string* s = session.eStrings.find(name); s != nullptr && !s->empty()) {
    std::string lit = "\"";
    for (char ch : *s) {
      if (ch == '"') lit += "\\\"";
      else lit += ch;
    }
    lit += "\"";
    return lit;
  }
  return resolveEScalar(name, session);
}

}

namespace piet::eval {
using namespace macro_detail;


// _E_SCALAR_KEY (display_hardening.py)
const std::vector<std::pair<std::string, std::string>>& eScalarKey() {
  static const std::vector<std::pair<std::string, std::string>> k = {
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
      // {eval/macros.cpp, display_hardening.cpp}. Keep them in step or
      // `display e(r2_w)` and `scalar q = e(r2_w)` disagree.
      {"r2_w", "r2_within"},
      // BUG-156 (wiring half, P2-PREP) — the rest of the names, so the Stata
      // spellings stop being unreachable the moment the engine emits the
      // keys. `ll` is already above; `ll_0`/`chi2` happen to map onto their
      // own cobra key names, which is why they need an entry at all (the
      // lookup below only rewrites names it FINDS in this table — an absent
      // name falls through as key==name, so these two are strictly
      // documentation; sigma_u/sigma_e/rho likewise). `r2_p` and `corr` are
      // the two that genuinely rename: poisson's pseudo-R2 and feols'
      // corr(u_i, Xb).
      {"ll_0", "ll_0"},     {"chi2", "chi2"},     {"r2_p", "r2_p"},
      // W1a-5d (2026-09-18): `r2_pseudo` is the SECOND name the same quantity
      // arrives under, and it was absent from this table, so it never reached
      // a STORED estimate's escalars -- `esttab <logit>, pr2` rendered a blank
      // cell. Measured on this engine: poisson publishes `r2_p` (.0126126) and
      // logit/probit publish `r2_pseudo` (.0321488 / .0345996), each leaving
      // the other missing, and nbreg publishes neither. `estimation_m4.cpp:430`
      // already emits it; only the travel was missing.
      {"r2_pseudo", "r2_pseudo"},
      {"sigma_u", "sigma_u"}, {"sigma_e", "sigma_e"}, {"rho", "rho"},
      {"corr", "corr_u_xb"},
  };
  return k;
}


// macros._resolve_est_scalar_refs — exposed (not file-local) for loops.cpp's
// if/while condition evaluator (M3a). Helpers above (resolveBRef,
// resolveEScalar, regexSubFn) stay file-local; only this entry point is
// part of the public macros.h surface.
std::string resolveEstScalarRefs(std::string expr, const EvalSession& session,
                                 std::string* err) {
  dprof::EScope _z(dprof::kEResolveEst);
  // AER scope-2 B2 (2026-09-08): `_b[eq:name]` -- Stata's equation-qualified
  // spelling (`_b[overall:difference]` after oaxaca, `_b[ATE:r1vs0.t]` after
  // teffects). The coeftable row NAME carries the same `eq:name` string, so the
  // lookup stays an exact match; only the reference grammar widens. The twin in
  // display_hardening.cpp (reBRef/reSeRef) MUST stay in step.
  // B2-4 (2026-09-09) widened the tail class by ONE character, `.`: the comment
  // above already named `_b[ATE:r1vs0.t]` as the target spelling, but `[\w:]`
  // has no dot, so the reference fell through to the undefined-matrix arm and
  // came back "matrix _b not found" (r(111)) -- an honest error, and the wrong
  // one. Stata's teffects colname carries the treatment VARIABLE after the
  // r#vs#, so the dot is structural, not decorative.
  // BUG-629 (eval half) widened the HEAD class by the digits and the tail by `#`:
  // a factor-level coefficient is named `2.c` (`0b.c` for the base, `1.c#c.x1`
  // for an interaction cell), so `local q = _b[2.c]` and `generate g = _b[2.c]`
  // fell through to "_b not found" / "unknown variable _b" while `test 2.c` and
  // `lincom _b[2.c]` answered (w2c_postest.log families (2)-(4)). The display
  // twin (display_hardening.cpp reBRef/reSeRef) carries the same classes.
  static const std::regex reB(R"(_b\[([A-Za-z_0-9/\x80-\xff][\w:.#\x80-\xff]*)\])");
  static const std::regex reSe(R"(_se\[([A-Za-z_0-9/\x80-\xff][\w:.#\x80-\xff]*)\])");
  static const std::regex reECall(R"(\be\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\))");
  // BUG-1518: a `_b[`/`_se[` that ends a longer name is that name's subscript,
  // not a coefficient: `g_b[1]` is observation 1 of the variable g_b (StataBE
  // 19.5: `assert g_b[1] == 1` and `generate z = g_b[1]*3` are rc 0). reB and
  // reSe have no left word boundary, so a match preceded by a name byte is left
  // as it is.
  auto endsName = [](const std::smatch& m) {
    return m.prefix().length() > 0 && isIdentChar(*(m.prefix().second - 1));
  };

  // A3: each of these four ran a full regex scan over EVERY expression, whether
  // or not the text could possibly contain its construct. A profile of a pure
  // macro-arithmetic loop (200k iterations, symbolized `sample`, 2026-08-06) put
  // ~18% of samples inside libc++'s regex engine and ~40% in malloc/free, most
  // of the latter being the regex engine's own per-execution state vectors —
  // so the cheapest scan is the one that never runs. A literal `find()` is a
  // memchr-class loop and is a strict pre-filter: every one of these patterns
  // requires its literal substring to appear, so no match can be missed.
  // (Deliberately NOT a single combined regex: the substitutions must stay
  // ordered and independent, and combining them would be a semantic change
  // dressed as an optimization.)
  if (expr.find(']') != std::string::npos) {
    // `[eq]_b[name]` / `[eq]_se[name]` are `_b[eq:name]` / `_se[eq:name]`
    // (BUG-1328: `[yb]_b[x1]` after logit), read before the `[eq]name` form,
    // which would take `_b` for the coefficient name.
    static const std::regex eqBRef(R"(\[([A-Za-z_\x80-\xff][\w\x80-\xff]*)\]_(b|se)\[([^\]]+)\])");
    expr = regexSubFn(eqBRef, expr, [&session, err](const std::smatch& m) {
      return valueRefText(resolveBRef(m.str(1) + ":" + m.str(3), m.str(2), session, err));
    });
    static const std::regex equationCoef(R"(\[([A-Za-z_\x80-\xff][\w\x80-\xff]*)\]([A-Za-z_0-9\x80-\xff][\w.#\x80-\xff]*))");
    expr = regexSubFn(equationCoef, expr, [&session, err](const std::smatch& m) {
      return valueRefText(resolveBRef(m.str(1) + ":" + m.str(2), "b", session, err));
    });
  }
  // BUG-1545: an ancillary parameter's name can carry parentheses (`_b[/var(e.yc)]`
  // after tobit, StataBE 19.5 1.5244086035135405), which reB's tail class does not
  // take; left as text, the row layer then bound `e` to the variable `eps`
  // (expandRowVarAbbrevs) and the engine refused `_b`. Read before reB.
  if (expr.find("[/") != std::string::npos) {
    static const std::regex reAnc(R"(_(b|se)\[(/[A-Za-z_]\w*\([^()\[\]]*\))\])");
    expr = regexSubFn(reAnc, expr, [&session, err](const std::smatch& m) {
      return valueRefText(resolveBRef(m.str(2), m.str(1), session, err));
    });
  }
  if (expr.find("_b[") != std::string::npos) {
    expr = regexSubFn(reB, expr, [&session, err, &endsName](const std::smatch& m) {
      if (endsName(m)) return m.str(0);
      return valueRefText(resolveBRef(m.str(1), "b", session, err));  // FU-842
    });
  }
  if (expr.find("_se[") != std::string::npos) {
    expr = regexSubFn(reSe, expr, [&session, err, &endsName](const std::smatch& m) {
      if (endsName(m)) return m.str(0);
      return valueRefText(resolveBRef(m.str(1), "se", session, err));  // FU-842
    });
  }
  // scalar(NAME) function form -> the stored scalar value (or the literal
  // token unchanged when undefined).
  if (expr.find("scalar(") != std::string::npos) {
    expr = substituteScalarCalls(expr, session);  // EXPRLAYER-5: was reScalarCall
  }
  // e(name) scalar — NEVER a matrix. The exclusion used to be the literal pair
  // b/V, which was complete while those were the only e-matrices that existed.
  // Once pca posts e(Ev)/e(L)/e(C)/e(Psi)/e(means)/e(sds)/e(components), the
  // literal list silently under-covers: this pass would resolve e(Ev) to
  // MISSING before the matrix pass ever saw it, so `e(Ev)[1,2]` evaluated as
  // `.[1,2]` and `colsof(e(Ev))` as `colsof(.)`. Asking the store instead of a
  // hardcoded pair keeps the same rule ("a matrix is not a scalar") true for
  // every matrix, including ones added later.
  if (hasBareEParen(expr)) {  // EXPRLAYER-3: reECall is \be\( — the \b is the guard
    const std::string whole = expr;
    expr = regexSubFn(reECall, expr, [&session, err, &whole](const std::smatch& m) {
      std::string inner = m.str(1);
      if (inner == "b" || inner == "V") {
        // eb-order T-P1: substituteMatrixRefs ran first and answered el()/
        // colsof()/rowsof() of an e(b)/e(V) that resolves, so one still here
        // is absent. Stata then reads e(b) as an e() macro, and the function
        // gets a string: r(109) "type mismatch", with or without a live e()
        // (w2v_estpost_ematrix.log ep13_el_eb_11 / ep13_colsof_eb after estpost,
        // ep15c_* after pca, ep15f_* with no e()). An e(V) the engine did not
        // send keeps its own named refusal.
        if (err != nullptr && err->empty() && absentClassMatrixRc(whole, 'e', inner) == "109") {
          const EvalSession::EMatrixAnswer a =
              session.resolveEMatrixFor(inner, EvalSession::EReader::kOperand);
          if (!a.matrix.has_value())
            *err = !a.absent ? "r(" + std::to_string(a.rc) + "): " + a.why
                             : "r(109): type mismatch: e(" + inner + ") is not a matrix here";
        }
        return m.str(0);
      }
      if (session.findClassMatrix('e', inner) != nullptr) return m.str(0);
      // W2 predictframe (2026-09-19): e(sample) is Stata's per-ROW estimation
      // sample mask, not a scalar, and this build keeps no such mask. It used to
      // fall through to resolveERef as an ABSENT name -> `.`, and a missing
      // condition is TRUE in every row plane, so after `regress y x2 t if id < 4`
      // (9 in-sample rows) `predict p if e(sample)` filled all 12 at rc 0, and
      // `count`/`gen`/`summarize`/`regress`/`keep if e(sample)` all answered
      // over 12 (measured on the frozen main binary, 2026-09-19; Stata: 9 --
      // esttab-beta.log:488 `predict ... if e(sample)` is the served shape).
      // Refused BY NAME on the same channel as r(111)/r(109) below, so every
      // caller maps it the same way; with no channel the token is left as text
      // for the evaluator to refuse in its own words -- never a silent `.`.
      //
      // e(sample) T2: the mask now exists (SessionState::esampleReading), and
      // every ROW caller rewrites the token before it gets here
      // (dispatcher.cpp rewriteEsampleToken: `(__piet_esample__==SERIAL)` or `0`).
      // What still arrives is a SCALAR context -- display, `local =`, `scalar
      // =`, `if e(sample) {` -- where Stata reads a variable at observation 1
      // (w2_esample.log B16-B19, B29-B29c; 0 with no estimation results,
      // A2-A4). The hook (EvalSession::esampleObs1) answers that value, or the
      // named r(198) for a model PIET has no mask for; with no hook (a
      // session-less harness) it is the same refusal.
      if (inner == "sample") {
        // The session decides (SessionState::esampleReading: no model or a
        // posted e() -> 0, a live mask -> observation 1, a model PIET has no
        // mask for -> the named r(198)). Without a session (a bare evaluator)
        // this layer's own absent-e() predicate, resolveEScalar's, says 0 for
        // "no estimation results" and anything else is refused by name.
        std::string why;
        if (session.esampleObs1) {
          if (std::optional<int> v = session.esampleObs1(&why)) return std::to_string(*v);
        } else if (!session.lastModelResult) {
          return std::string("0");
        } else {
          why = "e(sample) is not available in this version of PIET here: no data session is "
                "attached to evaluate the estimation sample.";
        }
        if (err != nullptr && err->empty()) *err = "r(198): " + why;
        return m.str(0);
      }
      // BUG-456: matrix (above) -> string -> scalar. The string arm is what
      // `local c = e(cmd)`, `if e(cmd)=="regress" {`, `assert e(cmd)=="regress"`
      // and `gen z = (e(cmd)=="regress")` all lacked; every one of them saw `.`.
      return valueRefText(resolveERef(inner, session));  // FU-842
    });
  }
  // BUG-456 follow-up: a NUMBER that has landed in a STRING-typed argument slot
  // is r(109) "type mismatch" (see stringFnNumericArgImpl's measurement block).
  // Reported on the SAME `err` channel as the r(111) above, distinguished by an
  // explicit `r(NNN): ` prefix that splitErrCode() hands the callers -- an
  // r(111) already found wins, it was raised earlier in the expression.
  if (err != nullptr && err->empty()) {
    std::string call;
    if (stringFnNumericArgImpl(expr, &call)) *err = stringSlotMismatchError(call);
  }
  return expr;
}


std::string fmtNumeric(double v) {
  if (std::isnan(v)) return ".";
  if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  return std::string(buf);
}


// ---------------------------------------------------------------------------
// capture_r_from_block

void captureRFromBlock(const nlohmann::ordered_json& block, EvalSession& session) {
  const std::string blockType = block.value("type", "");

  // --- Branch 1: summarize -> r() from the LAST variable's stats ---
  if (blockType == "summarize") {
    if (!block.contains("stats") || !block.at("stats").is_object() || block.at("stats").empty()) {
      return;
    }
    const auto& stats = block.at("stats");
    // Stata stores r() from the last variable in a multi-var summarize —
    // last key in dict order (ordered_json preserves document order).
    std::string lastVar;
    for (const auto& [k, v] : stats.items()) lastVar = k;
    const auto& varStats = stats.at(lastVar);

    static const std::vector<std::pair<std::string, std::string>> kSummarizeRMap = {
        {"N", "N"},           {"mean", "mean"},   {"sd", "sd"},   {"min", "min"},
        {"max", "max"},       {"sum", "sum"},     {"sum_w", "sum_w"}, {"Var", "Var"},
        {"p1", "p1"},         {"p5", "p5"},       {"p10", "p10"}, {"p25", "p25"},
        {"p50", "p50"},       {"p75", "p75"},     {"p90", "p90"}, {"p95", "p95"},
        {"p99", "p99"},       {"skewness", "skewness"}, {"kurtosis", "kurtosis"},
    };

    OrderedFloatDict newR;
    for (const auto& [statKey, rName] : kSummarizeRMap) {
      if (varStats.contains(statKey)) {
        auto f = jsonToFloat(varStats.at(statKey));
        if (f) newR.set(rName, *f);  // skip non-numeric (TypeError/ValueError)
      }
    }
    // Var = sd^2 when sd present but Var not already in stats.
    if (newR.contains("sd") && !newR.contains("Var")) {
      double sd = *newR.find("sd");
      newR.set("Var", sd * sd);
    }
    // No r(median): Stata's summarize posts r(p50) only, and a read of r(median)
    // after `summarize, detail` is `.` (FU-268, BUG-1013; w2o_rnames N16,
    // w2as_summary_stats_family sx_sum_d_u). The p50 alias this branch used to
    // post answered 5.25 there.
    session.rValues = newR;  // replace entire r() store (Stata semantics)
    return;
  }

  // --- Branch 3: test -> r-class Wald result (W:P5) ---
  // `test` is r-class in Stata (r(F), r(p), r(df), r(df_r), r(chi2), r(drop),
  // r(dropped)) but this codebase only ever wired summarize and lincom/nlcom,
  // so `test 1.treated` followed by `display r(p)` errored with "r(p) is not
  // set". The engine's test block already carries every value under its own
  // key names (F/p_F/df1/df2, plus chi2/p_chi2 on the chi-square form) — this
  // maps them onto Stata's r() spellings. Keep the lincom branch below
  // untouched: the two estimators publish different shapes.
  if (blockType == "estimation" && block.value("estimator", "") == "test") {
    // BUG-205(3), CORRECTED at the p5-wave regression (2026-08-08): the first
    // version mapped `p_chi2 -> p` unconditionally, on the stated premise that
    // "only one of the two source keys is ever present on a given block".
    // MEASURED FALSE: `test 1.treated` after `regress ..., vce(cluster id)`
    // emits BOTH (p_F=6.277e-26, p_chi2=9.705e-40), and the chi2 mapping ran
    // last, so r(p) silently became the chi2 tail on an F-form test — caught
    // by the p5 answerbook (want = Stata's F-based 6.277e-26). Stata's rule:
    // the DISPLAYED form owns r() — F-form sets {F, df, df_r, p}; the ML/chi2
    // form (no F on the block) sets {chi2, df, p}. The chi2 keys are now a
    // FALLBACK, applied only when the block carries no F.
    OrderedFloatDict newR;
    // The least-squares form carries F and p_F (both null when R V R' is exactly
    // zero: an exact `regress` fit, where Stata prints F(1, 48) = . and posts
    // r(F) = r(p) = missing at rc 0). A null there is Stata's missing value, so it
    // is posted as `.`; the chi2 form (no p_F key) keeps its present-only map.
    const bool fZero = block.contains("p_F") && block.at("p_F").is_null() &&
                       block.contains("F") && block.at("F").is_null();
    const bool fForm = block.contains("p_F") && (!block.at("p_F").is_null() || fZero);
    static const std::vector<std::pair<std::string, std::string>> kFMap = {
        {"F", "F"}, {"p_F", "p"}, {"df1", "df"}, {"df2", "df_r"}};
    static const std::vector<std::pair<std::string, std::string>> kChi2Map = {
        {"chi2", "chi2"}, {"p_chi2", "p"}, {"df1", "df"}};
    // BUG-1276: with every constraint dropped (df1 0) the chi2 form's statistic and
    // p are missing too, and Stata still posts them (`test x1` after `logit y x1 xs,
    // asis`: r(chi2) = r(p) = ., r(df) 0, r(dropped_1) 1).
    const bool allDropped = block.contains("dropped_rows") && block.contains("df1") &&
                            block.at("df1").is_number() && block.at("df1").get<double>() == 0;
    for (const auto& [src, dst] : (fForm ? kFMap : kChi2Map)) {
      if (block.contains(src) && !block.at(src).is_null()) {
        auto f = jsonToFloat(block.at(src));
        if (f) newR.set(dst, *f);
      } else if ((fZero && (src == "F" || src == "p_F")) ||
                 (allDropped && (src == "chi2" || src == "p_chi2"))) {
        newR.set(dst, std::nan(""));
      }
    }
    // r(dropped_#): the number of each constraint the engine dropped, in order.
    if (block.contains("dropped_rows") && block.at("dropped_rows").is_array()) {
      int k = 0;
      for (const auto& row : block.at("dropped_rows"))
        if (row.is_number()) newR.set("dropped_" + std::to_string(++k), row.get<double>());
    }
    // A malformed/empty test block must leave r() alone rather than clearing
    // it — F is the one key every shape of this block carries.
    if (!newR.contains("F") && !newR.contains("chi2")) return;
    session.rValues = newR;
    return;
  }

  // --- Branch 2: lincom / nlcom -> r-class post-estimation (WS-E) ---
  if (blockType == "estimation") {
    const std::string estimator = block.value("estimator", "");
    if (estimator != "lincom" && estimator != "nlcom") return;
    if (!block.contains("coeftable") || !block.at("coeftable").is_array() ||
        block.at("coeftable").empty()) {
      return;
    }
    const auto& row = block.at("coeftable").at(0);
    static const std::vector<std::pair<std::string, std::string>> kMap = {
        {"b", "estimate"}, {"se", "se"}, {"t", "t"}, {"p", "p"}, {"ci_lo", "lb"}, {"ci_hi", "ub"},
    };
    OrderedFloatDict newR;
    // A zero-variance combination (an exact `regress` fit): the engine posts se 0
    // with t, p and the interval null, and Stata's lincom posts r(se) 0 with r(t),
    // r(p), r(lb), r(ub) missing (StataBE `lincom x` after `regress yx x`).
    const bool zeroSe = row.contains("se") && row.at("se").is_number() &&
                        row.at("se").get<double>() == 0.0;
    for (const auto& [src, dst] : kMap) {
      if (row.contains(src) && !row.at(src).is_null()) {
        auto f = jsonToFloat(row.at(src));
        if (f) newR.set(dst, *f);
      } else if (zeroSe && row.contains(src) && src != "b" && src != "se") {
        newR.set(dst, std::nan(""));
      }
    }
    if (block.contains("df") && !block.at("df").is_null()) {
      auto f = jsonToFloat(block.at("df"));
      if (f) newR.set("df", *f);
    }
    session.rValues = newR;
    return;
  }

  // All other block types: do not touch r().
}

}
