// display_hardening.cpp — C++ port of piet/backend/piet/display_hardening.py
// (M3a; the frozen Python file is the spec — see the header for design
// notes on why this module's helpers are NOT shared with
// piet/eval/macros.cpp's similarly-named anonymous-namespace helpers).
#include "piet/display_hardening.h"

#include "piet/bcal.h"  // AER scope-2 B2-3: %tbNAME rendering

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
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

namespace piet {

namespace {

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
      // {eval/macros.cpp, display_hardening.cpp}. Keep them in step or
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
  static const std::regex r(R"(_b\[([A-Za-z_][A-Za-z0-9_:.]*)\])");
  return r;
}
const std::regex& reSeRef() {
  static const std::regex r(R"(_se\[([A-Za-z_][A-Za-z0-9_:.]*)\])");
  return r;
}
// _RE_E_REF — e(name) reference, applied with fullmatch semantics in Python
// (re.match anchored via ^...$ in the pattern itself).
const std::regex& reERef() {
  static const std::regex r(R"(^e\(\s*(.+?)\s*\)$)");
  return r;
}
// identifier-shaped token check (step 7 of _resolve_token)
const std::regex& reIdent() {
  static const std::regex r(R"(^[A-Za-z_]\w*$)");
  return r;
}
// _RE_C_REF — c(name) system-value reference (BUG-091). Whole-token match on a
// single identifier inside c(...); no nesting, matching Stata's c() namespace.
const std::regex& reCRef() {
  static const std::regex r(R"(^c\(\s*([A-Za-z_]\w*)\s*\)$)");
  return r;
}

// _RE_REQUEST_CALL — BUG-087/D9 (BETA Phase F, D9 locked decision):
// `_request(macroName)` call-syntax token. Deliberately NOT part of
// piet::eval::tryExtendedMacroFn()'s colon-form dispatch
// (`local NAME : func ...`, macros.cpp) — real Stata's `_request()` is
// call-syntax invoked from inside `display`, exactly the shape BUG-088
// traced (this file's resolveToken() falling through -> eval::evalArith()
// -> pyeval's NameError). Intercepted in flushUnquoted() below (NOT inside
// resolveToken()) for two reasons: (a) resolveToken() has no channel to
// report a genuine error back to its caller — BUG-088's fix added that
// channel to evalArith(), one layer up, and duplicating it into
// resolveToken() too would be a second, parallel error path; (b) the
// modal's "prompt" text is the PRECEDING display text on this same
// statement (BUG-087's own repro: `display as text "Include additional
// controls? ..." _request(controls_input)` — the string literal before the
// call IS the prompt a real Stata user sees) — only flushUnquoted() has
// that (via `parts`); resolveToken() only ever sees one bare token in
// isolation.
const std::regex& reRequestCall() {
  static const std::regex r(R"(^_request\(\s*([A-Za-z_]\w*)\s*\)$)");
  return r;
}

// PIET_REQUEST_<NAME> env var name for the headless fallback (macroName
// uppercased, any non-alnum char -> '_') — exact spec BUG-087's brief gives.
std::string requestEnvVarName(const std::string& macroName) {
  std::string out = "PIET_REQUEST_";
  for (char c : macroName) {
    unsigned char uc = static_cast<unsigned char>(c);
    out += std::isalnum(uc) ? static_cast<char>(std::toupper(uc)) : '_';
  }
  return out;
}

// _request(macroName) — BUG-087/D9: blocks on a real modal round-trip
// (windowed, on the do-executor worker thread) or reads
// PIET_REQUEST_<NAME>/PIET_REQUEST_DEFAULT (headless: --do/--selftest/
// --run-block/--evalcase never construct a Shell at all, so
// session.hasRequestPostEvent() is false there), sets the GLOBAL macro
// $macroName, and returns "" — `_request()` itself contributes no further
// display text (matches real Stata: the visible prompt is whatever was
// already displayed BEFORE the call blocks, not a "return value" from the
// call). On failure (cancelled run, wrong thread, or headless with no
// matching env var) sets *error and returns nullopt; the caller
// (flushUnquoted) turns that into an honest engine_error — same BUG-088
// shape, never a hang, never a silent literal-text fallback.
std::optional<std::string> resolveRequestCall(const std::string& macroName,
                                               const std::string& promptSoFar,
                                               SessionState& session, std::string* error) {
  if (session.hasRequestPostEvent()) {
    // A single-line `display _request(x)` submitted via #cmd-input goes
    // through the SYNCHRONOUS "command" bindRoute, which runs ON the UI
    // thread (shell.h's threading note) — blocking there would deadlock the
    // whole app (postEvent()'s queued JS injection, and the user's eventual
    // click, both need that same thread's run loop pumping). Refuse
    // honestly instead; see session_state.h's isCurrentThreadADoWorker()
    // doc comment for the full rationale.
    if (!piet::isCurrentThreadADoWorker()) {
      if (error) {
        *error =
            "_request(" + macroName +
            "): interactive input is only supported while running a full .do file (Run .do / "
            "the do-file editor) — it cannot be used from the single command line.";
      }
      return std::nullopt;
    }
    auto answer = session.requestUserInput(macroName, promptSoFar);
    if (!answer.has_value()) {
      if (error) {
        *error = "_request(" + macroName +
                 ") was cancelled — the run stopped before an answer arrived. Global macro $" +
                 macroName + " was not set.";
      }
      return std::nullopt;
    }
    session.eval.globals.set(macroName, *answer);
    return std::string();
  }

  // Headless: NEVER block waiting for a frontend that doesn't exist.
  std::string envName = requestEnvVarName(macroName);
  const char* v = std::getenv(envName.c_str());
  if (!v) v = std::getenv("PIET_REQUEST_DEFAULT");
  if (v != nullptr) {
    session.eval.globals.set(macroName, std::string(v));
    return std::string();
  }
  if (error) {
    *error = "_request(" + macroName +
             "): interactive input is unsupported in a headless run. Set the environment "
             "variable " +
             envName +
             " (or PIET_REQUEST_DEFAULT) before running, or set the macro yourself first: "
             "global " +
             macroName + " <value>.";
  }
  return std::nullopt;
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

// _fmt_numeric — file-local, Stata %9.0g-style display formatting. NOTE:
// this happens to be numerically identical to piet::eval::fmtNumeric
// (verified against both Python sources: same .9g / "." / "+Inf" / "-Inf"
// rule) but is duplicated here per the module-isolation rationale in the
// header — this is display_hardening.py's OWN local helper, not
// macros.fmtNumeric, and the two must not be conflated even though they
// currently agree.
std::string fmtNumeric(double v) {
  if (std::isnan(v)) return ".";
  if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  return std::string(buf);
}

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

// W:P5 — Stata's DEFAULT display format for a numeric expression, `%9.0g`.
//
// `display <numeric>` in Stata renders into nine characters: the most
// significant digits that fit, in fixed notation when possible and
// e-notation when not, with the leading zero of a |x|<1 magnitude dropped.
// Measured against the answerbook's own Stata 19.5 log:
//   2.157789219986937   -> "2.1577892"   (9 chars)
//   0.5926470951388326  -> ".5926471"    (9 chars formatted, leading 0 cut)
//   6.277415369414958e-26 -> "6.277e-26" (9 chars — only 4 significant
//                                         digits survive once "e-26" is paid)
// PIET printed full `repr` precision instead, so a pin whose magnitude forces
// Stata into short e-notation compared RED at rtol 1e-6 even though the
// underlying value was exact to fifteen digits. The number was never wrong;
// the rendering was.
//
// Integers keep their plain form (Stata prints `5`, not `5.0000000`), and a
// value needing more than nine characters even at one significant digit
// (only ±inf-adjacent magnitudes) falls back to %.9g rather than truncating
// to something unreadable.
std::string fmtStataDefaultGImpl(double v) {
  if (std::isnan(v)) return ".";
  if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
  char buf[64];
  // Widths are counted the way Stata counts them: on the string INCLUDING a
  // leading "0." — which is then stripped — and excluding a minus sign.
  const int kWidth = 9;
  for (int prec = 8; prec >= 1; --prec) {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
    std::string s(buf);
    const size_t signLen = (!s.empty() && s[0] == '-') ? 1 : 0;
    if (s.size() - signLen <= static_cast<size_t>(kWidth)) {
      // Stata writes ".5" where C writes "0.5".
      if (s.compare(signLen, 2, "0.") == 0) s.erase(signLen, 1);
      return s;
    }
  }
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  return std::string(buf);
}

// _apply_stata_fmt (router/macros.py) — Python signature is
// (fmt: str, value_str: str) -> str. The C++ piet::eval::applyStataFmt has
// argument order (value, fmt) AND different fallback behavior (falls back
// to gFormat() and supports -/0 flags + a prefix-match regex) — it is a
// DIFFERENT function serving exprfns.string(), not this one. This adapter
// re-implements router/macros.py's _apply_stata_fmt faithfully, PLUS the
// BUG-185 width fix on top:
//   - regex is a FULL match: ^%(-)?(\d+)?\.(\d+)([fgeEFG])$ (no 0 flag, no
//     trailing garbage tolerated)
//   - BUG-185: width (group 2) used to be parsed but UNUSED (a dead
//     variable inherited from the Python port). Stata right-justifies to
//     the field width by default and left-justifies when the spec starts
//     with '-'; a value already wider than the field is never truncated
//     (printf semantics) — that's exactly what the padding step below does.
//   - ftype in {f} -> fixed; {g,e} -> %.<prec>g / %.<prec>e (case folded
//     via .lower(), so %9.2E and %9.2e behave the same, matching Python's
//     `ftype = m.group(3).lower()`)
//   - on regex-non-match OR float(value_str) failure -> return value_str
//     UNCHANGED (no gFormat fallback, unlike the exprfns.h sibling) — this
//     preserves every pre-existing non-dash case byte-identical (%9s,
//     %td-style time tokens, and any other letter this function never
//     understood keep silently passing the raw value through untouched).
//   - BUG-185: a '-'-prefixed spec is BRAND NEW surface (the tokenizer used
//     to never route it here at all — see the isFmtTok gate below — so it
//     fell through and got echoed as literal source text instead of being
//     applied). A malformed one now reports an honest error via `err`
//     rather than resurrecting that echo-as-literal failure mode. This
//     branch is unreachable for any non-dash spec Stata accepts that this
//     code already handled, since only fmt[1]=='-' can hit it.
// ---------------------------------------------------------------------------
// BUG-192 / BUG-195: Stata's `%w.Pg` is NOT printf's `%.Pg`.
//
// `display %9.0g 42` printed `4e+01`, because the spec was handed to printf and
// printf reads `%.0g` as "zero significant digits". Stata's `g` is a GENERAL
// format: `P` bounds the significant digits, `w` bounds the field, and a value
// that fits shows as itself. `%9.0g 42` is `       42`.
//
// The renderer that implements that rule USED TO LIVE HERE. It now lives in
// piet/stata_format.h and is shared with eval/exprfns.cpp's applyStataFmt, the
// string()/tostring layer, which carried an independent printf-based twin of
// the same format with the same defect (BUG-195 — and that twin renders into
// STORED data, not pixels). Two hand-written twins of Stata's `g` rule is how
// they drift, so there is one renderer now. The full rule, its seven clauses,
// the `%w.0e` rider and the oracle that forced each of them are documented on
// that header; this file's behaviour is unchanged, byte for byte.
//
// The oracle is on disk: tests/kitchensink/fmt_oracle/fmt_g_oracle.{do,log},
// 513 StataBE 19.5 cases over all three surfaces. BUG-192's original capture
// was lost, which is why the rule spent a release with no on-disk authority
// behind it.
//
// What stays here rather than moving to the shared header is everything that
// is specific to formatting an ALREADY-STRINGIFIED value for `display`: this
// layer's own full-match spec grammar (so `%td`/`%9s` fall through untouched),
// the parse-failure and float()-failure fallbacks, the missing-token rider, and
// the right-justification into the field. string()/tostring share the rendering
// and none of those — measured, see padToField()'s comment.

std::string applyStataFmtLocal(const std::string& fmt, const std::string& valueStr,
                                std::string* err = nullptr) {
  // BUG-293(a): the bare date/time families. `display %td (20000)` printed the
  // raw 20000 because this layer's full-match grammar refuses every %t spec, so
  // the token never reached a renderer at all. Stata (19.5 IC, measured) prints
  // `04oct2014` and `%tc (1234567890000)` -> `13feb1999 23:31:30`. Same arm, same
  // renderers and the same "bare tc/td only" boundary as eval/exprfns.cpp's
  // string()/tostring half — the detail-coded and other-calendar forms stay a
  // named gap (BUG-296) rather than being mapped to a plausible wrong answer.
  // BUG-296 extends this arm past tc/td: the other %t calendars and a
  // detail-coded %td. One decode, shared with eval/exprfns.cpp's string()/
  // tostring half — the same renderers, the same named gap at %ty.
  {
    // AER scope-2 B2-3: `%tbNAME[:detail]`. A business-day INDEX inside the
    // calendar prints its calendar date; outside the calendar it prints the
    // raw number, and a calendar that cannot be loaded falls back to the
    // plain number -- Stata's own %g fallback ([D] Datetime business
    // calendars, item 5). Measured: `%tbNAME` of -1/99 outside the range
    // prints `-1`/`99`, of a missing value prints `.`.
    static const std::regex reTb(R"(^%-?\d{0,2}tb([A-Za-z_][A-Za-z0-9_]*)(?::(.*))?$)");
    std::smatch bm;
    if (std::regex_match(fmt, bm, reTb)) {
      const std::string detail = bm.size() > 2 ? bm[2].str() : std::string();
      const int fw = piet::fmt::tFamilyWidth('d', detail);
      // EVERY exit from this branch is RIGHT-ALIGNED in the family width, the
      // fallbacks included — measured this review (StataBE 19.5,
      // /private/tmp/fix-bcal/st/cap4.log): with `dcal` loaded,
      //   %tbdcal 3   `04jan2020`  ·  %tbdcal -1  `       -1`
      //   %tbdcal 99  `       99`  ·  %tbdcal .   `        .`
      // and with NO such calendar at all, %tbnosuchcl 3 is `        3` and
      // %tbnosuchcl -1 is `       -1`. The values were already right (the
      // review's non-blocking 4 says so); only the width was missing, so the
      // padding is hoisted out of the success path and applied to all of them.
      auto pad = [fw](const std::string& t) {
        if (fw > static_cast<int>(t.size()))
          return std::string(static_cast<size_t>(fw) - t.size(), ' ') + t;
        return t;
      };
      // The calendar-NAME rule on the third surface too (review 2026-09-09).
      // Behaviour-NEUTRAL on the VALUE and deliberately so: bcal::lookup()
      // already returns null for a name the rule refuses, so an over-long name
      // fell back to the raw number either way. It is here so the rule has ONE
      // predicate on every plane that reads a %tb name — the `format` plane is
      // where the missing check was an actual leniency divergence.
      if (!bcal::isValidCalName(bm[1].str())) return pad(valueStr);
      auto bv = eval::pythonFloat(valueStr);
      if (!bv) return pad(valueStr);
      auto cal = bcal::lookup(bm[1].str());
      if (!cal) return pad(valueStr);
      const double day = bcal::dofb(*cal, *bv);
      if (std::isnan(day)) return pad(valueStr);
      std::optional<std::string> r =
          detail.empty() ? piet::fmt::renderTd(day) : piet::fmt::renderTdDetail(day, detail);
      if (!r) return pad(valueStr);
      return pad(*r);
    }
  }
  {
    static const std::regex reT(R"(^%-?t([cCdqhmwy])(.*)$)");
    std::smatch tm;
    if (std::regex_match(fmt, tm, reT)) {
      auto tv = eval::pythonFloat(valueStr);
      if (!tv) return valueStr;
      const char fam = tm[1].str()[0];
      const std::string detail = tm[2].str();
      std::optional<std::string> r;
      if (detail.empty()) {
        switch (fam) {
          case 'c': r = piet::fmt::renderTc(*tv); break;
          case 'd': r = piet::fmt::renderTd(*tv); break;
          case 'q': r = piet::fmt::renderTq(*tv); break;
          case 'h': r = piet::fmt::renderTh(*tv); break;
          case 'm': r = piet::fmt::renderTm(*tv); break;
          case 'w': r = piet::fmt::renderTw(*tv); break;
          case 'y': r = piet::fmt::renderTy(*tv); break;
          default: break;  // %tC — named gap
        }
      } else if (fam == 'd') {
        r = piet::fmt::renderTdDetail(*tv, detail);
      }
      if (r) {
        // BUG-296: right-justify into the family's own field (see
        // piet::fmt::tFamilyWidth — the width is a property of the SPEC, not of
        // this value). string()/tostring do not pad; only display does.
        const int fw = piet::fmt::tFamilyWidth(fam, detail);
        if (fw > static_cast<int>(r->size())) {
          return std::string(static_cast<size_t>(fw) - r->size(), ' ') + *r;
        }
        return *r;
      }
      return valueStr;
    }
  }
  // BUG-293(a): the `0` flag. The spec grammar had no group for it, so
  // `%09.2f` failed the full match and fell through unformatted — PIET printed
  // `    42.50` where Stata prints `000042.50`. StataFmtSpec::zeroFill already
  // exists and is already honored by the shared renderer (BUG-248 wired the
  // string()/tostring surface); only display's own grammar never spoke it.
  // BUG-296: `,` decimal separator and the `c` grouping suffix, the same two
  // axes the string()/tostring grammar gained.
  static const std::regex re(R"(^%(-)?(0)?(\d+)?([.,])(\d+)([fgeEFG])(c)?$)");
  std::smatch m;
  if (!std::regex_match(fmt, m, re)) {
    if (err && fmt.size() >= 2 && fmt[1] == '-') *err = "invalid display format " + fmt;
    return valueStr;
  }
  piet::fmt::StataFmtSpec sp;
  sp.leftJustify = m[1].matched;
  sp.zeroFill = m[2].matched;
  sp.hasWidth = m[3].matched;
  if (sp.hasWidth) {
    try {
      sp.width = std::stoi(m[3].str());
    } catch (...) {
      sp.hasWidth = false;
    }
  }
  sp.commaDecimal = m[4].str() == ",";
  try {
    sp.prec = std::stoi(m[5].str());
  } catch (...) {
    return valueStr;
  }
  sp.conv = m[6].str()[0];
  sp.groupThousands = m[7].matched;
  // .lower(), matching the Python port's `ftype = m.group(3).lower()` — %9.2E
  // and %9.2e behave the same.
  if (sp.conv >= 'A' && sp.conv <= 'Z') sp.conv = static_cast<char>(sp.conv - 'A' + 'a');

  // BUG-192 rider (same oracle capture): a Stata missing value IS a value in
  // a numeric field and Stata right-justifies it like any other — `%9.0g .`
  // is eight blanks then a dot. It used to fall out of pythonFloat() as
  // unparseable and return the bare token with no padding at all, so a table
  // column containing one lost its alignment. `.a`-`.z` (the extended missing
  // codes) take the same field. DISPLAY-ONLY: the oracle's section B has
  // `string(., "%9.0g")` as a bare `.` with no field at all.
  {
    const std::string t = strutil::trim(valueStr);
    if (piet::fmt::isMissingToken(t)) return piet::fmt::padToField(t, sp);
  }

  auto v = eval::pythonFloat(valueStr);
  if (!v) return valueStr;  // float(value_str) raised -> except -> fall through

  // The one renderer (piet/stata_format.h). nullopt is the printf-overflow and
  // unreachable-conversion case, where both this function and its Python
  // ancestor return the value untouched.
  auto rendered = piet::fmt::renderValue(*v, sp);
  if (!rendered) return valueStr;
  // BUG-296: when an `f` spec OVERFLOWED into scientific, the field it lands in
  // is at least 8 — measured, `%6.2f 1234567.89` displays ` 1.2e+06` (8) where
  // the value itself is 7, while `%9.2f`/`%12.2f` use their own wider fields.
  // Scoped to the overflow arm: the ordinary fixed and general renderings keep
  // their own field exactly as before.
  if (sp.conv == 'f' && sp.width < 8 &&
      rendered->find('e') != std::string::npos) {
    piet::fmt::StataFmtSpec wide = sp;
    wide.hasWidth = true;
    wide.width = 8;
    return piet::fmt::padToField(*rendered, wide);
  }
  // BUG-185: pad to field width; printf semantics — never truncate a value
  // already wider than the field.
  return piet::fmt::padToField(*rendered, sp);
}

// ---------------------------------------------------------------------------
// BUG-456 (2026-09-08): the e-MACRO arm of the display twin. TWIN of
// macros.cpp's resolveERef -- same rule, same literal shape, and the same
// "always a string, never a number" ruling measured on StataNow 19.5 BE
// (plans/bug456-20260908/refute3_capture.log, 40/40 macro shapes). Returns the
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
std::string resolveEScalar(const std::string& name, SessionState& session) {
  // BUG-421: this twin reads SessionState::lastModel()->result; a posted
  // e-scalar (pca's e(rho), `estadd scalar`) lives in session.eval.eScalars and
  // reached neither. Checked LAST everywhere, so a fitted key always wins.
  const auto posted = [&]() -> std::string {
    if (const double* v = session.eval.eScalars.find(name)) return fmtNumeric(*v);
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
  return fmtNumeric(*val);
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
std::string resolveBRef(const std::string& varname, const std::string& kind,
                         SessionState& session, std::string* err = nullptr) {
  const auto& lm = session.lastModel();
  if (!lm.has_value()) {
    // Stata: r(301) "last estimates not found".
    if (err) *err = "last estimates not found";
    return ".";
  }

  // BUG-158: intercept alias — see the twin comment on macros.cpp's
  // resolveBRef. These two copies must stay in step.
  std::vector<std::string> tryNames{varname};
  if (varname == "_cons") tryNames.push_back("(intercept)");
  else if (varname == "(intercept)") tryNames.push_back("_cons");
  if (lm->result.has_value()) {
    const auto& result = *lm->result;
    if (result.is_object() && result.contains("coeftable") && result.at("coeftable").is_array()) {
      for (const auto& want : tryNames) {
        for (const auto& row : result.at("coeftable")) {
          if (row.is_object() && row.contains("name") && row.at("name").is_string() &&
              row.at("name").get<std::string>() == want) {
            const char* field = (kind == "b") ? "b" : "se";
            // Found the coefficient; a missing VALUE here is a real `.`.
            if (!row.contains(field) || row.at(field).is_null()) return ".";
            auto val = jsonToFloat(row.at(field));
            if (!val) return ".";
            return fmtNumeric(*val);
          }
        }
      }
    }
  }
  // Name not in the model — BUG-162.
  if (err) *err = varname + " not found";
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

// _resolve_token
std::string resolveToken(const std::string& token, SessionState& session,
                          std::string* err = nullptr) {
  std::smatch m;

  // 1. _b[var]
  if (std::regex_match(token, m, reBRef())) {
    return resolveBRef(m[1].str(), "b", session, err);
  }
  // 2. _se[var]
  if (std::regex_match(token, m, reSeRef())) {
    return resolveBRef(m[1].str(), "se", session, err);
  }
  // 3. e(name) — collapse redundant double-wrapping e(e(r2)).
  if (std::regex_match(token, m, reERef())) {
    std::string inner = m[1].str();
    while (true) {
      std::smatch mm;
      if (!std::regex_match(inner, mm, reERef())) break;
      inner = mm[1].str();
    }
    // BUG-456: string -> scalar (the e-matrix names never reach display's
    // token resolver as a bare value; `display e(b)` is refused upstream).
    if (auto lit = resolveEStringLiteral(inner, session)) return *lit;
    return resolveEScalar(inner, session);
  }
  // 4. _N / _n
  if (isSpecialScalar(token)) {
    return resolveN(token, session);
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
    return resolveEScalar(token, session);
  }
  // 7. Dataset variable — first observation's value.
  if (std::regex_match(token, reIdent())) {
    if (session.currentHbin().has_value()) {
      Datastore* ds = session.datastore();
      if (ds != nullptr && ds->has(token)) {
        if (ds->nRows() > 0) {
          std::string dt = ds->dtype(token);
          const cecon::Column& col = ds->col(token);
          double raw = 0.0;
          if (dt == "factor") {
            raw = static_cast<double>(col.codes()[0]);
          } else {
            raw = col.f64()[0];
          }
          return fmtValueFullPrecision(raw);
        }
      }
    }
  }
  // 8. c(name) — system c-class values (BUG-091; BETA A4 BS-091/BS-101:
  //    ~15-key spine via the shared piet::resolveCValue, cclass.h). Any
  //    other c(...) name falls through to the honest NameError downstream
  //    (evalArith sees `c` undefined) — never silently swallowed.
  if (std::regex_match(token, m, reCRef())) {
    auto cv = resolveCValue(m[1].str(), session);
    if (cv.has_value()) return cv->text;
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
  //     the string, not a code (`s[1]`→`ab`). Only an integer literal subscript
  //     is claimed here — anything else (expressions, `[i,j]`) keeps its
  //     existing route, so this arm can only add answers, never change one.
  //
  //     BUG-512: `display x[_N]` -- the last observation, the commonest subscript
  //     there is -- never reached this arm, because `_N` is not an integer
  //     literal; it fell into the matrix arm below and answered `matrix x not
  //     found`. The index is resolved with the macro layer's own substitution
  //     (`_N` -> the count, `_n` -> 1: w2_smalls.log section A), then matched
  //     exactly as before. `x[_N-1]` is still an expression and still unclaimed.
  {
    static const std::regex reVarSub(R"(^([A-Za-z_]\w*)\s*\[\s*(-?\d+)\s*\]$)");
    std::smatch vs;
    const std::string subTok = token.find('[') != std::string::npos
                                   ? eval::substituteObsSpecials(token, session.eval)
                                   : token;
    if (std::regex_match(subTok, vs, reVarSub)) {
      const std::string name = vs[1].str();
      long long idx = 0;
      try {
        idx = std::stoll(vs[2].str());
      } catch (...) {
        idx = 0;
      }
      // The computed-column cache answers a freshly `gen`d / `destring`d column
      // without touching disk — the same surface every other read consumer uses.
      if (const auto* cc = session.residentComputedCol(name)) {
        if (idx < 1 || idx > cc->nRows) return ".";
        const size_t k = static_cast<size_t>(idx - 1);
        if (cc->dtype == "factor")
          return k < cc->strs.size() ? cc->strs[k] : std::string(".");
        return k < cc->f64.size() ? fmtValueFullPrecision(cc->f64[k]) : std::string(".");
      }
      // Otherwise: is it a variable at all? The shadow card knows without an
      // mmap; datastore() is the honest fallback when there is no current card.
      bool isVar = false;
      if (const auto* card = session.residentReadCard()) {
        for (const auto& vi : *card)
          if (vi.name == name) { isVar = true; break; }
      }
      if (!isVar && session.currentHbin().has_value()) {
        Datastore* ds0 = session.datastore();
        isVar = ds0 != nullptr && ds0->has(name);
      }
      if (isVar) {
        Datastore* ds = session.datastore();  // materializes a virtual head if needed
        if (ds == nullptr || !ds->has(name)) return ".";
        if (idx < 1 || idx > ds->nRows()) return ".";
        const size_t k = static_cast<size_t>(idx - 1);
        const cecon::Column& col = ds->col(name);
        if (ds->dtype(name) == "factor") {
          const int32_t code = col.codes()[k];
          const auto& levels = col.levels;
          if (code >= 0 && static_cast<size_t>(code) < levels.size())
            return levels[static_cast<size_t>(code)];
          return ".";
        }
        return fmtValueFullPrecision(col.f64()[k]);
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
    static const std::regex reMatSub(R"(^([A-Za-z_]\w*)\s*\[[^\]]*\]$)");
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
        if (err) *err = "matrix " + name + " not found";
        return ".";
      }
    }
  }

  return token;
}

}  // namespace

// Exported at the P6 wave (see display_hardening.h) so the resampling tables
// share this one implementation instead of growing a second copy.
std::string fmtStataDefaultG(double v) { return fmtStataDefaultGImpl(v); }

// ---------------------------------------------------------------------------
// Public entry point — handle_display

ojson handleDisplay(const std::string& rawArgsIn, SessionState& session) {
  std::string rawArgs = rawArgsIn;

  // U7/S10 (RISK-2): strip a leading `as text|txt|result|res|error|err`
  // modifier (optionally `_continue`/`_n`), case-insensitive.
  {
    static const std::regex reAsMod(
        R"(^as\s+(?:text|txt|result|res|error|err)(?:_(?:continue|n))?\s+)",
        std::regex::icase);
    std::string stripped0 = strip(rawArgs);
    std::smatch m0;
    if (std::regex_search(stripped0, m0, reAsMod) && m0.position(0) == 0) {
      rawArgs = stripped0.substr(m0.length(0));
    }
  }

  std::string s = strip(rawArgs);

  std::vector<std::string> parts;
  std::vector<std::string> unquotedTokens;
  std::optional<std::string> pendingFmt;
  // BUG-088: set when an unresolved call-syntax token (an extended macro
  // function PIET doesn't support) raised a genuine error — surfaced as an
  // honest engine_error instead of echoing the literal source text.
  std::optional<std::string> displayError;

  auto flushUnquoted = [&]() {
    if (unquotedTokens.empty()) return;

    // BUG-087/D9 (BETA Phase F): a LONE `_request(macroName)` unquoted token
    // is handled as a dedicated case, not through the generic per-token
    // resolveToken() loop below — see reRequestCall()'s doc comment for why.
    // Scoped deliberately narrow (exactly one token, this exact shape) —
    // real Stata usage is always `_request(name)` standalone at the end of
    // a display statement (BUG-087's own repro); a token combined with
    // other unquoted text (e.g. `_request(x) foo`) falls through to the
    // generic path unchanged, where it hits the SAME pre-existing BUG-088
    // syntax-error-is-silent-literal-fallthrough behavior any other
    // malformed call-syntax expression gets — not a new gap introduced here.
    if (unquotedTokens.size() == 1) {
      std::smatch rm;
      if (std::regex_match(unquotedTokens[0], rm, reRequestCall())) {
        // Extract the macro name BEFORE clearing unquotedTokens: `rm` holds
        // iterators into unquotedTokens[0]'s string storage, so clearing the
        // vector first (destroying that string) would leave rm[1].str()
        // reading freed memory (caught live: a dangling-iterator bug that
        // produced garbage macro names during manual --do verification of
        // this exact code path before this fix).
        std::string macroName = rm[1].str();
        unquotedTokens.clear();
        std::string promptSoFar;
        for (const auto& p : parts) promptSoFar += p;
        std::string err;
        auto result = resolveRequestCall(macroName, promptSoFar, session, &err);
        if (result.has_value()) {
          // _request() contributes no further display text. A dangling
          // pending %fmt (e.g. `display %9.2f _request(x)`, not a real
          // usage anywhere in this codebase's corpus) has nothing numeric
          // left to format against — drop it rather than inventing a
          // formatting rule for a non-existent case.
          pendingFmt.reset();
          return;
        }
        if (!displayError) displayError = err;
        return;
      }
    }

    std::vector<std::string> resolved;
    resolved.reserve(unquotedTokens.size());
    for (const auto& tok : unquotedTokens) {
      // BUG-162: an unresolvable coefficient name is an honest error, not a
      // silent `.`. First error wins, matching the displayError convention
      // the _request path above already uses.
      std::string tokErr;
      resolved.push_back(resolveToken(tok, session, &tokErr));
      if (!tokErr.empty() && !displayError) displayError = tokErr;
    }
    // BUG-293(b) needs to know whether ANY resolution actually happened: a
    // token that resolved to real data (`c(current_date)` -> a date string) is
    // a value to print, not an expression that failed to parse.
    std::string origJoined;
    for (size_t i = 0; i < unquotedTokens.size(); ++i) {
      if (i > 0) origJoined += " ";
      origJoined += unquotedTokens[i];
    }
    unquotedTokens.clear();

    std::string joined;
    for (size_t i = 0; i < resolved.size(); ++i) {
      if (i > 0) joined += " ";
      joined += resolved[i];
    }

    // BS-091/BS-101 (BETA A4): resolveToken's step 8 (above) only catches a
    // BARE whole-token `c(name)` (its regex is a full-string match) — a
    // c(name) reference COMBINED with real arithmetic (`c(N)*2`) never
    // matches that whole-token pattern and would otherwise reach evalArith
    // as literal, unresolved text. substituteCRefs is a general find-
    // anywhere pass (piet::eval::resolveEstScalarRefs's own established
    // shape), so it also correctly handles the plain bare-token case
    // resolveToken already resolved (a no-op there — no more literal
    // "c(...)" text remains to find).
    joined = substituteCRefs(joined, session);

    std::string arithRaised;
    bool arithIsString = false;
    auto arithResult = eval::evalArith(joined, &session.eval, &arithRaised, &arithIsString);

    if (arithResult.has_value()) {
      if (pendingFmt.has_value()) {
        // BUG-185: a malformed '-'-prefixed spec reports itself via fmtErr
        // instead of silently dropping the format or echoing the spec text.
        std::string fmtErr;
        parts.push_back(applyStataFmtLocal(*pendingFmt, *arithResult, &fmtErr));
        if (!fmtErr.empty() && !displayError) displayError = fmtErr;
        pendingFmt.reset();
      } else {
        // W:P5: no explicit %fmt -> Stata's default `%9.0g` for a numeric
        // result. Non-numeric results (string concatenation, an unresolved
        // literal) pass through untouched — only a value that round-trips
        // through strtod as the WHOLE string is reformatted.
        // BUG-293: a STRING-valued expression is printed verbatim, never
        // re-formatted as a number. `display string(42.5,"%9.2f")` is `42.50`
        // in Stata; routing it through the default %9.0g gave `42.5` — the
        // format the user asked string() for, undone one layer later.
        const std::string& ar = *arithResult;
        char* endp = nullptr;
        const double av = std::strtod(ar.c_str(), &endp);
        const bool wholeNumber = !arithIsString && !ar.empty() && endp != nullptr &&
                                 *endp == '\0' && std::isfinite(av);
        parts.push_back(wholeNumber ? fmtStataDefaultGImpl(av) : ar);
      }
    } else {
      // BUG-088: a genuine NameError on a single call-syntax token `name(args)`
      // (an unsupported extended macro function) surfaces honestly rather than
      // being echoed as literal text. Narrowed to a lone call-syntax token so
      // ordinary literal display (`foo bar`, bare words) stays byte-identical.
      if (!arithRaised.empty() && !displayError) {
        static const std::regex reCallSyntax(
            R"(^\s*[A-Za-z_][A-Za-z0-9_]*\s*\(.*\)\s*$)");
        if (std::regex_match(joined, reCallSyntax)) displayError = arithRaised;
      }
      // BUG-293(b): an unevaluable expression stops being echoed back as its
      // own source text at rc 0. Reuses the BUG-246 classifier, so display and
      // `local NAME = <expr>` answer ONE mapping — including its two
      // deliberately-unclaimed shapes (string-literal RHS, bare-identifier
      // NameError = the dataset-variable read), which keep the legacy echo.
      //
      // Gated on there being exactly ONE unquoted segment with no quoted parts
      // beside it, and on the segment not being one of display's own
      // DIRECTIVES (`_n`, `_col(#)`, `_dup(#)`, …) — those are display's
      // language, not expressions, and are a named gap rather than an error.
      // `display "text" foo` and multi-token prose keep echoing: this claims
      // the shape the oracle measured (`display 1%2` -> r(198)), not every
      // line that ever reached this fallback.
      //
      // `joined == origJoined` is the load-bearing half, found by the selftest:
      // when resolveToken/substituteCRefs DID resolve the token, what reaches
      // evalArith is the resolved VALUE, and a value that is not arithmetic
      // (`display c(current_date)` -> `12 Aug 2026`) must print, not error.
      // Only a token that survived resolution untouched is a candidate.
      if (!displayError && parts.empty() && resolved.size() == 1 && !pendingFmt.has_value() &&
          joined == origJoined) {
        static const std::regex reDisplayDirective(
            R"(^_(n|newline|continue|asis|request|skip|col|dup|char)\b.*)");
        if (!std::regex_match(joined, reDisplayDirective)) {
          eval::MacroFnError me;
          if (eval::classifyFailedExpr(joined, arithRaised, &me) && me.failed()) {
            displayError = "r(" + me.rc + "): " + me.message;
          }
        }
      }
      if (pendingFmt.has_value() && resolved.size() == 1) {
        std::string fmtErr;
        parts.push_back(applyStataFmtLocal(*pendingFmt, resolved[0], &fmtErr));
        if (!fmtErr.empty() && !displayError) displayError = fmtErr;
        pendingFmt.reset();
      } else {
        if (pendingFmt.has_value()) {
          parts.push_back(*pendingFmt);
          pendingFmt.reset();
        }
        parts.push_back(joined);
      }
    }
  };

  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '"') {
      flushUnquoted();
      size_t j = s.find('"', i + 1);
      if (j == std::string::npos) {
        parts.push_back(s.substr(i + 1));
        break;
      }
      parts.push_back(s.substr(i + 1, j - (i + 1)));
      i = j + 1;
    } else if (std::isspace(static_cast<unsigned char>(s[i]))) {
      ++i;
    } else {
      // BUG-293(c): inside a PARENTHESIZED argument list a `"` is part of the
      // expression, not a segment delimiter. The scanner used to break on every
      // quote at any depth, so `display string(42.5,"%9.2f")` was shredded into
      // `string(42.5,` + a string part `%9.2f` + `)`, evalArith never saw a
      // callable expression, and display echoed the fragments back joined at rc
      // 0 (`string(42.5,%9.2f)`) where Stata prints `42.50`. Whitespace inside
      // the parens is swallowed for the same reason (`string(42.5, "%9.2f")`).
      // Deliberately one-sided: if the parens never close, the token is re-cut
      // under the ORIGINAL rule, so every unbalanced/degenerate line this
      // function ever handled stays byte-identical.
      size_t j = i;
      {
        int depth = 0;
        bool closed = true;
        while (j < s.size()) {
          const char c = s[j];
          if (c == '(') {
            ++depth;
          } else if (c == ')') {
            if (depth > 0) --depth;
          } else if (c == '"') {
            if (depth == 0) break;
            const size_t close = s.find('"', j + 1);
            if (close == std::string::npos) {  // unterminated — old rule wins
              closed = false;
              break;
            }
            j = close;
          } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (depth == 0) break;
          }
          ++j;
        }
        if (!closed || depth != 0) {
          j = i;
          while (j < s.size() && s[j] != '"' && !std::isspace(static_cast<unsigned char>(s[j]))) {
            ++j;
          }
        }
      }
      std::string token = s.substr(i, j - i);
      i = j;

      bool isFmtTok = false;
      if (!token.empty() && token[0] == '%') {
        if (token.size() >= 2 && std::isdigit(static_cast<unsigned char>(token[1]))) {
          isFmtTok = true;
        } else if (token.size() >= 2 && token[1] == 't') {
          isFmtTok = true;
        } else if (token.size() >= 2 && token[1] == '-') {
          // BUG-185: %-w.d (left-justify) — previously unrecognized here, so
          // it fell into unquotedTokens and got echoed as literal source
          // text. Actual validation happens in applyStataFmtLocal below.
          isFmtTok = true;
        }
      }

      if (isFmtTok) {
        flushUnquoted();
        pendingFmt = token;
      } else {
        unquotedTokens.push_back(token);
      }
    }
  }
  flushUnquoted();

  // BUG-088: an unsupported extended macro function was hit — fail loudly at the
  // point of failure instead of emitting confidently-wrong literal text.
  if (displayError.has_value()) {
    // BUG-293: an expression-layer error that already NAMES its Stata code
    // ("r(109): type mismatch" — evalArith's BUG-246/249 channel) must carry
    // that code into the block's `reason`, which is the only field
    // eval::coarseRc() reads as exact. Without it `capture noisily display
    // string("abc","%9.0g")` printed the right message and then answered
    // `_rc == 1` where Stata answers 109, so a do-file branching on the code
    // took the wrong branch off an error whose own text named it.
    //
    // Review finding NB1 (2026-09-09): this used to READ the prefix and leave
    // it in the message, so display was the one surface that printed
    // `r(109): type mismatch: ...` where the count/local/gen/if twins print
    // `type mismatch: ...` with the code carried in `reason` (Stata prints the
    // bare sentence). Measured before: `display strlen(e(nosuch))` ->
    // "r(109): type mismatch: `strlen(.)` ...", `display e(cmd)+1` ->
    // "r(109): type mismatch"; rc was 109 in both, so this was cosmetic
    // double-printing, not a wrong answer. The splitter is now the SHARED
    // eval::splitErrCode, so the prefix cannot be consumed two ways -- but its
    // r(111) DEFAULT belongs to the resolver's unprefixed channel, not to
    // display, so it is only consulted when a prefix is really there.
    std::optional<std::string> reason;
    std::string de = *displayError;
    if (de.rfind("r(", 0) == 0) {
      std::string stripped = de;
      const std::string code = eval::splitErrCode(&stripped);
      if (stripped != de) {  // splitErrCode only rewrites when the prefix parsed
        reason = code;
        de = stripped;
      }
    }
    return ojson(eval::errorBlock("engine_error", de, std::nullopt, std::nullopt, std::nullopt,
                                  reason));
  }

  std::string text;
  for (const auto& p : parts) text += p;

  ojson out;
  out["type"] = "display";
  out["text"] = text;
  return out;
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
  std::string joined = substituteCRefs(eval::resolveEstScalarRefs(e, session.eval), session);
  std::string raised;
  bool isString = false;
  auto r = eval::evalArith(joined, &session.eval, &raised, &isString);
  if (!r.has_value() || isString || !raised.empty()) return std::nullopt;
  return toNumber(*r);
}

}  // namespace piet
