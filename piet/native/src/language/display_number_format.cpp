// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// display_number_format.cpp — the `%fmt` number renderer shared by `display` and
// `file write` (default %9.0g / %10.0g, %21x, explicit %w.d{f,g,e} and string
// formats). See display_internal.h.
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

namespace piet {

using namespace display_detail;

namespace {

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

// BUG-444: Stata's `%21x` text, `[+-]1.<13 hex digits>X[+-]<3 hex digits>`.
// Zero is `+0.0000000000000X-3ff`. Stata's missing values are doubles at
// 2^1023 whose fraction carries the kind in its top 12 bits: `.` is
// `+1.0000000000000X+3ff`, `.a` `+1.0010000000000X+3ff`, `.b` `...0020...`
// (w2x_fn_string_math fx_fix_v, fx_fix_kt, fx_ceil_edges). The expression
// layer holds exactly those doubles (exprfns.h, stataMissingWithKind), and any
// other value in its missing band (|v| >= 2^1023: an overflow, the negative
// side) is plain `.`, so a missing value is spelled from its kind. c(maxdouble)
// is a real number and prints its own bits, +1.fffffffffffffX+3fe. A NaN or an
// inf reaching here is the data plane's system missing.
// The text is the IEEE fields read off directly (sign, leading bit, 52-bit
// fraction, unbiased exponent), which is how zero gets its `X-3ff`; a
// subnormal is spelled the same way, `+0.<fraction>X-3ff`. No oracle pins
// the subnormal spelling.
std::string renderHex21(double v) {
  char buf[48];
  if (std::isnan(v) || std::isinf(v)) v = eval::kStataMissingSentinel;
  if (const auto kind = eval::stataExtendedMissingKind(v)) {
    std::snprintf(buf, sizeof(buf), "+1.%013llxX+3ff",
                  static_cast<unsigned long long>(*kind) << 40);
    return buf;
  }
  // A negative zero prints as zero: Stata's `ceil(-0.5)` is
  // `+0.0000000000000X-3ff` (fx_ceil_edges), where C's ceil() gives -0.0.
  if (v == 0.0) return "+0.0000000000000X-3ff";
  const char sign = v < 0 ? '-' : '+';
  const double a = std::fabs(v);
  int e = 0;
  const double m = std::frexp(a, &e);  // a = m * 2^e, m in [0.5, 1)
  if (e - 1 < -1022) {
    const auto frac = static_cast<unsigned long long>(std::ldexp(a, 1022 + 52));
    std::snprintf(buf, sizeof(buf), "%c0.%013llxX-3ff", sign, frac);
    return buf;
  }
  const auto frac = static_cast<unsigned long long>(std::ldexp(m * 2.0 - 1.0, 52));
  const int x = e - 1;
  std::snprintf(buf, sizeof(buf), "%c1.%013llxX%c%03x", sign, frac, x < 0 ? '-' : '+',
                static_cast<unsigned>(x < 0 ? -x : x));
  return buf;
}

}  // namespace

namespace display_detail {

// A string display format: `%20s`, `%-20s`, `%~20s` (FU-141's scalar gate).
const std::regex& reStringFmtSpec() {
  static const std::regex r(R"(^%[-~]?\d*s$)");
  return r;
}

// BUG-1016: `display <numeric>` with NO format, measured against StataBE 19.5:
// `display xd[1]` for .4583333333333333 prints `.45833333`
// (tests/eval/golden/aer_w2/w2i_varsub.log:85 j_01) and `display _b[ATE:r1vs0.t]`
// prints `.02559116` and `-.22631858` (w2g_ebmat.log:1728, :1807) -- eight
// significant digits in nine characters once the leading zero is DROPPED, where
// fmtStataDefaultGImpl counts the nine on `0.4583333` and printed `.4583333`.
// For |x| >= 1 the two agree (2.1577892, 6.277e-26 above). A separate function
// on purpose: fmtStataDefaultG (fmtStataDefaultGImpl) is also the `%9.0g` cell
// renderer of the resampling, rdrobust and table handlers, whose own pins
// (freeze rdrobust.json) print `.3842431`; the explicit `%9.0g` format is
// applyStataFmt's (fmt_g_oracle.log:142-160). Only display's unformatted item
// takes this one.
// BUG-1126: that rule is the shared `%10.0g` (stata_format.h general: eight
// significant digits in nine characters), whose small and large sides printf's
// %g did not follow: `display 0.00001` printed 1e-05, 1e-6 1e-06, 2e-5 2e-05,
// 1e20 1e+20 and 1e-300 1e-300, where StataBE prints .00001, 1.000e-06, .00002,
// 1.000e+20 and 1.00e-300 (file write's unformatted `(exp)`, documented as
// %10.0g, prints the same). The values above agree with it.
std::string fmtDisplayDefaultG(double v) {
  if (std::isnan(v)) return ".";
  if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
  return piet::fmt::general(v, 10, 0);
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
// The oracle: internal capture fmt_g_oracle.{do,log},
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
                                std::string* err) {
  // BUG-1345: a string format on a number prints the number as display prints
  // it with no format, in no field (StataBE 19.5: `%10s 3.14159265` 3.1415927,
  // `%-10s 1234567.891` 1234567.9, `%~9s -2.5` -2.5, `%10s .` .). `%-10s 3` was
  // rc 1 `invalid display format %-10s`; `%10s` printed the full-precision text.
  if (std::regex_match(fmt, reStringFmtSpec())) {
    const std::string t = strutil::trim(valueStr);
    if (piet::fmt::isMissingToken(t)) return t;
    auto v = eval::pythonFloat(valueStr);
    return v ? fmtDisplayDefaultG(*v) : valueStr;
  }
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
  // BUG-444: `%21x`, Stata's hexadecimal view of the stored double. It used to
  // fail the grammar below and print the decimal text at rc 0, so a missing
  // value printed `.` where Stata prints `+1.0000000000000X+3ff` (`.a` is
  // `+1.0010000000000X+3ff`). Oracle: aer_w2 w2x_fn_string_math (every numeric
  // field is a %21x record; fx_fix_v/fx_fix_kt carry `.` and `.a`).
  {
    static const std::regex reX(R"(^%(-)?21x$)");
    std::smatch xm;
    if (std::regex_match(fmt, xm, reX)) {
      const std::string t = strutil::trim(valueStr);
      std::optional<double> v;
      if (piet::fmt::isMissingToken(t)) {
        v = eval::stataMissingWithKind(t.size() == 1 ? 0 : t[1] - 'a' + 1);
      } else {
        v = eval::pythonFloat(valueStr);
      }
      if (!v) return valueStr;
      return renderHex21(*v);
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
  // BUG-1353: an `e` spec narrower than 8 lands in the same field: `%5.0e 1.5`
  // displays ` 1.5e+00` and `%5.0e 1.5e+300` ` 2.e+300` (StataBE 19.5).
  // A left-justified `e` keeps its own field: `%-7.2e 1.5` is `1.5e+00`, no pad.
  // A right-justified `e` also keeps a blank where a minus sign would go, which
  // only shows when rounding carried the exponent to three digits: `%9.0e
  // 9.9999e99` displays ` 1.00e+100`, `%5.0e` ` 1.0e+100` (string() has no field).
  const bool eField = sp.conv == 'e' && !sp.leftJustify;
  const int signSlot = eField && !rendered->empty() && (*rendered)[0] != '-'
                           ? static_cast<int>(rendered->size()) + 1
                           : 0;
  if (((sp.conv == 'f' && rendered->find('e') != std::string::npos) || eField) &&
      sp.width < 8) {
    piet::fmt::StataFmtSpec wide = sp;
    wide.hasWidth = true;
    wide.width = std::max(8, signSlot);
    return piet::fmt::padToField(*rendered, wide);
  }
  if (signSlot > sp.width) {
    piet::fmt::StataFmtSpec wide = sp;
    wide.width = signSlot;
    return piet::fmt::padToField(*rendered, wide);
  }
  // BUG-185: pad to field width; printf semantics — never truncate a value
  // already wider than the field.
  return piet::fmt::padToField(*rendered, sp);
}

}  // namespace display_detail

// Exported at the P6 wave (see display_hardening.h) so the resampling tables
// share this one implementation instead of growing a second copy.
std::string fmtStataDefaultG(double v) { return fmtStataDefaultGImpl(v); }

// display_hardening.h: `file write`'s numeric %fmt, through display's renderer.
std::string formatNumberAsDisplayed(const std::string& fmt, const std::string& valueText) {
  return applyStataFmtLocal(fmt, valueText);
}

}  // namespace piet
